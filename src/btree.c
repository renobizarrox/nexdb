#define _GNU_SOURCE
#include "nexdb.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

static uint16_t rd16(const uint8_t *p, size_t off)
{ uint16_t v; memcpy(&v, p + off, 2); return v; }
static uint32_t rd32(const uint8_t *p, size_t off)
{ uint32_t v; memcpy(&v, p + off, 4); return v; }
static uint64_t rd64(const uint8_t *p, size_t off)
{ uint64_t v; memcpy(&v, p + off, 8); return v; }
static void wr16(uint8_t *p, size_t off, uint16_t v) { memcpy(p + off, &v, 2); }
static void wr32(uint8_t *p, size_t off, uint32_t v) { memcpy(p + off, &v, 4); }
static void wr64(uint8_t *p, size_t off, uint64_t v) { memcpy(p + off, &v, 8); }

#define BT_NEXT     0
#define BT_FLAGS    4
#define BT_NKEYS    5
#define BT_PAD      7
#define BT_HDR_SIZE 8

#define BT_LEAF 1
#define BT_ROOT 2

static size_t key_size(const Value *k)
{
    switch (k->tag) {
    case T_NULL:  return 1;
    case T_INT:
    case T_BIT:   return 1 + 8;
    case T_FLOAT: return 1 + 8;
    case T_TEXT:  return 1 + 4 + k->slen;
    case T_UUID:  return 1 + 16;
    default:      return 1 + 8;
    }
}

static size_t key_encode(const Value *k, uint8_t *buf, size_t cap)
{
    (void)cap;
    size_t o = 0;
    buf[o++] = k->tag;
    switch (k->tag) {
    case T_NULL: break;
    case T_INT:  wr64(buf, o, (uint64_t)k->i); o += 8; break;
    case T_BIT:  buf[o++] = k->i ? 1 : 0; break;
    case T_FLOAT: memcpy(buf + o, &k->f, 8); o += 8; break;
    case T_TEXT: wr32(buf, o, k->slen); o += 4; memcpy(buf + o, k->s, k->slen); o += k->slen; break;
    case T_UUID: memcpy(buf + o, k->uu, 16); o += 16; break;
    default:     wr64(buf, o, (uint64_t)k->i); o += 8; break;
    }
    return o;
}

static int key_decode(const uint8_t *buf, size_t cap, Value *out, size_t *consumed)
{
    if (cap < 1) return -1;
    size_t o = 0;
    uint8_t tag = buf[o++];
    switch (tag) {
    case T_NULL: *out = val_null(); break;
    case T_INT:  if (o + 8 > cap) return -1; *out = val_int((int64_t)rd64(buf, o)); o += 8; break;
    case T_BIT:  if (o + 1 > cap) return -1; *out = val_bit(buf[o]); o += 1; break;
    case T_FLOAT: if (o + 8 > cap) return -1; { double d; memcpy(&d, buf + o, 8); *out = val_float(d); o += 8; break; }
    case T_TEXT: if (o + 4 > cap) return -1; { uint32_t sl = rd32(buf, o); o += 4; if (o + sl > cap) return -1; *out = val_text_n((const char*)buf + o, sl); o += sl; break; }
    case T_UUID: if (o + 16 > cap) return -1; *out = val_uuid(buf + o); o += 16; break;
    default:     return -1;
    }
    *consumed = o;
    return 0;
}

static size_t entry_size(const uint8_t *pg, size_t off, int is_leaf)
{
    size_t o = off;
    uint8_t tag = pg[o++];
    switch (tag) {
    case T_NULL:                                 break;
    case T_INT:  case T_FLOAT:                   o += 8; break;
    case T_BIT:                                  o += 1; break;
    case T_TEXT: { uint32_t sl = rd32(pg, o); o += 4 + sl; break; }
    case T_UUID:                                 o += 16; break;
    default:                                     o += 8; break;
    }
    o += is_leaf ? 6 : 4;
    return o - off;
}

