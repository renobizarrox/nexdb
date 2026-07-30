/* pager.c - the physical layer.
 *
 * File layout
 *   page 0        : database header
 *   catalog chain : serialized table definitions
 *   links chain   : serialized associative memory edges
 *   heap pages    : one singly-linked chain of slotted pages per table
 *
 * A slotted page keeps a growing slot directory at the front and packs tuple
 * bytes in from the back, which is how most real engines do it:
 *
 *   +--------+-------------+ ...free... +-----------------+
 *   | header | slot dir -> |            | <- tuple bytes  |
 *   +--------+-------------+------------+-----------------+
 */
#define _GNU_SOURCE
#include "nexdb.h"
#include "wal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>

#define MAGIC       "nexdb\x01\x00"
#define MAGIC_LEN   8
/* v2 added per-column length limits, integer widths and key flags.
 * v3 added DEFAULT and IDENTITY metadata, and the packed UUID value type.
 * v4 added multi-index support (Index array replacing single btree_root).
 * v5 added free-list for reusing deleted pages. */
#define FMT_VERSION   6u
#define OVERFLOW_MARKER  0xFFFFu  /* ncols value signalling an overflow row */
#define OVERFLOW_STUB    (TUP_HDR_SIZE + 4u)  /* header + chain_head_pno = 30 */

/* heap page header: next(4) nslots(2) free_start(2) free_end(2) pad(2) */
#define HP_NEXT       0
#define HP_NSLOTS     4
#define HP_FREE_START 6
#define HP_FREE_END   8
#define HP_HDR_SIZE   12
#define SLOT_SIZE     4

/* tuple header: rid(8) access(4) last_access(8) strength(4) ncols(2) */
#define TUP_RID       0
#define TUP_ACCESS    8
#define TUP_LAST      12
#define TUP_STRENGTH  20
#define TUP_NCOLS     24
#define TUP_HDR_SIZE  26

/* chain page header: next(4) used(4) */
#define CH_HDR_SIZE 8
#define CH_PAYLOAD  (PAGE_SIZE - CH_HDR_SIZE)

/* Little-endian field accessors for on-disk page layouts. */
static uint16_t rd16(const uint8_t *p, size_t off)
{ uint16_t v; memcpy(&v, p + off, 2); return v; }
static uint32_t rd32(const uint8_t *p, size_t off)
{ uint32_t v; memcpy(&v, p + off, 4); return v; }
static uint64_t rd64(const uint8_t *p, size_t off)
{ uint64_t v; memcpy(&v, p + off, 8); return v; }
static void wr16(uint8_t *p, size_t off, uint16_t v) { memcpy(p + off, &v, 2); }
static void wr32(uint8_t *p, size_t off, uint32_t v) { memcpy(p + off, &v, 4); }
static void wr64(uint8_t *p, size_t off, uint64_t v) { memcpy(p + off, &v, 8); }

/* --------------------------------------------------------------- raw i/o */

int pager_read(DB *db, uint32_t pno, void *buf)
{
    ssize_t n = pread(db->fd, buf, PAGE_SIZE, (off_t)pno * PAGE_SIZE);
    if (n < 0) { snprintf(db->err, MAX_ERR, "read page %u: %s", pno, strerror(errno)); return -1; }
    if (n < PAGE_SIZE) memset((uint8_t *)buf + n, 0, PAGE_SIZE - n);
    return 0;
}

int pager_write(DB *db, uint32_t pno, const void *buf)
{
    if (db->txn_active) {
        if (pager_undo_capture(db, pno) < 0) return -1;
    }
    /* Write to the WAL first and fsync it.  If we crash before the main
     * file write below, recovery will replay this entry. */
    if (wal_append(db, pno, buf) < 0) return -1;
    ssize_t n = pwrite(db->fd, buf, PAGE_SIZE, (off_t)pno * PAGE_SIZE);
    if (n != PAGE_SIZE) {
        snprintf(db->err, MAX_ERR, "write page %u: %s", pno, strerror(errno));
        return -1;
    }
    if (pno >= db->page_count) db->page_count = pno + 1;
    return 0;
}

/* Pop a page from the free list, or extend the file if the list is empty. */
uint32_t pager_alloc(DB *db)
{
    if (db->free_list) {
        uint32_t pno = db->free_list;
        uint8_t pg[PAGE_SIZE];
        if (pager_read(db, pno, pg) < 0) return 0;
        db->free_list = rd32(pg, 0);  /* next free page */
        uint8_t zero[PAGE_SIZE];
        memset(zero, 0, sizeof zero);
        if (pager_write(db, pno, zero) < 0) return 0;
        return pno;
    }
    uint32_t pno = db->page_count;
    uint8_t zero[PAGE_SIZE];
    memset(zero, 0, sizeof zero);
    if (pager_write(db, pno, zero) < 0) return 0;
    db->page_count = pno + 1;
    return pno;
}

