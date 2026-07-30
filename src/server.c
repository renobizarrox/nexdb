/* server.c — JSON-over-TCP server mode for nexdb.
 *
 * Each client connection gets its own handler thread.  SQL execution is
 * serialised through a single mutex so that the DB handle is never accessed
 * concurrently.
 *
 * Wire format (newline-delimited JSON):
 *   Request:  {"sql":"...", "session":"...", "token":"..."}
 *   Response: {"ok":true, "text":"...", "columns":[...], "rows":[[...],...]}
 *             {"ok":false, "error":"..."}
 *
 * The "session" field is optional; if omitted the server generates one and
 * returns it in "session".  A session carries transaction state so that
 * BEGIN / COMMIT / ROLLBACK work across multiple requests from the same
 * client.
 */

#include "nexdb.h"
#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>

#ifdef ENABLE_TLS
# include <openssl/ssl.h>
# include <openssl/err.h>
# include <openssl/crypto.h>
static SSL_CTX *g_ssl_ctx = NULL;
#endif

/* ------------------------------------------------------------------ limits */
#define MAX_SESSIONS   64
#define MAX_LINE       (256 * 1024)  /* max incoming JSON line */
#define RESP_BUF        (64 * 1024)  /* response buffer size   */
#define MAX_CONN        64

/* ---------------------------------------------------------------- session */
typedef struct {
    char   id[37];         /* UUID string, empty = free slot */
    int64_t last_active;
    int    txn_active;
    int    txn_undo_depth;
    int    txn_depth;
    int    txn_sp_count;
    int    txn_sp[MAX_SAVEPOINTS];
    char   db_err[MAX_ERR];
} Session;

static Session sessions[MAX_SESSIONS];
static pthread_mutex_t sess_lock = PTHREAD_MUTEX_INITIALIZER;

/* Global server state (set once at startup, read-only after that). */
static pthread_mutex_t exec_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_token[128] = "";
static int g_session_ttl = 300;
static volatile int g_running = 1;

/* Connection counter */
static volatile int g_conn_count = 0;
static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------- UUID helpers */
static void uuid_str(char out[37])
{
    /* Generate a random-ish UUID v4.  This is not cryptographically strong
     * but is good enough for session tokens. */
    static pthread_mutex_t urand_lock = PTHREAD_MUTEX_INITIALIZER;
    uint8_t buf[16];
    pthread_mutex_lock(&urand_lock);
    static int seeded = 0;
    if (!seeded) { srand((unsigned)(time(NULL) ^ (uint64_t)pthread_self())); seeded = 1; }
    for (int i = 0; i < 16; i++) buf[i] = (uint8_t)(rand() & 0xFF);
    pthread_mutex_unlock(&urand_lock);
    buf[6] = (buf[6] & 0x0F) | 0x40; /* version 4 */
    buf[8] = (buf[8] & 0x3F) | 0x80; /* variant */
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
             buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
}

/* -------------------------------------------------------- session helpers */
static int session_find(const char *id)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].id[0] && strcmp(sessions[i].id, id) == 0)
            return i;
    return -1;
}

static int session_alloc(void)
{
    /* Reap stale sessions first */
    int64_t now = (int64_t)time(NULL);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].id[0] && now - sessions[i].last_active > g_session_ttl) {
            sessions[i].id[0] = 0;
            sessions[i].txn_active = 0;
        }
    }
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!sessions[i].id[0]) return i;
    return -1;
}

/* ----------------------------------------------------------- JSON helpers
 * All very ad-hoc and not intended to be robust against malicious input —
 * this is a personal-data daemon, not a public-facing web service. */

/* Find the value of a quoted string key in a JSON object and unescape it.
 * Returns buf on success, NULL if the key was not found.
 * Handles \" and \\ escape sequences; other escapes are left as-is. */
