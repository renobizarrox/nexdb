/* parser.c - recursive-descent parser for the nexdb dialect.
 *
 * Standard T-SQL subset:
 *   CREATE TABLE / DROP TABLE
 *   INSERT INTO ... VALUES (...), (...)
 *   SELECT [TOP n] cols FROM t [WHERE ...] [ORDER BY c [ASC|DESC]]
 *   UPDATE t SET c = expr [WHERE ...]
 *   DELETE FROM t [WHERE ...]
 *   PRINT 'text' ,  GO batch separator
 *
 * Memory extensions:
 *   RECALL 'what you half remember' [FROM t] [TOP n]
 *   REMEMBER FROM t WHERE ...      -- reinforce by hand
 *   FORGET FROM t WHERE ...        -- weaken by hand
 *   SHOW TABLES | SHOW MEMORY FROM t | SHOW LINKS [FROM t]
 *   CHECKPOINT
 *
 * Convention: lx->cur always holds the next unconsumed token. The caller
 * primes it with one lex_next() before the first parse_stmt().
 */
#define _GNU_SOURCE
#include "nexdb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

/* Recursively free an expression tree and any owned literal text. */
void expr_free(Expr *e)
{
    if (!e) return;
    expr_free(e->l);
    expr_free(e->r);
    for (int i = 0; i < e->nitems; i++) expr_free(e->items[i]);
    for (int i = 0; i < e->nargs; i++) expr_free(e->args[i]);
    if (e->sub) {
        stmt_free(e->sub);
        free(e->sub);
        e->sub = NULL;
    }
    val_clear(&e->lit);
    free(e);
}

/* Release all heap owned by a parsed statement (WHERE, SET list, subquery, etc.). */
void stmt_free(Stmt *s)
{
    expr_free(s->where);
    s->where = NULL;
    expr_free(s->having);
    s->having = NULL;
    for (int i = 0; i < s->njoins; i++) expr_free(s->joins[i].on);
    s->njoins = 0;
    for (int i = 0; i < s->nrows; i++)
        for (int j = 0; j < s->row_width[i]; j++)
            expr_free(s->rows[i][j]);
    s->nrows = 0;
    for (int i = 0; i < s->nsets; i++) expr_free(s->sets[i].val);
    s->nsets = 0;
    for (int i = 0; i < s->nitems; i++) {
        expr_free(s->items[i].e);
        s->items[i].e = NULL;
    }
    s->nitems = 0;
    for (int i = 0; i < s->ngroup; i++) expr_free(s->group[i]);
    s->ngroup = 0;
    for (int i = 0; i < s->norder; i++) expr_free(s->order[i].e);
    s->norder = 0;
    if (s->sub) {
        stmt_free(s->sub);
        free(s->sub);
        s->sub = NULL;
    }
    if (s->derived) {
        stmt_free(s->derived);
        free(s->derived);
        s->derived = NULL;
    }
    if (s->set_rhs) {
        stmt_free(s->set_rhs);
        free(s->set_rhs);
        s->set_rhs = NULL;
    }
    for (int i = 0; i < s->nargs; i++) {
        expr_free(s->args[i]);
        s->args[i] = NULL;
    }
    s->nargs = 0;
    if (s->stmts) {
        for (int i = 0; i < s->nstmts; i++) {
            if (s->stmts[i]) {
                stmt_free(s->stmts[i]);
                free(s->stmts[i]);
                s->stmts[i] = NULL;
            }
        }
        free(s->stmts);
        s->stmts = NULL;
        s->nstmts = 0;
    }
    if (s->else_stmts) {
        for (int i = 0; i < s->n_else; i++) {
            if (s->else_stmts[i]) {
                stmt_free(s->else_stmts[i]);
                free(s->else_stmts[i]);
                s->else_stmts[i] = NULL;
            }
        }
        free(s->else_stmts);
        s->else_stmts = NULL;
        s->n_else = 0;
    }
}

/* Release a parsed procedure program (the statement list and every statement). */
void prog_free(Program *p)
{
    if (!p) return;
    for (int i = 0; i < p->nstmts; i++) {
        if (p->stmts[i]) {
            stmt_free(p->stmts[i]);
            free(p->stmts[i]);
            p->stmts[i] = NULL;
        }
    }
    free(p->stmts);
    p->stmts = NULL;
    p->nstmts = 0;
}

/* ------------------------------------------------------------- utilities */

#define FAIL(...) do { snprintf(err, MAX_ERR, __VA_ARGS__); return -1; } while (0)

static int adv(Lexer *lx, char *err)
{
    if (lex_next(lx) < 0) FAIL("%s", lx->err);
    return 0;
}

static int eat_punct(Lexer *lx, const char *p, char *err)
{
    if (!tok_is_punct(&lx->cur, p))
        FAIL("expected '%s' but found '%s' on line %d", p,
             lx->cur.kind == TK_EOF ? "end of input" : lx->cur.text, lx->cur.line);
    return adv(lx, err);
}

static int opt_punct(Lexer *lx, const char *p, char *err)
{
    if (tok_is_punct(&lx->cur, p)) { if (adv(lx, err) < 0) return -1; return 1; }
    return 0;
}

static int eat_kw(Lexer *lx, const char *kw, char *err)
{
    if (!tok_is_kw(&lx->cur, kw))
        FAIL("expected %s but found '%s' on line %d", kw,
             lx->cur.kind == TK_EOF ? "end of input" : lx->cur.text, lx->cur.line);
    return adv(lx, err);
}

static int opt_kw(Lexer *lx, const char *kw, char *err)
{
    if (tok_is_kw(&lx->cur, kw)) { if (adv(lx, err) < 0) return -1; return 1; }
    return 0;
}

static int take_ident(Lexer *lx, char *out, size_t cap, char *err)
{
    if (lx->cur.kind != TK_IDENT)
        FAIL("expected a name but found '%s' on line %d",
             lx->cur.kind == TK_EOF ? "end of input" : lx->cur.text, lx->cur.line);
    /* Truncating instead of complaining let two different long names collapse
     * into one, and then quietly write data into the wrong column. */
    if (strlen(lx->cur.text) >= cap)
        FAIL("the name '%.32s...' on line %d is %zu characters; the limit is %zu",
             lx->cur.text, lx->cur.line, strlen(lx->cur.text), cap - 1);
    snprintf(out, cap, "%s", lx->cur.text);
    return adv(lx, err);
}

static Expr *ex_new(ExprKind k)
{
    Expr *e = calloc(1, sizeof(Expr));
    if (e) { e->kind = k; e->lit = val_null(); }
    return e;
}

/* --------------------------------------------------------- expressions */

static Expr *parse_or(Lexer *lx, char *err);
static int parse_type(Lexer *lx, Column *c, char *err);
static int parse_select(Lexer *lx, Stmt *s, char *err);
static Expr *parse_case(Lexer *lx, char *err);
static Expr *parse_cast(Lexer *lx, char *err);

/* Flag set by parse_primary when it encounters "table.*" — the select-list
 * loop reads it to turn the item into a qualified star expansion. */
static __thread int  g_sel_star_qual = 0;
static __thread char g_sel_star_name[MAX_NAME];

/* Map a name to AggKind, or AGG_NONE if it is not an aggregate function. */
int agg_kind_of(const char *name)
{
    if (!strcasecmp(name, "COUNT")) return AGG_COUNT;
    if (!strcasecmp(name, "SUM"))   return AGG_SUM;
    if (!strcasecmp(name, "AVG"))   return AGG_AVG;
    if (!strcasecmp(name, "MIN"))   return AGG_MIN;
    if (!strcasecmp(name, "MAX"))   return AGG_MAX;
    return AGG_NONE;
}

/* CASE WHEN a THEN b [WHEN c THEN d ...] [ELSE e] END
 * CASE x WHEN v THEN b ... END        (the simple, operand form)
 *
 * WHEN/THEN pairs live in items[] two at a time; l is the optional operand and
 * r is the ELSE branch. */
static Expr *parse_case(Lexer *lx, char *err)
{
    if (adv(lx, err) < 0) return NULL;            /* CASE */
    Expr *e = ex_new(EX_CASE);
    if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }

    if (!tok_is_kw(&lx->cur, "WHEN")) {           /* simple form */
        e->l = parse_or(lx, err);
        if (!e->l) { expr_free(e); return NULL; }
    }

    if (!tok_is_kw(&lx->cur, "WHEN")) {
        expr_free(e);
        snprintf(err, MAX_ERR, "CASE needs at least one WHEN (line %d)",
                 lx->cur.line);
        return NULL;
    }

    while (tok_is_kw(&lx->cur, "WHEN")) {
        if (e->nitems + 2 > MAX_IN_ITEMS) {
            expr_free(e);
            snprintf(err, MAX_ERR, "too many WHEN branches in one CASE");
            return NULL;
        }
        if (adv(lx, err) < 0) { expr_free(e); return NULL; }
        Expr *cond = parse_or(lx, err);
        if (!cond) { expr_free(e); return NULL; }
        e->items[e->nitems++] = cond;

        if (eat_kw(lx, "THEN", err) < 0) { expr_free(e); return NULL; }
        Expr *val = parse_or(lx, err);
        if (!val) { expr_free(e); return NULL; }
        e->items[e->nitems++] = val;
    }

    if (tok_is_kw(&lx->cur, "ELSE")) {
        if (adv(lx, err) < 0) { expr_free(e); return NULL; }
        e->r = parse_or(lx, err);
        if (!e->r) { expr_free(e); return NULL; }
    }
    if (eat_kw(lx, "END", err) < 0) { expr_free(e); return NULL; }
    return e;
}

/* CAST(expr AS type) */
static Expr *parse_cast(Lexer *lx, char *err)
{
    if (adv(lx, err) < 0) return NULL;            /* CAST */
    if (eat_punct(lx, "(", err) < 0) return NULL;

    Expr *e = ex_new(EX_CAST);
    if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
    e->l = parse_or(lx, err);
    if (!e->l) { expr_free(e); return NULL; }
    if (eat_kw(lx, "AS", err) < 0) { expr_free(e); return NULL; }

    Column tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.name, MAX_NAME, "cast");
    if (parse_type(lx, &tmp, err) < 0) { expr_free(e); return NULL; }
    e->cast_type = tmp.type;
    e->cast_sub  = tmp.sub;
    e->cast_len  = tmp.maxlen;

    if (eat_punct(lx, ")", err) < 0) { expr_free(e); return NULL; }
    return e;
}

