/* func.c - scalar functions, and the UUID type.
 *
 * Everything here is a pure value-to-value transformation, so it can be called
 * from expression evaluation without knowing anything about rows or storage.
 * Argument counts and types are checked and reported rather than guessed at.
 */
#define _GNU_SOURCE
#include "nexdb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------------ UUID */

Value val_uuid(const uint8_t bytes[16])
{
    Value v = val_null();
    v.tag = T_UUID;
    memcpy(v.uu, bytes, 16);
    return v;
}

/* Parse one hex digit; returns -1 if c is not hexadecimal. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Accepts the canonical 8-4-4-4-12 form, with or without surrounding braces.
 * Anything else is refused, which is the point: 'banana' was previously a
 * perfectly acceptable UUID. */
int uuid_parse(const char *s, uint8_t out[16])
{
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '{') s++;

    static const int groups[] = { 8, 4, 4, 4, 12 };
    int byte = 0;
    for (int g = 0; g < 5; g++) {
        for (int i = 0; i < groups[g]; i += 2) {
            int hi = hexval(s[0]), lo = hexval(s[1]);
            if (hi < 0 || lo < 0) return 0;
            out[byte++] = (uint8_t)((hi << 4) | lo);
            s += 2;
        }
        if (g < 4) {
            if (*s != '-') return 0;
            s++;
        }
    }
    if (*s == '}') s++;
    while (*s && isspace((unsigned char)*s)) s++;
    return *s == 0 && byte == 16;
}

void uuid_format(const uint8_t in[16], char *out, size_t cap)
{
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             in[0], in[1], in[2],  in[3],  in[4],  in[5],  in[6],  in[7],
             in[8], in[9], in[10], in[11], in[12], in[13], in[14], in[15]);
}

/* Version 4 UUID. Seeded from the clock and the address space; good enough to
 * avoid collisions in a single-user database, not a cryptographic source. */
void uuid_generate(uint8_t out[16])
{
    static int seeded = 0;
    if (!seeded) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned seed = (unsigned)(ts.tv_nsec ^ (ts.tv_sec << 8));
        seed ^= (unsigned)(uintptr_t)&seeded;
        srand(seed);
        seeded = 1;
    }
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(rand() & 0xff);
    out[6] = (uint8_t)((out[6] & 0x0f) | 0x40);   /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80);   /* variant   */
}

/* --------------------------------------------------------------- helpers */

