/* lexer.c - tokenizer for the T-SQL-flavoured dialect.
 *
 * Handles the syntax a SQL Server user expects to be able to type:
 *   -- line comments and  / * block comments * /
 *   'single quoted strings' with '' escapes,  N'unicode literals'
 *   [bracketed identifiers] and "quoted identifiers"
 *   @variables (tokenized as identifiers)
 *   multi-character operators <> != <= >=
 */
#define _GNU_SOURCE
#include "nexdb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>

/* Reset lexer state and point it at a NUL-terminated SQL source string. */
void lex_init(Lexer *lx, const char *src)
{
    memset(lx, 0, sizeof *lx);
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    lx->has_ahead = 0;
    lx->cur.kind = TK_EOF;
}

/* Advance past whitespace, line comments (--), and block comments (slash-star). */
static void skip_space(Lexer *lx)
{
    const char *s = lx->src;
    for (;;) {
        char c = s[lx->pos];
        if (c == 0) return;
        if (c == '\n') { lx->line++; lx->pos++; continue; }
        if (isspace((unsigned char)c)) { lx->pos++; continue; }
        if (c == '-' && s[lx->pos + 1] == '-') {
            while (s[lx->pos] && s[lx->pos] != '\n') lx->pos++;
            continue;
        }
        if (c == '/' && s[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (s[lx->pos] && !(s[lx->pos] == '*' && s[lx->pos + 1] == '/')) {
                if (s[lx->pos] == '\n') lx->line++;
                lx->pos++;
            }
            if (s[lx->pos]) lx->pos += 2;
            continue;
        }
        return;
    }
}

/* Read the next token from the source into *t. Returns 0 on success, -1 on error. */
static int scan_token(Lexer *lx, Token *t)
{
    const char *s = lx->src;
    skip_space(lx);
    memset(t, 0, sizeof *t);
    t->line = lx->line;

    char c = s[lx->pos];
    if (c == 0) { t->kind = TK_EOF; return 0; }

    /* string literal, optionally N-prefixed */
    if (c == '\'' || ((c == 'N' || c == 'n') && s[lx->pos + 1] == '\'')) {
        if (c != '\'') lx->pos++;              /* skip the N */
        lx->pos++;                             /* opening quote */
        size_t n = 0;
        while (s[lx->pos]) {
            if (s[lx->pos] == '\'') {
                if (s[lx->pos + 1] == '\'') {  /* '' -> literal quote */
                    if (n >= sizeof t->text - 1) goto too_long;
                    t->text[n++] = '\'';
                    lx->pos += 2;
                    continue;
                }
                lx->pos++;
                t->kind = TK_STRING;
                t->text[n] = 0;
                return 0;
            }
            if (s[lx->pos] == '\n') lx->line++;
            if (n >= sizeof t->text - 1) goto too_long;
            t->text[n++] = s[lx->pos];
            lx->pos++;
        }
        snprintf(lx->err, MAX_ERR, "unterminated string literal on line %d", t->line);
        return -1;

    too_long:
        snprintf(lx->err, MAX_ERR,
                 "string literal on line %d is longer than %d characters",
                 t->line, (int)sizeof t->text - 1);
        return -1;
    }

    /* bracketed or double-quoted identifier */
    if (c == '[' || c == '"') {
        char close = (c == '[') ? ']' : '"';
        lx->pos++;
        size_t n = 0;
        while (s[lx->pos] && s[lx->pos] != close) {
            if (n < sizeof t->text - 1) t->text[n++] = s[lx->pos];
            lx->pos++;
        }
        if (s[lx->pos] == close) lx->pos++;
        t->text[n] = 0;
        t->kind = TK_IDENT;
        return 0;
    }

    /* number */
    if (isdigit((unsigned char)c) ||
        (c == '.' && isdigit((unsigned char)s[lx->pos + 1]))) {
        size_t start = lx->pos;
        int is_float = 0;
        while (isdigit((unsigned char)s[lx->pos])) lx->pos++;
        if (s[lx->pos] == '.') { is_float = 1; lx->pos++;
            while (isdigit((unsigned char)s[lx->pos])) lx->pos++; }
        if (s[lx->pos] == 'e' || s[lx->pos] == 'E') {
            is_float = 1; lx->pos++;
            if (s[lx->pos] == '+' || s[lx->pos] == '-') lx->pos++;
            while (isdigit((unsigned char)s[lx->pos])) lx->pos++;
        }
        size_t len = lx->pos - start;
        if (len >= sizeof t->text) len = sizeof t->text - 1;
        memcpy(t->text, s + start, len);
        t->text[len] = 0;
        t->kind = TK_NUMBER;
        t->is_float = is_float;
        if (is_float) {
            t->fval = strtod(t->text, NULL);
        } else {
            errno = 0;
            t->ival = strtoll(t->text, NULL, 10);
            if (errno == ERANGE) {
                /* strtoll saturates at INT64_MAX, so 99999999999999999999 used
                 * to be stored as 9223372036854775807 without a word */
                snprintf(lx->err, MAX_ERR,
                         "the number %s on line %d is too large; the biggest "
                         "whole number is 9223372036854775807",
                         t->text, t->line);
                return -1;
            }
        }
        return 0;
    }

    /* identifier / keyword / @variable / _pseudo_column */
    if (isalpha((unsigned char)c) || c == '_' || c == '@' || c == '#') {
        size_t n = 0;
        while (isalnum((unsigned char)s[lx->pos]) || s[lx->pos] == '_' ||
               s[lx->pos] == '@' || s[lx->pos] == '#' || s[lx->pos] == '$') {
            if (n < sizeof t->text - 1) t->text[n++] = s[lx->pos];
            lx->pos++;
        }
        t->text[n] = 0;
        t->kind = TK_IDENT;
        return 0;
    }

    /* operators and punctuation */
    static const char *two[] = { "<>", "!=", "<=", ">=", "||", NULL };
    for (int i = 0; two[i]; i++) {
        if (s[lx->pos] == two[i][0] && s[lx->pos + 1] == two[i][1]) {
            t->kind = TK_PUNCT;
            t->text[0] = two[i][0];
            t->text[1] = two[i][1];
            t->text[2] = 0;
            lx->pos += 2;
            return 0;
        }
    }
    if (strchr("(),;*=<>+-/.%", c)) {
        t->kind = TK_PUNCT;
        t->text[0] = c;
        t->text[1] = 0;
        lx->pos++;
        return 0;
    }

    snprintf(lx->err, MAX_ERR, "unexpected character '%c' on line %d", c, t->line);
    return -1;
}

/* Advance the lexer: consume the current token and load the next one into lx->cur. */
int lex_next(Lexer *lx)
{
    if (lx->has_ahead) {
        lx->cur = lx->ahead;
        lx->has_ahead = 0;
        return 0;
    }
    return scan_token(lx, &lx->cur);
}

/* Look one token ahead without consuming it (used for disambiguation in the parser). */
Token *lex_peek(Lexer *lx)
{
    if (!lx->has_ahead) {
        if (scan_token(lx, &lx->ahead) < 0) {
            lx->ahead.kind = TK_EOF;
            lx->ahead.text[0] = 0;
        }
        lx->has_ahead = 1;
    }
    return &lx->ahead;
}

/* True if t is an identifier token whose text matches kw, case-insensitively. */
int tok_is_kw(const Token *t, const char *kw)
{
    return t->kind == TK_IDENT && strcasecmp(t->text, kw) == 0;
}

/* True if t is a punctuation token whose text exactly equals p. */
int tok_is_punct(const Token *t, const char *p)
{
    return t->kind == TK_PUNCT && strcmp(t->text, p) == 0;
}
