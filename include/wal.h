#ifndef WAL_H
#define WAL_H

#include "nexdb.h"

/* Open (or create) the WAL file at <dbpath>.wal */
int wal_open(DB *db);

/* Append one page-write record to the WAL, then fsync the WAL.
 * Returns 0 on success, -1 on error (sets db->err). */
int wal_append(DB *db, uint32_t pno, const uint8_t *data);

/* Recover: replay every valid entry in the WAL into the main database file,
 * then truncate the WAL.  Returns 0 on success (if no WAL exists, returns 0
 * immediately). */
int wal_recover(DB *db);

/* Checkpoint: fsync the main database file, then truncate the WAL so that
 * subsequent recovery has nothing to replay. */
int wal_checkpoint(DB *db);

/* Close the WAL file descriptor. */
void wal_close(DB *db);

#endif /* WAL_H */
