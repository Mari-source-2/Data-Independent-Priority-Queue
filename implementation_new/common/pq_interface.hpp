#pragma once

// Common interface of both priority queues, used by the workload runner and the Dijkstra driver.
// Semantics (Definition 1 of the paper): keys are integers >= 1, every key occurs at most once,
// Insert(k, p) sets the priority of k to min(old priority, p) and thereby also implements
// DecreaseKey, Delete(k) removes k, FindMin returns the minimum pair, ExtractMin returns and
// removes it. Pairs are ordered by priority, ties broken by key.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "memory.hpp"
#include "store.hpp"

namespace opq {

using Pair = std::pair<uint64_t, uint64_t>;  // (key, priority)

struct QueueStats {
    uint64_t operations = 0;
    uint64_t overflows = 0;              // buffer truncations (should stay 0; see Lemma 2 and Appendix C)
    uint64_t peak_client_records = 0;    // peak number of record slots held by the client
    uint64_t peak_stash = 0;             // Path Oblivious Heap only: peak number of elements at the root
    uint64_t position_map_entries = 0;   // Path Oblivious Heap only: peak size of the key -> reference map
    uint64_t encryption_ns = 0;          // time spent encrypting and decrypting objects
    uint64_t prf_evaluations = 0;        // evaluations of the routing function (our queues)
};

class PriorityQueue {
   public:
    virtual ~PriorityQueue() = default;
    virtual void insert(uint64_t key, uint64_t priority) = 0;
    virtual void erase(uint64_t key) = 0;
    virtual std::optional<Pair> find_min() = 0;
    virtual std::optional<Pair> extract_min() = 0;
    virtual QueueStats stats() const = 0;
    virtual std::string describe() const = 0;
};

}  // namespace opq
