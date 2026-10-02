#!/usr/bin/env bash
# A/B timing run, methodology in README.md ("Methodology"):
# warm: interleaved rounds, one untimed warmup + REPS timed runs per query;
# cold: per query and round, stop -> sync -> vmtouch -e datadir -> fresh
# process -> one timed run. Both binaries use the same datadir.
#
# Usage:
#   ab_run.sh A=<path> B=<path> [C=<path>] DATADIR=<dir> [OUT=...] [WARM_ROUNDS=4]
#             [REPS=5] [COLD_ROUNDS=8] [THREADS="1 8"] [PG_PORT=7931]
#             [WARM_FILES="queries_like.sql ..."] [WARM_RE=<ERE on query ids>]
#             [COLD_IDS="<qid> ..."] [ONLY="<qid> ..."] [DRY_RUN=1]
set -euo pipefail

for kv in "$@"; do export "${kv?}"; done

HERE=$(cd "$(dirname "$0")" && pwd)
: "${A:?A=<baseline serened>}" "${B:?B=<candidate serened>}" "${C:=}"
LABELS=(A B)
[[ -n $C ]] && LABELS+=(C)
: "${DATADIR:?DATADIR=<datadir built by schema.sql>}"
: "${OUT:=$PWD/runs/$(date +%Y%m%d_%H%M%S)}"
: "${WARM_ROUNDS:=4}" "${REPS:=5}" "${COLD_ROUNDS:=8}"
: "${THREADS:=1 8}" "${PG_PORT:=7931}" "${DRY_RUN:=0}" "${ONLY:=}" "${WARM_RE:=}"
: "${WARM_FILES:=queries_like.sql queries_regexp.sql queries_selectivity.sql}"
: "${COLD_IDS:=like_contains_mid_w like_prefix_freq_w like_suffix_sel_w like_under_suffix_w like_multi_heavy_w like_seg_w like_contains_mid_k like_contains_mid_t like_multi_heavy_k like_multi_heavy_t}"

CLIENT_CPUS=0
server_cpus() { [[ $1 == 1 ]] && echo 2,3 || echo 0-7; }

PSQL=(taskset -c "$CLIENT_CPUS" psql -X -h 127.0.0.1 -p "$PG_PORT" -U postgres -d postgres)
mkdir -p "$OUT"
RAW="$OUT/raw.tsv"
RES="$OUT/results.tsv"
LOG="$OUT/run.log"
[[ -f $RAW ]] || printf 'mode\tround\tthreads\tbin\tqid\trep\tms\n' >"$RAW"

log() { echo "$(date -Is) $*" | tee -a "$LOG"; }

# "<qid>\t<sql>" for every query in the given files.
list_queries() {
  local f
  for f in "$@"; do
    awk '/^-- [a-z0-9_]+ \|/ { id = $2; next }
         id != "" && /^SELECT/ { print id "\t" $0; id = "" }' "$HERE/$f"
  done
}

SERVER_PID=
start_server() {
  local bin=$1 t=$2
  log "start $bin threads=$t cpus=$(server_cpus "$t") load=$(cut -d' ' -f1-3 /proc/loadavg)"
  [[ $DRY_RUN == 1 ]] && return
  taskset -c "$(server_cpus "$t")" "$bin" "$DATADIR" \
    --listen="postgres://127.0.0.1:$PG_PORT" --cpu_threads="$t" \
    >>"$OUT/server.log" 2>&1 &
  SERVER_PID=$!
  local i
  for i in $(seq 600); do
    "${PSQL[@]}" -Atc 'SELECT 1' >/dev/null 2>&1 && break
    kill -0 "$SERVER_PID" 2>/dev/null || { log "server died"; exit 1; }
    sleep 0.1
  done
  local got
  got=$("${PSQL[@]}" -Atc "SELECT current_setting('threads')")
  [[ $got == "$t" ]] || { log "threads=$got, expected $t"; exit 1; }
}

stop_server() {
  [[ -z $SERVER_PID ]] && return
  kill -TERM "$SERVER_PID"
  wait "$SERVER_PID" || true
  SERVER_PID=
}
trap stop_server EXIT

