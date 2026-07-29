# nexdb

DISCLAIMER: DO NOT USE IN PRODUCTION ENVIRONMENTS, THIS IS AN EXPERIMENTAL PROJECT UNDER DEVELOPMENT.

A small experimental database engine that remembers what you actually use.

It stores ordinary tables and answers ordinary T-SQL queries, so you can keep
your day-to-day data in it. The difference is that every row quietly tracks how
often it gets used, and rows that keep showing up together become linked. Over
time the database develops an opinion about what matters to you — not because
you set a priority column, but as a side effect of how you used it.

Written from scratch in C11 with no dependencies: about 3,900 lines covering the
storage layer, the SQL parser, the executor and the memory model.

## Quick start

You need a C compiler and `make`. On macOS both come with the Xcode command line
tools — if `make` is not found, run `xcode-select --install` once. On Debian or
Ubuntu, `sudo apt install build-essential`.

```sh
cd ~/Documents/nexdb
make          # compiles build/nexdb, takes a couple of seconds
make test     # 137 integration tests, expect "137 passed, 0 failed"
make demo     # the five minute guided tour
make run      # interactive shell on a database called brain.ndb
```

Or drive the binary directly:

```sh
./build/nexdb mydata.ndb                       # interactive
./build/nexdb mydata.ndb -c "SELECT * FROM notes"   # one-off query
./build/nexdb mydata.ndb -f myscript.sql       # run a script
./build/nexdb -h                               # all the flags
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



## The two things that make it a memory



### 1. Rows get stronger with use and fade without it

Every row carries a `strength` and the time it was last touched. Reading a row
boosts its strength; time erodes it on an exponential curve — the same shape
psychologists use to model human forgetting.

```
strength now = stored strength × 2 ^ (−elapsed ÷ half-life)
on use:        stored strength = strength now + boost
```

The half-life is one week (`MEM_HALFLIFE_SECS` in `include/nexdb.h`). A row you look at daily climbs steadily. A row nobody has touched in two months sits near zero — still there, still queryable, just no longer something the database volunteers. **Nothing is ever deleted by decay.** Forgetting here means
"stops being suggested first", not "is gone".

You can see the memory state directly, and sort by it:

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

Be clear on what that last part does **not** do yet. A row still has to contain
one of your search words to be a candidate at all, so association affects the
*order* of results, never their membership. Asking about something linked to a
match will not surface that match's neighbours. See `LIMITATIONS.md` §4.

```sql
RECALL 'how do I make the coffee taste right';
RECALL 'shampoo' FROM notes TOP 5;
```

If nothing matches, it says so rather than returning its best guess dressed up
as an answer.

### Steering it by hand

```sql
REMEMBER FROM notes WHERE id = 4;   -- pin something important you rarely open
FORGET FROM notes WHERE id = 8;     -- zero the strength, keep the row
```



## SQL supported


| Area          | Supported                                                                                                                        |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| DDL           | `CREATE TABLE`, `DROP TABLE [IF EXISTS]`                                                                                         |
| Types         | `TINYINT`/`SMALLINT`/`INT`/`BIGINT`, `FLOAT`/`REAL`/`DECIMAL`, `NVARCHAR(n)`/`NVARCHAR(MAX)`/`VARCHAR`/`TEXT`, `BIT`, `DATETIME` |
| Constraints   | `NOT NULL`, `PRIMARY KEY`, `UNIQUE` — all enforced                                                                               |
| Queries       | `SELECT`, `TOP n`, column aliases with `AS`, `COUNT(*)`, `SUM`, `AVG`, `MIN`, `MAX`, `GROUP BY`, `WHERE`, `ORDER BY ... ASC/DESC` |
| Predicates    | `= <> != < <= > >=`, `AND`, `OR`, `NOT`, `LIKE` with `%`/`_` and `ESCAPE`, `IN (...)`, `IS [NOT] NULL`                           |
| Arithmetic    | `+ - * /`, and `+` concatenates when either side is text                                                                         |
| Writes        | `INSERT ... VALUES (...), (...)`, `UPDATE ... SET`, `DELETE`                                                                     |
| T-SQL surface | `GO` batches, `--` and `/* */` comments, `[bracketed]` and `"quoted"` identifiers, `N'literals'`, `''` escapes, `PRINT`          |
| Memory        | `RECALL`, `REMEMBER`, `FORGET`, `SHOW TABLES`, `SHOW MEMORY`, `SHOW LINKS`, `CHECKPOINT`                                         |


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

That last one matters more than it looks. Comparing a number against quoted text
used to render the number to a string and compare alphabetically, so on an `INT`
column `WHERE n > '10'` matched 9 and rejected 100. Numeric text now compares
numerically, and non-numeric text is an error.

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
include/nexdb.h   all types and interfaces
src/pager.c       4 KB pages, slotted heap pages, persisted catalog
src/memory.c      strength, decay, and the Hebbian association table
src/btree.c       B-tree index on primary keys
src/lexer.c       tokenizer
src/parser.c      recursive-descent parser producing an AST
src/func.c        scalar functions and UUID support
src/exec.c        executor, reinforcement, result formatting
src/select.c      GROUP BY and aggregation
src/main.c        shell and script runner
tests/            137 integration tests, run with `make test`
```

