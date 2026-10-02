#!/usr/bin/env bash
# Unattended chain for the PR #1309 and PR #1308 measurements; start it inside
# tmux. CASE is the commit of the (?i) extension, called (a) in the log; PORT
# is the commit of the port of part 2 onto main. WT is a SereneDB worktree on
# MAIN with a configured build_perf; each commit is checked out there detached
# and serened is rebuilt incrementally. Run pr2.sh data and pr2.sh check with
# MAIN only before the chain. Steps:
#   1. perf build of (a), binary, sha256;
#   2. answers and EXPLAIN of (a) against main: all answers equal main and the
#      table references, only the plan of rx_ci_w may differ; Query of rx_ci_w
#      is ALL on main and not ALL on (a);
#   3. timing run of (a) against main: rx_ci and rx_contains_rare on w, k, t,
#      warm cache only;
#   4. perf build of the port (PORT), binary, sha256;
#   5. pr2.sh check: must end with "no problems";
#   6. pr2.sh run, then summarize.
# Any error or mismatch stops the chain with "STOP: <reason>" in the log;
# the last line is "DONE" when everything passed.
#
# Usage: night.sh ROOT=<dir> WT=<worktree> CASE=<hash> PORT=<hash>
#                 [MAIN=<hash>] [FROM=1..6] [PG_PORT=7931]
set -euo pipefail
for kv in "$@"; do export "${kv?}"; done

HERE=$(cd "$(dirname "$0")" && pwd)
: "${MAIN:=96ab0bfc845a3280a1f4e8e8c1caee67696e8888}" "${CASE:?CASE=<hash>}" "${PORT:?PORT=<hash>}"
: "${ROOT:?ROOT=<dir>}" "${WT:?WT=<worktree>}" "${FROM:=1}" "${PG_PORT:=7931}"
: "${LOG:=$ROOT/logs/night_$(date -u +%Y%m%d_%H%M%S).log}"
mkdir -p "$ROOT/logs" "$(dirname "$LOG")"
CF=$ROOT/casefold
DD=$ROOT/pr2/datadir
MAIN_BIN=$ROOT/bin/serened-${MAIN:0:9}-perf
CASE_BIN=$ROOT/bin/serened-${CASE:0:9}-perf
PORT_BIN=$ROOT/bin/serened-${PORT:0:9}-perf
F="queries_like.sql queries_regexp.sql queries_selectivity.sql part2_bench_checks.sql"

log() { echo "$(date -Is) $*" | tee -a "$LOG"; }
stop() {
  log "STOP: $*"
  exit 1
}
trap 'stop "command failed (line $LINENO): $BASH_COMMAND"' ERR

build() {
  local h=$1 name=$2 bin=$3
  [[ $(git -C "$WT" cat-file -t "$h" 2>/dev/null) == commit ]] || stop "$name: commit $h not visible in $WT"
  git -C "$WT" merge-base --is-ancestor "$MAIN" "$h" || stop "$name: $h does not descend from main $MAIN"
  [[ -z $(git -C "$WT" status --porcelain) ]] || stop "$name: worktree $WT is not clean"
  [[ ! -e $bin ]] || stop "$name: $bin already exists"
  git -C "$WT" checkout -q --detach "$h"
  git -C "$WT" submodule update --recursive >>"$LOG" 2>&1
  local sm
  sm=$(git -C "$WT" submodule status --recursive | grep -v '^ ' || true)
  [[ -z $sm ]] || stop "$name: submodules out of sync: $sm"
  log "$name: $(git -C "$WT" log -1 --format='%H %s' "$h")"
  log "$name: diff from main: $(git -C "$WT" diff --shortstat "$MAIN" "$h")"
  local blog=$ROOT/logs/perf_ninja_serened_${h:0:9}.log
  (cd "$WT/build_perf" && date -Is && time ninja serened) >"$blog" 2>&1 ||
    stop "$name: ninja failed, see $blog"
  log "$name: $(grep -E '^\[[0-9]+/[0-9]+\]' "$blog" | tail -1 | cut -d' ' -f1) steps, $(grep '^real' "$blog")"
  cp "$WT/build_perf/bin/serened" "$bin"
  local l
  l=$(ldd "$bin" 2>&1 || true)
  [[ $l == *"not a dynamic executable"* ]] || stop "$name: $bin is not static: $l"
  log "$name: $bin sha256 $(sha256sum "$bin" | cut -d' ' -f1)"
}

