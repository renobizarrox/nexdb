/* select.c - the SELECT pipeline.
 *
 * A query runs as a series of stages, each one handing a list of values to the
 * next:
 *
 *   scan + WHERE  ->  group (if grouping)  ->  project  ->  DISTINCT
 *                 ->  ORDER BY  ->  TOP  ->  print  ->  reinforce
 *
 * Grouping is the only stage that changes the shape of the data, so it is kept
 * separate: when a query has aggregates or a GROUP BY, rows are folded into
 * groups first and the select list is then evaluated once per group with the
 * aggregate results made available to it.
 */
#define _GNU_SOURCE
#include "nexdb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>

/* Capture all printf through g_output_file so the TCP server can intercept text output. */
#undef printf
#define printf(...)   fprintf(g_output_file ? g_output_file : stdout, __VA_ARGS__)

/* When non-NULL, exec_select_into copies SELECT results to this capture
 * in addition to printing them (used by the TCP server for JSON output). */
__thread Capture *g_select_capture = NULL;
__thread Capture *g_pending_capture = NULL;

/* Set-operation ORDER BY support: the comparator resolves ORDER BY
 * expressions against the merged output columns. */
static __thread Capture *g_set_cap;
static __thread Stmt    *g_set_stmt;

/* ---------------------------------------------------------------- join help */

typedef struct {
    Table *table;
    char   alias[MAX_NAME];
    Row    row;
    int    matched;
} JoinTab;