static uint16_t total_used(const uint8_t *pg)
{
    int n = rd16(pg, BT_NKEYS);
    int leaf = pg[BT_FLAGS] & BT_LEAF;
    size_t off = BT_HDR_SIZE;
    if (!leaf) off += 4;
    for (int i = 0; i < n; i++) {
        off += entry_size(pg, (size_t)off, leaf);
    }
    return (uint16_t)off;
}

static int btree_search_pos(const uint8_t *pg, const Value *key, int *exact, char *err)
{
    int n = rd16(pg, BT_NKEYS);
    int leaf = pg[BT_FLAGS] & BT_LEAF;
    (void)err;

    size_t *offs = malloc(sizeof(size_t) * (size_t)(n + 1));
    if (!offs && n > 0) return -1;

    size_t off = BT_HDR_SIZE;
    if (!leaf) off += 4;
    for (int i = 0; i < n; i++) {
        offs[i] = off;
        off += entry_size(pg, off, leaf);
    }

    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        Value mk;
        size_t c;
        if (key_decode(pg + offs[mid], PAGE_SIZE - offs[mid], &mk, &c) < 0) {
            free(offs);
            return -1;
        }
        int ok;
        int cmp = val_compare(&mk, key, &ok);
        val_clear(&mk);
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    free(offs);

    if (lo < n) {
        size_t o = BT_HDR_SIZE;
        if (!leaf) o += 4;
        for (int i = 0; i < lo; i++)
            o += entry_size(pg, o, leaf);
        Value v;
        size_t c;
        if (key_decode(pg + o, PAGE_SIZE - o, &v, &c) < 0) {
            *exact = 0;
            return lo;
        }
        int ok;
        int cmp = val_compare(&v, key, &ok);
        val_clear(&v);
        *exact = (cmp == 0 && ok == 1);
    } else {
        *exact = 0;
    }
    return lo;
}

static uint32_t btree_child(const uint8_t *pg, int pos, const Value *key, int right_of_eq)
{
    int n = rd16(pg, BT_NKEYS);
    if (right_of_eq && pos < n) {
        size_t o = BT_HDR_SIZE + 4;
        for (int i = 0; i < pos; i++)
            o += entry_size(pg, o, 0);
        Value sep;
        size_t c;
        if (key_decode(pg + o, PAGE_SIZE - o, &sep, &c) == 0) {
            int ok;
            int cmp = val_compare(&sep, key, &ok);
            val_clear(&sep);
            if (cmp == 0 && ok == 1) pos++;
        }
    }
    if (pos == 0) return rd32(pg, BT_HDR_SIZE);
    size_t o = BT_HDR_SIZE + 4;
    for (int i = 0; i < pos - 1; i++)
        o += entry_size(pg, o, 0);
    o += entry_size(pg, o, 0);
    return rd32(pg, o - 4);
}

/* Descend from root to the leaf that could hold `key`, choosing children
 * left of equal separators (right_of_eq = 0) or right of them (= 1). */
static uint32_t btree_descend(DB *db, uint32_t root, const Value *key, int right_of_eq, char *err)
{
    uint8_t pg[PAGE_SIZE];
    uint32_t pno = root;
    for (;;) {
        if (pager_read(db, pno, pg) < 0) return 0;
        if (pg[BT_FLAGS] & BT_LEAF) break;
        int pos = btree_search_pos(pg, key, &(int){0}, err);
        if (pos < 0) return 0;
        pno = btree_child(pg, pos, key, right_of_eq);
    }
    return pno;
}

