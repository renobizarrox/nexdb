/* exec.c - the executor.
 *
 * Ordinary SQL runs the way you would expect. The interesting part is that
 * every statement that actually reads rows also reinforces them:
 *
 *   SELECT / RECALL  -> boost each returned row, and associate the returned
 *                       set with itself (co-access = Hebbian learning)
 *   UPDATE           -> boost, since writing is a strong signal of relevance
 *   DELETE           -> drop the row's associations
 *
 * So the database's idea of what matters is a by-product of how it gets used;
 * nobody has to maintain a priority column by hand.
 */
#define _GNU_SOURCE
#include "nexdb.h"
#include "wal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

/* When non-NULL, statement output goes here instead of stdout */
__thread FILE *g_output_file = NULL;

/* Capture all printf/putchar through g_output_file so the TCP server can
 * capture query text output without relying on stdout replacement (which
 * does not work on macOS due to libc internals using a separate __stdoutp). */
#undef printf
#define printf(...)   fprintf(g_output_file ? g_output_file : stdout, __VA_ARGS__)
#undef putchar
#define putchar(c)    fputc((c), g_output_file ? g_output_file : stdout)

/* memory pseudo-columns, addressable in SELECT lists, WHERE and ORDER BY */
#define PC_RID      (-2)
#define PC_STRENGTH (-3)
#define PC_ACCESS   (-4)
#define PC_LAST     (-5)
#define PC_SCORE    (-6)

/* Resolve _rid, _strength, etc. to internal pseudo-column indices, or -1. */
static int pseudo_col(const char *name)
{
    if (!strcasecmp(name, "_rid"))          return PC_RID;
    if (!strcasecmp(name, "_strength"))     return PC_STRENGTH;
    if (!strcasecmp(name, "_access"))       return PC_ACCESS;
    if (!strcasecmp(name, "_access_count")) return PC_ACCESS;
    if (!strcasecmp(name, "_last"))         return PC_LAST;
    if (!strcasecmp(name, "_last_access"))  return PC_LAST;
    if (!strcasecmp(name, "_score"))        return PC_SCORE;
    return -1;
}

/* Aggregate results for the group currently being projected. EX_AGG reads its
 * value from here rather than from the row, because an aggregate is a property
 * of a whole group. NULL outside a grouped projection. */
static __thread const Value *g_agg_values = NULL;
static __thread int          g_agg_count  = 0;

/* Stored-procedure CALL nesting depth (bodies may CALL other procedures, and
 * EXEC ('...') counts as a level so self-referential dynamic SQL is capped). */
#define MAX_PROC_DEPTH 128
static __thread int g_proc_depth = 0;

/* Return code of the innermost finished CALL, written by RETURN <expr> and
 * read by "EXEC @rc = proc" (ST_CALL retvar). */
static __thread int g_proc_retval = 0;

/* -------------------------------------------------------- procedure state */

#define MAX_VARS    64
#define MAX_CURSORS 16

/* Variable frame: parameters first (in declaration order), then variables
 * created by DECLARE as they are first executed, then any that a FETCH or
 * DECLARE adds later. Slot 0 is always @@fetch_status, written by FETCH.
 * Variables are block-scoped: each BEGIN ... END pushes a scope, and the
 * variables declared inside it are removed when it closes. */
typedef struct {
    char  name[MAX_NAME];
    Value v;
} VarSlot;

#define MAX_SCOPES 32

typedef struct {
    VarSlot slots[MAX_VARS];
    int     n;
    int     nparams;   /* slots[1 .. nparams] are the procedure's parameters */
    int     scope_depth;
    int     scope_overflow;   /* blocks nested beyond MAX_SCOPES: no-op scopes */
    int     scope_base[MAX_SCOPES];  /* slots[scope_base[d] .. n) are scope d */
} VarFrame;

static __thread VarFrame *g_var_frame = NULL;

/* Procedure-scoped cursors. sel is borrowed: it points into the parsed
 * program's statement tree and lives exactly as long as the CALL. */
typedef struct {
    char      name[MAX_NAME];
    Stmt     *sel;
    Capture   cap;
    int       opened;
    int       pos;      /* next row to FETCH */
} Cursor;

static __thread Cursor g_cursors[MAX_CURSORS];
static __thread int    g_ncursors = 0;

static Cursor *cursor_find(const char *name)
{
    for (int i = 0; i < g_ncursors; i++)
        if (strcasecmp(g_cursors[i].name, name) == 0)
            return &g_cursors[i];
    return NULL;
}

/* Innermost scope first, so a variable declared in an inner block shadows
 * one with the same name in an outer block or among the parameters. */
static VarSlot *var_find(const char *name)
{
    if (!g_var_frame) return NULL;
    for (int i = g_var_frame->n - 1; i >= 0; i--)
        if (strcasecmp(g_var_frame->slots[i].name, name) == 0)
            return &g_var_frame->slots[i];
    return NULL;
}

/* Push a variable scope for a BEGIN ... END block. */
static void scope_push(VarFrame *f)
{
    if (f->scope_depth < MAX_SCOPES - 1) {
        f->scope_base[++f->scope_depth] = f->n;
    } else {
        /* beyond the cap a scope is a no-op, so push and pop stay balanced */
        f->scope_overflow++;
    }
}

/* Pop the innermost scope, removing the variables declared inside it. */
static void scope_pop(VarFrame *f)
{
    if (f->scope_overflow) {
        f->scope_overflow--;
        return;
    }
    if (f->scope_depth > 0) {
        for (int i = f->scope_base[f->scope_depth]; i < f->n; i++)
            val_clear(&f->slots[i].v);
        f->n = f->scope_base[f->scope_depth];
        f->scope_depth--;
    }
}

/* True if a variable of this name exists in the current scope. */
static int var_in_scope(const VarFrame *f, const char *name)
{
    for (int i = f->scope_base[f->scope_depth]; i < f->n; i++)
        if (strcasecmp(f->slots[i].name, name) == 0)
            return 1;
    return 0;
}

/* control-flow return codes for exec_proc_stmt; negative = error */
enum { P_OK = 0, P_BREAK, P_CONTINUE, P_RETURN };

static int exec_proc_stmt(DB *db, Stmt *s, char *err);

int pseudo_col_index(const char *name) { return pseudo_col(name); }

/* select.c installs the current group's aggregate results here before it
 * projects that group, and clears them afterwards. */
void exec_set_agg_context(const Value *vals, int n)
{
    g_agg_values = vals;
    g_agg_count = n;
}

static void fmt_time(int64_t ts, char *out, size_t cap)
{
    if (ts <= 0) { snprintf(out, cap, "never"); return; }
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* ------------------------------------------------------- LIKE matching */

/* Iterative backtracking matcher for SQL LIKE: % = any run, _ = one char.
 *
 * `esc` is the ESCAPE character, or 0 for none. A character following the
 * escape is matched literally, which is the only way to search for a real
 * '%' or '_'. */
int like_match_esc(const char *text, const char *pat, char esc)
{
    const char *t = text, *p = pat;
    const char *star = NULL, *tmark = NULL;

    while (*t) {
        if (esc && *p == esc && p[1]) {
            /* escaped literal: must match exactly, no wildcard meaning */
            if (tolower((unsigned char)p[1]) == tolower((unsigned char)*t)) {
                p += 2; t++; continue;
            }
            if (star) { p = star + 1; t = ++tmark; continue; }
            return 0;
        }
        if (*p == '%') { star = p++; tmark = t; continue; }
        if (*p == '_' || tolower((unsigned char)*p) == tolower((unsigned char)*t)) {
            p++; t++; continue;
        }
        if (star) { p = star + 1; t = ++tmark; continue; }
        return 0;
    }

    while (*p == '%') p++;
    /* a trailing escaped character still needs something to match */
    if (esc && *p == esc && p[1]) return 0;
    return *p == 0;
}

int like_match(const char *text, const char *pat)
{
    return like_match_esc(text, pat, 0);
}

/* ------------------------------------------------------ expression eval */

static double row_strength_value(const Row *row, int64_t now)
{
    return mem_strength_at(row->strength, row->last_access, now);
}

/* Extra per-row score slot used by RECALL; keyed positionally by the caller. */
static __thread double g_score_hint = 0.0;

/* Used by eval_expr to run subqueries. Set before statement execution. */
DB *g_db = NULL;

/* Reinforcement is turned off during subquery/insert-select execution. */
static __thread int g_reinforce = 1;

/* Join context: when set, eval_expr resolves qualified column references
 * (e.g. "t.col") against these tables/rows instead of the single Table/Row
 * passed directly. */
static __thread const Table **g_join_tables = NULL;
static __thread const Row   **g_join_rows   = NULL;
static __thread const char  **g_join_aliases = NULL;
static __thread int          g_join_ntables = 0;

/* Correlation context: outer table/row/name for correlated subqueries.
 * Set before executing a subquery so the inner query can resolve qualified
 * references to the outer table (e.g. SELECT 1 FROM u WHERE u.x = t.n). */
static __thread const Table *g_corr_table = NULL;
static __thread const Row   *g_corr_row   = NULL;
static __thread char         g_corr_name[MAX_NAME];

void exec_set_join_ctx(const Table **tables, const Row **rows, const char **aliases, int n)
{
    g_join_tables  = tables;
    g_join_rows    = rows;
    g_join_aliases = aliases;
    g_join_ntables = n;
}

/* Recursively evaluate an expression against one row (or aggregate context). */
int eval_expr(const Expr *e, const Row *row, const Table *t, int64_t now,
              Value *out, char *err)
{
    *out = val_null();
    if (!e) return 0;

    switch (e->kind) {
    case EX_LIT:
        *out = val_copy(&e->lit);
        return 0;

    case EX_VAR: {
        /* @name — a stored-procedure variable or parameter */
        VarSlot *slot = var_find(e->col);
        if (!slot) {
            snprintf(err, MAX_ERR, "variable '%s' is not defined", e->col);
            return -1;
        }
        *out = val_copy(&slot->v);
        return 0;
    }

    case EX_AGG:
        if (!g_agg_values || e->agg_slot < 0 || e->agg_slot >= g_agg_count) {
            snprintf(err, MAX_ERR,
                     "%s can only be used in a select list or HAVING clause",
                     e->fname);
            return -1;
        }
        *out = val_copy(&g_agg_values[e->agg_slot]);
        return 0;

    case EX_FUNC: {
        Value args[MAX_FUNC_ARGS];
        int n = 0;
        for (; n < e->nargs; n++) {
            if (eval_expr(e->args[n], row, t, now, &args[n], err) < 0) {
                for (int k = 0; k < n; k++) val_clear(&args[k]);
                return -1;
            }
        }
        int rc = func_call(e->fname, args, n, out, err);
        for (int k = 0; k < n; k++) val_clear(&args[k]);
        return rc;
    }

    case EX_CAST: {
        Value v;
        if (eval_expr(e->l, row, t, now, &v, err) < 0) return -1;
        int rc = cast_value(&v, e->cast_type, e->cast_sub, e->cast_len, out, err);
        val_clear(&v);
        return rc;
    }

    case EX_CASE: {
        /* items[] holds WHEN/THEN pairs; l is the optional operand, r the ELSE */
        Value operand = val_null();
        int have_operand = 0;
        if (e->l) {
            if (eval_expr(e->l, row, t, now, &operand, err) < 0) return -1;
            have_operand = 1;
        }
        for (int i = 0; i + 1 < e->nitems; i += 2) {
            Value cond;
            if (eval_expr(e->items[i], row, t, now, &cond, err) < 0) {
                val_clear(&operand);
                return -1;
            }
            int hit;
            if (have_operand) {
                int ok;
                hit = (val_compare(&operand, &cond, &ok) == 0 && ok == 1);
            } else {
                hit = val_truthy(&cond);
            }
            val_clear(&cond);
            if (hit) {
                val_clear(&operand);
                return eval_expr(e->items[i + 1], row, t, now, out, err);
            }
        }
        val_clear(&operand);
        if (e->r) return eval_expr(e->r, row, t, now, out, err);
        return 0;                                  /* no ELSE: NULL */
    }

    case EX_BETWEEN: {
        Value v, lo, hi;
        if (eval_expr(e->l, row, t, now, &v, err) < 0) return -1;
        if (v.tag == T_NULL) { val_clear(&v); return 0; }
        if (eval_expr(e->args[0], row, t, now, &lo, err) < 0) {
            val_clear(&v); return -1;
        }
        if (eval_expr(e->args[1], row, t, now, &hi, err) < 0) {
            val_clear(&v); val_clear(&lo); return -1;
        }
        int ok1, ok2;
        int c1 = val_compare(&v, &lo, &ok1);
        int c2 = val_compare(&v, &hi, &ok2);
        int rc = 0;
        if (ok1 == -1 || ok2 == -1) {
            snprintf(err, MAX_ERR, "BETWEEN bounds are not comparable with the "
                                   "value being tested");
            rc = -1;
        } else if (ok1 == 0 || ok2 == 0) {
            *out = val_null();
        } else {
            int in = (c1 >= 0 && c2 <= 0);
            *out = val_bit(e->negated ? !in : in);
        }
        val_clear(&v); val_clear(&lo); val_clear(&hi);
        return rc;
    }

    case EX_COL: {
        /* qualified column reference: table.col */
        if (e->col_table[0]) {
            if (g_join_tables && g_join_rows) {
                for (int ti = 0; ti < g_join_ntables; ti++) {
                    const Table *ct = g_join_tables[ti];
                    if (strcasecmp(e->col_table, ct->name) == 0 ||
                        (g_join_aliases && g_join_aliases[ti] &&
                         strcasecmp(e->col_table, g_join_aliases[ti]) == 0)) {
                        int idx = table_col_index(ct, e->col);
                        if (idx < 0) {
                            snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                                     e->col, ct->name);
                            return -1;
                        }
                        if (idx < g_join_rows[ti]->ncols) {
                            *out = val_copy(&g_join_rows[ti]->v[idx]);
                        }
                        return 0;
                    }
                }
            }
            /* try the current table (allows qualified refs in single-table queries) */
            if (t && strcasecmp(e->col_table, t->name) == 0) {
                int idx = table_col_index(t, e->col);
                if (idx >= 0) {
                    if (idx < row->ncols) *out = val_copy(&row->v[idx]);
                    return 0;
                }
            }
            /* try the correlation table (outer query in a correlated subquery) */
            if (g_corr_table && strcasecmp(e->col_table, g_corr_name) == 0) {
                int idx = table_col_index(g_corr_table, e->col);
                if (idx >= 0) {
                    if (g_corr_row && idx < g_corr_row->ncols)
                        *out = val_copy(&g_corr_row->v[idx]);
                    else
                        *out = val_null();
                    return 0;
                }
            }
            snprintf(err, MAX_ERR,
                     "no column '%s' in table '%s' (or outer query)",
                     e->col, e->col_table);
            return -1;
        }
        int pc = pseudo_col(e->col);
        if (pc != -1) {
            switch (pc) {
            case PC_RID:      *out = val_int((int64_t)row->rid); return 0;
            case PC_STRENGTH: *out = val_float(row_strength_value(row, now)); return 0;
            case PC_ACCESS:   *out = val_int((int64_t)row->access_count); return 0;
            case PC_LAST:     *out = val_int(row->last_access); return 0;
            case PC_SCORE:    *out = val_float(g_score_hint); return 0;
            }
        }
        int idx = table_col_index(t, e->col);
        if (idx < 0) {
            snprintf(err, MAX_ERR, "no column '%s' in table '%s'", e->col, t->name);
            return -1;
        }
        if (idx >= row->ncols) { *out = val_null(); return 0; }
        *out = val_copy(&row->v[idx]);
        return 0;
    }

    case EX_NEG: {
        Value v;
        if (eval_expr(e->l, row, t, now, &v, err) < 0) return -1;
        if (v.tag == T_FLOAT)      *out = val_float(-v.f);
        else if (v.tag == T_INT)   *out = val_int(-v.i);
        else                       *out = val_null();
        val_clear(&v);
        return 0;
    }

    case EX_NOT: {
        Value v;
        if (eval_expr(e->l, row, t, now, &v, err) < 0) return -1;
        int truth = val_truthy(&v);
        int isnull = (v.tag == T_NULL);
        val_clear(&v);
        *out = isnull ? val_null() : val_bit(!truth);
        return 0;
    }

    case EX_IS_NULL:
    case EX_IS_NOT_NULL: {
        Value v;
        if (eval_expr(e->l, row, t, now, &v, err) < 0) return -1;
        int isnull = (v.tag == T_NULL);
        val_clear(&v);
        *out = val_bit(e->kind == EX_IS_NULL ? isnull : !isnull);
        return 0;
    }

    case EX_IN: {
        Value lv;
        if (eval_expr(e->l, row, t, now, &lv, err) < 0) return -1;
        int found = 0;

        if (e->sub) {
            /* IN (SELECT ...) */
            if (!g_db) { val_clear(&lv); snprintf(err, MAX_ERR, "no database"); return -1; }
            int save_reinforce = g_reinforce;
            g_reinforce = 0;
            const Value *save_agg = g_agg_values;
            int save_agg_count = g_agg_count;
            const Table **save_jt = g_join_tables;
            const Row **save_jr = g_join_rows;
            const char **save_ja = g_join_aliases;
            int save_jn = g_join_ntables;
            double save_score = g_score_hint;
            const Table *save_ct = g_corr_table;
            const Row *save_cr = g_corr_row;
            char save_cn[MAX_NAME];
            memcpy(save_cn, g_corr_name, MAX_NAME);
            g_agg_values = NULL;
            g_agg_count = 0;
            g_join_tables = NULL;
            g_join_rows = NULL;
            g_join_aliases = NULL;
            g_join_ntables = 0;
            g_score_hint = 0.0;
            g_corr_table = t;
            g_corr_row = row;
            if (t) snprintf(g_corr_name, MAX_NAME, "%s", t->name);
            else g_corr_name[0] = 0;

            Capture cap = {0};
            int rc = exec_select_into(g_db, e->sub, &cap, err);

            g_agg_values = save_agg;
            g_agg_count = save_agg_count;
            g_join_tables = save_jt;
            g_join_rows = save_jr;
            g_join_aliases = save_ja;
            g_join_ntables = save_jn;
            g_score_hint = save_score;
            g_corr_table = save_ct;
            g_corr_row = save_cr;
            memcpy(g_corr_name, save_cn, MAX_NAME);
            g_reinforce = save_reinforce;

            if (rc < 0) { val_clear(&lv); return -1; }
            for (int i = 0; i < cap.nrows && !found; i++) {
                int ok;
                if (val_compare(&lv, &cap.cells[i * cap.ncols], &ok) == 0 && ok == 1)
                    found = 1;
            }
            capture_free(&cap);
        } else {
            for (int i = 0; i < e->nitems && !found; i++) {
                Value rv;
                if (eval_expr(e->items[i], row, t, now, &rv, err) < 0) {
                    val_clear(&lv);
                    return -1;
                }
                int ok;
                if (val_compare(&lv, &rv, &ok) == 0 && ok == 1) found = 1;
                val_clear(&rv);
            }
        }

        int isnull = (lv.tag == T_NULL);
        val_clear(&lv);
        *out = isnull ? val_null() : val_bit(e->negated ? !found : found);
        return 0;
    }

    case EX_SUBQUERY: {
        if (!e->sub || !g_db) {
            snprintf(err, MAX_ERR, "empty subquery");
            return -1;
        }
        int save_reinforce = g_reinforce;
        g_reinforce = 0;
        const Value *save_agg = g_agg_values;
        int save_agg_count = g_agg_count;
        const Table **save_jt = g_join_tables;
        const Row **save_jr = g_join_rows;
        const char **save_ja = g_join_aliases;
        int save_jn = g_join_ntables;
        double save_score = g_score_hint;
        const Table *save_ct = g_corr_table;
        const Row *save_cr = g_corr_row;
        char save_cn[MAX_NAME];
        memcpy(save_cn, g_corr_name, MAX_NAME);
        g_agg_values = NULL;
        g_agg_count = 0;
        g_join_tables = NULL;
        g_join_rows = NULL;
        g_join_aliases = NULL;
        g_join_ntables = 0;
        g_score_hint = 0.0;
        g_corr_table = t;
        g_corr_row = row;
        if (t) snprintf(g_corr_name, MAX_NAME, "%s", t->name);
        else g_corr_name[0] = 0;

        Capture cap = {0};
        int rc = exec_select_into(g_db, e->sub, &cap, err);

        g_agg_values = save_agg;
        g_agg_count = save_agg_count;
        g_join_tables = save_jt;
        g_join_rows = save_jr;
        g_join_aliases = save_ja;
        g_join_ntables = save_jn;
        g_score_hint = save_score;
        g_corr_table = save_ct;
        g_corr_row = save_cr;
        memcpy(g_corr_name, save_cn, MAX_NAME);
        g_reinforce = save_reinforce;

        if (rc < 0) return -1;

        if (e->subq_type == SUBQ_EXISTS) {
            *out = val_bit(cap.nrows > 0);
            capture_free(&cap);
            return 0;
        }

        /* scalar subquery */
        if (cap.nrows == 0 || cap.ncols == 0)
            *out = val_null();
        else
            *out = val_copy(&cap.cells[0]);
        capture_free(&cap);
        return 0;
    }

    case EX_ANY:
    case EX_ALL: {
        if (!e->sub || !g_db) {
            snprintf(err, MAX_ERR, "empty subquery");
            return -1;
        }
        Value lv;
        if (eval_expr(e->l, row, t, now, &lv, err) < 0) return -1;
        int save_reinforce = g_reinforce;
        g_reinforce = 0;
        const Value *save_agg = g_agg_values;
        int save_agg_count = g_agg_count;
        const Table **save_jt = g_join_tables;
        const Row **save_jr = g_join_rows;
        const char **save_ja = g_join_aliases;
        int save_jn = g_join_ntables;
        double save_score = g_score_hint;
        const Table *save_ct = g_corr_table;
        const Row *save_cr = g_corr_row;
        char save_cn[MAX_NAME];
        memcpy(save_cn, g_corr_name, MAX_NAME);
        g_agg_values = NULL;
        g_agg_count = 0;
        g_join_tables = NULL;
        g_join_rows = NULL;
        g_join_aliases = NULL;
        g_join_ntables = 0;
        g_score_hint = 0.0;
        g_corr_table = t;
        g_corr_row = row;
        if (t) snprintf(g_corr_name, MAX_NAME, "%s", t->name);
        else g_corr_name[0] = 0;

        Capture cap = {0};
        int rc = exec_select_into(g_db, e->sub, &cap, err);

        g_agg_values = save_agg;
        g_agg_count = save_agg_count;
        g_join_tables = save_jt;
        g_join_rows = save_jr;
        g_join_aliases = save_ja;
        g_join_ntables = save_jn;
        g_score_hint = save_score;
        g_corr_table = save_ct;
        g_corr_row = save_cr;
        memcpy(g_corr_name, save_cn, MAX_NAME);
        g_reinforce = save_reinforce;

        if (rc < 0) { val_clear(&lv); return -1; }
        /* ALL starts true (vacuous truth), ANY starts false */
        int result = (e->kind == EX_ALL) ? 1 : 0;
        for (int i = 0; i < cap.nrows; i++) {
            int ok;
            int c = val_compare(&lv, &cap.cells[i * cap.ncols], &ok);
            if (ok != 1) continue;
            int matches = 0;
            switch (e->op) {
            case OP_EQ: matches = (c == 0); break;
            case OP_NE: matches = (c != 0); break;
            case OP_LT: matches = (c <  0); break;
            case OP_LE: matches = (c <= 0); break;
            case OP_GT: matches = (c >  0); break;
            case OP_GE: matches = (c >= 0); break;
            default: break;
            }
            if (e->kind == EX_ANY && matches) { result = 1; break; }
            if (e->kind == EX_ALL && !matches) { result = 0; break; }
        }
        *out = val_bit(result);
        capture_free(&cap);
        val_clear(&lv);
        return 0;
    }

    case EX_BIN: {
        /* the logical operators follow SQL three-valued logic: FALSE
         * dominates AND, TRUE dominates OR, and NULL propagates.  (WHERE
         * filters are unaffected — the final truthiness test collapses NULL
         * to false, which is exactly SQL's row-filtering behaviour.) */
        if (e->op == OP_AND || e->op == OP_OR) {
            Value a;
            if (eval_expr(e->l, row, t, now, &a, err) < 0) return -1;
            if (e->op == OP_AND && a.tag != T_NULL && !val_truthy(&a)) {
                *out = val_bit(0); return 0;
            }
            if (e->op == OP_OR && a.tag != T_NULL && val_truthy(&a)) {
                *out = val_bit(1); return 0;
            }
            Value b;
            if (eval_expr(e->r, row, t, now, &b, err) < 0) return -1;
            if (e->op == OP_AND) {
                if (b.tag != T_NULL && !val_truthy(&b)) {
                    val_clear(&a);
                    *out = val_bit(0);
                    return 0;
                }
                *out = (a.tag == T_NULL) ? val_null() : val_bit(1);
            } else {
                if (b.tag != T_NULL && val_truthy(&b)) {
                    val_clear(&a);
                    *out = val_bit(1);
                    return 0;
                }
                *out = (a.tag == T_NULL) ? val_null() : val_bit(0);
            }
            val_clear(&a);
            return 0;
        }

        Value a, b;
        if (eval_expr(e->l, row, t, now, &a, err) < 0) return -1;
        if (eval_expr(e->r, row, t, now, &b, err) < 0) { val_clear(&a); return -1; }

        int rc = 0;
        switch (e->op) {
        case OP_LIKE:
        case OP_NOT_LIKE: {
            if (a.tag == T_NULL || b.tag == T_NULL) { *out = val_null(); break; }
            /* match against the stored string itself; copying into a fixed
             * buffer here would silently stop matching past its end */
            char abuf[64], bbuf[64];
            const char *hay = (a.tag == T_TEXT && a.s) ? a.s : abuf;
            const char *pat = (b.tag == T_TEXT && b.s) ? b.s : bbuf;
            if (hay == abuf) val_format(&a, abuf, sizeof abuf);
            if (pat == bbuf) val_format(&b, bbuf, sizeof bbuf);
            int m = like_match_esc(hay, pat, e->esc);
            *out = val_bit(e->op == OP_LIKE ? m : !m);
            break;
        }
        case OP_MOD: {
            if (a.tag == T_NULL || b.tag == T_NULL) { *out = val_null(); break; }
            double x = (a.tag == T_FLOAT) ? a.f : (double)a.i;
            double y = (b.tag == T_FLOAT) ? b.f : (double)b.i;
            if (y == 0) { snprintf(err, MAX_ERR, "modulo by zero"); rc = -1; break; }
            if (a.tag == T_FLOAT || b.tag == T_FLOAT) *out = val_float(fmod(x, y));
            else *out = val_int(a.i % b.i);
            break;
        }
        case OP_MATCHES: {
            /* tsvector @@ tsquery */
            if (a.tag == T_NULL || b.tag == T_NULL) { *out = val_null(); break; }
            char abuf[512], bbuf[512];
            const char *tsv = (a.tag == T_TEXT && a.s) ? a.s : abuf;
            const char *tsq = (b.tag == T_TEXT && b.s) ? b.s : bbuf;
            if (tsv == abuf) val_format(&a, abuf, sizeof abuf);
            if (tsq == bbuf) val_format(&b, bbuf, sizeof bbuf);
            int m = fulltext_match(tsv, tsq, err);
            if (m < 0) { rc = -1; break; }
            *out = val_bit(m);
            break;
        }
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: {
            if (a.tag == T_NULL || b.tag == T_NULL) { *out = val_null(); break; }
            if (e->op == OP_ADD && (a.tag == T_TEXT || b.tag == T_TEXT)) {
                char abuf[512], bbuf[512], joined[1024];
                val_format(&a, abuf, sizeof abuf);
                val_format(&b, bbuf, sizeof bbuf);
                snprintf(joined, sizeof joined, "%s%s", abuf, bbuf);
                *out = val_text(joined);
                break;
            }
            int use_float = (a.tag == T_FLOAT || b.tag == T_FLOAT || e->op == OP_DIV);
            double x = (a.tag == T_FLOAT) ? a.f : (double)a.i;
            double y = (b.tag == T_FLOAT) ? b.f : (double)b.i;
            double r = 0;
            if (e->op == OP_ADD) r = x + y;
            if (e->op == OP_SUB) r = x - y;
            if (e->op == OP_MUL) r = x * y;
            if (e->op == OP_DIV) {
                if (y == 0) { snprintf(err, MAX_ERR, "division by zero"); rc = -1; break; }
                r = x / y;
            }
            *out = use_float ? val_float(r) : val_int((int64_t)r);
            break;
        }
        default: {
            int ok;
            int c = val_compare(&a, &b, &ok);
            if (ok == -1) {
                char abuf[128], bbuf[128];
                val_format(&a, abuf, sizeof abuf);
                val_format(&b, bbuf, sizeof bbuf);
                snprintf(err, MAX_ERR,
                         "cannot compare %s '%s' with %s '%s': the text is not "
                         "a number, so there is no meaningful ordering",
                         type_name(a.tag), abuf, type_name(b.tag), bbuf);
                rc = -1;
                break;
            }
            if (!ok) { *out = val_null(); break; }
            int res = 0;
            switch (e->op) {
            case OP_EQ: res = (c == 0); break;
            case OP_NE: res = (c != 0); break;
            case OP_LT: res = (c <  0); break;
            case OP_LE: res = (c <= 0); break;
            case OP_GT: res = (c >  0); break;
            case OP_GE: res = (c >= 0); break;
            default: break;
            }
            *out = val_bit(res);
            break;
        }
        }
        val_clear(&a);
        val_clear(&b);
        return rc;
    }
    }
    return 0;
}

