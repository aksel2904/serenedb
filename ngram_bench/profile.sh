#!/usr/bin/env bash
# perf profile of single queries on a running server (warm cache).
# For each query: 3 warmup runs, REPS timed runs (psql \timing), then
# `perf record -e cpu-clock --call-graph fp` attached to the server while the
# query runs PERF_REPS more times; writes perf report --children per query.
#
# Usage: profile.sh PID=<serened pid> OUT=<dir> [PG_PORT=7931] [REPS=10]
#                   [MIN_PERF_MS=3000] [QIDS="like_contains_mid_w ..."]
set -euo pipefail
for kv in "$@"; do export "${kv?}"; done

HERE=$(cd "$(dirname "$0")" && pwd)
: "${PID:?PID=<serened pid>}" "${OUT:?OUT=<dir>}"
: "${PG_PORT:=7931}" "${REPS:=10}" "${MIN_PERF_MS:=3000}"
: "${QIDS:=$(awk '/^-- like_[a-z0-9_]+_w \|/ { print $2 }' "$HERE/queries_like.sql" | xargs)}"

PSQL=(taskset -c 0 psql -X -h 127.0.0.1 -p "$PG_PORT" -U postgres -d postgres -At -v ON_ERROR_STOP=1)
mkdir -p "$OUT"

sql_of() {
  awk -v id="$1" '$1 == "--" && $2 == id { getline; print; exit }' "$HERE"/queries_*.sql
}

# Runs the query n times in one session, prints one ms value per run.
timed() {
  local sql=$1 n=$2
  { echo '\timing on'; for ((i = 0; i < n; i++)); do echo "$sql"; done; } |
    "${PSQL[@]}" -f - | awk '/^Time: / { print $2 }'
}

printf 'qid\tresult\truns\tmin_ms\tmedian_ms\tperf_runs\tsamples\n' >"$OUT/summary.tsv"
for qid in $QIDS; do
  sql=$(sql_of "$qid")
  [[ -n $sql ]] || { echo "no query $qid" >&2; exit 1; }
  result=$("${PSQL[@]}" -c "$sql")
  timed "$sql" 3 >/dev/null
  timed "$sql" "$REPS" >"$OUT/$qid.ms"
  read -r min med < <(sort -g "$OUT/$qid.ms" |
    awk '{ v[NR] = $1 } END { print v[1], (NR % 2 ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2) }')
  perf_runs=$(awk -v m="$med" -v t="$MIN_PERF_MS" 'BEGIN { n = int(t / m) + 1; print (n < 5 ? 5 : n) }')

  sudo perf record -q -e cpu-clock -F 4000 --call-graph fp -p "$PID" \
    -o "$OUT/$qid.perf.data" &
  perf_pid=$!
  sleep 1
  timed "$sql" "$perf_runs" >"$OUT/$qid.perf.ms"
  sudo kill -INT "$perf_pid"
  wait "$perf_pid" || true
  sudo chown "$(id -u):$(id -g)" "$OUT/$qid.perf.data"

  perf report -i "$OUT/$qid.perf.data" --children --stdio --sort symbol \
    -g none --percent-limit 0.5 2>/dev/null >"$OUT/$qid.children.txt"
  samples=$(perf report -i "$OUT/$qid.perf.data" --stdio --sort comm 2>/dev/null |
    awk '/^# Samples:/ && s == "" { s = $3 } END { print s }')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$qid" "$result" "$REPS" "$min" "$med" \
    "$perf_runs" "$samples" | tee -a "$OUT/summary.tsv"
done
