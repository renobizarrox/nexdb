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

/* ------------------------------------------------------- aggregate state */

typedef struct {
    AggKind kind;
    int64_t n;          /* rows counted                */
    double  sum;
    int     any;        /* saw at least one non-NULL   */
    int     is_float;   /* SUM/AVG should stay integral */
    Value   best;       /* MIN / MAX                   */
} AggAcc;

/* Reset aggregate accumulator state for one grouping pass. */
static void agg_init(AggAcc *a, AggKind k)
{
    memset(a, 0, sizeof *a);
    a->kind = k;
    a->best = val_null();
}

/* Feed one row's value into a running aggregate; returns -1 on type errors. */
static int agg_update(AggAcc *a, const Value *v, char *err)
{
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
static Res *g_res;
static Stmt *g_stmt;

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

#define MAX_AGGS 16

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
    return exec_select_into(db, s, NULL, err);
}

/* Full SELECT pipeline; optionally capture rows into cap instead of printing. */
int exec_select_into(DB *db, Stmt *s, Capture *capture, char *err)
{
    int64_t now = mem_now();
    Table *t = NULL;

    if (s->has_from) {
        t = cat_find(db, s->table);
        if (!t) { snprintf(err, MAX_ERR, "unknown table '%s'", s->table); return -1; }
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
        snprintf(err, MAX_ERR, "HAVING needs GROUP BY or an aggregate");
        return -1;
    }
    if (grouped && !s->has_from) {
        snprintf(err, MAX_ERR, "aggregates need a FROM clause");
        return -1;
    }
    if (grouped) {
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
    int ncols;

    if (s->has_from) {
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

    /* ORDER BY may name an output alias rather than repeat the expression, as in
     * "SELECT SUM(x) AS total ... ORDER BY total DESC". Resolve those to the
     * output column instead of evaluating them against a row, which is both
     * what SQL means and the only way it can work for an aggregate alias. */
    int order_from_col[MAX_ORDER_KEYS];
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

    Res res;
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

    /* ---- scan, filter, and either project directly or fold into groups */
    if (!grouped) {
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
                for (int ai = 0; ai < naggs; ai++)
                    agg_init(&groups[gidx].accs[ai], (AggKind)aggs[ai]->agg);
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
                for (int ai = 0; ai < naggs; ai++)
                    agg_init(&groups[0].accs[ai], (AggKind)aggs[ai]->agg);
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
            for (int ai = 0; ai < naggs; ai++) val_clear(&groups[gi].accs[ai].best);
        }
        free(groups);

        if (failed) { res_free(&res); return -1; }
        goto emit_grouped;
    }

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

    int limit = (s->top >= 0 && s->top < res.nrows) ? s->top : res.nrows;

    /* ---- hand the rows to the caller instead of printing, if asked */
    if (capture) {
        for (int i = 0; i < limit; i++) {
            if (capture_push(capture, &res.cells[order[i] * ncols], ncols) < 0) {
                free(order);
                res_free(&res);
                snprintf(err, MAX_ERR, "out of memory");
                return -1;
            }
        }
        if (!grouped && s->has_from && exec_reinforce_enabled()) {
            for (int i = 0; i < limit; i++)
                mem_touch(db, t, res.refs[order[i]], MEM_BOOST);
        }
        free(order);
        res_free(&res);
        return 0;
    }

    /* ---- print */
    Grid g;
    grid_init(&g, ncols);
    for (int c = 0; c < ncols; c++) snprintf(g.head[c], MAX_NAME, "%s", heads[c]);

    for (int i = 0; i < limit; i++) {
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
    grid_free(&g);

    /* ---- reinforce the rows actually shown (ungrouped queries only; the
     * grouped path already did it, and a FROM-less query has no rows) */
    if (!grouped && s->has_from && exec_reinforce_enabled()) {
        uint64_t rids[MEM_COACT_MAX];
        int nr = 0;
        for (int i = 0; i < limit; i++) {
            int src = order[i];
            mem_touch(db, t, res.refs[src], MEM_BOOST);
            if (nr < MEM_COACT_MAX) rids[nr++] = res.rids[src];
        }
        if (nr >= 2) mem_associate(db, rids, nr, MEM_LINK_BOOST);
    }

    free(order);
    res_free(&res);
    return 0;
}