static void btree_insert_entry(uint8_t *pg, int pos, const Value *key, uint32_t child, RowRef ref, int leaf)
{
    int n = rd16(pg, BT_NKEYS);
    size_t new_esize = key_size(key) + (leaf ? 6 : 4);

    size_t *offs = malloc(sizeof(size_t) * (size_t)(n + 2));
    size_t *szs  = malloc(sizeof(size_t) * (size_t)(n + 2));

    size_t off = BT_HDR_SIZE;
    if (!leaf) off += 4;
    for (int i = 0; i < n; i++) {
        offs[i] = off;
        szs[i] = entry_size(pg, off, leaf);
        off += szs[i];
    }
    offs[n] = off;

    size_t insert_off = (pos == n) ? offs[n] : offs[pos];
    size_t tail_len = offs[n] - insert_off;

    memmove(pg + insert_off + new_esize, pg + insert_off, tail_len);

    size_t wo = insert_off;
    wo += key_encode(key, pg + wo, PAGE_SIZE - wo);
    if (leaf) {
        wr32(pg, wo, ref.page);
        wr16(pg, wo + 4, ref.slot);
    } else {
        wr32(pg, wo, child);
    }

    wr16(pg, BT_NKEYS, (uint16_t)(n + 1));
    free(offs);
    free(szs);
}

static void btree_remove_entry(uint8_t *pg, int pos)
{
    int n = rd16(pg, BT_NKEYS);
    if (pos >= n) return;
    int leaf = pg[BT_FLAGS] & BT_LEAF;

    int i;
    size_t off = BT_HDR_SIZE;
    if (!leaf) off += 4;
    for (i = 0; i < pos; i++)
        off += entry_size(pg, off, leaf);
    size_t rm_size = entry_size(pg, off, leaf);
    size_t tail_off = off + rm_size;

    size_t total = BT_HDR_SIZE;
    if (!leaf) total += 4;
    for (i = 0; i < n; i++) {
        total += entry_size(pg, total, leaf);
    }

    memmove(pg + off, pg + tail_off, total - tail_off);
    wr16(pg, BT_NKEYS, (uint16_t)(n - 1));
}

