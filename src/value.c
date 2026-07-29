/* value.c - dynamically typed values and their comparison rules. */
#define _GNU_SOURCE
#include "nexdb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

/* Release heap storage owned by a value and reset it to NULL. */
void val_clear(Value *v)
{
    if (v->tag == T_TEXT && v->s) free(v->s);
    v->s = NULL;
    v->slen = 0;
    v->tag = T_NULL;
    v->i = 0;
    v->f = 0;
}

/* Construct a typed NULL value. */
Value val_null(void)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = T_NULL;
    return v;
}

/* Construct a 64-bit integer value. */
Value val_int(int64_t i)
{
    Value v = val_null();
    v.tag = T_INT;
    v.i = i;
    return v;
}

/* Construct a floating-point value. */
Value val_float(double f)
{
    Value v = val_null();
    v.tag = T_FLOAT;
    v.f = f;
    return v;
}

/* Construct a BIT (0/1) value. */
Value val_bit(int b)
{
    Value v = val_null();
    v.tag = T_BIT;
    v.i = b ? 1 : 0;
    return v;
}

/* Copy up to n bytes of text into a newly allocated Value. */
Value val_text_n(const char *s, size_t n)
{
    Value v = val_null();
    v.tag = T_TEXT;
    v.s = malloc(n + 1);
    if (!v.s) { v.tag = T_NULL; return v; }
    if (n) memcpy(v.s, s, n);
    v.s[n] = 0;
    v.slen = (uint32_t)n;
    return v;
}

/* NUL-terminated convenience wrapper around val_text_n. */
Value val_text(const char *s)
{
    return val_text_n(s, s ? strlen(s) : 0);
}

/* Deep-copy a value; text columns get their own buffer. */
Value val_copy(const Value *v)
{
    if (v->tag == T_TEXT) return val_text_n(v->s ? v->s : "", v->slen);
    return *v;
}

/* SQL-style truth test: NULL and empty strings are false. */
int val_truthy(const Value *v)
{
    switch (v->tag) {
    case T_NULL:  return 0;
    case T_INT:
    case T_BIT:   return v->i != 0;
    case T_FLOAT: return v->f != 0.0;
    case T_TEXT:  return v->slen != 0;
    case T_UUID:  return 1;
    }
    return 0;
}

/* Coerce INT, BIT, or FLOAT to a double for numeric comparison. */
static double val_num(const Value *v)
{
    if (v->tag == T_FLOAT) return v->f;
    return (double)v->i;
}

/* Parse a complete number out of a string. Returns 1 on success. Trailing or
 * leading spaces are fine; trailing junk is not, because "12abc" being treated
 * as 12 is exactly the kind of guess that hides mistakes. */
int text_to_number(const char *s, double *out, int *is_int, int64_t *ival)
{
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return 0;

    char *end = NULL;
    errno = 0;
    long long i = strtoll(s, &end, 10);
    if (end && end != s && errno != ERANGE) {
        const char *p = end;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) {
            if (is_int) *is_int = 1;
            if (ival) *ival = (int64_t)i;
            if (out) *out = (double)i;
            return 1;
        }
    }

    errno = 0;
    end = NULL;
    double d = strtod(s, &end);
    if (!end || end == s || errno == ERANGE) return 0;
    const char *p = end;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p) return 0;
    if (is_int) *is_int = 0;
    if (out) *out = d;
    return 1;
}

/* Returns -1/0/1.
 *
 *   *ok =  1  comparison is meaningful
 *   *ok =  0  undefined because one side is NULL (SQL's unknown)
 *   *ok = -1  the two values are not comparable at all
 *
 * That last case used to be handled by rendering the number to a string and
 * comparing alphabetically, so on an INT column "WHERE n > '10'" quietly
 * matched 9 and rejected 100. Text that spells a number is now compared
 * numerically; text that does not is an error the caller must report. */
