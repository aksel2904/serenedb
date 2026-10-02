-- ts_like forms, count(*). One query per line, preceded by `-- <id> | <form> | <target>`.
-- Targets: w = logs_w (wildcard n-gram field), k = logs_k (verbatim
-- keyword field), t = table logs without index.

-- like_contains_rare_w | contains | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%OutOfMemoryError%');
-- like_contains_rare_k | contains | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('%OutOfMemoryError%');
-- like_contains_rare_t | contains | t
SELECT count(*) FROM logs WHERE msg LIKE '%OutOfMemoryError%';

-- like_contains_mid_w | contains | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%timed out%');
-- like_contains_mid_k | contains | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('%timed out%');
-- like_contains_mid_t | contains | t
SELECT count(*) FROM logs WHERE msg LIKE '%timed out%';

-- like_contains_freq_w | contains | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%');
-- like_contains_freq_k | contains | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('%/api/v2/%');
-- like_contains_freq_t | contains | t
SELECT count(*) FROM logs WHERE msg LIKE '%/api/v2/%';

-- like_prefix_sel_w | prefix | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('connection to db-%');
-- like_prefix_sel_k | prefix | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('connection to db-%');
-- like_prefix_sel_t | prefix | t
SELECT count(*) FROM logs WHERE msg LIKE 'connection to db-%';

-- like_prefix_freq_w | prefix | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('GET /api/%');
-- like_prefix_freq_k | prefix | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('GET /api/%');
-- like_prefix_freq_t | prefix | t
SELECT count(*) FROM logs WHERE msg LIKE 'GET /api/%';

-- like_suffix_sel_w | suffix | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%card declined');
-- like_suffix_sel_k | suffix | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('%card declined');
-- like_suffix_sel_t | suffix | t
SELECT count(*) FROM logs WHERE msg LIKE '%card declined';

-- like_suffix_freq_w | suffix | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%ms');
-- like_suffix_freq_k | suffix | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('%ms');
-- like_suffix_freq_t | suffix | t
SELECT count(*) FROM logs WHERE msg LIKE '%ms';

-- like_under_prefix_w | under | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('disk usage 9_ pct on %');
-- like_under_prefix_k | under | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('disk usage 9_ pct on %');
-- like_under_prefix_t | under | t
SELECT count(*) FROM logs WHERE msg LIKE 'disk usage 9_ pct on %';

-- like_under_suffix_w | under | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%/dev/sd_3');
-- like_under_suffix_k | under | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('%/dev/sd_3');
-- like_under_suffix_t | under | t
SELECT count(*) FROM logs WHERE msg LIKE '%/dev/sd_3';

-- like_multi_sel_w | multi | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%connection%db-%timed out%');
-- like_multi_sel_k | multi | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('%connection%db-%timed out%');
-- like_multi_sel_t | multi | t
SELECT count(*) FROM logs WHERE msg LIKE '%connection%db-%timed out%';

-- like_multi_heavy_w | multi | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %');
-- like_multi_heavy_k | multi | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('GET /api/%/orders/% 500 %');
-- like_multi_heavy_t | multi | t
SELECT count(*) FROM logs WHERE msg LIKE 'GET /api/%/orders/% 500 %';

-- like_seg_w | seg | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('java.lang.OutOfMemoryError: Java heap space');
-- like_seg_k | seg | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_like('java.lang.OutOfMemoryError: Java heap space');
-- like_seg_t | seg | t
SELECT count(*) FROM logs WHERE msg LIKE 'java.lang.OutOfMemoryError: Java heap space';