/* Evaluate a WHERE clause against one row; NULL and false both mean no match. */
int row_matches(const Expr *where, const Row *row, const Table *t,
                       int64_t now, int *match, char *err)
{
    if (!where) { *match = 1; return 0; }
    Value v;
    if (eval_expr(where, row, t, now, &v, err) < 0) return -1;
    *match = val_truthy(&v);
    val_clear(&v);
    return 0;
}

/* Multi-table version of row_matches for joins. */
int row_matches_join(const Expr *where, const Table **tables,
                     const Row **rows, const char **aliases, int ntables,
                     int64_t now, int *match, char *err)
{
    if (!where) { *match = 1; return 0; }
    /* set up join context so qualified column refs resolve */
    const Table **sav_t = g_join_tables;
    const Row   **sav_r = g_join_rows;
    const char  **sav_a = g_join_aliases;
    int           sav_n = g_join_ntables;
    exec_set_join_ctx(tables, rows, aliases, ntables);

    /* evaluate using the first table as primary (qualified refs use ctx) */
    Value v;
    int rc = eval_expr(where, rows[0], tables[0], now, &v, err);
    if (rc == 0) *match = val_truthy(&v);
    val_clear(&v);

    exec_set_join_ctx(sav_t, sav_r, sav_a, sav_n);
    return rc;
}

/* --------------------------------------------------------- row set + sort */

typedef struct {
    Row   *rows;
    double *scores;
    int    n, cap;
} RowSet;

static void rs_init(RowSet *rs) { memset(rs, 0, sizeof *rs); }

static int rs_push(RowSet *rs, const Row *r, double score)
{
    if (rs->n == rs->cap) {
        int cap = rs->cap ? rs->cap * 2 : 64;
        Row *nr = realloc(rs->rows, sizeof(Row) * cap);
        if (!nr) return -1;
        double *ns = realloc(rs->scores, sizeof(double) * cap);
        if (!ns) { rs->rows = nr; return -1; }
        rs->rows = nr;
        rs->scores = ns;
        rs->cap = cap;
    }
    rs->rows[rs->n] = *r;
    rs->scores[rs->n] = score;
    rs->n++;
    return 0;
}

static void rs_free(RowSet *rs)
{
    for (int i = 0; i < rs->n; i++) row_clear(&rs->rows[i]);
    free(rs->rows);
    free(rs->scores);
    memset(rs, 0, sizeof *rs);
}

/* qsort context (thread-local: parallel server reads sort concurrently) */
static __thread const Table *g_sort_table;
static __thread int          g_sort_col;      /* >=0 real column, else PC_* */
static __thread int          g_sort_desc;
static __thread int64_t      g_sort_now;

static int cmp_rows(const void *x, const void *y)
{
    const Row *a = x, *b = y;
    Value av, bv;
    char dummy[MAX_ERR];
    int c = 0;

    if (g_sort_col >= 0) {
        av = (g_sort_col < a->ncols) ? a->v[g_sort_col] : val_null();
        bv = (g_sort_col < b->ncols) ? b->v[g_sort_col] : val_null();
        int ok;
        c = val_compare(&av, &bv, &ok);
        if (ok != 1) {
            /* NULLs sort first ascending; values that cannot be compared
             * numerically fall back to text order so the sort stays a total
             * order rather than aborting a query that is only displaying rows */
            int an = (av.tag == T_NULL), bn = (bv.tag == T_NULL);
            if (an || bn) {
                c = an == bn ? 0 : (an ? -1 : 1);
            } else {
                char ab[128], bb[128];
                val_format(&av, ab, sizeof ab);
                val_format(&bv, bb, sizeof bb);
                c = strcasecmp(ab, bb);
                c = c < 0 ? -1 : (c > 0 ? 1 : 0);
            }
        }
    } else {
        double x2 = 0, y2 = 0;
        switch (g_sort_col) {
        case PC_RID:      x2 = (double)a->rid; y2 = (double)b->rid; break;
        case PC_ACCESS:   x2 = a->access_count; y2 = b->access_count; break;
        case PC_LAST:     x2 = (double)a->last_access; y2 = (double)b->last_access; break;
        case PC_STRENGTH:
        default:
            x2 = row_strength_value(a, g_sort_now);
            y2 = row_strength_value(b, g_sort_now);
            break;
        }
        c = (x2 < y2) ? -1 : (x2 > y2 ? 1 : 0);
    }
    (void)dummy; (void)g_sort_table;
    return g_sort_desc ? -c : c;
}

/* ------------------------------------------------------- result printing */

void grid_init(Grid *g, int ncols)
{
    memset(g, 0, sizeof *g);
    g->ncols = ncols;
}

int grid_row(Grid *g, char **vals)
{
    if (g->nrows == g->cap) {
        int cap = g->cap ? g->cap * 2 : 32;
        char **nc = realloc(g->cells, sizeof(char *) * cap * g->ncols);
        if (!nc) return -1;
        g->cells = nc;
        g->cap = cap;
    }
    for (int i = 0; i < g->ncols; i++)
        g->cells[g->nrows * g->ncols + i] = vals[i];
    g->nrows++;
    return 0;
}

/* Column-aligned ASCII table for query results. */
void grid_print(Grid *g)
{
    int w[MAX_OUT_COLS];
    for (int c = 0; c < g->ncols; c++) {
        w[c] = (int)strlen(g->head[c]);
        for (int r = 0; r < g->nrows; r++) {
            const char *s = g->cells[r * g->ncols + c];
            int l = s ? (int)strlen(s) : 4;
            if (l > w[c]) w[c] = l;
        }
        if (w[c] > 48) w[c] = 48;
    }

    for (int c = 0; c < g->ncols; c++)
        printf("%s%-*s", c ? "  " : "", w[c], g->head[c]);
    printf("\n");
    for (int c = 0; c < g->ncols; c++) {
        if (c) printf("  ");
        for (int i = 0; i < w[c]; i++) putchar('-');
    }
    printf("\n");

    for (int r = 0; r < g->nrows; r++) {
        for (int c = 0; c < g->ncols; c++) {
            const char *s = g->cells[r * g->ncols + c];
            if (!s) s = "NULL";
            char trunc[64];
            if ((int)strlen(s) > w[c]) {
                snprintf(trunc, sizeof trunc, "%.*s...", w[c] - 3, s);
                s = trunc;
            }
            printf("%s%-*s", c ? "  " : "", w[c], s);
        }
        printf("\n");
    }
}

void grid_free(Grid *g)
{
    for (int i = 0; i < g->nrows * g->ncols; i++) free(g->cells[i]);
    free(g->cells);
    memset(g, 0, sizeof *g);
}

static char *dupf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* --------------------------------------------------- reinforcement helper */

/* Observer mode. Reading normally strengthens a row, which is the whole point,
 * but it also means you cannot inspect the memory without changing it. With
 * this off, queries leave strength and associations exactly as they were. */

void exec_set_reinforce(int on) { g_reinforce = on; }
int  exec_reinforce_enabled(void) { return g_reinforce; }

/* Boost every row in the set and wire them to each other.
 *
 * A boost of exactly zero means "associate only, do not touch": mem_touch also
 * increments the access counter, so calling it again after an explicit
 * REMEMBER would count one statement as two accesses. */
static void reinforce(DB *db, Table *t, RowSet *rs, int limit, double boost)
{
    uint64_t rids[MEM_COACT_MAX];
    int nr = 0;
    int n = (limit >= 0 && limit < rs->n) ? limit : rs->n;

    if (!g_reinforce) return;

    for (int i = 0; i < n; i++) {
        if (boost != 0.0) mem_touch(db, t, rs->rows[i].ref, boost);
        if (nr < MEM_COACT_MAX) rids[nr++] = rs->rows[i].rid;
    }
    if (nr >= 2) mem_associate(db, rids, nr, MEM_LINK_BOOST);
}


/* --------------------------------------------------------------- RECALL */

/* Split the phrase into distinct lowercase terms.
 *
 * Duplicates are dropped rather than counted twice. That is not just tidiness:
 * the term list is a fixed size, and without deduplication a phrase that
 * repeats a common word would spend the whole budget on it and silently lose
 * the words that actually mattered. */
static int tokenize_terms(const char *text, char terms[][64], int max)
{
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        char word[64];
        int k = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (k < 63) word[k++] = (char)tolower((unsigned char)*p);
            p++;
        }
        word[k] = 0;
        if (k < 2) continue;              /* ignore one-letter noise */

        int seen = 0;
        for (int i = 0; i < n && !seen; i++)
            if (strcmp(terms[i], word) == 0) seen = 1;
        if (seen) continue;

        memcpy(terms[n], word, (size_t)k + 1);
        n++;
    }
    return n;
}

static int lower_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 0;
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0) return 1;
    return 0;
}

