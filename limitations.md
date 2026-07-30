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

## 2. No nested transactions — FIXED

~~`UNDO_MAX = 256` pages supports a single level of undo. `BEGIN` inside an
active transaction is ignored.~~

~~**To fix:** Replace the flat undo array with a stack of savepoints. Each
`BEGIN` pushes a savepoint (recording the current undo depth). `ROLLBACK`
pops back to the most recent savepoint. `COMMIT` pops and discards. This
requires changing the undo infrastructure in `src/pager.c` from an array to
a stack-like structure.~~

Fixed: `BEGIN` inside an active transaction pushes a savepoint (recording the
current undo depth in `txn_sp[]`). `ROLLBACK` in a nested context restores
pages since the most recent savepoint. `COMMIT` in a nested context is a no-op
(the changes stay). The maximum nesting depth is `MAX_SAVEPOINTS = 16`.

## 3. B-tree pages are not recycled — FIXED

~~When a table is dropped or truncated, its heap pages are returned to the
free list. B-tree pages are not — they leak until the file is vacuumed.~~

~~**To fix:** In `btree_destroy()` (or at the call site in `cat_drop()` and
`exec_truncate()`), walk every B-tree node page and call `pager_free()` on
it. The B-tree code already has `INDEX_META_PAGE` / `INDEX_LEAF_PAGE` page
tags; walk internal nodes recursively. `VACUUM` already rebuilds the entire
file, so this is mainly relevant for frequent DDL-heavy workloads.~~

Fixed: `btree_destroy()` now calls `pager_free()` on every node page after
recursively freeing its children. This applies to `DROP TABLE`, `TRUNCATE`,
and `ALTER TABLE DROP COLUMN` — all three call `btree_destroy()` before
freeing heap pages.

## 4. ~~No network encryption~~ (fixed July 2026)

The TCP server now supports optional TLS encryption via OpenSSL. Pass
`--tls-cert cert.pem --tls-key key.pem` to `nexdb serve` to enable it.

Build with `NEXDB_TLS=1 make` to link OpenSSL (auto-detected via `pkg-config`
or the Homebrew path `/opt/homebrew/opt/openssl@3`). Without the environment
variable the build remains TLS-free — no dependency.

## 5. ~~No session persistence~~ (fixed July 2026)

Sessions are now persisted to `<database>.sessions` — a binary file written
alongside the database file. The server saves on every session state change
(alloc, free, BEGIN, COMMIT, ROLLBACK, SAVEPOINT) and flushes again on
clean shutdown. On restart, sessions that have not expired (per
`--session-ttl`) are restored. Transaction state is NOT restored across
restarts — in-flight transactions must be re‑started. The session ID and
TTL tracking survive, so clients can reconnect with the same session token.

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
| Nested transaction depth | `MAX_SAVEPOINTS` | 16 |
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
