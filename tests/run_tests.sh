#!/bin/sh
# Integration tests for nexdb. Run with:  make test
#
# Each test runs the binary as a fresh process against a scratch database, so
# anything that passes here also survives a restart.

set -u

BIN="${BIN:-./build/nexdb}"
DB="$(mktemp -u /tmp/nexdb-test-XXXXXX).ndb"
PASS=0
FAIL=0

cleanup() { rm -f "$DB"; }
trap cleanup EXIT

run() { "$BIN" "$DB" -c "$1" 2>&1; }

# check <description> <expected-substring> <actual-output>
check() {
    if printf '%s' "$3" | grep -qF -- "$2"; then
        PASS=$((PASS + 1))
        printf '  ok    %s\n' "$1"
    else
        FAIL=$((FAIL + 1))
        printf '  FAIL  %s\n' "$1"
        printf '        expected to find: %s\n' "$2"
        printf '        got: %s\n' "$(printf '%s' "$3" | head -6 | tr '\n' '/')"
    fi
}

# check_not <description> <forbidden-substring> <actual-output>
check_not() {
    if printf '%s' "$3" | grep -qF -- "$2"; then
        FAIL=$((FAIL + 1))
        printf '  FAIL  %s\n' "$1"
        printf '        did not expect: %s\n' "$2"
    else
        PASS=$((PASS + 1))
        printf '  ok    %s\n' "$1"
    fi
}

echo "nexdb test suite"
echo

echo "-- schema and basic dml"
out=$(run "CREATE TABLE notes (id INT PRIMARY KEY, topic NVARCHAR(50), body NVARCHAR(400), score FLOAT, done BIT)")
check "create table" "created with 5 columns" "$out"

out=$(run "CREATE TABLE notes (id INT)")
check "duplicate table rejected" "already exists" "$out"

