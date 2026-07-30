/* wal.c – write-ahead log for crash safety.
 *
 * Every page write goes through the WAL first (append + fsync) and then to the
 * main database file.  On startup, any WAL entries that were logged but whose
 * main-file write may not have survived are replayed, guaranteeing that no
 * committed write is lost.
 *
 * File format
 *   offset 0:   8 bytes   magic "nexdbwal"
 *   offset 8:   zero or more entries, each 4108 bytes:
 *                 uint32_t  entry_magic  (0x57414C45 = "WALE")
 *                 uint32_t  pno
 *                 uint8_t   data[PAGE_SIZE]  (4096)
 *                 uint32_t  checksum (XOR of all uint32_t words in the entry)
 */

#include "nexdb.h"
#include "wal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#define WAL_FILE_MAGIC   "nexdbwal"
#define WAL_ENTRY_MAGIC  0x57414C45u

/* Size of one WAL entry in bytes. */
#define WAL_ENTRY_BYTES  (4 + 4 + PAGE_SIZE + 4)

/* Compute a simple XOR checksum over the entry fields. */
static uint32_t entry_checksum(uint32_t magic, uint32_t pno,
                               const uint8_t *data)
{
    uint32_t h = magic ^ pno;
    for (int i = 0; i < (int)(PAGE_SIZE / 4); i++)
        h ^= ((const uint32_t *)data)[i];
    return h;
}

/* ------------------------------------------------------------------- open */

int wal_open(DB *db)
{
    if (db->wal_fd >= 0) return 0;
    snprintf(db->wal_path, sizeof db->wal_path, "%s.wal", db->path);
    db->wal_fd = open(db->wal_path, O_RDWR | O_CREAT, 0644);
    if (db->wal_fd < 0) {
        snprintf(db->err, MAX_ERR, "cannot open WAL %s: %s",
                 db->wal_path, strerror(errno));
        return -1;
    }
    /* initialise the file magic if the file is brand new */
    struct stat st;
    if (fstat(db->wal_fd, &st) < 0) {
        snprintf(db->err, MAX_ERR, "stat WAL %s: %s",
                 db->wal_path, strerror(errno));
        close(db->wal_fd); db->wal_fd = -1; return -1;
    }
    if (st.st_size == 0) {
        if (pwrite(db->wal_fd, WAL_FILE_MAGIC, 8, 0) != 8) {
            snprintf(db->err, MAX_ERR, "write WAL header %s: %s",
                     db->wal_path, strerror(errno));
            close(db->wal_fd); db->wal_fd = -1; return -1;
        }
        fsync(db->wal_fd);
    }
    return 0;
}

/* --------------------------------------------------------------- append */

