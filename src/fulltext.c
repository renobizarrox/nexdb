/* fulltext.c - PostgreSQL-style full-text search: the English tokenizer with
 * stop-word removal, tsvector / tsquery building, the @@ match operator, and
 * ts_rank. Everything here is a pure string transformation with no database
 * access, so func.c can call it from expression evaluation and exec.c can
 * feed it into the GIN index.
 *
 * Canonical formats (matching PostgreSQL's text forms):
 *   tsvector: "cat:1,3 dog:2"        lexeme, colon, comma-separated positions
 *   tsquery : "cat & dog | !mouse"   lexemes with & | ! and parentheses
 */
#define _GNU_SOURCE
#include "nexdb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

#define FT_LEX 64          /* max lexeme length (bounded at 63 chars) */

/* The English stop-word list, matching PostgreSQL's english_stop. These
 * tokens are never indexed and never match. */
static const char *const stop_words[] = {
    "a","about","above","after","again","against","all","am","an","and","any",
    "are","aren't","as","at","be","because","been","before","being","below",
    "between","both","but","by","can","can't","cannot","could","couldn't","did",
    "didn't","do","does","doesn't","doing","don't","down","during","each","few",
    "for","from","further","had","hadn't","has","hasn't","have","haven't",
    "having","he","he'd","he'll","he's","her","here","here's","hers","herself",
    "him","himself","his","how","how's","i","i'd","i'll","i'm","i've","if","in",
    "into","is","isn't","it","it's","its","itself","let's","me","more","most",
    "mustn't","my","myself","no","nor","not","of","off","on","once","only","or",
    "other","ought","our","ours","ourselves","out","over","own","same","shan't",
    "she","she'd","she'll","she's","should","shouldn't","so","some","such",
    "than","that","that's","the","their","theirs","them","themselves","then",
    "there","there's","these","they","they'd","they'll","they're","they've",
    "this","those","through","to","too","under","until","up","very","was",
    "wasn't","we","we'd","we'll","we're","we've","were","weren't","what",
    "what's","when","when's","where","where's","which","while","who","who's",
    "whom","why","why's","with","won't","would","wouldn't","you","you'd",
    "you'll","you're","you've","your","yours","yourself","yourselves",
    NULL
};

static int is_stop(const char *word)
{
    for (int i = 0; stop_words[i]; i++)
        if (strcmp(stop_words[i], word) == 0) return 1;
    return 0;
}

/* ----------------------------------------------------------------- lexemes */

void fulltext_iter_begin(FTIter *it, const char *text)
{
    it->p = text ? text : "";
}

/* Next lexeme: lowercase alphanumerics and underscores, stop words skipped.
 * Returns 1 = lexeme written, 0 = end of text. */
int fulltext_iter_next(FTIter *it, char *lex, size_t cap)
{
    const char *p = it->p;
    for (;;) {
        while (*p && !(isalnum((unsigned char)*p) || *p == '_')) p++;
        if (!*p) { it->p = p; return 0; }
        size_t k = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (k + 1 < cap) lex[k] = (char)tolower((unsigned char)*p);
            k++;
            p++;
        }
        if (k >= cap) k = cap - 1;
        lex[k] = 0;
        it->p = p;
        if (!is_stop(lex)) return 1;
    }
}

/* Distinct lexemes of a document, in first-seen order. */
int fulltext_terms(const char *text, char terms[][64], int max)
{
    FTIter it;
    fulltext_iter_begin(&it, text);
    char lex[FT_LEX];
    int n = 0;
    while (n < max && fulltext_iter_next(&it, lex, sizeof lex)) {
        int seen = 0;
        for (int i = 0; i < n && !seen; i++)
            if (strcmp(terms[i], lex) == 0) seen = 1;
        if (seen) continue;
        snprintf(terms[n], 64, "%s", lex);
        n++;
    }
    return n;
}

/* --------------------------------------------------------- growable output */

typedef struct {
    char  *s;
    size_t len, cap;
} GBuf;

static int gbuf_reserve(GBuf *g, size_t add, char *err)
{
    if (g->len + add + 1 <= g->cap) return 0;
    size_t cap = g->cap ? g->cap * 2 : 256;
    while (cap < g->len + add + 1) cap *= 2;
    char *ns = realloc(g->s, cap);
    if (!ns) {
        snprintf(err, MAX_ERR, "out of memory");
        return -1;
    }
    g->s = ns;
    g->cap = cap;
    return 0;
}

