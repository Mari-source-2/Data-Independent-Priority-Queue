// Workload generator with reference answers.
//
// Output format (one operation per line):
//   0, k, p   Insert(k, p)  (sets the priority of k to the minimum of its old and new priority)
//   1, k, p   DecreaseKey(k, p) on a present key (same semantics as Insert)
//   2, k      Delete(k) of a present key
//   3         ExtractMin
//   4         FindMin
// With --mix insert-extract, the workload contains only insertions of absent keys, ExtractMin and
// FindMin, which Path Oblivious Heap supports without a map from keys to references.
// The answers file contains one line per ExtractMin or FindMin: "(k, p)" or "empty".
// On standard output, the generator reports max_present, the largest number of keys present at
// once; the queue parameter n must be at least this value; and fill_ops, the number of operations
// after which the target size is first reached (0 if never), which can serve as a warmup length.
//
// While fewer than --size keys are present, insertions use absent keys (as long as at most half of the
// key space is in use), so that the target size is reached; afterwards, insertions draw arbitrary keys
// and thus also act as decrease-key operations.
//
// Fixes compared to the old generator: explicit seed, key and priority widths in bits, no division by
// zero on an empty queue (Delete/DecreaseKey become insertions), correct answers for an empty queue.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Reference priority queue with uniform sampling of present keys.
class Reference {
   public:
    void insert(uint64_t k, uint64_t p) {
        auto it = prio_.find(k);
        if (it == prio_.end()) {
            prio_[k] = p;
            order_.insert({p, k});
            index_[k] = keys_.size();
            keys_.push_back(k);
        } else if (p < it->second) {
            order_.erase({it->second, k});
            it->second = p;
            order_.insert({p, k});
        }
    }
    void erase(uint64_t k) {
        auto it = prio_.find(k);
        if (it == prio_.end()) return;
        order_.erase({it->second, k});
        prio_.erase(it);
        size_t i = index_[k];
        keys_[i] = keys_.back();
        index_[keys_[i]] = i;
        keys_.pop_back();
        index_.erase(k);
    }
    bool empty() const { return keys_.empty(); }
    bool contains(uint64_t k) const { return prio_.count(k) > 0; }
    size_t size() const { return keys_.size(); }
    uint64_t key_at(size_t i) const { return keys_[i]; }
    std::pair<uint64_t, uint64_t> min() const { return {order_.begin()->second, order_.begin()->first}; }

   private:
    std::unordered_map<uint64_t, uint64_t> prio_;
    std::set<std::pair<uint64_t, uint64_t>> order_;  // (priority, key)
    std::unordered_map<uint64_t, size_t> index_;
    std::vector<uint64_t> keys_;
};

uint64_t arg_u64(const std::string& s) { return std::stoull(s); }

}  // namespace

int main(int argc, char* argv[]) {
    uint64_t ops = 0, target = 0, key_bits = 24, prio_bits = 24, seed = 1, findmin_percent = 0;
    std::string out_path, answers_path, mix = "full";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i], v = argv[i + 1];
        if (a == "--ops") ops = arg_u64(v);
        else if (a == "--size") target = arg_u64(v);
        else if (a == "--key-bits") key_bits = arg_u64(v);
        else if (a == "--prio-bits") prio_bits = arg_u64(v);
        else if (a == "--seed") seed = arg_u64(v);
        else if (a == "--findmin-percent") findmin_percent = arg_u64(v);
        else if (a == "--out") out_path = v;
        else if (a == "--answers") answers_path = v;
        else if (a == "--mix") mix = v;
        else {
            std::cerr << "unknown argument " << a << "\n";
            return 1;
        }
    }
    if (mix != "full" && mix != "insert-extract") {
        std::cerr << "--mix must be full or insert-extract\n";
        return 1;
    }
    bool fresh_only = mix == "insert-extract";
    if (ops == 0 || target == 0 || out_path.empty() || answers_path.empty() || key_bits == 0 || key_bits > 64 ||
        prio_bits == 0 || prio_bits > 64) {
        std::cerr << "usage: generator --ops N --size S --out FILE --answers FILE [--key-bits K] [--prio-bits P]"
                     " [--seed X] [--findmin-percent F] [--mix full|insert-extract]\n";
        return 1;
    }
    uint64_t max_key = key_bits == 64 ? ~0ULL : (1ULL << key_bits) - 1;
    uint64_t max_prio = prio_bits == 64 ? ~0ULL : (1ULL << prio_bits) - 1;

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> dist_key(1, max_key);
    std::uniform_int_distribution<uint64_t> dist_prio(0, max_prio);
    std::uniform_int_distribution<int> dist_percent(1, 100);

    std::ofstream out(out_path), answers(answers_path);
    Reference ref;
    uint64_t max_present = 0, fill_ops = 0;
    for (uint64_t i = 0; i < ops; i++) {
        int insert_percent = ref.size() < target ? 90 : 25;
        int r = dist_percent(rng);
        int op;
        if (r <= insert_percent) {
            op = 0;
        } else {
            op = 1 + (r % 3);  // 1, 2, 3 with (almost) equal probability, as in the old generator
            if (findmin_percent > 0 && dist_percent(rng) <= static_cast<int>(findmin_percent)) op = 4;
        }
        if (fresh_only && (op == 1 || op == 2)) op = 3;
        if ((op == 1 || op == 2) && ref.empty()) op = 0;
        switch (op) {
            case 0: {
                // Fresh keys only with --mix insert-extract, and while the queue is being filled, so that
                // the target size is reached even when it is a large fraction of the key space.
                bool fresh = fresh_only || ref.size() < target;
                if (!fresh_only && ref.size() > max_key / 2) fresh = false;  // keep rejection sampling cheap
                uint64_t k = dist_key(rng), p = dist_prio(rng);
                while (fresh && ref.contains(k)) k = dist_key(rng);
                ref.insert(k, p);
                out << "0, " << k << ", " << p << "\n";
                break;
            }
            case 1: {
                uint64_t k = ref.key_at(std::uniform_int_distribution<size_t>(0, ref.size() - 1)(rng));
                uint64_t p = dist_prio(rng);
                ref.insert(k, p);
                out << "1, " << k << ", " << p << "\n";
                break;
            }
            case 2: {
                uint64_t k = ref.key_at(std::uniform_int_distribution<size_t>(0, ref.size() - 1)(rng));
                ref.erase(k);
                out << "2, " << k << "\n";
                break;
            }
            case 3:
            case 4: {
                if (ref.empty()) {
                    answers << "empty\n";
                } else {
                    auto [k, p] = ref.min();
                    answers << "(" << k << ", " << p << ")\n";
                    if (op == 3) ref.erase(k);
                }
                out << op << "\n";
                break;
            }
        }
        max_present = std::max<uint64_t>(max_present, ref.size());
        if (fill_ops == 0 && ref.size() >= target) fill_ops = i + 1;
    }
    std::cout << "max_present=" << max_present << "\nfill_ops=" << fill_ops << "\n";
    return 0;
}
