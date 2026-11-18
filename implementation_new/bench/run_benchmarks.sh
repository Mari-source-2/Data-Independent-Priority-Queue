#!/usr/bin/env bash
# Benchmarks. Usage: bench/run_benchmarks.sh <build directory> <output directory>
#
# Queues, all with the same buffer size B:
#   streaming  our queue (Section 3); the client holds at most three buckets besides the root (6B records)
#   cached     our queue (Section 3); the client downloads and holds a whole path per relocation (2L buckets)
#   spread     our queue with spread relocations (Appendix B), a = B/8; three buckets, snapshot, front (55B/8)
# streaming and cached relocate every a = B/A_DIV operations: A_DIV=4 (default) is Theorem 3 / Corollary 2
# (pseudorandom routing, which the implementation uses), A_DIV=8 is Theorem 1. Use a separate output
# directory for each A_DIV, since the summary scripts do not distinguish runs by a.
#   levelwise  baseline: the level-wise schedule of Jafargholi et al. (SODA'21) on the same tree and buffers,
#              relocating every a = B/A_DIV operations; its measured window covers at least 2^(L-1) a
#              operations, so that it contains a maintenance of all levels
#   poh        Path Oblivious Heap, padded, with its map from keys to references at the client
#
# Workload: insertions, decrease-key, deletion by key, ExtractMin and FindMin. All queues hide the type
# of every operation, so the cost of an operation does not depend on its type, and the average time per
# operation is the cost of one oblivious operation.
#
# Environments (PROFILES), each with a round-trip time and a bandwidth:
#   tee     no network: storage in the same process (emulates a TEE with data in untrusted memory)
#   lan     1 ms, 1 Gbit/s
#   region  10 ms, 1 Gbit/s
#   wan     50 ms, 100 Mbit/s
#
# MODE=local (default, anywhere): the storage is in the same process, and network profiles are
# emulated without waiting: every read request is charged one round-trip time, and every request the
# transfer time of its bytes at the bandwidth; the charges are added to the time of each operation.
# All profiles are evaluated in a single run per configuration (run_workload --profiles), since the
# emulation only charges time; every run measures 10^6 operations (more for levelwise, see above).
# MODE=netem (Linux, needs sudo): network profiles run client and server in two network namespaces
# connected by a virtual Ethernet pair; after the warmup, `tc netem` delays every packet by RTT/2 in
# each direction and limits each direction to the bandwidth. Real waiting: fewer measured operations.
#
# Other variables: A_DIV [4], NS [1048576 8388608], BS (buffer sizes) [1024], SEEDS [1],
# QUEUES [streaming cached spread poh], PROFILES [tee lan region wan], OPS_SCALE [1] (multiplies the
# number of measured operations per profile), OPS_OVERRIDE (if set, the number of measured operations
# of every run; for quick tests), FILL [0.95] (occupancy at which measurements start), LAT_NS [all of NS]
# (the n for which every operation's time is written to lat/, for tail plots; MODE=local only).
set -euo pipefail

BIN=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
MODE=${MODE:-local}
A_DIV=${A_DIV:-4}
case $A_DIV in 4|8) ;; *) echo "A_DIV must be 4 or 8"; exit 1 ;; esac
NS=${NS:-"1048576 8388608"}
BS=${BS:-1024}
SEEDS=${SEEDS:-1}
QUEUES=${QUEUES:-"streaming cached spread poh"}
PROFILES=${PROFILES:-"tee lan region wan"}
OPS_SCALE=${OPS_SCALE:-1}
FILL=${FILL:-0.95}
LAT_NS=${LAT_NS:-$NS}
mkdir -p "$OUT/raw" "$OUT/lat" "$OUT/work" "$OUT/storage"
for q in $QUEUES; do case $q in streaming|cached|spread|levelwise|poh) ;; *) echo "unknown queue $q"; exit 1 ;; esac; done