static int gbuf_add(GBuf *g, const char *s, size_t len, char *err)
{
    if (gbuf_reserve(g, len, err) < 0) return -1;
    memcpy(g->s + g->len, s, len);
    g->len += len;
    g->s[g->len] = 0;
    return 0;
}

/* --------------------------------------------------------------- tsvector */

/* to_tsvector: 'word:pos,pos word:pos'. Lexemes are lowercased, stop words
 * removed, positions renumbered consecutively from 1, repeated lexemes
 * collapsed into one entry with comma-separated positions. */
typedef struct {
    char lex[FT_LEX];
    size_t tail;               /* offset just past the entry's last digit */
} TsvEntry;

int fulltext_to_tsvector(const char *text, char **out, char *err)
{
    GBuf g;
    memset(&g, 0, sizeof g);
    TsvEntry *entries = NULL;
    size_t nentries = 0, cap = 0;
    FTIter it;
    fulltext_iter_begin(&it, text);
    char lex[FT_LEX];
    int pos = 0, any = 0;
    while (fulltext_iter_next(&it, lex, sizeof lex)) {
        pos++;
        size_t ei = nentries;
        for (size_t i = 0; i < nentries; i++)
            if (strcmp(entries[i].lex, lex) == 0) { ei = i; break; }
        char num[24];
        if (ei < nentries) {
            /* merge: insert ",pos" at the entry's tail */
            snprintf(num, sizeof num, ",%d", pos);
            if (gbuf_reserve(&g, strlen(num), err) < 0) goto fail;
            memmove(g.s + entries[ei].tail + strlen(num), g.s + entries[ei].tail,
                    g.len - entries[ei].tail + 1);
            memcpy(g.s + entries[ei].tail, num, strlen(num));
            g.len += strlen(num);
            entries[ei].tail += strlen(num);
            continue;
        }
        if (nentries == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            TsvEntry *ne = realloc(entries, ncap * sizeof *ne);
            if (!ne) {
                free(entries);
                snprintf(err, MAX_ERR, "out of memory");
                goto fail;
            }
            entries = ne;
            cap = ncap;
        }
        if (any && gbuf_add(&g, " ", 1, err) < 0) goto fail;
        if (gbuf_add(&g, lex, strlen(lex), err) < 0) goto fail;
        snprintf(num, sizeof num, ":%d", pos);
        if (gbuf_add(&g, num, strlen(num), err) < 0) goto fail;
        snprintf(entries[nentries].lex, FT_LEX, "%s", lex);
        entries[nentries].tail = g.len;
        nentries++;
        any = 1;
    }
    free(entries);
    if (!g.s) {
        g.s = malloc(1);
        if (!g.s) { snprintf(err, MAX_ERR, "out of memory"); return -1; }
        g.s[0] = 0;
    }
    *out = g.s;
    return 0;
fail:
    free(entries);
    free(g.s);
    return -1;
}

/* ----------------------------------------------------------------- tsquery */

typedef struct QNode {
    char op;               /* 'L' = lexeme, '&', '|', '!' */
    char lex[FT_LEX];
    struct QNode *l, *r;
} QNode;

static QNode *qnode(char op, const char *lex)
{
    QNode *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->op = op;
    if (lex) snprintf(n->lex, sizeof n->lex, "%s", lex);
    return n;
}

static void qnode_free(QNode *n)
{
    if (!n) return;
    qnode_free(n->l);
    qnode_free(n->r);
    free(n);
}

/* Skip whitespace; report the next non-space character (0 at end). */
static char qpeek(const char **pp)
{
    const char *p = *pp;
    while (*p && isspace((unsigned char)*p)) p++;
    *pp = p;
    return *p;
}

static QNode *parse_qor(const char **pp, char *err);

