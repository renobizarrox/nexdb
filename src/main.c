/* main.c - the nexdb shell.
 *
 *   nexdb mydata.ndb              interactive
 *   nexdb mydata.ndb -f setup.sql run a script
 *   nexdb mydata.ndb -c "SELECT * FROM notes"
 *   nexdb serve mydata.ndb        start daemon (port 7890)
 *   nexdb --connect localhost:7890  remote REPL
 *
 * When the database file is already open in another nexdb process, the shell
 * automatically routes -c/-f and the interactive prompt through that server's
 * per-database unix socket (<db>.sock).
 */
#define _GNU_SOURCE
#include "nexdb.h"
#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static const char *BANNER =
"nexdb 0.1 - a database that remembers what you use\n"
"type .help for shell commands, GO or ; to run a batch, .exit to leave\n";

static const char *HELP =
"shell commands\n"
"  .help              this text\n"
"  .tables            list tables (same as SHOW TABLES)\n"
"  .schema <table>    column definitions\n"
"  .memory <table>    memory state, strongest rows first\n"
"  .links             strongest associations\n"
"  .read <file>       execute a .sql file\n"
"  .exit / .quit      close cleanly (flushes memory to disk)\n"
"\n"
"sql\n"
"  CREATE TABLE t (id INT, note NVARCHAR(200))\n"
"  INSERT INTO t (id, note) VALUES (1, 'buy milk')\n"
"  SELECT TOP 10 * FROM t WHERE note LIKE '%milk%' ORDER BY _strength DESC\n"
"  UPDATE t SET note = 'buy oat milk' WHERE id = 1\n"
"  DELETE FROM t WHERE id = 1\n"
"\n"
"memory\n"
"  RECALL 'milk'                 fuzzy, association-aware lookup\n"
"  REMEMBER FROM t WHERE id = 1  reinforce by hand\n"
"  FORGET FROM t WHERE id = 1    fade a row out\n"
"  SHOW MEMORY FROM t            per-row strength and access counts\n"
"  SHOW LINKS                    which rows have wired together\n"
"  CHECKPOINT                    flush to disk now\n"
"\n"
"pseudo-columns usable anywhere a column is allowed:\n"
"  _rid  _strength  _access  _last_access\n";

/* Load an entire file into a malloc'd, NUL-terminated buffer. Caller frees. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

/* Print a CREATE TABLE-style description of one table from the catalog. */
static void schema_of(DB *db, const char *name)
{
    Table *t = cat_find(db, name);
    if (!t) { printf("unknown table '%s'\n", name); return; }
    printf("CREATE TABLE %s (\n", t->name);
    for (int i = 0; i < t->ncols; i++) {
        const Column *c = &t->cols[i];
        char decl[96];

        /* report the declared type honestly, including the width and length
         * that are now actually enforced */
        if (c->type == T_INT) {
            snprintf(decl, sizeof decl, "%s", int_sub_name(c->sub));
        } else if (c->type == T_TEXT && c->is_datetime) {
            snprintf(decl, sizeof decl, "DATETIME");
        } else if (c->type == T_TEXT && c->maxlen) {
            snprintf(decl, sizeof decl, "NVARCHAR(%u)", c->maxlen);
        } else if (c->type == T_TEXT) {
            snprintf(decl, sizeof decl, "NVARCHAR(MAX)");
        } else {
            snprintf(decl, sizeof decl, "%s", type_name(c->type));
        }

        /* assemble the whole declaration first, so the comma sits against the
         * text instead of floating out past the padding */
        char line[192];
        snprintf(line, sizeof line, "%-24s %s%s%s%s", c->name, decl,
                 c->is_pk ? " PRIMARY KEY" : "",
                 (c->unique && !c->is_pk) ? " UNIQUE" : "",
                 (c->not_null && !c->is_pk) ? " NOT NULL" : "");
        printf("    %s%s\n", line, i + 1 < t->ncols ? "," : "");
    }
    printf(");\n-- %lld row%s stored\n", (long long)t->nrows,
           t->nrows == 1 ? "" : "s");
}

