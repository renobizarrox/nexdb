# nexdb

DISCLAIMER: DO NOT USE IN PRODUCTION ENVIRONMENTS, THIS IS AN EXPERIMENTAL PROJECT UNDER DEVELOPMENT.

A small experimental database engine that remembers what you actually use.

It stores ordinary tables and answers ordinary T-SQL queries, so you can keep
your day-to-day data in it. The difference is that every row quietly tracks how
often it gets used, and rows that keep showing up together become linked. Over
time the database develops an opinion about what matters to you — not because
you set a priority column, but as a side effect of how you used it.

Written from scratch in C11 with no dependencies: about 15,000 lines covering
the storage layer, the SQL parser, the executor, the associative memory model,
a write-ahead log, a JSON-over-TCP server, and B-tree plus GIN (full-text)
indexes.

## Quick start

You need a C compiler and `make`. On macOS both come with the Xcode command line
tools — if `make` is not found, run `xcode-select --install` once. On Debian or
Ubuntu, `sudo apt install build-essential`.

```sh
cd ~/Documents/nexdb
make          # compiles build/nexdb, takes a couple of seconds
make test     # 358 integration tests, expect "358 passed, 2 failed"
make demo     # the five minute guided tour
make run      # interactive shell on a database called brain.ndb
```

Or drive the binary directly:

```sh
./build/nexdb mydata.ndb                            # interactive shell
./build/nexdb mydata.ndb -c "SELECT * FROM notes"   # one-off query
./build/nexdb mydata.ndb -f myscript.sql            # run a script
./build/nexdb -h                                    # all the flags
```

The database is a single file that you name yourself, created on first use.
`make clean` deletes the build directory and any `.ndb` files in the project.

In the shell, end a statement with `;` or type `GO` on its own line to run it.
`.help` lists the shell commands, `.exit` leaves cleanly.

```
nexdb> CREATE TABLE notes (id INT PRIMARY KEY, topic NVARCHAR(50), body NVARCHAR(400));
table 'notes' created with 3 columns

nexdb> INSERT INTO notes (id, topic, body) VALUES (1, 'coffee', 'grind finer for the aeropress');
(1 row inserted)

nexdb> SELECT * FROM notes WHERE topic = 'coffee';
```

## Server mode

nexdb also runs as a JSON-over-TCP daemon for local network access:

```sh
./build/nexdb serve mydata.ndb                                  # port 7890
./build/nexdb serve mydata.ndb --port 9000                      # custom port
./build/nexdb serve mydata.ndb --unix /tmp/nexdb.sock           # Unix socket
./build/nexdb serve mydata.ndb --token s3cret                   # auth token
./build/nexdb serve mydata.ndb --daemon --pidfile /tmp/nexdb.pid # background
```

The wire protocol is newline-delimited JSON:

```
→ {"sql":"SELECT 1 AS n","session":"<uuid>","token":"<token>"}
← {"ok":true,"text":"n\n-\n1\n\n(1 row)\n","columns":["n"],"rows":[["1"]],
   "session":"<uuid>"}
```

The `session` field is optional — the server generates one and returns it in the
welcome message. Include it on subsequent requests to keep transaction state.
The `token` field is only needed when `--token` is set.

Connect from any language:

```python
import socket, json
s = socket.socket()
s.connect(('127.0.0.1', 7890))
welcome = json.loads(s.recv(4096))
sid = welcome['session']
s.sendall((json.dumps({'sql':'SELECT 1','session':sid}) + '\n').encode())
resp = json.loads(s.recv(65536))
```

Or use the built-in REPL:

```sh
./build/nexdb --connect localhost:7890
```

**Single-process rule (PostgreSQL-style):** only the server ever opens the
data file. The CLI detects the file is already open (via the `flock()`
ownership test in `db_open()`) and automatically routes `-c`, `-f` and the
interactive prompt through the server's per-database unix socket `<db>.sock`,
so a shell and a daemon can use the same database at the same time:

```sh
./build/nexdb serve mydata.ndb --daemon
./build/nexdb mydata.ndb -c "INSERT INTO notes (id, topic, body) VALUES (2, 'espresso', '18g in, 36g out')"
./build/nexdb mydata.ndb -f setup.sql
./build/nexdb mydata.ndb --token s3cret -r -c "RECALL 'coffee'"
```

`--token` authenticates routed requests; `-r` is forwarded per request so
observer mode works through the socket too. Shell commands (`.read`,
`.tables`, ...) are only available in a local interactive session — the
routed prompt is a plain SQL REPL.

**Features:**
- Up to 64 concurrent clients with per-connection handler threads
- Reader-writer locking: read-only statements (`SELECT`, `RECALL`, `SHOW`,
  `PRINT`, `EXPLAIN`) run in parallel; writes and whole transactions are
  serialised (memory reinforcement is deferred and applied under a write
  lock, so parallel reads never write pages)