static QNode *parse_qterm(const char **pp, char *err)
{
    const char *p = *pp;
    char c = qpeek(&p);
    if (c == 0) {
        snprintf(err, MAX_ERR, "unexpected end of tsquery");
        return NULL;
    }
    if (c == '!') {
        p++;
        QNode *inner = parse_qterm(&p, err);
        if (!inner) return NULL;
        QNode *n = qnode('!', NULL);
        if (!n) { qnode_free(inner); snprintf(err, MAX_ERR, "out of memory"); return NULL; }
        n->l = inner;
        *pp = p;
        return n;
    }
    if (c == '(') {
        p++;
        QNode *n = parse_qor(&p, err);
        if (!n) return NULL;
        if (qpeek(&p) != ')') {
            qnode_free(n);
            snprintf(err, MAX_ERR, "expected ')' in tsquery");
            return NULL;
        }
        p++;
        *pp = p;
        return n;
    }
    if (c == '&' || c == '|' || c == ')') {
        snprintf(err, MAX_ERR, "unexpected '%c' in tsquery", c);
        return NULL;
    }
    /* a lexeme: any run of alphanumerics and underscores */
    char lex[FT_LEX];
    size_t k = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (k + 1 < sizeof lex) lex[k] = (char)tolower((unsigned char)*p);
        k++;
        p++;
    }
    if (k >= sizeof lex) k = sizeof lex - 1;
    lex[k] = 0;
    QNode *n = qnode('L', lex);
    if (!n) { snprintf(err, MAX_ERR, "out of memory"); return NULL; }
    *pp = p;
    return n;
}

static QNode *parse_qand(const char **pp, char *err)
{
    QNode *l = parse_qterm(pp, err);
    if (!l) return NULL;
    for (;;) {
        const char *p = *pp;
        char c = qpeek(&p);
        if (c != '&') break;
        p++;
        QNode *r = parse_qterm(&p, err);
        if (!r) { qnode_free(l); return NULL; }
        QNode *n = qnode('&', NULL);
        if (!n) {
            qnode_free(l); qnode_free(r);
            snprintf(err, MAX_ERR, "out of memory");
            return NULL;
        }
        n->l = l;
        n->r = r;
        l = n;
        *pp = p;
    }
    return l;
}

static QNode *parse_qor(const char **pp, char *err)
{
    QNode *l = parse_qand(pp, err);
    if (!l) return NULL;
    for (;;) {
        const char *p = *pp;
        char c = qpeek(&p);
        if (c != '|') break;
        p++;
        QNode *r = parse_qand(&p, err);
        if (!r) { qnode_free(l); return NULL; }
        QNode *n = qnode('|', NULL);
        if (!n) {
            qnode_free(l); qnode_free(r);
            snprintf(err, MAX_ERR, "out of memory");
            return NULL;
        }
        n->l = l;
        n->r = r;
        l = n;
        *pp = p;
    }
    return l;
}

/* Fold stop-word lexemes out of the tree. A bare or negated stop word
 * vanishes; a binary node whose children both vanish becomes empty, and a
 * binary node with one surviving child collapses to that child. Returns the
 * node to keep (NULL = empty; the returned tree is a subset of the input). */
static QNode *fold_stops(QNode *n)
{
    if (!n) return NULL;
    if (n->op == 'L') {
        if (is_stop(n->lex)) {
            qnode_free(n);
            return NULL;
        }
        return n;
    }
    if (n->op == '!') {
        QNode *inner = fold_stops(n->l);
        n->l = NULL;
        if (!inner) {
            qnode_free(n);
            return NULL;
        }
        n->l = inner;
        return n;
    }
    QNode *l = fold_stops(n->l);
    n->l = NULL;
    QNode *r = fold_stops(n->r);
    n->r = NULL;
    if (!l && !r) {
        qnode_free(n);
        return NULL;
    }
    if (!l) {
        qnode_free(n);
        return r;
    }
    if (!r) {
        qnode_free(n);
        return l;
    }
    n->l = l;
    n->r = r;
    return n;
}

static int qnode_serialize(GBuf *g, const QNode *n, char *err)
{
    if (n->op == 'L')
        return gbuf_add(g, n->lex, strlen(n->lex), err);
    if (n->op == '!') {
        if (gbuf_add(g, "!", 1, err) < 0) return -1;
        return qnode_serialize(g, n->l, err);
    }
    if (gbuf_add(g, "(", 1, err) < 0) return -1;
    if (qnode_serialize(g, n->l, err) < 0) return -1;
    if (gbuf_add(g, n->op == '&' ? " & " : " | ", 3, err) < 0) return -1;
    if (qnode_serialize(g, n->r, err) < 0) return -1;
    return gbuf_add(g, ")", 1, err);
}