/* Push a page onto the free list so pager_alloc can reuse it. */
void pager_free(DB *db, uint32_t pno)
{
    if (pno == 0) return;  /* never free the header page */
    uint8_t pg[PAGE_SIZE];
    memset(pg, 0, sizeof pg);
    wr32(pg, 0, db->free_list);
    if (pager_write(db, pno, pg) < 0) return;
    db->free_list = pno;
}

/* ----------------------------------------------------------- chain pages */

/* Write blob across a chain starting at *head (allocating pages as needed). */
static int chain_write(DB *db, uint32_t *head, const uint8_t *blob, size_t len)
{
    uint8_t pg[PAGE_SIZE];
    size_t off = 0;
    uint32_t pno = *head;

    if (pno == 0) {
        pno = pager_alloc(db);
        if (pno == 0) return -1;
        *head = pno;
    }

    for (;;) {
        if (pager_read(db, pno, pg) < 0) return -1;
        uint32_t next = rd32(pg, 0);
        size_t chunk = len - off;
        if (chunk > CH_PAYLOAD) chunk = CH_PAYLOAD;

        memset(pg + CH_HDR_SIZE, 0, CH_PAYLOAD);
        if (chunk) memcpy(pg + CH_HDR_SIZE, blob + off, chunk);
        wr32(pg, 4, (uint32_t)chunk);
        off += chunk;

        if (off < len) {
            if (next == 0) {
                next = pager_alloc(db);
                if (next == 0) return -1;
            }
            wr32(pg, 0, next);
            if (pager_write(db, pno, pg) < 0) return -1;
            pno = next;
        } else {
            /* keep any surplus pages linked but empty; they get reused later */
            wr32(pg, 0, next);
            if (pager_write(db, pno, pg) < 0) return -1;
            while (next) {
                uint8_t np[PAGE_SIZE];
                if (pager_read(db, next, np) < 0) return -1;
                uint32_t nn = rd32(np, 0);
                wr32(np, 4, 0);
                if (pager_write(db, next, np) < 0) return -1;
                next = nn;
            }
            return 0;
        }
    }
}

