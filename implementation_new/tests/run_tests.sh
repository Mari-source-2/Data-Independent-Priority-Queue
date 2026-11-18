#!/usr/bin/env bash
# Correctness tests for both queues. Usage: tests/run_tests.sh <build directory>
set -euo pipefail

BIN=$(cd "$1" && pwd)
TMP=$(mktemp -d)
trap 'kill $SERVER_PID 2>/dev/null || true; rm -rf "$TMP"' EXIT
SERVER_PID=""
FAILED=0

pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILED=1; }
field() { grep "^$2=" "$1" 2>/dev/null | head -1 | cut -d= -f2 || true; }

gen() {  # gen <name> <ops> <size> <key-bits> <prio-bits> <seed> <findmin-percent>
  "$BIN/generator" --ops "$2" --size "$3" --key-bits "$4" --prio-bits "$5" --seed "$6" \
    --findmin-percent "$7" --out "$TMP/$1.w" --answers "$TMP/$1.a" > "$TMP/$1.gen"
}

# run <label> <workload> <output> <args...>: runs and checks answers and overflows
run() {
  local label=$1 w=$2 out=$3; shift 3
  if "$BIN/run_workload" --workload "$TMP/$w.w" --answers "$TMP/$w.a" --dir "$TMP" "$@" > "$TMP/$out" 2> "$TMP/$out.err"; then
    if [ "$(field "$TMP/$out" overflows)" = "0" ]; then pass "$label"; else fail "$label (overflows)"; fi
  else
    fail "$label"; cat "$TMP/$out.err" | head -5
  fi
}

echo "== generating workloads"
gen small16 60000 3000 16 16 1 10     # 16-bit keys and priorities, with FindMin
gen small16b 60000 3000 16 16 7 10    # same length, different operations (trace test)
gen dense10 60000 600 10 16 2 0       # tiny key space: many repeated keys
gen wide24 60000 3000 24 24 3 5       # 3-byte fields
gen tiny 2000 20 16 16 4 20           # queue often empty
"$BIN/generator" --ops 30000 --size 3000 --key-bits 16 --prio-bits 16 --seed 5 --findmin-percent 10 \
  --mix insert-extract --out "$TMP/ie.w" --answers "$TMP/ie.a" > "$TMP/ie.gen"  # fresh insertions and extractions only

for w in small16 small16b dense10 wide24; do
  m=$(field "$TMP/$w.gen" max_present)
  if [ "$m" -le 3500 ]; then pass "workload $w has at most 3500 present keys ($m)"; else fail "workload $w has $m present keys"; fi
done

echo "== edge cases"
mkdir -p "$TMP/edge"
if "$BIN/edge_test" "$TMP/edge" > "$TMP/edge.out"; then pass "edge cases"; else fail "edge cases"; grep FAIL "$TMP/edge.out"; fi

echo "== our queue (in-process store)"
for mode in cached streaming; do
  for B in 256 512; do
    run "pq $mode B=$B small16"  small16 "pq_${mode}_${B}_small16"  --queue pq --n 4096 --B $B --mode $mode --key-bytes 2 --prio-bytes 2
    run "pq $mode B=$B dense10"  dense10 "pq_${mode}_${B}_dense10"  --queue pq --n 4096 --B $B --mode $mode --key-bytes 2 --prio-bytes 2
    run "pq $mode B=$B wide24"   wide24  "pq_${mode}_${B}_wide24"   --queue pq --n 4096 --B $B --mode $mode --key-bytes 3 --prio-bytes 3
  done
  run "pq $mode tiny" tiny "pq_${mode}_tiny" --queue pq --n 512 --B 256 --mode $mode --key-bytes 2 --prio-bytes 2
  run "pq $mode n=3500 small16" small16 "pq_${mode}_3500_small16" --queue pq --n 3500 --B 256 --mode $mode --key-bytes 2 --prio-bytes 2
done

echo "== our queue with a = B/4 (Theorem 3)"
for mode in cached streaming; do
  run "pq $mode B=256 a=64 small16" small16 "pq_${mode}_256_a64_small16" --queue pq --n 4096 --B 256 --a 64 --mode $mode --key-bytes 2 --prio-bytes 2
  run "pq $mode B=256 a=64 dense10" dense10 "pq_${mode}_256_a64_dense10" --queue pq --n 4096 --B 256 --a 64 --mode $mode --key-bytes 2 --prio-bytes 2
done
if "$BIN/run_workload" --workload "$TMP/tiny.w" --answers "$TMP/tiny.a" --dir "$TMP" --queue pq --n 512 --B 256 --a 65 > /dev/null 2> "$TMP/a65.err"; then
  fail "pq accepts a > B/4"
elif grep -q "relocation interval" "$TMP/a65.err"; then pass "pq rejects a > B/4"; else fail "pq a > B/4: unexpected error"; cat "$TMP/a65.err"; fi

