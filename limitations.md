# Limitations

What is missing and what it would take to fix it.

~~## 1. No indexes — FIXED~~

~~Every query is a full table scan. Inserts, updates, uniqueness checks — all linear.~~

~~Fixed in `src/btree.c` and `src/exec.c`: each UNIQUE column gets its own B-tree index. Inserts,
updates, deletes, and ALTER TABLE all maintain indexes. `SELECT WHERE indexed_col = literal` uses
the index for O(log n) point lookups. Full scans still run for range queries and non-indexed
columns, and range scans are not yet indexed.~~

~~## 8. Uniqueness is quadratic — FIXED (by item 1)~~

~~With no indexes, `check_unique()` did a full table scan per constrained column per row.~~
~~The B-tree on the PK makes PRIMARY KEY checks O(log n).~~

## 1. No crash safety

There is no write-ahead log. A power cut mid-write can corrupt the file. Only `CHECKPOINT` and clean exit call `fsync`.

**To fix:** Add a write-ahead log in `src/pager.c`. Requires:
- A `wal.c` / `wal.h` module that records every page mutation before it is applied
- On startup, replay the WAL to restore a consistent state
- Page checksums to detect torn writes (4 KB pages are usually safe, but not guaranteed)
- Atomic page write via a double-write buffer or a shadow-paging scheme
- Flush the WAL to disk (`fsync`) before applying in-place writes
- On clean shutdown, checkpoint the WAL and remove it

## 2. No joins — FIXED

~~The parser does not recognise `JOIN` syntax and the executor only handles single-table queries.~~

~~**To fix:** Add join support across the stack:~~
- ~~**Lexer/parser** (`src/lexer.c`, `src/parser.c`): parse `JOIN`, `INNER JOIN`, `LEFT JOIN`, `ON` clause, and add join clauses to the `Stmt` struct~~
- ~~**Expression resolver** (`src/exec.c`): handle qualified column references (`t.col`)~~
- ~~**Executor** (`src/select.c`): implement nested-loop join (sufficient for prototype), then consider hash join~~
- ~~**Output** (`src/exec.c`): handle columns from multiple tables in result formatting~~

~~Fixed in `src/parser.c`, `src/exec.c`, `src/select.c`: `JOIN`, `INNER JOIN`, `LEFT JOIN` with `ON` clauses are parsed, qualified column refs (`t.col`) resolve across the joined tables, and nested-loop join execution produces correct INNER/LEFT results. `table.*` expansion and aggregate queries with joins are also supported. Multiple joins and GROUP BY with joins remain TODO.~~

## 3. Single user only — FIXED

~~No file locking exists. Two processes pointed at the same file will corrupt it.~~

~~**To fix:** In `db_open()` (`src/pager.c:285`):~~
~~- Add an advisory `flock(fd, LOCK_EX | LOCK_NB)` on open~~
~~- On conflict, print a clear error message and exit~~
~~- For a server-mode future, replace with a lock manager~~

~~Fixed in `src/pager.c`: `db_open()` now acquires an advisory exclusive lock via `flock()`.
A second process gets a clear error and exits. The lock is automatically released when
the file descriptor is closed. Only one nexdb process at a time can open a given database
file, which is sufficient for the single-user use case. A server-mode future would replace
this with a proper lock manager.

## 4. Deleted pages are not reused — FIXED

~~`pager_alloc()` always extends the file. Dropped tables and emptied pages stay allocated forever.~~

~~**To fix:** Add a free-page list in `src/pager.c`:~~
~~- A new page (or header field tracking the head of a free list) records freed pages~~
~~- On `pager_alloc()`, check the free list first before extending the file~~
~~- On `cat_drop()`, walk the table's page chain and return every page to the free list~~
~~- On `heap_delete()`, if a page becomes completely empty, unlink it from the chain and return it to the free list~~
~~- The free list itself should be persisted in the catalog or a dedicated page chain~~

~~Fixed in `src/pager.c`: a singly-linked free list (head pointer at offset 32 in page 0) recycles
pages. `pager_alloc()` pops from the free list before extending the file. `pager_free()` pushes
pages back. `cat_drop()` frees every heap page of the dropped table. `exec_truncate()` frees all
heap pages in O(n) instead of tombstoning rows individually. `heap_delete()` frees a page when
the last live row is removed. B-tree pages are not yet recycled (see `btree_destroy()`).

## 5. RECALL only does literal substring matching — FIXED

~~RECALL tokenises the query into alphanumeric terms and does case-insensitive substring matching against text columns. It cannot connect related concepts.~~