/* Read a chain into a malloc'd buffer. Caller frees. */
static int chain_read(DB *db, uint32_t head, uint8_t **out, size_t *out_len)
{
    uint8_t pg[PAGE_SIZE];
    size_t cap = CH_PAYLOAD, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return -1;

    uint32_t pno = head;
    while (pno) {
        if (pager_read(db, pno, pg) < 0) { free(buf); return -1; }
        uint32_t next = rd32(pg, 0);
        uint32_t used = rd32(pg, 4);
        if (used > CH_PAYLOAD) used = CH_PAYLOAD;
        if (len + used > cap) {
            cap = (len + used) * 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        memcpy(buf + len, pg + CH_HDR_SIZE, used);
        len += used;
        pno = next;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

/* ------------------------------------------------------ header + catalog */

static int write_header(DB *db)
{
    uint8_t pg[PAGE_SIZE];
    memset(pg, 0, sizeof pg);
    memcpy(pg, MAGIC, MAGIC_LEN);
    wr32(pg, 8,  FMT_VERSION);
    wr32(pg, 12, db->page_count);
    wr32(pg, 16, db->free_list);
    wr32(pg, 20, db->catalog_page);
    wr32(pg, 24, db->links_page);
    wr64(pg, 28, db->next_rid);
    return pager_write(db, 0, pg);
}

static size_t catalog_serialize(const Catalog *c, uint8_t **out)
{
    size_t cap = 8 + (size_t)c->ntables *
                 (MAX_NAME + 8 + MAX_COLS * (MAX_NAME + 32 + 96) + 64);
    uint8_t *b = malloc(cap);
    if (!b) return 0;
    size_t o = 0;
    wr32(b, o, (uint32_t)c->ntables); o += 4;
    for (int i = 0; i < c->ntables; i++) {
        const Table *t = &c->tables[i];
        memcpy(b + o, t->name, MAX_NAME); o += MAX_NAME;
        wr32(b, o, (uint32_t)t->ncols);   o += 4;
        for (int j = 0; j < t->ncols; j++) {
            const Column *cl = &t->cols[j];
            memcpy(b + o, cl->name, MAX_NAME); o += MAX_NAME;
            b[o++] = cl->type;
            b[o++] = cl->not_null;
            b[o++] = cl->is_pk;
            b[o++] = cl->unique;
            b[o++] = cl->sub;
            b[o++] = cl->is_datetime;
            wr32(b, o, cl->maxlen); o += 4;
            b[o++] = cl->dflt;
            memcpy(b + o, cl->dflt_text, sizeof cl->dflt_text);
            o += sizeof cl->dflt_text;
            b[o++] = cl->identity;
            wr64(b, o, (uint64_t)cl->id_next); o += 8;
            wr64(b, o, (uint64_t)cl->id_step); o += 8;
        }
        wr32(b, o, t->first_page); o += 4;
        wr32(b, o, t->last_page);  o += 4;
        wr64(b, o, (uint64_t)t->nrows); o += 8;
        wr32(b, o, (uint32_t)t->nindexes); o += 4;
        for (int k = 0; k < t->nindexes; k++) {
            wr32(b, o, t->indexes[k].root); o += 4;
            b[o++] = (uint8_t)t->indexes[k].col;
            b[o++] = t->indexes[k].valid;
            b[o++] = 0; b[o++] = 0;
        }
    }
    *out = b;
    return o;
}

static void catalog_deserialize(Catalog *c, const uint8_t *b, size_t len)
{
    memset(c, 0, sizeof *c);
    if (len < 4) return;
    size_t o = 0;
    int n = (int)rd32(b, o); o += 4;
    if (n < 0 || n > MAX_TABLES) n = 0;
    for (int i = 0; i < n; i++) {
        Table *t = &c->tables[i];
        if (o + MAX_NAME + 4 > len) return;
        memcpy(t->name, b + o, MAX_NAME); o += MAX_NAME;
        t->name[MAX_NAME - 1] = 0;
        t->ncols = (int32_t)rd32(b, o); o += 4;
        if (t->ncols < 0 || t->ncols > MAX_COLS) return;
        for (int j = 0; j < t->ncols; j++) {
            Column *cl = &t->cols[j];
            if (o + MAX_NAME + 10 > len) return;
            memcpy(cl->name, b + o, MAX_NAME); o += MAX_NAME;
            cl->name[MAX_NAME - 1] = 0;
            cl->type        = b[o++];
            cl->not_null    = b[o++];
            cl->is_pk       = b[o++];
            cl->unique      = b[o++];
            cl->sub         = b[o++];
            cl->is_datetime = b[o++];
            cl->maxlen      = rd32(b, o); o += 4;
            if (o + 1 + sizeof cl->dflt_text + 17 > len) return;
            cl->dflt        = b[o++];
            memcpy(cl->dflt_text, b + o, sizeof cl->dflt_text);
            cl->dflt_text[sizeof cl->dflt_text - 1] = 0;
            o += sizeof cl->dflt_text;
            cl->identity    = b[o++];
            cl->id_next     = (int64_t)rd64(b, o); o += 8;
            cl->id_step     = (int64_t)rd64(b, o); o += 8;
        }
        if (o + 20 > len) return;
        t->first_page = rd32(b, o); o += 4;
        t->last_page  = rd32(b, o); o += 4;
        t->nrows      = (int64_t)rd64(b, o); o += 8;
        t->nindexes   = (int32_t)rd32(b, o); o += 4;
        if (t->nindexes < 0 || t->nindexes > MAX_INDEXES) t->nindexes = 0;
        for (int k = 0; k < t->nindexes; k++) {
            if (o + 8 > len) { t->nindexes = k; break; }
            t->indexes[k].root  = rd32(b, o); o += 4;
            t->indexes[k].col   = (int8_t)b[o++];
            t->indexes[k].valid = b[o++]; o += 2;
        }
        c->ntables = i + 1;
    }
}

int db_flush_catalog(DB *db)
{
    uint8_t *blob = NULL;
    size_t len = catalog_serialize(&db->cat, &blob);
    if (!blob) { snprintf(db->err, MAX_ERR, "out of memory"); return -1; }
    int rc = chain_write(db, &db->catalog_page, blob, len);
    free(blob);
    if (rc < 0) return -1;
    return write_header(db);
}

/* ------------------------------------------------------------ open/close */

int db_open(DB *db, const char *path)
{
    memset(db, 0, sizeof *db);
    db->wal_fd = -1;
    snprintf(db->path, sizeof db->path, "%s", path);

    db->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (db->fd < 0) {
        snprintf(db->err, MAX_ERR, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    /* Advisory exclusive lock: prevents two processes from corrupting the file */
    if (flock(db->fd, LOCK_EX | LOCK_NB) < 0) {
        close(db->fd);
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            snprintf(db->err, MAX_ERR,
                     "%s is already open in another nexdb process", path);
        else
            snprintf(db->err, MAX_ERR, "flock failed on %s: %s",
                     path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(db->fd, &st) < 0) {
        snprintf(db->err, MAX_ERR, "stat failed: %s", strerror(errno));
        return -1;
    }

    if (st.st_size == 0) {
        /* New database: discard any stale WAL from a previous run. */
        char wp[520];
        snprintf(wp, sizeof wp, "%s.wal", path);
        unlink(wp);

        db->page_count   = 1;
        db->free_list    = 0;
        db->catalog_page = 0;
        db->links_page   = 0;
        db->next_rid     = 1;
        memset(&db->cat, 0, sizeof db->cat);
        if (db_flush_catalog(db) < 0) return -1;
    } else {
        /* Recover any pending WAL entries before reading the header – this
         * ensures the main file is fully consistent. */
        if (wal_recover(db) < 0) return -1;

        uint8_t pg[PAGE_SIZE];
        if (pager_read(db, 0, pg) < 0) return -1;
        if (memcmp(pg, MAGIC, MAGIC_LEN) != 0) {
            snprintf(db->err, MAX_ERR, "%s is not a nexdb database", path);
            return -1;
        }
        uint32_t ver = rd32(pg, 8);
        if (ver != FMT_VERSION) {
            snprintf(db->err, MAX_ERR,
                     "%s was written by format version %u, this build expects "
                     "version %u. Export the data with the older binary and "
                     "reload it, rather than risking a misread.",
                     path, ver, FMT_VERSION);
            return -1;
        }
        db->page_count   = rd32(pg, 12);
        db->free_list    = rd32(pg, 16);
        db->catalog_page = rd32(pg, 20);
        db->links_page   = rd32(pg, 24);
        db->next_rid     = rd64(pg, 28);
        if (db->page_count == 0) db->page_count = 1;
        if (db->next_rid == 0) db->next_rid = 1;

        uint8_t *blob = NULL;
        size_t len = 0;
        if (db->catalog_page && chain_read(db, db->catalog_page, &blob, &len) == 0) {
            catalog_deserialize(&db->cat, blob, len);
            free(blob);
        }
    }

    if (links_load(db) < 0) return -1;
    return 0;
}

/* Push everything to stable storage. Without this, CHECKPOINT only moved bytes
 * into the OS page cache and a power cut still lost them. */
int db_sync(DB *db)
{
    if (db->fd < 0) return 0;
    if (fsync(db->fd) < 0) {
        snprintf(db->err, MAX_ERR, "fsync failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

void db_close(DB *db)
{
    if (db->fd >= 0) {
        links_flush(db);
        db_flush_catalog(db);
        wal_checkpoint(db);
        close(db->fd);
        db->fd = -1;
    }
    wal_close(db);
    links_free(db);
}

/* Used by memory.c to persist the link table through the chain writer. */
int db_write_links_blob(DB *db, const uint8_t *blob, size_t len)
{
    if (chain_write(db, &db->links_page, blob, len) < 0) return -1;
    return write_header(db);
}

int db_read_links_blob(DB *db, uint8_t **out, size_t *len)
{
    if (db->links_page == 0) { *out = NULL; *len = 0; return 0; }
    return chain_read(db, db->links_page, out, len);
}

/* --------------------------------------------------------------- catalog */

Table *cat_find(DB *db, const char *name)
{
    for (int i = 0; i < db->cat.ntables; i++)
        if (strcasecmp(db->cat.tables[i].name, name) == 0)
            return &db->cat.tables[i];
    return NULL;
}

Table *cat_create(DB *db, const char *name, const Column *cols, int ncols)
{
    if (cat_find(db, name)) {
        snprintf(db->err, MAX_ERR, "table '%s' already exists", name);
        return NULL;
    }
    if (db->cat.ntables >= MAX_TABLES) {
        snprintf(db->err, MAX_ERR, "too many tables (max %d)", MAX_TABLES);
        return NULL;
    }
    Table *t = &db->cat.tables[db->cat.ntables];
    memset(t, 0, sizeof *t);
    snprintf(t->name, MAX_NAME, "%s", name);
    t->ncols = ncols;
    for (int i = 0; i < ncols; i++) t->cols[i] = cols[i];
    t->first_page = 0;
    t->last_page = 0;
    t->nrows = 0;

    db->cat.ntables++;
    if (db_flush_catalog(db) < 0) return NULL;
    return t;
}

int cat_drop(DB *db, const char *name)
{
    for (int i = 0; i < db->cat.ntables; i++) {
        if (strcasecmp(db->cat.tables[i].name, name) == 0) {
            /* free all heap pages */
            Table *t = &db->cat.tables[i];
            uint32_t pno = t->first_page;
            while (pno) {
                uint8_t pg[PAGE_SIZE];
                if (pager_read(db, pno, pg) < 0) break;
                uint32_t next = rd32(pg, HP_NEXT);
                pager_free(db, pno);
                pno = next;
            }
            for (int j = i; j < db->cat.ntables - 1; j++)
                db->cat.tables[j] = db->cat.tables[j + 1];
            db->cat.ntables--;
            return db_flush_catalog(db);
        }
    }
    snprintf(db->err, MAX_ERR, "unknown table '%s'", name);
    return -1;
}

int table_col_index(const Table *t, const char *name)
{
    for (int i = 0; i < t->ncols; i++)
        if (strcasecmp(t->cols[i].name, name) == 0) return i;
    return -1;
}

/* ---------------------------------------------------------------- tuples */

/* Serialize a Row into the binary tuple format stored on heap pages. */
static size_t tuple_encode(const Row *r, uint8_t *buf, size_t cap)
{
    size_t o = 0;
    if (cap < TUP_HDR_SIZE) return 0;
    wr64(buf, TUP_RID, r->rid);
    wr32(buf, TUP_ACCESS, r->access_count);
    wr64(buf, TUP_LAST, (uint64_t)r->last_access);
    memcpy(buf + TUP_STRENGTH, &r->strength, 4);
    wr16(buf, TUP_NCOLS, (uint16_t)r->ncols);
    o = TUP_HDR_SIZE;

    for (int i = 0; i < r->ncols; i++) {
        const Value *v = &r->v[i];
        if (o + 1 > cap) return 0;
        buf[o++] = v->tag;
        switch (v->tag) {
        case T_NULL:
            break;
        case T_INT:
            if (o + 8 > cap) return 0;
            wr64(buf, o, (uint64_t)v->i); o += 8;
            break;
        case T_BIT:
            if (o + 1 > cap) return 0;
            buf[o++] = v->i ? 1 : 0;
            break;
        case T_FLOAT:
            if (o + 8 > cap) return 0;
            memcpy(buf + o, &v->f, 8); o += 8;
            break;
        case T_TEXT: {
            uint32_t n = v->slen;
            if (o + 4 + n > cap) return 0;
            wr32(buf, o, n); o += 4;
            if (n) memcpy(buf + o, v->s, n);
            o += n;
            break;
        }
        case T_UUID:
            if (o + 16 > cap) return 0;
            memcpy(buf + o, v->uu, 16);
            o += 16;
            break;
        default: return 0;
        }
    }
    return o;
}

/* Decode a tuple blob into a Row; returns -1 if the bytes are corrupt. */
static int tuple_decode(const uint8_t *buf, size_t len, Row *out)
{
    memset(out, 0, sizeof *out);
    if (len < TUP_HDR_SIZE) return -1;
    out->rid          = rd64(buf, TUP_RID);
    out->access_count = rd32(buf, TUP_ACCESS);
    out->last_access  = (int64_t)rd64(buf, TUP_LAST);
    memcpy(&out->strength, buf + TUP_STRENGTH, 4);
    out->ncols        = (int32_t)rd16(buf, TUP_NCOLS);
    if (out->ncols < 0 || out->ncols > MAX_COLS) return -1;

    size_t o = TUP_HDR_SIZE;
    for (int i = 0; i < out->ncols; i++) {
        if (o + 1 > len) return -1;
        uint8_t tag = buf[o++];
        switch (tag) {
        case T_NULL:
            out->v[i] = val_null();
            break;
        case T_INT:
            if (o + 8 > len) return -1;
            out->v[i] = val_int((int64_t)rd64(buf, o)); o += 8;
            break;
        case T_BIT:
            if (o + 1 > len) return -1;
            out->v[i] = val_bit(buf[o]); o += 1;
            break;
        case T_FLOAT: {
            double d;
            if (o + 8 > len) return -1;
            memcpy(&d, buf + o, 8); o += 8;
            out->v[i] = val_float(d);
            break;
        }
        case T_TEXT: {
            if (o + 4 > len) return -1;
            uint32_t n = rd32(buf, o); o += 4;
            if (o + n > len) return -1;
            out->v[i] = val_text_n((const char *)buf + o, n); o += n;
            break;
        }
        case T_UUID:
            if (o + 16 > len) return -1;
            out->v[i] = val_uuid(buf + o);
            o += 16;
            break;
        default:
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ heap pages */

/* Prepare a fresh heap page: empty slot directory, free space spans the rest. */
static void heap_page_init(uint8_t *pg)
{
    memset(pg, 0, PAGE_SIZE);
    wr32(pg, HP_NEXT, 0);
    wr16(pg, HP_NSLOTS, 0);
    wr16(pg, HP_FREE_START, HP_HDR_SIZE);
    wr16(pg, HP_FREE_END, PAGE_SIZE);
}

/* Pack a tuple into the page's free space at the back; returns the slot index. */
static int heap_page_put(uint8_t *pg, const uint8_t *tup, size_t tlen, uint16_t *slot_out)
{
    uint16_t nslots     = rd16(pg, HP_NSLOTS);
    uint16_t free_start = rd16(pg, HP_FREE_START);
    uint16_t free_end   = rd16(pg, HP_FREE_END);

    /* reuse a dead slot if one exists */
    int reuse = -1;
    for (uint16_t i = 0; i < nslots; i++) {
        if (rd16(pg, HP_HDR_SIZE + i * SLOT_SIZE + 2) == 0) { reuse = i; break; }
    }

    size_t need = tlen + (reuse >= 0 ? 0 : SLOT_SIZE);
    if (free_end < free_start + need) return -1;   /* no room */

    uint16_t off = (uint16_t)(free_end - tlen);
    memcpy(pg + off, tup, tlen);
    wr16(pg, HP_FREE_END, off);

    uint16_t slot;
    if (reuse >= 0) {
        slot = (uint16_t)reuse;
    } else {
        slot = nslots;
        wr16(pg, HP_NSLOTS, (uint16_t)(nslots + 1));
        wr16(pg, HP_FREE_START, (uint16_t)(free_start + SLOT_SIZE));
    }
    wr16(pg, HP_HDR_SIZE + slot * SLOT_SIZE + 0, off);
    wr16(pg, HP_HDR_SIZE + slot * SLOT_SIZE + 2, (uint16_t)tlen);
    *slot_out = slot;
    return 0;
}

/* overflow-chain forward declarations */
static void chain_free(DB *db, uint32_t head);
static int  row_decode(DB *db, const uint8_t *buf, size_t len, Row *out);
static void overflow_stub(const Row *r, uint32_t chain_head, uint8_t *stub);

int heap_insert(DB *db, Table *t, Row *r)
{
    uint8_t tup[MAX_ROW_SIZE];
    int overflow = 0;
    if (r->rid == 0) r->rid = db->next_rid++;
    if (r->strength <= 0) r->strength = (float)MEM_INIT_STRENGTH;
    if (r->last_access == 0) r->last_access = mem_now();

    size_t tlen = tuple_encode(r, tup, sizeof tup);
    if (tlen == 0) {
        snprintf(db->err, MAX_ERR, "row too large (max %zu bytes)", sizeof tup);
        return -1;
    }

    size_t inline_max = PAGE_SIZE - HP_HDR_SIZE - SLOT_SIZE;
    uint8_t stub[OVERFLOW_STUB];
    uint32_t chain_head = 0;
    size_t stub_len = tlen;

    if (tlen > inline_max) {
        overflow = 1;
        chain_head = 0;
        if (chain_write(db, &chain_head, tup, tlen) < 0) return -1;
        overflow_stub(r, chain_head, stub);
        stub_len = OVERFLOW_STUB;
    }

    uint8_t pg[PAGE_SIZE];
    uint32_t pno = t->first_page, prev = 0;
    const uint8_t *put_data = overflow ? stub : tup;
    while (pno) {
        if (pager_read(db, pno, pg) < 0) {
            if (overflow) chain_free(db, chain_head);
            return -1;
        }
        uint16_t slot;
        if (heap_page_put(pg, put_data, stub_len, &slot) == 0) {
            if (pager_write(db, pno, pg) < 0) {
                if (overflow) chain_free(db, chain_head);
                return -1;
            }
            r->ref.page = pno;
            r->ref.slot = slot;
            t->nrows++;
            return db_flush_catalog(db);
        }
        prev = pno;
        pno = rd32(pg, HP_NEXT);
    }

    uint32_t np = pager_alloc(db);
    if (np == 0) {
        if (overflow) chain_free(db, chain_head);
        snprintf(db->err, MAX_ERR, "cannot allocate page");
        return -1;
    }
    heap_page_init(pg);
    uint16_t slot;
    if (heap_page_put(pg, put_data, stub_len, &slot) < 0) {
        if (overflow) chain_free(db, chain_head);
        snprintf(db->err, MAX_ERR, "row does not fit in an empty page");
        return -1;
    }
    if (pager_write(db, np, pg) < 0) {
        if (overflow) chain_free(db, chain_head);
        return -1;
    }

    if (prev == 0) {
        t->first_page = np;
    } else {
        uint8_t pp[PAGE_SIZE];
        if (pager_read(db, prev, pp) < 0) {
            if (overflow) chain_free(db, chain_head);
            return -1;
        }
        wr32(pp, HP_NEXT, np);
        if (pager_write(db, prev, pp) < 0) {
            if (overflow) chain_free(db, chain_head);
            return -1;
        }
    }
    t->last_page = np;
    r->ref.page = np;
    r->ref.slot = slot;
    t->nrows++;
    return db_flush_catalog(db);
}

/* Return 1 if every slot on the page is a tombstone (offset==0, length==0). */
static int page_is_empty(const uint8_t *pg)
{
    uint16_t nslots = rd16(pg, HP_NSLOTS);
    for (uint16_t i = 0; i < nslots; i++) {
        uint16_t off = rd16(pg, HP_HDR_SIZE + i * SLOT_SIZE + 0);
        uint16_t len = rd16(pg, HP_HDR_SIZE + i * SLOT_SIZE + 2);
        if (off != 0 || len != 0) return 0;
    }
    return 1;
}

/* Free every heap page linked from first_page onward and reset the chain.
 * Also frees any overflow chains on rows that are still live. */
void heap_free_pages(DB *db, Table *t)
{
    uint32_t pno = t->first_page;
    while (pno) {
        uint8_t pg[PAGE_SIZE];
        if (pager_read(db, pno, pg) < 0) break;
        uint16_t nslots = rd16(pg, HP_NSLOTS);
        for (uint16_t i = 0; i < nslots; i++) {
            uint16_t off = rd16(pg, HP_HDR_SIZE + i * SLOT_SIZE + 0);
            uint16_t len = rd16(pg, HP_HDR_SIZE + i * SLOT_SIZE + 2);
            if (off >= TUP_HDR_SIZE + 4 && len >= TUP_HDR_SIZE + 4) {
                if (rd16(pg + off, TUP_NCOLS) == OVERFLOW_MARKER)
                    chain_free(db, rd32(pg + off, TUP_HDR_SIZE));
            }
        }
        uint32_t next = rd32(pg, HP_NEXT);
        pager_free(db, pno);
        pno = next;
    }
    t->first_page = 0;
    t->last_page  = 0;
    t->nrows      = 0;
}

int heap_delete(DB *db, Table *t, RowRef ref)
{
    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, ref.page, pg) < 0) return -1;
    uint16_t nslots = rd16(pg, HP_NSLOTS);
    if (ref.slot >= nslots) { snprintf(db->err, MAX_ERR, "bad row reference"); return -1; }
    uint16_t off = rd16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 0);
    uint16_t len = rd16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 2);
    if (off >= TUP_HDR_SIZE + 4 && len >= TUP_HDR_SIZE + 4) {
        if (rd16(pg + off, TUP_NCOLS) == OVERFLOW_MARKER) {
            uint32_t head = rd32(pg + off, TUP_HDR_SIZE);
            chain_free(db, head);
        }
    }
    wr16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 0, 0);
    wr16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 2, 0);
    if (pager_write(db, ref.page, pg) < 0) return -1;
    if (t->nrows > 0) t->nrows--;

    /* If the page is now completely empty, unlink it from the heap chain
     * and return it to the free list for reuse. */
    if (page_is_empty(pg)) {
        uint32_t prev = 0;
        uint32_t cur  = t->first_page;
        while (cur && cur != ref.page) {
            uint8_t cp[PAGE_SIZE];
            if (pager_read(db, cur, cp) < 0) return 0;
            prev = cur;
            cur  = rd32(cp, HP_NEXT);
        }
        if (cur == ref.page) {
            uint32_t next = rd32(pg, HP_NEXT);
            if (prev == 0) {
                t->first_page = next;
            } else {
                uint8_t pp[PAGE_SIZE];
                if (pager_read(db, prev, pp) < 0) return 0;
                wr32(pp, HP_NEXT, next);
                if (pager_write(db, prev, pp) < 0) return 0;
            }
            if (t->last_page == ref.page) t->last_page = prev;
            pager_free(db, ref.page);
            db_flush_catalog(db);
        }
    }
    return 0;
}

int heap_replace(DB *db, Table *t, RowRef ref, Row *r)
{
    /* Simple approach: tombstone the old tuple, append the new one. The rid
     * and memory metadata carry over, so the row keeps its history. */
    if (heap_delete(db, t, ref) < 0) return -1;   /* nrows-- */
    return heap_insert(db, t, r);                 /* nrows++ : net unchanged */
}

/* ----------------------------------------------------------------- scans */

void scan_init(Scan *s, DB *db, Table *t)
{
    s->db = db;
    s->t = t;
    s->page = t->first_page;
    s->slot = 0;
    s->loaded = 0;
}

/* Walk the singly-linked page chain and yield live rows, skipping tombstones. */
int scan_next(Scan *s, Row *out)
{
    for (;;) {
        if (s->page == 0) return 0;
        if (!s->loaded) {
            if (pager_read(s->db, s->page, s->buf) < 0) return 0;
            s->loaded = 1;
            s->slot = 0;
        }
        /* Everything read out of a page here is untrusted: a corrupt or
         * truncated file can claim any slot count and any offset. Validate
         * against the page bounds before dereferencing, or a garbage slot
         * sends tuple_decode reading off the end of the buffer. */
        uint16_t nslots = rd16(s->buf, HP_NSLOTS);
        if (nslots > (PAGE_SIZE - HP_HDR_SIZE) / SLOT_SIZE)
            nslots = (PAGE_SIZE - HP_HDR_SIZE) / SLOT_SIZE;
        while (s->slot < (int)nslots) {
            uint16_t off = rd16(s->buf, HP_HDR_SIZE + s->slot * SLOT_SIZE + 0);
            uint16_t len = rd16(s->buf, HP_HDR_SIZE + s->slot * SLOT_SIZE + 2);
            int slot = s->slot++;
            if (len == 0 || off == 0) continue;      /* dead slot */
            if (off < HP_HDR_SIZE || (size_t)off + len > PAGE_SIZE)
                continue;                            /* impossible slot: skip */
            if (row_decode(s->db, s->buf + off, len, out) < 0) continue;
            out->ref.page = s->page;
            out->ref.slot = (uint16_t)slot;
            return 1;
        }
        s->page = rd32(s->buf, HP_NEXT);
        s->loaded = 0;
    }
}

int heap_read_meta(DB *db, RowRef ref, uint32_t *access, int64_t *last, float *strength)
{
    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, ref.page, pg) < 0) return -1;
    uint16_t nslots = rd16(pg, HP_NSLOTS);
    if (ref.slot >= nslots) return -1;
    uint16_t off = rd16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 0);
    uint16_t len = rd16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 2);
    if (off == 0 || len < TUP_HDR_SIZE) return -1;
    if (off < HP_HDR_SIZE || (size_t)off + len > PAGE_SIZE) return -1;
    *access   = rd32(pg, off + TUP_ACCESS);
    *last     = (int64_t)rd64(pg, off + TUP_LAST);
    memcpy(strength, pg + off + TUP_STRENGTH, 4);
    return 0;
}

/* Read a full row from a heap page by its row reference. */
int heap_read_row(DB *db, RowRef ref, Row *out)
{
    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, ref.page, pg) < 0) return -1;
    uint16_t nslots = rd16(pg, HP_NSLOTS);
    if (ref.slot >= nslots) return -1;
    uint16_t off = rd16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 0);
    uint16_t len = rd16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 2);
    if (off == 0 || len < TUP_HDR_SIZE) return -1;
    if (off < HP_HDR_SIZE || (size_t)off + len > PAGE_SIZE) return -1;
    if (row_decode(db, pg + off, len, out) < 0) return -1;
    out->ref = ref;
    return 0;
}