/* Returns 1 if the line was a shell command and has been handled. */
static int shell_command(DB *db, const char *line, int *quit)
{
    while (isspace((unsigned char)*line)) line++;
    if (*line != '.') return 0;

    char cmd[64] = {0}, arg[512] = {0};
    sscanf(line, ".%63s %511[^\n]", cmd, arg);

    /* trim trailing whitespace off the argument */
    size_t al = strlen(arg);
    while (al && isspace((unsigned char)arg[al - 1])) arg[--al] = 0;

    if (!strcasecmp(cmd, "exit") || !strcasecmp(cmd, "quit")) {
        *quit = 1;
        return 1;
    }
    if (!strcasecmp(cmd, "help")) { printf("%s", HELP); return 1; }

    char sql[600];
    if (!strcasecmp(cmd, "tables")) {
        exec_script(db, "SHOW TABLES", 0);
        return 1;
    }
    if (!strcasecmp(cmd, "schema")) {
        if (!arg[0]) printf("usage: .schema <table>\n");
        else schema_of(db, arg);
        return 1;
    }
    if (!strcasecmp(cmd, "memory")) {
        if (!arg[0]) { printf("usage: .memory <table>\n"); return 1; }
        snprintf(sql, sizeof sql, "SHOW MEMORY FROM [%s]", arg);
        exec_script(db, sql, 0);
        return 1;
    }
    if (!strcasecmp(cmd, "links")) {
        exec_script(db, "SHOW LINKS", 0);
        return 1;
    }
    if (!strcasecmp(cmd, "read")) {
        if (!arg[0]) { printf("usage: .read <file>\n"); return 1; }
        char *src = read_file(arg);
        if (!src) { printf("cannot read '%s'\n", arg); return 1; }
        exec_script(db, src, 0);
        free(src);
        return 1;
    }

    printf("unknown shell command '.%s' - try .help\n", cmd);
    return 1;
}

/* True if the trimmed line is exactly the batch terminator GO. */
static int is_go(const char *line)
{
    while (isspace((unsigned char)*line)) line++;
    if (strncasecmp(line, "go", 2) != 0) return 0;
    line += 2;
    while (*line) {
        if (!isspace((unsigned char)*line) && *line != ';') return 0;
        line++;
    }
    return 1;
}

static int ends_with_semicolon(const char *buf)
{
    size_t n = strlen(buf);
    while (n && isspace((unsigned char)buf[n - 1])) n--;
    return n && buf[n - 1] == ';';
}

/* True if the buffered text is inside an unclosed BEGIN ... END block or
 * CASE expression. The REPL uses this so a multi-line CREATE PROCEDURE body
 * is not fired off at the first interior ';'. */
static int block_balance(const char *buf)
{
    Lexer lx;
    lex_init(&lx, buf);
    int bal = 0;
    while (lex_next(&lx) == 0 && lx.cur.kind != TK_EOF) {
        if (lx.cur.kind != TK_IDENT) continue;
        if (strcasecmp(lx.cur.text, "BEGIN") == 0) {
            Token *pk = lex_peek(&lx);
            if (!(pk->kind == TK_IDENT &&
                  (strcasecmp(pk->text, "TRANSACTION") == 0 ||
                   strcasecmp(pk->text, "TRAN") == 0)))
                bal++;
        } else if (strcasecmp(lx.cur.text, "CASE") == 0) {
            bal++;
        } else if (strcasecmp(lx.cur.text, "END") == 0) {
            bal--;
        }
    }
    return bal;
}

