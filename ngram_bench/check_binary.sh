#!/usr/bin/env bash
# Answers and EXPLAIN of the benchmark queries on one binary (not a timing
# run). Starts the server on DATADIR with default threads, runs every query
# of FILES once and with EXPLAIN, stops the server.
#
# Usage: check_binary.sh BIN=<serened> OUT=<dir> DATADIR=<dir> [PG_PORT=7931]
#                        [FILES="queries_like.sql ..."]
set -euo pipefail
for kv in "$@"; do export "${kv?}"; done

HERE=$(cd "$(dirname "$0")" && pwd)
: "${BIN:?}" "${OUT:?}"
: "${DATADIR:?DATADIR=<datadir built by schema.sql>}" "${PG_PORT:=7931}"
: "${FILES:=queries_like.sql queries_regexp.sql queries_selectivity.sql}"

PSQL=(psql -X -h 127.0.0.1 -p "$PG_PORT" -U postgres -d postgres -At)
mkdir -p "$OUT"

"$BIN" "$DATADIR" --listen="postgres://127.0.0.1:$PG_PORT" >"$OUT/server.log" 2>&1 &
PID=$!
trap 'kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null || true' EXIT
for i in $(seq 600); do
  "${PSQL[@]}" -c 'SELECT 1' >/dev/null 2>&1 && break
  kill -0 "$PID" 2>/dev/null || { echo "server died" >&2; exit 1; }
  sleep 0.1
done
echo "bin=$BIN sha256=$(sha256sum "$BIN" | cut -c1-16) threads=$("${PSQL[@]}" -c "SELECT current_setting('threads')")" >"$OUT/meta.txt"

for f in $FILES; do
  [[ -f $f ]] && path=$f || path=$HERE/$f
  name=$(basename "$f" .sql)
  awk '/^-- [a-z0-9_]+ \|/ { id = $2; next }
       id != "" && /^SELECT/ { print "\\echo @@ " id; print; id = "" }' "$path" >"$OUT/$name.q.sql"
  sed 's/^SELECT/EXPLAIN SELECT/' "$OUT/$name.q.sql" >"$OUT/$name.explain.sql"
  { echo '\timing on'; cat "$OUT/$name.q.sql"; } >"$OUT/$name.run.sql"
  "${PSQL[@]}" -f "$OUT/$name.run.sql" >"$OUT/$name.answers.log" 2>&1
  "${PSQL[@]}" -f "$OUT/$name.explain.sql" >"$OUT/$name.explain.log" 2>&1
done