# Runs each query (1 warmup + reps timed) in one psql session; appends timings
# to RAW and the first result of each query to RES.
run_batch() {
  local mode=$1 round=$2 t=$3 label=$4 warmup=$5 reps=$6
  shift 6
  local script="$OUT/batch.sql" qid sql r
  {
    echo '\timing on'
    while IFS=$'\t' read -r qid sql; do
      for ((r = 1 - warmup; r <= reps; r++)); do
        echo "\\echo @@ $qid $r"
        echo "$sql"
      done
    done
  } <<<"$(printf '%s\n' "$@")" >"$script"
  [[ $DRY_RUN == 1 ]] && { log "dry $mode r$round t$t $label $# queries"; return; }
  "${PSQL[@]}" -At -v ON_ERROR_STOP=1 -f "$script" >"$OUT/batch.out" 2>&1 ||
    { log "psql failed, see $OUT/batch.out"; exit 1; }
  awk -v m="$mode" -v rd="$round" -v t="$t" -v b="$label" \
      -v raw="$RAW" -v res="$RES" '
    /^@@ / { q = $2; rep = $3; first = 1; next }
    /^Time: / { if (rep > 0) printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", m, rd, t, b, q, rep, $2 >> raw; next }
    rep == 1 && first { printf "%s\t%s\t%s\t%s\n", b, t, q, $0 >> res; first = 0 }
  ' "$OUT/batch.out"
}

mapfile -t WARM_Q < <(list_queries $WARM_FILES)
if [[ -n $WARM_RE ]]; then
  mapfile -t WARM_Q < <(printf '%s\n' "${WARM_Q[@]}" | awk -F'\t' -v re="$WARM_RE" '$1 ~ re')
fi
mapfile -t ALL_Q < <(list_queries queries_like.sql queries_regexp.sql queries_selectivity.sql)
COLD_Q=()
for id in $COLD_IDS; do
  for line in "${ALL_Q[@]}"; do [[ ${line%%$'\t'*} == "$id" ]] && COLD_Q+=("$line"); done
done

keep_only() {
  local line id
  for line in "$@"; do
    for id in $ONLY; do [[ ${line%%$'\t'*} == "$id" ]] && printf '%s\n' "$line"; done
  done
  return 0
}
if [[ -n $ONLY ]]; then
  mapfile -t WARM_Q < <(keep_only "${WARM_Q[@]}")
  mapfile -t COLD_Q < <(keep_only "${COLD_Q[@]}")
fi

# Round r runs the binaries rotated by r-1: A B C, B C A, C A B, ...
order() {
  local n=${#LABELS[@]} i
  for ((i = 0; i < n; i++)); do echo "${LABELS[(i + $1 - 1) % n]}"; done
}
binpath() { case $1 in A) echo "$A" ;; B) echo "$B" ;; C) echo "$C" ;; esac; }

for label in "${LABELS[@]}"; do
  log "$label=$(binpath "$label") sha256=$(sha256sum "$(binpath "$label")" | cut -c1-16)"
done
log "datadir=$DATADIR warm=${#WARM_Q[@]} cold=${#COLD_Q[@]} threads=[$THREADS]"


for ((round = 1; round <= WARM_ROUNDS; round++)); do
  for t in $THREADS; do
    for label in $(order "$round"); do
      start_server "$(binpath "$label")" "$t"
      run_batch warm "$round" "$t" "$label" 1 "$REPS" "${WARM_Q[@]}"
      stop_server
    done
  done
done

for ((round = 1; round <= COLD_ROUNDS; round++)); do
  for t in $THREADS; do
    for q in "${COLD_Q[@]}"; do
      for label in $(order "$round"); do
        sync
        if [[ $DRY_RUN != 1 ]]; then
          vmtouch -q -e "$DATADIR"
          log "cold r$round t$t $label ${q%%$'\t'*} $(vmtouch "$DATADIR" | grep 'Resident Pages')"
        fi
        start_server "$(binpath "$label")" "$t"
        run_batch cold "$round" "$t" "$label" 0 1 "$q"
        stop_server
      done
    done
  done
done

log "done; summarize: python3 $HERE/summarize.py $OUT"