/* to_tsquery: parse "cat & dog | !mouse" and fold stop words out. An
 * all-stop-word query serializes to "" and matches nothing. */
int fulltext_to_tsquery(const char *text, char **out, char *err)
{
    const char *p = text ? text : "";
    QNode *root = parse_qor(&p, err);
    if (!root) return -1;
    if (qpeek(&p) != 0) {
        snprintf(err, MAX_ERR, "trailing garbage in tsquery at '%s'", p);
        qnode_free(root);
        return -1;
    }
    root = fold_stops(root);
    if (!root) {
        *out = malloc(1);
        if (!*out) { snprintf(err, MAX_ERR, "out of memory"); return -1; }
        (*out)[0] = 0;
        return 0;
    }
    GBuf g;
    memset(&g, 0, sizeof g);
    if (qnode_serialize(&g, root, err) < 0) {
        qnode_free(root);
        free(g.s);
        return -1;
    }
    qnode_free(root);
    if (!g.s) {
        g.s = malloc(1);
        if (!g.s) { snprintf(err, MAX_ERR, "out of memory"); return -1; }
        g.s[0] = 0;
    }
    *out = g.s;
    return 0;
}

/* plainto_tsquery: every lexeme ANDed together ("cat dog" -> "cat & dog"). */
int fulltext_plainto_tsquery(const char *text, char **out, char *err)
{
    GBuf g;
    memset(&g, 0, sizeof g);
    FTIter it;
    fulltext_iter_begin(&it, text);
    char lex[FT_LEX];
    int any = 0;
    while (fulltext_iter_next(&it, lex, sizeof lex)) {
        if (any && gbuf_add(&g, " & ", 3, err) < 0) goto fail;
        if (gbuf_add(&g, lex, strlen(lex), err) < 0) goto fail;
        any = 1;
    }
    if (!g.s) {
        g.s = malloc(1);
        if (!g.s) { snprintf(err, MAX_ERR, "out of memory"); return -1; }
        g.s[0] = 0;
    }
    *out = g.s;
    return 0;
fail:
    free(g.s);
    return -1;
}

/* -------------------------------------------------------- parsing a tsvector
 *
 * A tsvector text is "lexeme:pos,pos lexeme:pos". We parse it into a small
 * table of distinct lexemes and their positions; entries past the table's
 * capacity are ignored (they cannot change a match, only the rank). */

#define FT_MAX_WORDS 256
#define FT_MAX_POS   64

typedef struct {
    char   words[FT_MAX_WORDS][FT_LEX];
    int    npos[FT_MAX_WORDS];
    int    pos[FT_MAX_WORDS][FT_MAX_POS];
    int    nwords;
} TsvTable;

/* Parse "lexeme:pos,pos" pairs separated by spaces. Returns 0 on success,
 * -1 on a malformed tsvector. */
static int tsv_parse(const char *text, TsvTable *t)
{
    memset(t, 0, sizeof *t);
    const char *p = text;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char lex[FT_LEX];
        size_t k = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (k + 1 < sizeof lex) lex[k] = (char)tolower((unsigned char)*p);
            k++;
            p++;
        }
        if (k >= sizeof lex) k = sizeof lex - 1;
        lex[k] = 0;
        if (*p != ':') return -1;
        p++;
        int nw = -1;
        for (int i = 0; i < t->nwords && nw < 0; i++)
            if (strcmp(t->words[i], lex) == 0) nw = i;
        if (nw < 0) {
            if (t->nwords >= FT_MAX_WORDS) {
                /* table full: skip the rest of this entry */
                while (*p && isdigit((unsigned char)*p)) p++;
                while (*p == ',') {
                    p++;
                    while (*p && isdigit((unsigned char)*p)) p++;
                }
                continue;
            }
            nw = t->nwords++;
            snprintf(t->words[nw], FT_LEX, "%s", lex);
        }
        int ipos = 0;
        for (;;) {
            while (*p && isdigit((unsigned char)*p)) {
                ipos = ipos * 10 + (*p - '0');
                p++;
            }
            if (ipos > 0 && t->npos[nw] < FT_MAX_POS)
                t->pos[nw][t->npos[nw]++] = ipos;
            if (*p != ',') break;
            p++;
            ipos = 0;
        }
    }
    return 0;
}