/* Levenshtein distance between two null-terminated strings (case-sensitive). */
static int levenshtein(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    /* Use a single row of (nb+1) ints for O(min(na,nb)) memory. */
    if (na < nb) { const char *t = a; a = b; b = t; size_t n = na; na = nb; nb = n; }
    int *row = malloc(sizeof(int) * (nb + 1));
    if (!row) return (int)na; /* fallback: max distance */
    for (size_t j = 0; j <= nb; j++) row[j] = (int)j;
    for (size_t i = 1; i <= na; i++) {
        int prev = row[0];
        row[0] = (int)i;
        for (size_t j = 1; j <= nb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int ins = row[j - 1] + 1;
            int del = row[j] + 1;
            int sub = prev + cost;
            int best = ins < del ? ins : del;
            if (sub < best) best = sub;
            prev = row[j];
            row[j] = best;
        }
    }
    int dist = row[nb];
    free(row);
    return dist;
}

/* Maximum edit-distance threshold for fuzzy term matching. */
static int fuzzy_thresh(int wordlen)
{
    if (wordlen <= 4) return 0;       /* short words: exact only */
    if (wordlen <= 7) return 1;       /* 5-7 chars: 1 typo */
    return 2;                          /* 8+ chars: 2 typos */
}

/* How well does this row match the phrase, on text content alone? */
static double lexical_score(const Row *r, char terms[][64], int nterms)
{
    double hits = 0;
    for (int ti = 0; ti < nterms; ti++) {
        int term_len = (int)strlen(terms[ti]);
        int thresh = fuzzy_thresh(term_len);
        for (int c = 0; c < r->ncols; c++) {
            const Value *v = &r->v[c];
            if (v->tag == T_NULL) continue;
            /* search the stored text directly, so long values match in full */
            char buf[64];
            const char *hay = (v->tag == T_TEXT && v->s) ? v->s : buf;
            if (hay == buf) val_format(v, buf, sizeof buf);
            if (lower_contains(hay, terms[ti])) { hits += 1.0; break; }
            /* if no exact substring match, try fuzzy match against each word */
            if (thresh > 0) {
                int found = 0;
                const char *p = hay;
                while (*p && !found) {
                    while (*p && !isalnum((unsigned char)*p)) p++;
                    if (!*p) break;
                    char word[64];
                    int k = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                        if (k < 63) word[k++] = (char)tolower((unsigned char)*p);
                        p++;
                    }
                    word[k] = 0;
                    if (k > 0 && levenshtein(terms[ti], word) <= thresh) { found = 1; }
                }
                if (found) { hits += 0.8; break; }
            }
        }
    }
    return hits;
}

/* Fuzzy, association-aware search across one or all tables. */
static int exec_recall(DB *db, Stmt *s, char *err)
{
    int64_t now = mem_now();
    char terms[MAX_RECALL_TERMS][64];
    int nterms = tokenize_terms(s->recall_text, terms, MAX_RECALL_TERMS);
    int top = (s->top >= 0) ? s->top : 10;

    /* which tables to search: the named one, or all of them */
    int first = 0, last = db->cat.ntables;
    if (s->table[0]) {
        Table *t = cat_find(db, s->table);
        if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }
        first = (int)(t - db->cat.tables);
        last = first + 1;
    }

    typedef struct {
        Table  *t;
        Row     row;
        double  lex, score;
    } Hit;

    Hit *hits = NULL;
    int nhits = 0, caphits = 0;

    for (int ti = first; ti < last; ti++) {
        Table *t = &db->cat.tables[ti];
        Scan sc;
        Row r;
        scan_init(&sc, db, t);
        while (scan_next(&sc, &r)) {
            double lex = lexical_score(&r, terms, nterms);
            double strength = mem_strength_at(r.strength, r.last_access, now);
            if (lex == 0 && strength <= 0) { row_clear(&r); continue; }
            if (lex == 0) { row_clear(&r); continue; }   /* need a lexical hook */

            if (nhits == caphits) {
                int cap = caphits ? caphits * 2 : 64;
                Hit *nh = realloc(hits, sizeof(Hit) * cap);
                if (!nh) { row_clear(&r); break; }
                hits = nh;
                caphits = cap;
            }
            hits[nhits].t = t;
            hits[nhits].row = r;
            hits[nhits].lex = lex;
            /* familiarity counts: a well-worn row beats a cold exact match */
            hits[nhits].score = lex * 2.0
                              + log1p(strength) * 1.5
                              + log1p((double)r.access_count) * 0.5;
            nhits++;
        }
    }

    /* one round of spreading activation over the association graph: rows
     * linked to a strong hit get credit even if they matched weakly */
    for (int i = 0; i < nhits; i++) {
        uint64_t nb[64];
        float nw[64];
        int nn = mem_neighbors(db, hits[i].row.rid, nb, nw, 64);
        for (int k = 0; k < nn; k++) {
            for (int j = 0; j < nhits; j++) {
                if (hits[j].row.rid != nb[k]) continue;
                hits[j].score += hits[i].lex * nw[k] * MEM_SPREAD_FACTOR;
            }
        }
    }

    /* selection sort is plenty for a top-N over a small hit list */
    for (int i = 0; i < nhits && i < top; i++) {
        int best = i;
        for (int j = i + 1; j < nhits; j++)
            if (hits[j].score > hits[best].score) best = j;
        if (best != i) { Hit tmp = hits[i]; hits[i] = hits[best]; hits[best] = tmp; }
    }

    int shown = nhits < top ? nhits : top;
    if (shown == 0) {
        printf("nothing comes to mind for \"%s\"\n", s->recall_text);
        free(hits);
        return 0;
    }

    Grid g;
    grid_init(&g, 5);
    snprintf(g.head[0], MAX_NAME, "score");
    snprintf(g.head[1], MAX_NAME, "table");
    snprintf(g.head[2], MAX_NAME, "_rid");
    snprintf(g.head[3], MAX_NAME, "_strength");
    snprintf(g.head[4], MAX_NAME, "row");

    for (int i = 0; i < shown; i++) {
        Row *r = &hits[i].row;
        char rowbuf[512];
        size_t o = 0;
        rowbuf[0] = 0;
        for (int c = 0; c < r->ncols && o < sizeof rowbuf - 1; c++) {
            char cell[256];
            val_format(&r->v[c], cell, sizeof cell);
            o += (size_t)snprintf(rowbuf + o, sizeof rowbuf - o, "%s%s",
                                 c ? " | " : "", cell);
        }
        char *cells[5];
        cells[0] = dupf("%.3f", hits[i].score);
        cells[1] = strdup(hits[i].t->name);
        cells[2] = dupf("%llu", (unsigned long long)r->rid);
        cells[3] = dupf("%.3f", mem_strength_at(r->strength, r->last_access, now));
        cells[4] = strdup(rowbuf);
        grid_row(&g, cells);
    }
    grid_print(&g);
    printf("\n(%d recalled of %d considered)\n", shown, nhits);
    grid_free(&g);

    /* recalling is the strongest form of use: boost hard and wire together */
    if (g_reinforce) {
        uint64_t rids[MEM_COACT_MAX];
        int nr = 0;
        for (int i = 0; i < shown; i++) {
            mem_touch(db, hits[i].t, hits[i].row.ref, MEM_BOOST * 1.5);
            if (nr < MEM_COACT_MAX) rids[nr++] = hits[i].row.rid;
        }
        if (nr >= 2) mem_associate(db, rids, nr, MEM_LINK_BOOST);
    }

    for (int i = 0; i < nhits; i++) row_clear(&hits[i].row);
    free(hits);
    return 0;
}

/* --------------------------------------------- type and key enforcement */

/* Render a column's declared type, e.g. "BIGINT", "NVARCHAR(20)", "DATETIME". */
static const char *type_decl(const Column *c)
{
    static char buf[64];
    if (c->type == T_INT) {
        snprintf(buf, sizeof buf, "%s", int_sub_name(c->sub));
    } else if (c->type == T_TEXT) {
        if (c->is_datetime) snprintf(buf, sizeof buf, "DATETIME");
        else if (c->is_uuid) snprintf(buf, sizeof buf, "UNIQUEIDENTIFIER");
        else if (c->maxlen) snprintf(buf, sizeof buf, "NVARCHAR(%u)", c->maxlen);
        else snprintf(buf, sizeof buf, "NVARCHAR(MAX)");
    } else {
        snprintf(buf, sizeof buf, "%s", type_name(c->type));
    }
    return buf;
}

/* Fit a value into a column, or explain why it does not go.
 *
 * Every branch here used to be a silent coercion: a 21-character string into
 * NVARCHAR(5), 'yes' into a BIT, 1.5 into an INT, five billion into an INT.
 * Refusing the row is the only way the caller finds out something was wrong. */
static int coerce_to_column(const Column *c, Value *v, char *err)
{
    if (v->tag == T_NULL) return 0;      /* NOT NULL is checked separately */

    switch (c->type) {
    case T_INT: {
        int64_t iv;
        if (v->tag == T_INT) {
            iv = v->i;
        } else if (v->tag == T_BIT) {
            iv = v->i;
        } else if (v->tag == T_FLOAT) {
            if (v->f != floor(v->f)) {
                snprintf(err, MAX_ERR,
                         "cannot store %g in '%s': column is %s and the value "
                         "is not a whole number",
                         v->f, c->name, int_sub_name(c->sub));
                return -1;
            }
            if (v->f < -9.2233720368547758e18 || v->f > 9.2233720368547758e18) {
                snprintf(err, MAX_ERR, "value %g is out of range for '%s'",
                         v->f, c->name);
                return -1;
            }
            iv = (int64_t)v->f;
        } else {   /* T_TEXT */
            double d;
            int is_int = 0;
            int64_t parsed = 0;
            if (!text_to_number(v->s, &d, &is_int, &parsed)) {
                snprintf(err, MAX_ERR,
                         "cannot store '%s' in '%s': column is %s and the text "
                         "is not a number", v->s ? v->s : "", c->name,
                         int_sub_name(c->sub));
                return -1;
            }
            if (!is_int) {
                if (d != floor(d)) {
                    snprintf(err, MAX_ERR,
                             "cannot store '%s' in '%s': not a whole number",
                             v->s ? v->s : "", c->name);
                    return -1;
                }
                parsed = (int64_t)d;
            }
            iv = parsed;
        }

        int64_t lo, hi;
        int_range(c->sub, &lo, &hi);
        if (iv < lo || iv > hi) {
            snprintf(err, MAX_ERR,
                     "%lld is out of range for '%s' (%s holds %lld to %lld)",
                     (long long)iv, c->name, int_sub_name(c->sub),
                     (long long)lo, (long long)hi);
            return -1;
        }
        val_clear(v);
        *v = val_int(iv);
        return 0;
    }

    case T_FLOAT: {
        double d;
        if (v->tag == T_FLOAT) return 0;
        if (v->tag == T_INT || v->tag == T_BIT) {
            d = (double)v->i;
        } else {
            if (!text_to_number(v->s, &d, NULL, NULL)) {
                snprintf(err, MAX_ERR,
                         "cannot store '%s' in '%s': column is numeric and the "
                         "text is not a number", v->s ? v->s : "", c->name);
                return -1;
            }
        }
        val_clear(v);
        *v = val_float(d);
        return 0;
    }

    case T_BIT: {
        int64_t b;
        if (v->tag == T_INT || v->tag == T_BIT) {
            b = v->i;
        } else if (v->tag == T_FLOAT) {
            if (v->f != 0.0 && v->f != 1.0) {
                snprintf(err, MAX_ERR, "cannot store %g in BIT column '%s': "
                         "expected 0 or 1", v->f, c->name);
                return -1;
            }
            b = (int64_t)v->f;
        } else {
            const char *s = v->s ? v->s : "";
            if      (!strcasecmp(s, "1") || !strcasecmp(s, "true"))  b = 1;
            else if (!strcasecmp(s, "0") || !strcasecmp(s, "false")) b = 0;
            else {
                snprintf(err, MAX_ERR,
                         "cannot store '%s' in BIT column '%s': expected 0, 1, "
                         "true or false", s, c->name);
                return -1;
            }
        }
        if (b != 0 && b != 1) {
            snprintf(err, MAX_ERR,
                     "cannot store %lld in BIT column '%s': expected 0 or 1",
                     (long long)b, c->name);
            return -1;
        }
        val_clear(v);
        *v = val_bit((int)b);
        return 0;
    }

    case T_TEXT: {
        if (v->tag != T_TEXT) {           /* render numbers into text */
            char buf[64];
            val_format(v, buf, sizeof buf);
            val_clear(v);
            *v = val_text(buf);
        }
        if (c->is_datetime && !valid_datetime(v->s)) {
            snprintf(err, MAX_ERR,
                     "'%s' is not a valid date for '%s': expected YYYY-MM-DD, "
                     "optionally followed by HH:MM[:SS]", v->s ? v->s : "",
                     c->name);
            return -1;
        }
        if (c->is_uuid) {
            uint8_t bytes[16];
            if (!uuid_parse(v->s, bytes)) {
                snprintf(err, MAX_ERR,
                         "'%s' is not a UUID for '%s': expected 8-4-4-4-12 "
                         "hex digits", v->s ? v->s : "", c->name);
                return -1;
            }
        }
        if (c->maxlen && v->slen > c->maxlen) {
            snprintf(err, MAX_ERR,
                     "value for '%s' is %u characters; the column holds %u",
                     c->name, v->slen, c->maxlen);
            return -1;
        }
        return 0;
    }
    }
    return 0;
}

/* Find the index descriptor for a column, or -1 if none. */
int table_find_index(const Table *t, int col)
{
    for (int i = 0; i < t->nindexes; i++)
        if (t->indexes[i].valid && t->indexes[i].col == col)
            return i;
    return -1;
}

/* Ensure an index exists for the given column. Creates one if needed. */
int table_ensure_index(DB *db, Table *t, int col)
{
    int idx = table_find_index(t, col);
    if (idx >= 0) return idx;
    if (t->nindexes >= MAX_INDEXES) {
        snprintf(db->err, MAX_ERR, "too many indexes (max %d)", MAX_INDEXES);
        return -1;
    }
    idx = t->nindexes++;
    t->indexes[idx].col = col;
    btree_create(&t->indexes[idx]);
    return idx;
}

/* Insert one value into one index. A GIN index gets one posting-list entry
 * per distinct lexeme of the value; a plain btree index gets one entry. */
static int index_value_insert(DB *db, Index *ix, const Value *v, RowRef ref, char *err)
{
    if (ix->gin) {
        char vbuf[512], lex[64];
        const char *text = (v->tag == T_TEXT && v->s) ? v->s : vbuf;
        if (text == vbuf) val_format(v, vbuf, sizeof vbuf);
        FTIter it;
        fulltext_iter_begin(&it, text);
        while (fulltext_iter_next(&it, lex, sizeof lex)) {
            Value key = val_text(lex);
            int rc = btree_insert_dup(db, &ix->root, &key, ref, err);
            val_clear(&key);
            if (rc < 0) return -1;
        }
        return 0;
    }
    return btree_insert(db, &ix->root, v, ref, err);
}

/* Remove one value's entries from one index (btree: by key, GIN: the exact
 * posting-list entry matching the row reference). */
static int index_value_delete(DB *db, Index *ix, const Value *v, RowRef ref, char *err)
{
    if (ix->gin) {
        char vbuf[512], lex[64];
        const char *text = (v->tag == T_TEXT && v->s) ? v->s : vbuf;
        if (text == vbuf) val_format(v, vbuf, sizeof vbuf);
        FTIter it;
        fulltext_iter_begin(&it, text);
        while (fulltext_iter_next(&it, lex, sizeof lex)) {
            Value key = val_text(lex);
            int rc = btree_delete_ref(db, &ix->root, &key, ref, err);
#ifdef DEBUG_DELETE
            fprintf(stderr, "DBG ival_delete lex='%s' rc=%d root=%u err='%s' dberr='%s'\n",
                    lex, rc, ix->root, err, db->err);
#endif
            val_clear(&key);
            if (rc < 0) return -1;
        }
        return 0;
    }
    return btree_delete(db, &ix->root, v, err);
}

/* Insert a key+ref into every index that covers the given column. */
static int index_insert_row(DB *db, Table *t, const Row *r, char *err)
{
    for (int i = 0; i < t->nindexes; i++) {
        if (!t->indexes[i].valid) continue;
        int col = t->indexes[i].col;
        if (col < 0 || col >= r->ncols || r->v[col].tag == T_NULL) continue;
        uint32_t old_root = t->indexes[i].root;
        if (index_value_insert(db, &t->indexes[i], &r->v[col], r->ref, err) < 0)
            return -1;
        if (t->indexes[i].root != old_root)
            db_flush_catalog(db);
    }
    return 0;
}

/* Remove a row's entries from every index that covers a column. */
static int index_remove_row(DB *db, Table *t, const Row *r, char *err)
{
    for (int i = 0; i < t->nindexes; i++) {
        if (!t->indexes[i].valid) continue;
        int col = t->indexes[i].col;
        if (col < 0 || col >= r->ncols || r->v[col].tag == T_NULL) continue;
        if (index_value_delete(db, &t->indexes[i], &r->v[col], r->ref, err) < 0)
            return -1;
    }
    return 0;
}

/* Reject a row that would duplicate a PRIMARY KEY or UNIQUE value.
 *
 * Uses the B-tree index per-column for O(log n) lookups when an index exists,
 * falling back to a full scan for any column without an index. */
static int check_unique(DB *db, Table *t, const Row *cand, uint64_t exclude_rid,
                        char *err)
{
    for (int c = 0; c < t->ncols; c++) {
        if (!t->cols[c].unique) continue;
        if (c >= cand->ncols || cand->v[c].tag == T_NULL) continue;
        int idx = table_find_index(t, c);
        RowRef existing;
        if (idx >= 0) {
            int found = btree_find(db, t->indexes[idx].root, &cand->v[c], &existing, err);
            if (found == 0) {
                uint64_t found_rid = 0;
                if (exclude_rid) {
                    Row found_row;
                    if (heap_read_row(db, existing, &found_row) == 0)
                        found_rid = found_row.rid;
                    row_clear(&found_row);
                }
                if (!exclude_rid || found_rid != exclude_rid) {
                    char shown[128];
                    val_format(&cand->v[c], shown, sizeof shown);
                    snprintf(err, MAX_ERR,
                             "%s '%s' already exists in %s.%s",
                             t->cols[c].is_pk ? "primary key" : "unique value",
                             shown, t->name, t->cols[c].name);
                    return -1;
                }
            }
        } else {
            /* No index: full scan for this column */
            Scan sc;
            Row r;
            scan_init(&sc, db, t);
            while (scan_next(&sc, &r)) {
                if (r.rid == exclude_rid) { row_clear(&r); continue; }
                if (c >= r.ncols || r.v[c].tag == T_NULL) { row_clear(&r); continue; }
                int ok;
                if (val_compare(&cand->v[c], &r.v[c], &ok) == 0 && ok == 1) {
                    char shown[128];
                    val_format(&cand->v[c], shown, sizeof shown);
                    snprintf(err, MAX_ERR,
                             "%s '%s' already exists in %s.%s (row _rid %llu)",
                             t->cols[c].is_pk ? "primary key" : "unique value",
                             shown, t->name, t->cols[c].name,
                             (unsigned long long)r.rid);
                    row_clear(&r);
                    return -1;
                }
                row_clear(&r);
            }
        }
    }
    return 0;
}