echo "== level-wise baseline (schedule of Jafargholi et al. on the same tree)"
for B in 256 512; do
  run "pq levelwise B=$B small16" small16 "pq_levelwise_${B}_small16" --queue pq --n 4096 --B $B --mode levelwise --key-bytes 2 --prio-bytes 2
  run "pq levelwise B=$B dense10" dense10 "pq_levelwise_${B}_dense10" --queue pq --n 4096 --B $B --mode levelwise --key-bytes 2 --prio-bytes 2
done
run "pq levelwise B=256 a=64 small16" small16 pq_levelwise_256_a64_small16 --queue pq --n 4096 --B 256 --a 64 --mode levelwise --key-bytes 2 --prio-bytes 2
run "pq levelwise B=256 small16b" small16b pq_levelwise_256_small16b --queue pq --n 4096 --B 256 --mode levelwise --key-bytes 2 --prio-bytes 2
d1=$(field "$TMP/pq_levelwise_256_small16" trace_digest); d2=$(field "$TMP/pq_levelwise_256_small16b" trace_digest)
if [ -n "$d1" ] && [ "$d1" = "$d2" ]; then pass "pq levelwise: identical traces for different workloads and keys"; else fail "pq levelwise trace differs ($d1 vs $d2)"; fi
# A maintenance of all L levels makes one request per call: 2 (2^L - 1) round trips, 126 for L = 6.
m=$(field "$TMP/pq_levelwise_256_small16" max_round_trips_per_op)
if [ "$m" = 126 ]; then pass "pq levelwise: a full maintenance takes 2(2^L-1) = 126 round trips"; else fail "pq levelwise: max round trips per operation $m, expected 126"; fi

echo "== geometry and transfers per relocation (n=4096, B=256: L = ceil(log2(4n/B)) = 6)"
for f in pq_streaming_256_small16:32 pq_cached_256_small16:32 pq_streaming_256_a64_small16:64; do
  out=${f%%:*}; a=${f##*:}
  L=$(grep '^queue=' "$TMP/$out" | sed -E 's/.*L=([0-9]+).*/\1/')
  ops=$(field "$TMP/$out" operations); r=$((ops / a))
  rd=$(field "$TMP/$out" objects_read); wr=$(field "$TMP/$out" objects_written)
  case $out in *streaming*) want=$((r * (3 * L - 1))) ;; *) want=$((r * 2 * L)) ;; esac
  if [ "$L" = 6 ] && [ "$rd" = "$want" ] && [ "$wr" = "$want" ]; then pass "$out: L=$L, $rd objects read and written ($r relocations)"
  else fail "$out: L=$L, read $rd, written $wr, expected $want each"; fi
done

echo "== our queue with spread relocations (in-process store)"
for B in 256 512; do
  run "spread B=$B small16" small16 "spread_${B}_small16" --queue spread --n 4096 --B $B --key-bytes 2 --prio-bytes 2
  run "spread B=$B dense10" dense10 "spread_${B}_dense10" --queue spread --n 4096 --B $B --key-bytes 2 --prio-bytes 2
  run "spread B=$B wide24"  wide24  "spread_${B}_wide24"  --queue spread --n 4096 --B $B --key-bytes 3 --prio-bytes 3
done
run "spread B=64 small16" small16 spread_64_small16 --queue spread --n 4096 --B 64 --key-bytes 2 --prio-bytes 2
run "spread tiny" tiny spread_tiny --queue spread --n 512 --B 256 --key-bytes 2 --prio-bytes 2
run "spread n=3500 small16" small16 spread_3500_small16 --queue spread --n 3500 --B 256 --key-bytes 2 --prio-bytes 2
m=$(field "$TMP/spread_256_small16" max_objects_per_op)
if [ "$m" -le 3 ]; then pass "spread B=256: at most one call (3 buckets) per operation ($m)"; else fail "spread B=256 transfers $m buckets in one operation"; fi

echo "== data-independence of our queue's trace"
for mode in cached streaming; do
  run "pq $mode B=256 small16b" small16b "pq_${mode}_256_small16b" --queue pq --n 4096 --B 256 --mode $mode --key-bytes 2 --prio-bytes 2
  d1=$(field "$TMP/pq_${mode}_256_small16" trace_digest); d2=$(field "$TMP/pq_${mode}_256_small16b" trace_digest)
  if [ -n "$d1" ] && [ "$d1" = "$d2" ]; then pass "pq $mode: identical traces for different workloads and keys"; else fail "pq $mode trace differs ($d1 vs $d2)"; fi
done
run "spread B=256 small16b" small16b spread_256_small16b --queue spread --n 4096 --B 256 --key-bytes 2 --prio-bytes 2
d1=$(field "$TMP/spread_256_small16" trace_digest); d2=$(field "$TMP/spread_256_small16b" trace_digest)
if [ -n "$d1" ] && [ "$d1" = "$d2" ]; then pass "spread: identical traces for different workloads and keys"; else fail "spread trace differs ($d1 vs $d2)"; fi
c=$(field "$TMP/pq_cached_256_small16" objects_read); s=$(field "$TMP/pq_streaming_256_small16" objects_read)
echo "      objects read per run: cached=$c streaming=$s"