static int btree_insert_level(DB *db, uint32_t pno, const Value *key, RowRef ref,
                              int dup, Value *promote, uint32_t *promote_right,
                              char *err)
{
#ifdef DEBUG_SPLIT
    fprintf(stderr, "IN pno=%u key=%s\n", pno, key->tag == T_TEXT ? key->s : "?");
#endif
    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, pno, pg) < 0) return -1;
    size_t off;

    if (pg[BT_FLAGS] & BT_LEAF) {
        int exact;
        int pos = btree_search_pos(pg, key, &exact, err);
        if (pos < 0) return -1;
        if (!dup && exact) {
            snprintf(err, MAX_ERR, "duplicate key violates UNIQUE constraint");
            return -1;
        }
        size_t need = key_size(key) + 6;
        if (total_used(pg) + need <= PAGE_SIZE) {
            btree_insert_entry(pg, pos, key, 0, ref, 1);
            return pager_write(db, pno, pg);
        }

        /* The leaf is full: fold the new entry in and split into a left part
         * (this page) and a fresh right part. The first key of the right part
         * is promoted to the parent and stays in the right leaf, which is the
         * standard B+tree convention: a promoted separator is never removed
         * from the leaves. Leaf-to-leaf chains are threaded left -> right. */
        int n = rd16(pg, BT_NKEYS);
        Value  *all_keys = malloc(sizeof(Value) * (size_t)(n + 2));
        RowRef *all_refs = malloc(sizeof(RowRef) * (size_t)(n + 2));
        if (!all_keys || !all_refs) {
            free(all_keys); free(all_refs);
            snprintf(err, MAX_ERR, "out of memory");
            return -1;
        }
        off = BT_HDR_SIZE;
        for (int i = 0; i < n; i++) {
            size_t c;
            if (key_decode(pg + off, PAGE_SIZE - off, &all_keys[i], &c) < 0) {
                free(all_keys); free(all_refs);
                return -1;
            }
            off += c;
            all_refs[i].page = rd32(pg, off);
            all_refs[i].slot = rd16(pg, off + 4);
            off += 6;
        }
        all_keys[n] = val_copy(key);
        all_refs[n] = ref;
        n++;

        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                int ok;
                if (val_compare(&all_keys[i], &all_keys[j], &ok) > 0 && ok == 1) {
                    Value tk = all_keys[i]; all_keys[i] = all_keys[j]; all_keys[j] = tk;
                    RowRef tr = all_refs[i]; all_refs[i] = all_refs[j]; all_refs[j] = tr;
                }
            }
        }

        int half = n / 2;
        uint32_t old_next = rd32(pg, BT_NEXT);
        uint32_t right_pno = pager_alloc(db);
        if (right_pno == 0) {
            for (int i = 0; i < n; i++) val_clear(&all_keys[i]);
            free(all_keys); free(all_refs);
            return -1;
        }

        uint8_t right_buf[PAGE_SIZE];
        memset(right_buf, 0, PAGE_SIZE);
        right_buf[BT_FLAGS] = BT_LEAF;
        wr32(right_buf, BT_NEXT, old_next);

        memset(pg, 0, PAGE_SIZE);
        pg[BT_FLAGS] = BT_LEAF;
        wr32(pg, BT_NEXT, right_pno);
        off = BT_HDR_SIZE;
        for (int i = 0; i < half; i++) {
            off += key_encode(&all_keys[i], pg + off, PAGE_SIZE - off);
            wr32(pg, off, all_refs[i].page); off += 4;
            wr16(pg, off, all_refs[i].slot); off += 2;
        }
        wr16(pg, BT_NKEYS, (uint16_t)half);

        off = BT_HDR_SIZE;
        for (int i = half; i < n; i++) {
            off += key_encode(&all_keys[i], right_buf + off, PAGE_SIZE - off);
            wr32(right_buf, off, all_refs[i].page); off += 4;
            wr16(right_buf, off, all_refs[i].slot); off += 2;
        }
        wr16(right_buf, BT_NKEYS, (uint16_t)(n - half));

        *promote = val_copy(&all_keys[half]);
        *promote_right = right_pno;

        for (int i = 0; i < n; i++) val_clear(&all_keys[i]);
        free(all_keys); free(all_refs);

        if (pager_write(db, pno, pg) < 0) return -1;
        if (pager_write(db, right_pno, right_buf) < 0) return -1;
        return 1;
    }

    int exact;
    int pos = btree_search_pos(pg, key, &exact, err);
    if (pos < 0) return -1;

    uint32_t child_pno = btree_child(pg, pos, key, dup ? 0 : 1);

    int rc = btree_insert_level(db, child_pno, key, ref, dup,
                                promote, promote_right, err);
    if (rc <= 0) return rc;

    /* The child split and asked us to absorb (promote, promote_right). */
    size_t pneed = key_size(promote) + 4;
    if (total_used(pg) + pneed <= PAGE_SIZE) {
        int pe;
        int ppos = btree_search_pos(pg, promote, &pe, err);
        if (ppos < 0) return -1;
        btree_insert_entry(pg, ppos, promote, *promote_right, (RowRef){0,0}, 0);
        val_clear(promote);
        return pager_write(db, pno, pg);
    }

    /* This internal node is full too: fold the promotion in, split in two and
     * pass an even higher separator upward. The promoted key is removed from
     * the right part, whose leading child becomes the promoted key's former
     * right subtree. */
    int n = rd16(pg, BT_NKEYS);
    Value     *pk = malloc(sizeof(Value) * (size_t)(n + 2));
    uint32_t  *pc = malloc(sizeof(uint32_t) * (size_t)(n + 2));
    if (!pk || !pc) {
        free(pk); free(pc);
        snprintf(err, MAX_ERR, "out of memory");
        return -1;
    }
    uint32_t leftmost = rd32(pg, BT_HDR_SIZE);
    off = BT_HDR_SIZE + 4;
    for (int i = 0; i < n; i++) {
        size_t c;
        if (key_decode(pg + off, PAGE_SIZE - off, &pk[i], &c) < 0) {
            for (int j = 0; j < i; j++) val_clear(&pk[j]);
            free(pk); free(pc);
            return -1;
        }
        off += c;
        pc[i] = rd32(pg, off);
        off += 4;
    }
    pk[n] = *promote;               /* transfer ownership upward fold-in */
    pc[n] = *promote_right;
    n++;

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int ok;
            if (val_compare(&pk[i], &pk[j], &ok) > 0 && ok == 1) {
                Value tk = pk[i]; pk[i] = pk[j]; pk[j] = tk;
                uint32_t tc = pc[i]; pc[i] = pc[j]; pc[j] = tc;
            }
        }
    }

    int half = n / 2;
    uint32_t right_pno = pager_alloc(db);
    if (right_pno == 0) {
        for (int i = 0; i < n; i++) val_clear(&pk[i]);
        free(pk); free(pc);
        return -1;
    }

    uint8_t rb[PAGE_SIZE];
    memset(rb, 0, PAGE_SIZE);
    off = BT_HDR_SIZE + 4;
    wr32(rb, BT_HDR_SIZE, pc[half]);           /* leading child of right part */
    for (int i = half + 1; i < n; i++) {
        off += key_encode(&pk[i], rb + off, PAGE_SIZE - off);
        wr32(rb, off, pc[i]); off += 4;
    }
    wr16(rb, BT_NKEYS, (uint16_t)(n - half - 1));

    *promote = val_copy(&pk[half]);
    *promote_right = right_pno;

    memset(pg, 0, PAGE_SIZE);
    wr32(pg, BT_HDR_SIZE, leftmost);
    off = BT_HDR_SIZE + 4;
    for (int i = 0; i < half; i++) {
        off += key_encode(&pk[i], pg + off, PAGE_SIZE - off);
        wr32(pg, off, pc[i]); off += 4;
    }
    wr16(pg, BT_NKEYS, (uint16_t)half);

    for (int i = 0; i < n; i++) val_clear(&pk[i]);
    free(pk); free(pc);

    if (pager_write(db, pno, pg) < 0) return -1;
    if (pager_write(db, right_pno, rb) < 0) return -1;
    return 1;
}

