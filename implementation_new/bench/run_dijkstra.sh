#!/usr/bin/env bash
# Dijkstra benchmark. Usage: bench/run_dijkstra.sh <build directory> <output directory> <graph.gr>...
#
# Runs Dijkstra's algorithm (apps/dijkstra.cpp) from vertex SOURCE [1] on every DIMACS graph with every
# queue, all with the same buffer size B [1024]; checks every distance against a plain Dijkstra. The
# queue capacity is n = number of vertices. The emulated environments tee, lan, region and wan of
# bench/run_benchmarks.sh are evaluated in one run per configuration (dijkstra --profiles): the total
# time of the run plus, per environment, one round-trip time per read request and the transfer time of
# all bytes. Keys have 3 bytes and priorities (distances) PRIO_BYTES [4] bytes.
#
# Queues (QUEUES [streaming cached spread levelwise poh]): see bench/run_benchmarks.sh; streaming, cached
# and levelwise relocate every a = B/A_DIV operations, A_DIV [4], spread every a = B/8.
set -euo pipefail

BIN=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
shift 2
[ $# -ge 1 ] || { echo "usage: $0 <build dir> <output dir> <graph.gr>..."; exit 1; }
B=${B:-1024}
A_DIV=${A_DIV:-4}
SOURCE=${SOURCE:-1}
PRIO_BYTES=${PRIO_BYTES:-4}
QUEUES=${QUEUES:-"streaming cached spread levelwise poh"}
PROFSPEC="tee:0:0,lan:1000:1000,region:10000:1000,wan:50000:100"
PROFILES="tee lan region wan"
mkdir -p "$OUT/raw" "$OUT/storage"
for q in $QUEUES; do case $q in streaming|cached|spread|levelwise|poh) ;; *) echo "unknown queue $q"; exit 1 ;; esac; done
for g in "$@"; do [ -f "$g" ] || { echo "no graph $g"; exit 1; }; done

CSV="$OUT/dijkstra.csv"
HEADER="graph,vertices,arcs,queue,B,a,profile,exit_code,queue_operations,total_ms,network_ms,round_trips,bytes_down,bytes_up,objects_read,objects_written,encryption_ms,peak_client_records,position_map_entries,overflows,wrong_distances"
if [ -f "$CSV" ]; then
  [ "$(head -1 "$CSV")" = "$HEADER" ] || { echo "$CSV has a different header; use a fresh output directory"; exit 1; }
else
  echo "$HEADER" > "$CSV"
fi
field() { grep "^$2=" "$1" 2>/dev/null | head -1 | cut -d= -f2 || true; }
{
  echo "== $(date -u +%Y-%m-%dT%H:%M:%SZ) B=$B A_DIV=$A_DIV SOURCE=$SOURCE PRIO_BYTES=$PRIO_BYTES QUEUES=\"$QUEUES\" graphs: $*"
  uname -a
  (sysctl -n machdep.cpu.brand_string hw.memsize 2>/dev/null || grep -m1 "model name" /proc/cpuinfo) | tr '\n' ' '; echo
  for g in "$@"; do shasum -a 256 "$g"; done
  SRC=$(cd "$(dirname "$0")/.." && pwd)
  echo "code: $(git -C "$SRC" rev-parse HEAD 2>/dev/null) uncommitted diff sha1: $(git -C "$SRC" diff HEAD -- . 2>/dev/null | shasum | cut -c1-12)"
} >> "$OUT/provenance.txt"

for g in "$@"; do
  gname=$(basename "$g" .gr)
  for q in $QUEUES; do
    case $q in
      streaming|cached|levelwise) a=$((B / A_DIV)); args=(--queue pq --mode "$q" --B "$B" --a "$a") ;;
      spread) a=$((B / 8)); args=(--queue spread --B "$B") ;;
      poh)    a=""; args=(--queue poh --z 2) ;;  # fresh random leaves
    esac
    raw="$OUT/raw/$gname-$q.txt"
    echo "== $gname $q"
    rc=0
    "$BIN/dijkstra" "${args[@]}" --graph "$g" --source "$SOURCE" --key-bytes 3 --prio-bytes "$PRIO_BYTES" \
      --dir "$OUT/storage" --profiles "$PROFSPEC" > "$raw" 2> "$raw.err" || rc=$?
    for prof in $PROFILES; do
      line="$gname,$(field "$raw" vertices),$(field "$raw" arcs),$q,$([ "$q" = poh ] || echo "$B"),$a,$prof,$rc"
      line="$line,$(field "$raw" queue_operations),$(field "$raw" "$prof.total_ms"),$(field "$raw" "$prof.network_ms")"
      for f in round_trips bytes_down bytes_up objects_read objects_written encryption_ms peak_client_records position_map_entries overflows wrong_distances; do
        line="$line,$(field "$raw" "$f")"
      done
      echo "$line" >> "$CSV"
    done
    [ "$rc" = 0 ] || echo "WARNING: $gname $q exited with $rc; see $raw.err" >&2
    rm -f "$OUT"/storage/*.t[0-9]
  done
done
echo "results in $CSV"