# Query attribute of the Wildcard NGram node of one query in an explain log.
query_attr() {
  python3 - "$1" "$2" <<'EOF'
import re, sys
path, qid = sys.argv[1], sys.argv[2]
cur, want = None, False
for line in open(path, encoding="utf-8"):
    line = line.rstrip("\n")
    if line.startswith("@@ "):
        cur, want = line[3:], False
        continue
    if cur != qid:
        continue
    body = re.sub(r"[\u2502\u256d\u256e\u2570\u256f\u2500\u252c\u2534\u2524\u251c]", "", line).strip()
    if want:
        print(body)
        sys.exit(0)
    if body.startswith("Query:"):
        rest = body.split(":", 1)[1].strip()
        if rest:
            print(rest)
            sys.exit(0)
        want = True
print("<none>")
EOF
}

newest_run() {
  local d
  d=$(ls -td "$1"/ab_*)
  echo "${d%%$'\n'*}"
}

log "start MAIN=$MAIN CASE=$CASE PORT=$PORT FROM=$FROM log=$LOG"
[[ $(ss -ltn) != *":$PG_PORT "* ]] || stop "port $PG_PORT is busy"
[[ -x $MAIN_BIN ]] || stop "main binary $MAIN_BIN missing"
[[ -d $DD ]] || stop "datadir $DD missing (pr2.sh data)"
grep -q '^no problems$' "$ROOT/pr2/checks/compare_main.txt" || stop "main checks missing or failed (pr2.sh check MAIN=...)"
mkdir -p "$CF/checks" "$CF/runs"

if ((FROM <= 1)); then
  log "step 1: perf build of (a)"
  build "$CASE" "(a)" "$CASE_BIN"
fi

if ((FROM <= 2)); then
  log "step 2: answers and EXPLAIN of (a)"
  rm -rf "$CF/checks/case"
  "$HERE/check_binary.sh" BIN="$CASE_BIN" OUT="$CF/checks/case" DATADIR="$DD" FILES="$F" >>"$LOG" 2>&1
  python3 "$HERE/compare_checks.py" "$ROOT/pr2/checks/main" "$CF/checks/case" \
    --allow-plan-diff=rx_ci_w --show-plan=rx_ci_w >"$CF/checks/compare.txt" 2>&1 ||
    stop "(a): answers or plans differ from main, see $CF/checks/compare.txt"
  qa=$(query_attr "$ROOT/pr2/checks/main/queries_regexp.explain.log" rx_ci_w)
  qb=$(query_attr "$CF/checks/case/queries_regexp.explain.log" rx_ci_w)
  log "(a): rx_ci_w Query on main: $qa"
  log "(a): rx_ci_w Query on (a): $qb"
  [[ $qa == ALL ]] || stop "(a): rx_ci_w Query on main is not ALL: $qa"
  [[ $qb != ALL && $qb != '<none>' ]] || stop "(a): rx_ci_w Query on (a) is $qb, no tree"
  log "(a): checks passed, see $CF/checks/compare.txt"
fi

if ((FROM <= 3)); then
  log "step 3: timing run of (a)"
  "$HERE/ab_run.sh" A="$MAIN_BIN" B="$CASE_BIN" DATADIR="$DD" \
    OUT="$CF/runs/ab_$(date -u +%Y%m%d_%H%M%S)" WARM_FILES=queries_regexp.sql \
    WARM_RE='^rx_(ci|contains_rare)_(w|k|t)$' COLD_ROUNDS=0 >>"$LOG" 2>&1
  run=$(newest_run "$CF/runs")
  python3 "$HERE/summarize.py" "$run" >"$run/summary.tsv"
  grep -q '^# results differing from A: 0$' "$run/summary.tsv" ||
    stop "(a): answers differ between binaries during the run, see $run/summary.tsv"
  log "(a): run done: $run"
fi

if ((FROM <= 4)); then
  log "step 4: perf build of the port"
  build "$PORT" "port" "$PORT_BIN"
fi

if ((FROM <= 5)); then
  log "step 5: pr2.sh check"
  rm -rf "$ROOT/pr2/checks/port"
  "$HERE/pr2.sh" check MAIN="$MAIN" PORT="$PORT" >>"$LOG" 2>&1 ||
    stop "port: answers or plans differ from main, see $ROOT/pr2/checks/compare.txt"
  grep -q '^no problems$' "$ROOT/pr2/checks/compare.txt" ||
    stop "port: compare.txt does not end with 'no problems'"
  log "port: checks passed"
fi

if ((FROM <= 6)); then
  log "step 6: pr2.sh run"
  "$HERE/pr2.sh" run MAIN="$MAIN" PORT="$PORT" >>"$LOG" 2>&1
  run=$(newest_run "$ROOT/pr2/runs")
  python3 "$HERE/summarize.py" "$run" >"$run/summary.tsv"
  grep -q '^# results differing from A: 0$' "$run/summary.tsv" ||
    stop "port: answers differ between binaries during the run, see $run/summary.tsv"
  log "port: run done: $run"
fi

log "DONE"