- Structured `columns` / `rows` in the JSON response alongside `text`
- Session-managed transaction state (`BEGIN`/`COMMIT`/`ROLLBACK`)
- Optional auth token
- Session TTL (default 300 s, configurable via `--session-ttl`)
- Graceful shutdown on `SIGINT` / `SIGTERM` / `SIGQUIT` (dedicated `sigwait`
  thread + self-pipe, reliable even under load)
- Daemonisation with `--daemon` and PID file with `--pidfile`
- Unix domain socket with `--unix`, plus an automatic per-database socket at
  `<db>.sock` that the CLI uses to route around the single-process limit

## The two things that make it a memory

### 1. Rows get stronger with use and fade without it

Every row carries a `strength` and the time it was last touched. Reading a row
boosts its strength; time erodes it on an exponential curve — the same shape
psychologists use to model human forgetting.

```
strength now = stored strength × 2 ^(−elapsed ÷ half-life)
on use:        stored strength = strength now + boost
```

The half-life is one week (`MEM_HALFLIFE_SECS` in `include/nexdb.h`). A row you
look at daily climbs steadily. A row nobody has touched in two months sits near
zero — still there, still queryable, just no longer something the database
volunteers. **Nothing is ever deleted by decay.** Forgetting here means "stops
being suggested first", not "is gone".

You can see the memory state directly:

```sql
SHOW MEMORY FROM notes;
SELECT id, topic, _strength, _access FROM notes ORDER BY _strength DESC;
```

`_rid`, `_strength`, `_access` and `_last_access` are pseudo-columns. They work
anywhere a column works — select list, `WHERE`, `ORDER BY` — but you cannot
assign to them, because the memory layer owns them.

### 2. Rows used together become associated

When a statement returns several rows, those rows get pairwise links reinforced.
This is Hebbian learning, the "cells that fire together, wire together" rule.
Query your three coffee notes together a few times and the engine learns they
belong together, without being told that `topic` is meaningful.

```sql
SHOW LINKS TOP 10;
```

`RECALL` takes a rough phrase instead of an exact predicate, scores rows on word
matches, adds credit for familiarity, then uses the association graph to
re-rank: a row linked to a strong match is pushed up the results.

Fuzzy matching via Levenshtein edit distance handles typos: "shampoo" matches
shampoo, and "cafe" matches coffee. A row must still contain one of your search
words to be a candidate — association affects the *order* of results, never
their membership.

```sql
RECALL 'how do I make the coffee taste right';
RECALL 'shampoo' FROM notes TOP 5;
```

If nothing matches, the engine says so rather than returning its best guess
dressed up as an answer.

### Steering it by hand

```sql
REMEMBER FROM notes WHERE id = 4;   -- pin something important you rarely open
FORGET FROM notes WHERE id = 8;     -- zero the strength, keep the row and its links
```

## SQL supported