/* Interactive read-eval-print loop: buffer SQL until ';' or GO, then execute. */
static void repl(DB *db)
{
    char *batch = NULL;
    size_t blen = 0, bcap = 0;
    char line[4096];
    int quit = 0;

    printf("%s\n", BANNER);

    while (!quit) {
        printf("%s", blen ? "  ...> " : "nexdb> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { printf("\n"); break; }

        if (blen == 0 && shell_command(db, line, &quit)) continue;

        int go = is_go(line);
        if (!go) {
            size_t ll = strlen(line);
            if (blen + ll + 1 > bcap) {
                bcap = (blen + ll + 1) * 2;
                char *nb = realloc(batch, bcap);
                if (!nb) { fprintf(stderr, "out of memory\n"); break; }
                batch = nb;
            }
            memcpy(batch + blen, line, ll + 1);
            blen += ll;
        }

        int run = go || (blen && ends_with_semicolon(batch) && block_balance(batch) <= 0);
        if (run && blen) {
            exec_script(db, batch, 0);
            blen = 0;
            if (batch) batch[0] = 0;
        } else if (go) {
            blen = 0;
            if (batch) batch[0] = 0;
        }
    }
    free(batch);
}

/* --------------------------------------------------------- serve subcommand */
static int main_server(int argc, char **argv)
{
    const char *path = NULL;
    int port = 7890;
    const char *unix_path = NULL;
    const char *token = NULL;
    int session_ttl = 300;
    int daemon = 0;
    const char *pidfile = NULL;
    const char *tls_cert = NULL;
    const char *tls_key = NULL;

    /* Skip argv[0] ("serve") */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--unix") == 0 && i + 1 < argc)
            unix_path = argv[++i];
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc)
            token = argv[++i];
        else if (strcmp(argv[i], "--session-ttl") == 0 && i + 1 < argc)
            session_ttl = atoi(argv[++i]);
        else if (strcmp(argv[i], "--daemon") == 0)
            daemon = 1;
        else if (strcmp(argv[i], "--pidfile") == 0 && i + 1 < argc)
            pidfile = argv[++i];
        else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc)
            tls_cert = argv[++i];
        else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc)
            tls_key = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!path)
            path = argv[i];
        else {
            fprintf(stderr, "unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!path) {
        fprintf(stderr,
                "usage: nexdb serve <database-file> [--port N] [--unix PATH]\n"
                "                    [--token STR] [--session-ttl SECS]\n"
                "                    [--daemon] [--pidfile FILE]\n"
                "                    [--tls-cert FILE --tls-key FILE]\n");
        return 2;
    }

    /* Daemonise before opening the DB (so the child owns the file) */
    if (daemon) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) {
            /* Parent: write PID file if requested and exit */
            if (pidfile) {
                FILE *pf = fopen(pidfile, "w");
                if (pf) {
                    fprintf(pf, "%d\n", pid);
                    fclose(pf);
                } else {
                    fprintf(stderr, "error: cannot write pidfile '%s': %s\n",
                            pidfile, strerror(errno));
                }
            }
            exit(0);
        }
        /* Child: become session leader */
        if (setsid() < 0) { perror("setsid"); return 1; }
        /* Redirect stdio to /dev/null */
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, 0); dup2(nullfd, 1); dup2(nullfd, 2);
            close(nullfd);
        }
    }

    DB *db = malloc(sizeof(DB));
    if (!db) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    if (db_open(db, path) < 0) {
        fprintf(stderr, "error: %s\n", db->err);
        free(db);
        return 1;
    }

    int rc = server_run(db, port, unix_path, token, session_ttl,
                        tls_cert, tls_key);

    /* Remove PID file on clean shutdown */
    if (pidfile) unlink(pidfile);

    db_close(db);
    free(db);
    return rc ? 1 : 0;
}

/* Run a -c/-f/interactive session against a running server that owns the
 * database file, through its deterministic per-database unix socket. */
static int route_through_server(const char *path, const char *token,
                                const char *command, const char *script)
{
    char sock[1100];
    snprintf(sock, sizeof sock, "%s.sock", path);

    int observe = exec_reinforce_enabled();

    if (command)
        return server_proxy_exec(sock, token, command, observe);
    if (script) {
        char *src = read_file(script);
        if (!src) {
            fprintf(stderr, "error: cannot read '%s'\n", script);
            return 1;
        }
        int rc = server_proxy_exec(sock, token, src, observe);
        free(src);
        return rc;
    }
    /* Interactive session through the proxy REPL.  Shell commands (.read,
     * .tables, ...) are not available over the socket. */
    return server_connect_repl(sock, token) ? 1 : 0;
}

