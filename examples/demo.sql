-- demo.sql - a five minute tour of nexdb.
--
--   make
--   ./build/nexdb demo.ndb -f examples/demo.sql
--
-- Safe to run more than once: it starts by clearing out its own table.

PRINT '=== 1. it is an ordinary database ===';
GO

DROP TABLE IF EXISTS notes
GO

CREATE TABLE notes (
    id      INT PRIMARY KEY,
    topic   NVARCHAR(50),
    body    NVARCHAR(400),
    created DATETIME
)
GO

INSERT INTO notes (id, topic, body, created) VALUES
  (1, 'coffee',  'the good beans come from the roaster on 5th',     '2026-07-01'),
  (2, 'coffee',  'grind finer for the aeropress, 30 seconds',       '2026-07-02'),
  (3, 'coffee',  'oat milk froths better than almond',              '2026-07-03'),
  (4, 'taxes',   'quarterly estimate due january 15',               '2026-07-04'),
  (5, 'taxes',   'the accountant is marta, she answers on fridays', '2026-07-05'),
  (6, 'dog',     'buddy sees dr chen at riverside vet',             '2026-07-06'),
  (7, 'dog',     'buddy hates the blue shampoo, use the green one', '2026-07-07'),
  (8, 'car',     'tyres need replacing before winter',              '2026-07-08')
GO

-- Everything you would expect from T-SQL works.
SELECT TOP 3 id, topic, body FROM notes WHERE topic = 'coffee'
GO

SELECT COUNT(*) AS total FROM notes
GO

SELECT id, body FROM notes WHERE body LIKE '%buddy%'
GO

UPDATE notes SET body = 'buddy sees dr chen at riverside vet, tuesdays' WHERE id = 6
GO


PRINT '';
PRINT '=== 2. every row keeps a memory of being used ===';
GO

-- Read the coffee notes a few times, the way you would over a real week.
SELECT * FROM notes WHERE topic = 'coffee'
GO
SELECT * FROM notes WHERE topic = 'coffee'
GO
SELECT * FROM notes WHERE topic = 'coffee'
GO
SELECT * FROM notes WHERE topic = 'coffee'
GO

-- Look up one dog note once.
SELECT * FROM notes WHERE id = 7
GO

-- Now ask what the database thinks matters. Nobody set a priority column;
-- this ranking is purely a side effect of how the data was used.
PRINT '';
PRINT 'what this database now considers important:';
GO
SHOW MEMORY FROM notes
GO


PRINT '';
PRINT '=== 3. rows used together become associated ===';
GO

-- The three coffee rows kept coming back in the same result set, so they are
-- now wired to each other. This is Hebbian learning: fire together, wire
-- together.
SHOW LINKS TOP 8
GO


PRINT '';
PRINT '=== 4. RECALL is fuzzy and association-aware ===';
GO

-- You do not remember the exact wording, only roughly what it was about.
RECALL 'how do I make the coffee taste right'
GO

-- Recall does not need the topic name, just a word that appears anywhere.
RECALL 'shampoo'
GO

-- Ask about something the database has never heard of and it says so, rather
-- than returning its best guess as though it were an answer.
RECALL 'submarine maintenance'
GO


PRINT '';
PRINT '=== 5. you can steer the memory by hand ===';
GO

-- Something important that you rarely look at: pin it up.
REMEMBER FROM notes WHERE id = 4
GO

-- Something you want to stop surfacing: let it go. FORGET zeroes the strength
-- but keeps the row, so nothing is lost.
FORGET FROM notes WHERE id = 8
GO

SELECT id, topic, _strength, _access FROM notes ORDER BY _strength DESC
GO


PRINT '';
PRINT '=== 6. memory fades on its own ===';
GO

-- Strength halves every seven days of neglect. To watch a month pass without
-- waiting, run the shell with an offset clock and observer mode:
--
--   NEXDB_TIME_OFFSET=$((28*86400)) ./build/nexdb demo.ndb -r \
--       -c "SELECT id, _strength FROM notes ORDER BY _strength DESC"
--
-- The -r flag means the query does not reinforce what it reads, so you can
-- inspect the memory without changing it.

CHECKPOINT
GO

PRINT 'done. try the interactive shell:  ./build/nexdb demo.ndb';
GO