int wal_append(DB *db, uint32_t pno, const uint8_t *data)
{
    if (wal_open(db) < 0) return -1;

    uint8_t buf[WAL_ENTRY_BYTES];
    uint32_t magic = WAL_ENTRY_MAGIC;

    memcpy(buf,      &magic, 4);
    memcpy(buf + 4,  &pno,   4);
    memcpy(buf + 8,  data,   PAGE_SIZE);
    uint32_t cksum = entry_checksum(magic, pno, data);
    memcpy(buf + 8 + PAGE_SIZE, &cksum, 4);

    off_t off = lseek(db->wal_fd, 0, SEEK_END);
    if (off < 0) {
        snprintf(db->err, MAX_ERR, "seek WAL: %s", strerror(errno));
        return -1;
    }
    if (pwrite(db->wal_fd, buf, WAL_ENTRY_BYTES, off) != WAL_ENTRY_BYTES) {
        snprintf(db->err, MAX_ERR, "write WAL entry: %s", strerror(errno));
        return -1;
    }
    /* fsync the WAL before returning – the caller may then write the same
     * page to the main file, and a crash after that main write but before
     * checkpoint can be recovered by replaying this WAL entry. */
    if (fsync(db->wal_fd) < 0) {
        snprintf(db->err, MAX_ERR, "fsync WAL: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------- recover */

int wal_recover(DB *db)
{
    char path[520];
    snprintf(path, sizeof path, "%s.wal", db->path);

    struct stat st;
    if (stat(path, &st) < 0) {
        if (errno == ENOENT) return 0;   /* no WAL at all */
        snprintf(db->err, MAX_ERR, "stat WAL %s: %s", path, strerror(errno));
        return -1;
    }
    if (st.st_size < 8) {
        /* truncated/corrupt – discard */
        unlink(path);
        return 0;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(db->err, MAX_ERR, "open WAL %s for recovery: %s",
                 path, strerror(errno));
        return -1;
    }

    uint8_t hdr[8];
    if (read(fd, hdr, 8) != 8 || memcmp(hdr, WAL_FILE_MAGIC, 8) != 0) {
        close(fd);
        unlink(path);
        return 0;   /* not our WAL, discard it */
    }

    uint8_t entry[WAL_ENTRY_BYTES];
    int nrec = 0;
    while (read(fd, entry, WAL_ENTRY_BYTES) == WAL_ENTRY_BYTES) {
        uint32_t magic, pno, cksum, expected;
        memcpy(&magic, entry, 4);
        if (magic != WAL_ENTRY_MAGIC) break;
        memcpy(&pno, entry + 4, 4);
        memcpy(&cksum, entry + 8 + PAGE_SIZE, 4);
        expected = entry_checksum(magic, pno, entry + 8);
        if (cksum != expected) break;

        if (pwrite(db->fd, entry + 8, PAGE_SIZE,
                   (off_t)pno * PAGE_SIZE) != PAGE_SIZE) {
            close(fd);
            snprintf(db->err, MAX_ERR, "recover write page %u: %s",
                     pno, strerror(errno));
            return -1;
        }
        nrec++;
    }
    close(fd);

    if (nrec > 0 && fsync(db->fd) < 0) {
        snprintf(db->err, MAX_ERR, "recover fsync: %s", strerror(errno));
        return -1;
    }

    /* Re-create the WAL file with just the header (empty) */
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(db->err, MAX_ERR, "recreate WAL %s: %s",
                 path, strerror(errno));
        return -1;
    }
    if (pwrite(fd, WAL_FILE_MAGIC, 8, 0) != 8) {
        snprintf(db->err, MAX_ERR, "write WAL header after recovery: %s",
                 strerror(errno));
        close(fd); return -1;
    }
    fsync(fd);
    close(fd);

    /* Open the clean WAL through the normal path so wal_fd is set up */
    if (db->wal_fd >= 0) { close(db->wal_fd); db->wal_fd = -1; }
    if (wal_open(db) < 0) return -1;

    if (nrec > 0)
        printf("wal: recovered %d page(s) from %s\n", nrec, path);

    return 0;
}

/* ------------------------------------------------------------- checkpoint */

int wal_checkpoint(DB *db)
{
    if (db->wal_fd < 0) return 0;

    /* Make sure all page writes are durable in the main file first */
    if (fsync(db->fd) < 0) {
        snprintf(db->err, MAX_ERR, "checkpoint fsync main: %s",
                 strerror(errno));
        return -1;
    }

    /* Truncate the WAL – every committed write is now in the main file */
    if (ftruncate(db->wal_fd, 0) < 0) {
        snprintf(db->err, MAX_ERR, "checkpoint truncate WAL: %s",
                 strerror(errno));
        return -1;
    }
    /* Write out an empty header so the file is still recognisable */
    if (pwrite(db->wal_fd, WAL_FILE_MAGIC, 8, 0) != 8) {
        snprintf(db->err, MAX_ERR, "checkpoint write WAL header: %s",
                 strerror(errno));
        return -1;
    }
    if (fsync(db->wal_fd) < 0) {
        snprintf(db->err, MAX_ERR, "checkpoint fsync WAL: %s",
                 strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- close */

void wal_close(DB *db)
{
    if (db->wal_fd >= 0) {
        close(db->wal_fd);
        db->wal_fd = -1;
    }
}
