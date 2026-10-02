#!/usr/bin/env bash
# Part 2 step 0 on the base binary: per-query dict_fsst Initialize calls
# (point path), then Initialize(true) (scan path) during a segment merge.
# Works on a copy of the bench datadir; port 7935; needs sudo for bpftrace.
# Usage: run.sh BIN=<serened e4581109f perf build> DATADIR=<bench datadir> W=<work dir>
set -euo pipefail
for kv in "$@"; do export "${kv?}"; done
HERE=$(cd "$(dirname "$0")" && pwd)
: "${BIN:?}" "${DATADIR:?}" "${W:?}"
DD=$W/datadir
PORT=7935
OUT=$W/out
mkdir -p "$OUT"
sudo rm -rf "$OUT"
rm -rf "$DD"
mkdir -p "$OUT"
cp -a "$DATADIR" "$DD"
sed "s|@SERENED@|$BIN|g" "$HERE/init.bt.in" > "$W/init.bt"
PSQL="psql -X -h 127.0.0.1 -p $PORT -U postgres -d postgres -v ON_ERROR_STOP=1"

"$BIN" "$DD" --listen="postgres://127.0.0.1:$PORT" --remap_executable=false \
  --cpu_threads=1 > "$OUT/server.log" 2>&1 &
SPID=$!
trap 'kill $SPID 2>/dev/null || true' EXIT
for _ in $(seq 100); do $PSQL -c 'select 1' >/dev/null 2>&1 && break; sleep 0.2; done
$PSQL -Atc "select current_setting('threads')" > "$OUT/threads.txt"

traced() {
  local name=$1 sql=$2
  sudo BPFTRACE_STRLEN=200 bpftrace --unsafe "$W/init.bt" "$OUT/$name.ready" -o "$OUT/$name.bt.txt" > "$OUT/$name.bt.stderr" 2>&1 &
  local bpid=$!
  for _ in $(seq 600); do
    $PSQL -c "SELECT md5('ready')" > /dev/null
    [ -e "$OUT/$name.ready" ] && break
    sleep 0.2
  done
  [ -e "$OUT/$name.ready" ] || { echo "bpftrace not ready: $name"; exit 1; }
  { echo '\timing on'; echo "$sql"; } | $PSQL > "$OUT/$name.psql.txt" 2>&1
  sudo kill -INT "$(pgrep -P $bpid bpftrace || echo $bpid)" 2>/dev/null || true
  wait $bpid || true
}

grep -A1 -E '^-- like_[a-z_]+_w ' "$HERE/../queries_like.sql" | grep -v '^--$' |
  paste - - | while IFS=$'\t' read -r hdr sql; do
    name=$(echo "$hdr" | awk '{print $2}')
    # untraced warm-up so the traced run sees a warm cache
    echo "$sql" | $PSQL > /dev/null
    traced "$name" "$sql"
  done

# Scan path: a second segment, then a merge reads every block of the big
# segment through ColumnReader::Scan (iresearch/formats/column/merge.cpp).
$PSQL -c "INSERT INTO logs VALUES (1000001, 1, 'GET /api/v1/users/1 200 1 ms')" > "$OUT/merge_prep.txt"
$PSQL -c "VACUUM (REFRESH_TABLE) logs" >> "$OUT/merge_prep.txt"
$PSQL -Atc "SELECT c.relname, m.metric, m.value FROM sdb_metrics m JOIN pg_class c ON c.oid = m.relation_id WHERE c.relname = 'logs_w' AND m.metric IN ('num_segments', 'num_docs')" >> "$OUT/merge_prep.txt"
traced merge_logs_w "VACUUM (COMPACT_INDEX) logs_w;"
$PSQL -Atc "SELECT c.relname, m.metric, m.value FROM sdb_metrics m JOIN pg_class c ON c.oid = m.relation_id WHERE c.relname = 'logs_w' AND m.metric IN ('num_segments', 'num_docs')" >> "$OUT/merge_prep.txt"
kill $SPID; wait $SPID || true
trap - EXIT
echo done