profile() {  # profile <name>: prints "rtt_ms bandwidth_mbit measured_ops"
  if [ "$MODE" = local ]; then
    case $1 in
      tee)    echo "0 0 1000000" ;;
      lan)    echo "1 1000 1000000" ;;
      region) echo "10 1000 1000000" ;;
      wan)    echo "50 100 1000000" ;;
      *) echo "unknown profile $1" >&2; exit 1 ;;
    esac
    return
  fi
  case $1 in
    tee)    echo "0 0 1000000" ;;
    lan)    echo "1 1000 100000" ;;
    region) echo "10 1000 20000" ;;
    wan)    echo "50 100 10000" ;;
    *) echo "unknown profile $1" >&2; exit 1 ;;
  esac
}

for p in $PROFILES; do profile "$p" > /dev/null || exit 1; done  # catch typos before hours of runs
FIELDS="exit_code measured_ops warmup_ops total_ms avg_op_ms p50_op_ms p90_op_ms p99_op_ms p999_op_ms worst_op_ms max_round_trips_per_op max_objects_per_op round_trips objects_read objects_written bytes_down bytes_up store_ms network_ms encryption_ms peak_client_records peak_client_records_incl_map peak_stash position_map_entries peak_rss_bytes overflows mismatches trace_digest"
CSV="$OUT/results.csv"
HEADER="mode,profile,rtt_ms,bandwidth_mbit,n,B,a,queue,seed,$(echo $FIELDS | tr ' ' ',')"
# Rows are appended, so that an interrupted run can be resumed; the summary scripts keep the last row
# of every configuration. Refuse to mix results with a different column layout.
if [ -f "$CSV" ]; then
  [ "$(head -1 "$CSV")" = "$HEADER" ] || { echo "$CSV has a different header; use a fresh output directory"; exit 1; }
else
  echo "$HEADER" > "$CSV"
fi
# Never fails: a run that crashed before printing its statistics must not abort the grid.
field() { grep "^$2=" "$1" 2>/dev/null | head -1 | cut -d= -f2 || true; }
# One relocation interval per output directory, since the summary scripts do not distinguish runs by a.
if [ -f "$OUT/a_div" ]; then
  [ "$(cat "$OUT/a_div")" = "$A_DIV" ] || { echo "$OUT was used with A_DIV=$(cat "$OUT/a_div"); use a fresh output directory"; exit 1; }
else
  echo "$A_DIV" > "$OUT/a_div"
fi