/* Evaluate the table's CHECK constraint against a candidate row.  Returns 0
 * when the row is fine, -1 with `err` set when it violates the constraint.
 * A NULL result counts as satisfied (SQL three-valued logic). */
static int check_constraint(DB *db, Table *t, Row *row, char *err)
{
    if (!t->check[0]) return 0;
    (void)db;
    Expr *e = parse_expr_text(t->check, err);
    if (!e) return -1;
    Value v;
    int rc = eval_expr(e, row, t, mem_now(), &v, err);
    expr_free(e);
    if (rc < 0) return -1;
    int ok = (v.tag == T_NULL) || val_truthy(&v);
    val_clear(&v);
    if (!ok) {
        char where[MAX_ERR];
        where[0] = 0;
        if (t->ncols > 0) {
            Value first = val_copy(&row->v[0]);
            char shown[64];
            val_format(&first, shown, sizeof shown);
            val_clear(&first);
            snprintf(where, sizeof where, " (first column '%s' = %s)",
                     t->cols[0].name, shown);
        }
        snprintf(err, MAX_ERR, "CHECK constraint failed on '%s'%s: %s",
                 t->name, where, t->check);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------- foreign keys */

/* Depth cap for the recursive FK cascade walks.  A legitimate chain cannot
 * exceed the number of tables (<= MAX_TABLES), so this only trips on
 * reference cycles that the ancestor path failed to break. */
#define FK_CASCADE_MAX  256

/* One entry of the ancestor path threaded through a recursive FK cascade
 * walk.  If a walk reaches a (table, rid) it is already processing, the
 * reference graph is cyclic; re-entering it would loop forever, so it is
 * skipped (its subtree was already walked, or its row was already written). */
typedef struct {
    const Table *t;
    uint64_t rid;
} FkAncestor;

static int fk_on_path(const FkAncestor *path, int depth, const Table *t,
                      uint64_t rid)
{
    for (int i = 0; i < depth; i++)
        if (path[i].t == t && path[i].rid == rid) return 1;
    return 0;
}

/* Does the child's FK column match the parent's referenced column? */
static int fk_cols_match(const Row *prow, int pcol, const Row *crow, int ccol)
{
    if (pcol < 0 || ccol < 0 || pcol >= prow->ncols || ccol >= crow->ncols)
        return 0;
    const Value *a = &prow->v[pcol];
    const Value *b = &crow->v[ccol];
    if (a->tag == T_NULL || b->tag == T_NULL) return 0;
    int ok;
    return val_compare(a, b, &ok) == 0 && ok == 1;
}

/* Is there a row in `parent` whose column `pcol` holds value `v`?
 * Returns 1 yes, 0 no, -1 error. */
static int fk_ref_exists(DB *db, Table *parent, int pcol, const Value *v,
                         char *err)
{
    if (v->tag == T_NULL) return 1;   /* NULL is never checked */
    int idx = table_find_index(parent, pcol);
    if (idx >= 0) {
        RowRef ref;
        /* btree_find: 0 = key present, -1 = absent */
        int found = btree_find(db, parent->indexes[idx].root, v, &ref, err);
        return found == 0 ? 1 : 0;
    }
    /* no index on the referenced column: full scan */
    Scan sc;
    Row r;
    scan_init(&sc, db, parent);
    while (scan_next(&sc, &r)) {
        if (pcol < r.ncols && r.v[pcol].tag != T_NULL) {
            int ok;
            int eq = val_compare(v, &r.v[pcol], &ok);
            if (eq == 0 && ok == 1) {
                row_clear(&r);
                return 1;
            }
        }
        row_clear(&r);
    }
    return 0;
}

/* Child-side enforcement: every FK value of candidate row `r` (column-level
 * and table-level) must exist in the referenced table.  NULL values are not
 * checked, matching SQL semantics. */
static int check_fk_child(DB *db, Table *t, const Row *r, char *err)
{
    for (int c = 0; c < t->ncols; c++) {
        if (!t->cols[c].refs_table[0]) continue;
        if (c >= r->ncols || r->v[c].tag == T_NULL) continue;
        Table *p = cat_find(db, t->cols[c].refs_table);
        if (!p) {
            snprintf(err, MAX_ERR, "foreign key target table '%s' is gone",
                     t->cols[c].refs_table);
            return -1;
        }
        int pcol = table_col_index(p, t->cols[c].refs_col);
        int found = fk_ref_exists(db, p, pcol, &r->v[c], err);
        if (found < 0) return -1;
        if (!found) {
            char shown[128];
            val_format(&r->v[c], shown, sizeof shown);
            snprintf(err, MAX_ERR,
                     "foreign key violation: %s.%s = '%s' has no match in "
                     "%s.%s", t->name, t->cols[c].name, shown, p->name,
                     p->cols[pcol].name);
            return -1;
        }
    }
    for (int k = 0; k < t->nfks; k++) {
        const FK *fk = &t->fks[k];
        Table *p = cat_find(db, fk->table);
        if (!p) {
            snprintf(err, MAX_ERR, "foreign key target table '%s' is gone",
                     fk->table);
            return -1;
        }
        for (int i = 0; i < fk->ncols; i++) {
            int lc = table_col_index(t, fk->cols[i]);
            int pc = table_col_index(p, fk->refs[i]);
            if (lc < 0 || pc < 0) continue;
            if (lc >= r->ncols || r->v[lc].tag == T_NULL) continue;
            int found = fk_ref_exists(db, p, pc, &r->v[lc], err);
            if (found < 0) return -1;
            if (!found) {
                char shown[128];
                val_format(&r->v[lc], shown, sizeof shown);
                snprintf(err, MAX_ERR,
                         "foreign key violation: %s.%s = '%s' has no match in "
                         "%s.%s", t->name, fk->cols[i], shown, p->name,
                         fk->refs[i]);
                return -1;
            }
        }
    }
    return 0;
}

/* Validate a foreign key definition against the catalog at CREATE TABLE time:
 * the referenced table and columns must exist, the referenced columns must be
 * PRIMARY KEY or UNIQUE (they need an index), and the column counts must
 * agree.  The "_pk" placeholder is resolved to the parent's primary key. */
static int fk_validate(DB *db, Table *t, char *err)
{
    for (int c = 0; c < t->ncols; c++) {
        Column *cl = &t->cols[c];
        if (!cl->refs_table[0]) continue;
        Table *p = cat_find(db, cl->refs_table);
        if (!p) {
            snprintf(err, MAX_ERR, "foreign key on %s.%s references unknown "
                     "table '%s'", t->name, cl->name, cl->refs_table);
            return -1;
        }
        if (strcasecmp(cl->refs_col, "_pk") == 0) {
            int pcol = -1;
            for (int i = 0; i < p->ncols; i++)
                if (p->cols[i].is_pk) { pcol = i; break; }
            if (pcol < 0) {
                snprintf(err, MAX_ERR, "foreign key on %s.%s references "
                         "'%s' which has no PRIMARY KEY", t->name, cl->name,
                         p->name);
                return -1;
            }
            snprintf(cl->refs_col, MAX_NAME, "%s", p->cols[pcol].name);
        }
        int pcol = table_col_index(p, cl->refs_col);
        if (pcol < 0) {
            snprintf(err, MAX_ERR, "foreign key on %s.%s references unknown "
                     "column '%s.%s'", t->name, cl->name, p->name,
                     cl->refs_col);
            return -1;
        }
        if (table_find_index(p, pcol) < 0 && !p->cols[pcol].unique &&
            !p->cols[pcol].is_pk) {
            snprintf(err, MAX_ERR, "foreign key on %s.%s references '%s.%s' "
                     "which is not UNIQUE or a PRIMARY KEY", t->name, cl->name,
                     p->name, p->cols[pcol].name);
            return -1;
        }
    }
    for (int k = 0; k < t->nfks; k++) {
        FK *fk = &t->fks[k];
        Table *p = cat_find(db, fk->table);
        if (!p) {
            snprintf(err, MAX_ERR, "foreign key on %s references unknown "
                     "table '%s'", t->name, fk->table);
            return -1;
        }
        for (int i = 0; i < fk->ncols; i++) {
            int lc = table_col_index(t, fk->cols[i]);
            if (lc < 0) {
                snprintf(err, MAX_ERR, "foreign key on %s references unknown "
                         "column '%s'", t->name, fk->cols[i]);
                return -1;
            }
            if (strcasecmp(fk->refs[i], "_pk") == 0) {
                int pcol = -1;
                for (int j = 0; j < p->ncols; j++)
                    if (p->cols[j].is_pk) { pcol = j; break; }
                if (pcol < 0) {
                    snprintf(err, MAX_ERR, "foreign key on %s references "
                             "'%s' which has no PRIMARY KEY", t->name, p->name);
                    return -1;
                }
                snprintf(fk->refs[i], MAX_NAME, "%s", p->cols[pcol].name);
            }
            int pcol = table_col_index(p, fk->refs[i]);
            if (pcol < 0) {
                snprintf(err, MAX_ERR, "foreign key on %s references unknown "
                         "column '%s.%s'", t->name, p->name, fk->refs[i]);
                return -1;
            }
            if (table_find_index(p, pcol) < 0 && !p->cols[pcol].unique &&
                !p->cols[pcol].is_pk) {
                snprintf(err, MAX_ERR, "foreign key on %s references '%s.%s' "
                         "which is not UNIQUE or a PRIMARY KEY", t->name,
                         p->name, p->cols[pcol].name);
                return -1;
            }
        }
    }
    return 0;
}

/* Is table `t` referenced as a foreign key target by any other table? */
static int fk_is_referenced(const DB *db, const Table *t)
{
    for (int ti = 0; ti < db->cat.ntables; ti++) {
        const Table *other = &db->cat.tables[ti];
        if (other == t) continue;
        for (int c = 0; c < other->ncols; c++)
            if (other->cols[c].refs_table[0] &&
                strcasecmp(other->cols[c].refs_table, t->name) == 0)
                return 1;
        for (int k = 0; k < other->nfks; k++)
            if (strcasecmp(other->fks[k].table, t->name) == 0)
                return 1;
    }
    return 0;
}

/* Remove one row and its index entries; returns 1 deleted, 0 already gone. */
static int row_delete(DB *db, Table *t, Row *row, char *err)
{
    if (heap_delete(db, t, row->ref) != 0) return 0;
    mem_forget_row(db, row->rid);
    if (index_remove_row(db, t, row, err) < 0) return -1;
    return 1;
}

/* Forward declaration (mutually recursive with fk_parent_delete). */
static int fk_parent_delete(DB *db, Table *t, const Row *del, char *err,
                            int check_only, FkAncestor *path, int depth);

/* Delete one row and, through FK ON DELETE CASCADE, everything that (directly
 * or transitively) references it.  The row is deleted first, then the
 * referencing rows are walked by table scan, so in a reference cycle every
 * row disappears from the scan as soon as it is deleted and the walk
 * terminates; the ancestor path provides the same guarantee for the
 * non-mutating check pass.  With check_only, nothing is deleted: the walk
 * reports the first NO ACTION refusal in the whole cascade closure, so a
 * multi-row DELETE can verify everything before mutating anything.  Returns
 * 1 if the row was deleted, 0 if it was already gone, -1 on error. */
static int fk_delete_recursive(DB *db, Table *t, Row *r, char *err,
                               int check_only, FkAncestor *path, int depth)
{
    if (fk_on_path(path, depth, t, r->rid)) return 0;
    if (depth >= FK_CASCADE_MAX) {
        snprintf(err, MAX_ERR, "foreign key cascade is too deep or cyclic");
        return -1;
    }
    path[depth].t = t;
    path[depth].rid = r->rid;

    int del = 0;
    if (!check_only) {
        del = row_delete(db, t, r, err);
        if (del < 0) return -1;
    }
    if (fk_parent_delete(db, t, r, err, check_only, path, depth) < 0)
        return -1;
    return del;
}

/* Parent-side enforcement for DELETE: NO ACTION refuses to remove a row that
 * children reference; CASCADE removes the referencing rows first, recursing
 * through grandchildren.  With check_only, NO ACTION refusals are reported
 * but no cascade deletions happen, so a multi-row DELETE can verify every
 * row before mutating anything. */
static int fk_parent_delete(DB *db, Table *t, const Row *del, char *err,
                            int check_only, FkAncestor *path, int depth)
{
    for (int ti = 0; ti < db->cat.ntables; ti++) {
        Table *child = &db->cat.tables[ti];
        for (int c = 0; c < child->ncols; c++) {
            if (!child->cols[c].refs_table[0]) continue;
            if (strcasecmp(child->cols[c].refs_table, t->name) != 0) continue;
            int pcol = table_col_index(t, child->cols[c].refs_col);
            if (pcol < 0) continue;
            int cascade = (child->cols[c].on_delete == FK_CASCADE);
            Scan sc;
            Row r;
            scan_init(&sc, db, child);
            while (scan_next(&sc, &r)) {
                if (!fk_cols_match(del, pcol, &r, c)) { row_clear(&r); continue; }
                if (cascade) {
                    if (fk_delete_recursive(db, child, &r, err, check_only,
                                            path, depth) < 0) {
                        row_clear(&r);
                        return -1;
                    }
                } else {
                    char shown[128];
                    val_format(&del->v[pcol], shown, sizeof shown);
                    snprintf(err, MAX_ERR,
                             "cannot delete %s.%s = '%s': row %llu of '%s' "
                             "still references it",
                             t->name, t->cols[pcol].name, shown,
                             (unsigned long long)r.rid, child->name);
                    row_clear(&r);
                    return -1;
                }
                row_clear(&r);
            }
        }
        for (int k = 0; k < child->nfks; k++) {
            const FK *fk = &child->fks[k];
            if (strcasecmp(fk->table, t->name) != 0) continue;
            Scan sc;
            Row r;
            scan_init(&sc, db, child);
            while (scan_next(&sc, &r)) {
                int all = 1;
                for (int i = 0; i < fk->ncols && all; i++) {
                    int pcol = table_col_index(t, fk->refs[i]);
                    int ccol = table_col_index(child, fk->cols[i]);
                    if (pcol < 0 || ccol < 0 ||
                        !fk_cols_match(del, pcol, &r, ccol))
                        all = 0;
                }
                if (!all) { row_clear(&r); continue; }
                if (fk->on_delete == FK_CASCADE) {
                    if (fk_delete_recursive(db, child, &r, err, check_only,
                                            path, depth) < 0) {
                        row_clear(&r);
                        return -1;
                    }
                } else {
                    int pcol = table_col_index(t, fk->refs[0]);
                    char shown[128];
                    val_format(&del->v[pcol >= 0 ? pcol : 0], shown,
                               sizeof shown);
                    snprintf(err, MAX_ERR,
                             "cannot delete %s.%s = '%s': row %llu of '%s' "
                             "still references it",
                             t->name,
                             pcol >= 0 ? t->cols[pcol].name : "?",
                             shown,
                             (unsigned long long)r.rid, child->name);
                    row_clear(&r);
                    return -1;
                }
                row_clear(&r);
            }
        }
    }
    return 0;
}

/* Parent-side enforcement for UPDATE.  The statement evaluates the new row
 * (`newrow`) against the old one (`oldrow`) and calls this twice per row:
 * first with apply = 0, before the parent row is replaced, to refuse every
 * ON UPDATE NO ACTION policy in the whole cascade closure; then with
 * apply = 1, after the parent row and its indexes are updated, to rewrite
 * the CASCADE children (recursively through grandchildren).  Either pass
 * failing aborts the statement. */
static int fk_update_walk(DB *db, Table *t, const Row *oldrow,
                          const Row *newrow, char *err, int apply,
                          FkAncestor *path, int depth);

/* For one FK (a column-level FK with n = 1, or a table-level FK), walk every
 * row of `child` whose referencing columns (ccols[]) match the parent's OLD
 * key values (pcols[]).  With apply, ON UPDATE CASCADE rewrites the child's
 * referencing columns to the parent's new values (validating the child like
 * a statement would, then recursing so grandchildren follow); NO ACTION rows
 * are refused.  Without apply nothing is written: NO ACTION rows are refused
 * and CASCADE subtrees are walked with the prospective child row, so a
 * multi-row UPDATE verifies the whole cascade closure before touching
 * anything. */
static int fk_update_fk_rows(DB *db, Table *child, Table *t, int n,
                             const int *pcols, const int *ccols, int policy,
                             const Row *oldrow, const Row *newrow,
                             char *err, int apply, FkAncestor *path, int depth)
{
    Scan sc;
    Row r;
    scan_init(&sc, db, child);
    while (scan_next(&sc, &r)) {
        int match = 1;
        for (int i = 0; i < n && match; i++)
            if (!fk_cols_match(oldrow, pcols[i], &r, ccols[i])) match = 0;
        if (!match) { row_clear(&r); continue; }
        if (policy != FK_CASCADE) {
            char shown[128];
            val_format(&oldrow->v[pcols[0]], shown, sizeof shown);
            snprintf(err, MAX_ERR,
                     "cannot change %s.%s from '%s': row %llu of '%s' "
                     "still references it",
                     t->name, t->cols[pcols[0]].name, shown,
                     (unsigned long long)r.rid, child->name);
            row_clear(&r);
            return -1;
        }
        if (fk_on_path(path, depth, child, r.rid)) { row_clear(&r); continue; }
        path[depth].t = child;
        path[depth].rid = r.rid;

        /* the child's prospective row: referencing columns take the new
         * parent values, everything else stays as it is */
        Row cr;
        memset(&cr, 0, sizeof cr);
        cr.ref = r.ref;
        cr.rid = r.rid;
        cr.access_count = r.access_count;
        cr.last_access = r.last_access;
        cr.strength = r.strength;
        cr.ncols = r.ncols;
        for (int c = 0; c < r.ncols; c++) cr.v[c] = val_copy(&r.v[c]);
        for (int i = 0; i < n; i++) {
            val_clear(&cr.v[ccols[i]]);
            cr.v[ccols[i]] = val_copy(&newrow->v[pcols[i]]);
        }
        if (apply) {
            if (check_unique(db, child, &cr, r.rid, err) < 0 ||
                check_constraint(db, child, &cr, err) < 0 ||
                check_fk_child(db, child, &cr, err) < 0) {
                row_clear(&cr);
                row_clear(&r);
                return -1;
            }
            if (heap_replace(db, child, r.ref, &cr) < 0) {
                snprintf(err, MAX_ERR, "%s", db->err);
                row_clear(&cr);
                row_clear(&r);
                return -1;
            }
            for (int ki = 0; ki < child->nindexes; ki++) {
                if (!child->indexes[ki].valid) continue;
                int col = child->indexes[ki].col;
                if (col < 0) continue;
                uint32_t old_root = child->indexes[ki].root;
                if (r.v[col].tag != T_NULL)
                    index_value_delete(db, &child->indexes[ki], &r.v[col],
                                       r.ref, err);
                if (cr.v[col].tag != T_NULL)
                    index_value_insert(db, &child->indexes[ki], &cr.v[col],
                                       cr.ref, err);
                if (child->indexes[ki].root != old_root)
                    db_flush_catalog(db);
            }
        }
        int rc = fk_update_walk(db, child, &r, &cr, err, apply, path,
                                depth + 1);
        row_clear(&cr);
        row_clear(&r);
        if (rc < 0) return -1;
    }
    return 0;
}

static int fk_update_walk(DB *db, Table *t, const Row *oldrow,
                          const Row *newrow, char *err, int apply,
                          FkAncestor *path, int depth)
{
    if (depth >= FK_CASCADE_MAX) {
        snprintf(err, MAX_ERR,
                 "foreign key ON UPDATE cascade is too deep or cyclic");
        return -1;
    }
    for (int ti = 0; ti < db->cat.ntables; ti++) {
        Table *child = &db->cat.tables[ti];
        for (int c = 0; c < child->ncols; c++) {
            Column *cl = &child->cols[c];
            if (!cl->refs_table[0]) continue;
            if (strcasecmp(cl->refs_table, t->name) != 0) continue;
            int pcol = table_col_index(t, cl->refs_col);
            if (pcol < 0 || pcol >= t->ncols) continue;
            if (fk_cols_match(oldrow, pcol, newrow, pcol)) continue;
            if (fk_update_fk_rows(db, child, t, 1, &pcol, &c, cl->on_update,
                                  oldrow, newrow, err, apply, path, depth) < 0)
                return -1;
        }
        for (int k = 0; k < child->nfks; k++) {
            const FK *fk = &child->fks[k];
            if (strcasecmp(fk->table, t->name) != 0) continue;
            int pcols[MAX_FK_COLS];
            int ccols[MAX_FK_COLS];
            int n = 0;
            int changed = 0;
            for (int i = 0; i < fk->ncols; i++) {
                int pc = table_col_index(t, fk->refs[i]);
                int cc = table_col_index(child, fk->cols[i]);
                if (pc < 0 || cc < 0) continue;
                pcols[n] = pc;
                ccols[n] = cc;
                n++;
                if (pc >= t->ncols ||
                    !fk_cols_match(oldrow, pc, newrow, pc))
                    changed = 1;
            }
            if (n == 0 || !changed) continue;
            if (fk_update_fk_rows(db, child, t, n, pcols, ccols,
                                  fk->on_update, oldrow, newrow, err,
                                  apply, path, depth) < 0)
                return -1;
        }
    }
    return 0;
}

/* Fill a column the statement did not mention: IDENTITY counter, DEFAULT
 * value, or NULL. Returns 1 if a value was produced. */
static int apply_default(DB *db, Table *t, Column *c, Value *out, char *err)
{
    *out = val_null();

    if (c->identity) {
        int64_t v = c->id_next;
        int64_t lo, hi;
        int_range(c->sub, &lo, &hi);
        if (v < lo || v > hi) {
            snprintf(err, MAX_ERR,
                     "the IDENTITY counter for '%s' has run past the range of %s",
                     c->name, int_sub_name(c->sub));
            return -1;
        }
        c->id_next += c->id_step;
        *out = val_int(v);
        (void)t;
        db_flush_catalog(db);        /* the counter has to survive a restart */
        return 1;
    }

    switch (c->dflt) {
    case DFLT_NONE:
        return 0;
    case DFLT_GETDATE: {
        Value none[1];
        return func_call("GETDATE", none, 0, out, err) < 0 ? -1 : 1;
    }
    case DFLT_NEWID: {
        Value none[1];
        return func_call("NEWID", none, 0, out, err) < 0 ? -1 : 1;
    }
    case DFLT_LITERAL: {
        /* stored as text; let the normal coercion decide whether it fits */
        *out = val_text(c->dflt_text);
        return 1;
    }
    }
    return 0;
}

/* ------------------------------------------------- INSERT/UPDATE/DELETE */

static int exec_insert(DB *db, Stmt *s, char *err)
{
    Table *t = cat_find(db, s->table);
    if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }

    /* map the statement's column order onto the table's column order */
    int map[MAX_COLS];
    int nmap;
    if (s->n_ins_cols) {
        nmap = s->n_ins_cols;
        for (int i = 0; i < nmap; i++) {
            map[i] = table_col_index(t, s->ins_cols[i]);
            if (map[i] < 0) {
                snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                         s->ins_cols[i], t->name);
                return -1;
            }
        }
    } else {
        nmap = t->ncols;
        for (int i = 0; i < nmap; i++) map[i] = i;
    }

    Row empty;
    memset(&empty, 0, sizeof empty);
    int inserted = 0;

    for (int i = 0; i < s->nrows; i++) {
        if (s->row_width[i] != nmap) {
            snprintf(err, MAX_ERR,
                     "row %d has %d value%s but %d column%s expected",
                     i + 1, s->row_width[i], s->row_width[i] == 1 ? "" : "s",
                     nmap, nmap == 1 ? "" : "s");
            return -1;
        }
        Row r;
        memset(&r, 0, sizeof r);
        r.ncols = t->ncols;
        for (int c = 0; c < t->ncols; c++) r.v[c] = val_null();

        for (int j = 0; j < nmap; j++) {
            Value v;
            if (eval_expr(s->rows[i][j], &empty, t, mem_now(), &v, err) < 0) {
                row_clear(&r);
                return -1;
            }
            if (coerce_to_column(&t->cols[map[j]], &v, err) < 0) {
                val_clear(&v);
                row_clear(&r);
                return -1;
            }
            val_clear(&r.v[map[j]]);
            r.v[map[j]] = v;
        }

        /* columns the statement did not mention: IDENTITY, then DEFAULT */
        for (int c = 0; c < t->ncols; c++) {
            int named = 0;
            for (int j = 0; j < nmap; j++) if (map[j] == c) named = 1;
            if (named) continue;

            Value dv;
            int got = apply_default(db, t, &t->cols[c], &dv, err);
            if (got < 0) { row_clear(&r); return -1; }
            if (!got) continue;
            if (coerce_to_column(&t->cols[c], &dv, err) < 0) {
                val_clear(&dv);
                row_clear(&r);
                return -1;
            }
            val_clear(&r.v[c]);
            r.v[c] = dv;
        }

        /* NOT NULL applies to every column, including ones the statement never
         * mentioned - those default to NULL and must still be checked. */
        for (int c = 0; c < t->ncols; c++) {
            if (t->cols[c].not_null && r.v[c].tag == T_NULL) {
                snprintf(err, MAX_ERR, "column '%s' does not accept NULL",
                         t->cols[c].name);
                row_clear(&r);
                return -1;
            }
        }

        if (check_unique(db, t, &r, 0, err) < 0) {
            row_clear(&r);
            return -1;
        }

        if (check_constraint(db, t, &r, err) < 0) {
            row_clear(&r);
            return -1;
        }

        if (check_fk_child(db, t, &r, err) < 0) {
            row_clear(&r);
            return -1;
        }

        r.strength = (float)MEM_INIT_STRENGTH;
        r.last_access = mem_now();
        r.access_count = 0;
        if (heap_insert(db, t, &r) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            row_clear(&r);
            return -1;
        }

        if (index_insert_row(db, t, &r, err) < 0) {
            row_clear(&r);
            return -1;
        }

        row_clear(&r);
        inserted++;
    }

    printf("(%d row%s inserted)\n", inserted, inserted == 1 ? "" : "s");
    return 0;
}

