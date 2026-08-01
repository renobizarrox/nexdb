/* server.c — JSON-over-TCP server mode for nexdb.
 *
 * Each client connection gets its own handler thread.  Write statements are
 * serialised through a single reader-writer lock; read-only statements
 * (SELECT / RECALL / SHOW / PRINT / EXPLAIN) run concurrently under the read
 * side of the lock.  Because reinforcement would make a plain read write
 * pages, memory touches are deferred during reads and applied afterwards
 * under a write lock (see memory.c: mem_set_defer / mem_flush_pending).
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

/* Per-session undo-log copies.  The transaction undo entries themselves live
 * in the shared DB struct; sessions only swapped the depth counter, so two
 * interleaved transactions clobbered each other's undo history.  Each session
 * keeps its own copy of the entries, swapped in/out around every statement.
 * Allocated lazily when a session enters a transaction. */
static UndoEntry *g_session_undo[MAX_SESSIONS];

/* Global server state (set once at startup, read-only after that).
 * Reader-writer lock: write statements (and whole transactions) take the
 * write side; read-only statements take the read side and can run in
 * parallel. */
static pthread_rwlock_t exec_lock = PTHREAD_RWLOCK_INITIALIZER;
static char g_token[128] = "";
static int g_session_ttl = 300;
static volatile int g_running = 1;

/* Connection counter */
static volatile int g_conn_count = 0;
static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

/* Session persistence (populated in server_run). */
static char g_session_path[1024] = "";
static volatile int g_session_dirty = 0;

/* Set when the current handler thread owns exec_lock for an open transaction. */
static __thread int g_txn_lock_held = 0;

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
    /* Reap stale sessions first.  Sessions with an open transaction are
     * pinned — their handler thread holds exec_lock and must be the one
     * to finish the transaction. */
    int64_t now = (int64_t)time(NULL);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].id[0] && !sessions[i].txn_active &&
            now - sessions[i].last_active > g_session_ttl) {
            if (g_session_undo[i]) { free(g_session_undo[i]); g_session_undo[i] = NULL; }
            sessions[i].id[0] = 0;
            g_session_dirty = 1;
        }
    }
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!sessions[i].id[0]) return i;
    return -1;
}

/* ------------------------------------------------------ session persistence */
#define SESSION_MAGIC  0x5345534e  /* "NSES" */

static void sessions_save(void)
{
    if (!g_session_path[0]) return;
    char tmp[1032];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_session_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;

    uint32_t magic = SESSION_MAGIC;
    uint32_t ver = 1;

    pthread_mutex_lock(&sess_lock);
    uint32_t count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].id[0]) count++;
    /* header: magic + version + count */
    struct { uint32_t magic; uint32_t ver; uint32_t count; } hdr;
    hdr.magic = magic; hdr.ver = ver; hdr.count = count;
    write(fd, &hdr, sizeof hdr);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].id[0])
            write(fd, &sessions[i], sizeof(Session));
    pthread_mutex_unlock(&sess_lock);

    close(fd);
    rename(tmp, g_session_path);
}