static int join_tab_idx(JoinTab *tabs, int ntabs, const char *name)
{
    for (int i = 0; i < ntabs; i++) {
        if (strcasecmp(name, tabs[i].table->name) == 0 ||
            strcasecmp(name, tabs[i].alias) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------- aggregate state */

/* ---- full-text (GIN) candidate selection -------------------------------- */

/* Growing list of row refs harvested from a GIN posting list. */
typedef struct {
    RowRef *refs;
    int     n, cap;
    int     oom;
} RefSet;

static int ref_push(void *ctx, RowRef ref)
{
    RefSet *rs = ctx;
    if (rs->n == rs->cap) {
        int cap = rs->cap ? rs->cap * 2 : 64;
        RowRef *nr = realloc(rs->refs, sizeof(RowRef) * (size_t)cap);
        if (!nr) { rs->oom = 1; return 1; }
        rs->refs = nr;
        rs->cap = cap;
    }
    rs->refs[rs->n++] = ref;
    return 0;
}

/* Walk an AND chain looking for a full-text match conjunct over a GIN-
 * indexed column: to_tsvector(col) @@ 'query'. When found, extracts the
 * query's first positive lexeme into *term_out and the column into
 * *col_out. Returns 1 if the index can narrow the scan, 0 otherwise. */
static int gin_pick(DB *db, const Stmt *s, const Table *t, const Expr *e,
                    int *col_out, Value *term_out)
{
    (void)db;
    if (e->kind != EX_BIN) return 0;
    if (e->op == OP_AND)
        return gin_pick(db, s, t, e->l, col_out, term_out) ||
               gin_pick(db, s, t, e->r, col_out, term_out);
    if (e->op != OP_MATCHES) return 0;
    const Expr *call = e->l, *lit = e->r;
    if (call->kind != EX_FUNC || lit->kind != EX_LIT) return 0;
    if (strcasecmp(call->fname, "TO_TSVECTOR") != 0 || call->nargs != 1 ||
        call->args[0]->kind != EX_COL)
        return 0;
    if (lit->lit.tag != T_TEXT || !lit->lit.s) return 0;
    const char *qt = call->args[0]->col_table;
    if (qt[0] && strcasecmp(qt, t->name) != 0 &&
        strcasecmp(qt, s->alias) != 0)
        return 0;
    int col = table_col_index(t, call->args[0]->col);
    if (col < 0) return 0;
    int idx = table_find_index(t, col);
    if (idx < 0 || !t->indexes[idx].valid || !t->indexes[idx].gin) return 0;
    char terms[16][64];
    char err2[MAX_ERR];
    int n = fulltext_query_terms(lit->lit.s, terms, 16, err2);
    if (n <= 0) return 0;        /* nothing positive: the index cannot narrow */
    *col_out = col;
    *term_out = val_text(terms[0]);
    return 1;
}

typedef struct {
    AggKind kind;
    int64_t n;          /* rows counted                */
    double  sum;
    int     any;        /* saw at least one non-NULL   */
    int     is_float;   /* SUM/AVG should stay integral */
    Value   best;       /* MIN / MAX                   */
    int     distinct;   /* DISTINCT aggregate          */
    Value  *seen;       /* heap-allocated distinct values */
    int     nseen, cseen;
} AggAcc;

/* Reset aggregate accumulator state for one grouping pass. */
static void agg_init(AggAcc *a, AggKind k)
{
    memset(a, 0, sizeof *a);
    a->kind = k;
    a->best = val_null();
}

/* Free distinct-values storage within an AggAcc. */
static void agg_cleanup(AggAcc *a)
{
    if (a->seen) {
        for (int i = 0; i < a->nseen; i++) val_clear(&a->seen[i]);
        free(a->seen);
        a->seen = NULL;
        a->nseen = a->cseen = 0;
    }
}

/* Feed one row's value into a running aggregate; returns -1 on type errors. */
static int agg_update(AggAcc *a, const Value *v, char *err)
{
    /* DISTINCT: skip values already seen */
    if (a->distinct && v && v->tag != T_NULL) {
        for (int i = 0; i < a->nseen; i++) {
            int ok;
            if (val_compare(v, &a->seen[i], &ok) == 0 && ok == 1)
                return 0;  /* already seen, skip */
        }
        /* add to seen set */
        if (a->nseen >= a->cseen) {
            int nc = a->cseen ? a->cseen * 2 : 64;
            Value *ns = realloc(a->seen, (size_t)nc * sizeof(Value));
            if (!ns) { snprintf(err, MAX_ERR, "out of memory"); return -1; }
            a->seen = ns;
            a->cseen = nc;
        }
        a->seen[a->nseen++] = val_copy(v);
    }

    if (a->kind == AGG_COUNT) {
        if (v == NULL || v->tag != T_NULL) a->n++;   /* NULL passed for COUNT(*) */
        return 0;
    }
    if (!v || v->tag == T_NULL) return 0;            /* aggregates skip NULLs */

    switch (a->kind) {
    case AGG_SUM:
    case AGG_AVG: {
        double d;
        if (v->tag == T_INT || v->tag == T_BIT) {
            d = (double)v->i;
        } else if (v->tag == T_FLOAT) {
            d = v->f;
            a->is_float = 1;
        } else if (!text_to_number(v->s, &d, NULL, NULL)) {
            snprintf(err, MAX_ERR,
                     "%s needs numbers, and '%s' is not one",
                     a->kind == AGG_SUM ? "SUM" : "AVG", v->s ? v->s : "");
            return -1;
        } else {
            a->is_float = 1;
        }
        a->sum += d;
        a->n++;
        a->any = 1;
        return 0;
    }
    case AGG_MIN:
    case AGG_MAX: {
        if (!a->any) {
            a->best = val_copy(v);
            a->any = 1;
            a->n++;
            return 0;
        }
        int ok;
        int c = val_compare(v, &a->best, &ok);
        if (ok == -1) {
            snprintf(err, MAX_ERR, "%s cannot compare these values",
                     a->kind == AGG_MIN ? "MIN" : "MAX");
            return -1;
        }
        if (ok == 1 && ((a->kind == AGG_MIN && c < 0) ||
                        (a->kind == AGG_MAX && c > 0))) {
            val_clear(&a->best);
            a->best = val_copy(v);
        }
        a->n++;
        return 0;
    }
    default:
        return 0;
    }
}

static Value agg_result(const AggAcc *a)
{
    switch (a->kind) {
    case AGG_COUNT: return val_int(a->n);
    case AGG_SUM:
        if (!a->any) return val_null();
        return a->is_float ? val_float(a->sum) : val_int((int64_t)a->sum);
    case AGG_AVG:
        if (!a->any || a->n == 0) return val_null();
        return val_float(a->sum / (double)a->n);
    case AGG_MIN:
    case AGG_MAX: return val_copy(&a->best);
    default:      return val_null();
    }
}

/* --------------------------------------------------- collecting aggregates */

/* Walk an expression tree and register every aggregate node, assigning each a
 * slot so the projector can find its value later. */
static int collect_aggs(Expr *e, Expr **slots, int *n, int max, char *err)
{
    if (!e) return 0;
    if (e->kind == EX_AGG) {
        if (e->nargs && e->args[0]) {
            /* an aggregate of an aggregate is meaningless */
            Expr *inner[8];
            int k = 0;
            if (collect_aggs(e->args[0], inner, &k, 8, err) < 0) return -1;
            if (k > 0) {
                snprintf(err, MAX_ERR, "%s cannot contain another aggregate",
                         e->fname);
                return -1;
            }
        }
        if (*n >= max) {
            snprintf(err, MAX_ERR, "too many aggregates in one query");
            return -1;
        }
        e->agg_slot = *n;
        slots[(*n)++] = e;
        return 0;
    }
    if (collect_aggs(e->l, slots, n, max, err) < 0) return -1;
    if (collect_aggs(e->r, slots, n, max, err) < 0) return -1;
    for (int i = 0; i < e->nitems; i++)
        if (collect_aggs(e->items[i], slots, n, max, err) < 0) return -1;
    for (int i = 0; i < e->nargs; i++)
        if (collect_aggs(e->args[i], slots, n, max, err) < 0) return -1;
    return 0;
}

/* Does the tree contain any aggregate at all? */
static int expr_has_agg(const Expr *e)
{
    if (!e) return 0;
    if (e->kind == EX_AGG) return 1;
    if (expr_has_agg(e->l) || expr_has_agg(e->r)) return 1;
    for (int i = 0; i < e->nitems; i++)
        if (expr_has_agg(e->items[i])) return 1;
    for (int i = 0; i < e->nargs; i++)
        if (expr_has_agg(e->args[i])) return 1;
    return 0;
}

/* Is `e` structurally one of the GROUP BY keys? Compared by shape, so
 * "GROUP BY topic" satisfies "SELECT topic" and "GROUP BY LEN(s)" satisfies
 * "SELECT LEN(s)". */
static int same_expr(const Expr *a, const Expr *b)
{
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case EX_COL: return strcasecmp(a->col, b->col) == 0;
    case EX_LIT: {
        int ok;
        return val_compare(&a->lit, &b->lit, &ok) == 0 && ok != -1;
    }
    case EX_FUNC:
    case EX_AGG:
        if (strcasecmp(a->fname, b->fname) != 0) return 0;
        break;
    case EX_BIN:
        if (a->op != b->op) return 0;
        break;
    default:
        break;
    }
    if (a->nargs != b->nargs || a->nitems != b->nitems) return 0;
    if (!same_expr(a->l, b->l) || !same_expr(a->r, b->r)) return 0;
    for (int i = 0; i < a->nargs; i++)
        if (!same_expr(a->args[i], b->args[i])) return 0;
    for (int i = 0; i < a->nitems; i++)
        if (!same_expr(a->items[i], b->items[i])) return 0;
    return 1;
}

static int expr_is_grouped(const Expr *e, Stmt *s)
{
    for (int i = 0; i < s->ngroup; i++)
        if (same_expr(e, s->group[i])) return 1;
    return 0;
}

/* Every bare column in a grouped select list must itself be a grouping key. */
static int check_grouped(const Expr *e, Stmt *s, char *err)
{
    if (!e) return 0;
    if (e->kind == EX_AGG) return 0;
    if (expr_is_grouped(e, s)) return 0;
    if (e->kind == EX_COL) {
        if (pseudo_col_index(e->col) != -1) return 0;   /* memory columns are fine */
        snprintf(err, MAX_ERR,
                 "'%s' must appear in GROUP BY or inside an aggregate, because "
                 "the query groups rows together", e->col);
        return -1;
    }
    if (check_grouped(e->l, s, err) < 0) return -1;
    if (check_grouped(e->r, s, err) < 0) return -1;
    for (int i = 0; i < e->nitems; i++)
        if (check_grouped(e->items[i], s, err) < 0) return -1;
    for (int i = 0; i < e->nargs; i++)
        if (check_grouped(e->args[i], s, err) < 0) return -1;
    return 0;
}

/* Every column a GROUP BY key names must exist in the FROM table.  Column refs
 * are checked up front so that "GROUP BY nosuchcol" is reported as an unknown
 * column instead of surfacing later (or being masked by a "must appear in
 * GROUP BY" error that mentions the select list instead).  Returns 1 if a
 * subquery is encountered (its columns belong to a different scope and are
 * left for runtime evaluation), -1 on an unknown column, else 0. */
static int group_cols_exist(const Expr *e, const Table *t, char *err)
{
    if (!e) return 0;
    if (e->kind == EX_SUBQUERY) return 1;
    if (e->kind == EX_COL) {
        if (e->col_table[0]) {
            if (strcasecmp(e->col_table, t->name) != 0 ||
                table_col_index(t, e->col) < 0) {
                snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                         e->col, e->col_table[0] ? e->col_table : t->name);
                return -1;
            }
            return 0;
        }
        if (pseudo_col_index(e->col) != -1) return 0;
        if (table_col_index(t, e->col) < 0) {
            snprintf(err, MAX_ERR, "no column '%s' in table '%s'",
                     e->col, t->name);
            return -1;
        }
        return 0;
    }
    if (e->l) {
        int rc = group_cols_exist(e->l, t, err);
        if (rc) return rc;
    }
    if (e->r) {
        int rc = group_cols_exist(e->r, t, err);
        if (rc) return rc;
    }
    for (int i = 0; i < e->nitems; i++) {
        int rc = group_cols_exist(e->items[i], t, err);
        if (rc) return rc;
    }
    for (int i = 0; i < e->nargs; i++) {
        int rc = group_cols_exist(e->args[i], t, err);
        if (rc) return rc;
    }
    return 0;
}

/* ------------------------------------------------------------ result set */

typedef struct {
    Value   *cells;    /* nrows * ncols */
    Value   *keys;     /* nrows * nkeys */
    RowRef  *refs;
    uint64_t *rids;
    int      nrows, ncols, nkeys, cap;
} Res;

static void res_init(Res *r, int ncols, int nkeys)
{
    memset(r, 0, sizeof *r);
    r->ncols = ncols;
    r->nkeys = nkeys;
}

static int res_grow(Res *r)
{
    if (r->nrows < r->cap) return 0;
    int cap = r->cap ? r->cap * 2 : 32;
    Value *c = realloc(r->cells, sizeof(Value) * (size_t)cap * (size_t)(r->ncols ? r->ncols : 1));
    if (!c) return -1;
    r->cells = c;
    if (r->nkeys) {
        Value *k = realloc(r->keys, sizeof(Value) * (size_t)cap * (size_t)r->nkeys);
        if (!k) return -1;
        r->keys = k;
    }
    RowRef *rf = realloc(r->refs, sizeof(RowRef) * (size_t)cap);
    if (!rf) return -1;
    r->refs = rf;
    uint64_t *ri = realloc(r->rids, sizeof(uint64_t) * (size_t)cap);
    if (!ri) return -1;
    r->rids = ri;
    r->cap = cap;
    return 0;
}

static void res_free(Res *r)
{
    for (int i = 0; i < r->nrows; i++) {
        for (int c = 0; c < r->ncols; c++) val_clear(&r->cells[i * r->ncols + c]);
        for (int k = 0; k < r->nkeys; k++) val_clear(&r->keys[i * r->nkeys + k]);
    }
    free(r->cells);
    free(r->keys);
    free(r->refs);
    free(r->rids);
    memset(r, 0, sizeof *r);
}

/* sort context */
static __thread Res *g_res;
static __thread Stmt *g_stmt;

static int res_cmp(const void *xa, const void *xb)
{
    int ia = *(const int *)xa, ib = *(const int *)xb;
    for (int k = 0; k < g_res->nkeys; k++) {
        const Value *a = &g_res->keys[ia * g_res->nkeys + k];
        const Value *b = &g_res->keys[ib * g_res->nkeys + k];
        int ok;
        int c = val_compare(a, b, &ok);
        if (ok != 1) {
            int an = (a->tag == T_NULL), bn = (b->tag == T_NULL);
            if (an || bn) c = (an == bn) ? 0 : (an ? -1 : 1);
            else {
                char ab[128], bb[128];
                val_format(a, ab, sizeof ab);
                val_format(b, bb, sizeof bb);
                c = strcasecmp(ab, bb);
            }
        }
        if (c) return g_stmt->order[k].desc ? -c : c;
    }
    return ia - ib;      /* stable */
}

/* -------------------------------------------------------------- grouping */

typedef struct {
    char    key[512];
    Row     rep;             /* a representative row, for grouped columns */
    AggAcc  accs[MAX_AGGS];
    RowRef  refs[MEM_COACT_MAX];
    uint64_t rids[MEM_COACT_MAX];
    int     nrefs;
} Group;

/* --------------------------------------------------------------- helpers */

/* Expand '*' and work out how many output columns there will be. */
static int output_columns(Stmt *s, Table *t, char heads[][MAX_NAME],
                          Expr **exprs, int max, char *err)
{
    int n = 0;
    for (int i = 0; i < s->nitems; i++) {
        SelItem *it = &s->items[i];
        if (it->is_star) {
            for (int c = 0; c < t->ncols; c++) {
                if (n >= max) { snprintf(err, MAX_ERR, "too many output columns"); return -1; }
                exprs[n] = NULL;                  /* NULL means "column c" */
                snprintf(heads[n], MAX_NAME, "%s", t->cols[c].name);
                n++;
            }
        } else {
            if (n >= max) { snprintf(err, MAX_ERR, "too many output columns"); return -1; }
            exprs[n] = it->e;
            snprintf(heads[n], MAX_NAME, "%s",
                     it->alias[0] ? it->alias : it->label);
            n++;
        }
    }
    return n;
}

/* star columns are positional, so track which table column each maps to */
static void star_map(Stmt *s, Table *t, int *map, int ncols)
{
    int n = 0;
    for (int i = 0; i < s->nitems && n < ncols; i++) {
        if (s->items[i].is_star) {
            for (int c = 0; c < t->ncols && n < ncols; c++) map[n++] = c;
        } else {
            map[n++] = -1;
        }
    }
}

/* For join queries: track (table_idx, col_idx) for each star output column. */
typedef struct { int tab; int col; } ColSrc;

/* Expand '*' and 'table.*' across multiple tables for joins. */
static int output_columns_join(Stmt *s, JoinTab *tabs, int ntabs,
                               char heads[][MAX_NAME], Expr **exprs,
                               ColSrc *colsrc, int max, char *err)
{
    int n = 0;
    for (int i = 0; i < s->nitems; i++) {
        SelItem *it = &s->items[i];
        if (it->is_star) {
            for (int ti = 0; ti < ntabs; ti++) {
                Table *t = tabs[ti].table;
                for (int c = 0; c < t->ncols; c++) {
                    if (n >= max) { snprintf(err, MAX_ERR, "too many output columns"); return -1; }
                    exprs[n] = NULL;
                    colsrc[n].tab = ti;
                    colsrc[n].col = c;
                    snprintf(heads[n], MAX_NAME, "%s", t->cols[c].name);
                    n++;
                }
            }
        } else if (it->col_star) {
            int ti = join_tab_idx(tabs, ntabs, it->col_table);
            if (ti < 0) {
                snprintf(err, MAX_ERR, "unknown table '%s' in t.*", it->col_table);
                return -1;
            }
            Table *t = tabs[ti].table;
            for (int c = 0; c < t->ncols; c++) {
                if (n >= max) { snprintf(err, MAX_ERR, "too many output columns"); return -1; }
                exprs[n] = NULL;
                colsrc[n].tab = ti;
                colsrc[n].col = c;
                snprintf(heads[n], MAX_NAME, "%s", t->cols[c].name);
                n++;
            }
        } else {
            if (n >= max) { snprintf(err, MAX_ERR, "too many output columns"); return -1; }
            exprs[n] = it->e;
            colsrc[n].tab = -1;
            colsrc[n].col = -1;
            snprintf(heads[n], MAX_NAME, "%s",
                     it->alias[0] ? it->alias : it->label);
            n++;
        }
    }
    return n;
}

void capture_free(Capture *c)
{
    for (int i = 0; i < c->nrows * c->ncols; i++) val_clear(&c->cells[i]);
    free(c->cells);
    memset(c, 0, sizeof *c);
}

static int capture_push(Capture *c, const Value *row, int ncols)
{
    if (c->nrows == c->cap) {
        int cap = c->cap ? c->cap * 2 : 32;
        Value *nc = realloc(c->cells, sizeof(Value) * (size_t)cap * (size_t)ncols);
        if (!nc) return -1;
        c->cells = nc;
        c->cap = cap;
    }
    c->ncols = ncols;
    for (int i = 0; i < ncols; i++)
        c->cells[c->nrows * ncols + i] = val_copy(&row[i]);
    c->nrows++;
    return 0;
}

/* Public entry: run a SELECT and print results to stdout. */
int exec_select(DB *db, Stmt *s, char *err)
{
    if (g_pending_capture) {
        /* INSERT ... EXEC: the first SELECT of the CALLed procedure hands
         * its rows to the capture instead of printing them */
        Capture *cap = g_pending_capture;
        g_pending_capture = NULL;
        return exec_select_into(db, s, cap, err);
    }
    return exec_select_into(db, s, NULL, err);
}

/* ------------------------------------------------- set operations */

static int exec_select_core(DB *db, Stmt *s, Capture *capture, char *err);

/* Evaluate one ORDER BY expression of a compound query against the output
 * columns of the merged result: by column name, by position, or by evaluating
 * the expression against a virtual row of output values. */
static int set_eval_order(const Expr *e, int rowidx, Value *out, char *err)
{
    if (e->kind == EX_COL) {
        for (int i = 0; i < g_set_cap->ncols; i++)
            if (strcasecmp(g_set_cap->colnames[i], e->col) == 0) {
                *out = val_copy(&g_set_cap->cells[rowidx * g_set_cap->ncols + i]);
                return 0;
            }
        snprintf(err, MAX_ERR, "no column '%s' in the set operation result",
                 e->col);
        return -1;
    }
    if (e->kind == EX_LIT && e->lit.tag == T_INT) {
        int n = (int)e->lit.i;
        if (n >= 1 && n <= g_set_cap->ncols) {
            *out = val_copy(&g_set_cap->cells[rowidx * g_set_cap->ncols + n - 1]);
            return 0;
        }
        snprintf(err, MAX_ERR, "ORDER BY position %d is out of range", n);
        return -1;
    }
    Table vtbl;
    memset(&vtbl, 0, sizeof vtbl);
    vtbl.ncols = g_set_cap->ncols;
    for (int i = 0; i < g_set_cap->ncols && i < MAX_COLS; i++)
        snprintf(vtbl.cols[i].name, MAX_NAME, "%s", g_set_cap->colnames[i]);
    Row row;
    memset(&row, 0, sizeof row);
    row.ncols = g_set_cap->ncols;
    for (int i = 0; i < g_set_cap->ncols; i++)
        row.v[i] = val_copy(&g_set_cap->cells[rowidx * g_set_cap->ncols + i]);
    int rc = eval_expr(e, &row, &vtbl, mem_now(), out, err);
    for (int i = 0; i < row.ncols; i++) val_clear(&row.v[i]);
    return rc;
}

static int set_cmp(const void *xa, const void *xb)
{
    int ia = *(const int *)xa, ib = *(const int *)xb;
    for (int k = 0; k < g_set_stmt->norder; k++) {
        char err[MAX_ERR];
        Value va, vb;
        if (set_eval_order(g_set_stmt->order[k].e, ia, &va, err) < 0 ||
            set_eval_order(g_set_stmt->order[k].e, ib, &vb, err) < 0) {
            return 0;
        }
        int ok;
        int c = val_compare(&va, &vb, &ok);
        if (ok != 1) c = 0;
        val_clear(&va);
        val_clear(&vb);
        if (c) return g_set_stmt->order[k].desc ? -c : c;
    }
    return 0;
}

static int cap_row_equal(const Capture *a, int i, const Capture *b, int j)
{
    for (int c = 0; c < a->ncols; c++) {
        const Value *x = &a->cells[i * a->ncols + c];
        const Value *y = &b->cells[j * b->ncols + c];
        if (x->tag == T_NULL && y->tag == T_NULL) continue;   /* set semantics: NULL = NULL */
        if (x->tag == T_NULL || y->tag == T_NULL) return 0;
        int ok;
        if (val_compare(x, y, &ok) != 0 || !ok) return 0;
    }
    return 1;
}

/* Does row i of `c` appear anywhere in `d`? */
static int cap_contains(const Capture *c, int i, const Capture *d)
{
    for (int j = 0; j < d->nrows; j++)
        if (cap_row_equal(c, i, d, j)) return 1;
    return 0;
}

/* Drop duplicate rows from `c` in place, keeping the first occurrence. */
static void cap_dedupe(Capture *c)
{
    int out = 0;
    for (int i = 0; i < c->nrows; i++) {
        int dup = 0;
        for (int j = 0; j < out; j++)
            if (cap_row_equal(c, i, c, j)) { dup = 1; break; }
        if (dup) {
            for (int k = 0; k < c->ncols; k++) val_clear(&c->cells[i * c->ncols + k]);
            continue;
        }
        if (out != i) {
            for (int k = 0; k < c->ncols; k++)
                c->cells[out * c->ncols + k] = c->cells[i * c->ncols + k];
        }
        out++;
    }
    c->nrows = out;
}

/* Filter `a` in place: keep rows that appear in `b` when keep=1, or rows that
 * do not appear in `b` when keep=0. */
static void cap_filter(Capture *a, const Capture *b, int keep)
{
    int out = 0;
    for (int i = 0; i < a->nrows; i++) {
        int in_b = cap_contains(a, i, b);
        if (in_b != keep) {
            for (int k = 0; k < a->ncols; k++) val_clear(&a->cells[i * a->ncols + k]);
            continue;
        }
        if (out != i) {
            for (int k = 0; k < a->ncols; k++)
                a->cells[out * a->ncols + k] = a->cells[i * a->ncols + k];
        }
        out++;
    }
    a->nrows = out;
}

/* Merge two operand captures according to the set operator.  The result
 * replaces `a`; `b` is left untouched (the caller frees it). */
static void cap_merge(Capture *a, const Capture *b, int op)
{
    if (op == SET_UNION_ALL) {
        for (int i = 0; i < b->nrows; i++)
            capture_push(a, &b->cells[i * b->ncols], b->ncols);
        return;
    }
    if (op == SET_UNION) {
        for (int i = 0; i < b->nrows; i++)
            if (!cap_contains(b, i, a))
                capture_push(a, &b->cells[i * b->ncols], b->ncols);
        cap_dedupe(a);
        return;
    }
    if (op == SET_INTERSECT) {
        cap_filter(a, b, 1);
        cap_dedupe(a);
        return;
    }
    cap_filter(a, b, 0);   /* EXCEPT */
    cap_dedupe(a);
}

/* Execute a compound SELECT (UNION/INTERSECT/EXCEPT) into `capture` or the
 * printer.  `s` is the leftmost operand, carrying set_op/set_rhs plus any
 * trailing ORDER BY / LIMIT / OFFSET that bind to the whole result. */
static int exec_set_op(DB *db, Stmt *s, Capture *capture, char *err)
{
    /* Flatten the operator chain: operand[i] hangs off operand[i-1] via
     * set_rhs, and operand[i]->set_op is the operator that follows it.  The
     * chain is reduced left to right, so a UNION b INTERSECT c means
     * (a UNION b) INTERSECT c. */
    Stmt *operands[65];
    int   opcodes[64];
    int   n = 0;
    operands[0] = s;
    for (Stmt *p = s; p->set_op; p = p->set_rhs) {
        if (n >= 64) {
            snprintf(err, MAX_ERR, "too many set operations in one query");
            return -1;
        }
        opcodes[n] = p->set_op;
        operands[n + 1] = p->set_rhs;
        n++;
        if (!p->set_rhs) {
            snprintf(err, MAX_ERR, "malformed set operation chain");
            return -1;
        }
    }

    Capture a;
    memset(&a, 0, sizeof a);
    /* The root statement is both an operand and the holder of the trailing
     * ORDER BY / LIMIT / OFFSET that apply to the compound result.  Those
     * must not be applied to the operand itself, so they are suspended for
     * the duration of its evaluation. */
    int save_limit = s->limit, save_offset = s->offset, save_norder = s->norder;
    s->limit = -1;
    s->offset = 0;
    s->norder = 0;
    int rc = exec_select_core(db, operands[0], &a, err);
    s->limit = save_limit;
    s->offset = save_offset;
    s->norder = save_norder;
    if (rc < 0) return -1;

    for (int i = 0; i < n; i++) {
        Capture b;
        memset(&b, 0, sizeof b);
        if (exec_select_core(db, operands[i + 1], &b, err) < 0) {
            capture_free(&a);
            return -1;
        }
        if (a.ncols != b.ncols) {
            snprintf(err, MAX_ERR,
                     "the two sides of a set operation return different "
                     "numbers of columns (%d vs %d)", a.ncols, b.ncols);
            capture_free(&a);
            capture_free(&b);
            return -1;
        }
        cap_merge(&a, &b, opcodes[i]);
        capture_free(&b);
    }

    /* ORDER BY over the merged result: expressions are resolved against the
     * output columns (by name), the way SQL defines compound ordering. */
    if (s->norder && a.nrows > 1) {
        int *order = malloc(sizeof(int) * (size_t)a.nrows);
        if (!order) { capture_free(&a); snprintf(err, MAX_ERR, "out of memory"); return -1; }
        for (int i = 0; i < a.nrows; i++) order[i] = i;
        for (int k = 0; k < s->norder; k++) {
            if (expr_has_agg(s->order[k].e)) {
                free(order);
                capture_free(&a);
                snprintf(err, MAX_ERR,
                         "aggregates are not allowed in ORDER BY of a set "
                         "operation");
                return -1;
            }
        }
        g_set_cap = &a;
        g_set_stmt = s;
        qsort(order, (size_t)a.nrows, sizeof(int), set_cmp);
        g_set_cap = NULL;
        /* apply the permutation in place */
        Value *tmp = malloc(sizeof(Value) * (size_t)a.ncols);
        if (!tmp) { free(order); capture_free(&a); snprintf(err, MAX_ERR, "out of memory"); return -1; }
        for (int i = 0; i < a.nrows; i++) {
            while (order[i] != i) {
                int j = order[i];
                for (int c = 0; c < a.ncols; c++) {
                    Value t = a.cells[i * a.ncols + c];
                    a.cells[i * a.ncols + c] = a.cells[j * a.ncols + c];
                    a.cells[j * a.ncols + c] = t;
                }
                int oi = order[i];
                order[i] = order[j];
                order[j] = oi;
            }
        }
        free(tmp);
        free(order);
    }

    /* LIMIT / OFFSET window over the ordered result */
    int lo = s->offset > 0 ? s->offset : 0;
    if (lo > a.nrows) lo = a.nrows;
    int hi = a.nrows;
    if (s->limit >= 0 && lo + s->limit < hi) hi = lo + s->limit;

    if (capture) {
        capture->ncols = a.ncols;
        for (int c = 0; c < a.ncols && c < MAX_OUT_COLS; c++)
            snprintf(capture->colnames[c], MAX_NAME, "%s", a.colnames[c]);
        for (int i = lo; i < hi; i++)
            if (capture_push(capture, &a.cells[i * a.ncols], a.ncols) < 0) {
                capture_free(&a);
                snprintf(err, MAX_ERR, "out of memory");
                return -1;
            }
        capture_free(&a);
        return 0;
    }

    /* print */
    Grid g = {0};
    grid_init(&g, a.ncols);
    for (int c = 0; c < a.ncols; c++)
        snprintf(g.head[c], MAX_NAME, "%s", a.colnames[c]);
    for (int i = lo; i < hi; i++) {
        char *cells[MAX_OUT_COLS];
        for (int c = 0; c < a.ncols; c++) {
            char buf[512];
            val_format(&a.cells[i * a.ncols + c], buf, sizeof buf);
            cells[c] = strdup(buf);
        }
        grid_row(&g, cells);
    }
    if (g.nrows) grid_print(&g);
    printf("\n(%d row%s)\n", g.nrows, g.nrows == 1 ? "" : "s");

    if (g_select_capture) {
        g_select_capture->ncols = g.ncols;
        for (int c = 0; c < g.ncols; c++)
            snprintf(g_select_capture->colnames[c], MAX_NAME, "%s", g.head[c]);
        for (int r = 0; r < g.nrows; r++) {
            Value row[MAX_OUT_COLS];
            for (int c = 0; c < g.ncols; c++)
                row[c] = g.cells[r * g.ncols + c]
                             ? val_text(g.cells[r * g.ncols + c])
                             : val_null();
            capture_push(g_select_capture, row, g.ncols);
            for (int c = 0; c < g.ncols; c++) val_clear(&row[c]);
        }
    }
    grid_free(&g);
    capture_free(&a);
    return 0;
}

/* Full SELECT pipeline; optionally capture rows into cap instead of printing. */
int exec_select_into(DB *db, Stmt *s, Capture *capture, char *err)
{
    if (s->set_op)
        return exec_set_op(db, s, capture, err);
    return exec_select_core(db, s, capture, err);
}

static int exec_select_core(DB *db, Stmt *s, Capture *capture, char *err)
{
    g_db = db;
    int64_t now = mem_now();
    Table *t = NULL;

    if (s->has_from && !s->derived) {
        t = cat_find(db, s->table);
        if (!t) {
            /* a stored view expands to its SELECT body as a derived table */
            View *v = cat_find_view(db, s->table);
            if (v) {
                Stmt *vs = calloc(1, sizeof(Stmt));
                if (!vs) { snprintf(err, MAX_ERR, "out of memory"); return -1; }
                if (parse_view_sql(v->sql, vs, err) < 0) {
                    char verr[MAX_ERR];
                    snprintf(verr, sizeof verr, "%s", err);
                    free(vs);
                    snprintf(err, MAX_ERR, "view '%s': %s", s->table, verr);
                    return -1;
                }
                s->derived = vs;
            } else {
                snprintf(err, MAX_ERR, "unknown table '%s'", s->table);
                return -1;
            }
        }
    }

    /* ---- collect join tables (if any) */
    if (s->has_from && s->njoins > 0) {
        /* A view in FROM had already been expanded into s->derived; the
         * nested-loop executor never runs joins for derived tables, so
         * refuse instead of silently dropping the JOIN (wrong results). */
        if (s->derived) {
            snprintf(err, MAX_ERR,
                     "a view cannot be used as a JOIN operand (query the view "
                     "on its own)");
            return -1;
        }
        /* Only the first ON clause is ever evaluated; refuse extra joins so
         * they cannot silently disappear (on empty tables or in aggregates). */
        if (s->njoins > 1) {
            snprintf(err, MAX_ERR, "multiple JOINs are not yet supported");
            return -1;
        }
    }
    JoinTab join_tabs[1 + MAX_JOINS];
    int njoin_tabs = 0;
    if (s->has_from && s->njoins > 0) {
        memset(join_tabs, 0, sizeof join_tabs);
        join_tabs[njoin_tabs].table = t;
        snprintf(join_tabs[njoin_tabs].alias, MAX_NAME, "%s",
                 s->alias[0] ? s->alias : s->table);
        njoin_tabs++;
        for (int i = 0; i < s->njoins; i++) {
            Join *j = &s->joins[i];
            Table *jt = cat_find(db, j->table);
            if (!jt) { snprintf(err, MAX_ERR, "unknown table '%s' in JOIN", j->table); return -1; }
            join_tabs[njoin_tabs].table = jt;
            snprintf(join_tabs[njoin_tabs].alias, MAX_NAME, "%s",
                     j->alias[0] ? j->alias : j->table);
            njoin_tabs++;
        }
    }

    /* ---- plan: find the aggregates, decide whether this is a grouped query */
    Expr *aggs[MAX_AGGS];
    int naggs = 0;
    for (int i = 0; i < s->nitems; i++)
        if (s->items[i].e && collect_aggs(s->items[i].e, aggs, &naggs, MAX_AGGS, err) < 0)
            return -1;
    if (s->having && collect_aggs(s->having, aggs, &naggs, MAX_AGGS, err) < 0)
        return -1;
    for (int i = 0; i < s->norder; i++)
        if (collect_aggs(s->order[i].e, aggs, &naggs, MAX_AGGS, err) < 0)
            return -1;

    int grouped = (naggs > 0 || s->ngroup > 0);

    if (s->having && !grouped) {
        if (naggs == 0) {
            snprintf(err, MAX_ERR, "HAVING without GROUP BY needs an aggregate");
            return -1;
        }
        /* HAVING without GROUP BY: treat the entire result as one group */
        grouped = 1;
    }
    if (grouped && !s->has_from) {
        snprintf(err, MAX_ERR, "aggregates need a FROM clause");
        return -1;
    }
    if (grouped) {
        if (t) {
            for (int gi = 0; gi < s->ngroup; gi++) {
                int rc = group_cols_exist(s->group[gi], t, err);
                if (rc < 0) return -1;
                if (rc == 1) break;   /* subquery: leave for the row loop */
            }
        }
        for (int i = 0; i < s->nitems; i++) {
            if (s->items[i].is_star) {
                snprintf(err, MAX_ERR,
                         "SELECT * cannot be combined with grouping; list the "
                         "columns you are grouping by instead");
                return -1;
            }
            if (check_grouped(s->items[i].e, s, err) < 0) return -1;
        }
    }

    /* ---- work out the output shape */
    char heads[MAX_OUT_COLS][MAX_NAME];
    Expr *exprs[MAX_OUT_COLS];
    int colmap[MAX_OUT_COLS];
    ColSrc colsrc[MAX_OUT_COLS];
    int ncols;
    int order_from_col[MAX_ORDER_KEYS];
    Res res;
    memset(&res, 0, sizeof res);

    /* ---- derived table: FROM (SELECT ...) AS t must be handled before
     * output_columns, which needs a valid table pointer. */
    if (s->derived) {
        Capture inner;
        memset(&inner, 0, sizeof inner);
        if (exec_select_into(db, s->derived, &inner, err) < 0) return -1;

        Table vtbl;
        memset(&vtbl, 0, sizeof vtbl);
        snprintf(vtbl.name, MAX_NAME, "%s",
                 s->alias[0] ? s->alias : s->table);
        vtbl.ncols = inner.ncols;
        for (int i = 0; i < inner.ncols && i < MAX_COLS; i++)
            snprintf(vtbl.cols[i].name, MAX_NAME, "%s", inner.colnames[i]);

        ncols = output_columns(s, &vtbl, heads, exprs, MAX_OUT_COLS, err);
        if (ncols < 0) { capture_free(&inner); return -1; }
        star_map(s, &vtbl, colmap, ncols);

        for (int k = 0; k < s->norder; k++) order_from_col[k] = -1;
        for (int k = 0; k < s->norder; k++) {
            OrderKey *oe = &s->order[k];
            if (oe->e && oe->e->kind == EX_COL && !oe->e->col_table[0]) {
                for (int c = 0; c < ncols; c++)
                    if (strcasecmp(heads[c], oe->e->col) == 0)
                        { order_from_col[k] = c; break; }
            }
        }

        Res dres;
        memset(&dres, 0, sizeof dres);
        dres.ncols = ncols;
        dres.nkeys = s->norder;

        for (int ri = 0; ri < inner.nrows; ri++) {
            Row r;
            memset(&r, 0, sizeof r);
            r.ncols = inner.ncols;
            for (int c = 0; c < inner.ncols; c++)
                r.v[c] = val_copy(&inner.cells[ri * inner.ncols + c]);

            int m;
            if (row_matches(s->where, &r, &vtbl, now, &m, err) < 0) {
                row_clear(&r); res_free(&dres); capture_free(&inner); return -1;
            }
            if (!m) { row_clear(&r); continue; }

            if (res_grow(&dres) < 0) {
                row_clear(&r); res_free(&dres); capture_free(&inner);
                snprintf(err, MAX_ERR, "out of memory"); return -1;
            }

            int failed = 0;
            for (int c = 0; c < ncols && !failed; c++) {
                Value *dst = &dres.cells[dres.nrows * ncols + c];
                if (exprs[c]) {
                    if (eval_expr(exprs[c], &r, &vtbl, now, dst, err) < 0) failed = 1;
                } else {
                    int idx = colmap[c];
                    *dst = (idx >= 0 && idx < r.ncols) ? val_copy(&r.v[idx]) : val_null();
                }
            }
            for (int k = 0; k < s->norder && !failed; k++) {
                Value *dst = &dres.keys[dres.nrows * s->norder + k];
                if (order_from_col[k] >= 0)
                    *dst = val_copy(&dres.cells[dres.nrows * ncols + order_from_col[k]]);
                else if (eval_expr(s->order[k].e, &r, &vtbl, now, dst, err) < 0)
                    failed = 1;
            }
            if (failed) { row_clear(&r); res_free(&dres); capture_free(&inner); return -1; }

            dres.refs[dres.nrows].page = 0;
            dres.refs[dres.nrows].slot = 0;
            dres.rids[dres.nrows] = 0;
            dres.nrows++;
            row_clear(&r);
        }

        capture_free(&inner);
        res = dres;
        goto emit;
    }

    if (s->has_from && njoin_tabs > 1) {
        ncols = output_columns_join(s, join_tabs, njoin_tabs,
                                    heads, exprs, colsrc, MAX_OUT_COLS, err);
        if (ncols < 0) return -1;
        memset(colmap, 0, sizeof colmap); /* unused for join path */
    } else if (s->has_from) {
        ncols = output_columns(s, t, heads, exprs, MAX_OUT_COLS, err);
        if (ncols < 0) return -1;
        star_map(s, t, colmap, ncols);
    } else {
        ncols = 0;
        for (int i = 0; i < s->nitems; i++) {
            exprs[ncols] = s->items[i].e;
            snprintf(heads[ncols], MAX_NAME, "%s",
                     s->items[i].alias[0] ? s->items[i].alias : s->items[i].label);
            colmap[ncols] = -1;
            ncols++;
        }
    }

    /* if capturing, save column headings so callers (e.g. derived tables) can
     * inspect them */
    if (capture)
        for (int i = 0; i < ncols && i < MAX_OUT_COLS; i++)
            snprintf(capture->colnames[i], MAX_NAME, "%s", heads[i]);

    /* ORDER BY may name an output alias rather than repeat the expression, as in
     * "SELECT SUM(x) AS total ... ORDER BY total DESC". Resolve those to the
     * output column instead of evaluating them against a row, which is both
     * what SQL means and the only way it can work for an aggregate alias. */
    for (int k = 0; k < s->norder; k++) {
        order_from_col[k] = -1;
        Expr *oe = s->order[k].e;
        if (!oe || oe->kind != EX_COL) continue;
        for (int c = 0; c < ncols; c++) {
            if (strcasecmp(heads[c], oe->col) == 0) { order_from_col[k] = c; break; }
        }
        /* a real column of the table wins over a coincidentally equal heading */
        if (order_from_col[k] >= 0 && s->has_from && !grouped &&
            table_col_index(t, oe->col) >= 0)
            order_from_col[k] = -1;
    }

    if (grouped)
        for (int k = 0; k < s->norder; k++)
            if (order_from_col[k] < 0 && check_grouped(s->order[k].e, s, err) < 0)
                return -1;

    res_init(&res, ncols, s->norder);

    /* ---- SELECT with no FROM: one synthetic row, no scanning */
    if (!s->has_from) {
        Row empty;
        memset(&empty, 0, sizeof empty);
        Table fake;
        memset(&fake, 0, sizeof fake);
        snprintf(fake.name, MAX_NAME, "(none)");

        if (res_grow(&res) < 0) { res_free(&res); snprintf(err, MAX_ERR, "out of memory"); return -1; }
        for (int c = 0; c < ncols; c++) {
            if (eval_expr(exprs[c], &empty, &fake, now,
                          &res.cells[res.nrows * ncols + c], err) < 0) {
                res.nrows++;
                res_free(&res);
                return -1;
            }
        }
        for (int k = 0; k < s->norder; k++) {
            Value *dst = &res.keys[res.nrows * s->norder + k];
            if (order_from_col[k] >= 0) {
                *dst = val_copy(&res.cells[res.nrows * ncols + order_from_col[k]]);
            } else if (eval_expr(s->order[k].e, &empty, &fake, now, dst, err) < 0) {
                res.nrows++;
                res_free(&res);
                return -1;
            }
        }
        res.refs[res.nrows].page = 0;
        res.refs[res.nrows].slot = 0;
        res.rids[res.nrows] = 0;
        res.nrows++;
        goto emit;
    }

    /* ---- try point-lookup via index for "WHERE indexed_col = literal" */
    int point_lookup = 0;
    Value point_key;
    memset(&point_key, 0, sizeof point_key);
    int point_col = -1;
    if (!grouped && s->has_from && njoin_tabs <= 1 && s->where && s->where->kind == EX_BIN &&
        s->where->op == OP_EQ) {
        Expr *l = s->where->l, *r = s->where->r;
        if (l && r && l->kind == EX_COL && r->kind == EX_LIT) {
            point_col = table_col_index(t, l->col);
            if (point_col >= 0) {
                int idx = table_find_index(t, point_col);
                if (idx >= 0 && t->indexes[idx].valid) {
                    point_key = val_copy(&r->lit);
                    point_lookup = 1;
                }
            }
        } else if (l && r && l->kind == EX_LIT && r->kind == EX_COL) {
            point_col = table_col_index(t, r->col);
            if (point_col >= 0) {
                int idx = table_find_index(t, point_col);
                if (idx >= 0 && t->indexes[idx].valid) {
                    point_key = val_copy(&l->lit);
                    point_lookup = 1;
                }
            }
        }
    }

    /* ---- try a GIN full-text lookup for "WHERE to_tsvector(col) @@ 'q'".
     * Only the candidate row refs come from the index; the whole WHERE is
     * still re-applied as the filter, so AND chains around the match are
     * handled correctly and a lossy posting list can never drop a match. */
    int gin_lookup = 0;
    Value gin_key;
    memset(&gin_key, 0, sizeof gin_key);
    int gin_col = -1;
    if (!grouped && s->has_from && njoin_tabs <= 1 && s->where) {
        if (gin_pick(db, s, t, s->where, &gin_col, &gin_key)) gin_lookup = 1;
    }

    /* ---- scan, filter, and either project directly or fold into groups */
    if (njoin_tabs > 1) {
        /* ---- nested-loop join execution */
        if (grouped && s->ngroup > 0) {
            snprintf(err, MAX_ERR, "GROUP BY with JOIN is not yet supported");
            res_free(&res); return -1;
        }

        /* Aliases array for join context */
        const char *alias_ptrs[1 + MAX_JOINS];
        for (int i = 0; i < njoin_tabs; i++) alias_ptrs[i] = join_tabs[i].alias;

        if (grouped) {
            /* Aggregate-only (no GROUP BY) join: fold all joined rows into
             * a single aggregate group. */
            AggAcc jaccs[MAX_AGGS];
            memset(jaccs, 0, sizeof jaccs);
            for (int ai = 0; ai < naggs; ai++) {
                agg_init(&jaccs[ai], (AggKind)aggs[ai]->agg);
                jaccs[ai].distinct = aggs[ai]->agg_distinct;
            }

            /* Prepare scan for join table */
            Scan jsc1;
            scan_init(&jsc1, db, join_tabs[1].table);
            Scan psc1;
            Row pr1;
            scan_init(&psc1, db, t);

            while (scan_next(&psc1, &pr1)) {
                /* WHERE is applied per joined row below, so it can reference
                 * columns from either side of the join */

                /* LEFT JOIN: always get at least one row per left row */
                int any_match = 0;
                Row jr1;
                /* Reset join scan */
                scan_init(&jsc1, db, join_tabs[1].table);

                while (scan_next(&jsc1, &jr1)) {
                    int on_ok;
                    const Table *jtabs[2] = { t, join_tabs[1].table };
                    const Row *jrows[2] = { &pr1, &jr1 };
                    if (row_matches_join(s->joins[0].on, jtabs, jrows,
                                         alias_ptrs, 2, now, &on_ok, err) < 0) {
                        row_clear(&jr1); row_clear(&pr1); res_free(&res); return -1;
                    }
                    if (!on_ok) { row_clear(&jr1); continue; }

                    /* WHERE applies to the joined row, so joined columns can
                     * be referenced in it */
                    int w_ok;
                    if (row_matches_join(s->where, jtabs, jrows, alias_ptrs, 2,
                                         now, &w_ok, err) < 0) {
                        row_clear(&jr1); row_clear(&pr1); res_free(&res); return -1;
                    }
                    if (!w_ok) { row_clear(&jr1); continue; }
                    any_match = 1;

                    /* Feed join result into aggregates */
                    const Row *eval_rows[2] = { &pr1, &jr1 };
                    exec_set_join_ctx(jtabs, eval_rows, alias_ptrs, 2);
                    for (int ai = 0; ai < naggs; ai++) {
                        if (aggs[ai]->agg_star) {
                            if (agg_update(&jaccs[ai], NULL, err) < 0) {
                                exec_set_join_ctx(NULL, NULL, NULL, 0);
                                row_clear(&jr1); row_clear(&pr1); res_free(&res);
                                return -1;
                            }
                        } else {
                            Value av;
                            if (eval_expr(aggs[ai]->args[0], &pr1, t, now, &av, err) < 0) {
                                exec_set_join_ctx(NULL, NULL, NULL, 0);
                                row_clear(&jr1); row_clear(&pr1); res_free(&res);
                                return -1;
                            }
                            if (agg_update(&jaccs[ai], &av, err) < 0) {
                                val_clear(&av);
                                exec_set_join_ctx(NULL, NULL, NULL, 0);
                                row_clear(&jr1); row_clear(&pr1); res_free(&res);
                                return -1;
                            }
                            val_clear(&av);
                        }
                    }
                    exec_set_join_ctx(NULL, NULL, NULL, 0);
                    row_clear(&jr1);
                }

                /* LEFT JOIN: unmatched left rows still contribute to aggregate */
                if (s->joins[0].type == JOIN_LEFT && !any_match) {
                    Row null_r;
                    memset(&null_r, 0, sizeof null_r);
                    null_r.ncols = join_tabs[1].table->ncols;
                    const Table *jtabs[2] = { t, join_tabs[1].table };
                    const Row *jrows[2] = { &pr1, &null_r };
                    int w_ok;
                    if (row_matches_join(s->where, jtabs, jrows, alias_ptrs, 2,
                                         now, &w_ok, err) < 0) {
                        row_clear(&pr1); res_free(&res); return -1;
                    }
                    if (!w_ok) { row_clear(&pr1); continue; }
                    const Row *eval_rows[2] = { &pr1, &null_r };
                    exec_set_join_ctx(jtabs, eval_rows, alias_ptrs, 2);
                    for (int ai = 0; ai < naggs; ai++) {
                        if (aggs[ai]->agg_star) {
                            agg_update(&jaccs[ai], NULL, err);
                        } else {
                            Value av;
                            if (eval_expr(aggs[ai]->args[0], &pr1, t, now, &av, err) < 0) break;
                            agg_update(&jaccs[ai], &av, err);
                            val_clear(&av);
                        }
                    }
                    exec_set_join_ctx(NULL, NULL, NULL, 0);
                }
                row_clear(&pr1);
            }

            /* Emit aggregate result */
            Value agvals[MAX_AGGS];
            for (int ai = 0; ai < naggs; ai++)
                agvals[ai] = agg_result(&jaccs[ai]);
            exec_set_agg_context(agvals, naggs);

            /* Empty join result still produces one row for aggregates */
            if (res_grow(&res) < 0) {
                snprintf(err, MAX_ERR, "out of memory");
                res_free(&res); return -1;
            }
            for (int c = 0; c < ncols; c++) {
                if (eval_expr(exprs[c], &pr1, t, now,
                              &res.cells[res.nrows * ncols + c], err) < 0) {
                    res_free(&res); return -1;
                }
            }
            for (int k = 0; k < s->norder; k++) {
                Value *dst = &res.keys[res.nrows * s->norder + k];
                if (order_from_col[k] >= 0)
                    *dst = val_copy(&res.cells[res.nrows * ncols + order_from_col[k]]);
                else {
                    Row empty;
                    memset(&empty, 0, sizeof empty);
                    eval_expr(s->order[k].e, &empty, t, now, dst, err);
                }
            }
            res.refs[res.nrows] = (RowRef){0, 0};
            res.rids[res.nrows] = 0;
            res.nrows++;

            exec_set_agg_context(NULL, 0);
            for (int ai = 0; ai < naggs; ai++) { val_clear(&agvals[ai]); agg_cleanup(&jaccs[ai]); }
            goto emit;
        }

        /* Prepare scans for each join table (positions 1..njoin_tabs-1) */
        #define MAX_JT 8
        Scan jsc[MAX_JT];
        for (int i = 1; i < njoin_tabs; i++)
            scan_init(&jsc[i], db, join_tabs[i].table);

        /* Scan primary table */
        Scan psc;
        Row pr;
        scan_init(&psc, db, t);

        while (scan_next(&psc, &pr)) {
            /* WHERE is applied per joined row below, so it can reference
             * columns from either side of the join */
            join_tabs[0].row = pr;
            join_tabs[0].matched = 0;

            /* Nested-loop join: one join at a time (enforced above). */
            Row jr;
            Scan *js = &jsc[1];
            /* restart the join scan for every outer row */
            scan_init(js, db, join_tabs[1].table);
            int any_match = 0;

            while (scan_next(js, &jr)) {
                join_tabs[1].row = jr;

                /* Evaluate ON clause */
                Join *j = &s->joins[0];
                int on_ok;
                const Row *jrows[2] = { &pr, &jr };
                const Table *jtabs[2] = { t, join_tabs[1].table };
                if (row_matches_join(j->on, jtabs, jrows, alias_ptrs, 2,
                                     now, &on_ok, err) < 0) {
                    row_clear(&jr); row_clear(&pr); res_free(&res); goto join_cleanup;
                }
                if (!on_ok) { row_clear(&jr); continue; }

                const Row *w_rows[2] = { &pr, &jr };
                {
                    int w_ok;
                    if (row_matches_join(s->where, jtabs, w_rows, alias_ptrs, 2,
                                         now, &w_ok, err) < 0) {
                        row_clear(&jr); row_clear(&pr); res_free(&res);
                        goto join_cleanup;
                    }
                    if (!w_ok) { row_clear(&jr); continue; }
                }

                any_match = 1;
                join_tabs[1].matched = 1;

                /* Set up join context for expression evaluation */
                const Row *eval_rows[2] = { &pr, &jr };
                exec_set_join_ctx(jtabs, eval_rows, alias_ptrs, 2);

                if (res_grow(&res) < 0) {
                    row_clear(&jr); row_clear(&pr); res_free(&res);
                    snprintf(err, MAX_ERR, "out of memory");
                    goto join_cleanup;
                }
                int failed = 0;
                for (int c = 0; c < ncols && !failed; c++) {
                    Value *dst = &res.cells[res.nrows * ncols + c];
                    if (exprs[c]) {
                        if (eval_expr(exprs[c], &pr, t, now, dst, err) < 0) failed = 1;
                    } else {
                        int ti = colsrc[c].tab;
                        int ci = colsrc[c].col;
                        const Row *sr = (ti == 0) ? &pr : &jr;
                        *dst = (ci >= 0 && ci < sr->ncols) ? val_copy(&sr->v[ci]) : val_null();
                    }
                }
                for (int k = 0; k < s->norder && !failed; k++) {
                    Value *dst = &res.keys[res.nrows * s->norder + k];
                    if (order_from_col[k] >= 0)
                        *dst = val_copy(&res.cells[res.nrows * ncols + order_from_col[k]]);
                    else if (eval_expr(s->order[k].e, &pr, t, now, dst, err) < 0)
                        failed = 1;
                }
                res.refs[res.nrows] = pr.ref;
                res.rids[res.nrows] = pr.rid;
                res.nrows++;
                row_clear(&jr);
                if (failed) { row_clear(&pr); res_free(&res); goto join_cleanup; }
                exec_set_join_ctx(NULL, NULL, NULL, 0);
            }

            /* LEFT JOIN: emit left row with NULLs for right side */
            if (s->joins[0].type == JOIN_LEFT && !any_match) {
                Row null_r;
                memset(&null_r, 0, sizeof null_r);
                null_r.ncols = join_tabs[1].table->ncols;
                {
                    int w_ok;
                    const Table *w_tabs[2] = { t, join_tabs[1].table };
                    const Row *w_rows[2] = { &pr, &null_r };
                    if (row_matches_join(s->where, w_tabs, w_rows, alias_ptrs, 2,
                                         now, &w_ok, err) < 0) {
                        row_clear(&pr); res_free(&res); goto join_cleanup;
                    }
                    if (!w_ok) { row_clear(&pr); continue; }
                }
                if (res_grow(&res) < 0) {
                    row_clear(&pr); res_free(&res);
                    snprintf(err, MAX_ERR, "out of memory");
                    goto join_cleanup;
                }
                {
                    const Table *e_tabs[2] = { t, join_tabs[1].table };
                    const Row *e_rows[2] = { &pr, &null_r };
                    exec_set_join_ctx(e_tabs, e_rows, alias_ptrs, 2);
                }
                for (int c = 0; c < ncols; c++) {
                    Value *dst = &res.cells[res.nrows * ncols + c];
                    if (exprs[c]) {
                        if (eval_expr(exprs[c], &pr, t, now, dst, err) < 0) {
                            res_free(&res); goto join_cleanup;
                        }
                    } else {
                        int ti = colsrc[c].tab;
                        int ci = colsrc[c].col;
                        *dst = (ti == 0 && ci >= 0 && ci < pr.ncols) ? val_copy(&pr.v[ci])
                             : (ti > 0) ? val_null()
                             : val_null();
                    }
                }
                for (int k = 0; k < s->norder; k++) {
                    Value *dst = &res.keys[res.nrows * s->norder + k];
                    if (order_from_col[k] >= 0)
                        *dst = val_copy(&res.cells[res.nrows * ncols + order_from_col[k]]);
                    else if (eval_expr(s->order[k].e, &pr, t, now, dst, err) < 0) {
                        res_free(&res); goto join_cleanup;
                    }
                }
                res.refs[res.nrows] = pr.ref;
                res.rids[res.nrows] = pr.rid;
                res.nrows++;
            }

            row_clear(&pr);
            exec_set_join_ctx(NULL, NULL, NULL, 0);
        }

join_cleanup:
        if (err[0]) return -1;
        goto emit;
    } else if (!grouped) {
        if (gin_lookup) {
            int idx = table_find_index(t, gin_col);
            RefSet refs;
            memset(&refs, 0, sizeof refs);
            int rc = btree_run_foreach(db, t->indexes[idx].root, &gin_key,
                                       ref_push, &refs, err);
            val_clear(&gin_key);
            if (rc < 0 || refs.oom) {
                free(refs.refs);
                res_free(&res);
                if (!err[0]) snprintf(err, MAX_ERR, "out of memory");
                return -1;
            }
            for (int ri = 0; ri < refs.n; ri++) {
                Row r;
                if (heap_read_row(db, refs.refs[ri], &r) != 0) continue;
                /* re-apply the whole WHERE: the posting list is only a
                 * candidate set (AND clauses, negations, and any other
                 * conjuncts are checked here) */
                int m;
                if (row_matches(s->where, &r, t, now, &m, err) < 0) {
                    row_clear(&r);
                    free(refs.refs);
                    res_free(&res);
                    return -1;
                }
                if (!m) { row_clear(&r); continue; }
                if (res_grow(&res) < 0) {
                    row_clear(&r);
                    free(refs.refs);
                    res_free(&res);
                    snprintf(err, MAX_ERR, "out of memory");
                    return -1;
                }
                int failed = 0;
                for (int c = 0; c < ncols && !failed; c++) {
                    Value *dst = &res.cells[res.nrows * ncols + c];
                    if (exprs[c]) {
                        if (eval_expr(exprs[c], &r, t, now, dst, err) < 0) failed = 1;
                    } else {
                        int cm = colmap[c];
                        *dst = (cm >= 0 && cm < r.ncols) ? val_copy(&r.v[cm])
                                                         : val_null();
                    }
                }
                for (int k = 0; k < s->norder && !failed; k++) {
                    Value *dst = &res.keys[res.nrows * s->norder + k];
                    if (order_from_col[k] >= 0)
                        *dst = val_copy(&res.cells[res.nrows * ncols + order_from_col[k]]);
                    else if (eval_expr(s->order[k].e, &r, t, now, dst, err) < 0)
                        failed = 1;
                }
                res.refs[res.nrows] = r.ref;
                res.rids[res.nrows] = r.rid;
                res.nrows++;
                row_clear(&r);
                if (failed) { free(refs.refs); res_free(&res); return -1; }
            }
            free(refs.refs);
            goto point_done;
        }
        if (point_lookup) {
            int idx = table_find_index(t, point_col);
            RowRef ref;
            if (btree_find(db, t->indexes[idx].root, &point_key, &ref, err) == 0) {
                Row r;
                if (heap_read_row(db, ref, &r) == 0) {
                    if (res_grow(&res) < 0) {
                        row_clear(&r); val_clear(&point_key); res_free(&res);
                        snprintf(err, MAX_ERR, "out of memory");
                        return -1;
                    }
                    int failed = 0;
                    for (int c = 0; c < ncols && !failed; c++) {
                        Value *dst = &res.cells[res.nrows * ncols + c];
                        if (exprs[c]) {
                            if (eval_expr(exprs[c], &r, t, now, dst, err) < 0) failed = 1;
                        } else {
                            int cm = colmap[c];
                            *dst = (cm >= 0 && cm < r.ncols) ? val_copy(&r.v[cm]) : val_null();
                        }
                    }
                    for (int k = 0; k < s->norder && !failed; k++) {
                        Value *dst = &res.keys[res.nrows * s->norder + k];
                        if (order_from_col[k] >= 0)
                            *dst = val_copy(&res.cells[res.nrows * ncols + order_from_col[k]]);
                        else if (eval_expr(s->order[k].e, &r, t, now, dst, err) < 0)
                            failed = 1;
                    }
                    res.refs[res.nrows] = r.ref;
                    res.rids[res.nrows] = r.rid;
                    res.nrows++;
                    row_clear(&r);
                    if (failed) { val_clear(&point_key); res_free(&res); return -1; }
                }
            }
            val_clear(&point_key);
            goto point_done;
        }
        Scan sc;
        Row r;
        scan_init(&sc, db, t);
        while (scan_next(&sc, &r)) {
            int m;
            if (row_matches(s->where, &r, t, now, &m, err) < 0) {
                row_clear(&r); res_free(&res); return -1;
            }
            if (!m) { row_clear(&r); continue; }

            if (res_grow(&res) < 0) {
                row_clear(&r); res_free(&res);
                snprintf(err, MAX_ERR, "out of memory");
                return -1;
            }
            int failed = 0;
            for (int c = 0; c < ncols && !failed; c++) {
                Value *dst = &res.cells[res.nrows * ncols + c];
                if (exprs[c]) {
                    if (eval_expr(exprs[c], &r, t, now, dst, err) < 0) failed = 1;
                } else {
                    int idx = colmap[c];
                    *dst = (idx >= 0 && idx < r.ncols) ? val_copy(&r.v[idx])
                                                       : val_null();
                }
            }
            for (int k = 0; k < s->norder && !failed; k++) {
                Value *dst = &res.keys[res.nrows * s->norder + k];
                if (order_from_col[k] >= 0)
                    *dst = val_copy(&res.cells[res.nrows * ncols + order_from_col[k]]);
                else if (eval_expr(s->order[k].e, &r, t, now, dst, err) < 0)
                    failed = 1;
            }
            res.refs[res.nrows] = r.ref;
            res.rids[res.nrows] = r.rid;
            res.nrows++;
            row_clear(&r);
            if (failed) { res_free(&res); return -1; }
        }
    } else {
        /* ---- grouped: fold matching rows into groups keyed by the GROUP BY */
        Group *groups = NULL;
        int ngroups = 0, gcap = 0;

        Scan sc;
        Row r;
        scan_init(&sc, db, t);
        int failed = 0;

        while (!failed && scan_next(&sc, &r)) {
            int m;
            if (row_matches(s->where, &r, t, now, &m, err) < 0) {
                row_clear(&r); failed = 1; break;
            }
            if (!m) { row_clear(&r); continue; }

            /* build the group key by formatting each grouping expression */
            char key[512];
            size_t o = 0;
            key[0] = 0;
            for (int gi = 0; gi < s->ngroup; gi++) {
                Value gv;
                if (eval_expr(s->group[gi], &r, t, now, &gv, err) < 0) {
                    failed = 1; break;
                }
                char buf[128];
                val_format(&gv, buf, sizeof buf);
                o += (size_t)snprintf(key + o, sizeof key - o, "%s\x1f",
                                      gv.tag == T_NULL ? "\x01null" : buf);
                val_clear(&gv);
                if (o >= sizeof key - 1) break;
            }
            if (failed) { row_clear(&r); break; }

            int became_rep = 0;
            int gidx = -1;
            for (int gi = 0; gi < ngroups; gi++)
                if (strcmp(groups[gi].key, key) == 0) { gidx = gi; break; }

            if (gidx < 0) {
                if (ngroups == gcap) {
                    int cap = gcap ? gcap * 2 : 16;
                    Group *ng = realloc(groups, sizeof(Group) * (size_t)cap);
                    if (!ng) {
                        row_clear(&r);
                        snprintf(err, MAX_ERR, "out of memory");
                        failed = 1;
                        break;
                    }
                    groups = ng;
                    gcap = cap;
                }
                gidx = ngroups++;
                memset(&groups[gidx], 0, sizeof(Group));
                snprintf(groups[gidx].key, sizeof groups[gidx].key, "%s", key);
                groups[gidx].rep = r;             /* the group owns this row now */
                became_rep = 1;
                for (int ai = 0; ai < naggs; ai++) {
                    agg_init(&groups[gidx].accs[ai], (AggKind)aggs[ai]->agg);
                    groups[gidx].accs[ai].distinct = aggs[ai]->agg_distinct;
                }
            }

            Group *grp = &groups[gidx];
            if (grp->nrefs < MEM_COACT_MAX) {
                grp->refs[grp->nrefs] = r.ref;
                grp->rids[grp->nrefs] = r.rid;
                grp->nrefs++;
            }

            for (int ai = 0; ai < naggs && !failed; ai++) {
                if (aggs[ai]->agg_star) {
                    if (agg_update(&grp->accs[ai], NULL, err) < 0) failed = 1;
                } else {
                    Value av;
                    if (eval_expr(aggs[ai]->args[0], &r, t, now, &av, err) < 0) {
                        failed = 1;
                    } else {
                        if (agg_update(&grp->accs[ai], &av, err) < 0) failed = 1;
                        val_clear(&av);
                    }
                }
            }

            /* only free the row if the group did not adopt it as its rep */
            if (!became_rep) row_clear(&r);
        }

        /* An aggregate with no GROUP BY over an empty table still yields one
         * row, the way SQL requires: SELECT COUNT(*) FROM empty is 0, not
         * nothing. */
        if (!failed && ngroups == 0 && s->ngroup == 0) {
            Group *ng = calloc(1, sizeof(Group));
            if (!ng) { snprintf(err, MAX_ERR, "out of memory"); failed = 1; }
            else {
                groups = ng;
                gcap = ngroups = 1;
                memset(&groups[0].rep, 0, sizeof(Row));
                for (int ai = 0; ai < naggs; ai++) {
                    agg_init(&groups[0].accs[ai], (AggKind)aggs[ai]->agg);
                    groups[0].accs[ai].distinct = aggs[ai]->agg_distinct;
                }
            }
        }

        /* ---- project each group */
        for (int gi = 0; gi < ngroups && !failed; gi++) {
            Value agvals[MAX_AGGS];
            for (int ai = 0; ai < naggs; ai++) agvals[ai] = agg_result(&groups[gi].accs[ai]);
            exec_set_agg_context(agvals, naggs);

            int keep = 1;
            if (s->having) {
                Value hv;
                if (eval_expr(s->having, &groups[gi].rep, t, now, &hv, err) < 0) {
                    failed = 1;
                } else {
                    keep = val_truthy(&hv);
                    val_clear(&hv);
                }
            }

            if (!failed && keep) {
                if (res_grow(&res) < 0) {
                    snprintf(err, MAX_ERR, "out of memory");
                    failed = 1;
                } else {
                    for (int c = 0; c < ncols && !failed; c++) {
                        if (eval_expr(exprs[c], &groups[gi].rep, t, now,
                                      &res.cells[res.nrows * ncols + c], err) < 0)
                            failed = 1;
                    }
                    for (int k = 0; k < s->norder && !failed; k++) {
                        Value *dst = &res.keys[res.nrows * s->norder + k];
                        if (order_from_col[k] >= 0)
                            *dst = val_copy(&res.cells[res.nrows * ncols +
                                                       order_from_col[k]]);
                        else if (eval_expr(s->order[k].e, &groups[gi].rep, t, now,
                                           dst, err) < 0)
                            failed = 1;
                    }
                    res.refs[res.nrows] = groups[gi].nrefs ? groups[gi].refs[0]
                                                           : (RowRef){0, 0};
                    res.rids[res.nrows] = groups[gi].nrefs ? groups[gi].rids[0] : 0;
                    res.nrows++;
                }
            }

            exec_set_agg_context(NULL, 0);
            for (int ai = 0; ai < naggs; ai++) val_clear(&agvals[ai]);
        }

        /* reinforce every row that fed a group we displayed */
        if (!failed && exec_reinforce_enabled()) {
            for (int gi = 0; gi < ngroups; gi++) {
                for (int k = 0; k < groups[gi].nrefs; k++)
                    mem_touch(db, t, groups[gi].refs[k], MEM_BOOST);
                if (groups[gi].nrefs >= 2)
                    mem_associate(db, groups[gi].rids, groups[gi].nrefs,
                                  MEM_LINK_BOOST);
            }
        }

        for (int gi = 0; gi < ngroups; gi++) {
            row_clear(&groups[gi].rep);
            for (int ai = 0; ai < naggs; ai++) {
                val_clear(&groups[gi].accs[ai].best);
                agg_cleanup(&groups[gi].accs[ai]);
            }
        }
        free(groups);

        if (failed) { res_free(&res); return -1; }
        goto emit_grouped;
    }

point_done:
emit:
emit_grouped:
    /* ---- DISTINCT: drop rows whose formatted cells repeat */
    if (s->distinct && res.nrows > 1) {
        int out = 0;
        for (int i = 0; i < res.nrows; i++) {
            int dup = 0;
            for (int j = 0; j < out && !dup; j++) {
                dup = 1;
                for (int c = 0; c < ncols && dup; c++) {
                    int ok;
                    const Value *a = &res.cells[i * ncols + c];
                    const Value *b = &res.cells[j * ncols + c];
                    if (a->tag == T_NULL || b->tag == T_NULL) {
                        if (a->tag != b->tag) dup = 0;
                    } else if (!(val_compare(a, b, &ok) == 0 && ok == 1)) {
                        dup = 0;
                    }
                }
            }
            if (dup) {
                for (int c = 0; c < ncols; c++) val_clear(&res.cells[i * ncols + c]);
                for (int k = 0; k < res.nkeys; k++) val_clear(&res.keys[i * res.nkeys + k]);
                continue;
            }
            if (out != i) {
                for (int c = 0; c < ncols; c++)
                    res.cells[out * ncols + c] = res.cells[i * ncols + c];
                for (int k = 0; k < res.nkeys; k++)
                    res.keys[out * res.nkeys + k] = res.keys[i * res.nkeys + k];
                res.refs[out] = res.refs[i];
                res.rids[out] = res.rids[i];
            }
            out++;
        }
        res.nrows = out;
    }

    /* ---- ORDER BY, via an index permutation so cells stay put */
    int *order = malloc(sizeof(int) * (size_t)(res.nrows ? res.nrows : 1));
    if (!order) { res_free(&res); snprintf(err, MAX_ERR, "out of memory"); return -1; }
    for (int i = 0; i < res.nrows; i++) order[i] = i;
    if (s->norder && res.nrows > 1) {
        g_res = &res;
        g_stmt = s;
        qsort(order, (size_t)res.nrows, sizeof(int), res_cmp);
    }

    /* ---- the row window: TOP n, LIMIT n and OFFSET m all cut the ordered
     * rows; TOP and LIMIT cap the count, OFFSET skips leading rows */
    int lo = s->offset > 0 ? s->offset : 0;
    if (lo > res.nrows) lo = res.nrows;
    int hi = res.nrows;
    if (s->top >= 0 && lo + s->top < hi) hi = lo + s->top;
    if (s->limit >= 0 && lo + s->limit < hi) hi = lo + s->limit;

    /* ---- hand the rows to the caller instead of printing, if asked */
    if (capture) {
        capture->ncols = ncols;
        for (int i = 0; i < ncols && i < MAX_OUT_COLS; i++)
            snprintf(capture->colnames[i], MAX_NAME, "%s", heads[i]);
        for (int i = lo; i < hi; i++) {
            if (capture_push(capture, &res.cells[order[i] * ncols], ncols) < 0) {
                free(order);
                res_free(&res);
                snprintf(err, MAX_ERR, "out of memory");
                return -1;
            }
        }
        if (!grouped && s->has_from && exec_reinforce_enabled()) {
            for (int i = lo; i < hi; i++)
                mem_touch(db, t, res.refs[order[i]], MEM_BOOST);
        }
        free(order);
        res_free(&res);
        return 0;
    }

    /* ---- print */
    Grid g = {0};
    grid_init(&g, ncols);
    for (int c = 0; c < ncols; c++) snprintf(g.head[c], MAX_NAME, "%s", heads[c]);

    for (int i = lo; i < hi; i++) {
        char *cells[MAX_OUT_COLS];
        int src = order[i];
        for (int c = 0; c < ncols; c++) {
            char buf[512];
            const Value *v = &res.cells[src * ncols + c];
            /* strength is a continuous quantity, so show it to three decimals
             * the way SHOW MEMORY does rather than as a bare "1.0" */
            if (exprs[c] && exprs[c]->kind == EX_COL && v->tag == T_FLOAT &&
                strcasecmp(exprs[c]->col, "_strength") == 0)
                snprintf(buf, sizeof buf, "%.3f", v->f);
            else
                val_format(v, buf, sizeof buf);
            cells[c] = strdup(buf);
        }
        grid_row(&g, cells);
    }
    if (g.nrows) grid_print(&g);
    printf("\n(%d row%s)\n", g.nrows, g.nrows == 1 ? "" : "s");

    /* If a global capture was requested, copy the grid to it */
    if (g_select_capture) {
        g_select_capture->ncols = g.ncols;
        for (int c = 0; c < g.ncols; c++)
            snprintf(g_select_capture->colnames[c], MAX_NAME, "%s", g.head[c]);
        for (int r = 0; r < g.nrows; r++) {
            Value row[MAX_OUT_COLS];
            for (int c = 0; c < g.ncols; c++)
                row[c] = g.cells[r * g.ncols + c]
                             ? val_text(g.cells[r * g.ncols + c])
                             : val_null();
            capture_push(g_select_capture, row, g.ncols);
            for (int c = 0; c < g.ncols; c++) val_clear(&row[c]);
        }
    }

    grid_free(&g);

    /* ---- reinforce the rows actually shown (ungrouped queries only; the
     * grouped path already did it, and a FROM-less query has no rows) */
    if (!grouped && s->has_from && exec_reinforce_enabled()) {
        uint64_t rids[MEM_COACT_MAX];
        int nr = 0;
        for (int i = lo; i < hi; i++) {
            int src = order[i];
            if (t) mem_touch(db, t, res.refs[src], MEM_BOOST);
            if (nr < MEM_COACT_MAX) rids[nr++] = res.rids[src];
        }
        if (nr >= 2) mem_associate(db, rids, nr, MEM_LINK_BOOST);
    }

    free(order);
    res_free(&res);
    return 0;
}