/* INSERT INTO t (cols) SELECT ... */
static int exec_insert_select(DB *db, Stmt *s, char *err)
{
    Table *t = cat_find(db, s->table);
    if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }

    Capture cap;
    memset(&cap, 0, sizeof cap);
    if (s->sub->kind == ST_CALL) {
        /* INSERT INTO t EXEC proc: the procedure's first result set becomes
         * the captured rows (SELECTs inside the body hand them over via
         * g_pending_capture instead of printing) */
        g_pending_capture = &cap;
        int rc = exec_stmt(db, s->sub, err);
        g_pending_capture = NULL;
        if (rc < 0) {
            capture_free(&cap);
            return -1;
        }
    } else if (exec_select_into(db, s->sub, &cap, err) < 0) {
        capture_free(&cap);
        return -1;
    }

    int map[MAX_COLS], nmap;
    if (s->n_ins_cols) {
        nmap = s->n_ins_cols;
        for (int i = 0; i < nmap; i++) {
            map[i] = table_col_index(t, s->ins_cols[i]);
            if (map[i] < 0) {
                snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                         s->ins_cols[i], t->name);
                capture_free(&cap);
                return -1;
            }
        }
    } else {
        nmap = t->ncols;
        for (int i = 0; i < nmap; i++) map[i] = i;
    }

    if (cap.nrows && cap.ncols != nmap) {
        snprintf(err, MAX_ERR,
                 "the SELECT returns %d column%s but %d column%s expected",
                 cap.ncols, cap.ncols == 1 ? "" : "s", nmap, nmap == 1 ? "" : "s");
        capture_free(&cap);
        return -1;
    }

    int inserted = 0;
    for (int i = 0; i < cap.nrows; i++) {
        Row r;
        memset(&r, 0, sizeof r);
        r.ncols = t->ncols;
        for (int c = 0; c < t->ncols; c++) r.v[c] = val_null();

        int bad = 0;
        for (int j = 0; j < nmap && !bad; j++) {
            Value v = val_copy(&cap.cells[i * cap.ncols + j]);
            if (coerce_to_column(&t->cols[map[j]], &v, err) < 0) {
                val_clear(&v);
                bad = 1;
                break;
            }
            val_clear(&r.v[map[j]]);
            r.v[map[j]] = v;
        }
        if (bad) { row_clear(&r); capture_free(&cap); return -1; }

        for (int c = 0; c < t->ncols; c++) {
            int named = 0;
            for (int j = 0; j < nmap; j++) if (map[j] == c) named = 1;
            if (named) continue;
            Value dv;
            int got = apply_default(db, t, &t->cols[c], &dv, err);
            if (got < 0) { row_clear(&r); capture_free(&cap); return -1; }
            if (!got) continue;
            if (coerce_to_column(&t->cols[c], &dv, err) < 0) {
                val_clear(&dv); row_clear(&r); capture_free(&cap); return -1;
            }
            val_clear(&r.v[c]);
            r.v[c] = dv;
        }

        for (int c = 0; c < t->ncols; c++) {
            if (t->cols[c].not_null && r.v[c].tag == T_NULL) {
                snprintf(err, MAX_ERR, "column '%s' does not accept NULL",
                         t->cols[c].name);
                row_clear(&r);
                capture_free(&cap);
                return -1;
            }
        }
        if (check_unique(db, t, &r, 0, err) < 0) {
            row_clear(&r); capture_free(&cap); return -1;
        }
        if (check_constraint(db, t, &r, err) < 0) {
            row_clear(&r); capture_free(&cap); return -1;
        }
        if (check_fk_child(db, t, &r, err) < 0) {
            row_clear(&r); capture_free(&cap); return -1;
        }

        r.strength = (float)MEM_INIT_STRENGTH;
        r.last_access = mem_now();
        if (heap_insert(db, t, &r) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            row_clear(&r); capture_free(&cap); return -1;
        }

        if (index_insert_row(db, t, &r, err) < 0) {
            row_clear(&r); capture_free(&cap); return -1;
        }

        row_clear(&r);
        inserted++;
    }

    capture_free(&cap);
    printf("(%d row%s inserted)\n", inserted, inserted == 1 ? "" : "s");
    return 0;
}

/* Remove every row, without touching the schema. */
static int exec_truncate(DB *db, Stmt *s, char *err)
{
    Table *t = cat_find(db, s->table);
    if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }

    /* Collect RIDs for mem_forget before freeing pages. */
    uint64_t *rids = NULL;
    int n = 0, cap = 0;
    Scan sc;
    Row r;
    scan_init(&sc, db, t);
    while (scan_next(&sc, &r)) {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            uint64_t *ni = realloc(rids, sizeof(uint64_t) * (size_t)cap);
            if (!ni) { free(rids); row_clear(&r); snprintf(err, MAX_ERR, "out of memory"); return -1; }
            rids = ni;
        }
        rids[n++] = r.rid;
        row_clear(&r);
    }

    /* Destroy B-tree indexes. */
    for (int i = 0; i < t->nindexes; i++) {
        if (t->indexes[i].root) {
            btree_destroy(db, t->indexes[i].root);
            t->indexes[i].root = 0;
            t->indexes[i].valid = 0;
        }
    }
    t->nindexes = 0;

    /* Free all heap pages. */
    heap_free_pages(db, t);

    for (int i = 0; i < n; i++) mem_forget_row(db, rids[i]);
    free(rids);
    printf("(%d row%s removed)\n", n, n == 1 ? "" : "s");
    return db_flush_catalog(db);
}

/* ALTER TABLE ... ADD / DROP COLUMN.
 *
 * Rows store values positionally, so both operations rewrite every row. That is
 * slow but it keeps the on-disk layout honest: no hidden version-per-row. */
static int exec_alter(DB *db, Stmt *s, char *err)
{
    Table *t = cat_find(db, s->table);
    if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }

    int adding = (s->kind == ST_ALTER_ADD);

    /* RENAME COLUMN — metadata only, no data rewrite */
    if (s->kind == ST_ALTER_RENAME) {
        int idx = table_col_index(t, s->cols[0].name);
        if (idx < 0) {
            snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                     s->cols[0].name, t->name);
            return -1;
        }
        if (table_col_index(t, s->cols[1].name) >= 0) {
            snprintf(err, MAX_ERR, "column '%s' already exists in '%s'",
                     s->cols[1].name, t->name);
            return -1;
        }
        if (s->cols[1].name[0] == '_') {
            snprintf(err, MAX_ERR, "column names beginning with '_' are reserved");
            return -1;
        }
        snprintf(t->cols[idx].name, MAX_NAME, "%s", s->cols[1].name);
        printf("table '%s': column '%s' renamed to '%s'\n",
               t->name, s->cols[0].name, s->cols[1].name);
        return db_flush_catalog(db);
    }

    /* ALTER COLUMN TYPE */
    if (s->kind == ST_ALTER_TYPE) {
        int idx = table_col_index(t, s->cols[0].name);
        if (idx < 0) {
            snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                     s->cols[0].name, t->name);
            return -1;
        }
        Column *c = &t->cols[idx];

        /* The target definition, with the same normalization the catalog
         * change below applies (DATETIME only survives on TEXT columns). */
        Column newc = s->cols[0];
        if (newc.type != T_TEXT) {
            newc.is_datetime = 0;
            newc.is_uuid = 0;
        }

        /* Validate every stored value against the new type before touching
         * anything.  Narrowing conversions (BIGINT→INT, NVARCHAR(MAX)→
         * NVARCHAR(20), TEXT→DATETIME, FLOAT→INT, ...) must fail loudly
         * instead of silently corrupting the rows. */
        Scan sc;
        Row r;
        scan_init(&sc, db, t);
        while (scan_next(&sc, &r)) {
            if (idx < r.ncols && r.v[idx].tag != T_NULL) {
                Value v = val_copy(&r.v[idx]);
                char verr[MAX_ERR];
                if (coerce_to_column(&newc, &v, verr) < 0) {
                    char orig[64];
                    val_format(&r.v[idx], orig, sizeof orig);
                    snprintf(err, MAX_ERR,
                             "cannot change '%s' to %s: row %lld has value %s "
                             "(%s)", c->name, type_decl(&newc),
                             (long long)r.rid, orig, verr);
                    val_clear(&v);
                    row_clear(&r);
                    return -1;
                }
                val_clear(&v);
            }
            row_clear(&r);
        }

        /* The type is changing: an index keyed on this column holds values
         * encoded with the old type, so it must be rebuilt. */
        int rebuild_index = -1;
        for (int ki = 0; ki < t->nindexes; ki++) {
            if (t->indexes[ki].valid && t->indexes[ki].col == idx)
                rebuild_index = ki;
        }
        if (rebuild_index >= 0) {
            if (t->indexes[rebuild_index].root)
                btree_destroy(db, t->indexes[rebuild_index].root);
            t->indexes[rebuild_index].root = 0;
            t->indexes[rebuild_index].valid = 0;
        }

        c->type   = newc.type;
        c->sub    = newc.sub;
        c->maxlen = newc.maxlen;
        c->is_datetime = newc.is_datetime;
        c->is_uuid     = newc.is_uuid;
        if (db_flush_catalog(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }

        /* Re-insert every row's key with its new type */
        if (rebuild_index >= 0) {
            scan_init(&sc, db, t);
            while (scan_next(&sc, &r)) {
                if (idx < r.ncols && r.v[idx].tag != T_NULL) {
                    Value v = val_copy(&r.v[idx]);
                    if (coerce_to_column(c, &v, err) < 0) {
                        val_clear(&v); row_clear(&r);
                        return -1;
                    }
                    if (btree_insert(db, &t->indexes[rebuild_index].root,
                                     &v, r.ref, err) < 0) {
                        val_clear(&v); row_clear(&r);
                        return -1;
                    }
                    val_clear(&v);
                }
                row_clear(&r);
            }
        }

        printf("table '%s': column '%s' type changed\n", t->name, s->cols[0].name);
        return db_flush_catalog(db);
    }

    int dropidx = -1;

    if (adding) {
        if (t->ncols >= MAX_COLS) {
            snprintf(err, MAX_ERR, "table '%s' already has the maximum %d columns",
                     t->name, MAX_COLS);
            return -1;
        }
        if (table_col_index(t, s->cols[0].name) >= 0) {
            snprintf(err, MAX_ERR, "column '%s' already exists in '%s'",
                     s->cols[0].name, t->name);
            return -1;
        }
        if (s->cols[0].not_null && s->cols[0].dflt == DFLT_NONE && t->nrows > 0) {
            snprintf(err, MAX_ERR,
                     "cannot add NOT NULL column '%s' to a table with %lld rows "
                     "without a DEFAULT: the existing rows have no value for it",
                     s->cols[0].name, (long long)t->nrows);
            return -1;
        }
    } else {
        dropidx = table_col_index(t, s->cols[0].name);
        if (dropidx < 0) {
            snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                     s->cols[0].name, t->name);
            return -1;
        }
        if (t->ncols == 1) {
            snprintf(err, MAX_ERR,
                     "cannot drop '%s': a table needs at least one column",
                     s->cols[0].name);
            return -1;
        }
    }

    /* collect every row first, then rewrite */
    RowSet rs;
    rs_init(&rs);
    Scan sc;
    Row r;
    scan_init(&sc, db, t);
    while (scan_next(&sc, &r)) {
        if (rs_push(&rs, &r, 0) < 0) {
            row_clear(&r); rs_free(&rs);
            snprintf(err, MAX_ERR, "out of memory");
            return -1;
        }
    }

    /* update the schema */
    Column added;
    if (adding) {
        added = s->cols[0];
        t->cols[t->ncols++] = added;
    } else {
        for (int c = dropidx; c < t->ncols - 1; c++) t->cols[c] = t->cols[c + 1];
        t->ncols--;
    }

    /* add/drop index for the column */
    if (adding && added.unique) {
        int idx = table_ensure_index(db, t, t->ncols - 1);
        if (idx < 0) { rs_free(&rs); return -1; }
    } else if (!adding) {
        /* destroy index on the dropped column if one exists */
        for (int ki = 0; ki < t->nindexes; ki++) {
            if (t->indexes[ki].col == dropidx) {
                if (t->indexes[ki].root) btree_destroy(db, t->indexes[ki].root);
                /* shift remaining indexes down */
                for (int kk = ki; kk < t->nindexes - 1; kk++)
                    t->indexes[kk] = t->indexes[kk + 1];
                t->indexes[t->nindexes - 1].root = 0;
                t->indexes[t->nindexes - 1].col = -1;
                t->indexes[t->nindexes - 1].valid = 0;
                t->nindexes--;
                break;
            }
        }
    }

    if (db_flush_catalog(db) < 0) {
        rs_free(&rs);
        snprintf(err, MAX_ERR, "%s", db->err);
        return -1;
    }

    for (int i = 0; i < rs.n; i++) {
        Row *row = &rs.rows[i];
        Row out;
        memset(&out, 0, sizeof out);
        out.rid = row->rid;
        out.access_count = row->access_count;
        out.last_access = row->last_access;
        out.strength = row->strength;
        out.ncols = t->ncols;

        if (adding) {
            for (int c = 0; c < t->ncols - 1; c++)
                out.v[c] = (c < row->ncols) ? val_copy(&row->v[c]) : val_null();
            Value dv;
            int got = apply_default(db, t, &t->cols[t->ncols - 1], &dv, err);
            if (got < 0) { row_clear(&out); rs_free(&rs); return -1; }
            if (got && coerce_to_column(&t->cols[t->ncols - 1], &dv, err) < 0) {
                val_clear(&dv); row_clear(&out); rs_free(&rs); return -1;
            }
            out.v[t->ncols - 1] = got ? dv : val_null();
        } else {
            int o = 0;
            for (int c = 0; c < row->ncols; c++) {
                if (c == dropidx) continue;
                if (o < t->ncols) out.v[o++] = val_copy(&row->v[c]);
            }
            while (o < t->ncols) out.v[o++] = val_null();
        }

        RowRef old_ref = row->ref;
        if (heap_replace(db, t, old_ref, &out) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            row_clear(&out);
            rs_free(&rs);
            return -1;
        }

        /* update all indexes: delete old ref, insert new ref */
        for (int ki = 0; ki < t->nindexes; ki++) {
            if (!t->indexes[ki].valid) continue;
            int col = t->indexes[ki].col;
            if (col < 0) continue;
            uint32_t old_root = t->indexes[ki].root;
            if (col < row->ncols && row->v[col].tag != T_NULL)
                index_value_delete(db, &t->indexes[ki], &row->v[col], row->ref, err);
            if (col < out.ncols && out.v[col].tag != T_NULL) {
                if (index_value_insert(db, &t->indexes[ki], &out.v[col],
                                       out.ref, err) < 0) {
                    row_clear(&out); rs_free(&rs); return -1;
                }
            }
            if (t->indexes[ki].root != old_root)
                db_flush_catalog(db);
        }
        row_clear(&out);
    }
    rs_free(&rs);

    printf("table '%s': column '%s' %s\n", t->name, s->cols[0].name,
           adding ? "added" : "dropped");
    return db_flush_catalog(db);
}

