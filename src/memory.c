/* memory.c - the associative memory layer.
 *
 * Two mechanisms, both borrowed from how biological memory is modelled:
 *
 * 1. Strength with decay (Ebbinghaus forgetting curve).
 *    Every row carries a strength value and the timestamp of its last access.
 *    Strength decays exponentially with elapsed time and gets a boost each
 *    time the row is actually used by a query:
 *
 *        strength_now = stored * 2^(-elapsed / halflife)
 *        on access:  stored = strength_now + BOOST
 *
 *    A row read every day climbs; a row nobody touches fades toward zero but
 *    is never deleted. This is why the engine "remembers what you use".
 *
 * 2. Hebbian association (cells that fire together, wire together).
 *    Rows returned by the same statement get pairwise edges reinforced. RECALL
 *    then does one round of spreading activation over those edges, so asking
 *    about one thing surfaces the things it tends to appear with.
 */
#define _GNU_SOURCE
#include "nexdb.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* Wall clock, plus an optional offset in seconds from NEXDB_TIME_OFFSET.
 * The offset exists so you can watch a week of forgetting happen instantly:
 *
 *     NEXDB_TIME_OFFSET=$((14*86400)) nexdb brain.ndb -c "SHOW MEMORY FROM notes"
 *
 * Nothing else in the engine depends on the real clock. */
int64_t mem_now(void)
{
    static int64_t offset;
    static int loaded;
    if (!loaded) {
        const char *env = getenv("NEXDB_TIME_OFFSET");
        offset = env ? strtoll(env, NULL, 10) : 0;
        loaded = 1;
    }
    return (int64_t)time(NULL) + offset;
}

double mem_strength_at(double stored, int64_t last_access, int64_t now)
{
    if (stored <= 0) return 0.0;
    double elapsed = (double)(now - last_access);
    if (elapsed < 0) elapsed = 0;
    double decayed = stored * pow(2.0, -elapsed / MEM_HALFLIFE_SECS);
    return decayed < 1e-9 ? 0.0 : decayed;
}

double mem_row_strength(const Row *r, int64_t now)
{
    return mem_strength_at(r->strength, r->last_access, now);
}

/* Reinforce one row: decay to the present, add the boost, persist. */
int mem_touch(DB *db, Table *t, RowRef ref, double boost)
{
    (void)t;
    int64_t now = mem_now();
    uint32_t access;
    int64_t last;
    float stored;

    /* read the live metadata straight out of the page, decay it forward, boost */
    if (heap_read_meta(db, ref, &access, &last, &stored) < 0) return -1;

    double cur = mem_strength_at(stored, last, now) + boost;
    if (cur > MEM_MAX_STRENGTH) cur = MEM_MAX_STRENGTH;
    if (cur < 0) cur = 0;

    return heap_update_meta(db, ref, access + 1, now, (float)cur);
}

/* --------------------------------------------------- association hash map */

/* MurmurHash-style 64-bit mixer for association-table probing. */
static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Open-addressing slot index for the ordered pair (a, b). */
static uint32_t link_hash(uint64_t a, uint64_t b, uint32_t cap)
{
    return (uint32_t)(mix64(a * 0x9e3779b97f4a7c15ULL ^ b) & (cap - 1));
}

/* Allocate an empty association hash table with power-of-two capacity. */
static int links_init(LinkStore *ls, uint32_t cap)
{
    ls->e = calloc(cap, sizeof(Link));
    if (!ls->e) return -1;
    ls->cap = cap;
    ls->n = 0;
    ls->dirty = 0;
    return 0;
}

/* Find or optionally create the slot for edge (a, b); a must be less than b. */
static Link *link_slot(LinkStore *ls, uint64_t a, uint64_t b, int create)
{
    if (!ls->e) return NULL;
    uint32_t h = link_hash(a, b, ls->cap);
    for (uint32_t probe = 0; probe < ls->cap; probe++) {
        Link *l = &ls->e[(h + probe) & (ls->cap - 1)];
        if (l->a == 0) {
            if (!create) return NULL;
            l->a = a; l->b = b; l->w = 0;
            ls->n++;
            return l;
        }
        if (l->a == a && l->b == b) return l;
    }
    return NULL;
}

/* When the table gets crowded, halve every weight and drop the weakest edges.
 * That is deliberate: associations you stopped using should not crowd out new
 * ones forever. */
static void links_prune(LinkStore *ls)
{
    Link *old = ls->e;
    uint32_t oldcap = ls->cap;
    Link *fresh = calloc(oldcap, sizeof(Link));
    if (!fresh) return;

    ls->e = fresh;
    ls->n = 0;
    for (uint32_t i = 0; i < oldcap; i++) {
        if (old[i].a == 0) continue;
        float w = old[i].w * 0.5f;
        if (w < 0.05f) continue;
        Link *l = link_slot(ls, old[i].a, old[i].b, 1);
        if (l) l->w = w;
    }
    free(old);
    ls->dirty = 1;
}

/* Reinforce pairwise edges between every row in rids (Hebbian co-access). */
void mem_associate(DB *db, const uint64_t *rids, int n, double boost)
{
    if (n < 2) return;
    if (n > MEM_COACT_MAX) n = MEM_COACT_MAX;
    LinkStore *ls = &db->links;
    if (!ls->e && links_init(ls, 1024) < 0) return;

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            uint64_t a = rids[i], b = rids[j];
            if (a == b || a == 0 || b == 0) continue;
            if (a > b) { uint64_t tmp = a; a = b; b = tmp; }

            if (ls->n * 10 > ls->cap * 7) {
                if (ls->cap < MEM_LINK_CAP) {
                    /* grow */
                    LinkStore bigger;
                    if (links_init(&bigger, ls->cap * 2) == 0) {
                        for (uint32_t k = 0; k < ls->cap; k++) {
                            if (ls->e[k].a == 0) continue;
                            Link *l = link_slot(&bigger, ls->e[k].a, ls->e[k].b, 1);
                            if (l) l->w = ls->e[k].w;
                        }
                        free(ls->e);
                        bigger.dirty = 1;
                        *ls = bigger;
                    }
                } else {
                    links_prune(ls);
                }
            }

            Link *l = link_slot(ls, a, b, 1);
            if (!l) continue;
            l->w += (float)boost;
            if (l->w > MEM_LINK_MAX) l->w = (float)MEM_LINK_MAX;
            ls->dirty = 1;
        }
    }
}