int btree_create(Index *idx)
{
    idx->root = 0;
    idx->valid = 1;
    return 0;
}

int btree_find(DB *db, uint32_t root, const Value *key, RowRef *ref, char *err)
{
    if (root == 0) return -1;

    uint8_t pg[PAGE_SIZE];
    uint32_t pno = root;

    for (;;) {
        if (pager_read(db, pno, pg) < 0) return -1;
        int leaf = pg[BT_FLAGS] & BT_LEAF;

        int exact;
        int pos = btree_search_pos(pg, key, &exact, err);
        if (pos < 0) return -1;

        if (leaf) {
            if (!exact) return -1;
            size_t off = BT_HDR_SIZE;
            for (int i = 0; i < pos; i++)
                off += entry_size(pg, off, 1);
            size_t c;
            Value dummy;
            key_decode(pg + off, PAGE_SIZE - off, &dummy, &c);
            off += c;
            ref->page = rd32(pg, off);
            ref->slot = rd16(pg, off + 4);
            val_clear(&dummy);
            return 0;
        }

        pno = btree_child(pg, pos, key, 1);
    }
}

int btree_insert(DB *db, uint32_t *root, const Value *key, RowRef ref, char *err)
{
    if (*root == 0) {
        uint8_t pg[PAGE_SIZE];
        memset(pg, 0, PAGE_SIZE);
        pg[BT_FLAGS] = BT_LEAF | BT_ROOT;
        *root = pager_alloc(db);
        if (*root == 0) return -1;
        if (pager_write(db, *root, pg) < 0) return -1;
        btree_insert_entry(pg, 0, key, 0, ref, 1);
        return pager_write(db, *root, pg);
    }

    Value promote;
    uint32_t right_pno;
    int rc = btree_insert_level(db, *root, key, ref, 0, &promote, &right_pno, err);
    if (rc < 0) return -1;
    if (rc == 1) {
        uint8_t nr[PAGE_SIZE];
        memset(nr, 0, PAGE_SIZE);
        nr[BT_FLAGS] = BT_ROOT;
        size_t ro = BT_HDR_SIZE + 4;
        ro += key_encode(&promote, nr + ro, PAGE_SIZE - ro);
        wr32(nr, ro, right_pno);
        wr16(nr, BT_NKEYS, 1);
        wr32(nr, BT_HDR_SIZE, *root);
        val_clear(&promote);
        uint32_t np = pager_alloc(db);
        if (np == 0) return -1;
        if (pager_write(db, np, nr) < 0) return -1;
        *root = np;
    }
    return 0;
}

