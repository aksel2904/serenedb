-- n-gram LIKE / regex benchmark: schema and load.
-- Run with psql from the directory holding the CSV:
--   psql -h 127.0.0.1 -p 7931 -U postgres -d postgres -f schema.sql
\pset pager off
\timing on
\set ON_ERROR_STOP on

-- Wildcard n-gram field over the whole message (keyword() inner tokenizer):
-- one term per row, so ts_like / ts_regexp have the same whole-value
-- semantics as LIKE / regexp_full_match on the table.
CREATE TEXT SEARCH DICTIONARY bench_w3 AS generate_wildcard_ngrams(keyword(), 3);

CREATE TABLE logs(id BIGINT PRIMARY KEY, x INTEGER NOT NULL, msg VARCHAR NOT NULL);

\copy logs FROM 'logs_1m_s42.csv' WITH (FORMAT csv, HEADER true)

-- Indexes after the load (docs: build then index). Background compaction off,
-- one manual compaction below, so the segment layout is fixed for all runs.
-- x is INCLUDEd (stored, not indexed): `x > T` can only be a column filter.
CREATE INDEX logs_w ON logs USING inverted(msg bench_w3) INCLUDE (x)
  WITH (compaction_interval = 0);
CREATE INDEX logs_k ON logs USING inverted(msg) INCLUDE (x)
  WITH (compaction_interval = 0);

VACUUM (REFRESH_TABLE) logs;
VACUUM (COMPACT_INDEX) logs_w;
VACUUM (COMPACT_INDEX) logs_k;

SELECT count(*) AS rows, sum(length(msg)) AS msg_chars FROM logs;

SELECT c.relname, m.metric, m.value
FROM sdb_metrics m JOIN pg_class c ON c.oid = m.relation_id
WHERE c.relname IN ('logs_w', 'logs_k')
  AND m.metric IN ('num_docs', 'num_live_docs', 'num_segments', 'num_files', 'index_size')
ORDER BY c.relname, m.metric;