~~**To fix:** Replace the lexical scoring engine (`exec.c:646-671`). Options in order of effort:~~
- ~~**Easy:** Add fuzzy matching (Levenshtein distance) for typos and small variations~~
- ~~**Medium:** Add stemming (Porter stemmer) so "running" matches "run"~~
- ~~**Hard:** Add a TF-IDF inverted index to rank by relevance rather than hit count~~
- ~~**Very hard:** Add semantic embeddings (on-disk vector store + approximate nearest neighbour) for genuine concept-level recall~~

~~Fixed in `src/exec.c`: `lexical_score()` now falls back to Levenshtein-distance fuzzy matching when no exact substring match is found. The threshold is 0 edits for ≤4-char words, 1 edit for 5–7 char words, and 2 edits for 8+ char words. A fuzzy match contributes 0.8 to the lexical score vs 1.0 for an exact match, so exact hits still rank higher.~~

## 6. No transactions — FIXED

~~No `BEGIN`/`COMMIT`/`ROLLBACK`. Writes are applied in-place immediately. A script that fails mid-way leaves partial writes applied.~~

~~**To fix:** Add a transaction manager in a new `src/txn.c`:~~
- ~~`BEGIN` creates a savepoint in the WAL~~
- ~~Each write records old-page images in the WAL (undo log)~~
- ~~`ROLLBACK` replays the undo log to restore before-images~~
- ~~`COMMIT` fsyncs the WAL then marks the transaction durable~~
- ~~At minimum, wrap each individual statement in an implicit single-statement transaction for crash atomicity~~

~~Fixed in `src/pager.c` and `src/exec.c`: `BEGIN` marks the transaction active; each call to `pager_write()` captures the old page image into an in-memory `UndoEntry` array before writing; `ROLLBACK` restores images in reverse order; `COMMIT` fsyncs, serialises catalog/links, then clears the undo log. A statement failure inside an active transaction auto-rolls back. Nested transactions are not supported. Maximum 256 modified pages per transaction.~~

## 7. Rows are capped at ~4 KB — FIXED

~~A row must fit entirely within one 4,096-byte page (max ~4,080 bytes of payload after headers).~~

~~**To fix:** Add row chaining or overflow pages in `src/pager.c`:~~
- ~~**Overflow pages:** When a row exceeds `PAGE_SIZE`, store a stub in the main page pointing to one or more overflow (chained) pages holding the rest of the data~~
- ~~**Row chaining:** Allow a row's column data to span multiple heap pages in the table's chain~~
- ~~Also bump `MAX_TOKEN` in the lexer (`include/nexdb.h`) above 4096 since large text values need matching parser/lexer capacity~~

~~Fixed in `src/pager.c`: When a serialised row exceeds the inline limit (`PAGE_SIZE - HP_HDR_SIZE - SLOT_SIZE ≈ 4080 bytes`), the row is written as an overflow chain. A 30-byte stub (tuple header + `ncols = 0xFFFF` marker + chain head page number) is stored in the heap page slot, and the full encoded tuple is stored across one or more chain pages (each 4088 bytes of payload). On read, `row_decode()` detects the `OVERFLOW_MARKER` and reads the chain via `chain_read()` before decoding. On delete, `chain_free()` walks and frees every overflow page. `heap_free_pages()` (used by `cat_drop`/`TRUNCATE`) also frees overflow chains. `heap_replace()` works via `heap_delete` + `heap_insert`, both overflow-aware. Max row size is 64 KB (`MAX_ROW_SIZE`). Version bumped to 6. The lexer's `MAX_TOKEN` (4095 chars) is unchanged — very large string literals still need programmatic insertion via `-c` or scripting, or multiple `INSERT`/`UPDATE` steps.~~

## 8. Hard limits — FIXED

~~| Limit | Constant | Value |~~
~~|---|---|---|~~
~~| Tables per database | `MAX_TABLES` | 64 |~~
~~| Columns per table | `MAX_COLS` | 32 |~~
~~| Name length | `MAX_NAME` | 63 chars (+ null) |~~
~~| INSERT rows per statement | (hardcoded) | 64 |~~
~~| GROUP BY keys | `MAX_GROUP_KEYS` | 8 |~~
~~| ORDER BY keys | `MAX_ORDER_KEYS` | 8 |~~
~~| Aggregate functions per query | `MAX_AGGS` | 16 |~~
~~| SELECT list items | `MAX_SELECT_ITEMS` | 64 |~~
~~| IN list / CASE arms | `MAX_IN_ITEMS` | 64 |~~
~~| Function arguments | `MAX_FUNC_ARGS` | 8 |~~
~~| RECALL search terms | `MAX_RECALL_TERMS` | 64 |~~