/* Rewrite just the memory metadata of a tuple, in place. Fixed-size fields at
 * the head of the tuple make this a cheap surgical update. */
int heap_update_meta(DB *db, RowRef ref, uint32_t access, int64_t last, float strength)
{
    uint8_t pg[PAGE_SIZE];
    if (pager_read(db, ref.page, pg) < 0) return -1;
    uint16_t nslots = rd16(pg, HP_NSLOTS);
    if (ref.slot >= nslots) return -1;
    uint16_t off = rd16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 0);
    uint16_t len = rd16(pg, HP_HDR_SIZE + ref.slot * SLOT_SIZE + 2);
    if (off == 0 || len < TUP_HDR_SIZE) return -1;
    if (off < HP_HDR_SIZE || (size_t)off + len > PAGE_SIZE) return -1;
    wr32(pg, off + TUP_ACCESS, access);
    wr64(pg, off + TUP_LAST, (uint64_t)last);
    memcpy(pg + off + TUP_STRENGTH, &strength, 4);
    return pager_write(db, ref.page, pg);
}

/* ---------------------------------------------------------------------------
 * Overflow page chain helpers
 *
 * When a serialised row is too large for one heap page we store a 30-byte
 * stub inline (header + ncols = OVERFLOW_MARKER + chain head pno) and write
 * the full encoded tuple to a chain of overflow pages. */