static void sessions_load(void)
{
    if (!g_session_path[0]) return;
    int fd = open(g_session_path, O_RDONLY);
    if (fd < 0) return;

    struct { uint32_t magic; uint32_t ver; uint32_t count; } hdr;
    if (read(fd, &hdr, sizeof hdr) != sizeof hdr) { close(fd); return; }
    if (hdr.magic != SESSION_MAGIC || hdr.ver != 1) { close(fd); return; }
    if (hdr.count > MAX_SESSIONS) hdr.count = MAX_SESSIONS;

    pthread_mutex_lock(&sess_lock);
    int64_t now = (int64_t)time(NULL);
    for (uint32_t i = 0; i < hdr.count; i++) {
        Session s;
        if (read(fd, &s, sizeof(Session)) != sizeof(Session)) break;
        if (s.id[0] && now - s.last_active <= g_session_ttl) {
            s.txn_active = 0;
            s.txn_depth = 0;
            s.txn_undo_depth = 0;
            s.txn_sp_count = 0;
            for (int j = 0; j < MAX_SESSIONS; j++) {
                if (!sessions[j].id[0]) {
                    memcpy(&sessions[j], &s, sizeof(Session));
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&sess_lock);

    close(fd);
}

/* ------------------------------------------------------------ background GC */

#define GC_INTERVAL      30   /* seconds between GC cycles */
#define VACUUM_INTERVAL 300   /* minimum seconds between auto-vacuums */
#define VACUUM_FREE_PCT  20   /* auto-vacuum when this % of pages are free */

static void *gc_loop(void *arg)
{
    (void)arg;
    int64_t last_vacuum = 0;
    int counter = 0;

    while (g_running) {
        sleep(1);
        if (!g_running) break;
        counter++;
        if (counter < GC_INTERVAL) continue;
        counter = 0;

        /* 1. Reap stale sessions (sessions with open transactions are pinned) */
        pthread_mutex_lock(&sess_lock);
        int64_t now = (int64_t)time(NULL);
        int reaped = 0;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (sessions[i].id[0] && !sessions[i].txn_active &&
                now - sessions[i].last_active > g_session_ttl) {
                if (g_session_undo[i]) { free(g_session_undo[i]); g_session_undo[i] = NULL; }
                sessions[i].id[0] = 0;
                reaped = 1;
            }
        }
        if (reaped) g_session_dirty = 1;
        pthread_mutex_unlock(&sess_lock);

        /* 2. Auto-vacuum (non-blocking lock so we don't stall SQL) */
        if (pthread_rwlock_trywrlock(&exec_lock) == 0) {
            now = (int64_t)time(NULL);
            if (now - last_vacuum > VACUUM_INTERVAL) {
                uint32_t free = db_free_count(g_db);
                uint32_t total = g_db->page_count;
                if (total > 100 && free * 100 / total >= VACUUM_FREE_PCT) {
                    char err[MAX_ERR] = "";
                    db_vacuum(g_db, err);
                    last_vacuum = (int64_t)time(NULL);
                }
            }
            pthread_rwlock_unlock(&exec_lock);
        }
    }
    return NULL;
}

/* ----------------------------------------------------------- JSON helpers
 * All very ad-hoc and not intended to be robust against malicious input —
 * this is a personal-data daemon, not a public-facing web server. */

/* Find the value of a quoted string key in a JSON object and unescape it.
 * Returns buf on success, NULL if the key was not found.
 * Handles \" \\ \n \r \t escape sequences; other escapes are left as-is. */
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
            else if (*k == 'n') buf[pos++] = '\n';
            else if (*k == 'r') buf[pos++] = '\r';
            else if (*k == 't') buf[pos++] = '\t';
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

/* Decide whether a SQL batch is read-only by scanning it for write keywords.
 * A batch containing BEGIN/COMMIT/ROLLBACK or any DML/DDL keyword is a
 * writer; everything else (SELECT, RECALL, SHOW, PRINT, EXPLAIN) may run
 * under the read side of exec_lock. */
static int sql_is_read_only(const char *sql)
{
    static const char *const writes[] = {
        "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER", "TRUNCATE",
        "VACUUM", "CHECKPOINT", "BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT",
        "RELEASE", "REMEMBER", "FORGET", "CALL", "EXEC", "EXECUTE", NULL
    };
    Lexer lx;
    lex_init(&lx, sql);
    while (lex_next(&lx) == 0 && lx.cur.kind != TK_EOF) {
        if (lx.cur.kind == TK_IDENT) {
            for (int i = 0; writes[i]; i++)
                if (strcasecmp(lx.cur.text, writes[i]) == 0)
                    return 0;
        }
    }
    return 1;
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
        if (sess->txn_undo_depth > 0 && g_session_undo[sidx])
            memcpy(g_db->undo, g_session_undo[sidx],
                   sizeof(UndoEntry) * (size_t)sess->txn_undo_depth);
    } else {
        /* No session transaction: the shared DB struct must not be left
         * carrying another session's transaction state (a fresh BEGIN from
         * this session would otherwise nest on top of it). */
        g_db->txn_active = 0;
        g_db->txn_depth = 0;
        g_db->undo_depth = 0;
        g_db->txn_sp_count = 0;
    }

    Lexer lx;
    lex_init(&lx, sql);
    int lex_first = lex_next(&lx);
    if (lex_first < 0) {
        snprintf(out, outsz, "{\"ok\":false,\"error\":\"lex error\"}");
        return;
    }

    /* Capture engine text output through a per-thread temp file instead of
     * replacing stdout (which would race across handler threads).  All
     * statement output goes through the g_output_file macro in exec.c and
     * select.c. */
    FILE *capf = tmpfile();
    char *captured = NULL;
    if (!capf) {
        snprintf(out, outsz, "{\"ok\":false,\"error\":\"internal: tmpfile failed\"}");
        return;
    }
    g_output_file = capf;

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

    /* Restore output capture */
    g_select_capture = NULL;
    fflush(g_output_file);
    g_output_file = NULL;

    /* Read the captured output from the temp file */
    off_t fsize = ftello(capf);
    rewind(capf);
    if (fsize > 0) {
        captured = malloc((size_t)fsize + 1);
        if (captured) {
            size_t n = fread(captured, 1, (size_t)fsize, capf);
            captured[n] = 0;
        }
    }
    fclose(capf);

    if (sess) {
        sess->last_active = (int64_t)time(NULL);
        sess->txn_active = g_db->txn_active;
        sess->txn_depth = g_db->txn_depth;
        sess->txn_undo_depth = g_db->undo_depth;
        sess->txn_sp_count = g_db->txn_sp_count;
        memcpy(sess->txn_sp, g_db->txn_sp,
               sizeof(int) * (size_t)g_db->txn_sp_count);
        if (g_db->undo_depth > 0) {
            if (!g_session_undo[sidx])
                g_session_undo[sidx] = malloc(sizeof(UndoEntry) * (size_t)UNDO_MAX);
            if (g_session_undo[sidx])
                memcpy(g_session_undo[sidx], g_db->undo,
                       sizeof(UndoEntry) * (size_t)g_db->undo_depth);
        }
        g_session_dirty = 1;
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
        g_session_dirty = 1;
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
            char observe[8] = "";
            json_str(json, "sql", sql, sizeof sql);
            json_str(json, "session", sid, sizeof sid);
            json_str(json, "token", tok, sizeof tok);
            json_str(json, "observe", observe, sizeof observe);

            const char *active_sid = sid[0] ? sid : session_id;

            if (g_token[0] && strcmp(tok, g_token) != 0) {
                snprintf(resp, RESP_BUF,
                         "{\"ok\":false,\"error\":\"unauthorized\"}\n");
                conn_write(ssl, fd, resp, strlen(resp));
            } else if (sql[0]) {
                /* CLI clients can opt out of observer reinforcement for a
                 * single request with "observe":"0" (the -r flag).  This
                 * thread's executor globals are toggled only for the
                 * duration of this request. */
                int save_observe = exec_reinforce_enabled();
                if (observe[0] && strcmp(observe, "0") == 0)
                    exec_set_reinforce(0);
                else
                    exec_set_reinforce(1);
                /* Read-only batches run concurrently under the read side of
                 * exec_lock; everything else (writes, and whole transactions)
                 * takes the write side.  Within a transaction the write lock
                 * is held from BEGIN until COMMIT/ROLLBACK, so a concurrent
                 * transaction can never interleave with it. */
                int is_read = 0;
                if (!g_txn_lock_held) {
                    is_read = sql_is_read_only(sql);
                    if (is_read) {
                        pthread_rwlock_rdlock(&exec_lock);
                        mem_set_defer(1);
                    } else {
                        pthread_rwlock_wrlock(&exec_lock);
                        g_txn_lock_held = 1;
                    }
                }
                execute_sql(sql, active_sid, resp, RESP_BUF);
                exec_set_reinforce(save_observe);

                if (is_read) {
                    /* Reads must not write pages (reinforcement), so the
                     * touches were deferred; apply them now under a write
                     * lock. */
                    mem_set_defer(0);
                    pthread_rwlock_unlock(&exec_lock);
                    pthread_rwlock_wrlock(&exec_lock);
                    mem_flush_pending(g_db);
                    pthread_rwlock_unlock(&exec_lock);
                } else {
                    /* Drop the write lock unless the session is still in a
                     * transaction */
                    pthread_mutex_lock(&sess_lock);
                    int tx_sidx = session_find(active_sid);
                    int still_in_txn = (tx_sidx >= 0 && sessions[tx_sidx].txn_active);
                    pthread_mutex_unlock(&sess_lock);
                    if (!still_in_txn) {
                        pthread_rwlock_unlock(&exec_lock);
                        g_txn_lock_held = 0;
                    }
                }

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

    /* Client went away mid-transaction: roll back and release the lock. */
    if (g_txn_lock_held) {
        execute_sql("ROLLBACK", session_id, resp, RESP_BUF);
        pthread_rwlock_unlock(&exec_lock);
        g_txn_lock_held = 0;
    }

    pthread_mutex_lock(&sess_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (strcmp(sessions[i].id, session_id) == 0) {
            if (g_session_undo[i]) { free(g_session_undo[i]); g_session_undo[i] = NULL; }
            sessions[i].id[0] = 0;
            g_session_dirty = 1;
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

/* ------------------------------------------------------- signal handling
 * macOS signal routing to a multi-threaded process is unreliable: a signal
 * can be lost if the kernel picks a thread stuck in a syscall such as
 * select().  Instead of signal handlers we block SIGINT/SIGTERM/SIGQUIT in
 * every thread and dedicate one thread to sigwait(); it sets g_running and
 * writes to a self-pipe so the accept-loop select() wakes up immediately. */
static int g_sigpipe[2] = {-1, -1};

static void *sig_thread(void *arg)
{
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGQUIT);
    for (;;) {
        int sig;
        if (sigwait(&set, &sig) != 0) break;
        g_running = 0;
        if (g_sigpipe[1] >= 0) {
            char c = 1;
            ssize_t r = write(g_sigpipe[1], &c, 1);
            (void)r;
        }
    }
    return NULL;
}
/* Create a Unix listen socket at `path`.  Stale sockets (left by a previous
 * run that died without cleaning up) are unlinked first.  Returns the fd, or
 * -1 with an error already printed. */
static int unix_listen(const char *path)
{
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "error: unix socket: %s\n", strerror(errno));
        return -1;
    }
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&un, sizeof un) < 0) {
        fprintf(stderr, "error: bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0700);
    if (listen(fd, 16) < 0) {
        fprintf(stderr, "error: listen %s: %s\n", path, strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
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

    /* Block shutdown signals in every thread; the dedicated sig_thread
     * receives them via sigwait() (see sig_thread above).  This must happen
     * before any worker threads are created. */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGQUIT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);
    if (pipe(g_sigpipe) < 0) {
        g_sigpipe[0] = g_sigpipe[1] = -1;
    }
    pthread_t sig_thread_h;
    pthread_create(&sig_thread_h, NULL, sig_thread, NULL);
    pthread_detach(sig_thread_h);

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

    /* Per-database Unix socket.  Its path is derived deterministically from
     * the database file (<db>.sock) so any other nexdb process can find the
     * server and route requests through it (PostgreSQL-style: the daemon is
     * the only process that ever opens the data file; everyone else is a
     * client).  A failure here is not fatal — clients just cannot reach the
     * server through the socket. */
    char auto_sock[1100];
    snprintf(auto_sock, sizeof auto_sock, "%s.sock", db->path);
    int auto_unix_fd = unix_listen(auto_sock);
    if (auto_unix_fd < 0)
        fprintf(stderr, "warning: cannot create per-database socket %s; "
                        "other nexdb processes will not be able to route "
                        "through this server\n", auto_sock);

    /* Optional extra Unix socket */
    int unix_fd = -1;
    char sock_path[256];
    if (unix_path && unix_path[0]) {
        snprintf(sock_path, sizeof sock_path, "%s", unix_path);
        unix_fd = unix_listen(sock_path);
        if (unix_fd < 0) {
            close(listen_fd);
            if (auto_unix_fd >= 0) { close(auto_unix_fd); unlink(auto_sock); }
            return -1;
        }
    }

    printf("nexdb server listening on port %d", port);
    if (auto_unix_fd >= 0) printf(", unix:%s", auto_sock);
    if (unix_fd >= 0) printf(", unix:%s", sock_path);
    printf("\n");

    /* Session file sits next to the database */
    snprintf(g_session_path, sizeof g_session_path, "%s.sessions", db->path);

    /* Restore persisted sessions */
    sessions_load();

    /* Start background GC thread */
    pthread_t gc_thread;
    pthread_create(&gc_thread, NULL, gc_loop, NULL);

    /* Accept loop */
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        if (g_sigpipe[0] >= 0) {
            FD_SET(g_sigpipe[0], &rfds);
            if (g_sigpipe[0] > maxfd) maxfd = g_sigpipe[0];
        }
        if (unix_fd >= 0) {
            FD_SET(unix_fd, &rfds);
            if (unix_fd > maxfd) maxfd = unix_fd;
        }
        if (auto_unix_fd >= 0) {
            FD_SET(auto_unix_fd, &rfds);
            if (auto_unix_fd > maxfd) maxfd = auto_unix_fd;
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

        /* Persist sessions to disk (~1/s throttle from select timeout) */
        if (g_session_dirty) {
            g_session_dirty = 0;
            sessions_save();
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
        } else if (auto_unix_fd >= 0 && FD_ISSET(auto_unix_fd, &rfds)) {
            struct sockaddr_un ca;
            socklen_t clen = sizeof ca;
            cfd = accept(auto_unix_fd, (struct sockaddr *)&ca, &clen);
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

    /* Stop background GC thread */
    g_running = 0;
    pthread_join(gc_thread, NULL);

    /* Final session flush */
    sessions_save();

    close(listen_fd);
    if (unix_fd >= 0) {
        close(unix_fd);
        unlink(sock_path);
    }
    if (auto_unix_fd >= 0) {
        close(auto_unix_fd);
        unlink(auto_sock);
    }
    if (g_sigpipe[0] >= 0) close(g_sigpipe[0]);
    if (g_sigpipe[1] >= 0) close(g_sigpipe[1]);
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

/* Connect to "host:port" or a unix socket path (address starting with '/').
 * Returns a connected fd, or -1 with an error already printed. */
static int client_connect(const char *address)
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
    return fd;
}

int server_connect_repl(const char *address, const char *token)
{
    int fd = client_connect(address);
    if (fd < 0) return -1;

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
        rp += sprintf(rp, "\",\"session\":\"%s\"", session);
        if (token && token[0])
            rp += sprintf(rp, ",\"token\":\"%s\"", token);
        rp += sprintf(rp, "}\n");
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

/* One-shot client: send a single batch of SQL to a server and print the
 * response the same way exec_script would.  Used by the CLI to route -c, -f
 * and .read through the daemon when the database file is already open in
 * another process.  observe=0 sends "observe":"0" so queries do not
 * reinforce memory (the -r flag). */
int server_proxy_exec(const char *address, const char *token,
                      const char *sql, int observe)
{
    int fd = client_connect(address);
    if (fd < 0) return 1;

    char welcome[4096];
    if (recv_line(fd, welcome, sizeof welcome) < 0) {
        close(fd);
        return 1;
    }
    char session[37] = "";
    json_str(welcome, "session", session, sizeof session);

    char req[131072];
    char *rp = req;
    rp += sprintf(rp, "{\"sql\":\"");
    for (const char *s = sql; *s; s++) {
        if (*s == '"' || *s == '\\') *rp++ = '\\';
        if (rp - req >= (ptrdiff_t)sizeof req - 8) break;
        *rp++ = *s;
    }
    rp += sprintf(rp, "\",\"session\":\"%s\"", session);
    if (token && token[0])
        rp += sprintf(rp, ",\"token\":\"%s\"", token);
    if (!observe)
        rp += sprintf(rp, ",\"observe\":\"0\"");
    rp += sprintf(rp, "}\n");

    ssize_t w = write(fd, req, strlen(req));
    if (w < 0) {
        fprintf(stderr, "error: write: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    char resp[65536];
    if (recv_line(fd, resp, sizeof resp) < 0) {
        fprintf(stderr, "error: server closed the connection\n");
        close(fd);
        return 1;
    }
    close(fd);

    char text[65536] = "";
    char err[4096] = "";
    json_str(resp, "text", text, sizeof text);
    json_str(resp, "error", err, sizeof err);

    if (err[0]) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    if (text[0])
        printf("%s\n", text);
    return 0;
}
