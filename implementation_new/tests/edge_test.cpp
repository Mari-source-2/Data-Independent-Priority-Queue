// Edge cases that the random workloads rarely hit. Every check compares against a reference.

#include <iostream>
#include <map>
#include <set>
#include <string>

#include "../pq/buffered_queue.hpp"
#include "../pq/spread_queue.hpp"
#include "../poh/path_heap.hpp"

using namespace opq;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) failures++;
}

// Fills the queue to exactly n keys, then decreases the priority of every key and extracts all.
bool full_then_decrease(PriorityQueue& q, uint64_t n) {
    for (uint64_t k = 1; k <= n; k++) q.insert(k, 2 * n + k);
    for (uint64_t k = 1; k <= n; k++) q.insert(k, n + 1 - k);  // decrease-key at full occupancy
    for (uint64_t i = 0; i < n; i++) {
        auto m = q.extract_min();
        if (!m || m->first != n - i || m->second != i + 1) return false;
    }
    return !q.extract_min();
}

// Deleting absent keys, re-inserting deleted keys, and queries on an empty queue.
bool delete_reinsert(PriorityQueue& q) {
    if (q.find_min() || q.extract_min()) return false;
    q.erase(7);  // absent key
    q.insert(7, 50);
    q.erase(7);
    q.insert(7, 60);  // re-inserted after a delete: the old marker must not remove it
    auto m = q.find_min();
    if (!m || *m != Pair{7, 60}) return false;
    q.insert(7, 70);  // larger priority: no change
    m = q.extract_min();
    return m && *m == Pair{7, 60} && !q.find_min();
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string dir = argc > 1 ? argv[1] : ".";
    for (uint64_t n : {16ULL, 1000ULL}) {  // a power of two and a non-power of two
        {
            LocalStore store;
            PathHeap::Params p;
            p.n = n;
            p.key_bytes = 2;
            p.prio_bytes = 2;
            p.name = dir + "/edge_poh";
            p.enc_key = random_key();
            p.seed = 3;
            PathHeap q(store, p);
            check(full_then_decrease(q, n),
                  "poh n=" + std::to_string(n) + ": decrease-key at full occupancy");
        }
        {
            LocalStore store;
            PathHeap::Params p;
            p.n = n;
            p.key_bytes = 2;
            p.prio_bytes = 2;
            p.name = dir + "/edge_poh";
            p.enc_key = random_key();
            PathHeap q(store, p);
            check(delete_reinsert(q), "poh n=" + std::to_string(n) + ": delete, re-insert, empty queries");
        }
    }
    for (uint64_t n : {1024ULL, 1000ULL}) {
        for (auto mode : {RelocationMode::Cached, RelocationMode::Streaming}) {
            for (int test = 0; test < 2; test++) {
                LocalStore store;
                BufferedQueue::Params p;
                p.n = n;
                p.B = 64;
                p.key_bytes = 2;
                p.prio_bytes = 2;
                p.mode = mode;
                p.name = dir + "/edge_pq";
                p.enc_key = random_key();
                p.prf_key = random_key();
                BufferedQueue q(store, p);
                std::string m = mode == RelocationMode::Cached ? "cached" : "streaming";
                if (test == 0)
                    check(full_then_decrease(q, n) && q.stats().overflows == 0,
                          "pq n=" + std::to_string(n) + " " + m + ": decrease-key at full occupancy");
                else
                    check(delete_reinsert(q), "pq n=" + std::to_string(n) + " " + m + ": delete, re-insert, empty queries");
            }
        }
    }
    for (uint64_t n : {1024ULL, 1000ULL}) {
        for (int test = 0; test < 2; test++) {
            LocalStore store;
            SpreadQueue::Params p;
            p.n = n;
            p.B = 64;
            p.key_bytes = 2;
            p.prio_bytes = 2;
            p.name = dir + "/edge_spread";
            p.enc_key = random_key();
            p.prf_key = random_key();
            SpreadQueue q(store, p);
            if (test == 0)
                check(full_then_decrease(q, n) && q.stats().overflows == 0,
                      "spread n=" + std::to_string(n) + ": decrease-key at full occupancy");
            else
                check(delete_reinsert(q), "spread n=" + std::to_string(n) + ": delete, re-insert, empty queries");
        }
    }
    std::cout << (failures == 0 ? "ALL EDGE TESTS PASSED" : "SOME EDGE TESTS FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
