#!/usr/bin/env bash
# Remeasure for PR #1308: main (A) against the port of part 2 onto main (B).
# MAIN and PORT are commit hashes; binaries are expected at
# $ROOT/bin/serened-<first 9 of hash>-perf, everything else goes to $ROOT/pr2.
#
#   pr2.sh data  ROOT=<dir> MAIN=<hash>               # CSV + datadir built by main
#   pr2.sh check ROOT=<dir> MAIN=<hash> [PORT=<hash>] # answers and EXPLAIN, compare
#   pr2.sh run   ROOT=<dir> MAIN=<hash> PORT=<hash> [DRY_RUN=1]
set -euo pipefail
stage=${1:?data|check|run}
shift
for kv in "$@"; do export "${kv?}"; done

HERE=$(cd "$(dirname "$0")" && pwd)
: "${ROOT:?ROOT=<dir>}" "${MAIN:?MAIN=<hash>}" "${PORT:=}" "${DRY_RUN:=0}" "${PG_PORT:=7931}"
W=$ROOT/pr2
DD=$W/datadir
A=$ROOT/bin/serened-${MAIN:0:9}-perf
B=${PORT:+$ROOT/bin/serened-${PORT:0:9}-perf}
CHECKS=part2_bench_checks.sql
mkdir -p "$W"

case $stage in
  data)
    [[ -e $DD ]] && { echo "$DD exists" >&2; exit 1; }
    mkdir -p "$W/data" "$DD"
    cd "$W/data"
    python3 "$HERE/gen_logs.py" --rows 1000000 --seed 42 --out logs_1m_s42.csv 2>gen.log
    sha256sum logs_1m_s42.csv | tee -a gen.log
    "$A" "$DD" --listen="postgres://127.0.0.1:$PG_PORT" >"$W/server_load.log" 2>&1 &
    pid=$!
    trap 'kill -TERM $pid 2>/dev/null; wait $pid 2>/dev/null || true' EXIT
    for _ in $(seq 600); do
      psql -X -h 127.0.0.1 -p "$PG_PORT" -U postgres -d postgres -Atc 'SELECT 1' >/dev/null 2>&1 && break
      sleep 0.1
    done
    psql -X -h 127.0.0.1 -p "$PG_PORT" -U postgres -d postgres -f "$HERE/schema.sql" >"$W/schema_load.log" 2>&1
    tail -15 "$W/schema_load.log"
    ;;
  check)
    # Without PORT: main only, its answers against the table references.
    F="queries_like.sql queries_regexp.sql queries_selectivity.sql $CHECKS"
    [[ -e $W/checks/main ]] ||
      "$HERE/check_binary.sh" BIN="$A" OUT="$W/checks/main" DATADIR="$DD" FILES="$F"
    if [[ -z $PORT ]]; then
      python3 "$HERE/compare_checks.py" "$W/checks/main" "$W/checks/main" | tee "$W/checks/compare_main.txt"
      exit
    fi
    "$HERE/check_binary.sh" BIN="$B" OUT="$W/checks/port" DATADIR="$DD" FILES="$F"
    python3 "$HERE/compare_checks.py" "$W/checks/main" "$W/checks/port" | tee "$W/checks/compare.txt"
    ;;
  run)
    : "${PORT:?PORT=<hash>}"
    [[ $DRY_RUN == 1 ]] || grep -q '^no problems$' "$W/checks/compare.txt" ||
      { echo "run the check stage first; it must end with 'no problems'" >&2; exit 1; }
    "$HERE/ab_run.sh" A="$A" B="$B" DATADIR="$DD" DRY_RUN="$DRY_RUN" \
      OUT="$W/runs/ab_$(date -u +%Y%m%d_%H%M%S)" \
      WARM_RE='^(like_|rx_|s_like_(mid|heavy)_|s_like_freq_(xall|x1)_count$|s_rx_heavy_(xall|x1|x50)_count$)' \
      COLD_IDS='like_contains_mid_w like_prefix_freq_w like_multi_heavy_w like_seg_w like_contains_mid_t like_multi_heavy_t'
    ;;
  *)
    echo "unknown stage $stage" >&2
    exit 1
    ;;
esac
