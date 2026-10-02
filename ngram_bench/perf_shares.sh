#!/usr/bin/env bash
# Inclusive CPU shares per query from profile.sh output.
# The server remaps its code into anonymous memory (--remap_executable, on by
# default), so perf cannot symbolize it from the file; the binary is not PIE
# and keeps link-time addresses, so a /tmp/perf-<pid>.map built from nm
# resolves the samples.
#
# Usage: perf_shares.sh BIN=<serened> PID=<pid at record time> DIR=<profile dir>
set -euo pipefail
for kv in "$@"; do export "${kv?}"; done
: "${BIN:?}" "${PID:?}" "${DIR:?}"

MAP=/tmp/perf-$PID.map
if [[ ! -s $MAP ]]; then
  nm -C --defined-only -S "$BIN" |
    awk 'NF >= 4 && $3 ~ /^[tTwW]$/ { name = $4; for (i = 5; i <= NF; i++) name = name " " $i; print $1, $2, name }' >"$MAP"
fi

share() {
  awk -v pat="$1" 'index($0, pat) { v = $1 + 0; if (v > m) m = v } END { printf "%.1f", m }' "$2"
}

printf 'qid\tsamples\tread_FetchRow_pct\tmatch_LikeMatcher_pct\tread_plus_match_pct\n'
for data in "$DIR"/*.perf.data; do
  qid=$(basename "$data" .perf.data)
  rep="$DIR/$qid.children.full.txt"
  perf report -i "$data" --children --stdio --sort symbol -g none \
    --percent-limit 0 2>/dev/null >"$rep"
  samples=$(awk '/^# Samples:/ && s == "" { s = $3 } END { print s }' "$rep")
  read_pct=$(share 'irs::ColumnReader::PointReader::FetchRow(' "$rep")
  match_pct=$(share 'irs::LikeMatcher::Match(' "$rep")
  sum_pct=$(awk -v a="$read_pct" -v b="$match_pct" 'BEGIN { printf "%.1f", a + b }')
  printf '%s\t%s\t%s\t%s\t%s\n' "$qid" "$samples" "$read_pct" "$match_pct" "$sum_pct"
done