/* Free every page in an overflow chain. */
static void chain_free(DB *db, uint32_t head)
{
    while (head) {
        uint8_t pg[PAGE_SIZE];
        if (pager_read(db, head, pg) < 0) break;
        uint32_t next = rd32(pg, 0);
        pager_free(db, head);
        head = next;
    }
}

/* Decode a row that may be inline (normal) or in an overflow chain.
 * buf/len point to the in-page tuple data.  Returns 0 on success. */
static int row_decode(DB *db, const uint8_t *buf, size_t len, Row *out)
{
    if (len < TUP_HDR_SIZE + 4) goto inline_decode;
    if (rd16(buf, TUP_NCOLS) == OVERFLOW_MARKER) {
        uint32_t head = rd32(buf, TUP_HDR_SIZE);
        uint8_t *chain = NULL;
        size_t clen = 0;
        if (chain_read(db, head, &chain, &clen) < 0) return -1;
        int rc = tuple_decode(chain, clen, out);
        free(chain);
        return rc;
    }
inline_decode:
    return tuple_decode(buf, len, out);
}

/* Encode just the overflow stub for a row (header with OVERFLOW_MARKER
 * and the chain head page number).  Stub is always OVERFLOW_STUB bytes. */
static void overflow_stub(const Row *r, uint32_t chain_head, uint8_t *stub)
{
    memset(stub, 0, OVERFLOW_STUB);
    wr64(stub, TUP_RID, r->rid);
    wr32(stub, TUP_ACCESS, r->access_count);
    wr64(stub, TUP_LAST, (uint64_t)r->last_access);
    memcpy(stub + TUP_STRENGTH, &r->strength, 4);
    wr16(stub, TUP_NCOLS, OVERFLOW_MARKER);
    wr32(stub, TUP_HDR_SIZE, chain_head);
}