out=$(run "INSERT INTO notes (id, topic, body, score, done) VALUES
   (1,'coffee','the good beans come from the shop on 5th', 4.5, 1),
   (2,'coffee','grind finer for the aeropress', 3.0, 0),
   (3,'taxes','quarterly estimate due april 15', 9.5, 0),
   (4,'dogs','vet appointment for buddy in march', 2.0, 1),
   (5,'dogs','buddy hates the blue shampoo', 1.0, 0)")
check "multi-row insert" "(5 rows inserted)" "$out"

out=$(run "SELECT COUNT(*) FROM notes")
check "count all" "5" "$out"

out=$(run "SELECT id FROM notes WHERE topic = 'coffee'")
check "where equality" "(2 rows)" "$out"

out=$(run "SELECT id FROM notes WHERE body LIKE '%aeropress%'")
check "like wildcard" "(1 row)" "$out"

out=$(run "SELECT id FROM notes WHERE id IN (1, 3, 99)")
check "in list" "(2 rows)" "$out"

# ids 2 (3.0) and 3 (9.5) have score > 2 and done = 0
out=$(run "SELECT id FROM notes WHERE score > 2 AND done = 0")
check "and with comparison" "(2 rows)" "$out"

out=$(run "SELECT id FROM notes WHERE NOT topic = 'dogs'")
check "not" "(3 rows)" "$out"

out=$(run "SELECT TOP 2 id FROM notes ORDER BY score DESC")
check "top plus order by desc" "(2 rows)" "$out"

out=$(run "SELECT id, topic AS subject FROM notes WHERE id = 1")
check "column alias" "subject" "$out"

out=$(run "SELECT nosuchcol FROM notes")
check "unknown column is an error" "no column 'nosuchcol'" "$out"

out=$(run "SELECT * FROM nosuchtable")
check "unknown table is an error" "unknown table" "$out"

out=$(run "SELECT * FROM notes WHERE")
check "truncated sql is an error" "error:" "$out"

echo
echo "-- nulls"
out=$(run "INSERT INTO notes (id, topic) VALUES (6, 'sparse')")
check "partial insert leaves nulls" "(1 row inserted)" "$out"
out=$(run "SELECT id FROM notes WHERE body IS NULL")
check "is null" "(1 row)" "$out"
out=$(run "SELECT id FROM notes WHERE body IS NOT NULL")
check "is not null" "(5 rows)" "$out"
out=$(run "SELECT id FROM notes WHERE score > 0")
check "null excluded from comparisons" "(5 rows)" "$out"
out=$(run "INSERT INTO notes (topic) VALUES ('nope')")
check "not null constraint enforced" "does not accept NULL" "$out"

echo
echo "-- update and delete"
out=$(run "UPDATE notes SET body = 'buy oat milk instead', score = score + 1 WHERE id = 2")
check "update with arithmetic" "(1 row updated)" "$out"
out=$(run "SELECT body, score FROM notes WHERE id = 2")
check "update persisted" "buy oat milk instead" "$out"
check "arithmetic applied" "4" "$out"
out=$(run "UPDATE notes SET _strength = 5 WHERE id = 2")
check "memory columns are not assignable" "cannot be assigned" "$out"
out=$(run "DELETE FROM notes WHERE id = 6")
check "delete" "(1 row deleted)" "$out"
out=$(run "SELECT COUNT(*) FROM notes")
check "count after delete" "5" "$out"

echo
echo "-- t-sql surface details"
out=$(run "-- a comment
/* another
   comment */
SELECT [id] FROM [notes] WHERE topic = N'coffee'; ")
check "comments, brackets, N-literals" "(2 rows)" "$out"
out=$(run "PRINT 'hello'
GO
SELECT COUNT(*) FROM notes
GO")
check "print and GO batches" "hello" "$out"
out=$(run "SELECT id FROM notes WHERE body LIKE '%o''clock%'")
check "escaped quote in literal" "(0 rows)" "$out"
out=$(run "select ID from NOTES where TOPIC = 'COFFEE'")
check "case insensitivity" "(2 rows)" "$out"

echo
echo "-- memory: reinforcement"
run "SELECT * FROM notes WHERE topic = 'coffee'" >/dev/null
run "SELECT * FROM notes WHERE topic = 'coffee'" >/dev/null
run "SELECT * FROM notes WHERE topic = 'coffee'" >/dev/null
out=$(run "SELECT id, _access FROM notes ORDER BY _access DESC")
check "repeated reads raise access count" "id" "$out"

# ids 1 and 2 are the coffee rows we just read three times; either may come out
# on top, but a heavily used row must outrank a barely used one.
strong=$(run "SELECT TOP 1 id FROM notes ORDER BY _strength DESC" | sed -n 3p | tr -d ' ')
case "$strong" in
    1|2) check "most-used row is the strongest" "yes" "yes" ;;
    *)   check "most-used row is the strongest" "1 or 2" "$strong" ;;
esac

unused=$("$BIN" "$DB" -r -c "SELECT _strength FROM notes WHERE id = 4" | sed -n 3p | tr -d ' ')
used=$("$BIN" "$DB" -r -c "SELECT _strength FROM notes WHERE id = 1" | sed -n 3p | tr -d ' ')
ranked=$(awk -v a="$used" -v b="$unused" 'BEGIN { print (a > b) ? "yes" : "no" }')
check "a used row outranks a rarely used one" "yes" "$ranked"

out=$(run "SHOW MEMORY FROM notes")
check "show memory reports strength" "_strength" "$out"
check "show memory reports access counts" "_access" "$out"

echo
echo "-- memory: decay"
# Decay must be measured in observer mode (-r), otherwise the measuring query
# reinforces the row and pushes its last_access forward, and we would be
# testing our own interference instead of the forgetting curve.
DECAYDB="$(mktemp -u /tmp/nexdb-decay-XXXXXX).ndb"
peek() { NEXDB_TIME_OFFSET="$1" "$BIN" "$DECAYDB" -r -c \
         "SELECT TOP 1 _strength FROM t WHERE id = 1" | sed -n 3p | tr -d ' '; }

"$BIN" "$DECAYDB" -c "CREATE TABLE t (id INT, note NVARCHAR(50));
                      INSERT INTO t (id, note) VALUES (1, 'a memory')" >/dev/null

s0=$(peek 0)
s1=$(peek $((7 * 86400)))
s4=$(peek $((28 * 86400)))

check "a fresh row starts at full strength" "1.000" "$s0"
halved=$(awk -v a="$s0" -v b="$s1" \
         'BEGIN { r = b / a; print (r > 0.49 && r < 0.51) ? "yes" : "no" }')
check "one half-life halves strength" "yes" "$halved"
quartered=$(awk -v a="$s0" -v b="$s4" \
            'BEGIN { r = b / a; print (r > 0.055 && r < 0.07) ? "yes" : "no" }')
check "four half-lives leave about a sixteenth" "yes" "$quartered"

# observer mode must genuinely leave no trace
before=$("$BIN" "$DECAYDB" -r -c "SELECT _access FROM t WHERE id = 1" | sed -n 3p | tr -d ' ')
"$BIN" "$DECAYDB" -r -c "SELECT * FROM t" >/dev/null
after=$("$BIN" "$DECAYDB" -r -c "SELECT _access FROM t WHERE id = 1" | sed -n 3p | tr -d ' ')
check "observer mode does not reinforce" "$before" "$after"

# and normal mode must
"$BIN" "$DECAYDB" -c "SELECT * FROM t" >/dev/null
bumped=$("$BIN" "$DECAYDB" -r -c "SELECT _access FROM t WHERE id = 1" | sed -n 3p | tr -d ' ')
grew=$(awk -v a="$after" -v b="$bumped" 'BEGIN { print (b > a) ? "yes" : "no" }')
check "normal mode does reinforce" "yes" "$grew"
rm -f "$DECAYDB"

echo
echo "-- memory: associations and recall"
out=$(run "SHOW LINKS")
check "co-accessed rows are linked" "weight" "$out"
out=$(run "RECALL 'aeropress'")
check "recall finds a lexical match" "aeropress" "$out"
out=$(run "RECALL 'buddy'")
check "recall finds the dog rows" "buddy" "$out"
out=$(run "RECALL 'nonexistentgibberish'")
check "recall reports an honest miss" "nothing comes to mind" "$out"
out=$(run "REMEMBER FROM notes WHERE id = 5")
check "explicit remember" "reinforced" "$out"
out=$(run "FORGET FROM notes WHERE id = 5")
check "explicit forget" "faded" "$out"
out=$(run "SELECT _strength FROM notes WHERE id = 5")
check "forgotten row has zero strength, but still exists" "0.000" "$out"

echo
echo "-- storage: page spanning and durability"
run "CREATE TABLE bulk (n INT, filler NVARCHAR(200))" >/dev/null
i=1
sql=""
while [ "$i" -le 60 ]; do
    sql="$sql INSERT INTO bulk (n, filler) VALUES ($i, 'padding padding padding padding padding padding padding padding padding');"
    i=$((i + 1))
done
run "$sql" >/dev/null
out=$(run "SELECT COUNT(*) FROM bulk")
check "60 rows across multiple pages" "60" "$out"
out=$(run "SELECT n FROM bulk WHERE n = 60")
check "last row on the last page is readable" "(1 row)" "$out"
out=$(run "DELETE FROM bulk WHERE n < 30")
check "bulk delete" "(29 rows deleted)" "$out"
out=$(run "SELECT COUNT(*) FROM bulk")
check "count after bulk delete" "31" "$out"
run "INSERT INTO bulk (n, filler) VALUES (999, 'reuses a tombstoned slot')" >/dev/null
out=$(run "SELECT n FROM bulk WHERE n = 999")
check "insert reuses freed slots" "999" "$out"

out=$(run "SHOW TABLES")
check "show tables lists both tables" "bulk" "$out"
out=$(run "DROP TABLE bulk")
check "drop table" "dropped" "$out"
out=$(run "SELECT * FROM bulk")
check "dropped table is gone" "unknown table" "$out"

out=$(run "CHECKPOINT")
check "checkpoint" "checkpoint complete" "$out"

# reopening the same file must find everything still there
out=$(run "SELECT COUNT(*) FROM notes")
check "data survives every restart in this run" "5" "$out"

echo
echo "-- unsupported trailing syntax must not half-execute a statement"
# Regression: statements run as they are parsed, so an unsupported trailing
# clause used to leave a complete-looking prefix that had already executed.
# "DELETE FROM t LIMIT 1" emptied the whole table and then reported an error.
run "CREATE TABLE guard (id INT, n INT)" >/dev/null
run "INSERT INTO guard (id, n) VALUES (1,1),(2,2),(3,3)" >/dev/null

out=$(run "DELETE FROM guard LIMIT 1")
check "DELETE with a trailing LIMIT is refused" "Nothing was run" "$out"
out=$(run "SELECT COUNT(*) FROM guard")
check "...and deleted nothing" "3" "$out"

out=$(run "UPDATE guard SET n = 99 FROM other WHERE x = 1")
check "UPDATE with an unsupported FROM is refused" "Nothing was run" "$out"
out=$(run "SELECT n FROM guard WHERE n = 99")
check "...and updated nothing" "(0 rows)" "$out"

out=$(run "SELECT n FROM guard ORDER BY n LIMIT 2")
check "LIMIT on SELECT returns the first two rows" "1" "$out"
out=$(run "SELECT COUNT(*) FROM guard")
check "...and the table is untouched" "3" "$out"
out=$(run "SELECT n FROM guard ORDER BY n LIMIT 1 OFFSET 1")
check "LIMIT OFFSET paginates from the second row" "2" "$out"

echo
echo "-- UNION / INTERSECT / EXCEPT"
run "CREATE TABLE seta (x INT)" >/dev/null
run "INSERT INTO seta VALUES (1),(2),(2),(3)" >/dev/null
run "CREATE TABLE setb (x INT)" >/dev/null
run "INSERT INTO setb VALUES (2),(3),(4)" >/dev/null
out=$(run "SELECT x FROM seta UNION SELECT x FROM setb ORDER BY x")
check "UNION deduplicates across both sides" "(4 rows)" "$out"
out=$(run "SELECT x FROM seta UNION ALL SELECT x FROM setb ORDER BY x")
check "UNION ALL keeps duplicates" "(7 rows)" "$out"
out=$(run "SELECT x FROM seta INTERSECT SELECT x FROM setb ORDER BY x")
check "INTERSECT keeps rows in both sides" "(2 rows)" "$out"
out=$(run "SELECT x FROM seta EXCEPT SELECT x FROM setb ORDER BY x")
check "EXCEPT keeps rows only on the left" "1" "$out"
out=$(run "SELECT x FROM seta EXCEPT SELECT x FROM seta")
check "EXCEPT of a set with itself is empty" "(0 rows)" "$out"
out=$(run "SELECT x FROM seta UNION SELECT x FROM setb ORDER BY x DESC LIMIT 2")
check "trailing ORDER BY and LIMIT bind to the compound result" "(2 rows)" "$out"
out=$(run "SELECT x FROM seta UNION SELECT x FROM setb ORDER BY x DESC LIMIT 2")
check "...and the DESC order applies" "4" "$out"
out=$(run "SELECT x FROM seta UNION ALL SELECT x FROM setb LIMIT 2 OFFSET 3")
check "trailing OFFSET skips rows of the merged result" "(2 rows)" "$out"
out=$(run "SELECT * FROM (SELECT x FROM seta UNION SELECT 9) AS u WHERE x > 1 ORDER BY x")
check "a compound SELECT can be a derived table" "(3 rows)" "$out"
out=$(run "SELECT 9 UNION SELECT 1 UNION SELECT 5 ORDER BY x")
check "a three-way UNION chain is left-associative" "(3 rows)" "$out"
run "CREATE TABLE setc (n INT)" >/dev/null
run "INSERT INTO setc SELECT x FROM seta UNION SELECT x FROM setb" >/dev/null
out=$(run "SELECT COUNT(*) FROM setc")
check "INSERT..SELECT accepts a compound SELECT" "4" "$out"
out=$(run "SELECT x FROM seta UNION SELECT x, 1 FROM setb")
check "a set operation needs matching column counts" "different numbers of columns" "$out"
out=$(run "SELECT 1 UNION")
check "a set operator needs a SELECT after it" "expected SELECT after UNION" "$out"
out=$(run "SELECT n FROM guard JOIN other ON guard.id = other.id")
check "JOIN with missing table is refused" "unknown table" "$out"
run "CREATE TABLE other (id INT, x INT)" >/dev/null
run "INSERT INTO other (id, x) VALUES (1,10),(2,20)" >/dev/null
out=$(run "SELECT guard.n, other.x FROM guard INNER JOIN other ON guard.id = other.id")
check "INNER JOIN works" "1  10" "$out"
out=$(run "SELECT COUNT(*) FROM guard LEFT JOIN other ON guard.id = other.id")
check "LEFT JOIN works" "3" "$out"
out=$(run "SELECT guard.n, other.x FROM guard JOIN other ON guard.id = other.id WHERE other.x > 10")
check "WHERE on a joined column" "2  20" "$out"
out=$(run "SELECT guard.n, other.x FROM guard LEFT JOIN other ON guard.id = other.id WHERE other.x IS NULL")
check "LEFT JOIN keeps unmatched rows" "3" "$out"
out=$(run "SELECT COUNT(*) FROM guard JOIN other ON guard.id = other.id WHERE other.x > 10")
check "aggregates over a join with WHERE" "1" "$out"
out=$(run "SELECT COUNT(*) FROM guard JOIN other ON guard.id = other.id GROUP BY guard.n")
check "GROUP BY with JOIN is refused, not wrong" "not yet supported" "$out"
out=$(run "SELECT n FROM guard, other")
check "comma join is refused rather than ignored" "Nothing was run" "$out"

# but real statement separators must still work
out=$(run "SELECT COUNT(*) FROM guard; SELECT COUNT(*) FROM guard")
check "semicolon-separated batches still run" "(1 row)" "$out"
out=$(run "PRINT 'one'
GO
PRINT 'two'
GO")
check "GO-separated batches still run" "two" "$out"
run "DROP TABLE guard" >/dev/null

echo
echo "-- long values are handled honestly, never silently truncated"
run "CREATE TABLE big (id INT, body NVARCHAR(4000))" >/dev/null
long=$(awk 'BEGIN { for (i = 0; i < 300; i++) printf "abcdefghij"; printf "NEEDLE" }')
out=$(run "INSERT INTO big (id, body) VALUES (1, '$long')")
check "a 3006-character value is stored" "(1 row inserted)" "$out"
out=$(run "SELECT id FROM big WHERE body LIKE '%NEEDLE%'")
check "LIKE matches past the 512th character" "(1 row)" "$out"
out=$(run "RECALL 'NEEDLE'")
check "RECALL matches past the 512th character" "big" "$out"
huge=$(awk 'BEGIN { for (i = 0; i < 500; i++) printf "0123456789" }')
out=$(run "INSERT INTO big (id, body) VALUES (2, '$huge')")
check "an over-long literal is refused, not truncated" "longer than" "$out"
out=$(run "SELECT COUNT(*) FROM big")
check "the refused row was not stored" "1" "$out"
oversize=$(awk 'BEGIN { for (i = 0; i < 409; i++) printf "0123456789" }')
out=$(run "INSERT INTO big (id, body) VALUES (3, '$oversize')")
check "a value longer than the column is refused" "the column holds 4000" "$out"
# and with no declared limit, the page size is overflowed via chains
run "CREATE TABLE bigmax (id INT, body NVARCHAR(MAX))" >/dev/null
out=$(run "INSERT INTO bigmax (id, body) VALUES (3, '$oversize')")
check "a row too big for a page uses overflow chain" "1 row inserted" "$out"
out=$(run "SELECT id, LEN(body) FROM bigmax")
check "overflow row is stored and readable" "4090" "$out"
run "DROP TABLE bigmax" >/dev/null
run "DROP TABLE big" >/dev/null

echo
echo "-- corrupt input is rejected, not misread"
echo "this is definitely not a database" > "$DB.junk"
out=$("$BIN" "$DB.junk" -c "SELECT 1" 2>&1)
check "non-database file refused" "not a nexdb database" "$out"
rm -f "$DB.junk"

# A corrupt data page used to send tuple_decode reading off the end of the page
# buffer (ASan: stack-buffer-overflow in rd64). Slot offsets and lengths come
# out of the file and must be treated as hostile.
CORRUPT="$(mktemp -u /tmp/nexdb-corrupt-XXXXXX).ndb"
"$BIN" "$CORRUPT" -c "CREATE TABLE t (id INT, s NVARCHAR(80));
                      INSERT INTO t (id,s) VALUES (1,'aaaa'),(2,'bbbb'),(3,'cccc')" >/dev/null 2>&1
if command -v python3 >/dev/null 2>&1; then
    # smash the first heap page's slot directory with 0xff bytes
    python3 - "$CORRUPT" <<'PY'
import sys
with open(sys.argv[1], 'r+b') as f:
    f.seek(4096 * 2)
    f.write(b'\xff' * 256)
PY
    out=$("$BIN" "$CORRUPT" -r -c "SELECT COUNT(*) FROM t" 2>&1)
    rc=$?
    check_not "a corrupt page does not crash the engine" "Segmentation" "$out"
    if [ "$rc" -lt 128 ]; then
        PASS=$((PASS + 1)); printf '  ok    %s\n' "...and exits without a signal"
    else
        FAIL=$((FAIL + 1)); printf '  FAIL  %s (exit %d)\n' "...and exits without a signal" "$rc"
    fi
    for stmt in "SELECT * FROM t" "SHOW MEMORY FROM t" "RECALL 'aaaa'" "DELETE FROM t WHERE id = 1"; do
        out=$("$BIN" "$CORRUPT" -c "$stmt" 2>&1)
        rc=$?
        if [ "$rc" -lt 128 ]; then
            PASS=$((PASS + 1)); printf '  ok    %s\n' "corrupt page survives: $stmt"
        else
            FAIL=$((FAIL + 1)); printf '  FAIL  %s (exit %d)\n' "corrupt page survives: $stmt" "$rc"
        fi
    done
fi
rm -f "$CORRUPT"

echo
echo "-- the shipped example script actually runs"
# The demo is documentation people are told to run, so a parser change that
# breaks it must fail the build. Requiring statements to end at ';'/GO once
# broke every consecutive PRINT in it, and nothing caught that.
DEMODB="$(mktemp -u /tmp/nexdb-demo-XXXXXX).ndb"
for script in examples/*.sql; do
    [ -f "$script" ] || continue
    rm -f "$DEMODB"
    out=$("$BIN" "$DEMODB" -f "$script" 2>&1)
    check_not "$script runs without error" "error:" "$out"
    # and again, to prove it is re-runnable
    out=$("$BIN" "$DEMODB" -f "$script" 2>&1)
    check_not "$script is re-runnable" "error:" "$out"
done
rm -f "$DEMODB"

echo
echo "-- explicit REMEMBER counts as exactly one access"
# reinforce() used to call mem_touch a second time purely to build
# associations, so one REMEMBER incremented _access twice.
ACCDB="$(mktemp -u /tmp/nexdb-acc-XXXXXX).ndb"
"$BIN" "$ACCDB" -c "CREATE TABLE n (id INT); INSERT INTO n (id) VALUES (1),(2)" >/dev/null 2>&1
"$BIN" "$ACCDB" -c "REMEMBER FROM n WHERE id = 1" >/dev/null 2>&1
acc=$("$BIN" "$ACCDB" -r -c "SELECT _access FROM n WHERE id = 1" | sed -n 3p | tr -d ' ')
check "one REMEMBER = one access, not two" "1" "$acc"
"$BIN" "$ACCDB" -c "SELECT * FROM n" >/dev/null 2>&1
acc=$("$BIN" "$ACCDB" -r -c "SELECT _access FROM n WHERE id = 1" | sed -n 3p | tr -d ' ')
check "a following SELECT adds exactly one more" "2" "$acc"
rm -f "$ACCDB"

echo
echo "-- a long RECALL phrase is not silently clipped"
# recall_text was 512 bytes, so a long phrase lost its tail and the search
# quietly returned nothing instead of complaining.
RCDB="$(mktemp -u /tmp/nexdb-rc-XXXXXX).ndb"
"$BIN" "$RCDB" -c "CREATE TABLE n (id INT, body NVARCHAR(200));
                   INSERT INTO n (id, body) VALUES (1, 'the needle is in here')" >/dev/null 2>&1
phrase=$(awk 'BEGIN { for (i = 0; i < 200; i++) printf "zz "; printf "needle" }')
out=$("$BIN" "$RCDB" -r -c "RECALL '$phrase'" 2>&1)
check "a 600-character phrase still finds its last word" "needle is in here" "$out"
rm -f "$RCDB"

echo
echo "-- declared types are enforced, not decorative"
TY="$(mktemp -u /tmp/nexdb-types-XXXXXX).ndb"
ty() { "$BIN" "$TY" -c "$1" 2>&1; }
ty "CREATE TABLE t (id INT PRIMARY KEY, tiny TINYINT, small SMALLINT, big BIGINT,
                    s NVARCHAR(5), free NVARCHAR(MAX), f FLOAT, b BIT,
                    d DATETIME, u NVARCHAR(20) UNIQUE)" >/dev/null

out=$(ty "INSERT INTO t (id, s) VALUES (1, 'far too long')")
check "NVARCHAR(5) refuses a longer string" "the column holds 5" "$out"
out=$(ty "INSERT INTO t (id, s) VALUES (1, 'fits')")
check "...but accepts one that fits" "(1 row inserted)" "$out"
out=$(ty "INSERT INTO t (id, free) VALUES (2, 'no declared limit so this is fine')")
check "NVARCHAR(MAX) has no length limit" "(1 row inserted)" "$out"

out=$(ty "INSERT INTO t (id) VALUES (5000000000)")
check "INT refuses a value past 2^31" "out of range for 'id'" "$out"
out=$(ty "INSERT INTO t (id, tiny) VALUES (3, 999)")
check "TINYINT refuses 999" "TINYINT holds 0 to 255" "$out"
out=$(ty "INSERT INTO t (id, tiny) VALUES (3, -1)")
check "TINYINT refuses -1" "out of range" "$out"
out=$(ty "INSERT INTO t (id, small) VALUES (4, 40000)")
check "SMALLINT refuses 40000" "out of range" "$out"
out=$(ty "INSERT INTO t (id, big) VALUES (5, 5000000000)")
check "BIGINT accepts 5 billion" "(1 row inserted)" "$out"
out=$(ty "INSERT INTO t (id) VALUES (99999999999999999999)")
check "a literal past 64 bits is refused, not saturated" "too large" "$out"
out=$(ty "INSERT INTO t (id, tiny) VALUES (6, 1.5)")
check "an INT column refuses 1.5" "not a whole number" "$out"
out=$(ty "INSERT INTO t (id, tiny) VALUES (6, 42.0)")
check "...but accepts 42.0" "(1 row inserted)" "$out"

out=$(ty "INSERT INTO t (id, b) VALUES (7, 'yes')")
check "BIT refuses 'yes'" "expected 0, 1, true or false" "$out"
out=$(ty "INSERT INTO t (id, b) VALUES (7, 5)")
check "BIT refuses 5" "expected 0 or 1" "$out"
out=$(ty "INSERT INTO t (id, b) VALUES (7, 'true')")
check "BIT accepts 'true'" "(1 row inserted)" "$out"

out=$(ty "INSERT INTO t (id, d) VALUES (8, '2026-99-99')")
check "DATETIME refuses month 99" "not a valid date" "$out"
out=$(ty "INSERT INTO t (id, d) VALUES (8, '2026-02-30')")
check "DATETIME refuses 30 February" "not a valid date" "$out"
out=$(ty "INSERT INTO t (id, d) VALUES (8, '2024-02-29')")
check "DATETIME accepts a real leap day" "(1 row inserted)" "$out"
out=$(ty "INSERT INTO t (id, d) VALUES (9, '2026-03-01 14:30:00')")
check "DATETIME accepts a time component" "(1 row inserted)" "$out"
out=$(ty "INSERT INTO t (id, d) VALUES (10, 'tuesday')")
check "DATETIME refuses free text" "not a valid date" "$out"

out=$(ty "INSERT INTO t (id, f) VALUES (11, 'not a number')")
check "FLOAT refuses non-numeric text" "not a number" "$out"
out=$(ty "INSERT INTO t (id, f) VALUES (11, '2.5')")
check "FLOAT accepts numeric text" "(1 row inserted)" "$out"

echo
echo "-- PRIMARY KEY and UNIQUE are enforced"
out=$(ty "INSERT INTO t (id) VALUES (500)")
check "a fresh key inserts" "(1 row inserted)" "$out"
out=$(ty "INSERT INTO t (id) VALUES (500)")
check "a duplicate primary key is refused" "primary key '500' already exists" "$out"
out=$(ty "INSERT INTO t (id, u) VALUES (501, 'once')")
check "a unique value inserts" "(1 row inserted)" "$out"
out=$(ty "INSERT INTO t (id, u) VALUES (502, 'once')")
check "a duplicate unique value is refused" "unique value 'once' already exists" "$out"
out=$(ty "UPDATE t SET id = 500 WHERE id = 501")
check "UPDATE cannot create a duplicate key" "already exists" "$out"
out=$(ty "UPDATE t SET id = 501 WHERE id = 501")
check "...but a row may keep its own key" "(1 row updated)" "$out"
out=$(ty "INSERT INTO t (id, u) VALUES (503, NULL)")
check "NULLs do not collide under UNIQUE" "(1 row inserted)" "$out"
out=$(ty "INSERT INTO t (tiny) VALUES (1)")
check "PRIMARY KEY implies NOT NULL" "does not accept NULL" "$out"

echo
echo "-- names must be unambiguous"
out=$(ty "CREATE TABLE dup (a INT, a NVARCHAR(5))")
check "a column declared twice is refused" "declared twice" "$out"
out=$(ty "INSERT INTO t (id, id) VALUES (600, 601)")
check "a column listed twice in INSERT is refused" "listed twice" "$out"
out=$(ty "UPDATE t SET tiny = 1, tiny = 2 WHERE id = 500")
check "a column assigned twice in SET is refused" "assigned twice" "$out"
long_name=$(awk 'BEGIN { for (i = 0; i < 140; i++) printf "a" }')
out=$(ty "CREATE TABLE ln ($long_name INT)")
check "an over-long name is refused, not truncated" "the limit is 127" "$out"

echo
echo "-- comparison is numeric or an error, never accidentally alphabetical"
CMP="$(mktemp -u /tmp/nexdb-cmp-XXXXXX).ndb"
"$BIN" "$CMP" -c "CREATE TABLE c (n INT, f FLOAT, s NVARCHAR(20));
                  INSERT INTO c (n,f,s) VALUES (9,0.1234567890123,'a'),
                                               (10,2.5,'b'),(100,3.5,'c')" >/dev/null 2>&1
cmp_q() { "$BIN" "$CMP" -r -c "$1" 2>&1; }
out=$(cmp_q "SELECT n FROM c WHERE n > '10'")
check "n > '10' finds only 100" "(1 row)" "$out"
check "...and that row is 100" "100" "$out"
out=$(cmp_q "SELECT n FROM c WHERE n < '10'")
check "n < '10' finds only 9" "(1 row)" "$out"
out=$(cmp_q "SELECT n FROM c WHERE n < 'banana'")
check "comparing a number to non-numeric text is an error" "no meaningful ordering" "$out"
out=$(cmp_q "SELECT n FROM c WHERE s < 'b'")
check "text compared with text still works" "(1 row)" "$out"

out=$(cmp_q "SELECT f FROM c WHERE n = 9")
check "a float prints all its digits" "0.1234567890123" "$out"
out=$(cmp_q "SELECT n FROM c WHERE f = 0.1234567890123")
check "and matches its own stored value" "(1 row)" "$out"
out=$(cmp_q "SELECT n FROM c WHERE f = 0.123457")
check "and does not match a rounded-off version" "(0 rows)" "$out"
rm -f "$CMP"

echo
echo "-- LIKE can search for literal wildcards"
"$BIN" "$TY" -c "INSERT INTO t (id, free) VALUES (700, '50% off, today_only')" >/dev/null 2>&1
out=$(ty "SELECT id FROM t WHERE free LIKE '%!% off%' ESCAPE '!'")
check "ESCAPE finds a literal percent sign" "700" "$out"
out=$(ty "SELECT id FROM t WHERE free LIKE '%today!_only%' ESCAPE '!'")
check "ESCAPE finds a literal underscore" "700" "$out"
out=$(ty "SELECT id FROM t WHERE free LIKE '%99!% off%' ESCAPE '!'")
check "...and does not match the wrong literal" "(0 rows)" "$out"
out=$(ty "SELECT id FROM t WHERE free LIKE '%' ESCAPE 'xy'")
check "a bad ESCAPE argument is refused" "single-character" "$out"

echo
echo "-- durability and command line"
out=$(ty "CHECKPOINT")
check "CHECKPOINT reports flushing to disk" "flushed to disk" "$out"
out=$("$BIN" --version 2>&1)
check "--version prints a version" "nexdb" "$out"
[ -e "./--version" ] && { FAIL=$((FAIL+1)); echo "  FAIL  --version created a stray database file"; } \
                     || { PASS=$((PASS+1)); echo "  ok    --version creates no stray file"; }
out=$("$BIN" "$TY" --nonsense 2>&1)
check "an unknown option is refused" "unknown option" "$out"

echo
echo "-- FORGET fades a row without stealing its neighbour's links"
FG="$(mktemp -u /tmp/nexdb-forget-XXXXXX).ndb"
"$BIN" "$FG" -c "CREATE TABLE n (id INT, body NVARCHAR(40));
                 INSERT INTO n (id,body) VALUES (1,'alpha'),(2,'beta'),(3,'gamma')" >/dev/null 2>&1
"$BIN" "$FG" -c "SELECT * FROM n" >/dev/null 2>&1
"$BIN" "$FG" -c "SELECT * FROM n" >/dev/null 2>&1
before=$("$BIN" "$FG" -r -c "SHOW LINKS TOP 20" | grep -c '^[0-9]')
"$BIN" "$FG" -c "FORGET FROM n WHERE id = 1" >/dev/null 2>&1
after=$("$BIN" "$FG" -r -c "SHOW LINKS TOP 20" | grep -c '^[0-9]')
check "FORGET leaves the association graph intact" "$before" "$after"
out=$("$BIN" "$FG" -r -c "SELECT _strength FROM n WHERE id = 1" | sed -n 3p | tr -d ' ')
check "...while still zeroing the row's strength" "0.000" "$out"
# but a real DELETE must still clean up
"$BIN" "$FG" -c "DELETE FROM n WHERE id = 1" >/dev/null 2>&1
gone=$("$BIN" "$FG" -r -c "SHOW LINKS TOP 20" | grep -c '^[0-9]')
lost=$(awk -v a="$after" -v b="$gone" 'BEGIN { print (b < a) ? "yes" : "no" }')
check "DELETE does remove the row's links" "yes" "$lost"
rm -f "$FG"

echo
echo "-- SHOW LINKS honours TOP after filtering by table"
SL="$(mktemp -u /tmp/nexdb-links-XXXXXX).ndb"
"$BIN" "$SL" -c "CREATE TABLE p (id INT, t NVARCHAR(20)); CREATE TABLE q (id INT, t NVARCHAR(20));
                 INSERT INTO p (id,t) VALUES (1,'p1'),(2,'p2'),(3,'p3'),(4,'p4'),(5,'p5');
                 INSERT INTO q (id,t) VALUES (1,'q1'),(2,'q2'),(3,'q3'),(4,'q4'),(5,'q5')" >/dev/null 2>&1
"$BIN" "$SL" -c "SELECT * FROM q" >/dev/null 2>&1
"$BIN" "$SL" -c "SELECT * FROM q" >/dev/null 2>&1
"$BIN" "$SL" -c "SELECT * FROM p" >/dev/null 2>&1
rows=$("$BIN" "$SL" -r -c "SHOW LINKS FROM p TOP 5" | grep -c '^[0-9]')
check "asking for 5 links in one table returns 5" "5" "$rows"
rm -f "$SL"

rm -f "$TY"

echo
echo "-- scalar functions"
FN="$(mktemp -u /tmp/nexdb-fn-XXXXXX).ndb"
fn() { "$BIN" "$FN" -r -c "$1" 2>&1; }
"$BIN" "$FN" -c "CREATE TABLE t (id INT PRIMARY KEY, s NVARCHAR(20), n FLOAT);
                 INSERT INTO t (id,s,n) VALUES (1,'north',100.5),(2,'south',200),
                                               (3,'north',50.25),(4,'east',75),
                                               (5,'south',300)" >/dev/null 2>&1

out=$(fn "SELECT LEN('hello')");            check "LEN" "5" "$out"
out=$(fn "SELECT UPPER('abc')");            check "UPPER" "ABC" "$out"
out=$(fn "SELECT LOWER('ABC')");            check "LOWER" "abc" "$out"
out=$(fn "SELECT SUBSTRING('abcdef', 2, 3)"); check "SUBSTRING" "bcd" "$out"
out=$(fn "SELECT LEFT('abcdef', 2)");       check "LEFT" "ab" "$out"
out=$(fn "SELECT RIGHT('abcdef', 2)");      check "RIGHT" "ef" "$out"
out=$(fn "SELECT REPLACE('a-b-c','-','+')"); check "REPLACE" "a+b+c" "$out"
out=$(fn "SELECT TRIM('  x  ')");           check "TRIM" "x" "$out"
out=$(fn "SELECT REVERSE('abc')");          check "REVERSE" "cba" "$out"
out=$(fn "SELECT CONCAT('a','b','c')");     check "CONCAT" "abc" "$out"
out=$(fn "SELECT ABS(-7)");                 check "ABS" "7" "$out"
out=$(fn "SELECT ROUND(3.14159, 2)");       check "ROUND" "3.14" "$out"
out=$(fn "SELECT FLOOR(2.7)");              check "FLOOR" "2" "$out"
out=$(fn "SELECT CEILING(2.1)");            check "CEILING" "3" "$out"
out=$(fn "SELECT SQRT(16)");                check "SQRT" "4" "$out"
out=$(fn "SELECT SIGN(-3)");                check "SIGN" "-1" "$out"
out=$(fn "SELECT POWER(2,10)");             check "POWER" "1024" "$out"
out=$(fn "SELECT ISNULL(NULL,'fallback')"); check "ISNULL substitutes" "fallback" "$out"
out=$(fn "SELECT ISNULL('kept','fallback')"); check "ISNULL keeps a value" "kept" "$out"
out=$(fn "SELECT COALESCE(NULL,NULL,'third')"); check "COALESCE" "third" "$out"
out=$(fn "SELECT NULLIF(5,5)");             check "NULLIF matching gives NULL" "NULL" "$out"
out=$(fn "SELECT NULLIF(5,6)");             check "NULLIF differing keeps" "5" "$out"
out=$(fn "SELECT IIF(1=1,'yes','no')");     check "IIF" "yes" "$out"
out=$(fn "SELECT LEN(GETDATE())");          check "GETDATE returns a timestamp" "19" "$out"
out=$(fn "SELECT UPPER(NULL)");             check "a NULL argument gives NULL" "NULL" "$out"
out=$(fn "SELECT LEN()");                   check "wrong argument count is an error" "takes 1 argument" "$out"
out=$(fn "SELECT NOSUCHFUNC(1)");           check "an unknown function is named" "unknown function" "$out"
out=$(fn "SELECT SQRT(-1)");                check "SQRT of a negative is an error" "negative" "$out"

echo
echo "-- CAST"
out=$(fn "SELECT CAST('42' AS INT) + 1");   check "CAST text to INT" "43" "$out"
out=$(fn "SELECT CAST(3.99 AS INT)");       check "CAST truncates to INT" "3" "$out"
out=$(fn "SELECT CAST(7 AS NVARCHAR(10))"); check "CAST INT to text" "7" "$out"
out=$(fn "SELECT CAST('abc' AS INT)");      check "an impossible CAST is an error" "cannot convert" "$out"
out=$(fn "SELECT CAST(99999 AS TINYINT)");  check "CAST respects the target range" "does not fit" "$out"

echo
echo "-- UUID is a real type"
UU="$(mktemp -u /tmp/nexdb-uuid-XXXXXX).ndb"
uu() { "$BIN" "$UU" -c "$1" 2>&1; }
uu "CREATE TABLE u (id UNIQUEIDENTIFIER PRIMARY KEY DEFAULT NEWID(), label NVARCHAR(20))" >/dev/null
out=$(uu "INSERT INTO u (label) VALUES ('auto')")
check "DEFAULT NEWID() fills a UUID" "(1 row inserted)" "$out"
out=$("$BIN" "$UU" -r -c "SELECT LEN(CAST(id AS NVARCHAR(40))) FROM u")
check "a generated UUID is 36 characters" "36" "$out"
out=$(uu "INSERT INTO u (id, label) VALUES ('6F9619FF-8B86-D011-B42D-00CF4FC964FF','manual')")
check "a canonical UUID is accepted" "(1 row inserted)" "$out"
out=$("$BIN" "$UU" -r -c "SELECT label FROM u WHERE id = '6f9619ff-8b86-d011-b42d-00cf4fc964ff'")
check "UUID matching ignores case" "manual" "$out"
out=$(uu "INSERT INTO u (id, label) VALUES ('banana','bad')")
check "a non-UUID is refused" "not a UUID" "$out"
out=$(uu "INSERT INTO u (id, label) VALUES ('6F9619FF-8B86-D011-B42D-00CF4FC964FF','dup')")
check "UUID primary keys are enforced" "already exists" "$out"
rm -f "$UU"

echo
echo "-- aggregates and GROUP BY"
out=$(fn "SELECT COUNT(*) FROM t");            check "COUNT(*)" "5" "$out"
out=$(fn "SELECT SUM(n) FROM t");              check "SUM" "725.75" "$out"
out=$(fn "SELECT AVG(n) FROM t");              check "AVG" "145.15" "$out"
out=$(fn "SELECT MIN(n) FROM t");              check "MIN" "50.25" "$out"
out=$(fn "SELECT MAX(n) FROM t");              check "MAX" "300" "$out"
out=$(fn "SELECT COUNT(*) FROM t WHERE s = 'nowhere'")
check "COUNT over no rows is 0, not empty" "0" "$out"
out=$(fn "SELECT SUM(n) FROM t WHERE s = 'nowhere'")
check "SUM over no rows is NULL" "NULL" "$out"
out=$(fn "SELECT s, COUNT(*) FROM t GROUP BY s")
check "GROUP BY produces one row per key" "(3 rows)" "$out"
out=$(fn "SELECT s, SUM(n) AS total FROM t GROUP BY s ORDER BY total DESC")
check "ORDER BY an aggregate alias works" "south" "$out"
out=$(fn "SELECT s FROM t GROUP BY s HAVING SUM(n) > 150")
check "HAVING filters groups" "(2 rows)" "$out"
out=$(fn "SELECT s, COUNT(*) FROM t")
check "an ungrouped column is refused" "must appear in GROUP BY" "$out"
out=$(fn "SELECT SUM(COUNT(*)) FROM t")
check "a nested aggregate is refused" "cannot contain another aggregate" "$out"
out=$(fn "SELECT * FROM t GROUP BY s")
check "SELECT * with GROUP BY is refused" "cannot be combined" "$out"
out=$(fn "SELECT COUNT(*) FROM t HAVING COUNT(*) > 99")
check "HAVING can eliminate the single group" "(0 rows)" "$out"
out=$(fn "SELECT LEN(s) AS n FROM t GROUP BY LEN(s)")
check "GROUP BY an expression works" "(2 rows)" "$out"

echo
echo "-- expressions, DISTINCT, multi-key ORDER BY"
out=$(fn "SELECT 1");                       check "SELECT without FROM" "1" "$out"
out=$(fn "SELECT 2 + 3 * 4");               check "arithmetic precedence" "14" "$out"
out=$(fn "SELECT 7 % 3");                   check "modulo" "1" "$out"
out=$(fn "SELECT n * 2 FROM t WHERE id = 1"); check "an expression in the select list" "201" "$out"
out=$(fn "SELECT DISTINCT s FROM t");       check "DISTINCT collapses duplicates" "(3 rows)" "$out"
out=$(fn "SELECT DISTINCT s, s FROM t");    check "DISTINCT over several columns" "(3 rows)" "$out"
out=$(fn "SELECT id FROM t ORDER BY s ASC, n DESC")
check "multi-column ORDER BY" "(5 rows)" "$out"
first=$(fn "SELECT TOP 1 id FROM t ORDER BY s ASC, n DESC" | sed -n 3p | tr -d ' ')
check "...sorts by the first key then the second" "4" "$first"
out=$(fn "SELECT id FROM t WHERE n BETWEEN 60 AND 210")
check "BETWEEN" "(3 rows)" "$out"
out=$(fn "SELECT id FROM t WHERE n NOT BETWEEN 60 AND 210")
check "NOT BETWEEN" "(2 rows)" "$out"
out=$(fn "SELECT CASE WHEN n > 150 THEN 'big' ELSE 'small' END FROM t WHERE id = 2")
check "searched CASE" "big" "$out"
out=$(fn "SELECT CASE s WHEN 'north' THEN 'N' ELSE '?' END FROM t WHERE id = 1")
check "simple CASE" "N" "$out"
out=$(fn "SELECT CASE WHEN n > 9999 THEN 'x' END FROM t WHERE id = 1")
check "CASE with no match and no ELSE is NULL" "NULL" "$out"
out=$(fn "SELECT n AS amount FROM t WHERE id = 1")
check "AS alias becomes the heading" "amount" "$out"
out=$(fn "SELECT * FROM t GROUP BY nosuchcol")
check "grouping by an unknown column is an error" "no column" "$out"

echo
echo "-- DDL: ALTER, TRUNCATE, DEFAULT, IDENTITY"
DD="$(mktemp -u /tmp/nexdb-ddl-XXXXXX).ndb"
dd_() { "$BIN" "$DD" -c "$1" 2>&1; }
dd_ "CREATE TABLE t (id INT IDENTITY PRIMARY KEY, name NVARCHAR(20), qty INT DEFAULT 1)" >/dev/null
out=$(dd_ "INSERT INTO t (name) VALUES ('a'),('b'),('c')")
check "IDENTITY fills the key" "(3 rows inserted)" "$out"
ids=$("$BIN" "$DD" -r -c "SELECT id FROM t ORDER BY id" | sed -n '3,5p' | tr -d ' \n')
check "IDENTITY counts 1,2,3" "123" "$ids"
out=$("$BIN" "$DD" -r -c "SELECT qty FROM t WHERE id = 1")
check "DEFAULT fills an omitted column" "1" "$out"
out=$(dd_ "INSERT INTO t (name) VALUES ('d')")
check "IDENTITY keeps counting after a restart" "(1 row inserted)" "$out"
nextid=$("$BIN" "$DD" -r -c "SELECT id FROM t WHERE name = 'd'" | sed -n 3p | tr -d ' ')
check "...and does not restart at 1" "4" "$nextid"

out=$(dd_ "ALTER TABLE t ADD note NVARCHAR(30)")
check "ALTER TABLE ADD" "added" "$out"
out=$("$BIN" "$DD" -r -c "SELECT note FROM t WHERE id = 1")
check "existing rows read NULL for a new column" "NULL" "$out"
out=$(dd_ "ALTER TABLE t ADD status NVARCHAR(10) DEFAULT 'new'")
check "ALTER TABLE ADD with a DEFAULT" "added" "$out"
out=$("$BIN" "$DD" -r -c "SELECT status FROM t WHERE id = 1")
check "...backfills existing rows" "new" "$out"
out=$(dd_ "ALTER TABLE t ADD bad INT NOT NULL")
check "adding NOT NULL without a default is refused" "without a DEFAULT" "$out"
out=$(dd_ "ALTER TABLE t ADD name NVARCHAR(5)")
check "adding a duplicate column is refused" "already exists" "$out"
out=$(dd_ "ALTER TABLE t DROP COLUMN note")
check "ALTER TABLE DROP COLUMN" "dropped" "$out"
out=$("$BIN" "$DD" -r -c "SELECT name, qty, status FROM t WHERE id = 1")
check "remaining columns keep their values" "new" "$out"
out=$(dd_ "SELECT note FROM t")
check "the dropped column is gone" "no column" "$out"
out=$(dd_ "ALTER TABLE t DROP COLUMN nosuch")
check "dropping an unknown column is refused" "no column" "$out"

out=$(dd_ "CREATE TABLE arch (n NVARCHAR(20), q INT)")
check "a table for INSERT..SELECT" "created" "$out"
out=$(dd_ "INSERT INTO arch (n, q) SELECT name, qty FROM t")
check "INSERT INTO ... SELECT" "(4 rows inserted)" "$out"
out=$(dd_ "INSERT INTO arch (n, q) SELECT UPPER(name), qty * 10 FROM t WHERE id = 1")
check "INSERT..SELECT with expressions" "(1 row inserted)" "$out"
out=$(dd_ "INSERT INTO arch (n) SELECT name, qty FROM t")
check "a column-count mismatch is refused" "column" "$out"
out=$(dd_ "TRUNCATE TABLE arch")
check "TRUNCATE removes every row" "removed" "$out"
out=$(dd_ "SELECT COUNT(*) FROM arch")
check "...leaving the table empty but present" "0" "$out"

echo
echo "-- ALTER COLUMN TYPE validates every stored value before narrowing"
out=$(dd_ "CREATE TABLE wide (a BIGINT, s NVARCHAR(MAX))")
check "a table for ALTER COLUMN TYPE" "created" "$out"
out=$(dd_ "INSERT INTO wide (a, s) VALUES (5000000000, 'toolong'), (7, 'x')")
check "insert values that fit only the wide types" "(2 rows inserted)" "$out"
out=$(dd_ "ALTER TABLE wide ALTER COLUMN a INT")
check "narrowing BIGINT to INT with an overflow is refused" "has value 5000000000" "$out"
out=$(dd_ "ALTER TABLE wide ALTER COLUMN s NVARCHAR(2)")
check "narrowing NVARCHAR(MAX) past a long value is refused" "the column holds 2" "$out"
out=$(dd_ "DELETE FROM wide WHERE a = 5000000000; ALTER TABLE wide ALTER COLUMN a INT; ALTER TABLE wide ALTER COLUMN s NVARCHAR(2)")
check "narrowing succeeds once the rows fit" "type changed" "$out"
out=$(dd_ "SELECT a, s FROM wide")
check "the narrowed rows read back correctly" "7" "$out"

echo
echo "-- CHECK constraints are stored, enforced, and survive a restart"
out=$(dd_ "CREATE TABLE inv (qty INT CHECK (qty > 0), price INT, CHECK (price >= 0), note NVARCHAR(20) CHECK (note <> 'nope'))")
check "CREATE TABLE with column- and table-level CHECK" "created" "$out"
out=$(dd_ "INSERT INTO inv (qty, price) VALUES (5, 10)")
check "a row satisfying every CHECK inserts" "(1 row inserted)" "$out"
out=$(dd_ "INSERT INTO inv (qty, price, note) VALUES (5, 10, 'nope')")
check "a column-level CHECK violation is refused" "CHECK constraint failed" "$out"
out=$(dd_ "INSERT INTO inv (qty, price) VALUES (-1, 10)")
check "a table-level CHECK violation is refused" "CHECK constraint failed" "$out"
out=$(dd_ "INSERT INTO inv (qty, price, note) VALUES (3, 1, NULL)")
check "NULL satisfies a CHECK (three-valued logic)" "(1 row inserted)" "$out"
out=$(dd_ "UPDATE inv SET qty = -9 WHERE qty = 3")
check "UPDATE into a CHECK violation is refused" "CHECK constraint failed" "$out"
out=$(dd_ "UPDATE inv SET qty = 7 WHERE qty = 3")
check "UPDATE to a valid value applies" "(1 row updated)" "$out"
out=$(dd_ "SELECT qty FROM inv ORDER BY qty")
check "only valid rows are present" "5" "$out"
out=$(dd_ "INSERT INTO inv (qty, price) SELECT 9, 9")
check "INSERT..SELECT is CHECKed too" "(1 row inserted)" "$out"
out=$(dd_ "INSERT INTO inv (qty, price) SELECT -9, 9")
check "...and a violation there is refused" "CHECK constraint failed" "$out"

echo
echo "-- FOREIGN KEY constraints are validated, enforced, and survive a restart"
out=$(dd_ "CREATE TABLE par (id INT PRIMARY KEY, code NVARCHAR(10) UNIQUE); CREATE TABLE chi (x INT REFERENCES par(id) ON DELETE CASCADE, y NVARCHAR(10) REFERENCES par(code) ON DELETE CASCADE); CREATE TABLE chi2 (a INT, b NVARCHAR(10), FOREIGN KEY (a, b) REFERENCES par(id, code))")
check "FKs on columns, CASCADE, and a table-level composite FK" "created" "$out"
out=$(dd_ "INSERT INTO par VALUES (1, 'aa'); INSERT INTO chi VALUES (1, 'aa'); INSERT INTO chi2 VALUES (1, 'aa')")
check "rows matching the parent insert" "(1 row inserted)" "$out"
out=$(dd_ "INSERT INTO chi VALUES (99, 'aa')")
check "a column-level FK violation is refused" "foreign key violation" "$out"
out=$(dd_ "INSERT INTO chi2 VALUES (1, 'zz')")
check "a composite FK violation is refused" "foreign key violation" "$out"
out=$(dd_ "INSERT INTO chi VALUES (NULL, 'aa')")
check "NULL foreign keys are allowed" "(1 row inserted)" "$out"
out=$(dd_ "UPDATE chi SET x = 5 WHERE x = 1")
check "UPDATE into an FK violation is refused" "foreign key violation" "$out"
out=$(dd_ "UPDATE par SET id = 7 WHERE id = 1")
check "changing a referenced parent key is refused" "cannot change" "$out"
out=$(dd_ "DELETE FROM par WHERE id = 1")
check "deleting a referenced parent row is refused" "still references it" "$out"
out=$(dd_ "INSERT INTO par VALUES (2, 'bb'); INSERT INTO chi VALUES (2, 'bb')")
check "a second parent row inserts" "(1 row inserted)" "$out"
out=$(dd_ "DELETE FROM par WHERE code = 'bb'")
check "ON DELETE CASCADE removes the children too" "(1 row deleted)" "$out"
out=$(dd_ "SELECT COUNT(*) FROM chi")
check "CASCADE removed the referencing chi rows" "2" "$out"
out=$(dd_ "DROP TABLE par")
check "a referenced table cannot be dropped" "references it" "$out"
out=$(dd_ "CREATE TABLE bad (z INT REFERENCES par(zzz))")
check "a missing referenced column is refused" "unknown column" "$out"
out=$(dd_ "CREATE TABLE par2 (id INT PRIMARY KEY, val INT); CREATE TABLE bad (z INT REFERENCES par2(val))")
check "a non-unique referenced column is refused" "not UNIQUE" "$out"
out=$(dd_ "CREATE TABLE bad (z INT REFERENCES nosuch)")
check "a missing referenced table is refused" "unknown table" "$out"
out=$(dd_ "SELECT x FROM chi ORDER BY x")
check "FK rows survive the create-time validation" "1" "$out"
out=$(dd_ "VACUUM; INSERT INTO chi VALUES (1, 'aa'); SELECT COUNT(*) FROM chi")
check "FKs still enforced after VACUUM" "3" "$out"
out=$(dd_ "SELECT id FROM par WHERE id = 1")
check "point lookups still hit the index after VACUUM" "1" "$out"

echo
echo "-- views are stored, expanded at query time, and survive a restart"
out=$(run "CREATE TABLE base (id INT PRIMARY KEY, name NVARCHAR(20)); CREATE VIEW vw AS SELECT id, name FROM base")
check "CREATE VIEW with a SELECT body" "created" "$out"
out=$(run "SELECT * FROM vw")
check "selecting from a view" "0 rows" "$out"
out=$(run "INSERT INTO base VALUES (1, 'a'), (2, 'b'); SELECT * FROM vw")
check "a view sees rows inserted after it was created" "2 rows" "$out"
out=$(run "SELECT id FROM vw WHERE id = 2")
check "WHERE on a view" "2" "$out"
out=$(run "SELECT vw.name FROM vw ORDER BY id DESC")
check "qualified references to a view" "b" "$out"
out=$(run "SELECT COUNT(*) FROM (SELECT id FROM vw) AS x")
check "a view inside a derived table" "COUNT" "$out"
out=$(run "CREATE VIEW vw2 AS SELECT id FROM base WHERE id > 1 UNION SELECT 9; SELECT * FROM vw2")
check "a view body may be a set operation" "9" "$out"
out=$(run "SELECT * FROM vw")
check "the view still works after a restart" "2 rows" "$out"
out=$(run "CREATE VIEW vw AS SELECT 1")
check "a duplicate view name is refused" "already exists" "$out"
out=$(run "CREATE VIEW vbad AS SELECT bogus FROM nosuch; SELECT * FROM vbad")
check "a view on missing tables errors when used" "unknown table" "$out"
out=$(run "DROP VIEW vw2; SELECT * FROM vw2")
check "DROP VIEW removes the view" "unknown table" "$out"
out=$(run "DROP VIEW IF EXISTS nosuch")
check_not "DROP VIEW IF EXISTS on a missing view" "error" "$out"
out=$(run "SELECT * FROM vw")
check "the surviving view is untouched" "2 rows" "$out"

# ---------------------------------------------------------------- procedures

run "CREATE TABLE pbase (id INT PRIMARY KEY, n NVARCHAR(20))" >/dev/null
run "INSERT INTO pbase (id, n) VALUES (1,'one'),(2,'two')" >/dev/null
out=$(run "CREATE PROCEDURE psel AS SELECT id, n FROM pbase")
check "CREATE PROCEDURE works" "procedure 'psel' created" "$out"
out=$(run "CALL psel")
check "CALL runs the stored SELECT" "1   one" "$out"
out=$(run "CALL psel")
check "the stored body is stable across CALLs" "2   two" "$out"
out=$(run "CREATE PROCEDURE pins AS INSERT INTO pbase VALUES (3, 'three'); CALL pins; SELECT COUNT(*) FROM pbase")
check "CALL runs a stored INSERT" "3" "$out"
out=$(run "CALL pins; CALL psel")
check "a CALLed INSERT is visible to a CALLed SELECT" "3   three" "$out"
out=$(run "CREATE PROCEDURE pdel AS DELETE FROM pbase WHERE id = 2; CALL pdel; SELECT COUNT(*) FROM pbase")
check "CALL runs a stored DELETE" "2" "$out"
out=$(run "CREATE PROCEDURE pmk AS CREATE TABLE pgen (x INT); CALL pmk; SELECT * FROM pgen")
check "a procedure body may be DDL" "0 rows" "$out"
out=$(run "CREATE PROCEDURE pbad AS SELECT bogus FROM nosuch; CALL pbad")
check "a failing body errors at CALL time" "unknown table" "$out"
out=$(run "CREATE PROCEDURE prec AS CALL prec; CALL prec")
check "runaway recursion is refused" "depth exceeded" "$out"
out=$(run "CREATE PROCEDURE pbase AS SELECT 1; CALL pbase")
check "a procedure may share a table's name (separate namespaces)" "1" "$out"
out=$(run "DROP PROCEDURE pbase; CREATE PROCEDURE pbase AS SELECT 1")
check "a procedure may share a table's name after dropping its shadow" "created" "$out"
out=$(run "DROP PROCEDURE pbase; SELECT id, n FROM pbase ORDER BY id")
check "a table named like a dropped procedure still works" "1   one" "$out"
out=$(run "CREATE PROCEDURE psel AS SELECT 2")
check "a duplicate procedure name is refused" "already exists" "$out"
out=$(run "CREATE PROCEDURE pbad2 AS")
check "an empty procedure body is refused" "must be a statement" "$out"
out=$(run "CALL nosuchproc")
check "CALL of an unknown procedure is an error" "unknown procedure" "$out"
out=$(run "SELECT id, n FROM pbase ORDER BY id")
check "CALL leaves the database usable" "1   one" "$out"
out=$(run "CALL psel")
check "procedures survive a restart" "3   three" "$out"
out=$(run "VACUUM; CALL psel")
check "procedures survive VACUUM" "1   one" "$out"
out=$(run "DROP PROCEDURE psel; CALL psel")
check "DROP PROCEDURE removes the procedure" "unknown procedure" "$out"
out=$(run "DROP PROCEDURE IF EXISTS psel")
check_not "DROP PROCEDURE IF EXISTS on a missing procedure" "error" "$out"
out=$(run "CREATE PROCEDURE pins2 AS INSERT INTO pbase VALUES (4, 'four'); CALL pins2")
check "a surviving procedure still works" "1 row inserted" "$out"

out=$(run "DROP PROCEDURE psel; DROP PROCEDURE pins; DROP PROCEDURE pdel; DROP PROCEDURE pmk; DROP PROCEDURE pbad; DROP PROCEDURE prec; DROP PROCEDURE pins2")
check "old test procedures can be dropped" "dropped" "$out"

out=$(run "CREATE PROCEDURE padd (@a INT, @b NVARCHAR(20)) AS INSERT INTO pbase VALUES (@a, @b); CALL padd(5, 'five'); SELECT id, n FROM pbase WHERE id = 5")
check "parameters are passed to the body" "5   five" "$out"
out=$(run "CALL padd(1, 'x', 2)")
check "too many arguments are refused" "takes at most 2" "$out"
out=$(run "CREATE PROCEDURE pwhat AS SELECT @x; CALL pwhat")
check "an undeclared variable is an error" "not defined" "$out"
out=$(run "CREATE PROCEDURE pvars AS BEGIN DECLARE @a INT = 1; DECLARE @b NVARCHAR(10); SET @b = 'hi'; SELECT @a, @b; END; CALL pvars")
check "DECLARE and SET work" "1     hi" "$out"
out=$(run "CREATE PROCEDURE pif (@x INT) AS IF @x > 0 SELECT 'pos' ELSE SELECT 'neg'; CALL pif(3); CALL pif(-1)")
check "IF/ELSE branches" "pos" "$out"
out=$(run "CREATE PROCEDURE pwhile AS BEGIN DECLARE @i INT = 10; WHILE @i < 14 BEGIN SET @i = @i + 1; IF @i = 12 CONTINUE; INSERT INTO pbase VALUES (@i, 'w'); END END; CALL pwhile; SELECT id, n FROM pbase WHERE n = 'w' ORDER BY id")
check "WHILE with CONTINUE works" "11  w" "$out"
out=$(run "CREATE PROCEDURE pbrk AS BEGIN DECLARE @i INT = 0; WHILE 1 = 1 BEGIN SET @i = @i + 1; IF @i >= 3 BREAK; END SELECT @i; END; CALL pbrk")
check "BREAK exits the loop" "3" "$out"
out=$(run "CREATE PROCEDURE pret AS BEGIN IF 1 = 0 RETURN; SELECT 'no'; END; CALL pret")
check "RETURN stops the procedure" "no" "$out"
out=$(run "CREATE PROCEDURE pcur AS BEGIN DECLARE @a INT; DECLARE c CURSOR FOR SELECT id FROM pbase WHERE id <= 5 ORDER BY id; OPEN c; FETCH NEXT FROM c INTO @a; WHILE @@fetch_status = 0 BEGIN INSERT INTO pbase VALUES (100 + @a, 'cur'); FETCH NEXT FROM c INTO @a; END CLOSE c; DEALLOCATE c; END; CALL pcur; SELECT COUNT(*) FROM pbase WHERE n = 'cur'")
check "a cursor walks the rows" "4" "$out"
out=$(run "CREATE PROCEDURE pcur2 AS BEGIN DECLARE @a INT; DECLARE c CURSOR FOR SELECT id FROM pbase WHERE id < 0; OPEN c; FETCH NEXT FROM c INTO @a; SELECT @@fetch_status; CLOSE c; END; CALL pcur2")
check "an empty cursor gives fetch status -1" "-1" "$out"
out=$(run "CREATE PROCEDURE pnest AS CALL padd(6, 'six'); CALL pnest; SELECT id, n FROM pbase WHERE id = 6")
check "nested CALLs with parameters work" "6   six" "$out"
out=$(run "CREATE PROCEDURE pbeg AS BEGIN INSERT INTO pbase VALUES (7, 'seven'); INSERT INTO pbase VALUES (8, 'eight'); END; CALL pbeg; SELECT COUNT(*) FROM pbase WHERE n = 'seven' OR n = 'eight'")
check "a BEGIN...END body runs every statement" "2" "$out"
out=$(run "IF 1 = 1 SELECT 1")
check "IF at the top level is refused" "unrecognised statement" "$out"
out=$(run "DECLARE @x INT")
check "DECLARE at the top level is refused" "unrecognised statement" "$out"
out=$(run "CREATE PROCEDURE pgotcha AS BEGIN SELECT CASE WHEN 1 = 1 THEN 'yes' END; END; CALL pgotcha")
check "a CASE expression inside a body works" "yes" "$out"
out=$(run "DROP PROCEDURE pwhat; DROP PROCEDURE pvars; DROP PROCEDURE pif; DROP PROCEDURE pwhile; DROP PROCEDURE pbrk; DROP PROCEDURE pret; DROP PROCEDURE pcur; DROP PROCEDURE pcur2; DROP PROCEDURE pnest; DROP PROCEDURE pbeg; DROP PROCEDURE pgotcha")
check "finished test procedures can be dropped" "dropped" "$out"

out=$(run "CREATE PROCEDURE pdef (@a INT = 5) AS SELECT @a; CALL pdef")
check "a default parameter is used when the argument is omitted" "5" "$out"
out=$(run "CALL pdef(9)")
check "a supplied argument overrides the default" "9" "$out"
out=$(run "CREATE PROCEDURE pboth (@a INT = 2, @b INT = @a * 10) AS SELECT @a, @b; CALL pboth")
check "a default may use earlier parameters" "2     20" "$out"
out=$(run "CALL pboth(@b = 99, @a = 1)")
check "named arguments may arrive in any order" "1     99" "$out"
out=$(run "CREATE PROCEDURE pneed (@a INT) AS SELECT @a; CALL pneed")
check "a missing argument without a default is an error" "expects an argument" "$out"
out=$(run "CALL pneed(@z = 1)")
check "an unknown parameter name is an error" "has no parameter" "$out"
out=$(run "CALL pboth(@a = 1, @a = 2)")
check "a parameter supplied twice is an error" "more than once" "$out"
out=$(run "CREATE PROCEDURE pdbl (@a INT, @b INT OUTPUT) AS SET @b = @a * 2; CREATE PROCEDURE pwo AS BEGIN DECLARE @x INT; CALL pdbl(21, @x OUTPUT); PRINT @x; END; CALL pwo")
check "OUTPUT parameters write back to the caller" "42" "$out"
out=$(run "CALL pdbl(1, 2 OUTPUT)")
check "an OUTPUT argument must be a variable" "must be a variable" "$out"
out=$(run "CREATE PROCEDURE pwo2 AS BEGIN DECLARE @x INT; EXEC pdbl(@a = 8, @b = @x OUTPUT); PRINT @x; END; CALL pwo2")
check "named OUTPUT arguments write back" "16" "$out"
out=$(run "EXEC padd 15, 'fifteen'; SELECT id, n FROM pbase WHERE id = 15")
check "EXEC calls a procedure with bare arguments" "15  fifteen" "$out"
out=$(run "CREATE PROCEDURE prc AS RETURN 42; CREATE PROCEDURE prt AS BEGIN DECLARE @r INT; EXEC @r = prc; PRINT @r; END; CALL prt")
check "EXEC reads the return code" "42" "$out"
out=$(run "PRINT 1 + 2 * 3")
check "PRINT evaluates expressions" "7" "$out"

out=$(run "CREATE TABLE pcap (a INT, b NVARCHAR(20)); CREATE PROCEDURE pinser AS BEGIN SELECT 1, 'one'; SELECT 2, 'two'; END; INSERT INTO pcap EXEC pinser; SELECT COUNT(*) FROM pcap")
check "INSERT ... EXEC captures the first result set" "2" "$out"
out=$(run "CREATE PROCEDURE pinser2 AS BEGIN DECLARE @i INT = 1; WHILE @i < 4 BEGIN SELECT @i, 'x'; SET @i = @i + 1; END END; INSERT INTO pcap EXEC pinser2; SELECT COUNT(*) FROM pcap")
check "a later SELECT in the body is not captured" "3" "$out"

out=$(run "CREATE PROCEDURE pscop AS BEGIN DECLARE @x INT = 3; BEGIN DECLARE @x INT = 2; SELECT @x AS inner_v; END; SELECT @x AS outer_v; END; CALL pscop")
check "a block-local DECLARE shadows the outer variable" "inner_v" "$out"
check "the outer variable is restored after the block" "3" "$out"
out=$(run "CREATE PROCEDURE pdup AS BEGIN DECLARE @x INT; DECLARE @x INT; END; CALL pdup")
check "re-declaring a variable in the same scope is refused" "already declared" "$out"
run "DROP PROCEDURE pscop; DROP PROCEDURE pdup" >/dev/null

out=$(run "CREATE PROCEDURE perr AS BEGIN DECLARE @x INT; SET @x = 1 / 0; END; BEGIN TRY EXEC perr; SELECT 'unreached'; END TRY BEGIN CATCH SELECT CONCAT('caught: ', @@error_message) AS m; END CATCH")
check "TRY/CATCH catches a body error" "caught: division by zero" "$out"
check_not "TRY/CATCH skips the rest of the TRY block" "unreached" "$out"
out=$(run "BEGIN TRY SET @x = 1 / 0; END TRY BEGIN CATCH SELECT 'topcatch'; END CATCH")
check "TRY/CATCH works at the top level" "topcatch" "$out"
out=$(run "BEGIN TRY EXEC ('SELECT * FROM nosuch'); END TRY BEGIN CATCH SELECT 'dyncaught'; END CATCH")
check "a dynamic SQL failure is catchable" "dyncaught" "$out"

out=$(run "CREATE PROCEDURE pdyn AS BEGIN DECLARE @q NVARCHAR(100); SET @q = 'SELECT 42 AS v'; EXEC (@q); END; CALL pdyn")
check "EXEC ('...') runs dynamic SQL in the caller frame" "42" "$out"
out=$(run "CREATE PROCEDURE pdyn2 AS BEGIN DECLARE @r INT; EXEC @r = ('SELECT 7'); SELECT @r AS v; END; CALL pdyn2")
check "EXEC @rc = ('...') returns the dynamic SELECT value" "7" "$out"

out=$(run "CREATE PROCEDURE pcache AS SELECT 1; CALL pcache; DROP PROCEDURE pcache; CREATE PROCEDURE pcache AS SELECT 2; CALL pcache")
check "the parsed body is refreshed after DROP + CREATE" "2" "$out"

out=$(run "CREATE PROCEDURE pdeep (@i INT) AS BEGIN IF @i < 40 CALL pdeep(@i + 1); END; CALL pdeep(0); SELECT 'deep-ok'")
check "recursion can nest past 32 levels" "deep-ok" "$out"

rm -f "$DD" "$FN"

echo
printf '%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
