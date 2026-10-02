-- Part 2 checks on the benchmark dataset (schema.sql): OR, NOT and two
-- predicates in AND, which the bench query files do not cover. Same format as
-- queries_*.sql: `-- <id> | <form> | <target>` then one query line.
-- Targets: w = logs_w, t = table logs without index.

-- o_like_or_w | or | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%timed out%') OR msg @@ ts_like('%card declined');
-- o_like_or_t | or | t
SELECT count(*) FROM logs WHERE msg LIKE '%timed out%' OR msg LIKE '%card declined';

-- o_like_not_w | not | w
SELECT count(*) FROM logs_w WHERE NOT (msg @@ ts_like('%timed out%'));
-- o_like_not_t | not | t
SELECT count(*) FROM logs WHERE NOT (msg LIKE '%timed out%');

-- o_like_and2_w | and2 | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND msg @@ ts_like('connection to db-%');
-- o_like_and2_t | and2 | t
SELECT count(*) FROM logs WHERE msg LIKE '%timed out%' AND msg LIKE 'connection to db-%';

-- o_rx_or_w | or | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') OR msg @@ ts_regexp('.*card declined');
-- o_rx_or_t | or | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*timed out.*') OR regexp_full_match(msg, '.*card declined');

-- o_rx_not_w | not | w
SELECT count(*) FROM logs_w WHERE NOT (msg @@ ts_regexp('.*timed out.*'));
-- o_rx_not_t | not | t
SELECT count(*) FROM logs WHERE NOT regexp_full_match(msg, '.*timed out.*');

-- o_rx_and2_w | and2 | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND msg @@ ts_like('connection to db-%');
-- o_rx_and2_t | and2 | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*timed out.*') AND msg LIKE 'connection to db-%';
