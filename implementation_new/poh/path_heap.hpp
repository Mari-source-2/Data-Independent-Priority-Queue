#pragma once

// Path Oblivious Heap (Shi, S&P'20), Path variant, used as the baseline.
//
// Tree of depth L = ceil(log2 n) with buckets of z elements; the root is held by the client and has
// no capacity bound (it acts as the stash; its peak size is reported). Every element is assigned a
// uniformly random leaf and resides on the path to it; every bucket stores the minimum of its subtree.
// Each bucket is stored as two objects: its subtree minimum (table 0) and its elements (table 1),
// so that the minima of siblings can be fetched without the full buckets.
//
// Every operation consists of at most two parts, executed on one batch of buckets that is fetched
// in a single round trip and written back in a single message:
//   delete part: read the path of a reference and the minima of its siblings, remove the element,
//                evict the path and recompute the subtree minima on it;
//   evict part:  evict one random path in each half of the tree.
// Every operation performs both parts, as in the padded variant of Shi (Appendix D), on a random
// path where no real deletion is needed, so all operations have the same access pattern and the
// operation types stay hidden. Without padding, the server would learn the type of every operation,
// so we do not consider that variant.
//
// Path Oblivious Heap addresses elements by references (leaf, timestamp). To offer the key-based
// interface of the paper, the client keeps a position map from keys to references, whose size is
// reported separately.
//
// Leaves are drawn from a cryptographically secure generator (AES-CTR under a fresh random key), since
// the server observes every accessed leaf. A nonzero seed makes runs reproducible but insecure (tests).
// The associated data binds every object to a random instance identifier and its location, but not to
// a version: detecting replayed buckets would need per-bucket client state, so against the server this
// baseline is secure only in the honest-but-curious setting.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/bytes.hpp"
#include "../common/crypto.hpp"
#include "../common/pq_interface.hpp"
#include "../common/store.hpp"

namespace opq {

class PathHeap : public PriorityQueue {
   public:
    struct Params {
        uint64_t n = 0;          // maximum number of present elements
        uint64_t z = 2;          // bucket capacity
        size_t key_bytes = 3;
        size_t prio_bytes = 3;
        std::string name = "poh";
        Key enc_key{};
        uint64_t seed = 0;       // 0: secure random leaves; otherwise reproducible and INSECURE (tests only)
        // Without the key map, only Insert of absent keys, FindMin and ExtractMin are supported, which is
        // Path Oblivious Heap's own interface without references; the caller guarantees absent keys.
        bool key_map = true;
    };

    PathHeap(Store& store, const Params& p)
        : store_(store), p_(p), aead_(p.enc_key), rng_(p.seed != 0 ? SecureRng::insecure_from_seed(p.seed) : SecureRng()) {
        if (p.n < 2) throw std::invalid_argument("n must be at least 2");
        if (p.z == 0) throw std::invalid_argument("z must be positive");
        max_key_ = max_value(p.key_bytes);
        max_prio_ = max_value(p.prio_bytes);
        L_ = static_cast<unsigned>(std::ceil(std::log2(static_cast<double>(p.n))));
        leaves_ = 1ULL << L_;
        first_leaf_ = leaves_ - 1;
        // Leaves lie in [0, 2^L). Timestamps lie in [0, 2^(L+1)): a decrease-key allocates the new
        // element before the old one is freed, so up to n + 1 <= 2^L + 1 timestamps are in use.
        ref_bytes_ = (L_ + 7) / 8;
        ts_bytes_ = (L_ + 8) / 8;
        elem_size_ = p.key_bytes + p.prio_bytes + ref_bytes_ + ts_bytes_;
        create_tree();
    }