static Expr *parse_primary(Lexer *lx, char *err)
{
    Token *t = &lx->cur;

    if (tok_is_punct(t, "(")) {
        if (adv(lx, err) < 0) return NULL;
    /* subquery (SELECT ...) */
    if (tok_is_kw(&lx->cur, "SELECT")) {
        if (adv(lx, err) < 0) return NULL;
        struct Stmt *sub = calloc(1, sizeof(struct Stmt));
        if (!sub) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        if (parse_select(lx, sub, err) < 0) { free(sub); return NULL; }
        if (eat_punct(lx, ")", err) < 0) { stmt_free(sub); free(sub); return NULL; }
            Expr *e = ex_new(EX_SUBQUERY);
            if (!e) { stmt_free(sub); free(sub); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            e->sub = sub;
            e->subq_type = SUBQ_SCALAR;
            return e;
        }
        Expr *inner = parse_or(lx, err);
        if (!inner) return NULL;
        if (eat_punct(lx, ")", err) < 0) { expr_free(inner); return NULL; }
        return inner;
    }

    if (tok_is_punct(t, "-")) {
        if (adv(lx, err) < 0) return NULL;
        Expr *sub = parse_primary(lx, err);
        if (!sub) return NULL;
        Expr *e = ex_new(EX_NEG);
        if (!e) { expr_free(sub); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        e->l = sub;
        return e;
    }

    if (t->kind == TK_NUMBER) {
        Expr *e = ex_new(EX_LIT);
        if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        e->lit = t->is_float ? val_float(t->fval) : val_int(t->ival);
        if (adv(lx, err) < 0) { expr_free(e); return NULL; }
        return e;
    }

    if (t->kind == TK_STRING) {
        Expr *e = ex_new(EX_LIT);
        if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        e->lit = val_text(t->text);
        if (adv(lx, err) < 0) { expr_free(e); return NULL; }
        return e;
    }

    if (t->kind == TK_IDENT) {
        /* @name — a stored-procedure variable or parameter. Resolved against
         * the current variable frame at evaluation time. */
        if (t->text[0] == '@') {
            Expr *e = ex_new(EX_VAR);
            if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            snprintf(e->col, MAX_NAME, "%s", t->text);
            if (adv(lx, err) < 0) { expr_free(e); return NULL; }
            return e;
        }
        if (tok_is_kw(t, "NULL")) {
            Expr *e = ex_new(EX_LIT);
            if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            if (adv(lx, err) < 0) { expr_free(e); return NULL; }
            return e;
        }
        if (tok_is_kw(t, "CASE"))  return parse_case(lx, err);
        if (tok_is_kw(t, "CAST"))  return parse_cast(lx, err);

        /* A name followed by '(' is a call. Aggregates are recorded as their own
         * kind because they cannot be evaluated one row at a time. */
        char name[MAX_NAME];
        snprintf(name, sizeof name, "%s", t->text);

        /* Check for qualified column: table.col or table.* */
        if (tok_is_punct(lex_peek(lx), ".")) {
            char qual[MAX_NAME];
            snprintf(qual, sizeof qual, "%s", name);
            if (adv(lx, err) < 0) return NULL;  /* ident */
            if (adv(lx, err) < 0) return NULL;  /* '.' */
            if (tok_is_punct(&lx->cur, "*")) {
                /* table.* — signal to select-list loop */
                g_sel_star_qual = 1;
                snprintf(g_sel_star_name, MAX_NAME, "%s", qual);
                if (adv(lx, err) < 0) return NULL;  /* '*' */
                Expr *d = ex_new(EX_LIT);
                if (d) d->lit = val_null();
                return d;
            }
            if (lx->cur.kind != TK_IDENT) {
                snprintf(err, MAX_ERR, "expected column name after '%s.' on line %d",
                         qual, lx->cur.line);
                return NULL;
            }
            Expr *e = ex_new(EX_COL);
            if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            snprintf(e->col_table, MAX_NAME, "%s", qual);
            snprintf(e->col, MAX_NAME, "%s", lx->cur.text);
            if (adv(lx, err) < 0) { expr_free(e); return NULL; }
            return e;
        }

        int is_agg = agg_kind_of(name) != AGG_NONE;

        if (tok_is_punct(lex_peek(lx), "(") && (is_agg || func_exists(name))) {
            if (adv(lx, err) < 0) return NULL;      /* the name */
            if (adv(lx, err) < 0) return NULL;      /* '('      */

            Expr *e = ex_new(is_agg ? EX_AGG : EX_FUNC);
            if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            snprintf(e->fname, MAX_NAME, "%s", name);
            e->agg = (uint8_t)agg_kind_of(name);

            if (tok_is_kw(&lx->cur, "DISTINCT")) {
                if (adv(lx, err) < 0) { expr_free(e); return NULL; }
                e->agg_distinct = 1;
            }

            if (is_agg && tok_is_punct(&lx->cur, "*")) {
                if (e->agg != AGG_COUNT) {
                    expr_free(e);
                    snprintf(err, MAX_ERR, "only COUNT accepts *, not %s", name);
                    return NULL;
                }
                e->agg_star = 1;
                if (adv(lx, err) < 0) { expr_free(e); return NULL; }
            } else if (!tok_is_punct(&lx->cur, ")")) {
                for (;;) {
                    if (e->nargs >= MAX_FUNC_ARGS) {
                        expr_free(e);
                        snprintf(err, MAX_ERR, "%s takes at most %d arguments",
                                 name, MAX_FUNC_ARGS);
                        return NULL;
                    }
                    Expr *arg = parse_or(lx, err);
                    if (!arg) { expr_free(e); return NULL; }
                    e->args[e->nargs++] = arg;
                    int more = opt_punct(lx, ",", err);
                    if (more < 0) { expr_free(e); return NULL; }
                    if (!more) break;
                }
            }
            if (eat_punct(lx, ")", err) < 0) { expr_free(e); return NULL; }

            if (is_agg && !e->agg_star && e->nargs != 1) {
                expr_free(e);
                snprintf(err, MAX_ERR, "%s takes exactly one argument", name);
                return NULL;
            }
            return e;
        }

        /* a name followed by '(' was meant as a call, so say so plainly rather
         * than complaining about the parenthesis further down the line */
        if (tok_is_punct(lex_peek(lx), "(")) {
            snprintf(err, MAX_ERR,
                     "unknown function '%s' on line %d", name, t->line);
            return NULL;
        }

        Expr *e = ex_new(EX_COL);
        if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        snprintf(e->col, MAX_NAME, "%s", t->text);
        if (strlen(t->text) >= MAX_NAME) {
            expr_free(e);
            snprintf(err, MAX_ERR, "the name on line %d is too long (limit %d)",
                     t->line, MAX_NAME - 1);
            return NULL;
        }
        if (adv(lx, err) < 0) { expr_free(e); return NULL; }
        return e;
    }

    snprintf(err, MAX_ERR, "unexpected '%s' in expression on line %d",
             t->kind == TK_EOF ? "end of input" : t->text, t->line);
    return NULL;
}

static Expr *mk_bin(BinOp op, Expr *l, Expr *r)
{
    Expr *e = ex_new(EX_BIN);
    if (!e) { expr_free(l); expr_free(r); return NULL; }
    e->op = op; e->l = l; e->r = r;
    return e;
}

static Expr *parse_mul(Lexer *lx, char *err)
{
    Expr *l = parse_primary(lx, err);
    if (!l) return NULL;
    for (;;) {
        BinOp op;
        if      (tok_is_punct(&lx->cur, "*")) op = OP_MUL;
        else if (tok_is_punct(&lx->cur, "/")) op = OP_DIV;
        else if (tok_is_punct(&lx->cur, "%")) op = OP_MOD;
        else return l;
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        Expr *r = parse_primary(lx, err);
        if (!r) { expr_free(l); return NULL; }
        l = mk_bin(op, l, r);
        if (!l) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
    }
}

static Expr *parse_add(Lexer *lx, char *err)
{
    Expr *l = parse_mul(lx, err);
    if (!l) return NULL;
    for (;;) {
        BinOp op;
        if      (tok_is_punct(&lx->cur, "+")) op = OP_ADD;
        else if (tok_is_punct(&lx->cur, "-")) op = OP_SUB;
        else return l;
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        Expr *r = parse_mul(lx, err);
        if (!r) { expr_free(l); return NULL; }
        l = mk_bin(op, l, r);
        if (!l) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
    }
}

static Expr *parse_pred(Lexer *lx, char *err)
{
    /* EXISTS (subquery) — unary prefix, doesn't consume a left operand */
    {
        int ex_neg = 0;
        if (tok_is_kw(&lx->cur, "NOT") && tok_is_kw(lex_peek(lx), "EXISTS")) {
            ex_neg = 1;
            if (adv(lx, err) < 0) return NULL;
        }
        if (tok_is_kw(&lx->cur, "EXISTS")) {
            if (adv(lx, err) < 0) return NULL;
            if (eat_punct(lx, "(", err) < 0) return NULL;
            if (!tok_is_kw(&lx->cur, "SELECT")) {
                snprintf(err, MAX_ERR, "EXISTS requires a subquery (SELECT ...)");
                return NULL;
            }
            if (adv(lx, err) < 0) return NULL;
            struct Stmt *sub = calloc(1, sizeof(struct Stmt));
            if (!sub) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            if (parse_select(lx, sub, err) < 0) { free(sub); return NULL; }
            if (eat_punct(lx, ")", err) < 0) { stmt_free(sub); free(sub); return NULL; }
            Expr *e = ex_new(EX_SUBQUERY);
            if (!e) { stmt_free(sub); free(sub); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            e->sub = sub;
            e->subq_type = SUBQ_EXISTS;
            e->negated = ex_neg;
            return e;
        }
    }

    Expr *l = parse_add(lx, err);
    if (!l) return NULL;

    /* IS [NOT] NULL */
    if (tok_is_kw(&lx->cur, "IS")) {
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        int neg = opt_kw(lx, "NOT", err);
        if (neg < 0) { expr_free(l); return NULL; }
        if (eat_kw(lx, "NULL", err) < 0) { expr_free(l); return NULL; }
        Expr *e = ex_new(neg ? EX_IS_NOT_NULL : EX_IS_NULL);
        if (!e) { expr_free(l); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        e->l = l;
        return e;
    }

    /* [NOT] LIKE / [NOT] IN / [NOT] BETWEEN */
    int negated = 0;
    if (tok_is_kw(&lx->cur, "NOT") &&
        (tok_is_kw(lex_peek(lx), "LIKE") || tok_is_kw(lex_peek(lx), "IN") ||
         tok_is_kw(lex_peek(lx), "BETWEEN"))) {
        negated = 1;
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
    }

    if (tok_is_kw(&lx->cur, "BETWEEN")) {
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        Expr *e = ex_new(EX_BETWEEN);
        if (!e) { expr_free(l); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        e->l = l;
        e->negated = negated;
        e->args[0] = parse_add(lx, err);
        if (!e->args[0]) { expr_free(e); return NULL; }
        if (eat_kw(lx, "AND", err) < 0) { expr_free(e); return NULL; }
        e->args[1] = parse_add(lx, err);
        if (!e->args[1]) { expr_free(e); return NULL; }
        e->nargs = 2;
        return e;
    }

    if (tok_is_kw(&lx->cur, "LIKE")) {
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        Expr *r = parse_add(lx, err);
        if (!r) { expr_free(l); return NULL; }
        Expr *e = mk_bin(negated ? OP_NOT_LIKE : OP_LIKE, l, r);
        if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }

        /* ESCAPE 'c' - without it a literal % or _ cannot be searched for */
        if (tok_is_kw(&lx->cur, "ESCAPE")) {
            if (adv(lx, err) < 0) { expr_free(e); return NULL; }
            if (lx->cur.kind != TK_STRING || strlen(lx->cur.text) != 1) {
                expr_free(e);
                snprintf(err, MAX_ERR,
                         "ESCAPE needs a single-character string, as in "
                         "LIKE '%%50!%% off' ESCAPE '!' (line %d)", lx->cur.line);
                return NULL;
            }
            e->esc = lx->cur.text[0];
            if (adv(lx, err) < 0) { expr_free(e); return NULL; }
        }
        return e;
    }

    if (tok_is_kw(&lx->cur, "IN")) {
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        if (eat_punct(lx, "(", err) < 0) { expr_free(l); return NULL; }

        /* IN (SELECT ...) — subquery */
        if (tok_is_kw(&lx->cur, "SELECT")) {
            if (adv(lx, err) < 0) { expr_free(l); return NULL; }
            struct Stmt *sub = calloc(1, sizeof(struct Stmt));
            if (!sub) { expr_free(l); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            if (parse_select(lx, sub, err) < 0) { free(sub); expr_free(l); return NULL; }
            if (eat_punct(lx, ")", err) < 0) { stmt_free(sub); free(sub); expr_free(l); return NULL; }
            Expr *e = ex_new(EX_IN);
            if (!e) { stmt_free(sub); free(sub); expr_free(l); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
            e->l = l;
            e->negated = negated;
            e->sub = sub;
            e->nitems = 0;  /* signals subquery instead of list */
            return e;
        }

        Expr *e = ex_new(EX_IN);
        if (!e) { expr_free(l); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        e->l = l;
        e->negated = negated;
        for (;;) {
            if (e->nitems >= MAX_IN_ITEMS) {
                expr_free(e);
                snprintf(err, MAX_ERR, "too many values in IN list");
                return NULL;
            }
            Expr *item = parse_add(lx, err);
            if (!item) { expr_free(e); return NULL; }
            e->items[e->nitems++] = item;
            int more = opt_punct(lx, ",", err);
            if (more < 0) { expr_free(e); return NULL; }
            if (!more) break;
        }
        if (eat_punct(lx, ")", err) < 0) { expr_free(e); return NULL; }
        return e;
    }

    if (negated) {   /* we consumed NOT but nothing it can negate followed */
        expr_free(l);
        snprintf(err, MAX_ERR,
                 "expected LIKE, IN or BETWEEN after NOT on line %d",
                 lx->cur.line);
        return NULL;
    }

    BinOp op;
    if      (tok_is_punct(&lx->cur, "="))  op = OP_EQ;
    else if (tok_is_punct(&lx->cur, "<>")) op = OP_NE;
    else if (tok_is_punct(&lx->cur, "!=")) op = OP_NE;
    else if (tok_is_punct(&lx->cur, "<=")) op = OP_LE;
    else if (tok_is_punct(&lx->cur, ">=")) op = OP_GE;
    else if (tok_is_punct(&lx->cur, "<"))  op = OP_LT;
    else if (tok_is_punct(&lx->cur, ">"))  op = OP_GT;
    else return l;

    if (adv(lx, err) < 0) { expr_free(l); return NULL; }
    /* ANY / ALL (subquery) after comparison operator, e.g. x > ALL (SELECT ...) */
    if (tok_is_kw(&lx->cur, "ANY") || tok_is_kw(&lx->cur, "ALL")) {
        int is_any = tok_is_kw(&lx->cur, "ANY");
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        if (eat_punct(lx, "(", err) < 0) { expr_free(l); return NULL; }
        if (!tok_is_kw(&lx->cur, "SELECT")) {
            expr_free(l);
            snprintf(err, MAX_ERR, "ANY/ALL requires a subquery (SELECT ...)");
            return NULL;
        }
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        struct Stmt *sub = calloc(1, sizeof(struct Stmt));
        if (!sub) { expr_free(l); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        if (parse_select(lx, sub, err) < 0) { free(sub); expr_free(l); return NULL; }
        if (eat_punct(lx, ")", err) < 0) { stmt_free(sub); free(sub); expr_free(l); return NULL; }
        Expr *e = ex_new(is_any ? EX_ANY : EX_ALL);
        if (!e) { stmt_free(sub); free(sub); expr_free(l); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        e->l = l;
        e->op = op;
        e->sub = sub;
        return e;
    }
    Expr *r = parse_add(lx, err);
    if (!r) { expr_free(l); return NULL; }
    Expr *e = mk_bin(op, l, r);
    if (!e) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
    return e;
}

static Expr *parse_not(Lexer *lx, char *err)
{
    if (tok_is_kw(&lx->cur, "NOT")) {
        if (adv(lx, err) < 0) return NULL;
        Expr *sub = parse_not(lx, err);
        if (!sub) return NULL;
        Expr *e = ex_new(EX_NOT);
        if (!e) { expr_free(sub); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        e->l = sub;
        return e;
    }
    return parse_pred(lx, err);
}

static Expr *parse_and(Lexer *lx, char *err)
{
    Expr *l = parse_not(lx, err);
    if (!l) return NULL;
    while (tok_is_kw(&lx->cur, "AND")) {
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        Expr *r = parse_not(lx, err);
        if (!r) { expr_free(l); return NULL; }
        l = mk_bin(OP_AND, l, r);
        if (!l) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
    }
    return l;
}

static Expr *parse_or(Lexer *lx, char *err)
{
    Expr *l = parse_and(lx, err);
    if (!l) return NULL;
    while (tok_is_kw(&lx->cur, "OR")) {
        if (adv(lx, err) < 0) { expr_free(l); return NULL; }
        Expr *r = parse_and(lx, err);
        if (!r) { expr_free(l); return NULL; }
        l = mk_bin(OP_OR, l, r);
        if (!l) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
    }
    return l;
}

/* Public entry: parse a standalone expression from text.  Used by the
 * executor to evaluate CHECK constraints stored in the catalog. */
Expr *parse_expr_text(const char *sql, char *err)
{
    Lexer lx;
    lex_init(&lx, sql);
    if (lex_next(&lx) < 0) {
        snprintf(err, MAX_ERR, "%s", lx.err);
        return NULL;
    }
    Expr *e = parse_or(&lx, err);
    if (!e) return NULL;
    if (lx.cur.kind != TK_EOF) {
        snprintf(err, MAX_ERR, "trailing tokens after expression on line %d",
                 lx.cur.line);
        expr_free(e);
        return NULL;
    }
    return e;
}

/* After CHECK (, copy the parenthesised expression into out as text, and
 * leave the lexer just past the closing ')'.  The expression is stored in
 * the catalog verbatim and re-parsed at enforcement time. */
static int parse_check_text(Lexer *lx, char *out, size_t cap, char *err)
{
    if (eat_kw(lx, "CHECK", err) < 0) return -1;
    if (eat_punct(lx, "(", err) < 0) return -1;
    size_t o = 0;
    int depth = 1;
    for (;;) {
        if (lx->cur.kind == TK_EOF)
            FAIL("unterminated CHECK constraint (missing ')')");
        if (tok_is_punct(&lx->cur, "(")) depth++;
        if (tok_is_punct(&lx->cur, ")")) {
            depth--;
            if (depth == 0) {
                if (adv(lx, err) < 0) return -1;
                out[o] = 0;
                return 0;
            }
        }
        const char *t = lx->cur.text;
        size_t tl = strlen(t);
        if (o && o + tl + 1 < cap) out[o++] = ' ';
        /* string literals arrive unquoted, so re-quote them (escaping any
         * embedded quotes the way the lexer unescaped them) */
        if (lx->cur.kind == TK_STRING) {
            if (o + 3 > cap) FAIL("CHECK constraint text is too long");
            out[o++] = '\'';
            for (size_t i = 0; i < tl && o + 2 < cap; i++) {
                if (t[i] == '\'') out[o++] = '\'';
                out[o++] = t[i];
            }
            out[o++] = '\'';
        } else {
            if (o + tl + 1 > cap) FAIL("CHECK constraint text is too long");
            memcpy(out + o, t, tl);
            o += tl;
        }
        if (adv(lx, err) < 0) return -1;
    }
}

/* ---------------------------------------------------------- data types */

/* Fills in the type, the integer width, the declared character limit and
 * whether the column needs date validation. All of that used to be parsed and
 * discarded, which is why NVARCHAR(5) would happily store a paragraph. */
static int parse_type(Lexer *lx, Column *c, char *err)
{
    if (lx->cur.kind != TK_IDENT)
        FAIL("expected a data type on line %d", lx->cur.line);

    const char *n = lx->cur.text;
    int is_datetime = 0, is_uuid = 0, want_len = 0;

    if (!strcasecmp(n, "INT") || !strcasecmp(n, "INTEGER")) {
        c->type = T_INT; c->sub = SUB_INT;
    } else if (!strcasecmp(n, "BIGINT")) {
        c->type = T_INT; c->sub = SUB_BIGINT;
    } else if (!strcasecmp(n, "SMALLINT")) {
        c->type = T_INT; c->sub = SUB_SMALLINT;
    } else if (!strcasecmp(n, "TINYINT")) {
        c->type = T_INT; c->sub = SUB_TINYINT;
    } else if (!strcasecmp(n, "FLOAT") || !strcasecmp(n, "REAL") ||
               !strcasecmp(n, "DECIMAL") || !strcasecmp(n, "NUMERIC") ||
               !strcasecmp(n, "MONEY")) {
        c->type = T_FLOAT;
    } else if (!strcasecmp(n, "BIT")) {
        c->type = T_BIT;
    } else if (!strcasecmp(n, "NVARCHAR") || !strcasecmp(n, "VARCHAR") ||
               !strcasecmp(n, "CHAR") || !strcasecmp(n, "NCHAR")) {
        c->type = T_TEXT; want_len = 1;
    } else if (!strcasecmp(n, "TEXT") || !strcasecmp(n, "NTEXT")) {
        c->type = T_TEXT;
    } else if (!strcasecmp(n, "DATETIME") || !strcasecmp(n, "DATE") ||
               !strcasecmp(n, "DATETIME2") || !strcasecmp(n, "SMALLDATETIME")) {
        c->type = T_TEXT; is_datetime = 1;
    } else if (!strcasecmp(n, "UNIQUEIDENTIFIER")) {
        c->type = T_TEXT; is_uuid = 1;
    } else {
        FAIL("unknown data type '%s' on line %d", n, lx->cur.line);
    }

    c->is_datetime = (uint8_t)is_datetime;
    if (is_datetime) c->maxlen = 19;          /* YYYY-MM-DD HH:MM:SS */
    if (is_uuid)     c->maxlen = 36;          /* canonical UUID text */

    if (adv(lx, err) < 0) return -1;

    /* an optional length or precision: (50), (18,2), (MAX) */
    if (tok_is_punct(&lx->cur, "(")) {
        if (adv(lx, err) < 0) return -1;
        if (tok_is_kw(&lx->cur, "MAX")) {
            c->maxlen = 0;                    /* unlimited, page size permitting */
            if (adv(lx, err) < 0) return -1;
        } else if (lx->cur.kind == TK_NUMBER && !lx->cur.is_float) {
            if (lx->cur.ival <= 0)
                FAIL("length for column '%s' must be positive", c->name);
            if (want_len || c->type == T_TEXT) c->maxlen = (uint32_t)lx->cur.ival;
            if (adv(lx, err) < 0) return -1;
            /* a scale, as in DECIMAL(18,2): accepted and ignored */
            if (tok_is_punct(&lx->cur, ",")) {
                if (adv(lx, err) < 0) return -1;
                if (lx->cur.kind != TK_NUMBER)
                    FAIL("expected a scale after ',' on line %d", lx->cur.line);
                if (adv(lx, err) < 0) return -1;
            }
        } else {
            FAIL("expected a length for '%s' on line %d", c->name, lx->cur.line);
        }
        if (eat_punct(lx, ")", err) < 0) return -1;
    }
    return 0;
}

/* ---------------------------------------------------------- statements */

/* AND a CHECK expression into the statement's table-level predicate. */
static void append_check(Stmt *s, const char *expr)
{
    if (s->check[0]) {
        size_t o = strlen(s->check);
        snprintf(s->check + o, sizeof s->check - o, " AND (%s)", expr);
    } else {
        snprintf(s->check, sizeof s->check, "(%s)", expr);
    }
}

/* CREATE VIEW name AS SELECT ... — the SELECT body is validated here and
 * stored verbatim in Stmt::view_sql for re-parsing at query time. */
static int parse_create_view(Lexer *lx, Stmt *s, char *err)
{
    s->kind = ST_CREATE_VIEW;
    if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
    if (eat_kw(lx, "AS", err) < 0) return -1;
    if (!tok_is_kw(&lx->cur, "SELECT"))
        FAIL("a view body must be a SELECT");

    const char *start = lx->src + lx->pos - strlen(lx->cur.text);
    if (adv(lx, err) < 0) return -1;

    Stmt probe;
    memset(&probe, 0, sizeof probe);
    probe.top = -1;
    if (parse_select(lx, &probe, err) < 0) return -1;
    stmt_free(&probe);

    const char *end = lx->src + lx->pos;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\n' || end[-1] == '\r'))
        end--;
    size_t n = (size_t)(end - start);
    if (n >= MAX_VIEW_SQL) {
        snprintf(err, MAX_ERR, "view body too long (max %d bytes)", MAX_VIEW_SQL);
        return -1;
    }
    memcpy(s->view_sql, start, n);
    s->view_sql[n] = 0;
    return 0;
}

/* Re-parse a stored view body into a SELECT statement. */
int parse_view_sql(const char *sql, Stmt *out, char *err)
{
    Lexer lx;
    lex_init(&lx, sql);
    if (lex_next(&lx) < 0) {
        snprintf(err, MAX_ERR, "%s", lx.err);
        return -1;
    }
    if (!tok_is_kw(&lx.cur, "SELECT")) {
        snprintf(err, MAX_ERR, "a view body must be a SELECT");
        return -1;
    }
    if (adv(&lx, err) < 0) return -1;
    memset(out, 0, sizeof *out);
    out->top = -1;
    return parse_select(&lx, out, err);
}

static int parse_stmt_nobound(Lexer *lx, Stmt *out, char *err);
static int parse_block_list(Lexer *lx, Stmt ***list, int *n, char *err);
static int parse_branch_statement(Lexer *lx, Stmt *s, char *err);

/* Append a heap-allocated statement to a statement list (cap MAX_PROG_STMTS). */
static int list_append(Stmt ***list, int *n, Stmt *st, char *err)
{
    if (*n >= MAX_PROG_STMTS) FAIL("procedure body too long (max %d statements)",
                                   MAX_PROG_STMTS);
    Stmt **grown = realloc(*list, (size_t)(*n + 1) * sizeof(Stmt *));
    if (!grown) FAIL("out of memory");
    *list = grown;
    (*list)[(*n)++] = st;
    return 0;
}

/* Parse one statement that may appear inside a procedure body: the control
 * flow statements (BEGIN block, IF, WHILE, BREAK, CONTINUE, RETURN, DECLARE,
 * SET) and cursor statements (DECLARE CURSOR, OPEN, FETCH, CLOSE,
 * DEALLOCATE), falling back to any regular SQL statement. Returns 1 = parsed,
 * 0 = end of input, -1 = error. */
static int parse_proc_statement(Lexer *lx, Stmt *s, char *err)
{
    Token *t = &lx->cur;

    /* BEGIN TRY ... END TRY BEGIN CATCH ... END CATCH */
    if (tok_is_kw(t, "BEGIN") && tok_is_kw(lex_peek(lx), "TRY")) {
        if (adv(lx, err) < 0) return -1;
        if (adv(lx, err) < 0) return -1;
        s->kind = ST_TRY;
        if (parse_block_list(lx, &s->stmts, &s->nstmts, err) < 0) return -1;
        if (eat_kw(lx, "TRY", err) < 0) return -1;
        if (!(tok_is_kw(&lx->cur, "BEGIN") && tok_is_kw(lex_peek(lx), "CATCH")))
            FAIL("expected BEGIN CATCH after END TRY");
        if (adv(lx, err) < 0) return -1;
        if (adv(lx, err) < 0) return -1;
        if (parse_block_list(lx, &s->else_stmts, &s->n_else, err) < 0) return -1;
        if (eat_kw(lx, "CATCH", err) < 0) return -1;
        return 1;
    }

    if (tok_is_kw(t, "BEGIN") &&
        !tok_is_kw(lex_peek(lx), "TRANSACTION") &&
        !tok_is_kw(lex_peek(lx), "TRAN") &&
        !tok_is_kw(lex_peek(lx), "TRY") &&
        !tok_is_kw(lex_peek(lx), "CATCH")) {
        if (adv(lx, err) < 0) return -1;
        s->kind = ST_BLOCK;
        if (parse_block_list(lx, &s->stmts, &s->nstmts, err) < 0) return -1;
        return 1;
    }

    if (tok_is_kw(t, "IF")) {
        if (adv(lx, err) < 0) return -1;
        s->kind = ST_IF;
        int paren = tok_is_punct(&lx->cur, "(");
        if (paren && adv(lx, err) < 0) return -1;
        s->where = parse_or(lx, err);
        if (!s->where) return -1;
        if (paren && !tok_is_punct(&lx->cur, ")")) {
            snprintf(err, MAX_ERR, "expected ')' after IF condition");
            return -1;
        }
        if (paren && adv(lx, err) < 0) return -1;
        Stmt *then_b = calloc(1, sizeof(Stmt));
        if (!then_b) FAIL("out of memory");
        then_b->top = -1;
        if (parse_branch_statement(lx, then_b, err) < 0) { free(then_b); return -1; }
        s->stmts = malloc(sizeof(Stmt *));
        if (!s->stmts) FAIL("out of memory");
        s->stmts[0] = then_b;
        s->nstmts = 1;
        /* optional ELSE (separators allowed between the branches) */
        for (;;) {
            if (tok_is_punct(&lx->cur, ";") || tok_is_kw(&lx->cur, "GO")) {
                if (adv(lx, err) < 0) return -1;
                continue;
            }
            break;
        }
        if (tok_is_kw(&lx->cur, "ELSE")) {
            if (adv(lx, err) < 0) return -1;
            Stmt *else_b = calloc(1, sizeof(Stmt));
            if (!else_b) FAIL("out of memory");
            else_b->top = -1;
            if (parse_branch_statement(lx, else_b, err) < 0) { free(else_b); return -1; }
            s->else_stmts = malloc(sizeof(Stmt *));
            if (!s->else_stmts) FAIL("out of memory");
            s->else_stmts[0] = else_b;
            s->n_else = 1;
        }
        return 1;
    }

    if (tok_is_kw(t, "WHILE")) {
        if (adv(lx, err) < 0) return -1;
        s->kind = ST_WHILE;
        int paren = tok_is_punct(&lx->cur, "(");
        if (paren && adv(lx, err) < 0) return -1;
        s->where = parse_or(lx, err);
        if (!s->where) return -1;
        if (paren && !tok_is_punct(&lx->cur, ")")) {
            snprintf(err, MAX_ERR, "expected ')' after WHILE condition");
            return -1;
        }
        if (paren && adv(lx, err) < 0) return -1;
        Stmt *body = calloc(1, sizeof(Stmt));
        if (!body) FAIL("out of memory");
        body->top = -1;
        if (parse_branch_statement(lx, body, err) < 0) { free(body); return -1; }
        s->stmts = malloc(sizeof(Stmt *));
        if (!s->stmts) FAIL("out of memory");
        s->stmts[0] = body;
        s->nstmts = 1;
        return 1;
    }

    if (tok_is_kw(t, "BREAK")) {
        s->kind = ST_BREAK;
        if (adv(lx, err) < 0) return -1;
        return 1;
    }
    if (tok_is_kw(t, "CONTINUE")) {
        s->kind = ST_CONTINUE;
        if (adv(lx, err) < 0) return -1;
        return 1;
    }
    if (tok_is_kw(t, "RETURN")) {
        s->kind = ST_RETURN;
        if (adv(lx, err) < 0) return -1;
        /* optional result expression, evaluated for its side effects only */
        if (!(lx->cur.kind == TK_EOF ||
              tok_is_punct(&lx->cur, ";") ||
              tok_is_kw(&lx->cur, "GO") ||
              tok_is_kw(&lx->cur, "END") ||
              tok_is_kw(&lx->cur, "ELSE"))) {
            s->sets[0].val = parse_or(lx, err);
            if (!s->sets[0].val) return -1;
            s->nsets = 1;
        }
        return 1;
    }

    if (tok_is_kw(t, "DECLARE")) {
        if (adv(lx, err) < 0) return -1;
        if (lx->cur.kind != TK_IDENT)
            FAIL("DECLARE needs a variable name starting with '@' or a cursor name");
        /* DECLARE name CURSOR FOR SELECT ... */
        if (tok_is_kw(lex_peek(lx), "CURSOR")) {
            s->kind = ST_DECLARE_CURSOR;
            if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
            if (eat_kw(lx, "CURSOR", err) < 0) return -1;
            if (eat_kw(lx, "FOR", err) < 0) return -1;
            if (!tok_is_kw(&lx->cur, "SELECT"))
                FAIL("a cursor must be FOR SELECT ...");
            s->derived = calloc(1, sizeof(Stmt));
            if (!s->derived) FAIL("out of memory");
            s->derived->top = -1;
            if (adv(lx, err) < 0) return -1;
            if (parse_select(lx, s->derived, err) < 0) return -1;
            return 1;
        }
        if (lx->cur.text[0] != '@')
            FAIL("DECLARE needs a variable name starting with '@'");
        s->kind = ST_DECLARE;
        if (take_ident(lx, s->sets[0].col, MAX_NAME, err) < 0) return -1;
        memset(&s->cols[0], 0, sizeof s->cols[0]);
        if (parse_type(lx, &s->cols[0], err) < 0) return -1;
        s->ncols = 1;
        if (tok_is_punct(&lx->cur, "=")) {
            if (adv(lx, err) < 0) return -1;
            s->sets[0].val = parse_or(lx, err);
            if (!s->sets[0].val) return -1;
            s->nsets = 1;
        }
        return 1;
    }

    if (tok_is_kw(t, "SET")) {
        if (adv(lx, err) < 0) return -1;
        s->kind = ST_SET;
        if (lx->cur.kind != TK_IDENT || lx->cur.text[0] != '@')
            FAIL("SET needs a variable name starting with '@'");
        if (take_ident(lx, s->sets[0].col, MAX_NAME, err) < 0) return -1;
        if (eat_punct(lx, "=", err) < 0) return -1;
        s->sets[0].val = parse_or(lx, err);
        if (!s->sets[0].val) return -1;
        s->nsets = 1;
        return 1;
    }

    if (tok_is_kw(t, "OPEN")) {
        s->kind = ST_OPEN;
        if (adv(lx, err) < 0) return -1;
        if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
        return 1;
    }
    if (tok_is_kw(t, "CLOSE")) {
        s->kind = ST_CLOSE;
        if (adv(lx, err) < 0) return -1;
        if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
        return 1;
    }
    if (tok_is_kw(t, "DEALLOCATE")) {
        s->kind = ST_DEALLOCATE;
        if (adv(lx, err) < 0) return -1;
        if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
        return 1;
    }
    if (tok_is_kw(t, "FETCH")) {
        if (adv(lx, err) < 0) return -1;
        s->kind = ST_FETCH;
        if (opt_kw(lx, "NEXT", err) < 0) return -1;
        if (eat_kw(lx, "FROM", err) < 0) return -1;
        if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
        if (eat_kw(lx, "INTO", err) < 0) return -1;
        for (;;) {
            if (s->n_ins_cols >= MAX_COLS)
                FAIL("too many FETCH INTO targets (max %d)", MAX_COLS);
            if (lx->cur.kind != TK_IDENT || lx->cur.text[0] != '@')
                FAIL("FETCH INTO targets must be variables starting with '@'");
            if (take_ident(lx, s->ins_cols[s->n_ins_cols], MAX_NAME, err) < 0) return -1;
            s->n_ins_cols++;
            int more = opt_punct(lx, ",", err);
            if (more < 0) return -1;
            if (!more) break;
        }
        return 1;
    }

    /* any regular SQL statement; it must end at a statement boundary or at a
     * keyword that the enclosing structure consumes (END, ELSE) */
    int rc = parse_stmt_nobound(lx, s, err);
    if (rc <= 0) return rc;
    if (s->kind == ST_EXPLAIN) return 1;
    if (!(lx->cur.kind == TK_EOF ||
          tok_is_punct(&lx->cur, ";") ||
          tok_is_kw(&lx->cur, "GO") ||
          tok_is_kw(&lx->cur, "END") ||
          tok_is_kw(&lx->cur, "ELSE"))) {
        snprintf(err, MAX_ERR,
                 "unexpected '%s' on line %d, after what otherwise looked like "
                 "a complete statement - this dialect may not support that "
                 "clause. Nothing was run.",
                 lx->cur.text, lx->cur.line);
        stmt_free(s);
        return -1;
    }
    return 1;
}

/* The body of IF/WHILE: either a BEGIN ... END block or a single statement. */
static int parse_branch_statement(Lexer *lx, Stmt *s, char *err)
{
    if (tok_is_kw(&lx->cur, "BEGIN") &&
        !tok_is_kw(lex_peek(lx), "TRANSACTION") &&
        !tok_is_kw(lex_peek(lx), "TRAN") &&
        !tok_is_kw(lex_peek(lx), "TRY") &&
        !tok_is_kw(lex_peek(lx), "CATCH")) {
        if (adv(lx, err) < 0) return -1;
        s->kind = ST_BLOCK;
        if (parse_block_list(lx, &s->stmts, &s->nstmts, err) < 0) return -1;
        return 0;
    }
    return parse_proc_statement(lx, s, err);
}

/* Parse statements until the matching END (which is consumed) or end of
 * input. Used for BEGIN ... END blocks and procedure bodies. */
static int parse_block_list(Lexer *lx, Stmt ***list, int *n, char *err)
{
    for (;;) {
        if (lx->cur.kind == TK_EOF) {
            snprintf(err, MAX_ERR, "expected END to close the block on line %d",
                     lx->cur.line);
            return -1;
        }
        if (tok_is_kw(&lx->cur, "END")) {
            if (adv(lx, err) < 0) return -1;
            return 1;   /* block closed */
        }
        if (tok_is_punct(&lx->cur, ";") || tok_is_kw(&lx->cur, "GO")) {
            if (adv(lx, err) < 0) return -1;
            continue;
        }
        Stmt *st = calloc(1, sizeof(Stmt));
        if (!st) FAIL("out of memory");
        st->top = -1;
        int rc = parse_proc_statement(lx, st, err);
        if (rc < 0) {
            stmt_free(st);
            free(st);
            return -1;
        }
        if (rc == 0) {
            free(st);
            snprintf(err, MAX_ERR, "expected END to close the block on line %d",
                     lx->cur.line);
            return -1;
        }
        if (list_append(list, n, st, err) < 0) {
            stmt_free(st);
            free(st);
            return -1;
        }
    }
}

/* Parse a procedure body in place from a lexer: either a BEGIN ... END block
 * or a single statement (legacy bodies). On success the lexer sits right after
 * the body, so the caller can slice the source text. */
static int parse_program_raw(Lexer *lx, Program *pgm, char *err)
{
    memset(pgm, 0, sizeof *pgm);
    if (lx->cur.kind == TK_EOF) return 0;
    if (tok_is_kw(&lx->cur, "BEGIN") &&
        !tok_is_kw(lex_peek(lx), "TRANSACTION") &&
        !tok_is_kw(lex_peek(lx), "TRAN") &&
        !tok_is_kw(lex_peek(lx), "TRY") &&
        !tok_is_kw(lex_peek(lx), "CATCH")) {
        if (adv(lx, err) < 0) return -1;
        return parse_block_list(lx, &pgm->stmts, &pgm->nstmts, err);
    }
    Stmt *st = calloc(1, sizeof(Stmt));
    if (!st) FAIL("out of memory");
    st->top = -1;
    int rc = parse_proc_statement(lx, st, err);
    if (rc < 0) {
        stmt_free(st);
        free(st);
        return -1;
    }
    if (rc == 0) {
        free(st);
        return 0;
    }
    if (list_append(&pgm->stmts, &pgm->nstmts, st, err) < 0) {
        stmt_free(st);
        free(st);
        return -1;
    }
    return 1;
}

/* Re-parse a stored procedure body into a program. */
int parse_program(const char *sql, Program *pgm, char *err)
{
    Lexer lx;
    lex_init(&lx, sql);
    if (lex_next(&lx) < 0) {
        snprintf(err, MAX_ERR, "%s", lx.err);
        return -1;
    }
    int rc = parse_program_raw(&lx, pgm, err);
    if (rc < 0) { prog_free(pgm); return -1; }
    if (rc == 0) {
        prog_free(pgm);
        snprintf(err, MAX_ERR, "a procedure body must be a statement or a BEGIN ... END block");
        return -1;
    }
    /* the body must be the whole text */
    while (tok_is_punct(&lx.cur, ";") || tok_is_kw(&lx.cur, "GO")) {
        if (adv(&lx, err) < 0) { prog_free(pgm); return -1; }
    }
    if (lx.cur.kind != TK_EOF) {
        prog_free(pgm);
        snprintf(err, MAX_ERR, "unexpected '%s' after the procedure body",
                 lx.cur.text);
        return -1;
    }
    return 0;
}

/* Re-parse a stored procedure parameter list "(@a INT = 5, @b NVARCHAR(10))"
 * into names and optional default expressions. Used at CALL time to build the
 * variable frame; defaults are evaluated in the callee frame, so they may
 * reference earlier parameters. The list was already validated when the
 * procedure was created. */
int proc_parse_params(const char *text, char names[][MAX_NAME], Expr *defs[],
                      int max, int *n, char *err)
{
    Lexer lx;
    lex_init(&lx, text);
    if (lex_next(&lx) < 0) {
        snprintf(err, MAX_ERR, "%s", lx.err);
        return -1;
    }
    *n = 0;
    if (eat_punct(&lx, "(", err) < 0) return -1;
    for (;;) {
        if (tok_is_punct(&lx.cur, ")")) {
            if (adv(&lx, err) < 0) return -1;
            break;
        }
        if (*n >= max) FAIL("too many parameters (max %d)", max);
        if (lx.cur.kind != TK_IDENT || lx.cur.text[0] != '@') {
            snprintf(err, MAX_ERR, "malformed stored parameter list '%s'", text);
            return -1;
        }
        if (take_ident(&lx, names[*n], MAX_NAME, err) < 0) return -1;
        Column pt;
        memset(&pt, 0, sizeof pt);
        if (parse_type(&lx, &pt, err) < 0) return -1;
        defs[*n] = NULL;
        if (tok_is_punct(&lx.cur, "=")) {
            if (adv(&lx, err) < 0) return -1;
            defs[*n] = parse_or(&lx, err);
            if (!defs[*n]) return -1;
        }
        /* OUTPUT is accepted (it is validated at CREATE time); the frame has
         * no notion of direction, only the caller's OUTPUT arguments matter */
        if (tok_is_kw(&lx.cur, "OUTPUT") || tok_is_kw(&lx.cur, "OUT")) {
            if (adv(&lx, err) < 0) return -1;
        }
        (*n)++;
        if (tok_is_punct(&lx.cur, ",")) {
            if (adv(&lx, err) < 0) return -1;
            continue;
        }
        if (tok_is_punct(&lx.cur, ")")) {
            if (adv(&lx, err) < 0) return -1;
            break;
        }
        snprintf(err, MAX_ERR, "malformed stored parameter list '%s'", text);
        return -1;
    }
    return 0;
}

/* CREATE PROCEDURE name [( @a TYPE, @b TYPE ... )] AS <body> — the parameter
 * list and the body are validated here and stored verbatim in Stmt::param_sql
 * and Stmt::proc_sql for re-parsing at CALL time. The body is either a
 * BEGIN ... END block or a single statement. */
static int parse_create_proc(Lexer *lx, Stmt *s, char *err)
{
    s->kind = ST_CREATE_PROC;
    if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;

    /* optional parameter list: (@a INT, @b NVARCHAR(10)) */
    if (tok_is_punct(&lx->cur, "(")) {
        const char *pstart = lx->src + lx->pos - 1;   /* include '(' */
        if (adv(lx, err) < 0) return -1;
        for (;;) {
            if (tok_is_punct(&lx->cur, ")")) break;   /* zero parameters */
            if (lx->cur.kind != TK_IDENT || lx->cur.text[0] != '@')
                FAIL("expected a parameter like @name TYPE");
            if (adv(lx, err) < 0) return -1;
            Column pt;
            memset(&pt, 0, sizeof pt);
            if (parse_type(lx, &pt, err) < 0) return -1;
            /* optional default: @a INT = 5 (stored verbatim, so only the
             * syntax needs validating here) */
            if (tok_is_punct(&lx->cur, "=")) {
                if (adv(lx, err) < 0) return -1;
                Expr *d = parse_or(lx, err);
                if (!d) return -1;
                expr_free(d);
            }
            /* optional OUTPUT marker: @b INT OUTPUT */
            if (tok_is_kw(&lx->cur, "OUTPUT") || tok_is_kw(&lx->cur, "OUT")) {
                if (adv(lx, err) < 0) return -1;
            }
            if (tok_is_punct(&lx->cur, ",")) {
                if (adv(lx, err) < 0) return -1;
                continue;
            }
            if (!tok_is_punct(&lx->cur, ")"))
                FAIL("expected ',' or ')' in the parameter list");
            break;
        }
        const char *pend = lx->src + lx->pos + 1;      /* include ')' */
        size_t pn = (size_t)(pend - pstart);
        if (pn >= MAX_PROC_SQL) {
            snprintf(err, MAX_ERR, "parameter list too long (max %d bytes)",
                     MAX_PROC_SQL);
            return -1;
        }
        memcpy(s->param_sql, pstart, pn);
        s->param_sql[pn] = 0;
        if (adv(lx, err) < 0) return -1;
    }

    if (eat_kw(lx, "AS", err) < 0) return -1;

    const char *start = lx->src + lx->pos - strlen(lx->cur.text);

    Program probe;
    int rc = parse_program_raw(lx, &probe, err);
    if (rc < 0) return -1;
    if (rc == 0) {
        prog_free(&probe);
        FAIL("a procedure body must be a statement or a BEGIN ... END block");
    }
    prog_free(&probe);

    const char *end = lx->src + lx->pos;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\n' || end[-1] == '\r'))
        end--;
    size_t n = (size_t)(end - start);
    if (n >= MAX_PROC_SQL) {
        snprintf(err, MAX_ERR, "procedure body too long (max %d bytes)", MAX_PROC_SQL);
        return -1;
    }
    memcpy(s->proc_sql, start, n);
    s->proc_sql[n] = 0;
    return 0;
}

static int parse_create(Lexer *lx, Stmt *s, char *err)
{
    if (eat_kw(lx, "TABLE", err) < 0) return -1;
    s->kind = ST_CREATE;
    if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
    if (eat_punct(lx, "(", err) < 0) return -1;

    for (;;) {
        /* Table-level constraint: CREATE TABLE t (a INT, CHECK (a > 0)) */
        if (tok_is_kw(&lx->cur, "CHECK")) {
            char tmp[512];
            if (parse_check_text(lx, tmp, sizeof tmp, err) < 0) return -1;
            append_check(s, tmp);
            int more = opt_punct(lx, ",", err);
            if (more < 0) return -1;
            if (!more) break;
            continue;
        }
        /* Table-level foreign key: FOREIGN KEY (a, b) REFERENCES p(x, y) */
        if (tok_is_kw(&lx->cur, "FOREIGN")) {
            if (adv(lx, err) < 0) return -1;
            if (eat_kw(lx, "KEY", err) < 0) return -1;
            if (s->nfks >= MAX_FKS) FAIL("too many FOREIGN KEYs (max %d)", MAX_FKS);
            FK *fk = &s->fks[s->nfks];
            memset(fk, 0, sizeof *fk);
            if (eat_punct(lx, "(", err) < 0) return -1;
            for (;;) {
                if (fk->ncols >= MAX_FK_COLS)
                    FAIL("too many columns in FOREIGN KEY (max %d)", MAX_FK_COLS);
                if (take_ident(lx, fk->cols[fk->ncols], MAX_NAME, err) < 0) return -1;
                fk->ncols++;
                int more = opt_punct(lx, ",", err);
                if (more < 0) return -1;
                if (!more) break;
            }
            if (eat_punct(lx, ")", err) < 0) return -1;
            if (eat_kw(lx, "REFERENCES", err) < 0) return -1;
            if (take_ident(lx, fk->table, MAX_NAME, err) < 0) return -1;
            if (tok_is_punct(&lx->cur, "(")) {
                if (adv(lx, err) < 0) return -1;
                for (int i = 0; i < fk->ncols; i++) {
                    if (take_ident(lx, fk->refs[i], MAX_NAME, err) < 0) return -1;
                    if (i + 1 < fk->ncols) {
                        if (eat_punct(lx, ",", err) < 0) return -1;
                    }
                }
                if (eat_punct(lx, ")", err) < 0) return -1;
            } else {
                for (int i = 0; i < fk->ncols; i++)
                    snprintf(fk->refs[i], MAX_NAME, "%s", "_pk");
            }
            if (tok_is_kw(&lx->cur, "ON")) {
                if (adv(lx, err) < 0) return -1;
                if (tok_is_kw(&lx->cur, "UPDATE"))
                    FAIL("ON UPDATE is not supported on a FOREIGN KEY");
                if (eat_kw(lx, "DELETE", err) < 0) return -1;
                if (tok_is_kw(&lx->cur, "CASCADE")) {
                    if (adv(lx, err) < 0) return -1;
                    fk->on_delete = FK_CASCADE;
                } else if (eat_kw(lx, "NO", err) < 0) {
                    return -1;
                } else if (eat_kw(lx, "ACTION", err) < 0) {
                    return -1;
                }
            }
            s->nfks++;
            int more = opt_punct(lx, ",", err);
            if (more < 0) return -1;
            if (!more) break;
            continue;
        }
        if (s->ncols >= MAX_COLS) FAIL("too many columns (max %d)", MAX_COLS);
        Column *c = &s->cols[s->ncols];
        memset(c, 0, sizeof *c);
        if (take_ident(lx, c->name, MAX_NAME, err) < 0) return -1;
        if (c->name[0] == '_')
            FAIL("column names beginning with '_' are reserved for memory "
                 "pseudo-columns");
        /* Two columns whose names match are indistinguishable everywhere else
         * in the engine, and an INSERT naming both silently misplaced data. */
        for (int k = 0; k < s->ncols; k++)
            if (strcasecmp(s->cols[k].name, c->name) == 0)
                FAIL("column '%s' is declared twice in table '%s'",
                     c->name, s->table);
        if (parse_type(lx, c, err) < 0) return -1;

        for (;;) {
            if (tok_is_kw(&lx->cur, "NOT")) {
                if (adv(lx, err) < 0) return -1;
                if (eat_kw(lx, "NULL", err) < 0) return -1;
                c->not_null = 1;
            } else if (tok_is_kw(&lx->cur, "NULL")) {
                if (adv(lx, err) < 0) return -1;
            } else if (tok_is_kw(&lx->cur, "PRIMARY")) {
                if (adv(lx, err) < 0) return -1;
                if (eat_kw(lx, "KEY", err) < 0) return -1;
                c->not_null = 1;
                c->unique = 1;
                c->is_pk = 1;
            } else if (tok_is_kw(&lx->cur, "UNIQUE")) {
                if (adv(lx, err) < 0) return -1;
                c->unique = 1;
            } else if (tok_is_kw(&lx->cur, "DEFAULT")) {
                if (adv(lx, err) < 0) return -1;
                if (tok_is_kw(&lx->cur, "GETDATE") ||
                    tok_is_kw(&lx->cur, "CURRENT_TIMESTAMP") ||
                    tok_is_kw(&lx->cur, "NEWID")) {
                    c->dflt = tok_is_kw(&lx->cur, "NEWID") ? DFLT_NEWID
                                                           : DFLT_GETDATE;
                    if (adv(lx, err) < 0) return -1;
                    /* the parentheses are optional in this position */
                    if (tok_is_punct(&lx->cur, "(")) {
                        if (adv(lx, err) < 0) return -1;
                        if (eat_punct(lx, ")", err) < 0) return -1;
                    }
                } else if (lx->cur.kind == TK_STRING ||
                           lx->cur.kind == TK_NUMBER ||
                           tok_is_kw(&lx->cur, "NULL")) {
                    /* Stored as text and coerced at insert time, so it can live
                     * in the catalog without carrying a parse tree around. */
                    c->dflt = DFLT_LITERAL;
                    if (tok_is_kw(&lx->cur, "NULL")) c->dflt = DFLT_NONE;
                    else snprintf(c->dflt_text, sizeof c->dflt_text, "%s",
                                  lx->cur.text);
                    if (adv(lx, err) < 0) return -1;
                } else {
                    FAIL("DEFAULT for '%s' must be a literal, GETDATE() or "
                         "NEWID() (line %d)", c->name, lx->cur.line);
                }
            } else if (tok_is_kw(&lx->cur, "IDENTITY")) {
                if (adv(lx, err) < 0) return -1;
                if (c->type != T_INT)
                    FAIL("IDENTITY only applies to integer columns, and '%s' "
                         "is not one", c->name);
                c->identity = 1;
                c->id_next = 1;
                c->id_step = 1;
                if (tok_is_punct(&lx->cur, "(")) {
                    if (adv(lx, err) < 0) return -1;
                    if (lx->cur.kind != TK_NUMBER || lx->cur.is_float)
                        FAIL("IDENTITY seed must be a whole number");
                    c->id_next = lx->cur.ival;
                    if (adv(lx, err) < 0) return -1;
                    if (eat_punct(lx, ",", err) < 0) return -1;
                    if (lx->cur.kind != TK_NUMBER || lx->cur.is_float)
                        FAIL("IDENTITY step must be a whole number");
                    c->id_step = lx->cur.ival;
                    if (c->id_step == 0) FAIL("IDENTITY step cannot be zero");
                    if (adv(lx, err) < 0) return -1;
                    if (eat_punct(lx, ")", err) < 0) return -1;
                }
            } else if (tok_is_kw(&lx->cur, "CHECK")) {
                /* column-level constraint: qty INT CHECK (qty > 0) */
                char tmp[512];
                if (parse_check_text(lx, tmp, sizeof tmp, err) < 0) return -1;
                append_check(s, tmp);
            } else if (tok_is_kw(&lx->cur, "REFERENCES")) {
                /* column-level foreign key: pid INT REFERENCES parent(id) */
                if (adv(lx, err) < 0) return -1;
                if (take_ident(lx, c->refs_table, MAX_NAME, err) < 0) return -1;
                if (tok_is_punct(&lx->cur, "(")) {
                    if (adv(lx, err) < 0) return -1;
                    if (take_ident(lx, c->refs_col, MAX_NAME, err) < 0) return -1;
                    if (eat_punct(lx, ")", err) < 0) return -1;
                } else {
                    /* REFERENCES parent without a column names the parent's
                     * primary key */
                    snprintf(c->refs_col, MAX_NAME, "%s", "_pk");
                }
                if (tok_is_kw(&lx->cur, "ON")) {
                    if (adv(lx, err) < 0) return -1;
                    if (tok_is_kw(&lx->cur, "UPDATE"))
                        FAIL("ON UPDATE is not supported on a FOREIGN KEY");
                    if (eat_kw(lx, "DELETE", err) < 0) return -1;
                    if (tok_is_kw(&lx->cur, "CASCADE")) {
                        if (adv(lx, err) < 0) return -1;
                        c->on_delete = FK_CASCADE;
                    } else if (eat_kw(lx, "NO", err) < 0) {
                        return -1;
                    } else if (eat_kw(lx, "ACTION", err) < 0) {
                        return -1;
                    }
                }
            } else {
                break;
            }
        }
        s->ncols++;

        int more = opt_punct(lx, ",", err);
        if (more < 0) return -1;
        if (!more) break;
    }
    return eat_punct(lx, ")", err);
}

static int parse_insert(Lexer *lx, Stmt *s, char *err)
{
    s->kind = ST_INSERT;
    if (opt_kw(lx, "INTO", err) < 0) return -1;
    if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;

    /* optional column list; distinguished from VALUES by the '(' */
    if (tok_is_punct(&lx->cur, "(")) {
        if (adv(lx, err) < 0) return -1;
        for (;;) {
            if (s->n_ins_cols >= MAX_COLS) FAIL("too many columns in INSERT");
            if (take_ident(lx, s->ins_cols[s->n_ins_cols], MAX_NAME, err) < 0) return -1;
            /* "INSERT INTO t (id, id) VALUES (9, 10)" stored 10 and left the
             * other column NULL, which is never what anyone means */
            for (int k = 0; k < s->n_ins_cols; k++)
                if (strcasecmp(s->ins_cols[k], s->ins_cols[s->n_ins_cols]) == 0)
                    FAIL("column '%s' is listed twice in the same INSERT",
                         s->ins_cols[s->n_ins_cols]);
            s->n_ins_cols++;
            int more = opt_punct(lx, ",", err);
            if (more < 0) return -1;
            if (!more) break;
        }
        if (eat_punct(lx, ")", err) < 0) return -1;
    }

    /* INSERT INTO t (cols) SELECT ... */
    if (tok_is_kw(&lx->cur, "SELECT")) {
        if (adv(lx, err) < 0) return -1;
        Stmt *sub = calloc(1, sizeof(Stmt));
        if (!sub) FAIL("out of memory");
        sub->top = -1;
        if (parse_select(lx, sub, err) < 0) {
            stmt_free(sub);
            free(sub);
            return -1;
        }
        s->sub = sub;
        return 0;
    }

    /* INSERT INTO t (cols) EXEC proc [args] — rows come from the
     * procedure's first result set */
    if (tok_is_kw(&lx->cur, "EXEC") || tok_is_kw(&lx->cur, "EXECUTE") ||
        tok_is_kw(&lx->cur, "CALL")) {
        Stmt *sub = calloc(1, sizeof(Stmt));
        if (!sub) FAIL("out of memory");
        sub->top = -1;
        if (parse_stmt_nobound(lx, sub, err) < 0) {
            stmt_free(sub);
            free(sub);
            return -1;
        }
        if (sub->kind != ST_CALL) {
            stmt_free(sub);
            free(sub);
            FAIL("INSERT ... EXEC expects a procedure call");
        }
        s->sub = sub;
        return 0;
    }

    if (eat_kw(lx, "VALUES", err) < 0) return -1;

    for (;;) {
        if (s->nrows >= 64) FAIL("at most 64 rows per INSERT statement");
        if (eat_punct(lx, "(", err) < 0) return -1;
        int w = 0;
        for (;;) {
            if (w >= MAX_COLS) FAIL("too many values in a row");
            Expr *e = parse_or(lx, err);
            if (!e) return -1;
            s->rows[s->nrows][w++] = e;
            s->row_width[s->nrows] = w;
            int more = opt_punct(lx, ",", err);
            if (more < 0) return -1;
            if (!more) break;
        }
        if (eat_punct(lx, ")", err) < 0) return -1;
        s->nrows++;
        int more = opt_punct(lx, ",", err);
        if (more < 0) return -1;
        if (!more) break;
    }
    return 0;
}

static int parse_top(Lexer *lx, Stmt *s, char *err)
{
    if (!tok_is_kw(&lx->cur, "TOP")) return 0;
    if (adv(lx, err) < 0) return -1;
    int paren = opt_punct(lx, "(", err);
    if (paren < 0) return -1;
    if (lx->cur.kind != TK_NUMBER || lx->cur.is_float)
        FAIL("TOP needs a whole number on line %d", lx->cur.line);
    s->top = (int)lx->cur.ival;
    if (s->top < 0) s->top = 0;
    if (adv(lx, err) < 0) return -1;
    if (paren && eat_punct(lx, ")", err) < 0) return -1;
    return 0;
}

/* ORDER BY expr [ASC|DESC] [, expr [ASC|DESC]]... */
static int parse_order_by(Lexer *lx, Stmt *s, char *err)
{
    if (!tok_is_kw(&lx->cur, "ORDER")) return 0;
    if (adv(lx, err) < 0) return -1;
    if (eat_kw(lx, "BY", err) < 0) return -1;

    for (;;) {
        if (s->norder >= MAX_ORDER_KEYS)
            FAIL("at most %d ORDER BY keys", MAX_ORDER_KEYS);
        Expr *e = parse_or(lx, err);
        if (!e) return -1;
        s->order[s->norder].e = e;
        s->order[s->norder].desc = 0;
        if (tok_is_kw(&lx->cur, "DESC")) {
            s->order[s->norder].desc = 1;
            if (adv(lx, err) < 0) return -1;
        } else if (tok_is_kw(&lx->cur, "ASC")) {
            if (adv(lx, err) < 0) return -1;
        }
        s->norder++;
        int more = opt_punct(lx, ",", err);
        if (more < 0) return -1;
        if (!more) break;
    }
    return 0;
}

static int parse_group_by(Lexer *lx, Stmt *s, char *err)
{
    if (!tok_is_kw(&lx->cur, "GROUP")) return 0;
    if (adv(lx, err) < 0) return -1;
    if (eat_kw(lx, "BY", err) < 0) return -1;

    for (;;) {
        if (s->ngroup >= MAX_GROUP_KEYS)
            FAIL("at most %d GROUP BY keys", MAX_GROUP_KEYS);
        Expr *e = parse_or(lx, err);
        if (!e) return -1;
        s->group[s->ngroup++] = e;
        int more = opt_punct(lx, ",", err);
        if (more < 0) return -1;
        if (!more) break;
    }

    if (tok_is_kw(&lx->cur, "HAVING")) {
        if (adv(lx, err) < 0) return -1;
        s->having = parse_or(lx, err);
        if (!s->having) return -1;
    }
    return 0;
}

static int parse_where(Lexer *lx, Stmt *s, char *err)
{
    if (!tok_is_kw(&lx->cur, "WHERE")) return 0;
    if (adv(lx, err) < 0) return -1;
    s->where = parse_or(lx, err);
    return s->where ? 0 : -1;
}

/* Build a column heading for an unaliased expression, the way a SQL shell does:
 * a bare column keeps its name, anything else gets a rendering of the call. */
static void label_for(const Expr *e, char *out, size_t cap)
{
    switch (e->kind) {
    case EX_COL:  snprintf(out, cap, "%s", e->col); break;
    case EX_AGG:
        if (e->agg_star) snprintf(out, cap, "%s(*)", e->fname);
        else if (e->nargs == 1 && e->args[0]->kind == EX_COL)
            snprintf(out, cap, "%s(%s)", e->fname, e->args[0]->col);
        else snprintf(out, cap, "%s(...)", e->fname);
        break;
    case EX_FUNC:
        if (e->nargs == 1 && e->args[0]->kind == EX_COL)
            snprintf(out, cap, "%s(%s)", e->fname, e->args[0]->col);
        else snprintf(out, cap, "%s(...)", e->fname);
        break;
    case EX_CAST: snprintf(out, cap, "cast"); break;
    case EX_CASE: snprintf(out, cap, "case"); break;
    case EX_LIT:  snprintf(out, cap, "value"); break;
    default:      snprintf(out, cap, "expr"); break;
    }
}

/* One SELECT operand of a compound query (or a plain SELECT when no set
 * operator follows): DISTINCT/TOP, select list, FROM/JOIN, WHERE, GROUP BY,
 * HAVING.  ORDER BY and LIMIT/OFFSET are handled by parse_select so they
 * bind to the whole compound result. */
static int parse_select_core(Lexer *lx, Stmt *s, char *err)
{
    s->kind = ST_SELECT;
    s->top = -1;
    s->limit = -1;
    s->offset = 0;

    if (tok_is_kw(&lx->cur, "DISTINCT")) {
        s->distinct = 1;
        if (adv(lx, err) < 0) return -1;
    } else if (tok_is_kw(&lx->cur, "ALL")) {
        if (adv(lx, err) < 0) return -1;
    }
    if (parse_top(lx, s, err) < 0) return -1;

    for (;;) {
        if (s->nitems >= MAX_SELECT_ITEMS) FAIL("too many items in select list");
        SelItem *it = &s->items[s->nitems];
        memset(it, 0, sizeof *it);

        if (tok_is_punct(&lx->cur, "*")) {
            it->is_star = 1;
            if (adv(lx, err) < 0) return -1;
        } else {
            /* Reset star-qualifier flag before parsing expression */
            g_sel_star_qual = 0;
            it->e = parse_or(lx, err);
            if (!it->e) return -1;
            /* Check if parse_primary encountered "table.*" */
            if (g_sel_star_qual) {
                it->col_star = 1;
                snprintf(it->col_table, MAX_NAME, "%s", g_sel_star_name);
                expr_free(it->e);
                it->e = NULL;
                g_sel_star_qual = 0;
            } else {
                label_for(it->e, it->label, MAX_NAME);
            }
        }

        if (tok_is_kw(&lx->cur, "AS")) {
            if (adv(lx, err) < 0) return -1;
            if (take_ident(lx, it->alias, MAX_NAME, err) < 0) return -1;
        } else if (lx->cur.kind == TK_IDENT && !it->is_star && !it->col_star &&
                   !tok_is_kw(&lx->cur, "FROM") &&
                   !tok_is_kw(&lx->cur, "GROUP") &&
                   !tok_is_kw(&lx->cur, "ORDER") &&
                   !tok_is_kw(&lx->cur, "HAVING") &&
                   !tok_is_kw(&lx->cur, "UNION") &&
                   !tok_is_kw(&lx->cur, "INTERSECT") &&
                   !tok_is_kw(&lx->cur, "EXCEPT") &&
                   !tok_is_kw(&lx->cur, "LIMIT") &&
                   !tok_is_kw(&lx->cur, "OFFSET") &&
                   !tok_is_kw(&lx->cur, "END") &&
                   !tok_is_kw(&lx->cur, "ELSE") &&
                   !tok_is_kw(&lx->cur, "GO")) {
            /* the bare-word alias form: SELECT n total FROM t */
            if (take_ident(lx, it->alias, MAX_NAME, err) < 0) return -1;
        }
        s->nitems++;

        int more = opt_punct(lx, ",", err);
        if (more < 0) return -1;
        if (!more) break;
    }

    /* FROM is optional, so SELECT 1 and SELECT NEWID() work */
    if (tok_is_kw(&lx->cur, "FROM")) {
        if (adv(lx, err) < 0) return -1;
        /* FROM (SELECT ...) AS alias — derived table */
        if (tok_is_punct(&lx->cur, "(")) {
            if (adv(lx, err) < 0) return -1;
            if (!tok_is_kw(&lx->cur, "SELECT"))
                FAIL("expected SELECT after '(' in FROM clause for derived table");
            if (adv(lx, err) < 0) return -1;
            s->derived = calloc(1, sizeof(struct Stmt));
            if (!s->derived) FAIL("out of memory");
            if (parse_select(lx, s->derived, err) < 0) return -1;
            if (eat_punct(lx, ")", err) < 0) return -1;
            if (tok_is_kw(&lx->cur, "AS")) {
                if (adv(lx, err) < 0) return -1;
            }
            if (lx->cur.kind != TK_IDENT ||
                tok_is_kw(&lx->cur, "WHERE") ||
                tok_is_kw(&lx->cur, "GROUP") ||
                tok_is_kw(&lx->cur, "ORDER") ||
                tok_is_kw(&lx->cur, "HAVING") ||
                tok_is_kw(&lx->cur, "UNION") ||
                tok_is_kw(&lx->cur, "INTERSECT") ||
                tok_is_kw(&lx->cur, "EXCEPT") ||
                tok_is_kw(&lx->cur, "LIMIT") ||
                tok_is_kw(&lx->cur, "OFFSET") ||
                tok_is_kw(&lx->cur, "GO")) {
                FAIL("derived table requires an alias, e.g. FROM (SELECT ...) AS t");
            }
            if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
            snprintf(s->alias, MAX_NAME, "%s", s->table);
            s->has_from = 1;
            goto after_derived_from;
        }
        if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
        /* optional alias for the primary table */
        if (lx->cur.kind == TK_IDENT &&
            !tok_is_kw(&lx->cur, "WHERE") &&
            !tok_is_kw(&lx->cur, "GROUP") &&
            !tok_is_kw(&lx->cur, "ORDER") &&
            !tok_is_kw(&lx->cur, "HAVING") &&
            !tok_is_kw(&lx->cur, "UNION") &&
            !tok_is_kw(&lx->cur, "INTERSECT") &&
            !tok_is_kw(&lx->cur, "EXCEPT") &&
            !tok_is_kw(&lx->cur, "LIMIT") &&
            !tok_is_kw(&lx->cur, "OFFSET") &&
            !tok_is_kw(&lx->cur, "INNER") &&
            !tok_is_kw(&lx->cur, "LEFT") &&
            !tok_is_kw(&lx->cur, "JOIN") &&
            !tok_is_kw(&lx->cur, "ON") &&
            !tok_is_kw(&lx->cur, "END") &&
            !tok_is_kw(&lx->cur, "ELSE") &&
            !tok_is_kw(&lx->cur, "GO")) {
            if (take_ident(lx, s->alias, MAX_NAME, err) < 0) return -1;
        }
        /* parse JOIN clauses */
        while (tok_is_kw(&lx->cur, "JOIN") ||
               tok_is_kw(&lx->cur, "INNER") ||
               tok_is_kw(&lx->cur, "LEFT")) {
            if (s->njoins >= MAX_JOINS) FAIL("too many JOINs (max %d)", MAX_JOINS);
            Join *j = &s->joins[s->njoins];

            if (tok_is_kw(&lx->cur, "LEFT")) {
                j->type = JOIN_LEFT;
                if (adv(lx, err) < 0) return -1;
            } else {
                j->type = JOIN_INNER;
            }
            if (tok_is_kw(&lx->cur, "INNER")) {
                if (adv(lx, err) < 0) return -1;
            }
            if (eat_kw(lx, "JOIN", err) < 0) return -1;
            if (take_ident(lx, j->table, MAX_NAME, err) < 0) return -1;
            /* optional alias */
            if (lx->cur.kind == TK_IDENT &&
                !tok_is_kw(&lx->cur, "ON") &&
                !tok_is_kw(&lx->cur, "WHERE") &&
                !tok_is_kw(&lx->cur, "GROUP") &&
                !tok_is_kw(&lx->cur, "ORDER") &&
                !tok_is_kw(&lx->cur, "HAVING") &&
                !tok_is_kw(&lx->cur, "UNION") &&
                !tok_is_kw(&lx->cur, "INTERSECT") &&
                !tok_is_kw(&lx->cur, "EXCEPT") &&
                !tok_is_kw(&lx->cur, "LIMIT") &&
                !tok_is_kw(&lx->cur, "OFFSET") &&
                !tok_is_kw(&lx->cur, "GO")) {
                if (take_ident(lx, j->alias, MAX_NAME, err) < 0) return -1;
            }
            if (eat_kw(lx, "ON", err) < 0) return -1;
            j->on = parse_or(lx, err);
            if (!j->on) return -1;
            s->njoins++;
        }
        s->has_from = 1;
after_derived_from:
        ;
    } else {
        for (int i = 0; i < s->nitems; i++)
            if (s->items[i].is_star)
                FAIL("SELECT * needs a FROM clause");
    }

    if (parse_where(lx, s, err) < 0) return -1;
    if (parse_group_by(lx, s, err) < 0) return -1;
    if (tok_is_kw(&lx->cur, "HAVING")) {
        if (adv(lx, err) < 0) return -1;
        s->having = parse_or(lx, err);
        if (!s->having) return -1;
    }

    if (!s->has_from && (s->where || s->ngroup || s->having))
        FAIL("WHERE, GROUP BY and HAVING need a FROM clause");
    return 0;
}

static int parse_select(Lexer *lx, Stmt *s, char *err)
{
    Stmt *root = s;
    if (parse_select_core(lx, root, err) < 0) return -1;

    /* UNION [ALL] / INTERSECT / EXCEPT, left-associative: each operator
     * binds to the previous operand, so (a UNION b) INTERSECT c. */
    for (;;) {
        int op = 0;
        if      (tok_is_kw(&lx->cur, "UNION"))     op = SET_UNION;
        else if (tok_is_kw(&lx->cur, "INTERSECT")) op = SET_INTERSECT;
        else if (tok_is_kw(&lx->cur, "EXCEPT"))    op = SET_EXCEPT;
        else break;
        if (adv(lx, err) < 0) return -1;
        if (op == SET_UNION && tok_is_kw(&lx->cur, "ALL")) {
            if (adv(lx, err) < 0) return -1;
            op = SET_UNION_ALL;
        }
        if (!tok_is_kw(&lx->cur, "SELECT"))
            FAIL("expected SELECT after %s",
                 op == SET_UNION_ALL ? "UNION ALL"
                 : op == SET_UNION ? "UNION"
                 : op == SET_INTERSECT ? "INTERSECT" : "EXCEPT");
        if (adv(lx, err) < 0) return -1;
        Stmt *rhs = calloc(1, sizeof(Stmt));
        if (!rhs) FAIL("out of memory");
        if (parse_select_core(lx, rhs, err) < 0) {
            free(rhs);
            return -1;
        }
        s->set_op = op;
        s->set_rhs = rhs;
        s = rhs;
    }

    /* Trailing ORDER BY / LIMIT / OFFSET apply to the compound result */
    if (parse_order_by(lx, root, err) < 0) return -1;

    /* LIMIT n [OFFSET m] or bare OFFSET m — pagination after ORDER BY */
    if (tok_is_kw(&lx->cur, "LIMIT") || tok_is_kw(&lx->cur, "OFFSET")) {
        if (tok_is_kw(&lx->cur, "LIMIT")) {
            if (adv(lx, err) < 0) return -1;
            if (lx->cur.kind != TK_NUMBER || lx->cur.is_float)
                FAIL("LIMIT needs a whole number on line %d", lx->cur.line);
            root->limit = (int)lx->cur.ival;
            if (adv(lx, err) < 0) return -1;
            if (root->limit < 0)
                FAIL("LIMIT cannot be negative on line %d", lx->cur.line);
        }
        if (tok_is_kw(&lx->cur, "OFFSET")) {
            if (adv(lx, err) < 0) return -1;
            if (lx->cur.kind != TK_NUMBER || lx->cur.is_float)
                FAIL("OFFSET needs a whole number on line %d", lx->cur.line);
            root->offset = (int)lx->cur.ival;
            if (adv(lx, err) < 0) return -1;
            if (root->offset < 0)
                FAIL("OFFSET cannot be negative on line %d", lx->cur.line);
        }
    }

    return 0;
}

static int parse_update(Lexer *lx, Stmt *s, char *err)
{
    s->kind = ST_UPDATE;
    if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
    if (eat_kw(lx, "SET", err) < 0) return -1;
    for (;;) {
        if (s->nsets >= MAX_SET_ITEMS) FAIL("too many assignments in SET");
        SetItem *si = &s->sets[s->nsets];
        memset(si, 0, sizeof *si);
        if (take_ident(lx, si->col, MAX_NAME, err) < 0) return -1;
        if (eat_punct(lx, "=", err) < 0) return -1;
        si->val = parse_or(lx, err);
        if (!si->val) return -1;
        s->nsets++;
        int more = opt_punct(lx, ",", err);
        if (more < 0) return -1;
        if (!more) break;
    }
    return parse_where(lx, s, err);
}

static int parse_delete(Lexer *lx, Stmt *s, char *err)
{
    s->kind = ST_DELETE;
    if (opt_kw(lx, "FROM", err) < 0) return -1;
    if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
    return parse_where(lx, s, err);
}

static int parse_recall(Lexer *lx, Stmt *s, char *err)
{
    s->kind = ST_RECALL;
    s->top = -1;
    if (parse_top(lx, s, err) < 0) return -1;
    if (lx->cur.kind != TK_STRING)
        FAIL("RECALL expects a quoted phrase, e.g. RECALL 'coffee receipts'");
    snprintf(s->recall_text, sizeof s->recall_text, "%s", lx->cur.text);
    if (adv(lx, err) < 0) return -1;
    if (tok_is_kw(&lx->cur, "FROM")) {
        if (adv(lx, err) < 0) return -1;
        if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
    }
    if (parse_top(lx, s, err) < 0) return -1;
    return 0;
}

static int parse_remember(Lexer *lx, Stmt *s, StKind kind, char *err)
{
    s->kind = kind;
    if (opt_kw(lx, "FROM", err) < 0) return -1;
    if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
    return parse_where(lx, s, err);
}

static int parse_show(Lexer *lx, Stmt *s, char *err)
{
    s->top = -1;
    if (tok_is_kw(&lx->cur, "TABLES")) {
        s->kind = ST_SHOW_TABLES;
        return adv(lx, err);
    }
    if (tok_is_kw(&lx->cur, "MEMORY") || tok_is_kw(&lx->cur, "LINKS")) {
        s->kind = tok_is_kw(&lx->cur, "MEMORY") ? ST_SHOW_MEMORY : ST_SHOW_LINKS;
        if (adv(lx, err) < 0) return -1;
        if (parse_top(lx, s, err) < 0) return -1;
    }
    if (tok_is_kw(&lx->cur, "FROM")) {
        if (adv(lx, err) < 0) return -1;
        if (take_ident(lx, s->table, MAX_NAME, err) < 0) return -1;
    } else if (s->kind == ST_SHOW_MEMORY) {
        FAIL("SHOW MEMORY needs a table: SHOW MEMORY FROM <table>");
    }
    if (parse_top(lx, s, err) < 0) return -1;
    return 0;
}

/* --------------------------------------------------------- entry point */

/* parse one statement without any trailing-garbage boundary check. This is
 * the common core of batch statements and procedure-body statements; the
 * callers add their own boundary rules. */
/* One CALL/EXEC argument: [@name =] <expr> [OUTPUT]. The named form is
 * detected before parse_or runs, so the '=' in "@a = 1" is not misread as a
 * comparison. */
static int parse_one_call_arg(Lexer *lx, Stmt *out, char *err)
{
    if (out->nargs >= MAX_FUNC_ARGS)
        FAIL("too many CALL arguments (max %d)", MAX_FUNC_ARGS);
    int i = out->nargs;
    if (lx->cur.kind == TK_IDENT && lx->cur.text[0] == '@' &&
        tok_is_punct(lex_peek(lx), "=")) {
        snprintf(out->argnames[i], MAX_NAME, "%s", lx->cur.text);
        if (adv(lx, err) < 0) return -1;
        if (adv(lx, err) < 0) return -1;
    }
    out->args[i] = parse_or(lx, err);
    if (!out->args[i]) return -1;
    out->nargs++;
    if (tok_is_kw(&lx->cur, "OUTPUT") || tok_is_kw(&lx->cur, "OUT")) {
        if (out->args[i]->kind != EX_VAR) {
            snprintf(err, MAX_ERR, "an OUTPUT argument must be a variable");
            return -1;
        }
        out->argout[i] = 1;
        if (adv(lx, err) < 0) return -1;
    }
    return 0;
}

/* Procedure arguments: CALL p(1, @x OUTPUT, @a = 2) or the parenthesised form
 * EXEC p(1) — and bare form EXEC p 1, 'x' — arguments run up to the statement
 * terminator (or END/ELSE inside a procedure body). */
static int parse_call_args(Lexer *lx, Stmt *out, char *err)
{
    if (tok_is_punct(&lx->cur, "(")) {
        if (adv(lx, err) < 0) return -1;
        if (tok_is_punct(&lx->cur, ")")) {
            if (adv(lx, err) < 0) return -1;
            return 0;
        }
        for (;;) {
            if (parse_one_call_arg(lx, out, err) < 0) return -1;
            int more = opt_punct(lx, ",", err);
            if (more < 0) return -1;
            if (!more) break;
        }
        return eat_punct(lx, ")", err);
    }
    for (;;) {
        if (lx->cur.kind == TK_EOF || tok_is_punct(&lx->cur, ";") ||
            tok_is_kw(&lx->cur, "GO") || tok_is_kw(&lx->cur, "END") ||
            tok_is_kw(&lx->cur, "ELSE"))
            return 0;
        if (parse_one_call_arg(lx, out, err) < 0) return -1;
        int more = opt_punct(lx, ",", err);
        if (more < 0) return -1;
        if (!more) return 0;
    }
}

static int parse_stmt_nobound(Lexer *lx, Stmt *out, char *err)
{
    memset(out, 0, sizeof *out);
    out->top = -1;

    /* skip statement separators and batch terminators */
    for (;;) {
        if (lx->cur.kind == TK_EOF) return 0;
        if (tok_is_punct(&lx->cur, ";") || tok_is_kw(&lx->cur, "GO")) {
            if (adv(lx, err) < 0) return -1;
            continue;
        }
        break;
    }

    Token *t = &lx->cur;
    int rc;

    if (tok_is_kw(t, "CREATE")) {
        if (adv(lx, err) < 0) return -1;
        if (tok_is_kw(&lx->cur, "VIEW")) {
            if (adv(lx, err) < 0) return -1;
            rc = parse_create_view(lx, out, err);
        } else if (tok_is_kw(&lx->cur, "PROCEDURE")) {
            if (adv(lx, err) < 0) return -1;
            rc = parse_create_proc(lx, out, err);
        } else {
            rc = parse_create(lx, out, err);
        }
    }
    else if (tok_is_kw(t, "DROP")) {
        if (adv(lx, err) < 0) return -1;
        if (tok_is_kw(&lx->cur, "VIEW")) {
            if (adv(lx, err) < 0) return -1;
            out->kind = ST_DROP_VIEW;
            if (tok_is_kw(&lx->cur, "IF")) {
                if (adv(lx, err) < 0) return -1;
                if (eat_kw(lx, "EXISTS", err) < 0) return -1;
                out->if_exists = 1;
            }
            rc = take_ident(lx, out->table, MAX_NAME, err);
        } else if (tok_is_kw(&lx->cur, "PROCEDURE")) {
            if (adv(lx, err) < 0) return -1;
            out->kind = ST_DROP_PROC;
            if (tok_is_kw(&lx->cur, "IF")) {
                if (adv(lx, err) < 0) return -1;
                if (eat_kw(lx, "EXISTS", err) < 0) return -1;
                out->if_exists = 1;
            }
            rc = take_ident(lx, out->table, MAX_NAME, err);
        } else {
            if (eat_kw(lx, "TABLE", err) < 0) return -1;
            out->kind = ST_DROP;
            if (tok_is_kw(&lx->cur, "IF")) {
                if (adv(lx, err) < 0) return -1;
                if (eat_kw(lx, "EXISTS", err) < 0) return -1;
                out->if_exists = 1;
            }
            rc = take_ident(lx, out->table, MAX_NAME, err);
        }
    }
    else if (tok_is_kw(t, "CALL") || tok_is_kw(t, "EXEC") || tok_is_kw(t, "EXECUTE")) {
        int exec_form = !tok_is_kw(t, "CALL");
        if (adv(lx, err) < 0) return -1;
        out->kind = ST_CALL;
        if (exec_form) {
            /* EXEC @rc = proc ... — the return-code target */
            if (lx->cur.kind == TK_IDENT && lx->cur.text[0] == '@' &&
                tok_is_punct(lex_peek(lx), "=")) {
                snprintf(out->retvar, MAX_NAME, "%s", lx->cur.text);
                if (adv(lx, err) < 0) return -1;
                if (adv(lx, err) < 0) return -1;
            }
            /* EXEC ('...') / EXEC @rc = ('...') — dynamic SQL */
            if (tok_is_punct(&lx->cur, "(")) {
                if (adv(lx, err) < 0) return -1;
                out->kind = ST_EXEC_SQL;
                out->sets[0].val = parse_or(lx, err);
                if (!out->sets[0].val) return -1;
                out->nsets = 1;
                if (eat_punct(lx, ")", err) < 0) return -1;
                rc = 0;
                goto exec_done;
            }
        }
        rc = take_ident(lx, out->table, MAX_NAME, err);
        if (rc < 0) return rc;
        if (parse_call_args(lx, out, err) < 0) return -1;
        rc = 0;
    exec_done:
        ;
    }
    else if (tok_is_kw(t, "ALTER")) {
        if (adv(lx, err) < 0) return -1;
        if (eat_kw(lx, "TABLE", err) < 0) return -1;
        if (take_ident(lx, out->table, MAX_NAME, err) < 0) return -1;
        if (tok_is_kw(&lx->cur, "ADD")) {
            if (adv(lx, err) < 0) return -1;
            if (opt_kw(lx, "COLUMN", err) < 0) return -1;
            out->kind = ST_ALTER_ADD;
            Column *c = &out->cols[0];
            memset(c, 0, sizeof *c);
            if (take_ident(lx, c->name, MAX_NAME, err) < 0) return -1;
            if (c->name[0] == '_')
                FAIL("column names beginning with '_' are reserved");
            if (parse_type(lx, c, err) < 0) return -1;
            out->ncols = 1;
            /* an added column has to be nullable or have a default: existing
             * rows cannot retroactively supply a value */
            for (;;) {
                if (tok_is_kw(&lx->cur, "NOT")) {
                    if (adv(lx, err) < 0) return -1;
                    if (eat_kw(lx, "NULL", err) < 0) return -1;
                    c->not_null = 1;
                } else if (tok_is_kw(&lx->cur, "NULL")) {
                    if (adv(lx, err) < 0) return -1;
                } else if (tok_is_kw(&lx->cur, "UNIQUE")) {
                    if (adv(lx, err) < 0) return -1;
                    c->unique = 1;
                } else if (tok_is_kw(&lx->cur, "DEFAULT")) {
                    if (adv(lx, err) < 0) return -1;
                    if (lx->cur.kind != TK_STRING && lx->cur.kind != TK_NUMBER)
                        FAIL("DEFAULT on an added column must be a literal");
                    c->dflt = DFLT_LITERAL;
                    snprintf(c->dflt_text, sizeof c->dflt_text, "%s", lx->cur.text);
                    if (adv(lx, err) < 0) return -1;
                } else break;
            }
            rc = 0;
        } else if (tok_is_kw(&lx->cur, "DROP")) {
            if (adv(lx, err) < 0) return -1;
            if (opt_kw(lx, "COLUMN", err) < 0) return -1;
            out->kind = ST_ALTER_DROP;
            rc = take_ident(lx, out->cols[0].name, MAX_NAME, err);
            out->ncols = 1;
        } else if (tok_is_kw(&lx->cur, "RENAME")) {
            if (adv(lx, err) < 0) return -1;
            if (opt_kw(lx, "COLUMN", err) < 0) return -1;
            out->kind = ST_ALTER_RENAME;
            if (take_ident(lx, out->cols[0].name, MAX_NAME, err) < 0) return -1;
            if (eat_kw(lx, "TO", err) < 0) return -1;
            rc = take_ident(lx, out->cols[1].name, MAX_NAME, err);
            out->ncols = 2;
        } else if (tok_is_kw(&lx->cur, "ALTER")) {
            if (adv(lx, err) < 0) return -1;
            if (opt_kw(lx, "COLUMN", err) < 0) return -1;
            out->kind = ST_ALTER_TYPE;
            memset(&out->cols[0], 0, sizeof(Column));
            if (take_ident(lx, out->cols[0].name, MAX_NAME, err) < 0) return -1;
            opt_kw(lx, "TYPE", err);   /* optional: ALTER COLUMN a INT */
            if (parse_type(lx, &out->cols[0], err) < 0) return -1;
            out->ncols = 1;
            rc = 0;
        } else {
            FAIL("ALTER TABLE supports ADD, DROP, RENAME COLUMN, and ALTER COLUMN (line %d)",
                 lx->cur.line);
        }
    }
    else if (tok_is_kw(t, "TRUNCATE")) {
        if (adv(lx, err) < 0) return -1;
        if (opt_kw(lx, "TABLE", err) < 0) return -1;
        out->kind = ST_TRUNCATE;
        rc = take_ident(lx, out->table, MAX_NAME, err);
    }
    else if (tok_is_kw(t, "INSERT"))   { if (adv(lx, err) < 0) return -1; rc = parse_insert(lx, out, err); }
    else if (tok_is_kw(t, "SELECT"))   { if (adv(lx, err) < 0) return -1; rc = parse_select(lx, out, err); }
    else if (tok_is_kw(t, "UPDATE"))   { if (adv(lx, err) < 0) return -1; rc = parse_update(lx, out, err); }
    else if (tok_is_kw(t, "DELETE"))   { if (adv(lx, err) < 0) return -1; rc = parse_delete(lx, out, err); }
    else if (tok_is_kw(t, "RECALL"))   { if (adv(lx, err) < 0) return -1; rc = parse_recall(lx, out, err); }
    else if (tok_is_kw(t, "REMEMBER")) { if (adv(lx, err) < 0) return -1; rc = parse_remember(lx, out, ST_REMEMBER, err); }
    else if (tok_is_kw(t, "FORGET"))   { if (adv(lx, err) < 0) return -1; rc = parse_remember(lx, out, ST_FORGET, err); }
    else if (tok_is_kw(t, "SHOW"))     { if (adv(lx, err) < 0) return -1; rc = parse_show(lx, out, err); }
    else if (tok_is_kw(t, "CHECKPOINT")) { out->kind = ST_CHECKPOINT; rc = adv(lx, err); }
    else if (tok_is_kw(t, "PRINT")) {
        if (adv(lx, err) < 0) return -1;
        out->kind = ST_PRINT;
        out->sets[0].val = parse_or(lx, err);
        if (!out->sets[0].val) return -1;
        out->nsets = 1;
        rc = 0;
    }
    else if (tok_is_kw(t, "BEGIN") && tok_is_kw(lex_peek(lx), "TRY")) {
        if (adv(lx, err) < 0) return -1;
        if (adv(lx, err) < 0) return -1;
        out->kind = ST_TRY;
        if (parse_block_list(lx, &out->stmts, &out->nstmts, err) < 0) return -1;
        if (eat_kw(lx, "TRY", err) < 0) return -1;
        if (!(tok_is_kw(&lx->cur, "BEGIN") && tok_is_kw(lex_peek(lx), "CATCH")))
            FAIL("expected BEGIN CATCH after END TRY");
        if (adv(lx, err) < 0) return -1;
        if (adv(lx, err) < 0) return -1;
        if (parse_block_list(lx, &out->else_stmts, &out->n_else, err) < 0) return -1;
        if (eat_kw(lx, "CATCH", err) < 0) return -1;
        rc = 0;
    }
    else if (tok_is_kw(t, "BEGIN")) {
        out->kind = ST_BEGIN;
        if (adv(lx, err) < 0) return -1;
        /* optional "TRANSACTION" keyword */
        if (tok_is_kw(&lx->cur, "TRANSACTION")) {
            if (adv(lx, err) < 0) return -1;
        }
        rc = 0;
    }
    else if (tok_is_kw(t, "COMMIT")) {
        out->kind = ST_COMMIT;
        if (adv(lx, err) < 0) return -1;
        if (tok_is_kw(&lx->cur, "TRANSACTION")) {
            if (adv(lx, err) < 0) return -1;
        }
        rc = 0;
    }
    else if (tok_is_kw(t, "ROLLBACK")) {
        out->kind = ST_ROLLBACK;
        if (adv(lx, err) < 0) return -1;
        if (tok_is_kw(&lx->cur, "TRANSACTION")) {
            if (adv(lx, err) < 0) return -1;
        }
        rc = 0;
    }
    else if (tok_is_kw(t, "EXPLAIN")) {
        if (adv(lx, err) < 0) return -1;
        out->kind = ST_EXPLAIN;
        /* parse the inner statement */
        out->sub = calloc(1, sizeof(Stmt));
        if (!out->sub) { snprintf(err, MAX_ERR, "out of memory"); return -1; }
        rc = parse_stmt(lx, out->sub, err);
        if (rc <= 0) { stmt_free(out); return rc; }
        /* inner stmt already consumed its terminator; skip boundary check below */
        rc = 1;
    }
    else if (tok_is_kw(t, "VACUUM")) {
        out->kind = ST_VACUUM;
        rc = adv(lx, err);
    }
    else {
        snprintf(err, MAX_ERR, "unrecognised statement starting at '%s' (line %d)",
                 t->text, t->line);
        return -1;
    }

    if (rc < 0) { stmt_free(out); return -1; }

    /* EXPLAIN skips the boundary check — its inner statement already consumed
     * the terminator. */
    if (out->kind == ST_EXPLAIN)
        return 1;

    return 1;
}

/* parse one statement without consuming its trailing ';'. The separator
 * skip and the trailing-garbage boundary check are identical to parse_stmt;
 * this exists so a CREATE PROCEDURE body probe leaves the batch's statement
 * boundary intact for the outer statement. */
static int parse_stmt_core(Lexer *lx, Stmt *out, char *err)
{
    int rc = parse_stmt_nobound(lx, out, err);
    if (rc <= 0) return rc;

    /* EXPLAIN's inner statement already consumed the terminator */
    if (out->kind == ST_EXPLAIN)
        return 1;

    /* A statement has to end at a statement boundary: end of input, ';' or GO.
     *
     * This check matters more than it looks. Statements are executed as they
     * are parsed, so without it any unsupported trailing clause would leave a
     * complete-looking statement behind and that truncated statement would
     * already have run by the time the error surfaced. "DELETE FROM t LIMIT 1"
     * deleted every row in t and then complained about LIMIT. Refusing the
     * whole statement up front is the only safe behaviour. */
    if (!(lx->cur.kind == TK_EOF ||
          tok_is_punct(&lx->cur, ";") ||
          tok_is_kw(&lx->cur, "GO"))) {
        snprintf(err, MAX_ERR,
                 "unexpected '%s' on line %d, after what otherwise looked like "
                 "a complete statement - this dialect may not support that "
                 "clause. Nothing was run.",
                 lx->cur.text, lx->cur.line);
        stmt_free(out);
        return -1;
    }

    return 1;
}

int parse_stmt(Lexer *lx, Stmt *out, char *err)
{
    int rc = parse_stmt_core(lx, out, err);
    if (rc <= 0) return rc;

    /* EXPLAIN's inner statement already consumed the terminator */
    if (out->kind == ST_EXPLAIN)
        return 1;

    /* an optional trailing semicolon */
    if (tok_is_punct(&lx->cur, ";")) {
        if (adv(lx, err) < 0) { stmt_free(out); return -1; }
    }
    return 1;
}