/* ---------------------------------------------------------------------------
 * Transaction undo log
 *
 * Before a page is written inside an explicit transaction, its old content
 * is saved so it can be restored on ROLLBACK.  A given page is captured only
 * once per transaction (the first write to it). */

int pager_undo_capture(DB *db, uint32_t pno)
{
    if (!db->txn_active) return 0;
    for (int i = 0; i < db->undo_depth; i++)
        if (db->undo[i].pno == pno) return 0;  /* already captured */
    if (db->undo_depth >= UNDO_MAX) {
        snprintf(db->err, MAX_ERR, "transaction too large: max %d modified pages", UNDO_MAX);
        return -1;
    }
    UndoEntry *e = &db->undo[db->undo_depth++];
    e->pno = pno;
    return pager_read(db, pno, e->old);
}

void pager_undo_rollback(DB *db)
{
    if (!db->txn_active) return;
    /* Restore in reverse order so nested overwrites unroll correctly. */
    for (int i = db->undo_depth - 1; i >= 0; i--) {
        uint32_t pno = db->undo[i].pno;
        /* Log the undo write to the WAL so that a crash during rollback
         * still leaves a consistent state after recovery. */
        wal_append(db, pno, db->undo[i].old);
        pwrite(db->fd, db->undo[i].old, PAGE_SIZE, (off_t)pno * PAGE_SIZE);
    }
    db->undo_depth = 0;
    db->txn_active = 0;
}

void pager_undo_commit(DB *db)
{
    if (!db->txn_active) return;
    fsync(db->fd);
    wal_checkpoint(db);
    db->undo_depth = 0;
    db->txn_active = 0;
}