/* Like btree_insert but for non-unique keys (GIN posting lists): equal keys
 * are allowed, and equal-key runs are always descended into leftmost and
 * walked forward across sibling leaves, so a run may span split leaves. */
int btree_insert_dup(DB *db, uint32_t *root, const Value *key, RowRef ref, char *err)
{
    if (*root == 0) {
        uint8_t pg[PAGE_SIZE];
        memset(pg, 0, PAGE_SIZE);
        pg[BT_FLAGS] = BT_LEAF | BT_ROOT;
        *root = pager_alloc(db);
        if (*root == 0) return -1;
        if (pager_write(db, *root, pg) < 0) return -1;
        btree_insert_entry(pg, 0, key, 0, ref, 1);
        return pager_write(db, *root, pg);
    }

    Value promote;
    uint32_t right_pno;
    int rc = btree_insert_level(db, *root, key, ref, 1, &promote, &right_pno, err);
    if (rc < 0) return -1;
    if (rc == 1) {
        uint8_t nr[PAGE_SIZE];
        memset(nr, 0, PAGE_SIZE);
        nr[BT_FLAGS] = BT_ROOT;
        size_t ro = BT_HDR_SIZE + 4;
        ro += key_encode(&promote, nr + ro, PAGE_SIZE - ro);
        wr32(nr, ro, right_pno);
        wr16(nr, BT_NKEYS, 1);
        wr32(nr, BT_HDR_SIZE, *root);
        val_clear(&promote);
        uint32_t np = pager_alloc(db);
        if (np == 0) return -1;
        if (pager_write(db, np, nr) < 0) return -1;
        *root = np;
    }
    return 0;
}

int btree_delete(DB *db, uint32_t *root, const Value *key, char *err)
{
    if (*root == 0) return -1;

    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, *root, pg) < 0) return -1;

    int leaf = pg[BT_FLAGS] & BT_LEAF;
    if (!leaf) {
        int exact;
        int pos = btree_search_pos(pg, key, &exact, err);
        if (pos < 0) return -1;

        uint32_t child_pno = btree_child(pg, pos, key, 1);

        uint8_t child[PAGE_SIZE];
        if (pager_read(db, child_pno, child) < 0) return -1;

        if (child[BT_FLAGS] & BT_LEAF) {
            int cex;
            int cpos = btree_search_pos(child, key, &cex, err);
            if (cpos < 0) return -1;
            if (!cex) return -1;

            btree_remove_entry(child, cpos);
            if (pager_write(db, child_pno, child) < 0) return -1;

            /* leaf-level coalescing: if leaf is <50% full try to merge
             * entries from the next sibling */
            if (total_used(child) < PAGE_SIZE / 2) {
                uint32_t sib_pno = rd32(child, BT_NEXT);
                if (sib_pno != 0) {
                    uint8_t sib[PAGE_SIZE];
                    if (pager_read(db, sib_pno, sib) == 0 &&
                        (sib[BT_FLAGS] & BT_LEAF)) {
                        /* collect sibling entries */
                        int sn = rd16(sib, BT_NKEYS);
                        size_t soff = BT_HDR_SIZE;
                        int ok = 1;
                        for (int si = 0; si < sn && ok; si++) {
                            size_t es = entry_size(sib, soff, 1);
                            if (total_used(child) + es <= PAGE_SIZE) {
                                Value k;
                                size_t c;
                                key_decode(sib + soff, PAGE_SIZE - soff, &k, &c);
                                RowRef sr = {rd32(sib, soff + c),
                                             rd16(sib, soff + c + 4)};
                                btree_insert_entry(child, rd16(child, BT_NKEYS),
                                                   &k, 0, sr, 1);
                                val_clear(&k);
                            } else { ok = 0; }
                            soff += es;
                        }
                        if (ok && sn > 0) {
                            wr32(child, BT_NEXT, rd32(sib, BT_NEXT));
                        }
                        pager_write(db, child_pno, child);
                    }
                }
            }

            int np = rd16(pg, BT_NKEYS);
            if (np == 0) {
                *root = child_pno;
                pg[BT_FLAGS] = (uint8_t)(pg[BT_FLAGS] | BT_LEAF);
                if (pager_read(db, *root, pg) < 0) return -1;
                pg[BT_FLAGS] |= BT_ROOT | BT_LEAF;
                return pager_write(db, *root, pg);
            }
            return 0;
        }

        return btree_delete(db, &child_pno, key, err);
    }

    int exact;
    int pos = btree_search_pos(pg, key, &exact, err);
    if (pos < 0) return -1;
    if (!exact) return -1;

    btree_remove_entry(pg, pos);
    return pager_write(db, *root, pg);
}