static int exec_update(DB *db, Stmt *s, char *err)
{
    Table *t = cat_find(db, s->table);
    if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }
    int64_t now = mem_now();

    for (int i = 0; i < s->nsets; i++) {
        if (pseudo_col(s->sets[i].col) != -1) {
            snprintf(err, MAX_ERR,
                     "'%s' is maintained by the memory layer and cannot be "
                     "assigned; use REMEMBER or FORGET instead", s->sets[i].col);
            return -1;
        }
        if (table_col_index(t, s->sets[i].col) < 0) {
            snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                     s->sets[i].col, t->name);
            return -1;
        }
        /* "SET n = 1, n = 2" was last-writer-wins with no warning */
        for (int j = 0; j < i; j++) {
            if (strcasecmp(s->sets[i].col, s->sets[j].col) == 0) {
                snprintf(err, MAX_ERR,
                         "column '%s' is assigned twice in the same SET",
                         s->sets[i].col);
                return -1;
            }
        }
    }

    /* collect first, then modify, so we never mutate a page mid-scan */
    RowSet rs;
    rs_init(&rs);
    Scan sc;
    Row r;
    scan_init(&sc, db, t);
    while (scan_next(&sc, &r)) {
        int m;
        if (row_matches(s->where, &r, t, now, &m, err) < 0) {
            row_clear(&r); rs_free(&rs); return -1;
        }
        if (!m) { row_clear(&r); continue; }
        if (rs_push(&rs, &r, 0) < 0) {
            row_clear(&r); rs_free(&rs);
            snprintf(err, MAX_ERR, "out of memory");
            return -1;
        }
    }

    int changed = 0;
    for (int i = 0; i < rs.n; i++) {
        Row *row = &rs.rows[i];

        /* save the old column values so the parent-side foreign key check can
         * see what changed */
        Value old_v[MAX_COLS];
        for (int c = 0; c < t->ncols; c++) old_v[c] = val_copy(&row->v[c]);

        /* save indexed column values before SET modifies them */
        Value old_idx_vals[MAX_INDEXES];
        for (int ki = 0; ki < t->nindexes; ki++) {
            int col = t->indexes[ki].col;
            old_idx_vals[ki] = (col >= 0 && col < row->ncols)
                               ? val_copy(&row->v[col]) : val_null();
        }

        for (int j = 0; j < s->nsets; j++) {
            int idx = table_col_index(t, s->sets[j].col);
            Value v;
            if (eval_expr(s->sets[j].val, row, t, now, &v, err) < 0) {
                for (int ki = 0; ki < t->nindexes; ki++) val_clear(&old_idx_vals[ki]);
                for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
                rs_free(&rs);
                return -1;
            }
            if (coerce_to_column(&t->cols[idx], &v, err) < 0) {
                val_clear(&v);
                for (int ki = 0; ki < t->nindexes; ki++) val_clear(&old_idx_vals[ki]);
                for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
                rs_free(&rs);
                return -1;
            }
            if (v.tag == T_NULL && t->cols[idx].not_null) {
                snprintf(err, MAX_ERR, "column '%s' does not accept NULL",
                         t->cols[idx].name);
                val_clear(&v);
                for (int ki = 0; ki < t->nindexes; ki++) val_clear(&old_idx_vals[ki]);
                for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
                rs_free(&rs);
                return -1;
            }
            val_clear(&row->v[idx]);
            row->v[idx] = v;
        }

        if (check_unique(db, t, row, row->rid, err) < 0) {
            for (int ki = 0; ki < t->nindexes; ki++) val_clear(&old_idx_vals[ki]);
            for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
            rs_free(&rs);
            return -1;
        }
        if (check_constraint(db, t, row, err) < 0) {
            for (int ki = 0; ki < t->nindexes; ki++) val_clear(&old_idx_vals[ki]);
            for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
            rs_free(&rs);
            return -1;
        }
        if (check_fk_child(db, t, row, err) < 0) {
            for (int ki = 0; ki < t->nindexes; ki++) val_clear(&old_idx_vals[ki]);
            for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
            rs_free(&rs);
            return -1;
        }
        /* the old row, for the recursive ON UPDATE cascade walks (borrows the
         * old_v values, which are cleaned up below) */
        Row oldrow;
        memset(&oldrow, 0, sizeof oldrow);
        oldrow.ncols = t->ncols;
        for (int c = 0; c < t->ncols; c++) oldrow.v[c] = old_v[c];
        FkAncestor walk_path[FK_CASCADE_MAX];
        if (fk_update_walk(db, t, &oldrow, row, err, 0, walk_path, 0) < 0) {
            for (int ki = 0; ki < t->nindexes; ki++) val_clear(&old_idx_vals[ki]);
            for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
            rs_free(&rs);
            return -1;
        }
        double cur = mem_strength_at(row->strength, row->last_access, now);
        row->strength = (float)cur;
        row->last_access = now;
        if (heap_replace(db, t, row->ref, row) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            for (int ki = 0; ki < t->nindexes; ki++) val_clear(&old_idx_vals[ki]);
            for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
            rs_free(&rs);
            return -1;
        }

        /* update all indexes: delete old entries, insert new ones */
        for (int ki = 0; ki < t->nindexes; ki++) {
            if (!t->indexes[ki].valid) continue;
            int col = t->indexes[ki].col;
            if (col < 0) continue;
            uint32_t old_root = t->indexes[ki].root;
            if (old_idx_vals[ki].tag != T_NULL)
                index_value_delete(db, &t->indexes[ki], &old_idx_vals[ki],
                                   row->ref, err);
            if (col < row->ncols && row->v[col].tag != T_NULL)
                index_value_insert(db, &t->indexes[ki], &row->v[col],
                                   row->ref, err);
            if (t->indexes[ki].root != old_root)
                db_flush_catalog(db);
            val_clear(&old_idx_vals[ki]);
        }

        /* only after the parent row and its indexes carry the new value:
         * rewrite the referencing CASCADE children (recursively).  Must run
         * before old_v is cleared: the walk matches children against the old
         * key values. */
        if (fk_update_walk(db, t, &oldrow, row, err, 1, walk_path, 0) < 0) {
            for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);
            rs_free(&rs);
            return -1;
        }
        for (int c = 0; c < t->ncols; c++) val_clear(&old_v[c]);

        mem_touch(db, t, row->ref, MEM_BOOST);
        changed++;
    }
    rs_free(&rs);

    printf("(%d row%s updated)\n", changed, changed == 1 ? "" : "s");
    return db_flush_catalog(db);
}

static int exec_delete(DB *db, Stmt *s, char *err)
{
#ifdef DEBUG_DELETE
    fprintf(stderr, "DBG exec_delete entry\n");
#endif
    Table *t = cat_find(db, s->table);
    if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }
    int64_t now = mem_now();

    RowSet rs;
    rs_init(&rs);
    Scan sc;
    Row r;
    scan_init(&sc, db, t);
    while (scan_next(&sc, &r)) {
        int m;
        if (row_matches(s->where, &r, t, now, &m, err) < 0) {
#ifdef DEBUG_DELETE
            fprintf(stderr, "DBG row_matches fail err='%s'\n", err);
#endif
            row_clear(&r); rs_free(&rs); return -1;
        }
        if (m) {
            if (rs_push(&rs, &r, 0) < 0) {
#ifdef DEBUG_DELETE
                fprintf(stderr, "DBG rs_push fail\n");
#endif
                row_clear(&r); rs_free(&rs);
                snprintf(err, MAX_ERR, "out of memory");
                return -1;
            }
        }
    }

    int deleted = 0;
    FkAncestor walk_path[FK_CASCADE_MAX];
    for (int i = 0; i < rs.n; i++) {
        if (fk_delete_recursive(db, t, &rs.rows[i], err, 1, walk_path, 0) < 0) {
#ifdef DEBUG_DELETE
            fprintf(stderr, "DBG fk check_only fail i=%d err='%s'\n", i, err);
#endif
            rs_free(&rs);
            return -1;
        }
    }
    for (int i = 0; i < rs.n; i++) {
        Row *row = &rs.rows[i];
        int del = fk_delete_recursive(db, t, row, err, 0, walk_path, 0);
        if (del < 0) {
#ifdef DEBUG_DELETE
            fprintf(stderr, "DBG fk real fail i=%d err='%s'\n", i, err);
#endif
            rs_free(&rs);
            return -1;
        }
        deleted += del;
    }
    rs_free(&rs);

    printf("(%d row%s deleted)\n", deleted, deleted == 1 ? "" : "s");
    return db_flush_catalog(db);
}

/* ------------------------------------------------- REMEMBER / FORGET */

static int exec_reinforce_stmt(DB *db, Stmt *s, char *err)
{
    Table *t = cat_find(db, s->table);
    if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }
    int64_t now = mem_now();
    int forget = (s->kind == ST_FORGET);

    RowSet rs;
    rs_init(&rs);
    Scan sc;
    Row r;
    scan_init(&sc, db, t);
    while (scan_next(&sc, &r)) {
        int m;
        if (row_matches(s->where, &r, t, now, &m, err) < 0) {
            row_clear(&r); rs_free(&rs); return -1;
        }
        if (!m) { row_clear(&r); continue; }
        if (rs_push(&rs, &r, 0) < 0) {
            row_clear(&r); rs_free(&rs);
            snprintf(err, MAX_ERR, "out of memory");
            return -1;
        }
    }

    for (int i = 0; i < rs.n; i++) {
        if (forget) {
            /* Fade the row but leave the association graph alone. Zeroing its
             * links also stripped the *other* row in each pair of a connection
             * it had earned, which is collateral damage nobody asked for.
             * Deleting a row still clears its links, because it is gone. */
            heap_update_meta(db, rs.rows[i].ref, rs.rows[i].access_count, now, 0.0f);
        } else {
            mem_touch(db, t, rs.rows[i].ref, MEM_BOOST * 3.0);
        }
    }
    if (!forget) reinforce(db, t, &rs, -1, 0.0);   /* associate without extra boost */

    printf("(%d row%s %s)\n", rs.n, rs.n == 1 ? "" : "s",
           forget ? "faded" : "reinforced");
    rs_free(&rs);
    return 0;
}

/* ------------------------------------------------------------ SHOW ... */

static int exec_show_tables(DB *db)
{
    Grid g;
    grid_init(&g, 4);
    snprintf(g.head[0], MAX_NAME, "table");
    snprintf(g.head[1], MAX_NAME, "columns");
    snprintf(g.head[2], MAX_NAME, "rows");
    snprintf(g.head[3], MAX_NAME, "avg_strength");
    int64_t now = mem_now();

    for (int i = 0; i < db->cat.ntables; i++) {
        Table *t = &db->cat.tables[i];
        double sum = 0;
        int64_t cnt = 0;
        Scan sc;
        Row r;
        scan_init(&sc, db, t);
        while (scan_next(&sc, &r)) {
            sum += mem_strength_at(r.strength, r.last_access, now);
            cnt++;
            row_clear(&r);
        }
        char *cells[4];
        cells[0] = strdup(t->name);
        cells[1] = dupf("%d", t->ncols);
        cells[2] = dupf("%lld", (long long)cnt);
        cells[3] = dupf("%.3f", cnt ? sum / (double)cnt : 0.0);
        grid_row(&g, cells);
    }
    if (g.nrows) grid_print(&g);
    printf("\n(%d table%s, %d association%s)\n", g.nrows, g.nrows == 1 ? "" : "s",
           mem_link_count(db), mem_link_count(db) == 1 ? "" : "s");
    grid_free(&g);
    return 0;
}

static int exec_show_memory(DB *db, Stmt *s, char *err)
{
    Table *t = cat_find(db, s->table);
    if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }
    int64_t now = mem_now();

    RowSet rs;
    rs_init(&rs);
    Scan sc;
    Row r;
    scan_init(&sc, db, t);
    while (scan_next(&sc, &r)) {
        if (rs_push(&rs, &r, 0) < 0) { row_clear(&r); break; }
    }

    g_sort_table = t;
    g_sort_col = PC_STRENGTH;
    g_sort_desc = 1;
    g_sort_now = now;
    if (rs.n > 1) qsort(rs.rows, rs.n, sizeof(Row), cmp_rows);

    int limit = (s->top >= 0 && s->top < rs.n) ? s->top : rs.n;

    Grid g;
    grid_init(&g, 5);
    snprintf(g.head[0], MAX_NAME, "_rid");
    snprintf(g.head[1], MAX_NAME, "_strength");
    snprintf(g.head[2], MAX_NAME, "_access");
    snprintf(g.head[3], MAX_NAME, "_last_access");
    snprintf(g.head[4], MAX_NAME, "preview");

    for (int i = 0; i < limit; i++) {
        Row *row = &rs.rows[i];
        char prev[256];
        size_t o = 0;
        prev[0] = 0;
        for (int c = 0; c < row->ncols && o < sizeof prev - 1; c++) {
            char cell[128];
            val_format(&row->v[c], cell, sizeof cell);
            o += (size_t)snprintf(prev + o, sizeof prev - o, "%s%s", c ? " | " : "", cell);
        }
        char tbuf[64];
        fmt_time(row->last_access, tbuf, sizeof tbuf);
        char *cells[5];
        cells[0] = dupf("%llu", (unsigned long long)row->rid);
        cells[1] = dupf("%.3f", mem_strength_at(row->strength, row->last_access, now));
        cells[2] = dupf("%u", row->access_count);
        cells[3] = strdup(tbuf);
        cells[4] = strdup(prev);
        grid_row(&g, cells);
    }
    if (g.nrows) grid_print(&g);
    printf("\n(%d row%s, strongest first)\n", g.nrows, g.nrows == 1 ? "" : "s");
    grid_free(&g);
    rs_free(&rs);
    return 0;
}

/* Find a short preview of whichever row owns this rid. */
static int preview_rid(DB *db, uint64_t rid, char *out, size_t cap, char *tname,
                       size_t tcap)
{
    for (int i = 0; i < db->cat.ntables; i++) {
        Table *t = &db->cat.tables[i];
        Scan sc;
        Row r;
        scan_init(&sc, db, t);
        while (scan_next(&sc, &r)) {
            if (r.rid == rid) {
                size_t o = 0;
                out[0] = 0;
                for (int c = 0; c < r.ncols && o < cap - 1; c++) {
                    char cell[128];
                    val_format(&r.v[c], cell, sizeof cell);
                    o += (size_t)snprintf(out + o, cap - o, "%s%s", c ? " | " : "", cell);
                }
                snprintf(tname, tcap, "%s", t->name);
                row_clear(&r);
                return 1;
            }
            row_clear(&r);
        }
    }
    snprintf(out, cap, "(deleted)");
    snprintf(tname, tcap, "-");
    return 0;
}

static int exec_show_links(DB *db, Stmt *s)
{
    int top = (s->top >= 0) ? s->top : 20;
    /* Fetch every link and filter by table *before* applying TOP. Taking the
     * global top N first meant "SHOW LINKS FROM p TOP 5" could return 2,
     * because the other 3 slots went to links in another table. */
    int total = mem_link_count(db);
    int want = (total > 0) ? total : 1;
    Link *buf = malloc(sizeof(Link) * (size_t)want);
    if (!buf) return -1;
    int n = mem_top_links(db, buf, want);

    Grid g;
    grid_init(&g, 4);
    snprintf(g.head[0], MAX_NAME, "weight");
    snprintf(g.head[1], MAX_NAME, "table");
    snprintf(g.head[2], MAX_NAME, "row_a");
    snprintf(g.head[3], MAX_NAME, "row_b");

    for (int i = 0; i < n && g.nrows < top; i++) {
        char pa[192], pb[192], ta[MAX_NAME], tb[MAX_NAME];
        preview_rid(db, buf[i].a, pa, sizeof pa, ta, sizeof ta);
        preview_rid(db, buf[i].b, pb, sizeof pb, tb, sizeof tb);
        if (s->table[0] && strcasecmp(ta, s->table) && strcasecmp(tb, s->table))
            continue;
        char *cells[4];
        cells[0] = dupf("%.3f", buf[i].w);
        cells[1] = dupf("%s%s%s", ta, strcasecmp(ta, tb) ? " / " : "",
                        strcasecmp(ta, tb) ? tb : "");
        /* prefix the rid: two different rows can hold identical text, and
         * without it the pair looks nonsensically like a self-link */
        cells[2] = dupf("#%llu %s", (unsigned long long)buf[i].a, pa);
        cells[3] = dupf("#%llu %s", (unsigned long long)buf[i].b, pb);
        grid_row(&g, cells);
    }
    if (g.nrows) grid_print(&g);
    else printf("no associations yet - run some queries first\n");
    printf("\n(%d of %d association%s)\n", g.nrows, mem_link_count(db),
           mem_link_count(db) == 1 ? "" : "s");
    grid_free(&g);
    free(buf);
    return 0;
}