static int tsv_has(const TsvTable *t, const char *lex)
{
    for (int i = 0; i < t->nwords; i++)
        if (strcmp(t->words[i], lex) == 0) return 1;
    return 0;
}

/* Evaluate a parsed tsquery tree against the table. */
static int qnode_eval(const QNode *n, const TsvTable *t)
{
    if (n->op == 'L') return tsv_has(t, n->lex);
    if (n->op == '!') return !qnode_eval(n->l, t);
    if (n->op == '&') return qnode_eval(n->l, t) && qnode_eval(n->r, t);
    return qnode_eval(n->l, t) || qnode_eval(n->r, t);
}

/* Parse a serialized tsquery (as produced by to_tsquery) for matching. */
static int qparse_and_match(const char *tsq, const TsvTable *t, char *err)
{
    if (!tsq[0]) return 0;                 /* empty query matches nothing */
    const char *p = tsq;
    QNode *root = parse_qor(&p, err);
    if (!root) return -1;
    int m = qnode_eval(root, t);
    qnode_free(root);
    return m;
}

/* tsvector @@ tsquery: 1 = match, 0 = no match, -1 = malformed input. */
int fulltext_match(const char *tsv, const char *tsq, char *err)
{
    TsvTable t;
    if (tsv_parse(tsv, &t) < 0) {
        snprintf(err, MAX_ERR, "malformed tsvector '%s'", tsv ? tsv : "");
        return -1;
    }
    return qparse_and_match(tsq, &t, err);
}

/* 1 if the lexeme appears as a positive (non-negated) term of the query. */
static int qnode_includes(const QNode *n, const char *lex)
{
    if (!n) return 0;
    if (n->op == 'L') return strcmp(n->lex, lex) == 0;
    if (n->op == '!') return 0;
    return qnode_includes(n->l, lex) || qnode_includes(n->r, lex);
}

static void qnode_collect(const QNode *n, char terms[][64], int *count, int max)
{
    if (!n || *count >= max) return;
    if (n->op == 'L') {
        if (!is_stop(n->lex)) snprintf(terms[(*count)++], 64, "%s", n->lex);
        return;
    }
    if (n->op == '!') return;   /* negated terms never narrow candidates */
    qnode_collect(n->l, terms, count, max);
    qnode_collect(n->r, terms, count, max);
}

/* The positive (non-negated) lexemes of a tsquery, stop words removed, in
 * query order. The planner uses the first one to choose a GIN posting list;
 * a query with no positive terms (pure negation, or all stop words) cannot
 * be narrowed by the index. Returns the term count, or -1 on a parse error. */
int fulltext_query_terms(const char *tsq, char terms[][64], int max, char *err)
{
    if (!tsq || !tsq[0]) return 0;
    const char *p = tsq;
    QNode *root = parse_qor(&p, err);
    if (!root) return -1;
    int n = 0;
    qnode_collect(root, terms, &n, max);
    qnode_free(root);
    return n;
}

/* ts_rank: position-weighted density of the matched lexemes: the sum over
 * every position of a positive lexeme of 1/(1+pos). A document whose matches
 * sit near the start ranks higher than one with the same matches buried at
 * the end; more matches rank higher than fewer. */
double fulltext_rank(const char *tsv, const char *tsq)
{
    char err[MAX_ERR];
    TsvTable t;
    if (tsv_parse(tsv, &t) < 0) return 0.0;
    if (!tsq[0]) return 0.0;
    const char *p = tsq;
    QNode *root = parse_qor(&p, err);
    if (!root) return 0.0;
    double rank = 0.0;
    for (int i = 0; i < t.nwords; i++) {
        if (!qnode_includes(root, t.words[i])) continue;
        for (int j = 0; j < t.npos[i]; j++)
            rank += 1.0 / (1.0 + t.pos[i][j]);
    }
    qnode_free(root);
    return rank;
}