static const char *json_str(const char *json, const char *key, char *buf, int bufsz)
{
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k) return NULL;
    k += strlen(pat);
    while (*k && (unsigned char)*k <= ' ') k++;
    if (*k != ':') return NULL;
    k++;
    while (*k && (unsigned char)*k <= ' ') k++;
    if (*k != '"') return NULL;
    k++;
    int pos = 0;
    while (*k && pos < bufsz - 1) {
        if (*k == '"') break;
        if (*k == '\\' && *(k + 1)) {
            k++;
            if (*k == '"') buf[pos++] = '"';
            else if (*k == '\\') buf[pos++] = '\\';
            else { buf[pos++] = '\\'; buf[pos++] = *k; }
        } else {
            buf[pos++] = *k;
        }
        k++;
    }
    buf[pos] = 0;
    return buf;
}

/* Write a JSON-quoted string into `buf` (advancing the pointer). */
static void json_quote(char **pbuf, const char *s)
{
    char *p = *pbuf;
    *p++ = '"';
    while (*s) {
        if (*s == '"' || *s == '\\') *p++ = '\\';
        else if (*s == '\n') { *p++ = '\\'; *p++ = 'n'; s++; continue; }
        else if (*s == '\r') { *p++ = '\\'; *p++ = 'r'; s++; continue; }
        else if (*s == '\t') { *p++ = '\\'; *p++ = 't'; s++; continue; }
        *p++ = *s++;
    }
    *p++ = '"';
    *p = 0;
    *pbuf = p;
}