~~**To fix:** All are single-constant bumps in `include/nexdb.h` — but some have structural implications:~~
~~- `MAX_TABLES` / `MAX_COLS` / `MAX_NAME`: Bump the constant, but very wide tables may hit the row-size cap (item 7) faster~~
~~- `MAX_INSERT_ROWS` (hardcoded 64): Bump the `rows` array in the `Stmt` union (`include/nexdb.h`)~~
~~- `MAX_AGGS` / `MAX_GROUP_KEYS` / `MAX_ORDER_KEYS`: Simple constant bump~~
~~- `MAX_FUNC_ARGS` (currently 8): Bump to 16 or remove the limit entirely~~

~~All bumped in `include/nexdb.h`. `MAX_AGGS` moved from `select.c` to the header. The Stmt struct now uses the `MAX_INSERT_ROWS` constant instead of hardcoded `64`.~~

New limits:

| Limit | Constant | Old | New |
|---|---|---|---|
| Tables per database | `MAX_TABLES` | 64 | 128 |
| Columns per table | `MAX_COLS` | 32 | 32 (unchanged) |
| Name length | `MAX_NAME` | 63 | 127 |
| Indexes per table | `MAX_INDEXES` | 16 | 32 |
| INSERT rows per statement | `MAX_INSERT_ROWS` | 64 | 128 |
| GROUP BY keys | `MAX_GROUP_KEYS` | 8 | 16 |
| ORDER BY keys | `MAX_ORDER_KEYS` | 8 | 16 |
| Joins per query | `MAX_JOINS` | 8 | 16 |
| Aggregate functions | `MAX_AGGS` | 16 | 32 |
| SELECT list items | `MAX_SELECT_ITEMS` | 64 | 128 |
| IN list / CASE arms | `MAX_IN_ITEMS` | 64 | 128 |
| SET clauses (UPDATE) | `MAX_SET_ITEMS` | 32 | 64 |
| Function arguments | `MAX_FUNC_ARGS` | 8 | 16 |
| RECALL search terms | `MAX_RECALL_TERMS` | 64 | 128 |
| Output columns (result) | `MAX_OUT_COLS` | 40 | 64 |

## 9. Other gaps

These are not in the README's "not yet" list but are missing:

~~### No secondary indexes (FIXED — see item 1 above)~~
~~The B-tree only covers the PK column. UNIQUE constraints on other columns still do full scans.~~

~~### No index usage in SELECT (range scans) (FIXED — see item 1 above)~~
~~The index is used by `check_unique()` but SELECT queries do not use it yet. `WHERE pk = ?` still does a full scan.~~

~~### No `ALTER TABLE`~~
~~There is an `ALTER TABLE ... ADD COLUMN` in the parser (`parser.c`) and executor (`exec.c`), but no `DROP COLUMN`, `ALTER COLUMN TYPE`, or `RENAME COLUMN`.~~

~~FIXED: ADD COLUMN, DROP COLUMN, RENAME COLUMN, and ALTER COLUMN TYPE are all implemented.~~

~~### No `DISTINCT` in aggregates~~
~~`COUNT(DISTINCT x)` is not parsed.~~

~~FIXED: `Expr.agg_distinct` flag, `AggAcc.distinct`/`seen`/`nseen`/`cseen` fields for tracking seen values.~~

### No subqueries
`SELECT * FROM (SELECT ...)` and `WHERE x IN (SELECT ...)` are not supported. The parser would need to handle subselects as expressions, and the executor would need to execute sub-plans.

FIXED: Scalar subqueries `(SELECT x FROM t)`, `IN (SELECT ...)`, `NOT IN (SELECT ...)`, `EXISTS (SELECT ...)`, and `NOT EXISTS (SELECT ...)` are all implemented and evaluated at runtime. FROM-clause subqueries (derived tables) and correlated subqueries (inner query referencing outer columns) remain unimplemented.

~~### No `HAVING` without `GROUP BY`~~
~~`HAVING` is parsed but there may still be uncovered edge cases (the pre-existing test failures suggest at least one).~~

~~FIXED: HAVING without GROUP BY now works (treats entire result as one group).~~

~~### No `EXPLAIN`~~
~~No query plan introspection.~~

~~FIXED: EXPLAIN parses any statement and displays a query plan (table scan, joins, WHERE filter, GROUP/ORDER BY, TOP, aggregates).~~

~~### No `VACUUM` / file compaction~~
~~Even with a free list, the file never shrinks. A `VACUUM` command that rewrites live rows into a new file then swaps is the standard approach.~~

~~FIXED: `exec_vacuum()` rewrites into a temp file, swaps atomically via `rename()`, reopens.