/* Format the current time as YYYY-MM-DD HH:MM:SS for GETDATE and friends. */
static void now_text(char *out, size_t cap)
{
    time_t t = (time_t)mem_now();
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

static int as_number(const Value *v, double *out)
{
    switch (v->tag) {
    case T_INT:
    case T_BIT:   *out = (double)v->i; return 1;
    case T_FLOAT: *out = v->f;         return 1;
    case T_TEXT:  return text_to_number(v->s, out, NULL, NULL);
    default:      return 0;
    }
}

/* Render any value into text for the string functions. */
static void as_text(const Value *v, char *buf, size_t cap, const char **out)
{
    if (v->tag == T_TEXT && v->s) { *out = v->s; return; }
    val_format(v, buf, cap);
    *out = buf;
}

#define FN_ERR(...) do { snprintf(err, MAX_ERR, __VA_ARGS__); return -1; } while (0)

static int want_args(const char *name, int got, int lo, int hi, char *err)
{
    if (got < lo || got > hi) {
        if (lo == hi)
            FN_ERR("%s takes %d argument%s, got %d", name, lo,
                   lo == 1 ? "" : "s", got);
        FN_ERR("%s takes between %d and %d arguments, got %d", name, lo, hi, got);
    }
    return 0;
}

/* ------------------------------------------------------------ dispatcher */

/* Returns 1 if `name` is a known scalar function, so the parser can tell a
 * function call apart from a column reference followed by a stray paren. */
int func_exists(const char *name)
{
    static const char *known[] = {
        "LEN", "DATALENGTH", "UPPER", "LOWER", "SUBSTRING", "LTRIM", "RTRIM",
        "TRIM", "LEFT", "RIGHT", "REPLACE", "CONCAT", "REVERSE",
        "ABS", "ROUND", "FLOOR", "CEILING", "CEIL", "SIGN", "SQRT", "POWER",
        "ISNULL", "COALESCE", "NULLIF", "IIF",
        "GETDATE", "CURRENT_TIMESTAMP", "SYSDATETIME", "NEWID",
        "TO_TSVECTOR", "TO_TSQUERY", "PLAINTO_TSQUERY", "TS_RANK",
        NULL
    };
    for (int i = 0; known[i]; i++)
        if (!strcasecmp(name, known[i])) return 1;
    return 0;
}

/* Evaluate a scalar function over already-evaluated arguments. */
int func_call(const char *name, Value *a, int n, Value *out, char *err)
{
    *out = val_null();

    /* ---- functions that tolerate NULL arguments ---- */
    if (!strcasecmp(name, "ISNULL")) {
        if (want_args("ISNULL", n, 2, 2, err) < 0) return -1;
        *out = val_copy(a[0].tag == T_NULL ? &a[1] : &a[0]);
        return 0;
    }
    if (!strcasecmp(name, "COALESCE")) {
        if (want_args("COALESCE", n, 1, MAX_FUNC_ARGS, err) < 0) return -1;
        for (int i = 0; i < n; i++)
            if (a[i].tag != T_NULL) { *out = val_copy(&a[i]); return 0; }
        return 0;                              /* all NULL */
    }
    if (!strcasecmp(name, "NULLIF")) {
        if (want_args("NULLIF", n, 2, 2, err) < 0) return -1;
        int ok;
        if (val_compare(&a[0], &a[1], &ok) == 0 && ok == 1) return 0;
        *out = val_copy(&a[0]);
        return 0;
    }
    if (!strcasecmp(name, "IIF")) {
        if (want_args("IIF", n, 3, 3, err) < 0) return -1;
        *out = val_copy(val_truthy(&a[0]) ? &a[1] : &a[2]);
        return 0;
    }
    if (!strcasecmp(name, "GETDATE") || !strcasecmp(name, "CURRENT_TIMESTAMP") ||
        !strcasecmp(name, "SYSDATETIME")) {
        if (want_args(name, n, 0, 0, err) < 0) return -1;
        char buf[32];
        now_text(buf, sizeof buf);
        *out = val_text(buf);
        return 0;
    }
    if (!strcasecmp(name, "NEWID")) {
        if (want_args("NEWID", n, 0, 0, err) < 0) return -1;
        uint8_t bytes[16];
        uuid_generate(bytes);
        *out = val_uuid(bytes);
        return 0;
    }

    /* ---- everything below returns NULL if any argument is NULL ---- */
    for (int i = 0; i < n; i++)
        if (a[i].tag == T_NULL) return 0;

    /* string functions */
    if (!strcasecmp(name, "LEN") || !strcasecmp(name, "DATALENGTH")) {
        if (want_args(name, n, 1, 1, err) < 0) return -1;
        char buf[512];
        const char *s;
        as_text(&a[0], buf, sizeof buf, &s);
        *out = val_int((int64_t)strlen(s));
        return 0;
    }
    if (!strcasecmp(name, "UPPER") || !strcasecmp(name, "LOWER")) {
        if (want_args(name, n, 1, 1, err) < 0) return -1;
        char buf[512];
        const char *s;
        as_text(&a[0], buf, sizeof buf, &s);
        size_t len = strlen(s);
        char *copy = malloc(len + 1);
        if (!copy) FN_ERR("out of memory");
        int up = !strcasecmp(name, "UPPER");
        for (size_t i = 0; i < len; i++)
            copy[i] = (char)(up ? toupper((unsigned char)s[i])
                                : tolower((unsigned char)s[i]));
        copy[len] = 0;
        *out = val_text_n(copy, len);
        free(copy);
        return 0;
    }
    if (!strcasecmp(name, "REVERSE")) {
        if (want_args(name, n, 1, 1, err) < 0) return -1;
        char buf[512];
        const char *s;
        as_text(&a[0], buf, sizeof buf, &s);
        size_t len = strlen(s);
        char *copy = malloc(len + 1);
        if (!copy) FN_ERR("out of memory");
        for (size_t i = 0; i < len; i++) copy[i] = s[len - 1 - i];
        copy[len] = 0;
        *out = val_text_n(copy, len);
        free(copy);
        return 0;
    }
    if (!strcasecmp(name, "LTRIM") || !strcasecmp(name, "RTRIM") ||
        !strcasecmp(name, "TRIM")) {
        if (want_args(name, n, 1, 1, err) < 0) return -1;
        char buf[512];
        const char *s;
        as_text(&a[0], buf, sizeof buf, &s);
        const char *b = s;
        const char *e = s + strlen(s);
        if (strcasecmp(name, "RTRIM"))            /* LTRIM or TRIM */
            while (*b && isspace((unsigned char)*b)) b++;
        if (strcasecmp(name, "LTRIM"))            /* RTRIM or TRIM */
            while (e > b && isspace((unsigned char)e[-1])) e--;
        *out = val_text_n(b, (size_t)(e - b));
        return 0;
    }
    if (!strcasecmp(name, "SUBSTRING")) {
        if (want_args("SUBSTRING", n, 2, 3, err) < 0) return -1;
        char buf[512];
        const char *s;
        as_text(&a[0], buf, sizeof buf, &s);
        double dstart, dlen = 0;
        if (!as_number(&a[1], &dstart))
            FN_ERR("SUBSTRING needs a numeric start position");
        size_t len = strlen(s);
        /* T-SQL counts from 1 */
        long start = (long)dstart;
        long count;
        if (n == 3) {
            if (!as_number(&a[2], &dlen))
                FN_ERR("SUBSTRING needs a numeric length");
            count = (long)dlen;
            if (count < 0) FN_ERR("SUBSTRING length cannot be negative");
        } else {
            count = (long)len;
        }
        if (start < 1) { count += start - 1; start = 1; }
        if (start > (long)len || count <= 0) { *out = val_text(""); return 0; }
        if ((size_t)(start - 1 + count) > len) count = (long)len - (start - 1);
        *out = val_text_n(s + start - 1, (size_t)count);
        return 0;
    }
    if (!strcasecmp(name, "LEFT") || !strcasecmp(name, "RIGHT")) {
        if (want_args(name, n, 2, 2, err) < 0) return -1;
        char buf[512];
        const char *s;
        as_text(&a[0], buf, sizeof buf, &s);
        double dn;
        if (!as_number(&a[1], &dn)) FN_ERR("%s needs a numeric length", name);
        long k = (long)dn;
        if (k < 0) FN_ERR("%s length cannot be negative", name);
        size_t len = strlen(s);
        if ((size_t)k > len) k = (long)len;
        if (!strcasecmp(name, "LEFT")) *out = val_text_n(s, (size_t)k);
        else                           *out = val_text_n(s + len - (size_t)k, (size_t)k);
        return 0;
    }
    if (!strcasecmp(name, "REPLACE")) {
        if (want_args("REPLACE", n, 3, 3, err) < 0) return -1;
        char b0[512], b1[512], b2[512];
        const char *s, *find, *rep;
        as_text(&a[0], b0, sizeof b0, &s);
        as_text(&a[1], b1, sizeof b1, &find);
        as_text(&a[2], b2, sizeof b2, &rep);
        size_t flen = strlen(find);
        if (flen == 0) { *out = val_text(s); return 0; }

        size_t cap = strlen(s) + 1, used = 0;
        char *res = malloc(cap);
        if (!res) FN_ERR("out of memory");
        for (const char *p = s; *p; ) {
            if (strncasecmp(p, find, flen) == 0) {
                size_t rlen = strlen(rep);
                if (used + rlen + 1 > cap) {
                    cap = (used + rlen + 1) * 2;
                    char *nr = realloc(res, cap);
                    if (!nr) { free(res); FN_ERR("out of memory"); }
                    res = nr;
                }
                memcpy(res + used, rep, rlen);
                used += rlen;
                p += flen;
            } else {
                if (used + 2 > cap) {
                    cap *= 2;
                    char *nr = realloc(res, cap);
                    if (!nr) { free(res); FN_ERR("out of memory"); }
                    res = nr;
                }
                res[used++] = *p++;
            }
        }
        res[used] = 0;
        *out = val_text_n(res, used);
        free(res);
        return 0;
    }
    if (!strcasecmp(name, "CONCAT")) {
        if (want_args("CONCAT", n, 1, MAX_FUNC_ARGS, err) < 0) return -1;
        char joined[1024];
        size_t o = 0;
        joined[0] = 0;
        for (int i = 0; i < n && o < sizeof joined - 1; i++) {
            char buf[512];
            const char *s;
            as_text(&a[i], buf, sizeof buf, &s);
            o += (size_t)snprintf(joined + o, sizeof joined - o, "%s", s);
        }
        *out = val_text(joined);
        return 0;
    }

    /* numeric functions */
    double x, y;
    if (!strcasecmp(name, "ABS") || !strcasecmp(name, "FLOOR") ||
        !strcasecmp(name, "CEILING") || !strcasecmp(name, "CEIL") ||
        !strcasecmp(name, "SIGN") || !strcasecmp(name, "SQRT")) {
        if (want_args(name, n, 1, 1, err) < 0) return -1;
        if (!as_number(&a[0], &x)) FN_ERR("%s needs a number", name);
        int was_int = (a[0].tag == T_INT || a[0].tag == T_BIT);
        if (!strcasecmp(name, "ABS")) {
            if (was_int) { *out = val_int(a[0].i < 0 ? -a[0].i : a[0].i); return 0; }
            *out = val_float(fabs(x));
        } else if (!strcasecmp(name, "SIGN")) {
            *out = val_int(x < 0 ? -1 : (x > 0 ? 1 : 0));
        } else if (!strcasecmp(name, "SQRT")) {
            if (x < 0) FN_ERR("SQRT of a negative number");
            *out = val_float(sqrt(x));
        } else if (!strcasecmp(name, "FLOOR")) {
            *out = was_int ? val_int(a[0].i) : val_float(floor(x));
        } else {
            *out = was_int ? val_int(a[0].i) : val_float(ceil(x));
        }
        return 0;
    }
    if (!strcasecmp(name, "ROUND")) {
        if (want_args("ROUND", n, 1, 2, err) < 0) return -1;
        if (!as_number(&a[0], &x)) FN_ERR("ROUND needs a number");
        int digits = 0;
        if (n == 2) {
            double d;
            if (!as_number(&a[1], &d)) FN_ERR("ROUND needs a numeric digit count");
            digits = (int)d;
            if (digits < 0 || digits > 15)
                FN_ERR("ROUND digits must be between 0 and 15");
        }
        double scale = pow(10.0, digits);
        double r = (x < 0 ? -1 : 1) * floor(fabs(x) * scale + 0.5) / scale;
        *out = (digits == 0 && (a[0].tag == T_INT || a[0].tag == T_BIT))
                 ? val_int((int64_t)r) : val_float(r);
        return 0;
    }
    if (!strcasecmp(name, "POWER")) {
        if (want_args("POWER", n, 2, 2, err) < 0) return -1;
        if (!as_number(&a[0], &x) || !as_number(&a[1], &y))
            FN_ERR("POWER needs two numbers");
        *out = val_float(pow(x, y));
        return 0;
    }

    /* full-text search: to_tsvector / to_tsquery / plainto_tsquery / ts_rank.
     * NULL in, NULL out, like the rest of the functions below the NULL
     * guard. */
    if (!strcasecmp(name, "TO_TSVECTOR") || !strcasecmp(name, "TO_TSQUERY") ||
        !strcasecmp(name, "PLAINTO_TSQUERY")) {
        if (want_args(name, n, 1, 1, err) < 0) return -1;
        char buf[512];
        const char *s;
        as_text(&a[0], buf, sizeof buf, &s);
        char *res = NULL;
        int rc = strcasecmp(name, "TO_TSVECTOR") == 0
                     ? fulltext_to_tsvector(s, &res, err)
                     : strcasecmp(name, "TO_TSQUERY") == 0
                           ? fulltext_to_tsquery(s, &res, err)
                           : fulltext_plainto_tsquery(s, &res, err);
        if (rc < 0) return -1;
        *out = val_text(res);
        free(res);
        return 0;
    }
    if (!strcasecmp(name, "TS_RANK")) {
        if (want_args("TS_RANK", n, 2, 2, err) < 0) return -1;
        char buf1[512], buf2[512];
        const char *s1, *s2;
        as_text(&a[0], buf1, sizeof buf1, &s1);
        as_text(&a[1], buf2, sizeof buf2, &s2);
        *out = val_float(fulltext_rank(s1, s2));
        return 0;
    }

    FN_ERR("unknown function %s", name);
}

/* ------------------------------------------------------------------ CAST */

int cast_value(const Value *in, uint8_t type, uint8_t sub, uint32_t len,
               Value *out, char *err)
{
    *out = val_null();
    if (in->tag == T_NULL) return 0;

    switch (type) {
    case T_INT: {
        double d;
        int is_int = 0;
        int64_t iv = 0;
        if (in->tag == T_INT || in->tag == T_BIT) {
            iv = in->i;
        } else if (in->tag == T_FLOAT) {
            iv = (int64_t)in->f;            /* CAST truncates, by definition */
        } else if (in->tag == T_TEXT) {
            if (!text_to_number(in->s, &d, &is_int, &iv))
                FN_ERR("cannot convert '%s' to %s", in->s ? in->s : "",
                       int_sub_name(sub));
            if (!is_int) iv = (int64_t)d;
        } else {
            FN_ERR("cannot convert a %s to %s", type_name(in->tag),
                   int_sub_name(sub));
        }
        int64_t lo, hi;
        int_range(sub, &lo, &hi);
        if (iv < lo || iv > hi)
            FN_ERR("%lld does not fit in %s", (long long)iv, int_sub_name(sub));
        *out = val_int(iv);
        return 0;
    }
    case T_FLOAT: {
        double d;
        if (!as_number(in, &d))
            FN_ERR("cannot convert %s to a number", type_name(in->tag));
        *out = val_float(d);
        return 0;
    }
    case T_BIT: {
        double d;
        if (!as_number(in, &d)) FN_ERR("cannot convert %s to BIT",
                                       type_name(in->tag));
        if (d != 0 && d != 1) FN_ERR("cannot convert %g to BIT", d);
        *out = val_bit(d != 0);
        return 0;
    }
    case T_UUID: {
        uint8_t bytes[16];
        if (in->tag == T_UUID) { *out = val_copy(in); return 0; }
        char buf[64];
        const char *s;
        as_text(in, buf, sizeof buf, &s);
        if (!uuid_parse(s, bytes)) FN_ERR("'%s' is not a UUID", s);
        *out = val_uuid(bytes);
        return 0;
    }
    case T_TEXT: {
        char buf[512];
        const char *s;
        as_text(in, buf, sizeof buf, &s);
        size_t n = strlen(s);
        if (len && n > len) n = len;         /* CAST truncates deliberately */
        *out = val_text_n(s, n);
        return 0;
    }
    }
    FN_ERR("unsupported CAST target");
}