/* Execute SQL and produce a JSON response with text, columns, and rows. */
static void execute_sql(const char *sql, const char *session_id,
                        char *out, size_t outsz)
{
    Session *sess = NULL;
    int sidx = -1;
    if (session_id && session_id[0]) {
        sidx = session_find(session_id);
        if (sidx >= 0) sess = &sessions[sidx];
    }

    if (sess && sess->txn_active) {
        g_db->txn_active = 1;
        g_db->txn_depth = sess->txn_depth;
        g_db->undo_depth = sess->txn_undo_depth;
        g_db->txn_sp_count = sess->txn_sp_count;
        memcpy(g_db->txn_sp, sess->txn_sp,
               sizeof(int) * (size_t)sess->txn_sp_count);
    }

    Lexer lx;
    lex_init(&lx, sql);
    int lex_first = lex_next(&lx);
    if (lex_first < 0) {
        snprintf(out, outsz, "{\"ok\":false,\"error\":\"lex error\"}");
        return;
    }

    /* Redirect stdout to a temp file for text capture */
    char tmp_path[] = "/tmp/nexdb_capture_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    int old_stdout_fd = -1;
    char *captured = NULL;
    if (tmp_fd < 0) {
        snprintf(out, outsz, "{\"ok\":false,\"error\":\"internal: mkstemp failed\"}");
        return;
    }
    unlink(tmp_path);  /* Remove the directory entry; fd remains valid */

    fflush(stdout);
    old_stdout_fd = dup(STDOUT_FILENO);
    dup2(tmp_fd, STDOUT_FILENO);

    /* Also capture structured SELECT results */
    Capture cap = {0};
    g_select_capture = &cap;

    int rc = 0;
    char err[MAX_ERR] = "";

    Stmt *stmt = NULL;
    for (;;) {
        stmt = calloc(1, sizeof *stmt);
        if (!stmt) { rc = -1; snprintf(err, sizeof err, "out of memory"); break; }
        stmt->top = -1;
        int r = parse_stmt(&lx, stmt, err);
        if (r < 0) { stmt_free(stmt); free(stmt); stmt = NULL; rc = -1; break; }
        if (r == 0) { stmt_free(stmt); free(stmt); stmt = NULL; break; }
        rc = exec_stmt(g_db, stmt, err);
        stmt_free(stmt);
        free(stmt);
        stmt = NULL;
        if (rc < 0) break;
    }
    free(stmt);

    /* Restore stdout */
    g_select_capture = NULL;
    fflush(stdout);
    dup2(old_stdout_fd, STDOUT_FILENO);
    close(old_stdout_fd);

    /* Read the captured output from the temp file */
    off_t fsize = lseek(tmp_fd, 0, SEEK_END);
    lseek(tmp_fd, 0, SEEK_SET);
    if (fsize > 0) {
        captured = malloc((size_t)fsize + 1);
        if (captured) {
            ssize_t n = read(tmp_fd, captured, (size_t)fsize);
            if (n > 0) {
                captured[n] = 0;
            } else {
                free(captured);
                captured = NULL;
            }
        }
    }
    close(tmp_fd);

    if (sess) {
        sess->last_active = (int64_t)time(NULL);
        sess->txn_active = g_db->txn_active;
        sess->txn_depth = g_db->txn_depth;
        sess->txn_undo_depth = g_db->undo_depth;
        sess->txn_sp_count = g_db->txn_sp_count;
        memcpy(sess->txn_sp, g_db->txn_sp,
               sizeof(int) * (size_t)g_db->txn_sp_count);
    }

    /* Build JSON response */
    char *p = out;
    char *end = out + outsz;

    if (rc < 0) {
        p += snprintf(p, (size_t)(end - p),
                      "{\"ok\":false,\"error\":");
        json_quote(&p, err[0] ? err : (captured ? captured : ""));
        p += snprintf(p, (size_t)(end - p), ",\"session\":");
        json_quote(&p, session_id ? session_id : "");
        p += snprintf(p, (size_t)(end - p), "}");
    } else {
        p += snprintf(p, (size_t)(end - p), "{\"ok\":true");

        if (captured && captured[0]) {
            p += snprintf(p, (size_t)(end - p), ",\"text\":");
            json_quote(&p, captured);
        }

        if (cap.ncols > 0) {
            p += snprintf(p, (size_t)(end - p), ",\"columns\":[");
            for (int c = 0; c < cap.ncols; c++) {
                if (c) *p++ = ',';
                json_quote(&p, cap.colnames[c]);
            }
            *p++ = ']';

            p += snprintf(p, (size_t)(end - p), ",\"rows\":[");
            for (int r = 0; r < cap.nrows; r++) {
                if (r) *p++ = ',';
                *p++ = '[';
                for (int c = 0; c < cap.ncols; c++) {
                    if (c) *p++ = ',';
                    const Value *v = &cap.cells[r * cap.ncols + c];
                    if (v->tag == T_NULL) {
                        p += snprintf(p, (size_t)(end - p), "null");
                    } else {
                        char buf[128];
                        val_format(v, buf, sizeof buf);
                        json_quote(&p, buf);
                    }
                }
                *p++ = ']';
            }
            *p++ = ']';
        }

        p += snprintf(p, (size_t)(end - p), ",\"session\":");
        json_quote(&p, session_id ? session_id : "");
        p += snprintf(p, (size_t)(end - p), "}");
    }

    capture_free(&cap);
    free(captured);

    if (!sess || !sess->txn_active) {
        g_db->txn_active = 0;
        g_db->txn_depth = 0;
        g_db->undo_depth = 0;
        g_db->txn_sp_count = 0;
    }
}

/* ----------------------------------------------------------- per-client handler */

#ifdef ENABLE_TLS
/* Wrappers for TLS-aware read/write.  When ssl is non-NULL they use OpenSSL;
 * otherwise they fall back to plain read/write. */
static ssize_t conn_read(SSL *ssl, int fd, void *buf, size_t count)
{
    return ssl ? SSL_read(ssl, buf, (int)count) : read(fd, buf, count);
}
static ssize_t conn_write(SSL *ssl, int fd, const void *buf, size_t count)
{
    return ssl ? SSL_write(ssl, buf, (int)count) : write(fd, buf, count);
}
#else
/* Plain wrappers when TLS is not compiled in – the compiler will optimise
 * the ssl parameter away. */
static ssize_t conn_read(void *ssl, int fd, void *buf, size_t count)
{
    (void)ssl; return read(fd, buf, count);
}
static ssize_t conn_write(void *ssl, int fd, const void *buf, size_t count)
{
    (void)ssl; return write(fd, buf, count);
}
#endif

