#pragma once

// Parameters and helpers shared by our two queues (Section 3 and Appendix B), which differ only in
// when the calls of a relocation are executed and how the operations reach the root.

#include <cstdint>
#include <stdexcept>
#include <string>

#include "../common/pq_interface.hpp"
#include "buffer_tree.hpp"

namespace opq {

struct TreeQueueParams {
    uint64_t n = 0;           // maximum number of present keys
    uint64_t B = 0;           // buffer capacity in records
    size_t key_bytes = 3;     // key field width
    size_t prio_bytes = 3;    // priority field width
    std::string name = "pq";  // table name at the server
    Key enc_key{};
    Key prf_key{};
};

class TreeQueue : public PriorityQueue {
   public:
    QueueStats stats() const override {
        QueueStats s;
        s.operations = t_;
        s.overflows = tree_.overflows();
        s.peak_client_records = tree_.peak_client_records();
        s.encryption_ns = tree_.encryption_ns();
        s.prf_evaluations = tree_.prf_evaluations();
        return s;
    }
    unsigned height() const { return tree_.height(); }

   protected:
    // `divisor`: B must be a positive multiple of it (8 for both constructions).
    TreeQueue(Store& store, const TreeQueueParams& p, uint64_t divisor)
        : B_(p.B), tree_(store, tree_params(p, divisor)) {
        max_key_ = max_value(p.key_bytes);
        max_prio_ = max_value(p.prio_bytes);
    }

    void check_key(uint64_t key) const {
        if (key == 0 || key > max_key_) throw std::invalid_argument("key out of range (keys are 1..max)");
    }
    void check_priority(uint64_t priority) const {
        if (priority > max_prio_) throw std::invalid_argument("priority out of range");
    }

    uint64_t B_;
    BufferTree tree_;
    uint64_t max_key_ = 0, max_prio_ = 0;
    uint64_t t_ = 0;  // operations so far

   private:
    static BufferTree::Params tree_params(const TreeQueueParams& p, uint64_t divisor) {
        if (p.B == 0 || p.B % divisor != 0)
            throw std::invalid_argument("B must be a positive multiple of " + std::to_string(divisor));
        if (p.n < p.B) throw std::invalid_argument("the construction requires n >= B (the analysis assumes n >= 2B)");
        BufferTree::Params t;
        t.n = p.n;
        t.B = p.B;
        t.key_bytes = p.key_bytes;
        t.prio_bytes = p.prio_bytes;
        t.name = p.name;
        t.enc_key = p.enc_key;
        t.prf_key = p.prf_key;
        return t;
    }
};

}  // namespace opq
