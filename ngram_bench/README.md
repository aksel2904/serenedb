# n-gram LIKE / regex benchmark

Dataset generator, schema, queries and run scripts used to measure `ts_like` and `ts_regexp` on `generate_wildcard_ngrams` fields in SereneDB:

- **part 1**: `ts_regexp` on a wildcard n-gram field answered from the n-gram index. Literals required by the regex are extracted, looked up as grams, and every candidate is checked with the regex (serenedb/serenedb#1294, PR #1301);
- **part 2**: the per-candidate check of top-level `ts_like` / `ts_regexp` moved out of the index iterator into a table-filter step that reads the stored terms column by scan windows instead of one point read per candidate (serenedb/serenedb#1293, PR #1308);
- **`(?i)` extension**: case-insensitive ASCII letters in the literal extraction of part 1 (PR #1309).

This directory holds no results.

## Measured commits

The benchmark does not depend on the code of the branch it is committed on. The scripts start a given `serened` binary and talk to it over the PostgreSQL protocol, so any commit can be measured. These commits were measured:

| Commit | What | Role in the runs |
|---|---|---|
| `e4581109ff83d885612417ae1c9decd064002f64` | `main` after #1286 | A in the A/B/C run; builds its datadir; profile of the base, `p2_step0/` |
| `0e41b90f599d0e0ba7083ce6d2374b7a9d5db22c` | part 1 on top of `e4581109f` (PR #1301) | B in the A/B/C run |
| `5315d924384be41c0ef67d9cb872a85f079ca597` | part 2 on top of `0e41b90f5` | C in the A/B/C run; profile of part 2 |
| `96ab0bfc845a3280a1f4e8e8c1caee67696e8888` | `main` after #1303 | A in the PR #1308 and PR #1309 runs; builds their datadir |
| `ae9e83aa586e4017323ba7ed93475416fc022240` | part 2 ported onto `96ab0bfc8` (PR #1308) | B in the PR #1308 run |
| `8b1dbdf9f3075648b442f83e255212df49d1d840` | `(?i)` extension on `96ab0bfc8` (first commit of PR #1309) | B in the PR #1309 run |

`a386abb1ae4c4bf05e1efaa42d1ace19781339d8`, the parent of the commit that adds this directory, is `5315d9243` plus one fix; it was not timed.

## Build

Every binary is built the same way, `serened` only:

```bash
git checkout <commit> && git submodule update --init --recursive
cmake --preset perf -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
      -DSDB_EMBEDDED_DOCS=OFF -DUSE_DEBUG_INFO=COLUMNS
cd build_perf && ninja serened
cp bin/serened <ROOT>/bin/serened-<first 9 characters of the commit>-perf
```

`pr2.sh` and `night.sh` look for binaries under that name.

This is the CI `perf` configuration:
- `RelWithDebInfo` (`-O3`) with frame pointers;
- no ThinLTO;
- line and column debug info;
- static, jemalloc, no asserts.

The release package differs: `Release` without frame pointers, ThinLTO, no debug info. The instruction set is the same default `haswell()` in both (AVX2, no AVX-512).

Machine used: KVM VM, 8 vCPUs (4 cores x 2 threads, Intel Icelake), 31 GiB RAM, Ubuntu 24.04, clang 21.1.8, lld 21. On this VM logical CPUs 2k and 2k+1 are the two threads of core k; the pinning below assumes that layout.

## Dataset

```bash
cd <data dir>
python3 <repo>/ngram_bench/gen_logs.py --rows 1000000 --seed 42 --out logs_1m_s42.csv 2> gen.log
```

- **Rows.** 1,000,000 rows `id,x,msg`. `msg` is a log message body from 15 templates (GET/POST lines, logins, timeouts, disk usage, Java exceptions, a Cyrillic template, ...); `x` is uniform in [0, 100) and independent of `msg`.
- **Determinism.** The output is deterministic: with Python 3.12.3 the CSV is 52,329,092 bytes, sha256 `f29f5f6fa8eca0bc754ff2406c93decd03991467374770f65845cb3cd13c7279`. `gen.log` prints the sha256 and the row count per template.
- **Load.** Start a server on an empty data directory (listen on localhost only), then load from the directory that holds the CSV:

  ```bash
  serened <datadir> --listen='postgres://127.0.0.1:7931' &
  psql -h 127.0.0.1 -p 7931 -U postgres -d postgres -f <repo>/ngram_bench/schema.sql
  ```

  `schema.sql` creates:
  - the table `logs`;
  - the wildcard index `logs_w`: `generate_wildcard_ngrams(keyword(), 3)`, no positions, so every candidate is checked;
  - the verbatim index `logs_k`.

  `x` is `INCLUDE`d in both, background compaction is disabled, and one manual `VACUUM (COMPACT_INDEX)` leaves one segment per index.
- **One datadir per comparison.** Build the datadir with binary A and run every binary of the comparison on that same directory: a copy reads differently when the cache is cold. `pr2.sh data` does this for the runs on `96ab0bfc8`.

## Queries

One query per line, preceded by `-- <id> | <form> | <target>`. Targets:
- `w`: the wildcard field `logs_w`;
- `k`: the verbatim field `logs_k`;
- `t`: the table `logs` without an index (`LIKE`, `regexp_full_match`).

| File | Contents |
|---|---|
| `queries_like.sql` | 12 `ts_like` forms x 3 targets, `count(*)` |
| `queries_regexp.sql` | 16 `ts_regexp` forms x 3 targets, `count(*)` (on `e4581109f`, `ts_regexp` on `w` returns wrong answers) |
| `queries_selectivity.sql` | `ts_like` / `ts_regexp` `AND x > T` on `w`, T = 98/89/49/9 (about 1/10/50/90% of rows) and no `x` predicate (`xall`); forms count, stream (`sum(x)`), top-k (`ORDER BY BM25(...) DESC LIMIT 10`) |
| `part2_bench_checks.sql` | OR, NOT and two predicates in AND on `w` and `t`, for answer and EXPLAIN checks only |

Forms: contains, prefix, suffix, `_`, multi-segment, seg. **seg** here means an exact whole-term match, a pattern without `%` or `_`; this is our definition. The forms are named after the `q_ngl_*` rows of #1286, but its dataset is lost, so the numbers are not comparable with #1286 or #1293.

## Methodology (`ab_run.sh`)

Following #1286:
- every binary runs on the same datadir;
- warm cache: 4 interleaved rounds; in each round a fresh server per binary, then per query one untimed warm-up and 5 timed runs;
- cold cache: 8 rounds; per query and round: stop the server, `sync`, `vmtouch -e <datadir>`, start a fresh server, one timed run. `vmtouch <datadir>` after the eviction writes the resident pages to `run.log`.

Where #1286 does not say how, the choices are ours:
- **Statistic.** Warm: the median over rounds of the per-round minimum, with the min..max of those minima. Cold: the median over rounds.
- **Binary order.** It rotates every round: A B C, B C A, C A B, ... (A B, B A with two binaries).
- **Threads.** The server flag `--cpu_threads=1|8`; after every start the script checks `current_setting('threads')`. A session `SET threads` is not used.
- **Pinning.**
  - The `psql` client runs on CPU 0.
  - t1: `--cpu_threads=1`, server on CPUs 2,3 (one core).
  - t8: `--cpu_threads=8`, server on CPUs 0-7.

  With the server pinned to CPUs 2,3, jemalloc reports "Number of CPUs detected is not deterministic. Per-CPU arena disabled."; it does not with 0-7. A/B comparisons are unaffected, but t1/t8 ratios are not a clean scaling measure.
- **Cold subset.** Cold runs cover a few queries only (`COLD_IDS`, 10 by default), since each one restarts the server.
- **Timer.** `psql \timing`, client-side.
- **Load.** The load average is logged at every server start. Nothing else (builds, tests) runs on the machine during a run.

Answers and plans are checked before timing (`check_binary.sh`, `compare_checks.py`, below).

## Running

Linux only. Tools: `python3`, `psql`, `taskset`, `vmtouch` (cold cache), `sha256sum`, `ss` (`night.sh`), `perf` and `bpftrace` (profiles only). All scripts take `NAME=value` arguments; `PG_PORT` defaults to 7931 (`p2_step0/run.sh` uses 7935). Paths below are relative to the repository root.

### Answers and plans

```bash
ngram_bench/check_binary.sh BIN=<A serened> OUT=checks/a DATADIR=<datadir> \
  FILES="queries_like.sql queries_regexp.sql queries_selectivity.sql part2_bench_checks.sql"
ngram_bench/check_binary.sh BIN=<B serened> OUT=checks/b DATADIR=<datadir> FILES="..."
python3 ngram_bench/compare_checks.py checks/a checks/b
```

- `check_binary.sh` starts a server with default threads, runs every query once (with `\timing`) and once with `EXPLAIN`, then stops the server.
- `compare_checks.py` compares B with A on every query (top-k: number of rows), `w` and `k` with `t`, `s_rx_*` with `s_like_*`, and the plans without `Verify` lines; it prints the `Verify` values per query group, ends with "no problems" or exits with 1 on any mismatch. `--allow-plan-diff=<ids>` lets the listed plans differ; `--show-plan=<ids>` prints them. On `e4581109f`, `ts_regexp` on `w` gives wrong answers, so every comparison that includes it reports them.

### Timing run

```bash
ngram_bench/ab_run.sh A=<serened> B=<serened> [C=<serened>] DATADIR=<datadir> OUT=<run dir>
python3 ngram_bench/summarize.py <run dir> > <run dir>/summary.tsv
```

- `DRY_RUN=1` prints the plan without starting servers.
- Other parameters: `WARM_ROUNDS` (4), `REPS` (5), `COLD_ROUNDS` (8), `THREADS` ("1 8"), `WARM_FILES` (the three query files), `WARM_RE` (ERE on warm query ids), `COLD_IDS`, `ONLY`.
- Output: `raw.tsv` (one line per timed execution), `results.tsv` (first answer per batch), `run.log` (server starts with load average, cold-cache residency), `server.log`.
- `summarize.py` prints the statistics above with deltas against A, the rows where a variant is slower than A (`apart` when every value of the variant is above every value of A), and the queries whose answer differs from A's. It uses every row of `raw.tsv`; to leave a batch out, copy the run directory and drop its rows from `raw.tsv` first.

### The runs

- **A/B/C** (`e4581109f`, `0e41b90f5`, `5315d9243`) on the datadir built by `e4581109f`, all defaults:

  ```bash
  ngram_bench/ab_run.sh A=<e4581109f> B=<0e41b90f5> C=<5315d9243> DATADIR=<datadir> OUT=<run dir>
  ```

- **PR #1308** (`96ab0bfc8` against `ae9e83aa5`), `pr2.sh` with `ROOT` holding `bin/`:

  ```bash
  ngram_bench/pr2.sh data  ROOT=<root> MAIN=96ab0bfc8...
  ngram_bench/pr2.sh check ROOT=<root> MAIN=96ab0bfc8... PORT=ae9e83aa5...
  ngram_bench/pr2.sh run   ROOT=<root> MAIN=96ab0bfc8... PORT=ae9e83aa5...
  ```

  `data` generates the CSV and builds `<root>/pr2/datadir` with the `main` binary. `check` runs `check_binary.sh` on both binaries and `compare_checks.py`; `run` refuses to start unless that ended with "no problems". `run` is `ab_run.sh` on 119 warm queries (`WARM_RE`) and 6 cold ones (`COLD_IDS`), the rest by default.
- **PR #1309** (`96ab0bfc8` against `8b1dbdf9f`) on the same datadir, warm cache only:

  ```bash
  ngram_bench/check_binary.sh BIN=<8b1dbdf9f> OUT=<root>/casefold/checks/case DATADIR=<root>/pr2/datadir FILES="..."
  python3 ngram_bench/compare_checks.py <root>/pr2/checks/main <root>/casefold/checks/case \
    --allow-plan-diff=rx_ci_w --show-plan=rx_ci_w
  ngram_bench/ab_run.sh A=<96ab0bfc8> B=<8b1dbdf9f> DATADIR=<root>/pr2/datadir OUT=<run dir> \
    WARM_FILES=queries_regexp.sql WARM_RE='^rx_(ci|contains_rare)_(w|k|t)$' COLD_ROUNDS=0
  ```

- **`night.sh`** chains the last two unattended: incremental perf builds of `8b1dbdf9f` and `ae9e83aa5` in a worktree (`WT`) with a configured `build_perf`, the checks and the runs above. It stops with `STOP: <reason>` in its log on any error or mismatch and ends with `DONE`; `FROM=<step>` resumes. Run `pr2.sh data` and `pr2.sh check` with `MAIN` only first.

  ```bash
  ngram_bench/night.sh ROOT=<root> WT=<worktree> CASE=8b1dbdf9f... PORT=ae9e83aa5...
  ```

### Profiles

- **`profile.sh PID=<serened pid> OUT=<dir> [QIDS=...]`**: on a running server, per query: 3 warm-up runs, 10 timed runs, then `sudo perf record -e cpu-clock -F 4000 --call-graph fp -p <pid>` while the query repeats for at least 3 s. Profiles were taken at t1 (`--cpu_threads=1`, server on CPUs 2,3) with a warm cache.
- **`perf_shares.sh BIN=<serened> PID=<pid> DIR=<profile dir>`**: inclusive shares of `irs::ColumnReader::PointReader::FetchRow` and `irs::LikeMatcher::Match`.
  - `serened` copies its machine code into anonymous huge-page memory at startup (`--remap_executable`, on by default), so `perf` cannot symbolize it from the file.
  - The binary is not PIE, so the script builds `/tmp/perf-<pid>.map` from `nm` and the samples resolve. Running the server with `--remap_executable=false` also works, but then the server is not configured as in the timing runs.
- **`perf_categories.py`**: `perf script -i <q>.perf.data -F comm,ip,sym --no-inline | python3 ngram_bench/perf_categories.py`, with the map from `perf_shares.sh` in place, splits the samples into categories; each sample goes to the first rule that matches any frame of its stack (the rules are in the docstring): matcher, per-row verify call, dict_fsst decode, block scan init, point read, gather, other table filter, n-gram candidate selection, rest.
- **`p2_step0/`**: bpftrace probes on `duckdb::dict_fsst::CompressedStringScanState::Initialize` and `StringFetchRow` for the 12 `like_*_w` forms on `e4581109f`, then on a segment merge (the scan path), on a copy of the datadir.
  - `run.sh BIN=<e4581109f perf build> DATADIR=<datadir> W=<work dir>` renders `init.bt` from `init.bt.in`; `analyze.py <W>/out` summarizes.
  - The struct offsets in `init.bt.in` come from the disassembly of the `e4581109f` perf build and must be rechecked for any other binary.
  - Needs `sudo` and bpftrace.