static void *client_handler(void *arg)
{
    int fd = (int)(intptr_t)arg;

#ifdef ENABLE_TLS
    SSL *ssl = NULL;
    if (g_ssl_ctx) {
        ssl = SSL_new(g_ssl_ctx);
        if (ssl) {
            SSL_set_fd(ssl, fd);
            int hr = SSL_accept(ssl);
            if (hr <= 0) {
                SSL_free(ssl);
                ssl = NULL;
            }
        }
        if (!ssl) {
            close(fd);
            return NULL;
        }
    }
#else
    void *ssl = NULL;
#endif

    pthread_mutex_lock(&conn_lock);
    g_conn_count++;
    pthread_mutex_unlock(&conn_lock);

    /* Allocate working buffers on the heap to avoid stack overflow in
     * handler threads (default pthread stack on macOS is generous, but
     * using 256 KB local arrays on every connection is wasteful). */
    char *line = malloc(MAX_LINE);
    char *resp = malloc(RESP_BUF);
    if (!line || !resp) {
        if (line) free(line);
        if (resp) free(resp);
#ifdef ENABLE_TLS
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
        close(fd);
        return NULL;
    }
    size_t llen = 0;
    char session_id[37] = "";

    uuid_str(session_id);
    pthread_mutex_lock(&sess_lock);
    int sidx = session_alloc();
    if (sidx >= 0) {
        memcpy(sessions[sidx].id, session_id, 37);
        sessions[sidx].last_active = (int64_t)time(NULL);
        sessions[sidx].txn_active = 0;
    }
    pthread_mutex_unlock(&sess_lock);

    snprintf(resp, RESP_BUF,
             "{\"ok\":true,\"session\":\"%s\",\"text\":\"connected\"}\n",
             session_id);
    conn_write(ssl, fd, resp, strlen(resp));

    while (g_running) {
        ssize_t n = conn_read(ssl, fd, line + llen, MAX_LINE - llen - 1);
        if (n <= 0) break;
        llen += (size_t)n;
        line[llen] = 0;

        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            char *json = line;
            size_t consumed = (size_t)(nl - line) + 1;

            char sql[1024] = "";
            char tok[1024] = "";
            char sid[37] = "";
            json_str(json, "sql", sql, sizeof sql);
            json_str(json, "session", sid, sizeof sid);
            json_str(json, "token", tok, sizeof tok);

            const char *active_sid = sid[0] ? sid : session_id;

            if (g_token[0] && strcmp(tok, g_token) != 0) {
                snprintf(resp, RESP_BUF,
                         "{\"ok\":false,\"error\":\"unauthorized\"}\n");
                conn_write(ssl, fd, resp, strlen(resp));
            } else if (sql[0]) {
                pthread_mutex_lock(&exec_lock);
                execute_sql(sql, active_sid, resp, RESP_BUF);
                pthread_mutex_unlock(&exec_lock);

                size_t rlen = strlen(resp);
                if (rlen + 2 < RESP_BUF) {
                    resp[rlen] = '\n';
                    resp[rlen + 1] = 0;
                }
                conn_write(ssl, fd, resp, strlen(resp));
            }

            memmove(line, line + consumed, llen - consumed);
            llen -= consumed;
            line[llen] = 0;
        }
    }

#ifdef ENABLE_TLS
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
    close(fd);

    pthread_mutex_lock(&sess_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (strcmp(sessions[i].id, session_id) == 0) {
            sessions[i].id[0] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&sess_lock);

    free(line);
    free(resp);

    pthread_mutex_lock(&conn_lock);
    g_conn_count--;
    pthread_mutex_unlock(&conn_lock);

    return NULL;
}

