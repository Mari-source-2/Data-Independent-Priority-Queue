# Data-independent priority queue: implementation

This directory contains our two constructions, the buffered queue of Section 3 of the paper and its variant
with spread relocations from Appendix B, the Path Oblivious Heap baseline, a
bucket server, a workload generator with reference answers, and the benchmark drivers. The old code is in
`../implementation_old`.

## Build

Requires CMake ≥ 3.16, a C++20 compiler and OpenSSL 3.

```sh
cmake -S . -B build -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3   # path only needed on macOS/Homebrew
cmake --build build -j
tests/run_tests.sh build                                            # or: ctest --test-dir build
```

## Layout

| Path | Content |
|---|---|
| `pq/buffer_tree.hpp` | tree of buffers shared by both constructions: buckets, PushDown/PullUp, path relocations |
| `pq/tree_queue.hpp` | parameters and helpers shared by both constructions |
| `pq/buffered_queue.hpp` | Section 3: one complete relocation after every a operations (Figure 1; a = B/8 by default) |
| `pq/spread_queue.hpp` | Appendix B: every relocation spread over the a = B/8 operations that follow it |
| `poh/path_heap.hpp` | Path Oblivious Heap (Shi, S&P'20), path variant, padded |
| `common/store.hpp` | outsourced storage: memory-mapped tables, in-process store and network client, protocol limits, statistics |
| `common/server.cpp` | bucket server (tables of fixed-size encrypted objects in files) |
| `common/crypto.hpp` | AES-128 PRF for routing, AES-CTR generator, AES-128-GCM for objects, key derivation and key files |
| `generator/` | workload generator with reference answers |
| `apps/run_workload.cpp` | runs a workload, checks every answer, prints statistics |
| `apps/dijkstra.cpp` | Dijkstra on a DIMACS graph, checked against a plain Dijkstra |
| `tests/` | correctness, data-independence, edge cases, tamper and replay detection |
| `bench/run_benchmarks.sh`, `bench/summarize.py`, `bench/plotdata.py` | benchmarks at equal client memory in four environments, their summary, and data files for the paper's figures |

## Running

```sh
# workload: 10^6 operations, about 2^20 present keys, 3-byte keys and priorities
build/generator --ops 1000000 --size 1000000 --key-bits 24 --prio-bits 24 --seed 1 \
                --out w.txt --answers a.txt          # prints max_present; use n >= max_present

# our queue, in-process store
build/run_workload --queue pq --n 1048576 --B 1024 --mode streaming --workload w.txt --answers a.txt

# our queue against a server (start the server first, possibly on another machine)
build/server 5001 /path/to/storage &
build/run_workload --queue pq --n 1048576 --B 1024 --store remote --host 127.0.0.1 --port 5001 \
                   --workload w.txt --answers a.txt

# our queue with spread relocations (Appendix B)
build/run_workload --queue spread --n 1048576 --B 1024 --workload w.txt --answers a.txt

# Path Oblivious Heap
build/run_workload --queue poh --n 1048576 --z 2 --workload w.txt --answers a.txt

# Dijkstra
build/dijkstra --queue pq --B 1024 --graph USA-road-d.NY.gr --source 1 --key-bytes 3 --prio-bytes 5
```

Common options: `--key-bytes`, `--prio-bytes` (field widths), `--store local|remote`, `--host`, `--port`,
`--dir` (where the in-process store puts its files), `--keyfile` (persistent master key),
`--max-ops` (number of measured operations), `--rtt-us` and `--bandwidth-mbit` (network emulation of
the in-process store: every read request is charged the round-trip time and every request the transfer
time of its bytes; the charge is added to the time of the operation, nothing waits), `--warmup K` (execute the first K operations without measuring
them, e.g. the generator's `fill_ops`), `--ready-file`/`--go-file` (pause after the warmup until the
go file exists), `--latency-file` (write every measured operation's time in microseconds).

### Our queue

- `--n`: upper bound on the number of present keys; `--B`: capacity of an element buffer in records
  (multiple of 8, at most n; the analysis assumes n ≥ 2B). Delete buffers have capacity B/2, so an internal
  bucket has 3B/2 records. The tree has depth L = ceil(log2(4n/B)).
- `--a`: operations between two relocations (only for `--queue pq`; the spread queue uses a = B/8),
  default B/8 (Theorem 1), at most B/4 (Theorem 3 and
  Corollary 2 for a pseudorandom routing function, which the implementation uses).
- `--mode streaming` (default): a relocation processes its path one call at a time and holds at most three
  buckets besides the root, i.e. at most 6B records (Lemma 1), with 2L-1 round trips per relocation.
  It does not write back or re-read the path node that PushDown has just emptied, so a relocation
  transfers 3L-1 buckets in each direction.
- `--mode cached`: a relocation reads its 2L buckets in one round trip and writes them in one message;
  the client holds at most 3B/2 + 3B(L-1) + 2B records during a relocation (root plus 2L buckets).

Every bucket is authenticated together with a random instance identifier (chosen when the tables are
created), its location and a version that is a function of the relocation counter. So a server that swaps
buckets, replays an old version, or serves buckets of an earlier session under the same keys is detected,
without per-bucket client state.

### Our queue with spread relocations

- `--queue spread`, `--n`, `--B` (multiple of 8, at most n).
- Relocation j is triggered after operation (j+1)a, with a = B/8, and every operation of the following a
  operations executes the next ⌈2L/a⌉ of its 2L calls, streaming as above. Meanwhile, operations act on a
  front and answer queries from a snapshot of the root; the front is merged into the root at the end of
  the period. Every operation thus transfers at most 3⌈2L/a⌉ buckets in ⌈2L/a⌉ round trips.
- The client holds the root, the snapshot, the front and at most three buckets: at most 55B/8 records.

### Path Oblivious Heap

- `--n`, `--z` (bucket size), `--seed` (nonzero: reproducible but **insecure** leaves, for tests only).
- Leaves are drawn from AES-CTR under a fresh random key: the server observes every accessed leaf, so a
  predictable generator would reveal the operation types and link deletions to insertions.
- Every operation performs a delete path and two eviction paths, as in the padded variant of Shi's paper,
  so that operation types are hidden and the baseline is at least oblivious. Every operation takes exactly
  one round trip. The unpadded variant reveals the operation types and is not implemented.
- Unlike our queues, Path Oblivious Heap is oblivious but not data-independent: its accesses follow random
  leaves that must stay secret.
- Path Oblivious Heap addresses elements by references, so the key-based interface needs a map from keys to
  references at the client. Its size is reported separately as `position_map_entries` and is **not**
  included in `peak_client_records`; `peak_client_records_incl_map` adds it. The map has one entry per
  present key.

## Output

`run_workload` and `dijkstra` print `key=value` lines:

| Field | Meaning |
|---|---|
| `round_trips`, `write_messages` | read requests (each waits for a reply) and write messages |
| `objects_read`, `objects_written`, `bytes_down`, `bytes_up` | communication volume |
| `trace_digest` | hash of every request (kind, number of objects, objects); equal for equal operation counts (data-independence) |
| `overflows` | buffer truncations by the overflow rule; must be 0 |
| `peak_client_records` | peak number of record slots held by the client (excluding the position map) |
| `peak_client_records_incl_map` | the same plus the position map of Path Oblivious Heap |
| `worst_op_ms`, `max_objects_per_op`, `max_round_trips_per_op` | worst case over single operations (`run_workload`) |
| `p50_op_ms`, `p90_op_ms`, `p99_op_ms`, `p999_op_ms` | percentiles of the per-operation time |
| `histogram_us` | number of operations whose time lies in [2^k, 2^(k+1)) microseconds |
| `store_ms`, `network_ms`, `encryption_ms` | time spent in storage requests (network, server), network delay charged by the emulation (included in the operation times, not in `store_ms`), and time spent in encryption |
| `total_ms`, `avg_op_ms`, `wall_ms` | time spent in the measured queue operations (parsing and answer checks excluded), its average, and the wall-clock time of the measured phase |
| `prf_evaluations` | evaluations of the routing function (our queues) |
| `warmup_ops`, `measured_ops` | unmeasured and measured operations; all statistics refer to the measured ones |
| `peak_stash`, `position_map_entries` | Path Oblivious Heap: peak root size and peak map size |
| `peak_rss_bytes` | peak resident memory of the client process |
| `mismatches` / `wrong_distances` | wrong answers |

Exit status: 0 success, 1 error (including an unknown option), 2 wrong answers, 3 buffer overflows.

## Benchmarks

```sh
# the runs of the paper (A_DIV=4: a = B/4), all into results/
NS="65536 131072 262144 524288 1048576 2097152 4194304 8388608" BS=1024 A_DIV=4 LAT_NS="1048576 8388608" \
  QUEUES="streaming cached spread levelwise poh" bench/run_benchmarks.sh build results/          # sweep over n
NS=8388608 BS="256 512 2048 4096" A_DIV=4 LAT_NS=0 QUEUES="streaming cached spread levelwise" \
  bench/run_benchmarks.sh build results/                                                       # sweep over B
bench/run_dijkstra.sh build results/dijkstra USA-road-d.NY.gr USA-road-d.FLA.gr               # DIMACS road networks
python3 bench/summarize.py results/results.csv
cp results/results.csv results/dijkstra/dijkstra.csv <paper>/data/bench/   # tracked in the paper repository
python3 bench/paperdata.py <paper>/data/bench <paper>/data/bench <paper>/data/bench/dijkstra.csv  # figure data, tables, summary
```

The road networks are those of the 9th DIMACS Implementation Challenge (`USA-road-d.*.gr`); the
challenge's site no longer serves them, but the Internet Archive has copies. `bench/plotdata.py` writes
further data files (tails of the time per operation, with `--all`).

- **Names in the paper.** `streaming` = PRQ-Stream, `cached` = PRQ-Batch, `spread` = PRQ-Spread (PRQ: path-relocation
  queue, our construction), `levelwise` = JLS (the schedule of Jafargholi, Larsen, and Simkin), `poh` = Path Heap.
- **Queues.** Our queue with buffer size B (default 1024) in three configurations: `streaming` (the
  client holds a constant number of buckets, at most 6B records), `cached` (it downloads and holds a
  whole path per relocation, 2L buckets) and `spread` (Appendix B, a = B/8, at most 55B/8 records);
  `streaming` and `cached` relocate every a = B/A_DIV operations, A_DIV = 4 (default) or 8;
  the baseline `levelwise`, the schedule of Jafargholi et al. (SODA'21) on the same tree and buffers
  (`--mode levelwise`), whose measured window covers at least 2^(L-1) a operations so that it contains a
  maintenance of all levels; and Path Heap, padded, with its map from keys to references at the client.
- **Workload.** Insertions, decrease-key, deletion by key, ExtractMin and FindMin. All queues hide the
  type of every operation, so the average time per operation is the cost of one oblivious operation.
- **Environments.** `tee`: no network (in-process storage; emulates a TEE with data in untrusted
  memory). `lan`: 1 ms round trip, 1 Gbit/s. `region`: 10 ms, 1 Gbit/s. `wan`: 50 ms, 100 Mbit/s.
  The bandwidth limit matters because our relocations transfer many buckets at once.
- **Network emulation.** The default `MODE=local` (used for the paper) keeps the storage in the client's
  process and charges every read request one round-trip time and every request the transfer time of its
  bytes at the bandwidth (writes conservatively too); the charges are added to the operation times, so
  the emulation is exact and costs no waiting, and one run per configuration evaluates all environments
  (`run_workload --profiles`). It ignores TCP effects such as congestion control. With
  `MODE=netem` (Linux, passwordless `sudo` for `ip` and `tc`), client and server instead run in two
  network namespaces connected by a virtual Ethernet pair, and `tc netem` delays every packet by half the
  round-trip time and limits the bandwidth in each direction.
- **Steady state.** Every run first fills the queue to 95% of n without measuring (`--warmup`); the
  network emulation starts after that. Measured operations: 10^6 in every environment with
  `MODE=local`; with `MODE=netem`, 10^6 without network down to 10^4 at 50 ms.
- **Checks.** Every answer is compared with the reference answer. The script refuses workloads that never
  reach the target size, records the exit status of every run, and warns about runs that fail or measure
  fewer operations than requested; the summary scripts skip failed runs, keep the last row of a repeated
  configuration and average over seeds.

The server tables for n = 2^23 take up to about 1.5 GB of disk space (Path Heap is the largest; ours
about 0.45 GB); the script deletes them after every run.
Tables are memory-mapped files, so reading or writing an object is a memory copy; for the TEE setting,
put the output directory on a RAM disk (e.g. `/dev/shm`) to avoid disk write-back.

## Choosing parameters

The client holds at most 6B records in streaming mode, 55B/8 with spread relocations and
3B/2 + 3B(L-1) + 2B in cached mode, with L = ceil(log2(4n/B)). For correctness, Theorems 1 and 3 of the
paper (a = B/8, and every a ≤ B/4 with a (pseudo)random routing function) require B ≥ 8⌈log(16Nn/δ)⌉;
that is B ≥ 648 for N = 2^24, n = 2^23 and δ = 2^-30, and the default B = 1024 satisfies this.

## Known limitations

- Only the master key is persisted (`--keyfile`); reloading a saved tree is not implemented.
- `dijkstra` keeps the graph and the distance array at the client, so it does not respect a small client
  budget; it measures the queue under a Dijkstra workload.
- Path Oblivious Heap implements every insertion of a present key as delete plus insert, even if the
  priority does not decrease.
- Path Oblivious Heap authenticates objects with the instance identifier and their location but without a
  version (that would need per-bucket client state), so replays within a session are not detected: against
  the server, the baseline is secure in the honest-but-curious setting only.
- The client's computation time depends on the contents of the buckets; the access pattern and the message
  sizes do not. Timing of the client's computation is outside the model.