    // ---------------------------------------------------------------- key-based interface
    void insert(uint64_t key, uint64_t priority) override {
        if (key == 0 || key > max_key_) throw std::invalid_argument("key out of range");
        if (priority > max_prio_) throw std::invalid_argument("priority out of range");
        if (!p_.key_map) {
            root_.add(new_element(key, priority));
            access(std::nullopt);
            operations_++;
            return;
        }
        auto it = pos_map_.find(key);
        if (it == pos_map_.end()) {
            Elem e = new_element(key, priority);
            root_.add(e);
            pos_map_[key] = e;
            access(std::nullopt);
        } else {
            // Decrease-key: delete the old element and insert one with the smaller priority.
            Elem old = it->second;
            Elem e = new_element(key, std::min(priority, old.prio));
            root_.add(e);
            it->second = e;
            access(old);
            free_timestamp(old.ts);
        }
        note_map();
        operations_++;
    }

    void erase(uint64_t key) override {
        if (!p_.key_map) throw std::logic_error("Delete needs the key map");
        auto it = pos_map_.find(key);
        operations_++;
        if (it == pos_map_.end()) {
            access(std::nullopt);
            return;
        }
        Elem old = it->second;
        pos_map_.erase(it);
        access(old);
        free_timestamp(old.ts);
    }

    std::optional<Pair> find_min() override {
        std::optional<Pair> result;
        if (root_.min.key != 0) result = Pair{root_.min.key, root_.min.prio};
        access(std::nullopt);
        operations_++;
        return result;
    }

    std::optional<Pair> extract_min() override {
        if (root_.min.key == 0) {
            access(std::nullopt);
            operations_++;
            return std::nullopt;
        }
        Elem m = root_.min;
        if (p_.key_map) pos_map_.erase(m.key);
        access(m);
        free_timestamp(m.ts);
        operations_++;
        return Pair{m.key, m.prio};
    }

    QueueStats stats() const override {
        QueueStats s = stats_;
        s.operations = operations_;
        s.peak_client_records = memory_.peak();
        s.encryption_ns = crypto_ns_;
        return s;
    }

    std::string describe() const override {
        return "path oblivious heap, z=" + std::to_string(p_.z) + ", L=" + std::to_string(L_);
    }

   private:
    struct Elem {
        uint64_t key = 0;  // 0: no element
        uint64_t prio = std::numeric_limits<uint64_t>::max();
        uint64_t pos = 0;  // assigned leaf
        uint64_t ts = 0;   // timestamp, unique among present elements
        bool same(const Elem& o) const { return key != 0 && pos == o.pos && ts == o.ts; }
    };
    static bool less(const Elem& a, const Elem& b) {
        if (a.key == 0) return false;
        if (b.key == 0) return true;
        return a.prio != b.prio ? a.prio < b.prio : a.key < b.key;
    }
    struct Bucket {
        std::vector<Elem> content;
        Elem min;  // minimum of the subtree
        void add(const Elem& e) {
            content.push_back(e);
            if (less(e, min)) min = e;
        }
        bool offer_min(const Elem& e) {
            if (less(e, min)) {
                min = e;
                return true;
            }
            return false;
        }
        void recompute_own_min() {
            min = Elem{};
            for (const Elem& e : content)
                if (less(e, min)) min = e;
        }
    };

    // ---------------------------------------------------------------- geometry and storage
    uint64_t leaf_node(uint64_t leaf) const { return first_leaf_ + leaf; }
    static uint64_t sibling(uint64_t u) { return (u % 2 == 1) ? u + 1 : u - 1; }

    // Nodes v_0 (root), ..., v_L (leaf) on the path to `leaf`.
    std::vector<uint64_t> path_of(uint64_t leaf) const {
        std::vector<uint64_t> v(L_ + 1);
        v[L_] = leaf_node(leaf);
        for (int i = static_cast<int>(L_) - 1; i >= 0; i--) v[i] = (v[i + 1] - 1) / 2;
        return v;
    }