/* ------------------------------------------------------- signal handler */
static void sighandle(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------ main server loop */
int server_run(DB *db, int port, const char *unix_path,
               const char *token, int session_ttl,
               const char *tls_cert, const char *tls_key)
{
    g_db = db;
    if (token) {
        snprintf(g_token, sizeof g_token, "%s", token);
    }
    g_session_ttl = session_ttl;

#ifdef ENABLE_TLS
    if (tls_cert && tls_key) {
        SSL_library_init();
        SSL_load_error_strings();
        OPENSSL_add_all_algorithms_noconf();
        g_ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (!g_ssl_ctx) {
            fprintf(stderr, "error: SSL_CTX_new failed\n");
            return -1;
        }
        SSL_CTX_set_mode(g_ssl_ctx, SSL_MODE_AUTO_RETRY);
        if (SSL_CTX_use_certificate_file(g_ssl_ctx, tls_cert, SSL_FILETYPE_PEM) <= 0) {
            fprintf(stderr, "error: cannot load TLS cert '%s'\n", tls_cert);
            SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = NULL;
            return -1;
        }
        if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, tls_key, SSL_FILETYPE_PEM) <= 0) {
            fprintf(stderr, "error: cannot load TLS key '%s'\n", tls_key);
            SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = NULL;
            return -1;
        }
        if (!SSL_CTX_check_private_key(g_ssl_ctx)) {
            fprintf(stderr, "error: TLS cert and key do not match\n");
            SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = NULL;
            return -1;
        }
    }
#else
    (void)tls_cert;
    (void)tls_key;
    if (tls_cert || tls_key) {
        fprintf(stderr, "error: TLS support not compiled in (install OpenSSL headers and rebuild)\n");
        return -1;
    }
#endif

    /* Ignore SIGPIPE so write() to a closed socket returns -1 instead of
     * killing the process. */
    signal(SIGPIPE, SIG_IGN);

    /* Graceful shutdown on SIGINT/SIGTERM */
    signal(SIGINT,  sighandle);
    signal(SIGTERM, sighandle);

    /* Create TCP listen socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "error: socket: %s\n", strerror(errno));
        return -1;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "error: bind port %d: %s\n", port, strerror(errno));
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 16) < 0) {
        fprintf(stderr, "error: listen: %s\n", strerror(errno));
        close(listen_fd);
        return -1;
    }

    /* Create Unix socket if requested */
    int unix_fd = -1;
    char sock_path[256];
    if (unix_path && unix_path[0]) {
        snprintf(sock_path, sizeof sock_path, "%s", unix_path);
        unlink(sock_path);
        unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (unix_fd < 0) {
            fprintf(stderr, "error: unix socket: %s\n", strerror(errno));
            close(listen_fd);
            return -1;
        }
        struct sockaddr_un un;
        memset(&un, 0, sizeof un);
        un.sun_family = AF_UNIX;
        snprintf(un.sun_path, sizeof un.sun_path, "%s", sock_path);
        if (bind(unix_fd, (struct sockaddr *)&un, sizeof un) < 0) {
            fprintf(stderr, "error: bind %s: %s\n", sock_path, strerror(errno));
            close(unix_fd); close(listen_fd);
            return -1;
        }
        chmod(sock_path, 0700);
        listen(unix_fd, 16);
    }

    printf("nexdb server listening on port %d", port);
    if (unix_fd >= 0) printf(", unix:%s", sock_path);
    printf("\n");

    /* Accept loop */
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        if (unix_fd >= 0) {
            FD_SET(unix_fd, &rfds);
            if (unix_fd > maxfd) maxfd = unix_fd;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "select: %s\n", strerror(errno));
            break;
        }
        if (ret == 0) continue;

        int cfd = -1;
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in ca;
            socklen_t clen = sizeof ca;
            cfd = accept(listen_fd, (struct sockaddr *)&ca, &clen);
        } else if (unix_fd >= 0 && FD_ISSET(unix_fd, &rfds)) {
            struct sockaddr_un ca;
            socklen_t clen = sizeof ca;
            cfd = accept(unix_fd, (struct sockaddr *)&ca, &clen);
        }

        if (cfd >= 0) {
            /* Disable Nagle for lower latency */
            int n = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &n, sizeof n);

            /* Enforce connection limit */
            pthread_mutex_lock(&conn_lock);
            int too_many = (g_conn_count >= MAX_CONN);
            pthread_mutex_unlock(&conn_lock);

            if (too_many) {
                char *msg = "{\"ok\":false,\"error\":\"too many connections\"}\n";
                write(cfd, msg, strlen(msg));
                close(cfd);
                continue;
            }

            pthread_t thr;
            pthread_create(&thr, NULL, client_handler, (void *)(intptr_t)cfd);
            pthread_detach(thr);
        }
    }

    close(listen_fd);
    if (unix_fd >= 0) {
        close(unix_fd);
        unlink(sock_path);
    }