double mem_link_weight(DB *db, uint64_t a, uint64_t b)
{
    if (a > b) { uint64_t t = a; a = b; b = t; }
    Link *l = link_slot(&db->links, a, b, 0);
    return l ? l->w : 0.0;
}

/* Return up to max neighbors of rid, filling parallel rid/weight arrays. */
int mem_neighbors(DB *db, uint64_t rid, uint64_t *out_rid, float *out_w, int max)
{
    LinkStore *ls = &db->links;
    int n = 0;
    if (!ls->e) return 0;
    for (uint32_t i = 0; i < ls->cap && n < max; i++) {
        Link *l = &ls->e[i];
        if (l->a == 0) continue;
        if (l->a == rid)      { out_rid[n] = l->b; out_w[n] = l->w; n++; }
        else if (l->b == rid) { out_rid[n] = l->a; out_w[n] = l->w; n++; }
    }
    return n;
}

int mem_link_count(DB *db)
{
    return (int)db->links.n;
}

/* Set a single link's weight directly (used by VACUUM to copy links). */
void mem_link_set(DB *db, uint64_t a, uint64_t b, float w)
{
    if (a == 0 || b == 0 || a == b) return;
    if (a > b) { uint64_t t = a; a = b; b = t; }
    LinkStore *ls = &db->links;
    if (!ls->e && links_init(ls, 1024) < 0) return;
    Link *l = link_slot(ls, a, b, 1);
    if (l) { l->w = w; ls->dirty = 1; }
}

static int link_cmp_desc(const void *x, const void *y)
{
    const Link *a = x, *b = y;
    if (a->w < b->w) return 1;
    if (a->w > b->w) return -1;
    return 0;
}

int mem_top_links(DB *db, Link *out, int max)
{
    LinkStore *ls = &db->links;
    if (!ls->e || ls->n == 0) return 0;
    Link *all = malloc(sizeof(Link) * ls->n);
    if (!all) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ls->cap; i++) {
        /* skip faded edges: zero weight means the association is gone, and
         * the rows behind it may have been deleted */
        if (ls->e[i].a != 0 && ls->e[i].w > 0 && n < ls->n) all[n++] = ls->e[i];
    }
    qsort(all, n, sizeof(Link), link_cmp_desc);
    int k = (int)(n < (uint32_t)max ? n : (uint32_t)max);
    memcpy(out, all, sizeof(Link) * k);
    free(all);
    return k;
}

void mem_forget_row(DB *db, uint64_t rid)
{
    LinkStore *ls = &db->links;
    if (!ls->e) return;
    for (uint32_t i = 0; i < ls->cap; i++) {
        Link *l = &ls->e[i];
        if (l->a == rid || l->b == rid) { l->w = 0; ls->dirty = 1; }
    }
}

/* ---------------------------------------------------------- persistence */

int links_load(DB *db)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    if (links_init(&db->links, 1024) < 0) {
        snprintf(db->err, MAX_ERR, "out of memory");
        return -1;
    }
    if (db_read_links_blob(db, &blob, &len) < 0) return -1;
    if (!blob || len < 4) { free(blob); return 0; }

    uint32_t count;
    memcpy(&count, blob, 4);
    size_t need = 4 + (size_t)count * 20;
    if (need > len) count = (uint32_t)((len - 4) / 20);

    uint32_t cap = 1024;
    while (cap < count * 2 && cap < MEM_LINK_CAP) cap *= 2;
    if (cap != db->links.cap) {
        free(db->links.e);
        if (links_init(&db->links, cap) < 0) { free(blob); return -1; }
    }

    size_t o = 4;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t a, b;
        float w;
        memcpy(&a, blob + o, 8); o += 8;
        memcpy(&b, blob + o, 8); o += 8;
        memcpy(&w, blob + o, 4); o += 4;
        if (a == 0 || w <= 0) continue;
        Link *l = link_slot(&db->links, a, b, 1);
        if (l) l->w = w;
    }
    free(blob);
    db->links.dirty = 0;
    return 0;
}

int links_flush(DB *db)
{
    LinkStore *ls = &db->links;
    if (!ls->e || !ls->dirty) return 0;

    size_t cap = 4 + (size_t)ls->n * 20;
    uint8_t *blob = malloc(cap ? cap : 4);
    if (!blob) return -1;

    uint32_t count = 0;
    size_t o = 4;
    for (uint32_t i = 0; i < ls->cap; i++) {
        Link *l = &ls->e[i];
        if (l->a == 0 || l->w <= 0) continue;
        if (o + 20 > cap) break;
        memcpy(blob + o, &l->a, 8); o += 8;
        memcpy(blob + o, &l->b, 8); o += 8;
        memcpy(blob + o, &l->w, 4); o += 4;
        count++;
    }
    memcpy(blob, &count, 4);

    int rc = db_write_links_blob(db, blob, o);
    free(blob);
    if (rc == 0) ls->dirty = 0;
    return rc;
}

void links_free(DB *db)
{
    free(db->links.e);
    db->links.e = NULL;
    db->links.cap = 0;
    db->links.n = 0;
}
