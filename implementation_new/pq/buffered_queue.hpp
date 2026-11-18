#pragma once

// Our data-independent priority queue (Section 3 of the paper): operations act on the root, and
// after every a operations the client performs one complete path relocation. By default a = B/8
// (Theorem 1); every a <= B/4 is admissible for a (pseudo)random routing function (Theorem 3 and
// Corollary 2 in Appendix C), which halves the amortized cost for a = B/4.
//
// Relocation modes:
//   Cached    : read the 2L buckets of the path and its siblings in one request, process them,
//               write them back in one message. Client memory at most 3B/2 + 3B(L-1) + 2B records.
//   Streaming : process the path call by call, holding at most three buckets besides the root
//               (Lemma 1). Client memory at most 6B records, 2L-1 round trips per relocation.
//   Levelwise : baseline, not our construction: the level-wise schedule of Jafargholi et al. (SODA'21)
//               on the same tree and buffers (BufferTree::levelwise_maintain); every a operations, it
//               maintains whole levels, and once every 2^(L-1) a operations all of them.
//
// Between relocations, the root is kept in a hash map with an ordered index, so that operations take
// O(log B) time; at a relocation it is handed to the tree as sorted buffers and taken back afterwards.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tree_queue.hpp"

namespace opq {

enum class RelocationMode { Cached, Streaming, Levelwise };

class BufferedQueue : public TreeQueue {
   public:
    struct Params : TreeQueueParams {
        RelocationMode mode = RelocationMode::Streaming;
        uint64_t a = 0;  // operations between two relocations; 0 means B/8
    };

    BufferedQueue(Store& store, const Params& p) : TreeQueue(store, p, 8), mode_(p.mode) {
        a_ = p.a == 0 ? B_ / 8 : p.a;
        if (a_ < 1 || a_ > B_ / 4) throw std::invalid_argument("the relocation interval a must satisfy 1 <= a <= B/4");
        tree_.set_base_records(B_ + tree_.delete_capacity());  // the root buffers
    }

    // ---------------------------------------------------------------- operations (Figure 1)
    void insert(uint64_t key, uint64_t priority) override {
        check_key(key);
        check_priority(priority);
        auto it = E_.find(key);
        if (it == E_.end()) {
            E_.emplace(key, Slot{priority, tree_.route(key)});
            order_.insert({priority, key});
            if (E_.size() > B_) truncate_root_pairs();
        } else if (priority < it->second.prio) {
            order_.erase({it->second.prio, key});
            it->second.prio = priority;
            order_.insert({priority, key});
        }
        tick();
    }

    void erase(uint64_t key) override {
        check_key(key);
        remove_pair(key);
        add_marker(key);
        tick();
    }

    std::optional<Pair> find_min() override {
        std::optional<Pair> result;
        if (!order_.empty()) result = Pair{order_.begin()->second, order_.begin()->first};
        tick();
        return result;
    }

    std::optional<Pair> extract_min() override {
        std::optional<Pair> result;
        if (!order_.empty()) {
            result = Pair{order_.begin()->second, order_.begin()->first};
            remove_pair(result->first);
            add_marker(result->first);
        }
        tick();
        return result;
    }

    std::string describe() const override {
        return "buffered queue, B=" + std::to_string(B_) + ", a=" + std::to_string(a_) + ", L=" + std::to_string(tree_.height()) + ", mode=" +
               (mode_ == RelocationMode::Cached ? "cached" : mode_ == RelocationMode::Levelwise ? "levelwise" : "streaming");
    }

   private:
    struct Slot {
        uint64_t prio, h;
    };

    void remove_pair(uint64_t key) {
        auto it = E_.find(key);
        if (it != E_.end()) {
            order_.erase({it->second.prio, key});
            E_.erase(it);
        }
    }

    void add_marker(uint64_t key) {
        D_.insert(key);
        const uint64_t cap = tree_.delete_capacity();
        if (D_.size() > cap) {  // overflow rule: keep the B/2 smallest keys
            std::vector<uint64_t> keys(D_.begin(), D_.end());
            std::nth_element(keys.begin(), keys.begin() + static_cast<long>(cap), keys.end());
            for (size_t i = cap; i < keys.size(); i++) D_.erase(keys[i]);
            tree_.count_overflow();
        }
    }

    void truncate_root_pairs() {  // overflow rule: keep the B smallest keys
        std::vector<uint64_t> keys;
        keys.reserve(E_.size());
        for (const auto& kv : E_) keys.push_back(kv.first);
        std::nth_element(keys.begin(), keys.begin() + static_cast<long>(B_), keys.end());
        for (size_t i = B_; i < keys.size(); i++) remove_pair(keys[i]);
        tree_.count_overflow();
    }

    void tick() {
        t_++;
        if (t_ % a_ == 0) relocate(t_ / a_ - 1);
    }

    void relocate(uint64_t J) {
        // Hand the root to the tree as sorted buffers.
        BufferTree::Buffer& root = tree_.root;
        root.clear();
        root.E.reserve(E_.size());
        for (const auto& [k, s] : E_) root.E.push_back(BufferTree::Entry{k, s.prio, s.h});
        std::sort(root.E.begin(), root.E.end(), [](const auto& x, const auto& y) { return x.key < y.key; });
        root.D.assign(D_.begin(), D_.end());
        std::sort(root.D.begin(), root.D.end());

        if (mode_ == RelocationMode::Cached) {
            tree_.relocate_cached(J);
        } else if (mode_ == RelocationMode::Levelwise) {
            tree_.levelwise_maintain(J + 1);
        } else {
            for (unsigned l = 1; l <= tree_.calls_per_relocation(); l++) tree_.streaming_call(J, l);
        }

        // Take it back: PushDown(root) emptied it, and PullUp(root) refilled E_rho.
        E_.clear();
        D_.clear();
        order_.clear();
        for (const auto& e : root.E) {
            E_.emplace(e.key, Slot{e.prio, e.h});
            order_.insert({e.prio, e.key});
        }
        for (uint64_t k : root.D) D_.insert(k);
        root.clear();
    }

    RelocationMode mode_;
    uint64_t a_ = 0;
    std::unordered_map<uint64_t, Slot> E_;              // E_rho
    std::unordered_set<uint64_t> D_;                     // D_rho
    std::set<std::pair<uint64_t, uint64_t>> order_;      // (priority, key) of E_rho
};

}  // namespace opq