#ifdef ENABLE_TLS
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
#endif
    return 0;
}

/* --------------------------------------------------------------- client REPL */
/* Connect to a remote server and run a simple REPL that sends SQL and prints
 * JSON responses as text. */

static int recv_line(int fd, char *buf, size_t cap)
{
    size_t pos = 0;
    while (pos + 1 < cap) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n <= 0) return -1;
        if (buf[pos] == '\n') { buf[pos] = 0; return (int)pos; }
        pos++;
    }
    buf[pos] = 0;
    return (int)pos;
}

int server_connect_repl(const char *address)
{
    int fd;
    int is_unix = (address[0] == '/');

    if (is_unix) {
        struct sockaddr_un un;
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return -1; }
        memset(&un, 0, sizeof un);
        un.sun_family = AF_UNIX;
        snprintf(un.sun_path, sizeof un.sun_path, "%s", address);
        if (connect(fd, (struct sockaddr *)&un, sizeof un) < 0) {
            fprintf(stderr, "connect %s: %s\n", address, strerror(errno));
            close(fd); return -1;
        }
    } else {
        /* Parse host:port */
        char host[256] = "127.0.0.1";
        int port = 7890;
        const char *colon = strchr(address, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - address);
            if (hlen > 0) {
                if (hlen >= sizeof host) hlen = sizeof host - 1;
                memcpy(host, address, hlen);
                host[hlen] = 0;
            }
            port = atoi(colon + 1);
            if (port <= 0 || port > 65535) port = 7890;
        }

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return -1; }
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &a.sin_addr) <= 0) {
            fprintf(stderr, "invalid address '%s'\n", host);
            close(fd); return -1;
        }
        if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
            fprintf(stderr, "connect %s:%d: %s\n", host, port, strerror(errno));
            close(fd); return -1;
        }
    }

    /* Read the welcome message */
    char welcome[4096];
    if (recv_line(fd, welcome, sizeof welcome) < 0) {
        close(fd);
        return -1;
    }
    /* Extract session ID from welcome */
    char session[37] = "";
    json_str(welcome, "session", session, sizeof session);

    char line[4096];
    char resp[65536];

    printf("connected to nexdb server\n");

    for (;;) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t llen = strlen(line);
        while (llen && (line[llen - 1] == '\n' || line[llen - 1] == '\r'))
            line[--llen] = 0;
        if (llen == 0) continue;
        if (!strcmp(line, ".exit") || !strcmp(line, ".quit")) break;

        /* Build JSON request — properly escape SQL */
        char req[65536];
        char *rp = req;
        rp += sprintf(rp, "{\"sql\":\"");
        for (char *s = line; *s; s++) {
            if (*s == '"' || *s == '\\') *rp++ = '\\';
            *rp++ = *s;
        }
        rp += sprintf(rp, "\",\"session\":\"%s\"}\n", session);
        write(fd, req, strlen(req));

        /* Read response */
        if (recv_line(fd, resp, sizeof resp) < 0) break;

        /* Parse and display */
        char text[65536] = "";
        char err[4096] = "";
        json_str(resp, "text", text, sizeof text);
        json_str(resp, "error", err, sizeof err);

        if (err[0]) {
            printf("error: %s\n", err);
        } else if (text[0]) {
            printf("%s\n", text);
        }

        /* Update session from response */
        json_str(resp, "session", session, sizeof session);
    }

    close(fd);
    return 0;
}