    // Deepest level at which the paths to two leaves still coincide.
    unsigned common_depth(uint64_t a, uint64_t b) const {
        uint64_t x = a ^ b;
        unsigned bits = 0;
        while (x) {
            bits++;
            x >>= 1;
        }
        return L_ - bits;
    }

    void create_tree() {
        instance_ = random_key();
        uint64_t count = 2 * leaves_ - 2;  // all nodes except the root
        store_.create(p_.name, {TableSpec{elem_size_ + Aead::kOverhead, count},
                                TableSpec{p_.z * elem_size_ + Aead::kOverhead, count}});
        std::vector<ObjRef> refs;
        std::vector<std::string> data;
        Bucket empty;
        for (uint64_t u = 1; u <= count; u++) {
            refs.push_back({0, u - 1});
            data.push_back(seal_min(u, empty.min));
            refs.push_back({1, u - 1});
            data.push_back(seal_content(u, empty));
            if (refs.size() * (2 * elem_size_ + p_.z * elem_size_) >= (1 << 20)) {
                store_.write(refs, data);
                refs.clear();
                data.clear();
            }
        }
        if (!refs.empty()) store_.write(refs, data);
        store_.reset_stats();
        crypto_ns_ = 0;
        memory_.set(0);
    }

    void put_elem(char* q, const Elem& e) const {
        write_le(q, e.key, p_.key_bytes);
        write_le(q + p_.key_bytes, e.key == 0 ? 0 : e.prio, p_.prio_bytes);
        write_le(q + p_.key_bytes + p_.prio_bytes, e.pos, ref_bytes_);
        write_le(q + p_.key_bytes + p_.prio_bytes + ref_bytes_, e.ts, ts_bytes_);
    }
    Elem get_elem(const char* q) const {
        Elem e;
        e.key = read_le(q, p_.key_bytes);
        if (e.key == 0) return Elem{};
        e.prio = read_le(q + p_.key_bytes, p_.prio_bytes);
        e.pos = read_le(q + p_.key_bytes + p_.prio_bytes, ref_bytes_);
        e.ts = read_le(q + p_.key_bytes + p_.prio_bytes + ref_bytes_, ts_bytes_);
        return e;
    }

    // Object tables: 0 = subtree minimum of node u (one element), 1 = content of node u (z elements).
    Aead::Ad ad_of(uint64_t table, uint64_t u) const { return Aead::make_ad(instance_, table, u - 1, 0); }

    // Unsealed objects: kOverhead bytes of space for IV and tag, then the plaintext.
    std::string plain_min(const Elem& m) const {
        std::string obj(Aead::kOverhead + elem_size_, '\0');
        put_elem(obj.data() + Aead::kOverhead, m);
        return obj;
    }
    std::string plain_content(const Bucket& b) const {
        if (b.content.size() > p_.z) throw std::logic_error("overfull bucket");
        std::string obj(Aead::kOverhead + p_.z * elem_size_, '\0');
        char* q = obj.data() + Aead::kOverhead;
        for (const Elem& e : b.content) {
            put_elem(q, e);
            q += elem_size_;
        }
        return obj;
    }
    std::string seal_min(uint64_t u, const Elem& m) {
        std::string obj = plain_min(m);
        aead_.seal(obj, ad_of(0, u));
        return obj;
    }
    std::string seal_content(uint64_t u, const Bucket& b) {
        std::string obj = plain_content(b);
        aead_.seal(obj, ad_of(1, u));
        return obj;
    }
    Elem parse_min(const std::string& obj) const {
        if (obj.size() != Aead::kOverhead + elem_size_) throw std::runtime_error("object of wrong size");
        return get_elem(obj.data() + Aead::kOverhead);
    }
    void parse_content(const std::string& obj, Bucket& b) const {
        if (obj.size() != Aead::kOverhead + p_.z * elem_size_) throw std::runtime_error("object of wrong size");
        for (uint64_t i = 0; i < p_.z; i++) {
            Elem e = get_elem(obj.data() + Aead::kOverhead + i * elem_size_);
            if (e.key != 0) b.content.push_back(e);
        }
    }