int btree_destroy(DB *db, uint32_t root)
{
    if (root == 0) return 0;
    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, root, pg) < 0) return 0;
    if (!(pg[BT_FLAGS] & BT_LEAF)) {
        int n = rd16(pg, BT_NKEYS);
        size_t off = BT_HDR_SIZE + 4;
        uint32_t first = rd32(pg, BT_HDR_SIZE);
        btree_destroy(db, first);
        for (int i = 0; i < n; i++) {
            off += entry_size(pg, off, 0);
            off -= 4;
            uint32_t cp = rd32(pg, off);
            btree_destroy(db, cp);
        }
    }
    pager_free(db, root);
    return 0;
}

int btree_has_key(DB *db, uint32_t root, const Value *key, char *err)
{
    RowRef dummy;
    return btree_find(db, root, key, &dummy, err) == 0;
}

/* Visit every entry whose key equals `key`, in ascending order, starting at
 * the leftmost occurrence. Equal-key runs may span sibling leaves (a split
 * may cut through a run); the walk follows the BT_NEXT chain while the next
 * leaf still begins with the key. Returns 0 on success, -1 on error; the
 * visit callback may return nonzero to stop the walk early. */
int btree_run_foreach(DB *db, uint32_t root, const Value *key,
                      int (*visit)(void *ctx, RowRef ref), void *ctx, char *err)
{
    if (root == 0) return 0;

    uint32_t pno = btree_descend(db, root, key, 0, err);
    if (pno == 0) return -1;

    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, pno, pg) < 0) return -1;

    for (;;) {
        int n = rd16(pg, BT_NKEYS);
        int exact;
        int pos = btree_search_pos(pg, key, &exact, err);
        if (pos < 0) return -1;

        if (pos < n) {
            Value k;
            size_t c;
            size_t off = BT_HDR_SIZE;
            for (int i = 0; i < pos; i++)
                off += entry_size(pg, off, 1);
            if (key_decode(pg + off, PAGE_SIZE - off, &k, &c) == 0) {
                int ok;
                if (val_compare(&k, key, &ok) == 0 && ok == 1) {
                    for (int i = pos; i < n; i++) {
                        if (i > pos) {
                            off += entry_size(pg, off, 1);
                            if (key_decode(pg + off, PAGE_SIZE - off, &k, &c) < 0)
                                { val_clear(&k); return -1; }
                        }
                        int ok2;
                        if (val_compare(&k, key, &ok2) != 0 || ok2 != 1) break;
                        RowRef ref = { rd32(pg, off + c), rd16(pg, off + c + 4) };
                        if (visit && visit(ctx, ref) != 0) {
                            val_clear(&k);
                            return 0;
                        }
                    }
                }
                val_clear(&k);
            }
        }

        uint32_t next = rd32(pg, BT_NEXT);
        if (next == 0) return 0;
        if (pager_read(db, next, pg) < 0) return -1;
        if (rd16(pg, BT_NKEYS) == 0) continue;
        Value k0;
        size_t c0;
        if (key_decode(pg + BT_HDR_SIZE, PAGE_SIZE - BT_HDR_SIZE, &k0, &c0) < 0)
            return -1;
        int ok0;
        int cmp0 = val_compare(&k0, key, &ok0);
        val_clear(&k0);
        if (cmp0 != 0 || ok0 != 1) return 0;
    }
}

