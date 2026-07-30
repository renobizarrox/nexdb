# Limitations

What is still missing and what it would take to fix it.

## 1. Index range scans

`WHERE pk = literal` uses the B-tree for O(log n) point lookups. Range queries
(`WHERE pk > 10`, `WHERE pk BETWEEN 1 AND 100`) still do a full table scan.
Secondary indexes exist for UNIQUE constraint enforcement but are not used by
`SELECT` at all.

**To fix:** Extend `scan_init()` to accept an optional index + range (start key,
end key, inclusive/exclusive flags). The executor (`exec.c`) already
distinguishes equality predicates from range predicates; route range predicates
to an index scan. For secondary indexes, teach the query planner to detect
indexed columns in `WHERE` and pick the index.

## 2. No nested transactions

`UNDO_MAX = 256` pages supports a single level of undo. `BEGIN` inside an
active transaction is ignored.

**To fix:** Replace the flat undo array with a stack of savepoints. Each
`BEGIN` pushes a savepoint (recording the current undo depth). `ROLLBACK`
pops back to the most recent savepoint. `COMMIT` pops and discards. This
requires changing the undo infrastructure in `src/pager.c` from an array to
a stack-like structure.

## 3. B-tree pages are not recycled

When a table is dropped or truncated, its heap pages are returned to the
free list. B-tree pages are not — they leak until the file is vacuumed.

**To fix:** In `btree_destroy()` (or at the call site in `cat_drop()` and
`exec_truncate()`), walk every B-tree node page and call `pager_free()` on
it. The B-tree code already has `INDEX_META_PAGE` / `INDEX_LEAF_PAGE` page
tags; walk internal nodes recursively. `VACUUM` already rebuilds the entire
file, so this is mainly relevant for frequent DDL-heavy workloads.

## 4. No network encryption

The TCP server sends everything in cleartext. It binds to loopback by default;
for remote access you must tunnel through SSH (`ssh -L 7890:localhost:7890 host`)
or run behind a TLS proxy (e.g. `nginx`, `stunnel`). The Unix socket (`--unix`)
is a sensible alternative for same-machine access.

**To fix:** Add TLS support. The smallest change would be to accept an optional
`--tls-cert` and `--tls-key` flag, then wrap the socket with OpenSSL or
LibreSSL before the read/write loop in `client_handler`. This adds a dependency.

## 5. No session persistence

Server sessions (transaction state, undo depth) live only in the in-memory
`sessions[]` array. If the server process is killed, all in-flight
transactions are lost. The WAL ensures committed data survives, but the
application must re-authenticate and restart any interrupted `BEGIN`..`COMMIT`
sequence.

**To fix:** Serialise session state to the database file (or a separate
`.sessions` file) on each `COMMIT` / `ROLLBACK`. On startup, reload active
sessions that had an open transaction. This is subtle — the WAL replay
must complete before session state can be restored, and sessions whose
transaction was mid-flight at the time of crash must be auto-rolled-back.

## 6. Execution is serialised

All client requests share a single `DB` handle behind `exec_lock`. Statements
from different clients never execute concurrently. This is safe but limits
throughput on multi-core machines.

**To fix:** Make `DB` re-entrant or use a reader-writer lock so that read-only
queries (`SELECT`, `RECALL`, `SHOW`) can run in parallel while writes
(`INSERT`, `UPDATE`, `DELETE`, DDL) exclude readers. This requires auditing
every global and static variable in the executor for thread safety.

## 7. Single process

`db_open()` acquires an advisory exclusive lock via `flock()`. A second process
gets a clear error. Only one `nexdb` process at a time can open a given
database file. The server accepts up to 64 concurrent clients, but they all
share the same process.

**To fix:** Replace `flock()` with a listening daemon that proxies requests
to worker processes, or use POSIX shared memory + semaphores for multi-process
concurrency. This is a major architectural change.

## 8. Hard limits

| Limit | Constant | Value |
| --- | --- | --- |
| Tables per database | `MAX_TABLES` | 128 |
| Columns per table | `MAX_COLS` | 32 |
| Name length | `MAX_NAME` | 127 chars |
| Indexes per table | `MAX_INDEXES` | 32 |
| INSERT rows per statement | `MAX_INSERT_ROWS` | 128 |
| GROUP BY keys | `MAX_GROUP_KEYS` | 16 |
| ORDER BY keys | `MAX_ORDER_KEYS` | 16 |
| Joins per query | `MAX_JOINS` | 16 |
| Aggregate functions | `MAX_AGGS` | 32 |
| SELECT list items | `MAX_SELECT_ITEMS` | 128 |
| IN list / CASE arms | `MAX_IN_ITEMS` | 128 |
| SET clauses (UPDATE) | `MAX_SET_ITEMS` | 64 |
| Function arguments | `MAX_FUNC_ARGS` | 16 |
| RECALL search terms | `MAX_RECALL_TERMS` | 128 |
| Output columns (result) | `MAX_OUT_COLS` | 64 |
| Maximum connections (server) | `MAX_CONN` | 64 |
| Maximum sessions (server) | `MAX_SESSIONS` | 64 |
| Undo pages per transaction | `UNDO_MAX` | 256 |
| Maximum row size | `MAX_ROW_SIZE` | 64 KB |

Most are single-constant bumps in `include/nexdb.h`. Some have structural
implications:
- `MAX_TABLES` / `MAX_COLS` / `MAX_NAME`: bump the constant, recompile.
- `MAX_ROW_SIZE` is already 64 KB via overflow page chains; increasing it
  further is straightforward.
- `MAX_CONN` / `MAX_SESSIONS`: each connection uses ~260 KB for working
  buffers plus a pthread (8 MB default stack). Bumping beyond a few hundred
  requires reducing per-connection memory.

## 9. Other gaps

- **JOIN with GROUP BY on multiple joins** may have uncovered edge cases — the
  nested-loop executor handles one join at a time.
- **`ALTER COLUMN TYPE`** can widen a type (e.g. `INT` → `BIGINT`) but cannot
  narrow it (e.g. `BIGINT` → `INT`) if existing values overflow.
- **`INSERT ... SELECT`** does not support `TOP`, `ORDER BY`, or aggregates
  in the subquery (these are evaluated per-row during the insert scan, not
  materialised first).
- **No `LIMIT` / `OFFSET`** — T-SQL `TOP` is the only pagination mechanism.
- **No `UNION` / `INTERSECT` / `EXCEPT`** — set operations are not parsed.
- **No `FOREIGN KEY`** — referential integrity is not tracked.
- **No `CHECK` constraints** — arbitrary predicates per column are not stored.
- **No `VIEW`** — stored queries are not supported.
- **No `TRIGGER`** — event-driven logic is not supported.
- **No stored procedures** — procedural SQL is not parsed.
- **Full-text search** is limited to the RECALL engine; no inverted index or
  tokenizer with stop-word removal exists.
- **`VACUUM`** requires free disk space equal to the current database size
  (it writes a temp file, then atomically replaces the original).