/* ----------------------------------------------------------- VACUUM */

/* Rewrite the entire database into a fresh file then atomically swap,
 * reclaiming all free space and defragmenting pages. */
static int exec_vacuum(DB *db, char *err)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.vacuum", db->path);

    /* flush everything to the current file first */
    if (links_flush(db) < 0 || db_flush_catalog(db) < 0 || db_sync(db) < 0) {
        snprintf(err, MAX_ERR, "%s", db->err);
        return -1;
    }

    /* open a fresh database at the temp path */
    DB *ndb = calloc(1, sizeof(DB));
    if (!ndb) {
        snprintf(err, MAX_ERR, "out of memory");
        return -1;
    }
    if (db_open(ndb, tmp) < 0) {
        snprintf(err, MAX_ERR, "cannot create temp database for VACUUM: %s", ndb->err);
        free(ndb);
        return -1;
    }
    ndb->next_rid = db->next_rid;

    /* recreate every table and copy indexes */
    for (int i = 0; i < db->cat.ntables; i++) {
        Table *ot = &db->cat.tables[i];
        if (!cat_create(ndb, ot->name, ot->cols, ot->ncols)) {
            snprintf(err, MAX_ERR, "VACUUM: %s", ndb->err);
            db_close(ndb); free(ndb); unlink(tmp);
            return -1;
        }
        Table *nt = cat_find(ndb, ot->name);
        for (int ki = 0; ki < ot->nindexes; ki++) {
            if (ot->indexes[ki].valid) {
                int nki = table_ensure_index(ndb, nt, ot->indexes[ki].col);
                if (nki >= 0) {
                    nt->indexes[nki].gin = ot->indexes[ki].gin;
                    snprintf(nt->indexes[nki].name, MAX_NAME, "%s",
                             ot->indexes[ki].name);
                }
            }
        }
        snprintf(nt->check, sizeof nt->check, "%s", ot->check);
        nt->nfks = ot->nfks;
        for (int k = 0; k < ot->nfks; k++) nt->fks[k] = ot->fks[k];
        for (int c = 0; c < nt->ncols; c++)
            nt->cols[c] = ot->cols[c];
        Scan sc;
        Row r;
        scan_init(&sc, db, ot);
        while (scan_next(&sc, &r)) {
            /* heap_insert will use the row's existing rid since it's non-zero */
            if (heap_insert(ndb, nt, &r) < 0) {
                snprintf(err, MAX_ERR, "VACUUM: %s", ndb->err);
                row_clear(&r); db_close(ndb); free(ndb); unlink(tmp);
                return -1;
            }
            if (index_insert_row(ndb, nt, &r, err) < 0) {
                snprintf(err, MAX_ERR, "VACUUM: %s", err);
                row_clear(&r); db_close(ndb); free(ndb); unlink(tmp);
                return -1;
            }
            row_clear(&r);
        }
    }

    /* copy stored views */
    ndb->cat.nviews = db->cat.nviews;
    for (int i = 0; i < db->cat.nviews; i++)
        ndb->cat.views[i] = db->cat.views[i];

    /* copy stored procedures */
    ndb->cat.nprocs = db->cat.nprocs;
    for (int i = 0; i < db->cat.nprocs; i++) {
        ndb->cat.procs[i] = db->cat.procs[i];
        /* the parsed-body cache belongs to the old DB handle */
        ndb->cat.procs[i].cache = NULL;
    }

    /* copy associative memory links */
    for (uint32_t i = 0; i < db->links.n; i++) {
        Link *lk = &db->links.e[i];
        if (lk->a && lk->b)
            mem_link_set(ndb, lk->a, lk->b, lk->w);
    }

    /* close the new database (flushes its catalog and links) */
    db_close(ndb);
    free(ndb);

    /* atomically swap files: close old, rename temp over it, reopen */
    char final_path[512];
    snprintf(final_path, sizeof final_path, "%s", db->path);
    db_close(db);

    if (rename(tmp, final_path) < 0) {
        snprintf(err, MAX_ERR, "VACUUM: cannot swap files: %s", strerror(errno));
        /* try to reopen the original */
        unlink(tmp);
        db_open(db, final_path);
        return -1;
    }

    if (db_open(db, final_path) < 0) {
        snprintf(err, MAX_ERR, "VACUUM: cannot reopen database: %s", db->err);
        return -1;
    }

    printf("vacuum complete (file compacted)\n");
    return 0;
}

/* Public wrapper for auto-vacuum (used by the server GC thread). */
int db_vacuum(DB *db, char *err)
{
    return exec_vacuum(db, err);
}

/* ----------------------------------------------------------- dispatcher */

/* -------------------------------------------------- procedure statements */

/* Parse and execute a dynamic SQL batch (EXEC ('...')). Unlike exec_script,
 * the first failure stops the batch and reports the error to the caller, so
 * TRY/CATCH around the EXEC sees it. Statements see the enclosing procedure's
 * variable frame. */
static int exec_dynamic_batch(DB *db, const char *sql, char *err)
{
    g_db = db;
    Lexer lx;
    lex_init(&lx, sql);
    if (lex_next(&lx) < 0) {
        snprintf(err, MAX_ERR, "%s", lx.err);
        return -1;
    }
    for (;;) {
        Stmt st;
        int rc = parse_stmt(&lx, &st, err);
        if (rc == 0) break;
        if (rc < 0) {
            if (!err[0]) snprintf(err, MAX_ERR, "%s", lx.err);
            return -1;
        }
        if (exec_stmt(db, &st, err) < 0) {
            stmt_free(&st);
            return -1;
        }
        stmt_free(&st);
    }
    links_flush(db);
    db_flush_catalog(db);
    return 0;
}

/* Execute one statement inside a procedure body. Returns a control-flow code
 * (P_OK / P_BREAK / P_CONTINUE / P_RETURN) or -1 on error. */
static int exec_proc_stmt(DB *db, Stmt *s, char *err)
{
    switch (s->kind) {
    case ST_BLOCK:
        if (!g_var_frame) return -1;
        scope_push(g_var_frame);
        for (int i = 0; i < s->nstmts; i++) {
            int rc = exec_proc_stmt(db, s->stmts[i], err);
            if (rc != P_OK) {
                scope_pop(g_var_frame);
                return rc;
            }
        }
        scope_pop(g_var_frame);
        return P_OK;

    case ST_IF: {
        int take_else = 0;
        if (s->where) {
            Value cv;
            if (eval_expr(s->where, NULL, NULL, mem_now(), &cv, err) < 0) return -1;
            take_else = !val_truthy(&cv);
            val_clear(&cv);
        }
        if (!take_else) {
            for (int i = 0; i < s->nstmts; i++) {
                int rc = exec_proc_stmt(db, s->stmts[i], err);
                if (rc != P_OK) return rc;
            }
        } else {
            for (int i = 0; i < s->n_else; i++) {
                int rc = exec_proc_stmt(db, s->else_stmts[i], err);
                if (rc != P_OK) return rc;
            }
        }
        return P_OK;
    }

    case ST_WHILE: {
        for (;;) {
            Value cv;
            if (s->where) {
                if (eval_expr(s->where, NULL, NULL, mem_now(), &cv, err) < 0) return -1;
            } else {
                cv = val_null();
            }
            int run = val_truthy(&cv);
            val_clear(&cv);
            if (!run) return P_OK;
            for (int i = 0; i < s->nstmts; i++) {
                int rc = exec_proc_stmt(db, s->stmts[i], err);
                if (rc == P_BREAK) return P_OK;
                if (rc == P_CONTINUE) break;
                if (rc != P_OK) return rc;
            }
        }
    }

    case ST_BREAK:
        return P_BREAK;

    case ST_CONTINUE:
        return P_CONTINUE;

    case ST_RETURN:
        if (s->nsets && s->sets[0].val) {
            Value rv;
            if (eval_expr(s->sets[0].val, NULL, NULL, mem_now(), &rv, err) < 0) return -1;
            if (rv.tag == T_INT || rv.tag == T_BIT)
                g_proc_retval = (int)rv.i;
            else if (rv.tag == T_FLOAT)
                g_proc_retval = (int)rv.f;
            else
                g_proc_retval = 0;
            val_clear(&rv);
        }
        return P_RETURN;

    case ST_DECLARE: {
        const char *name = s->sets[0].col;
        if (name[0] == '@' && name[1] == '@') {
            snprintf(err, MAX_ERR, "cannot declare system variable '%s'", name);
            return -1;
        }
        if (!g_var_frame) {
            snprintf(err, MAX_ERR, "DECLARE can only be used inside a procedure");
            return -1;
        }
        if (var_in_scope(g_var_frame, name)) {
            snprintf(err, MAX_ERR, "variable '%s' already declared in this scope",
                     name);
            return -1;
        }
        if (g_var_frame->n >= MAX_VARS) {
            snprintf(err, MAX_ERR, "too many variables (max %d)", MAX_VARS);
            return -1;
        }
        VarSlot *slot = &g_var_frame->slots[g_var_frame->n++];
        snprintf(slot->name, MAX_NAME, "%s", name);
        val_clear(&slot->v);
        if (s->nsets && s->sets[0].val) {
            if (eval_expr(s->sets[0].val, NULL, NULL, mem_now(), &slot->v, err) < 0)
                return -1;
        } else {
            slot->v = val_null();
        }
        return P_OK;
    }

    case ST_SET: {
        const char *name = s->sets[0].col;
        if (name[0] == '@' && name[1] == '@') {
            snprintf(err, MAX_ERR, "cannot modify system variable '%s'", name);
            return -1;
        }
        VarSlot *slot = var_find(name);
        if (!slot) {
            snprintf(err, MAX_ERR, "variable '%s' has not been declared", name);
            return -1;
        }
        Value nv;
        if (eval_expr(s->sets[0].val, NULL, NULL, mem_now(), &nv, err) < 0) return -1;
        val_clear(&slot->v);
        slot->v = nv;
        return P_OK;
    }

    case ST_DECLARE_CURSOR: {
        if (cursor_find(s->table)) {
            snprintf(err, MAX_ERR, "cursor '%s' already exists", s->table);
            return -1;
        }
        if (g_ncursors >= MAX_CURSORS) {
            snprintf(err, MAX_ERR, "too many cursors (max %d)", MAX_CURSORS);
            return -1;
        }
        Cursor *c = &g_cursors[g_ncursors++];
        snprintf(c->name, MAX_NAME, "%s", s->table);
        c->sel = s->derived;   /* borrowed from the program */
        c->opened = 0;
        c->pos = 0;
        return P_OK;
    }

    case ST_OPEN: {
        Cursor *c = cursor_find(s->table);
        if (!c) {
            snprintf(err, MAX_ERR, "unknown cursor '%s'", s->table);
            return -1;
        }
        if (!c->sel) {
            snprintf(err, MAX_ERR, "cursor '%s' has no SELECT", s->table);
            return -1;
        }
        capture_free(&c->cap);
        if (exec_select_into(db, c->sel, &c->cap, err) < 0) return -1;
        c->opened = 1;
        c->pos = 0;
        return P_OK;
    }

    case ST_FETCH: {
        Cursor *c = cursor_find(s->table);
        if (!c) {
            snprintf(err, MAX_ERR, "unknown cursor '%s'", s->table);
            return -1;
        }
        if (!c->opened) {
            snprintf(err, MAX_ERR, "cursor '%s' is not open", s->table);
            return -1;
        }
        int status;
        if (c->pos < c->cap.nrows) {
            for (int i = 0; i < s->n_ins_cols; i++) {
                VarSlot *slot = var_find(s->ins_cols[i]);
                if (!slot) {
                    snprintf(err, MAX_ERR, "variable '%s' has not been declared",
                             s->ins_cols[i]);
                    return -1;
                }
                int ci = i < c->cap.ncols ? i : c->cap.ncols - 1;
                val_clear(&slot->v);
                slot->v = val_copy(&c->cap.cells[c->pos * c->cap.ncols + ci]);
            }
            c->pos++;
            status = 0;
        } else {
            status = -1;
        }
        VarSlot *st = var_find("@@fetch_status");
        if (st) {
            val_clear(&st->v);
            st->v = val_int(status);
        }
        return P_OK;
    }

    case ST_CLOSE: {
        Cursor *c = cursor_find(s->table);
        if (!c) {
            snprintf(err, MAX_ERR, "unknown cursor '%s'", s->table);
            return -1;
        }
        if (c->opened) {
            capture_free(&c->cap);
            c->opened = 0;
        }
        return P_OK;
    }

    case ST_DEALLOCATE: {
        for (int i = 0; i < g_ncursors; i++) {
            if (strcasecmp(g_cursors[i].name, s->table) != 0) continue;
            capture_free(&g_cursors[i].cap);
            for (int j = i; j < g_ncursors - 1; j++)
                g_cursors[j] = g_cursors[j + 1];
            g_ncursors--;
            return P_OK;
        }
        snprintf(err, MAX_ERR, "unknown cursor '%s'", s->table);
        return -1;
    }

    case ST_TRY: {
        int rc = P_OK;
        for (int i = 0; i < s->nstmts && rc == P_OK; i++)
            rc = exec_proc_stmt(db, s->stmts[i], err);
        if (rc < 0) {
            /* the error is caught: expose its message as @@error_message and
             * run the CATCH block, which may itself error out */
            VarFrame *f = g_var_frame;
            if (f) {
                VarSlot *em = var_find("@@error_message");
                if (!em && f->n < MAX_VARS) {
                    em = &f->slots[f->n++];
                    snprintf(em->name, MAX_NAME, "@@error_message");
                }
                if (em) {
                    val_clear(&em->v);
                    em->v = val_text(err[0] ? err : "error");
                }
            }
            for (int i = 0; i < s->n_else && rc != P_OK; i++)
                rc = exec_proc_stmt(db, s->else_stmts[i], err);
            if (rc < 0) return -1;
            return P_OK;
        }
        return rc;
    }

    case ST_EXEC_SQL: {
        Value v;
        if (eval_expr(s->sets[0].val, NULL, NULL, mem_now(), &v, err) < 0)
            return -1;
        if (v.tag != T_TEXT) {
            val_clear(&v);
            snprintf(err, MAX_ERR, "EXEC ('...') needs a string expression");
            return -1;
        }
        /* guard against self-referential strings like EXEC(@s) */
        if (g_proc_depth >= MAX_PROC_DEPTH) {
            val_clear(&v);
            snprintf(err, MAX_ERR, "procedure call depth exceeded (max %d)",
                     MAX_PROC_DEPTH);
            return -1;
        }
        g_proc_depth++;
        int rc = exec_dynamic_batch(db, v.s, err);
        g_proc_depth--;
        val_clear(&v);
        if (rc < 0) return -1;
        if (s->retvar[0]) {
            VarSlot *cs = var_find(s->retvar);
            if (cs) {
                val_clear(&cs->v);
                cs->v = val_int(g_proc_retval);
            }
        }
        return P_OK;
    }

    default:
        return exec_stmt(db, s, err) < 0 ? -1 : P_OK;
    }
}

/* Top-level statement dispatcher: CREATE, SELECT, RECALL, CHECKPOINT, etc. */
static int exec_stmt_inner(DB *db, Stmt *s, char *err);

int exec_stmt(DB *db, Stmt *s, char *err)
{
    g_db = db;
    /* Outside a CALL there is no variable frame; provide a scratch one so
     * top-level TRY/CATCH can expose @@error_message and EXEC @rc = proc has
     * somewhere to write. Nested dispatches keep using it (the check is
     * whether one is already installed). */
    VarFrame scratch;
    VarFrame *saved = g_var_frame;
    if (!saved) {
        memset(&scratch, 0, sizeof scratch);
        g_var_frame = &scratch;
    }
    int rc = exec_stmt_inner(db, s, err);
    g_var_frame = saved;
    return rc;
}