echo "== Path Oblivious Heap (in-process store)"
for w in small16 dense10 wide24 tiny; do
  kb=2; [ "$w" = wide24 ] && kb=3
  run "poh $w" $w "poh_$w" --queue poh --n 4096 --z 2 --key-bytes $kb --prio-bytes $kb
done
run "poh without key map, insert-extract" ie poh_ie --queue poh --n 4096 --z 2 --key-bytes 2 --prio-bytes 2 --no-key-map
m=$(field "$TMP/poh_ie" position_map_entries)
if [ "$m" = "0" ]; then pass "poh without key map stores no references"; else fail "poh without key map has $m map entries"; fi
run "pq insert-extract" ie pq_ie --queue pq --n 4096 --B 256 --key-bytes 2 --prio-bytes 2
run "poh n=3500 small16" small16 poh_3500 --queue poh --n 3500 --z 2 --key-bytes 2 --prio-bytes 2
ops=$(field "$TMP/poh_small16" operations); rt=$(field "$TMP/poh_small16" round_trips)
if [ "$ops" = "$rt" ]; then pass "poh: exactly one round trip per operation ($rt)"; else fail "poh round trips $rt for $ops operations"; fi

echo "== remote store"
PORT=$((20000 + RANDOM % 20000))
mkdir -p "$TMP/server"
"$BIN/server" $PORT "$TMP/server" > "$TMP/server.log" 2>&1 &
SERVER_PID=$!
for i in $(seq 1 100); do
  if grep -q "listening" "$TMP/server.log" 2>/dev/null; then break; fi
  sleep 0.1
done
run "pq streaming remote" small16 pq_remote --queue pq --n 4096 --B 256 --mode streaming --key-bytes 2 --prio-bytes 2 --store remote --port $PORT
d1=$(field "$TMP/pq_streaming_256_small16" trace_digest); d2=$(field "$TMP/pq_remote" trace_digest)
if [ "$d1" = "$d2" ]; then pass "pq remote trace equals in-process trace"; else fail "pq remote trace differs"; fi
run "pq cached remote" small16 pq_remote_cached --queue pq --n 4096 --B 256 --mode cached --key-bytes 2 --prio-bytes 2 --store remote --port $PORT
run "pq cached remote B=16384" small16 pq_remote_large --queue pq --n 131072 --B 16384 --mode cached --key-bytes 2 --prio-bytes 2 --store remote --port $PORT
run "pq streaming remote B=16384" small16 pq_remote_large_s --queue pq --n 131072 --B 16384 --mode streaming --key-bytes 2 --prio-bytes 2 --store remote --port $PORT
run "spread remote" small16 spread_remote --queue spread --n 4096 --B 256 --key-bytes 2 --prio-bytes 2 --store remote --port $PORT
d1=$(field "$TMP/spread_256_small16" trace_digest); d2=$(field "$TMP/spread_remote" trace_digest)
if [ "$d1" = "$d2" ]; then pass "spread remote trace equals in-process trace"; else fail "spread remote trace differs"; fi
run "poh remote" small16 poh_remote --queue poh --n 4096 --z 2 --key-bytes 2 --prio-bytes 2 --store remote --port $PORT
kill $SERVER_PID; SERVER_PID=""

echo "== persistent key file"
run "pq with key file (create)" tiny pq_key1 --queue pq --n 512 --B 256 --key-bytes 2 --prio-bytes 2 --keyfile "$TMP/key"
run "pq with key file (reuse)" tiny pq_key2 --queue pq --n 512 --B 256 --key-bytes 2 --prio-bytes 2 --keyfile "$TMP/key"

echo "== tamper and replay detection"
if "$BIN/tamper_test" "$TMP" > "$TMP/tamper.out"; then pass "swap and replay detected"; else fail "tamper test"; cat "$TMP/tamper.out"; fi

echo "== Dijkstra"
python3 - "$TMP/graph.gr" <<'PY'
import random, sys
random.seed(5)
V = 1500
arcs = []
for u in range(1, V + 1):
    for _ in range(3):
        v = random.randint(1, V)
        if v != u:
            w = random.randint(1, 1000)
            arcs += [(u, v, w), (v, u, w)]
with open(sys.argv[1], "w") as f:
    f.write(f"p sp {V} {len(arcs)}\n")
    for u, v, w in arcs:
        f.write(f"a {u} {v} {w}\n")
PY
for q in "--queue pq --B 128 --mode streaming" "--queue pq --B 128 --mode cached" "--queue poh --z 2"; do
  if "$BIN/dijkstra" --graph "$TMP/graph.gr" --source 1 --key-bytes 2 --prio-bytes 4 --dir "$TMP" $q > "$TMP/dij.out" 2> "$TMP/dij.err"; then
    pass "dijkstra $q"
  else
    fail "dijkstra $q"; cat "$TMP/dij.err" | head -3; grep wrong "$TMP/dij.out" || true
  fi
done

if [ $FAILED -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi
exit $FAILED