    // Timestamps lie in [0, 2^(L+1)): a decrease-key allocates the new element before the old one is
    // freed, so up to n + 1 <= 2^L + 1 timestamps are in use. Fresh ones come from a counter.
    uint64_t take_timestamp() {
        if (!free_timestamps_.empty()) {
            uint64_t ts = free_timestamps_.back();
            free_timestamps_.pop_back();
            return ts;
        }
        if (next_timestamp_ == 2 * leaves_) throw std::logic_error("out of timestamps");
        return next_timestamp_++;
    }
    void free_timestamp(uint64_t ts) { free_timestamps_.push_back(ts); }

    Elem new_element(uint64_t key, uint64_t prio) {
        Elem e;
        e.key = key;
        e.prio = prio;
        e.pos = rng_.below(leaves_);
        e.ts = take_timestamp();
        return e;
    }

    void note_map() { stats_.position_map_entries = std::max<uint64_t>(stats_.position_map_entries, pos_map_.size()); }

    // ---------------------------------------------------------------- eviction (Shi, Section 3)
    // Moves every element as deep as possible along the path to `leaf`, updating the subtree minima
    // of the buckets it passes.
    void evict(std::vector<Bucket*>& pb, uint64_t leaf, bool update_minima) {
        for (int level = static_cast<int>(L_) - 1; level >= 0; level--) {
            size_t i = 0;
            while (i < pb[level]->content.size()) {
                Elem e = pb[level]->content[i];
                int target = static_cast<int>(common_depth(e.pos, leaf));
                bool moved = false;
                for (int j = target; j > level; j--) {
                    if (!moved) {
                        if (pb[j]->content.size() < p_.z) {
                            pb[j]->content.push_back(e);
                            if (update_minima) pb[j]->offer_min(e);
                            pb[level]->content[i] = pb[level]->content.back();
                            pb[level]->content.pop_back();
                            moved = true;
                        }
                    } else if (update_minima && !pb[j]->offer_min(e)) {
                        break;
                    }
                }
                if (!moved) i++;
            }
        }
    }

    // Recomputes the subtree minima along a path from the buckets on it and the minima of the siblings.
    void recompute_minima(std::vector<Bucket*>& pb, const std::vector<Elem>& sibling_min) {
        for (unsigned level = 0; level <= L_; level++) pb[level]->recompute_own_min();
        for (int level = static_cast<int>(L_) - 1; level >= 0; level--) {
            pb[level]->offer_min(pb[level + 1]->min);
            pb[level]->offer_min(sibling_min[level + 1]);
        }
    }