The whole database is one file. Page 0 is the header; the catalog and the
association table live in their own page chains; each table is a linked list of
slotted pages. Rows are variable-length, with the memory metadata as fixed-size
fields at the head of each row so reinforcement can be written in place without
rewriting the row.

Tests run the binary as a fresh process against a scratch database, so anything
passing has already survived a restart. The suite also runs clean under
AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wno-format-truncation \
    -Iinclude src/*.c -o /tmp/nexdb-asan -lm
BIN=/tmp/nexdb-asan sh tests/run_tests.sh
```



## What it does not do yet

This was verified by probing the binary,
with the silent-wrongness bugs listed first. The short version, in the order
they'd hurt:

- **No crash safety.** There's no write-ahead log, so a power cut mid-write can
corrupt the file. Only a clean exit calls `fsync`; `CHECKPOINT` writes pages
but leaves them in the OS cache. Keep backups.
- **No joins.** `GROUP BY` and aggregates (`SUM`, `AVG`, `MIN`, `MAX`) are supported; scalar functions like `LEN`, `UPPER`, `SUBSTRING`, `COALESCE`, `ABS`, `ROUND` and others work too.
- **Single user.** No locking; do not point two processes at one file.
- **Deleted pages are not reused.** Space is reclaimed within a page but a page
emptied entirely stays allocated, so heavy delete cycles grow the file.
- `RECALL` **needs a literal word match to get started.** It is substring and
familiarity based, not semantic — it will not connect "car" to "vehicle". Real
semantic recall would need embeddings, which is the biggest single upgrade
available.
- **No transactions**, so no `BEGIN`/`COMMIT`/`ROLLBACK`, and a script that fails
halfway leaves the earlier statements applied.
- **Rows must fit in one 4 KB page**, capping a row at roughly 4,000 bytes.
- Limits: 64 tables, 32 columns per table, 63-character names.



## Tuning the memory

All in `include/nexdb.h`, and worth playing with:


| Constant            | Default | Effect                                           |
| ------------------- | ------- | ------------------------------------------------ |
| `MEM_HALFLIFE_SECS` | 7 days  | how fast unused rows fade                        |
| `MEM_BOOST`         | 1.0     | how much a single access counts                  |
| `MEM_INIT_STRENGTH` | 1.0     | how strong a brand new row starts                |
| `MEM_MAX_STRENGTH`  | 1000    | saturation ceiling                               |
| `MEM_LINK_BOOST`    | 0.30    | how fast associations form                       |
| `MEM_COACT_MAX`     | 12      | how many rows a single query links together      |
| `MEM_SPREAD_FACTOR` | 0.45    | how much activation `RECALL` passes along a link |


Change one, run `make && make test`, and see what the behaviour feels like.