int main(int argc, char **argv)
{
    /* Serve subcommand */
    if (argc >= 2 && strcmp(argv[1], "serve") == 0)
        return main_server(argc - 1, argv + 1);

    /* Remote connect mode --client host:port (or unix path) */
    const char *conn_token = NULL;
    const char *connect_addr = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--connect") == 0) {
            if (i + 1 < argc)
                connect_addr = argv[i + 1];
            else {
                fprintf(stderr, "error: --connect requires an address (host:port or /path)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            conn_token = argv[i + 1];
        }
    }
    if (connect_addr)
        return server_connect_repl(connect_addr, conn_token);

    const char *path = NULL;
    const char *script = NULL;
    const char *command = NULL;
    const char *token = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc)      script = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) command = argv[++i];
        else if (!strcmp(argv[i], "--token") && i + 1 < argc) token = argv[++i];
        else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--no-reinforce")) {
            exec_set_reinforce(0);
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: nexdb <database-file> [options]\n"
                   "  -f <file>        run a SQL script\n"
                   "  -c \"<sql>\"       run one batch of SQL\n"
                   "  -r               observer mode: queries do not reinforce\n"
                   "                   what they read, so you can inspect the\n"
                   "                   memory without changing it\n"
                   "  --token STR      auth token when routed through a\n"
                   "                   running server\n"
                   "  -h               this text\n"
                   "\n"
                   "remote / server:\n"
                   "  nexdb serve <db> [--port N] [--unix PATH] [--token STR]\n"
                   "                 [--session-ttl SECS] [--daemon] [--pidfile FILE]\n"
                   "                 [--tls-cert FILE --tls-key FILE]\n"
                   "  nexdb --connect host:port [--token STR]\n"
                   "\n"
                   "If <database-file> is already open in another nexdb process\n"
                   "the shell automatically routes -c, -f and the interactive\n"
                   "prompt through that server's unix socket (<db>.sock).\n"
                   "\n"
                   "environment:\n"
                   "  NEXDB_TIME_OFFSET  seconds to add to the clock, for\n"
                   "                     watching decay without waiting\n");
            return 0;
        }
        else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("nexdb %s\n", NEXDB_VERSION);
            return 0;
        }
        /* Anything else that looks like a flag is a mistake. Treating it as a
         * filename meant "nexdb --version" created a database called
         * "--version" and dropped you into the shell. */
        else if (argv[i][0] == '-' && argv[i][1] != 0) {
            fprintf(stderr, "unknown option '%s' (try -h)\n", argv[i]);
            return 2;
        }
        else if (!path) path = argv[i];
        else { fprintf(stderr, "unexpected argument '%s'\n", argv[i]); return 2; }
    }

    if (!path) path = "nexdb.ndb";

    /* DB is over ten megabytes (the catalog holds every table's metadata), so
     * it lives on the heap; a stack frame that big would overflow. */
    DB *db = malloc(sizeof(DB));
    if (!db) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    if (db_open(db, path) < 0) {
        /* The file is held by another process — almost certainly a running
         * `nexdb serve` on the same database.  Route the request through
         * that server's per-database unix socket (PostgreSQL-style: only
         * the daemon ever touches the data file). */
        if (strstr(db->err, "already open in another nexdb process")) {
            int rc = route_through_server(path, token, command, script);
            free(db);
            return rc;
        }
        fprintf(stderr, "error: %s\n", db->err);
        free(db);
        return 1;
    }

    int rc = 0;
    if (command) {
        rc = exec_script(db, command, 0);
    } else if (script) {
        char *src = read_file(script);
        if (!src) {
            fprintf(stderr, "error: cannot read '%s'\n", script);
            db_close(db);
            free(db);
            return 1;
        }
        rc = exec_script(db, src, 0);
        free(src);
    } else {
        repl(db);
    }

    db_close(db);
    free(db);
    return rc ? 1 : 0;
}