    // ---------------------------------------------------------------- one batched access
    // One padded access: the delete part on the path of `del` (or of a random leaf if there is nothing
    // to delete) and the evict part on one random path in each half of the tree. All objects are
    // fetched in one round trip and written back in one message.
    void access(std::optional<Elem> del) {
        uint64_t half = leaves_ / 2;
        uint64_t del_leaf = del ? del->pos : rng_.below(leaves_);
        uint64_t e1 = rng_.below(half);
        uint64_t e2 = half + rng_.below(half);

        std::vector<uint64_t> pdel = path_of(del_leaf), p1 = path_of(e1), p2 = path_of(e2);
        std::set<uint64_t> full, min_only;
        for (unsigned i = 1; i <= L_; i++) {
            full.insert(pdel[i]);
            full.insert(p1[i]);
            full.insert(p2[i]);
        }
        for (unsigned i = 1; i <= L_; i++)
            if (!full.count(sibling(pdel[i]))) min_only.insert(sibling(pdel[i]));

        std::vector<ObjRef> refs;
        for (uint64_t u : full) {
            refs.push_back({0, u - 1});
            refs.push_back({1, u - 1});
        }
        for (uint64_t u : min_only) refs.push_back({0, u - 1});
        std::vector<std::string> data = store_.read(refs);
        {
            ScopedTimer t(crypto_ns_);
            for (size_t i = 0; i < refs.size(); i++)
                aead_.open(data[i], Aead::make_ad(instance_, refs[i].table, refs[i].id, 0));
        }
        std::unordered_map<uint64_t, Bucket> buckets;
        std::unordered_map<uint64_t, Elem> mins;
        size_t idx = 0;
        for (uint64_t u : full) {
            Bucket& b = buckets[u];
            b.min = parse_min(data[idx++]);
            parse_content(data[idx++], b);
        }
        for (uint64_t u : min_only) mins[u] = parse_min(data[idx++]);
        memory_.set(root_.content.size() + full.size() * (p_.z + 1) + min_only.size());
        stats_.peak_stash = std::max<uint64_t>(stats_.peak_stash, root_.content.size());

        auto path_buckets = [&](const std::vector<uint64_t>& path) {
            std::vector<Bucket*> pb(L_ + 1);
            pb[0] = &root_;
            for (unsigned i = 1; i <= L_; i++) pb[i] = &buckets.at(path[i]);
            return pb;
        };

        {
            // Delete part. Siblings' minima, taken before any bucket changes in this access.
            std::vector<Elem> sib(L_ + 1);
            for (unsigned i = 1; i <= L_; i++) {
                uint64_t s = sibling(pdel[i]);
                sib[i] = full.count(s) ? buckets.at(s).min : mins.at(s);
            }
            std::vector<Bucket*> pb = path_buckets(pdel);
            bool was_min = false;
            if (del) {
                bool found = false;
                for (int level = static_cast<int>(L_); level >= 0 && !found; level--) {
                    auto& c = pb[level]->content;
                    for (size_t i = 0; i < c.size(); i++) {
                        if (c[i].same(*del)) {
                            was_min = pb[level]->min.same(*del);
                            c[i] = c.back();
                            c.pop_back();
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) throw std::logic_error("element to delete not found on its path");
            }
            if (was_min) {
                evict(pb, del_leaf, false);
                recompute_minima(pb, sib);
            } else {
                evict(pb, del_leaf, true);
            }
        }
        {
            // Evict part.
            for (const auto* path : {&p1, &p2}) {
                std::vector<Bucket*> pb = path_buckets(*path);
                evict(pb, (*path)[L_] - first_leaf_, true);
            }
        }
        stats_.peak_stash = std::max<uint64_t>(stats_.peak_stash, root_.content.size());

        std::vector<ObjRef> wrefs;
        std::vector<std::string> wdata;
        for (uint64_t u : full) {
            const Bucket& b = buckets.at(u);
            wrefs.push_back({0, u - 1});
            wdata.push_back(plain_min(b.min));
            wrefs.push_back({1, u - 1});
            wdata.push_back(plain_content(b));
        }
        {
            ScopedTimer t(crypto_ns_);
            for (size_t i = 0; i < wrefs.size(); i++)
                aead_.seal(wdata[i], Aead::make_ad(instance_, wrefs[i].table, wrefs[i].id, 0));
        }
        store_.write(wrefs, wdata);
    }

    Store& store_;
    Params p_;
    Aead aead_;
    uint64_t max_key_ = 0, max_prio_ = 0;
    unsigned L_ = 0;
    uint64_t leaves_ = 0, first_leaf_ = 0;
    size_t ref_bytes_ = 0, ts_bytes_ = 0, elem_size_ = 0;
    SecureRng rng_;
    Key instance_{};
    std::vector<uint64_t> free_timestamps_;
    uint64_t next_timestamp_ = 0;
    uint64_t crypto_ns_ = 0;
    std::unordered_map<uint64_t, Elem> pos_map_;
    Bucket root_;
    QueueStats stats_;
    PeakCounter memory_;
    uint64_t operations_ = 0;
};

}  // namespace opq