int val_compare(const Value *a, const Value *b, int *ok)
{
    *ok = 1;
    if (a->tag == T_NULL || b->tag == T_NULL) { *ok = 0; return 0; }

    /* UUIDs compare by their bytes, and against text by parsing it */
    if (a->tag == T_UUID || b->tag == T_UUID) {
        uint8_t tmp[16];
        const uint8_t *x, *y;
        if (a->tag == T_UUID) {
            x = a->uu;
        } else {
            if (a->tag != T_TEXT || !uuid_parse(a->s, tmp)) { *ok = -1; return 0; }
            x = tmp;
        }
        uint8_t tmp2[16];
        if (b->tag == T_UUID) {
            y = b->uu;
        } else {
            if (b->tag != T_TEXT || !uuid_parse(b->s, tmp2)) { *ok = -1; return 0; }
            y = tmp2;
        }
        int c = memcmp(x, y, 16);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }

    if (a->tag == T_TEXT && b->tag == T_TEXT) {
        int c = strcasecmp(a->s ? a->s : "", b->s ? b->s : "");
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }

    if (a->tag == T_TEXT || b->tag == T_TEXT) {
        const Value *text = (a->tag == T_TEXT) ? a : b;
        const Value *num  = (a->tag == T_TEXT) ? b : a;
        double parsed;
        if (!text_to_number(text->s, &parsed, NULL, NULL)) {
            *ok = -1;              /* e.g. n < 'banana' */
            return 0;
        }
        double other = val_num(num);
        double x = (a->tag == T_TEXT) ? parsed : other;
        double y = (a->tag == T_TEXT) ? other  : parsed;
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    double x = val_num(a), y = val_num(b);
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* Render a value as human-readable text for display and debugging. */
void val_format(const Value *v, char *out, size_t cap)
{
    switch (v->tag) {
    case T_NULL:  snprintf(out, cap, "NULL"); break;
    case T_INT:   snprintf(out, cap, "%lld", (long long)v->i); break;
    case T_BIT:   snprintf(out, cap, "%d", v->i ? 1 : 0); break;
    case T_FLOAT:
        if (v->f == floor(v->f) && fabs(v->f) < 1e15) {
            snprintf(out, cap, "%.1f", v->f);
        } else {
            /* Shortest representation that reads back as the same double.
             * Plain %g rounds to 6 significant digits, which meant the value
             * you were shown was not the value stored, and a WHERE clause
             * using the displayed number failed to match its own row. */
            for (int prec = 15; prec <= 17; prec++) {
                snprintf(out, cap, "%.*g", prec, v->f);
                if (strtod(out, NULL) == v->f) break;
            }
        }
        break;
    case T_TEXT:  snprintf(out, cap, "%s", v->s ? v->s : ""); break;
    case T_UUID:  uuid_format(v->uu, out, cap); break;
    default:      snprintf(out, cap, "?"); break;
    }
}

/* Return the inclusive min/max for a declared integer width. */
int int_range(uint8_t sub, int64_t *lo, int64_t *hi)
{
    switch (sub) {
    case SUB_TINYINT:  *lo = 0;           *hi = 255;         return 1;
    case SUB_SMALLINT: *lo = -32768;      *hi = 32767;       return 1;
    case SUB_INT:      *lo = -2147483648LL; *hi = 2147483647LL; return 1;
    default:           *lo = INT64_MIN;   *hi = INT64_MAX;   return 1;
    }
}

/* Map an IntSub enum to its SQL type name for error messages. */
const char *int_sub_name(uint8_t sub)
{
    switch (sub) {
    case SUB_TINYINT:  return "TINYINT";
    case SUB_SMALLINT: return "SMALLINT";
    case SUB_INT:      return "INT";
    default:           return "BIGINT";
    }
}

/* Accepts ISO 8601-ish date and date-time text: YYYY-MM-DD, optionally followed
 * by HH:MM or HH:MM:SS (space or 'T' separated). Real calendar validation,
 * including leap years, because '2026-99-99' used to be a perfectly good date. */
int valid_datetime(const char *s)
{
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;

    int y, mo, d;
    if (!(isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) &&
          isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) &&
          s[4] == '-' && isdigit((unsigned char)s[5]) &&
          isdigit((unsigned char)s[6]) && s[7] == '-' &&
          isdigit((unsigned char)s[8]) && isdigit((unsigned char)s[9])))
        return 0;

    y  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    mo = (s[5]-'0')*10 + (s[6]-'0');
    d  = (s[8]-'0')*10 + (s[9]-'0');
    if (mo < 1 || mo > 12 || d < 1) return 0;

    static const int dim[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int maxd = dim[mo - 1];
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) maxd = 29;
    if (d > maxd) return 0;

    const char *p = s + 10;
    if (!*p) return 1;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return 1;
    if (*p == 'T' || *p == 't') p++;

    /* HH:MM[:SS] */
    if (!(isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
          p[2] == ':' && isdigit((unsigned char)p[3]) &&
          isdigit((unsigned char)p[4])))
        return 0;
    int hh = (p[0]-'0')*10 + (p[1]-'0');
    int mi = (p[3]-'0')*10 + (p[4]-'0');
    if (hh > 23 || mi > 59) return 0;
    p += 5;
    if (*p == ':') {
        if (!(isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]))) return 0;
        int ss = (p[1]-'0')*10 + (p[2]-'0');
        if (ss > 59) return 0;
        p += 3;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    return *p == 0;
}

const char *type_name(uint8_t tag)
{
    switch (tag) {
    case T_INT:   return "INT";
    case T_FLOAT: return "FLOAT";
    case T_TEXT:  return "NVARCHAR";
    case T_BIT:   return "BIT";
    case T_UUID:  return "UNIQUEIDENTIFIER";
    default:      return "NULL";
    }
}

/* Free every column in a row without touching memory metadata. */
void row_clear(Row *r)
{
    for (int i = 0; i < r->ncols; i++) val_clear(&r->v[i]);
    r->ncols = 0;
}