/* Delete the entry with the given key and row reference (GIN posting-list
 * removal). The reference distinguishes the entry within a run of equal
 * keys, which may span sibling leaves. */
int btree_delete_ref(DB *db, uint32_t *root, const Value *key, RowRef ref, char *err)
{
    if (*root == 0) {
        snprintf(err, MAX_ERR, "index is empty");
        return -1;
    }

    uint32_t pno = btree_descend(db, *root, key, 0, err);
    if (pno == 0) {
        snprintf(err, MAX_ERR, "descent failed while deleting from index");
        return -1;
    }

#ifdef DEBUG_DELREF
    fprintf(stderr, "DRF: descend root=%u -> %u key=%s ref=(%u,%u)\n",
            *root, pno, key->tag == T_TEXT ? key->s : "?", ref.page, ref.slot);
#endif
    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, pno, pg) < 0) {
#ifdef DEBUG_DELREF
        fprintf(stderr, "DRF: pager_read %u failed: %s\n", pno, db->err);
#endif
        return -1;
    }

    for (;;) {
        int n = rd16(pg, BT_NKEYS);
        int exact;
        int pos = btree_search_pos(pg, key, &exact, err);
        if (pos < 0) {
#ifdef DEBUG_DELREF
            fprintf(stderr, "DRF: search_pos fail on leaf %u\n", pno);
#endif
            return -1;
        }

        if (pos < n) {
            size_t off = BT_HDR_SIZE;
            for (int i = 0; i < pos; i++)
                off += entry_size(pg, off, 1);
            for (int i = pos; i < n; i++) {
                Value k;
                size_t c;
                if (key_decode(pg + off, PAGE_SIZE - off, &k, &c) < 0) {
#ifdef DEBUG_DELREF
                    fprintf(stderr, "DRF: scan decode fail leaf=%u i=%d off=%zu\n",
                            pno, i, off);
#endif
                    return -1;
                }
                int ok;
                int cmp = val_compare(&k, key, &ok);
                val_clear(&k);
                if (cmp != 0 || ok != 1) break;
                if (rd32(pg, off + c) == ref.page && rd16(pg, off + c + 4) == ref.slot) {
                    btree_remove_entry(pg, i);
                    return pager_write(db, pno, pg);
                }
                off += entry_size(pg, off, 1);
            }
        }

        uint32_t next = rd32(pg, BT_NEXT);
        if (next == 0) {
#ifdef DEBUG_DELREF
            fprintf(stderr, "DRF MISS: key=%s ref=(%u,%u)\n",
                    key->tag == T_TEXT ? key->s : "?", ref.page, ref.slot);
#endif
            snprintf(err, MAX_ERR, "index entry for row (page %u, slot %u) not found",
                     ref.page, ref.slot);
            return -1;
        }
        pno = next;
        if (pager_read(db, next, pg) < 0) {
#ifdef DEBUG_DELREF
            fprintf(stderr, "DRF: pager_read next %u failed: %s\n", next, db->err);
#endif
            return -1;
        }
        if (rd16(pg, BT_NKEYS) == 0) continue;
        Value k0;
        size_t c0;
        if (key_decode(pg + BT_HDR_SIZE, PAGE_SIZE - BT_HDR_SIZE, &k0, &c0) < 0) {
#ifdef DEBUG_DELREF
            fprintf(stderr, "DRF: first-key decode fail on leaf %u\n", next);
#endif
            return -1;
        }
        int ok0;
        int cmp0 = val_compare(&k0, key, &ok0);
        val_clear(&k0);
        if (cmp0 != 0 || ok0 != 1) return -1;
    }
}