static int exec_stmt_inner(DB *db, Stmt *s, char *err)
{
#ifdef DEBUG_DELETE
    fprintf(stderr, "DBG exec_stmt_inner kind=%d\n", (int)s->kind);
#endif
    switch (s->kind) {
    case ST_NOOP:
        return 0;

    case ST_CREATE: {
        if (!cat_create(db, s->table, s->cols, s->ncols)) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        Table *ct = cat_find(db, s->table);
        if (ct && s->check[0]) {
            snprintf(ct->check, sizeof ct->check, "%s", s->check);
            db_flush_catalog(db);
        }
        int has_fk = 0;
        for (int ci = 0; ci < s->ncols && ct && !has_fk; ci++)
            if (s->cols[ci].refs_table[0]) has_fk = 1;
        if (ct && (s->nfks || has_fk)) {
            for (int i = 0; i < ct->ncols; i++)
                ct->cols[i] = s->cols[i];
            ct->nfks = s->nfks;
            for (int i = 0; i < s->nfks; i++) ct->fks[i] = s->fks[i];
            if (fk_validate(db, ct, err) < 0) {
                /* undo the failed create: the table is empty and has no
                 * indexes yet, so removing the catalog slot is enough */
                db->cat.ntables--;
                return -1;
            }
        }
        for (int ci = 0; ci < s->ncols && ct; ci++) {
            if (s->cols[ci].unique) {
                int idx = table_ensure_index(db, ct, ci);
                if (idx < 0) return -1;
            }
        }
        if (db_flush_catalog(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        printf("table '%s' created with %d column%s\n", s->table, s->ncols,
               s->ncols == 1 ? "" : "s");
        return 0;
    }

    case ST_CREATE_VIEW: {
        if (cat_find(db, s->table) || cat_find_view(db, s->table)) {
            snprintf(err, MAX_ERR, "table or view '%s' already exists", s->table);
            return -1;
        }
        if (db->cat.nviews >= MAX_VIEWS) {
            snprintf(err, MAX_ERR, "too many views (max %d)", MAX_VIEWS);
            return -1;
        }
        View *v = &db->cat.views[db->cat.nviews];
        snprintf(v->name, MAX_NAME, "%s", s->table);
        snprintf(v->sql, MAX_VIEW_SQL, "%s", s->view_sql);
        db->cat.nviews++;
        if (db_flush_catalog(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        printf("view '%s' created\n", s->table);
        return 0;
    }

    case ST_DROP_VIEW: {
        int found = 0;
        for (int i = 0; i < db->cat.nviews; i++) {
            if (strcasecmp(db->cat.views[i].name, s->table) != 0) continue;
            found = 1;
            for (int j = i; j < db->cat.nviews - 1; j++)
                db->cat.views[j] = db->cat.views[j + 1];
            db->cat.nviews--;
            break;
        }
        if (!found && !s->if_exists) {
            snprintf(err, MAX_ERR, "unknown view '%s'", s->table);
            return -1;
        }
        if (found && db_flush_catalog(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        if (found) printf("view '%s' dropped\n", s->table);
        return 0;
    }

    case ST_CREATE_PROC: {
        /* procedures live in their own namespace: a procedure may share a
         * name with a table or view (CALL and FROM look up different lists) */
        if (cat_find_proc(db, s->table)) {
            snprintf(err, MAX_ERR, "procedure '%s' already exists", s->table);
            return -1;
        }
        if (db->cat.nprocs >= MAX_PROCS) {
            snprintf(err, MAX_ERR, "too many procedures (max %d)", MAX_PROCS);
            return -1;
        }
        Proc *p = &db->cat.procs[db->cat.nprocs];
        p->cache = NULL;
        snprintf(p->name, MAX_NAME, "%s", s->table);
        snprintf(p->sql, MAX_PROC_SQL, "%s", s->proc_sql);
        snprintf(p->params, MAX_PROC_SQL, "%s", s->param_sql);
        db->cat.nprocs++;
        if (db_flush_catalog(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        printf("procedure '%s' created\n", s->table);
        return 0;
    }

    case ST_DROP_PROC: {
        int found = 0;
        for (int i = 0; i < db->cat.nprocs; i++) {
            if (strcasecmp(db->cat.procs[i].name, s->table) != 0) continue;
            found = 1;
            if (db->cat.procs[i].cache) {
                prog_free(db->cat.procs[i].cache);
                free(db->cat.procs[i].cache);
            }
            for (int j = i; j < db->cat.nprocs - 1; j++)
                db->cat.procs[j] = db->cat.procs[j + 1];
            db->cat.nprocs--;
            break;
        }
        if (!found && !s->if_exists) {
            snprintf(err, MAX_ERR, "unknown procedure '%s'", s->table);
            return -1;
        }
        if (found && db_flush_catalog(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        if (found) printf("procedure '%s' dropped\n", s->table);
        return 0;
    }

    case ST_CREATE_INDEX: {
        Table *t = cat_find(db, s->table);
        if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }
        if (!s->index_gin) {
            snprintf(err, MAX_ERR,
                     "only CREATE INDEX ... USING GIN is supported");
            return -1;
        }
        int col = table_col_index(t, s->index_col);
        if (col < 0) {
            snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                     s->index_col, t->name);
            return -1;
        }
        for (int i = 0; i < t->nindexes; i++) {
            if (!t->indexes[i].valid) continue;
            if (strcasecmp(t->indexes[i].name, s->index_name) == 0) {
                snprintf(err, MAX_ERR, "index '%s' already exists", s->index_name);
                return -1;
            }
            if (t->indexes[i].gin && t->indexes[i].col == col) {
                snprintf(err, MAX_ERR,
                         "a full-text index already exists on column '%s'",
                         s->index_col);
                return -1;
            }
        }
        if (t->nindexes >= MAX_INDEXES) {
            snprintf(err, MAX_ERR, "too many indexes (max %d)", MAX_INDEXES);
            return -1;
        }
        int idx = t->nindexes++;
        Index *ix = &t->indexes[idx];
        memset(ix, 0, sizeof *ix);
        ix->col = col;
        ix->gin = 1;
        snprintf(ix->name, MAX_NAME, "%s", s->index_name);
        btree_create(ix);
        /* build the posting lists over the existing rows */
        Scan sc;
        Row r;
        scan_init(&sc, db, t);
        while (scan_next(&sc, &r)) {
            if (col < r.ncols && r.v[col].tag != T_NULL) {
                if (index_value_insert(db, ix, &r.v[col], r.ref, err) < 0) {
                    row_clear(&r);
                    btree_destroy(db, ix->root);
                    t->nindexes--;
                    return -1;
                }
            }
            row_clear(&r);
        }
        if (db_flush_catalog(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        printf("index '%s' created on %s (%s)\n", s->index_name, t->name,
               s->index_col);
        return 0;
    }

    case ST_DROP_INDEX: {
        for (int ti = 0; ti < db->cat.ntables; ti++) {
            Table *t = &db->cat.tables[ti];
            for (int ki = 0; ki < t->nindexes; ki++) {
                if (!t->indexes[ki].valid) continue;
                if (strcasecmp(t->indexes[ki].name, s->index_name) != 0)
                    continue;
                if (s->table[0] && strcasecmp(t->name, s->table) != 0) {
                    snprintf(err, MAX_ERR, "index '%s' is on table '%s', not '%s'",
                             s->index_name, t->name, s->table);
                    return -1;
                }
                btree_destroy(db, t->indexes[ki].root);
                for (int j = ki; j < t->nindexes - 1; j++)
                    t->indexes[j] = t->indexes[j + 1];
                t->nindexes--;
                if (db_flush_catalog(db) < 0) {
                    snprintf(err, MAX_ERR, "%s", db->err);
                    return -1;
                }
                printf("index '%s' dropped\n", s->index_name);
                return 0;
            }
        }
        if (s->if_exists) return 0;
        snprintf(err, MAX_ERR, "unknown index '%s'", s->index_name);
        return -1;
    }

    case ST_CALL: {
        Proc *p = cat_find_proc(db, s->table);
        if (!p) {
            snprintf(err, MAX_ERR, "unknown procedure '%s'", s->table);
            return -1;
        }
        if (g_proc_depth >= MAX_PROC_DEPTH) {
            snprintf(err, MAX_ERR, "procedure call depth exceeded (max %d)",
                     MAX_PROC_DEPTH);
            return -1;
        }

        /* the callee's variable frame; parameters first, @@fetch_status at
         * slot 0 (so FETCH works even before any variable is declared) */
        VarFrame frame;
        memset(&frame, 0, sizeof frame);
        snprintf(frame.slots[0].name, MAX_NAME, "@@fetch_status");
        frame.slots[0].v = val_int(0);
        frame.n = 1;

        char pnames[MAX_VARS][MAX_NAME];
        Expr *pdefs[MAX_VARS];
        int nparams = 0;
        if (p->params[0]) {
            if (proc_parse_params(p->params, pnames, pdefs, MAX_VARS, &nparams, err) < 0)
                return -1;
            for (int i = 0; i < nparams; i++) {
                if (frame.n >= MAX_VARS) {
                    snprintf(err, MAX_ERR, "too many parameters (max %d)",
                             MAX_VARS - 1);
                    return -1;
                }
                snprintf(frame.slots[frame.n].name, MAX_NAME, "%s", pnames[i]);
                frame.slots[frame.n].v = val_null();
                frame.n++;
            }
        }
        frame.nparams = nparams;

        /* map each argument to a callee frame slot; named arguments can
         * arrive in any order, so the mapping is by name, not position */
        int slot[MAX_FUNC_ARGS];
        int assigned[MAX_VARS];
        for (int j = 0; j < MAX_VARS; j++) assigned[j] = 0;
        for (int i = 0; i < s->nargs; i++) {
            if (s->argnames[i][0]) {
                int k = -1;
                for (int j = 0; j < nparams; j++)
                    if (strcasecmp(pnames[j], s->argnames[i]) == 0) { k = j; break; }
                if (k < 0) {
                    snprintf(err, MAX_ERR, "procedure '%s' has no parameter '%s'",
                             s->table, s->argnames[i]);
                    return -1;
                }
                slot[i] = 1 + k;
            } else {
                if (i >= nparams) {
                    snprintf(err, MAX_ERR,
                             "procedure '%s' takes at most %d argument%s, got %d",
                             s->table, nparams, nparams == 1 ? "" : "s", s->nargs);
                    return -1;
                }
                slot[i] = 1 + i;
            }
            if (assigned[slot[i] - 1]) {
                snprintf(err, MAX_ERR, "parameter '%s' was supplied more than once",
                         pnames[slot[i] - 1]);
                return -1;
            }
            assigned[slot[i] - 1] = 1;
        }
        /* parameters without defaults must be supplied */
        for (int j = 0; j < nparams; j++) {
            if (!assigned[j] && !pdefs[j]) {
                snprintf(err, MAX_ERR, "procedure '%s' expects an argument for '%s'",
                         s->table, pnames[j]);
                return -1;
            }
        }

        /* bind arguments in the caller's frame, then switch frames */
        for (int i = 0; i < s->nargs; i++) {
            if (eval_expr(s->args[i], NULL, NULL, mem_now(), &frame.slots[slot[i]].v, err) < 0)
                return -1;
        }

        /* the body is parsed once and cached; DROP and DB close free it */
        Program *pgm = p->cache;
        if (!pgm) {
            pgm = calloc(1, sizeof *pgm);
            if (!pgm) {
                snprintf(err, MAX_ERR, "out of memory");
                return -1;
            }
            if (parse_program(p->sql, pgm, err) < 0) {
                free(pgm);
                return -1;
            }
            p->cache = pgm;
        }

        VarFrame *saved = g_var_frame;
        g_var_frame = &frame;
        g_proc_depth++;
        g_proc_retval = 0;
        /* defaults run in the callee frame, so they may use earlier parameters */
        for (int j = 0; j < nparams; j++) {
            if (assigned[j] || !pdefs[j]) continue;
            if (eval_expr(pdefs[j], NULL, NULL, mem_now(), &frame.slots[1 + j].v, err) < 0) {
                g_proc_depth--;
                g_var_frame = saved;
                for (int k = 0; k < g_ncursors; k++)
                    capture_free(&g_cursors[k].cap);
                g_ncursors = 0;
                return -1;
            }
        }
        int rc = P_OK;
        for (int i = 0; i < pgm->nstmts && rc == P_OK; i++)
            rc = exec_proc_stmt(db, pgm->stmts[i], err);
        g_proc_depth--;
        g_var_frame = saved;

        /* cursors borrow their SELECT from this call's program, so they must
         * all be gone before the program is freed. Sweep at every CALL (not
         * just the outermost): cursors are call-scoped, so nothing can dangle. */
        for (int i = 0; i < g_ncursors; i++)
            capture_free(&g_cursors[i].cap);
        g_ncursors = 0;

        /* copy OUTPUT parameters back into the caller's variables */
        for (int i = 0; i < s->nargs; i++) {
            if (!s->argout[i]) continue;
            VarSlot *cs = var_find(s->args[i]->col);
            if (!cs) continue;
            val_clear(&cs->v);
            cs->v = val_copy(&frame.slots[slot[i]].v);
        }
        /* EXEC @rc = proc: hand over the return code */
        if (s->retvar[0]) {
            VarSlot *cs = var_find(s->retvar);
            if (cs) {
                val_clear(&cs->v);
                cs->v = val_int(g_proc_retval);
            }
        }
        if (rc < 0) return -1;
        return 0;
    }

    case ST_DROP: {
        Table *t = cat_find(db, s->table);
        if (!t && s->if_exists) return 0;
        if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }
        if (fk_is_referenced(db, t)) {
            snprintf(err, MAX_ERR,
                     "cannot drop '%s': another table's FOREIGN KEY "
                     "references it", s->table);
            return -1;
        }
        for (int ki = 0; ki < t->nindexes; ki++) {
            if (t->indexes[ki].root) {
                btree_destroy(db, t->indexes[ki].root);
                t->indexes[ki].root = 0;
                t->indexes[ki].valid = 0;
            }
        }
        t->nindexes = 0;
        /* forget the associations belonging to the rows we are about to lose */
        Scan sc;
        Row r;
        scan_init(&sc, db, t);
        while (scan_next(&sc, &r)) { mem_forget_row(db, r.rid); row_clear(&r); }
        if (cat_drop(db, s->table) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        printf("table '%s' dropped\n", s->table);
        return 0;
    }

    case ST_ALTER_ADD:
    case ST_ALTER_DROP:
    case ST_ALTER_RENAME:
    case ST_ALTER_TYPE:  return exec_alter(db, s, err);
    case ST_TRUNCATE:    return exec_truncate(db, s, err);
    case ST_INSERT:      return s->sub ? exec_insert_select(db, s, err)
                                       : exec_insert(db, s, err);
    case ST_SELECT:      return exec_select(db, s, err);
    case ST_UPDATE:      return exec_update(db, s, err);
    case ST_DELETE:      return exec_delete(db, s, err);
    case ST_RECALL:      return exec_recall(db, s, err);
    case ST_REMEMBER:
    case ST_FORGET:      return exec_reinforce_stmt(db, s, err);
    case ST_SHOW_TABLES: return exec_show_tables(db);
    case ST_SHOW_MEMORY: return exec_show_memory(db, s, err);
    case ST_SHOW_LINKS:  return exec_show_links(db, s);

    case ST_PRINT: {
        Value v;
        if (s->nsets && s->sets[0].val) {
            if (eval_expr(s->sets[0].val, NULL, NULL, mem_now(), &v, err) < 0)
                return -1;
        } else {
            v = val_text(s->msg);
        }
        char buf[1024];
        val_format(&v, buf, sizeof buf);
        printf("%s\n", buf);
        val_clear(&v);
        return 0;
    }

    case ST_CHECKPOINT:
        /* fsync, not just pwrite: otherwise "checkpoint complete" promised
         * durability it had not delivered. */
        if (links_flush(db) < 0 || db_flush_catalog(db) < 0 || db_sync(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        if (wal_checkpoint(db) < 0) {
            snprintf(err, MAX_ERR, "%s", db->err);
            return -1;
        }
        printf("checkpoint complete (flushed to disk)\n");
        return 0;

    case ST_BEGIN:
        if (db->txn_depth > 0) {
            /* Nested BEGIN: push a savepoint at the current undo depth */
            if (db->txn_sp_count >= MAX_SAVEPOINTS) {
                snprintf(err, MAX_ERR, "too many nested transactions (max %d)",
                         MAX_SAVEPOINTS);
                return -1;
            }
            db->txn_sp[db->txn_sp_count++] = db->undo_depth;
            db->txn_depth++;
            return 0;
        }
        db->txn_active = 1;
        db->txn_depth = 1;
        db->undo_depth = 0;
        db->txn_sp_count = 0;
        return 0;

    case ST_COMMIT:
        if (db->txn_depth == 0) {
            snprintf(err, MAX_ERR, "no active transaction");
            return -1;
        }
        if (db->txn_depth == 1) {
            /* Outer commit: flush and finalise */
            if (links_flush(db) < 0) {
                snprintf(err, MAX_ERR, "%s", db->err);
                return -1;
            }
            if (db_flush_catalog(db) < 0) {
                snprintf(err, MAX_ERR, "%s", db->err);
                return -1;
            }
        }
        pager_undo_commit(db);
        return 0;

    case ST_ROLLBACK:
        if (db->txn_depth == 0) {
            snprintf(err, MAX_ERR, "no active transaction");
            return -1;
        }
        pager_undo_rollback(db);
        return 0;

    case ST_EXPLAIN: {
        Stmt *inner = s->sub;
        if (!inner) {
            printf("EXPLAIN of empty statement\n");
            return 0;
        }
        printf("QUERY PLAN\n----------\n");
        switch (inner->kind) {
        case ST_SELECT:
        case ST_RECALL:
            printf("SCAN TABLE %s\n", inner->table[0] ? inner->table : "(subquery)");
            if (inner->where) printf("WHERE (filter expression)\n");
            if (inner->njoins > 0) {
                for (int j = 0; j < inner->njoins; j++)
                    printf("JOIN %s (%s)\n", inner->joins[j].table,
                           inner->joins[j].type == JOIN_LEFT ? "LEFT" : "INNER");
            }
            if (inner->ngroup > 0) printf("GROUP BY %d key(s)\n", inner->ngroup);
            if (inner->norder > 0) printf("ORDER BY %d key(s)\n", inner->norder);
            if (inner->top >= 0)   printf("TOP %d\n", inner->top);
            int naggs = 0;
            for (int i = 0; i < inner->nitems; i++)
                if (inner->items[i].e && inner->items[i].e->kind == EX_AGG) naggs++;
            if (naggs) printf("AGGREGATE %d function(s)\n", naggs);
            break;
        case ST_INSERT:
            printf("INSERT INTO %s\n", inner->table);
            if (inner->sub) printf("  (from SELECT)\n");
            else            printf("  %d row(s)\n", inner->nrows);
            break;
        case ST_UPDATE:
            printf("UPDATE %s", inner->table);
            if (inner->where) printf(" WHERE (filter)");
            printf("\n");
            break;
        case ST_DELETE:
            printf("DELETE FROM %s", inner->table);
            if (inner->where) printf(" WHERE (filter)");
            printf("\n");
            break;
        case ST_CREATE:
            printf("CREATE TABLE %s (%d cols)\n", inner->table, inner->ncols);
            break;
        case ST_DROP:
            printf("DROP TABLE %s\n", inner->table);
            break;
        case ST_TRUNCATE:
            printf("TRUNCATE TABLE %s\n", inner->table);
            break;
        default:
            printf("%s\n", inner->kind == ST_ALTER_ADD ? "ALTER TABLE ADD COLUMN" :
                   inner->kind == ST_ALTER_DROP ? "ALTER TABLE DROP COLUMN" :
                   inner->kind == ST_ALTER_RENAME ? "ALTER TABLE RENAME COLUMN" :
                   inner->kind == ST_ALTER_TYPE ? "ALTER TABLE ALTER COLUMN" :
                   inner->kind == ST_VACUUM ? "VACUUM" :
                   inner->kind == ST_CHECKPOINT ? "CHECKPOINT" :
                   inner->kind == ST_REMEMBER ? "REMEMBER" :
                   inner->kind == ST_FORGET ? "FORGET" :
                   inner->kind == ST_BEGIN ? "BEGIN" :
                   inner->kind == ST_COMMIT ? "COMMIT" :
                   inner->kind == ST_ROLLBACK ? "ROLLBACK" :
                   inner->kind == ST_PRINT ? "PRINT" : "<unknown>");
            break;
        }
        return 0;
    }

    case ST_VACUUM:
        return exec_vacuum(db, err);

    case ST_IF:
    case ST_WHILE:
    case ST_BLOCK:
    case ST_BREAK:
    case ST_CONTINUE:
    case ST_RETURN:
    case ST_DECLARE:
    case ST_SET:
    case ST_DECLARE_CURSOR:
    case ST_OPEN:
    case ST_FETCH:
    case ST_CLOSE:
    case ST_DEALLOCATE:
        snprintf(err, MAX_ERR,
                 "this statement can only be used inside a stored procedure body");
        return -1;
    case ST_TRY:
    case ST_EXEC_SQL:
        /* also reachable at batch level: the proc-statement dispatcher owns
         * the implementation, and it never routes these back here */
        return exec_proc_stmt(db, s, err) < 0 ? -1 : 0;
    }
    snprintf(err, MAX_ERR, "unimplemented statement");
    return -1;
}

/* Parse and execute every statement in a SQL script until EOF or error. */
int exec_script(DB *db, const char *sql, int echo)
{
    g_db = db;
    Lexer lx;
    char err[MAX_ERR];
    int failures = 0;

    lex_init(&lx, sql);
    if (lex_next(&lx) < 0) {
        fprintf(stderr, "error: %s\n", lx.err);
        return 1;
    }

    for (;;) {
        Stmt st;
        err[0] = 0;
        int rc = parse_stmt(&lx, &st, err);
        if (rc == 0) break;
        if (rc < 0) {
            fprintf(stderr, "error: %s\n", err[0] ? err : lx.err);
            return failures + 1;
        }
        if (echo) printf("\n");
        if (exec_stmt(db, &st, err) < 0) {
            fprintf(stderr, "error: %s\n", err[0] ? err : "statement failed");
            if (db->txn_active) {
                pager_undo_rollback(db);
                fprintf(stderr, "transaction rolled back\n");
            }
            failures++;
        }
        stmt_free(&st);
    }
    links_flush(db);
    db_flush_catalog(db);
    return failures;
}
