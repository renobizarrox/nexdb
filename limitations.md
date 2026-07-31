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

**Status: fixed.** Since July 2026 the server uses a reader-writer lock
instead of a plain mutex. Read-only statements (`SELECT`, `RECALL`, `SHOW`,
`PRINT`, `EXPLAIN`) run concurrently under the read side of the lock; write
statements and whole transactions (BEGIN..COMMIT) take the write side.

Two details make this safe:

- **Deferred reinforcement.** A plain `SELECT` normally writes strength /
  access metadata. While a read statement runs, `mem_touch()` and
  `mem_associate()` only record what happened (per-thread pending buffers);
  after the statement the server re-acquires the write lock and applies the
  touches via `mem_flush_pending()`. Row identity is verified before a
  deferred touch is applied, so a row deleted while the reader was waiting
  is not reinforced.
- **Thread-local executor state.** Every mutable global in the execution
  path (`g_output_file`, `g_select_capture`, `g_reinforce`, aggregate/join/
  correlation/sort contexts, parser star-column state) is now
  `__thread`. Statement text output is captured through a per-thread
  `tmpfile()` instead of a process-wide `dup2()` of stdout.

Transactions are still fully serialised (SQLite-style write lock held from
BEGIN until COMMIT/ROLLBACK), and writes are excluded while readers run. All
engine code remains single-threaded per statement; only the server layer
coordinates the lock. Shutdown signals are handled by a dedicated `sigwait()`
thread plus a self-pipe that wakes the accept loop, avoiding macOS's
unreliable signal routing to multi-threaded processes.

## 7. Single process — FIXED (PostgreSQL-style daemon routing)

~~`db_open()` acquires an advisory exclusive lock via `flock()`. A second
process gets a clear error. Only one `nexdb` process at a time can open a
given database file. The server accepts up to 64 concurrent clients, but they
all share the same process.~~

~~**To fix:** Replace `flock()` with a listening daemon that proxies requests
to worker processes, or use POSIX shared memory + semaphores for
multi-process concurrency. This is a major architectural change.~~

Fixed (July 2026), PostgreSQL-style: the server is the only process that ever
opens the data file; every other `nexdb` process is a client.

- `nexdb serve <db>` now always creates a per-database unix socket at
  `<db>.sock` (derived deterministically from the database path), in addition
  to any `--unix PATH`.
- When the CLI finds the file locked by another process (the `flock()`
  failure in `db_open()`), it automatically connects to `<db>.sock` and
  routes the request through the running server:
  - `-c "<sql>"` and `-f <script>` are sent as one batch via
    `server_proxy_exec()` and print the same output as a local run;
  - the interactive prompt becomes a remote REPL over the socket (shell
    commands like `.read` and `.tables` are not available in that mode);
  - `--token STR` authenticates when the server runs with a token;
  - `-r` (observer mode) is forwarded per request (`"observe":"0"`), so a
    routed query still does not reinforce memory.
- `flock()` remains in place as the ownership test — it never needs to be
  held across processes, only to detect "someone else owns this file".
- Remaining gaps: two `nexdb serve` invocations on the same database still
  conflict (the second exits with the flock error, like PostgreSQL's
  "already in use"); the interactive proxy REPL does not support shell
  commands; clients must use the same database path string as the server for
  auto-routing to find the socket (a different relative/absolute spelling of
  the same file will not match `<db>.sock`).

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

- **`GROUP BY` with `JOIN`** and **multiple `JOIN`s in one query** are refused
  at execution; the nested-loop executor handles exactly one join at a time.
  `WHERE` on either side of the join works, including with aggregates.
- **`ALTER COLUMN TYPE`** can narrow a type only when every existing value
  fits the new type; widening always works.
- **`FOREIGN KEY`** supports `ON DELETE NO ACTION` (default) and `ON DELETE
  CASCADE`; `ON UPDATE` is not supported, and cascades do not recurse through
  grandchildren.
- **Views** expand to their stored `SELECT` at query time; they cannot be used
  as a `JOIN` operand, and outer queries over a view (or any derived table)
  cannot use aggregates or `GROUP BY`.
- **No `TRIGGER`** — event-driven logic is not supported.
- **Stored procedures** hold exactly one statement (any statement, including
  DDL and other `CALL`s). There are no parameters, variables, or control-flow
  keywords; `BEGIN ... END` bodies are not supported — statement separators
  end the procedure body, so multi-statement bodies are impossible by
  construction. Procedure and table/view names share one namespace.
- **Full-text search** is limited to the RECALL engine; no inverted index or
  tokenizer with stop-word removal exists.
- **`VACUUM`** requires free disk space equal to the current database size
  (it writes a temp file, then atomically replaces the original).