# Provenance of every invocation: machine, compiler, library, code version.
{
  echo "== $(date -u +%Y-%m-%dT%H:%M:%SZ) MODE=$MODE A_DIV=$A_DIV NS=\"$NS\" BS=\"$BS\" SEEDS=\"$SEEDS\" QUEUES=\"$QUEUES\" PROFILES=\"$PROFILES\" FILL=$FILL"
  uname -a
  (sysctl -n machdep.cpu.brand_string hw.memsize 2>/dev/null || grep -m1 "model name" /proc/cpuinfo) | tr '\n' ' '; echo
  grep -E "^CMAKE_CXX_COMPILER:|^CMAKE_BUILD_TYPE:|^OPENSSL_CRYPTO_LIBRARY:" "$BIN/CMakeCache.txt" 2>/dev/null || true
  SRC=$(cd "$(dirname "$0")/.." && pwd)
  echo "code: $(git -C "$SRC" rev-parse HEAD 2>/dev/null) uncommitted diff sha1: $(git -C "$SRC" diff HEAD -- . 2>/dev/null | shasum | cut -c1-12)"
  git -C "$SRC" status --short -- . 2>/dev/null | head -50
} >> "$OUT/provenance.txt"
# The exact sources, since they may be uncommitted.
# Excludes the build and every results directory, including this output directory if it lies inside.
SRC=$(cd "$(dirname "$0")/.." && pwd)
EXCL=(--exclude ./build --exclude './bench/results*' --exclude './results*')
case $OUT in "$SRC"/*) EXCL+=(--exclude "./${OUT#"$SRC"/}") ;; esac
tar -czf "$OUT/source-$(date -u +%Y%m%dT%H%M%SZ).tgz" -C "$SRC" "${EXCL[@]}" .

# ------------------------------------------------------------------ network namespaces (MODE=netem)
NETNS=opq-server
CLIENT_IF=opq-veth0
SERVER_IF=opq-veth1
SERVER_IP=10.77.0.2
setup_netns() {
  sudo ip netns add $NETNS
  sudo ip link add $CLIENT_IF type veth peer name $SERVER_IF
  sudo ip link set $SERVER_IF netns $NETNS
  sudo ip addr add 10.77.0.1/24 dev $CLIENT_IF
  sudo ip link set $CLIENT_IF up
  sudo ip netns exec $NETNS ip addr add $SERVER_IP/24 dev $SERVER_IF
  sudo ip netns exec $NETNS ip link set $SERVER_IF up
  sudo ip netns exec $NETNS ip link set lo up
}
set_link() {  # set_link <rtt_ms> <mbit>: delay RTT/2 and rate limit in each direction
  local half
  half=$(python3 -c "print($1/2)")
  sudo tc qdisc replace dev $CLIENT_IF root netem delay "${half}ms" rate "${2}mbit" limit 1000000
  sudo ip netns exec $NETNS tc qdisc replace dev $SERVER_IF root netem delay "${half}ms" rate "${2}mbit" limit 1000000
}
clear_link() {
  sudo tc qdisc del dev $CLIENT_IF root 2>/dev/null || true
  sudo ip netns exec $NETNS tc qdisc del dev $SERVER_IF root 2>/dev/null || true
}
SERVER_PID=""
cleanup() {
  [ -n "$SERVER_PID" ] && sudo kill "$SERVER_PID" 2>/dev/null || true
  if [ "$MODE" = netem ]; then
    sudo ip link del $CLIENT_IF 2>/dev/null || true
    sudo ip netns del $NETNS 2>/dev/null || true
  fi
}
trap cleanup EXIT
if [ "$MODE" = netem ]; then cleanup; setup_netns; fi

# ------------------------------------------------------------------ one run
run_one() {  # run_one <name> <csv prefix> <profile> <warmup> <ops> <workload file prefix> <queue args...>
  local name=$1 prefix=$2 prof=$3 warm=$4 ops=$5 wl=$6; shift 6
  local raw="$OUT/raw/$name.txt" rtt mbit dummy
  read -r rtt mbit dummy <<< "$(profile "$prof")"
  local common=(--workload "$wl.w" --answers "$wl.a" --key-bytes 3 --prio-bytes 3 --warmup "$warm"
    --max-ops "$ops" --latency-file "$OUT/lat/$name.txt")
  echo "== $name"
  if [ "$MODE" = local ] || [ "$prof" = tee ]; then
    local rc=0
    "$BIN/run_workload" "$@" "${common[@]}" --dir "$OUT/storage" --rtt-us $((rtt * 1000)) \
      --bandwidth-mbit "$mbit" > "$raw" 2> "$raw.err" || rc=$?
  else
    local port=$((20000 + RANDOM % 20000)) ready="$OUT/storage/ready" go="$OUT/storage/go"
    rm -f "$ready" "$go"
    clear_link
    sudo ip netns exec $NETNS "$BIN/server" "$port" "$OUT/storage" --once > "$OUT/storage/server.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 100); do grep -q listening "$OUT/storage/server.log" 2>/dev/null && break; sleep 0.1; done
    local rc=0
    "$BIN/run_workload" "$@" "${common[@]}" --store remote --host $SERVER_IP --port "$port" \
      --ready-file "$ready" --go-file "$go" > "$raw" 2> "$raw.err" &
    local client=$!
    while [ ! -f "$ready" ] && kill -0 $client 2>/dev/null; do sleep 0.2; done
    set_link "$rtt" "$mbit"
    touch "$go"
    wait $client || rc=$?
    clear_link
    wait $SERVER_PID 2>/dev/null || true
    SERVER_PID=""
  fi
  # Exit codes of run_workload: 0 ok, 1 error, 2 wrong answers, 3 buffer overflows. Every run is
  # recorded; anything but a clean, complete run is reported loudly.
  local line="$prefix,$rc"
  for f in ${FIELDS#exit_code }; do line="$line,$(field "$raw" "$f")"; done
  echo "$line" >> "$CSV"
  local measured
  measured=$(field "$raw" measured_ops)
  if [ "$rc" != 0 ] || [ "${measured:-0}" != "$ops" ]; then
    echo "WARNING: $name exited with $rc after ${measured:-0} of $ops measured operations; see $raw.err" >&2
  fi
  if [ "$MODE" = netem ]; then sudo rm -f "$OUT"/storage/*.t[0-9]; else rm -f "$OUT"/storage/*.t[0-9]; fi
}

# ------------------------------------------------------------------ one run, all profiles (MODE=local)
PERPROF=" total_ms avg_op_ms p50_op_ms p90_op_ms p99_op_ms p999_op_ms worst_op_ms network_ms "
PROFSPEC=""
for p in $PROFILES; do read -r rtt mbit dummy <<< "$(profile "$p")"; PROFSPEC="$PROFSPEC${PROFSPEC:+,}$p:$((rtt * 1000)):$mbit"; done
run_multi() {  # run_multi <name head> <name tail> <csv: n,B,a,queue,seed> <warmup> <ops> <workload prefix> <queue args...>
  local head=$1 tail=$2 cols=$3 warm=$4 ops=$5 wl=$6; shift 6
  local raw="$OUT/raw/$head-all-$tail.txt" rc=0
  echo "== $head-$tail ($ops measured operations, profiles $PROFILES)"
  local n=${cols%%,*} lat=()
  case " $LAT_NS " in *" $n "*) lat=(--latency-file "$OUT/lat/$head-$tail") ;; esac
  "$BIN/run_workload" "$@" --workload "$wl.w" --answers "$wl.a" --key-bytes 3 --prio-bytes 3 --warmup "$warm" \
    --max-ops "$ops" --dir "$OUT/storage" --profiles "$PROFSPEC" "${lat[@]}" > "$raw" 2> "$raw.err" || rc=$?
  local prof rtt mbit dummy f line
  for prof in $PROFILES; do
    read -r rtt mbit dummy <<< "$(profile "$prof")"
    line="$MODE,$prof,$rtt,$mbit,$cols,$rc"
    for f in ${FIELDS#exit_code }; do
      case $PERPROF in *" $f "*) line="$line,$(field "$raw" "$prof.$f")" ;; *) line="$line,$(field "$raw" "$f")" ;; esac
    done
    echo "$line" >> "$CSV"
    [ -f "$OUT/lat/$head-$tail-$prof.txt" ] && mv "$OUT/lat/$head-$tail-$prof.txt" "$OUT/lat/$head-$prof-$tail.txt"
  done
  local measured
  measured=$(field "$raw" measured_ops)
  if [ "$rc" != 0 ] || [ "${measured:-0}" != "$ops" ]; then
    echo "WARNING: $head-$tail exited with $rc after ${measured:-0} of $ops measured operations; see $raw.err" >&2
  fi
  rm -f "$OUT"/storage/*.t[0-9]
}
depth() { local L=0; while [ $(( $2 << L )) -lt $(( 4 * $1 )) ]; do L=$((L + 1)); done; echo $L; }  # depth <n> <B>

# ------------------------------------------------------------------ the grid
BASE_OPS=$((1000000 * OPS_SCALE)); [ -n "${OPS_OVERRIDE:-}" ] && BASE_OPS=$OPS_OVERRIDE
for n in $NS; do
  # Longest measured window: levelwise needs 2^(L-1) a operations to contain a maintenance of all levels.
  longest=$BASE_OPS
  case " $QUEUES " in *" levelwise "*)
    for B in $BS; do L=$(depth "$n" "$B"); P=$(( (1 << (L - 1)) * (B / A_DIV) )); [ "$P" -gt "$longest" ] && longest=$P; done ;;
  esac
  for seed in $SEEDS; do
    target=$(python3 -c "print(int($n*$FILL))")
    wl="$OUT/work/n$n-s$seed"
    if [ ! -f "$wl.gen" ]; then
      "$BIN/generator" --ops $((target * 3 / 2 + longest)) --size "$target" --key-bits 24 \
        --prio-bits 24 --seed "$seed" --findmin-percent 10 --out "$wl.w" --answers "$wl.a" > "$wl.gen"
    fi
    warm=$(field "$wl.gen" fill_ops)
    [ "$(field "$wl.gen" max_present)" -le "$n" ] || { echo "workload exceeds n"; exit 1; }
    [ "$warm" -gt 0 ] || { echo "workload never reaches the target size; measurements would start from a non-full queue"; exit 1; }
    if [ "$MODE" = local ]; then
      for B in $BS; do
        for q in $QUEUES; do
          ops=$BASE_OPS
          case $q in
            poh)       continue ;;
            streaming) a=$((B / A_DIV)); args=(--queue pq --mode streaming --n "$n" --B "$B" --a "$a") ;;
            cached)    a=$((B / A_DIV)); args=(--queue pq --mode cached --n "$n" --B "$B" --a "$a") ;;
            levelwise) a=$((B / A_DIV)); args=(--queue pq --mode levelwise --n "$n" --B "$B" --a "$a")
                       L=$(depth "$n" "$B"); P=$(( (1 << (L - 1)) * a )); [ "$P" -gt "$ops" ] && ops=$P ;;
            spread)    a=$((B / 8)); args=(--queue spread --n "$n" --B "$B") ;;
          esac
          run_multi "$q-n$n-B$B-a$a" "s$seed" "$n,$B,$a,$q,$seed" "$warm" "$ops" "$wl" "${args[@]}"
        done
      done
      case " $QUEUES " in *" poh "*)  # no buffer size: one run; fresh random leaves (--seed would select the insecure test generator)
        run_multi "poh-n$n" "s$seed" "$n,,,poh,$seed" "$warm" "$BASE_OPS" "$wl" --queue poh --z 2 --n "$n" ;;
      esac
      continue
    fi
    for prof in $PROFILES; do
      read -r rtt mbit ops <<< "$(profile "$prof")"
      ops=$((ops * OPS_SCALE)); [ -n "${OPS_OVERRIDE:-}" ] && ops=$OPS_OVERRIDE
      for B in $BS; do
        for q in $QUEUES; do
          [ "$q" = poh ] && continue
          case $q in
            streaming) a=$((B / A_DIV)); args=(--queue pq --mode streaming --n "$n" --B "$B" --a "$a") ;;
            cached)    a=$((B / A_DIV)); args=(--queue pq --mode cached --n "$n" --B "$B" --a "$a") ;;
            levelwise) a=$((B / A_DIV)); args=(--queue pq --mode levelwise --n "$n" --B "$B" --a "$a") ;;
            spread)    a=$((B / 8)); args=(--queue spread --n "$n" --B "$B") ;;
          esac
          run_one "$q-n$n-B$B-a$a-$prof-s$seed" "$MODE,$prof,$rtt,$mbit,$n,$B,$a,$q,$seed" "$prof" "$warm" "$ops" "$wl" "${args[@]}"
        done
      done
      # Path Heap has no buffer size: one run per profile.
      case " $QUEUES " in *" poh "*)
        run_one "poh-n$n-$prof-s$seed" "$MODE,$prof,$rtt,$mbit,$n,,,poh,$seed" "$prof" "$warm" "$ops" "$wl" \
          --queue poh --z 2 --n "$n"  # fresh random leaves; --seed would select the insecure test generator
      esac
    done
  done
done
echo "results in $CSV; summary: python3 bench/summarize.py $CSV"
