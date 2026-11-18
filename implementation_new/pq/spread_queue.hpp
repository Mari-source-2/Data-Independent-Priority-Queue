#pragma once

// Our queue with spread relocations (Appendix B of the paper).
//
// With a = B/8 as in Section 3, relocation j is triggered after operation (j+1)a and runs during the operations
// (j+1)a+1, ..., (j+2)a, its period: every operation of the period executes the next c = ceil(2L/a)
// of its 2L calls (streaming, at most three buckets besides the root). The relocation works on the
// root buffers E_rho, D_rho. Meanwhile, operations act on a front (E_G, D_G) and answer queries from
// a snapshot of E_rho taken when the relocation was triggered:
//   FindMin returns min((snapshot minus D_G) merged with E_G).
// After operation (j+2)a, the front is merged into the root, E_rho <- (E_rho minus D_G) merged with E_G
// and D_rho <- D_rho united with D_G, and the snapshot for relocation j+1 is taken.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tree_queue.hpp"

namespace opq {

class SpreadQueue : public TreeQueue {
   public:
    struct Params : TreeQueueParams {
        Params() { name = "spread"; }
    };

    SpreadQueue(Store& store, const Params& p) : TreeQueue(store, p, 8) {
        a_ = B_ / 8;
        calls_ = tree_.calls_per_relocation();
        c_ = (calls_ + a_ - 1) / a_;
        // Root buffers (capacities B and B/2), snapshot (at most B/2 + a pairs without overflow, the
        // bound in the proof of Theorem 2 that the paper uses) and front (at most a entries in each buffer).
        tree_.set_base_records(B_ + tree_.delete_capacity() + (B_ / 2 + a_) + 2 * a_);
    }

    // ---------------------------------------------------------------- operations
    void insert(uint64_t key, uint64_t priority) override {
        check_key(key);
        check_priority(priority);
        auto it = front_E_.find(key);
        if (it == front_E_.end()) {
            front_E_.emplace(key, priority);
            front_order_.insert({priority, key});
        } else if (priority < it->second) {
            front_order_.erase({it->second, key});
            it->second = priority;
            front_order_.insert({priority, key});
        }
        finish_operation();
    }

    void erase(uint64_t key) override {
        check_key(key);
        front_delete(key);
        finish_operation();
    }

    std::optional<Pair> find_min() override {
        std::optional<Pair> r = query();
        finish_operation();
        return r;
    }

    std::optional<Pair> extract_min() override {
        std::optional<Pair> r = query();
        if (r) front_delete(r->first);
        finish_operation();
        return r;
    }

    std::string describe() const override {
        return "spread queue, B=" + std::to_string(B_) + ", a=" + std::to_string(a_) + ", L=" + std::to_string(tree_.height()) +
               ", calls per operation=" + std::to_string(c_);
    }

   private:
    void front_delete(uint64_t key) {
        auto it = front_E_.find(key);
        if (it != front_E_.end()) {
            front_order_.erase({it->second, key});
            front_E_.erase(it);
        }
        front_D_.insert(key);
    }

    // min((snapshot minus D_G) merged with E_G): the smaller of the smallest snapshot pair whose key
    // is not in D_G and the smallest pair of E_G. At most |D_G| <= a snapshot pairs are skipped.
    std::optional<Pair> query() const {
        std::optional<std::pair<uint64_t, uint64_t>> best;  // (priority, key)
        for (const auto& pk : snap_order_) {
            if (!front_D_.count(pk.second)) {
                best = pk;
                break;
            }
        }
        if (!front_order_.empty() && (!best || *front_order_.begin() < *best)) best = *front_order_.begin();
        if (!best) return std::nullopt;
        return Pair{best->second, best->first};
    }

    // Relocation phase and merge phase of operation t.
    void finish_operation() {
        uint64_t t = ++t_;
        if (t > a_) {
            uint64_t j = (t + a_ - 1) / a_ - 2;
            uint64_t i = t - (j + 1) * a_;  // position in the period, 1..a
            uint64_t first = (i - 1) * c_ + 1, last = std::min<uint64_t>(i * c_, calls_);
            for (uint64_t l = first; l <= last; l++) tree_.streaming_call(j, static_cast<unsigned>(l));
        }
        if (t % a_ == 0) merge();
    }

    // E_rho <- (E_rho minus D_G) merged with E_G, D_rho <- D_rho united with D_G, on sorted buffers (Lemma 10, Appendix B).
    void merge() {
        BufferTree::Buffer& root = tree_.root;
        std::vector<uint64_t> dg(front_D_.begin(), front_D_.end());
        std::sort(dg.begin(), dg.end());
        std::vector<BufferTree::Entry> eg;
        eg.reserve(front_E_.size());
        for (const auto& [k, p] : front_E_) eg.push_back(BufferTree::Entry{k, p, tree_.route(k)});
        std::sort(eg.begin(), eg.end(), [](const auto& x, const auto& y) { return x.key < y.key; });

        std::vector<BufferTree::Entry> merged;
        merged.reserve(root.E.size() + eg.size());
        size_t i = 0, j = 0, m = 0;
        while (i < root.E.size() || j < eg.size()) {
            if (j == eg.size() || (i < root.E.size() && root.E[i].key < eg[j].key)) {
                const auto& e = root.E[i++];
                while (m < dg.size() && dg[m] < e.key) m++;
                if (m < dg.size() && dg[m] == e.key) continue;
                merged.push_back(e);
            } else if (i == root.E.size() || eg[j].key < root.E[i].key) {
                merged.push_back(eg[j++]);
            } else {
                auto e = eg[j++];
                const auto& old = root.E[i++];
                while (m < dg.size() && dg[m] < old.key) m++;
                if (!(m < dg.size() && dg[m] == old.key) && old.prio < e.prio) e.prio = old.prio;
                merged.push_back(e);
            }
        }
        root.E.swap(merged);
        std::vector<uint64_t> d;
        std::set_union(root.D.begin(), root.D.end(), dg.begin(), dg.end(), std::back_inserter(d));
        root.D.swap(d);
        tree_.truncate(root);

        front_E_.clear();
        front_order_.clear();
        front_D_.clear();
        snap_order_.clear();
        for (const auto& e : root.E) snap_order_.insert({e.prio, e.key});
    }

    uint64_t a_ = 0, c_ = 0, calls_ = 0;
    std::set<std::pair<uint64_t, uint64_t>> snap_order_;   // snapshot of E_rho: (priority, key)
    std::unordered_map<uint64_t, uint64_t> front_E_;       // E_G: key -> priority
    std::set<std::pair<uint64_t, uint64_t>> front_order_;  // E_G: (priority, key)
    std::unordered_set<uint64_t> front_D_;                 // D_G
};

}  // namespace opq