| Area | Supported |
| --- | --- |
| DDL | `CREATE TABLE`, `DROP TABLE [IF EXISTS]`, `TRUNCATE TABLE`, `ALTER TABLE` (ADD/DROP/RENAME/ALTER COLUMN TYPE) |
| Types | `TINYINT`/`SMALLINT`/`INT`/`BIGINT`, `FLOAT`/`REAL`/`DECIMAL`/`MONEY`, `NVARCHAR(n)`/`VARCHAR`/`TEXT`, `BIT`, `DATETIME`, `UNIQUEIDENTIFIER` |
| Constraints | `NOT NULL`/`NULL`, `PRIMARY KEY`, `UNIQUE`, `DEFAULT` (literal, `GETDATE()`, `NEWID()`), `IDENTITY(seed,step)`, `CHECK (expr)`, `FOREIGN KEY` / `REFERENCES ... ON DELETE CASCADE` — all enforced |
| Queries | `SELECT [ALL\|DISTINCT] [TOP n]` with expressions, column aliases, `FROM` (table, subquery, joins), `WHERE`, `GROUP BY` + `HAVING`, `ORDER BY`, `LIMIT n [OFFSET m]` |
| Set ops | `UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT`, with trailing `ORDER BY` / `LIMIT` / `OFFSET` on the combined result |
| Views | `CREATE VIEW name AS SELECT ...`, `DROP VIEW [IF EXISTS]` — stored, expanded at query time, persistent |
| Stored procedures | `CREATE PROCEDURE name [(@a TYPE [= default] [OUTPUT], ...)] AS <statement | BEGIN ... END>`, `CALL name(args)`, `EXEC [@rc =] name [args]`, `EXEC ('...')` dynamic SQL, `DROP PROCEDURE [IF EXISTS]` — stored, persistent, may nest 128 deep (bodies parsed once and cached); parameters (named args, defaults, `OUTPUT` write-back, `RETURN` codes via `EXEC @rc =`), `DECLARE`/`SET` variables with block scoping, `IF`/`ELSE`, `WHILE` with `BREAK`/`CONTINUE`, `RETURN`, `PRINT <expr>`, `BEGIN TRY ... END TRY BEGIN CATCH ... END CATCH` (`@@error_message`), `INSERT INTO t EXEC proc`, and cursors (`DECLARE ... CURSOR FOR` / `OPEN` / `FETCH NEXT FROM ... INTO` / `@@fetch_status` / `CLOSE` / `DEALLOCATE`) |
| Joins | `INNER JOIN`, `LEFT JOIN`, `table.*` expansion, qualified `table.col` references |
| Predicates | `= <> != < <= > >=`, `AND`/`OR`/`NOT`, `LIKE` with `%`/`_` and `ESCAPE`, `IN (...)`, `IS [NOT] NULL`, `BETWEEN`/`NOT BETWEEN` |
| Expressions | Arithmetic `+ - * / %`, `CASE` (simple and searched), `CAST`, scalar functions, subqueries (scalar, `IN`, `EXISTS`, `ANY`/`ALL`, correlated) |
| Aggregates | `COUNT(*)`, `COUNT(DISTINCT x)`, `SUM`, `AVG`, `MIN`, `MAX` |
| Scalar functions | `LEN`, `UPPER`, `LOWER`, `SUBSTRING`, `LEFT`, `RIGHT`, `REPLACE`, `CONCAT`, `REVERSE`, `TRIM`, `LTRIM`, `RTRIM`, `ABS`, `ROUND`, `FLOOR`, `CEILING`, `SIGN`, `SQRT`, `POWER`, `ISNULL`, `COALESCE`, `NULLIF`, `IIF`, `GETDATE`, `NEWID` |
| Writes | `INSERT ... VALUES (...), (...)`, `INSERT ... SELECT`, `UPDATE ... SET`, `DELETE` |
| T-SQL surface | `GO` batches, `--` and `/* */` comments, `[bracketed]` and `"quoted"` identifiers, `N'literals'`, `''` escapes, `PRINT` |
| Memory | `RECALL`, `REMEMBER`, `FORGET`, `SHOW TABLES`, `SHOW MEMORY`, `SHOW LINKS` |
| Utility | `BEGIN`/`COMMIT`/`ROLLBACK`, `CHECKPOINT`, `EXPLAIN`, `VACUUM` |
| Indexing | B-tree indexes on `PRIMARY KEY` and `UNIQUE` columns (O(log n) point lookups); GIN inverted full-text indexes for `@@` |
| Full-text search | `to_tsvector(text)`, `to_tsquery(...)` / `plainto_tsquery(...)` (with `&`/`\|`/`!`), the `@@` operator, `ts_rank`, `CREATE INDEX ... USING GIN`, `DROP INDEX [IF EXISTS]` |

Everything is case-insensitive, including string comparison.

### Types are enforced

Declared types are real constraints, not documentation. Each of these is refused
with an explanation rather than quietly mangled:

```sql
INSERT INTO t (s)  VALUES ('far too long');  -- s is NVARCHAR(5)
INSERT INTO t (id) VALUES (5000000000);      -- id is INT: out of range
INSERT INTO t (b)  VALUES ('yes');           -- b is BIT: expected 0, 1, true, false
INSERT INTO t (d)  VALUES ('2026-02-30');    -- d is DATETIME: no such day
INSERT INTO t (id) VALUES (1);               -- id is a PRIMARY KEY that exists
SELECT * FROM t WHERE id < 'banana';         -- no meaningful ordering
```

Numeric text compares numerically. Non-numeric text compared against a number
is an error.

### Full-text search

An index built with `USING GIN` maintains an inverted index of lexemes over a
text column: values are lowercased, split on punctuation, and common English
stop words are dropped. Query it with the `@@` operator (`tsvector @@ tsquery`):

```sql
CREATE INDEX notes_fts ON notes (body) USING GIN;

SELECT id FROM notes WHERE to_tsvector(body) @@ to_tsquery('coffee & grind');
SELECT id FROM notes WHERE to_tsvector(body) @@ to_tsquery('extract | brew');
SELECT id FROM notes WHERE to_tsvector(body) @@ to_tsquery('!decaf');
SELECT id FROM notes WHERE to_tsvector(body) @@ plainto_tsquery('grind finer');

SELECT id, ts_rank(to_tsvector(body), to_tsquery('coffee')) AS r
FROM notes
WHERE to_tsvector(body) @@ to_tsquery('coffee')
ORDER BY r DESC;

DROP INDEX notes_fts;
```

