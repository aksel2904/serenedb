-- Predicate AND x > T on logs_w, T = 98/89/49/9 -> about 1/10/50/90% of rows;
-- xall = no x predicate. Forms: count, stream (sum(x)), topk (BM25, LIMIT 10).
-- s_rx_* only after part 1 (ts_regexp on w is wrong before it).
-- One query per line, preceded by `-- <id> | <form> | <selectivity>`.

-- s_like_mid_xall_count | count | xall
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%timed out%');
-- s_like_mid_xall_stream | stream | xall
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%timed out%');
-- s_like_mid_xall_topk | topk | xall
SELECT x FROM logs_w WHERE msg @@ ts_like('%timed out%') ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_mid_x1_count | count | x1
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 98;
-- s_like_mid_x1_stream | stream | x1
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 98;
-- s_like_mid_x1_topk | topk | x1
SELECT x FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 98 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_mid_x10_count | count | x10
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 89;
-- s_like_mid_x10_stream | stream | x10
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 89;
-- s_like_mid_x10_topk | topk | x10
SELECT x FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 89 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_mid_x50_count | count | x50
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 49;
-- s_like_mid_x50_stream | stream | x50
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 49;
-- s_like_mid_x50_topk | topk | x50
SELECT x FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 49 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_mid_x90_count | count | x90
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 9;
-- s_like_mid_x90_stream | stream | x90
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 9;
-- s_like_mid_x90_topk | topk | x90
SELECT x FROM logs_w WHERE msg @@ ts_like('%timed out%') AND x > 9 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_freq_xall_count | count | xall
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%');
-- s_like_freq_xall_stream | stream | xall
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%');
-- s_like_freq_xall_topk | topk | xall
SELECT x FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_freq_x1_count | count | x1
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 98;
-- s_like_freq_x1_stream | stream | x1
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 98;
-- s_like_freq_x1_topk | topk | x1
SELECT x FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 98 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_freq_x10_count | count | x10
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 89;
-- s_like_freq_x10_stream | stream | x10
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 89;
-- s_like_freq_x10_topk | topk | x10
SELECT x FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 89 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_freq_x50_count | count | x50
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 49;
-- s_like_freq_x50_stream | stream | x50
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 49;
-- s_like_freq_x50_topk | topk | x50
SELECT x FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 49 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_freq_x90_count | count | x90
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 9;
-- s_like_freq_x90_stream | stream | x90
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 9;
-- s_like_freq_x90_topk | topk | x90
SELECT x FROM logs_w WHERE msg @@ ts_like('%/api/v2/%') AND x > 9 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_heavy_xall_count | count | xall
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %');
-- s_like_heavy_xall_stream | stream | xall
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %');
-- s_like_heavy_xall_topk | topk | xall
SELECT x FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_heavy_x1_count | count | x1
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 98;
-- s_like_heavy_x1_stream | stream | x1
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 98;
-- s_like_heavy_x1_topk | topk | x1
SELECT x FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 98 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_heavy_x10_count | count | x10
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 89;
-- s_like_heavy_x10_stream | stream | x10
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 89;
-- s_like_heavy_x10_topk | topk | x10
SELECT x FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 89 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_heavy_x50_count | count | x50
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 49;
-- s_like_heavy_x50_stream | stream | x50
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 49;
-- s_like_heavy_x50_topk | topk | x50
SELECT x FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 49 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_like_heavy_x90_count | count | x90
SELECT count(*) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 9;
-- s_like_heavy_x90_stream | stream | x90
SELECT sum(x) FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 9;
-- s_like_heavy_x90_topk | topk | x90
SELECT x FROM logs_w WHERE msg @@ ts_like('GET /api/%/orders/% 500 %') AND x > 9 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_mid_xall_count | count | xall
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*');
-- s_rx_mid_xall_stream | stream | xall
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*');
-- s_rx_mid_xall_topk | topk | xall
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_mid_x1_count | count | x1
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 98;
-- s_rx_mid_x1_stream | stream | x1
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 98;
-- s_rx_mid_x1_topk | topk | x1
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 98 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_mid_x10_count | count | x10
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 89;
-- s_rx_mid_x10_stream | stream | x10
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 89;
-- s_rx_mid_x10_topk | topk | x10
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 89 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_mid_x50_count | count | x50
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 49;
-- s_rx_mid_x50_stream | stream | x50
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 49;
-- s_rx_mid_x50_topk | topk | x50
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 49 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_mid_x90_count | count | x90
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 9;
-- s_rx_mid_x90_stream | stream | x90
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 9;
-- s_rx_mid_x90_topk | topk | x90
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*timed out.*') AND x > 9 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_freq_xall_count | count | xall
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*');
-- s_rx_freq_xall_stream | stream | xall
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*');
-- s_rx_freq_xall_topk | topk | xall
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_freq_x1_count | count | x1
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 98;
-- s_rx_freq_x1_stream | stream | x1
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 98;
-- s_rx_freq_x1_topk | topk | x1
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 98 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_freq_x10_count | count | x10
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 89;
-- s_rx_freq_x10_stream | stream | x10
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 89;
-- s_rx_freq_x10_topk | topk | x10
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 89 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_freq_x50_count | count | x50
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 49;
-- s_rx_freq_x50_stream | stream | x50
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 49;
-- s_rx_freq_x50_topk | topk | x50
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 49 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_freq_x90_count | count | x90
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 9;
-- s_rx_freq_x90_stream | stream | x90
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 9;
-- s_rx_freq_x90_topk | topk | x90
SELECT x FROM logs_w WHERE msg @@ ts_regexp('.*/api/v2/.*') AND x > 9 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_heavy_xall_count | count | xall
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*');
-- s_rx_heavy_xall_stream | stream | xall
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*');
-- s_rx_heavy_xall_topk | topk | xall
SELECT x FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_heavy_x1_count | count | x1
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 98;
-- s_rx_heavy_x1_stream | stream | x1
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 98;
-- s_rx_heavy_x1_topk | topk | x1
SELECT x FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 98 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_heavy_x10_count | count | x10
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 89;
-- s_rx_heavy_x10_stream | stream | x10
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 89;
-- s_rx_heavy_x10_topk | topk | x10
SELECT x FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 89 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_heavy_x50_count | count | x50
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 49;
-- s_rx_heavy_x50_stream | stream | x50
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 49;
-- s_rx_heavy_x50_topk | topk | x50
SELECT x FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 49 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;

-- s_rx_heavy_x90_count | count | x90
SELECT count(*) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 9;
-- s_rx_heavy_x90_stream | stream | x90
SELECT sum(x) FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 9;
-- s_rx_heavy_x90_topk | topk | x90
SELECT x FROM logs_w WHERE msg @@ ts_regexp('GET /api/.*/orders/.* 500 .*') AND x > 9 ORDER BY BM25(logs_w.tableoid) DESC LIMIT 10;
