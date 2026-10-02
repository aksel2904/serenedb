-- ts_regexp forms, count(*). On w before part 1 the result is wrong
-- (ts_regexp on a wildcard field is matched against the gram dictionary):
-- compare timings only after part 1.
-- One query per line, preceded by `-- <id> | <form> | <target>`.
-- Targets: w = logs_w (wildcard n-gram field), k = logs_k (verbatim
-- keyword field), t = table logs without index.

-- rx_contains_rare_w | contains | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*OutOfMemoryError.*');
-- rx_contains_rare_k | contains | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*OutOfMemoryError.*');
-- rx_contains_rare_t | contains | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*OutOfMemoryError.*');

-- rx_contains_mid_w | contains | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*');
-- rx_contains_mid_k | contains | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*timed out.*');
-- rx_contains_mid_t | contains | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*timed out.*');

-- rx_contains_freq_w | contains | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*');
-- rx_contains_freq_k | contains | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*/api/v2/.*');
-- rx_contains_freq_t | contains | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*/api/v2/.*');

-- rx_prefix_sel_w | prefix | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('connection to db-.*');
-- rx_prefix_sel_k | prefix | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('connection to db-.*');
-- rx_prefix_sel_t | prefix | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, 'connection to db-.*');

-- rx_prefix_freq_w | prefix | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*');
-- rx_prefix_freq_k | prefix | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('GET /api/.*');
-- rx_prefix_freq_t | prefix | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, 'GET /api/.*');

-- rx_suffix_sel_w | suffix | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*card declined');
-- rx_suffix_sel_k | suffix | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*card declined');
-- rx_suffix_sel_t | suffix | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*card declined');

-- rx_suffix_freq_w | suffix | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*ms');
-- rx_suffix_freq_k | suffix | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*ms');
-- rx_suffix_freq_t | suffix | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*ms');

-- rx_under_prefix_w | under | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('disk usage 9. pct on .*');
-- rx_under_prefix_k | under | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('disk usage 9. pct on .*');
-- rx_under_prefix_t | under | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, 'disk usage 9. pct on .*');

-- rx_under_suffix_w | under | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*/dev/sd.3');
-- rx_under_suffix_k | under | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*/dev/sd.3');
-- rx_under_suffix_t | under | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*/dev/sd.3');

-- rx_multi_sel_w | multi | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*connection.*db-.*timed out.*');
-- rx_multi_sel_k | multi | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*connection.*db-.*timed out.*');
-- rx_multi_sel_t | multi | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*connection.*db-.*timed out.*');

-- rx_multi_heavy_w | multi | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*');
-- rx_multi_heavy_k | multi | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*');
-- rx_multi_heavy_t | multi | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, 'GET /api/.*/orders/.* 500 .*');

-- rx_seg_w | seg | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('java\.lang\.OutOfMemoryError: Java heap space');
-- rx_seg_k | seg | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('java\.lang\.OutOfMemoryError: Java heap space');
-- rx_seg_t | seg | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, 'java\.lang\.OutOfMemoryError: Java heap space');

-- rx_alt_w | alt | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*(timed out|card declined).*');
-- rx_alt_k | alt | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*(timed out|card declined).*');
-- rx_alt_t | alt | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*(timed out|card declined).*');

-- rx_class_w | class | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('GET /api/v[12]/orders/[0-9]+ 5[0-9][0-9] [0-9]+ms');
-- rx_class_k | class | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('GET /api/v[12]/orders/[0-9]+ 5[0-9][0-9] [0-9]+ms');
-- rx_class_t | class | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, 'GET /api/v[12]/orders/[0-9]+ 5[0-9][0-9] [0-9]+ms');

-- rx_ci_w | ci | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('(?i).*outofmemoryerror.*');
-- rx_ci_k | ci | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('(?i).*outofmemoryerror.*');
-- rx_ci_t | ci | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '(?i).*outofmemoryerror.*');

-- rx_nolit_w | nolit | w
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+.*');
-- rx_nolit_k | nolit | k
SELECT count(*) FROM logs_k WHERE msg @@ ts_regexp('.*[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+.*');
-- rx_nolit_t | nolit | t
SELECT count(*) FROM logs WHERE regexp_full_match(msg, '.*[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+.*');