`to_tsquery` accepts `&` (and), `|` (or) and `!` (not); `plainto_tsquery`
treats its argument as an AND of its words, so the example above behaves like
`grind & finer`. `ts_rank` scores each match (0.0–1.0) and can be used in
`ORDER BY` to push the strongest matches to the top. The GIN index is
lossy-safe: the posting list produces candidate rows and the full predicate is
re-applied on top, so results are always exact even when several rows share a
lexeme. A query made entirely of stop words matches nothing.

## Watching a month pass in a second

Decay is slow by design, which makes it awkward to see. Two flags help:

```sh
# jump the clock forward four weeks
NEXDB_TIME_OFFSET=$((28*86400)) ./build/nexdb demo.ndb -r \
    -c "SELECT id, _strength FROM notes ORDER BY _strength DESC"
```

`-r` is observer mode: queries do not reinforce what they read. Without it,
the very act of checking a row's strength raises it, and you end up measuring
your own interference. It is also the right flag for dashboards and backups.

## How it works inside

```
include/nexdb.h   all types and interfaces (783 lines)
include/server.h  server public API
include/wal.h     WAL page types

src/pager.c       paged file, slotted heap pages, free list, catalog (1319 lines)
src/memory.c      strength decay, Hebbian association table (417 lines)
src/btree.c       B-tree and GIN index, bottom-up splits (850 lines)
src/fulltext.c    tokenizer, stop words, tsvector/tsquery/ts_rank, @@ (625 lines)
src/lexer.c       tokenizer (238 lines)
src/parser.c      recursive-descent parser → AST (2547 lines)
src/exec.c        executor, reinforcement, result formatting (4191 lines)
src/select.c      GROUP BY, aggregation, capture for server (1952 lines)
src/func.c        scalar functions, UUID support (505 lines)
src/wal.c         write-ahead log for crash safety (234 lines)
src/server.c      JSON-over-TCP server, sessions, auth, proxy client (1220 lines)
src/main.c        shell, script runner, CLI, auto-routing (501 lines)
src/value.c       value API, type conversion (322 lines)
tests/            358 integration tests, run with `make test`
```

The whole database is one file. Page 0 is the header; the catalog and the
association table live in their own page chains; each table is a linked list of
slotted pages. Rows are variable-length, with memory metadata at the head of
each row so reinforcement can be written in place.

A write-ahead log (`<db>.wal`) records every page mutation before it is applied.
On startup, `db_open()` replays any pending entries. `CHECKPOINT` fsyncs the
main file and truncates the WAL. This ensures crash safety.

Tests run the binary as a fresh process against a scratch database, so anything
passing has already survived a restart. The suite also runs clean under
AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make asan
```

## What it does not do yet

See `limitations.md` for the full list. The short version:

- **No range scans via indexes.** `WHERE pk = literal` uses the B-tree (and
  `@@` full-text predicates use the GIN index), but range queries still do a
  full table scan.
- **TLS is now built-in** (since July 2026). Pass `--tls-cert cert.pem --tls-key key.pem`
  to `serve` to enable OpenSSL encryption on TCP connections. The default is still
  cleartext for local development. Build with `NEXDB_TLS=1 make` to link OpenSSL.
- **Writes are serialised.** Read-only statements run in parallel under the
  reader-writer lock, but writes and whole transactions take the write side
  (SQLite-style), so write throughput is single-threaded per database.
- **Session persistence.** Sessions are saved to `<database>.sessions` on every
  state change and restored on restart. In-flight transactions are not carried
  across restarts (the WAL protects committed data).
- **Single process.** `flock()` means only one process can open the file — the
  CLI routes around it by proxying through the running server's `<db>.sock`.
- **Rows must fit in 64 KB** (`MAX_ROW_SIZE`), enforced by overflow page chains.
- **Limits:** 128 tables, 32 columns per table, 127-char names, 16 JOINs per
  query, 16 ORDER BY keys, 16 GROUP BY keys, 32 aggregate functions.

## Tuning the memory

All in `include/nexdb.h`:

| Constant | Default | Effect |
| --- | --- | --- |
| `MEM_HALFLIFE_SECS` | 7 days | how fast unused rows fade |
| `MEM_BOOST` | 1.0 | how much a single access counts |
| `MEM_INIT_STRENGTH` | 1.0 | how strong a brand new row starts |
| `MEM_MAX_STRENGTH` | 1000 | saturation ceiling |
| `MEM_LINK_BOOST` | 0.30 | how fast associations form |
| `MEM_COACT_MAX` | 12 | how many rows a single query links together |
| `MEM_SPREAD_FACTOR` | 0.45 | how much activation `RECALL` passes along a link |

Change one, run `make && make test`, and see what the behaviour feels like.
