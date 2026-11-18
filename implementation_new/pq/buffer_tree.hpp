#pragma once

// The tree of buffers shared by both of our constructions (Section 3 and Appendix B of the paper):
// geometry, encrypted buckets, the local procedures PushDown and PullUp, and path relocations.
//
// Tree: complete binary tree of depth L = ceil(log2(4n/B)), nodes numbered in breadth-first order
// (root 0, children of u are 2u+1 and 2u+2). Internal nodes hold an element buffer E of capacity B
// and a delete buffer D of capacity B/2, leaves only an element buffer. The client holds the root; every
// other node is stored encrypted at the server.
//
// Buffers are sorted vectors (by key), so PushDown and PullUp are linear merges. The order of the
// entries inside a bucket is hidden by the encryption.
//
// Routing: h(k) = PRF(k) mod 2^L. A key at depth d continues into child number bit d of h(k).
// Relocation j follows the path that goes into child number bit d of j at depth d.
//
// Relocations:
//   relocate_cached(j) : read the 2L buckets of the path and its siblings in one request, process
//                        them, write them back in one message. Client memory at most 2L buckets
//                        besides the root, that is, at most 3B(L-1) + 2B records.
//   streaming_call(j, l): the l-th of the 2L calls of relocation j (PushDown(v_0), ..., PushDown(v_{L-1}),
//                        PullUp(v_{L-1}), ..., PullUp(v_0)). The client keeps the bucket of the next
//                        path node in memory between calls and does not write back or read a path node
//                        between PushDown and PullUp at it, which empty and refill it. So every call
//                        transfers at most three buckets, and the client holds at most three buckets
//                        besides the root (Lemma 1), also when the calls are spread over several operations.
//
// Associated data of every bucket: (instance, table, index, version). The instance is a random
// identifier chosen when the tree is created, so buckets of other sessions are rejected. Versions
// are a deterministic function of the relocation counter, so the client detects replayed or swapped
// buckets without storing per-bucket state: creation writes version 0, relocation j writes version
// 2j+1 during the downward pass of a streaming relocation and version 2j+2 when it is done with a bucket.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/bytes.hpp"
#include "../common/crypto.hpp"
#include "../common/memory.hpp"
#include "../common/store.hpp"

namespace opq {

class BufferTree {
   public:
    struct Params {
        uint64_t n = 0;           // maximum number of present keys
        uint64_t B = 0;           // buffer capacity in records
        size_t key_bytes = 3;     // key field width
        size_t prio_bytes = 3;    // priority field width
        std::string name = "pq";  // table name at the server
        Key enc_key{};
        Key prf_key{};
    };
    struct Entry {
        uint64_t key;
        uint64_t prio;
        uint64_t h;  // leaf label h(key), computed once when the entry is created or loaded
    };
    // Both vectors are sorted by key and contain every key at most once.
    struct Buffer {
        std::vector<Entry> E;
        std::vector<uint64_t> D;
        void clear() {
            E.clear();
            D.clear();
        }
    };

    BufferTree(Store& store, const Params& p) : store_(store), p_(p), prf_(p.prf_key), aead_(p.enc_key) {
        max_value(p.key_bytes);  // validate the field widths before anything is created
        max_value(p.prio_bytes);
        if (p.B < 2 || p.n < p.B) throw std::invalid_argument("the construction requires 2 <= B <= n");
        if (p.B % 2 != 0) throw std::invalid_argument("B must be even");
        // L = ceil(log2(4n/B)): the smallest L with 2^L * B >= 4n, so 4n/B <= 2^L < 8n/B (eq. (1)).
        L_ = 0;
        while (L_ <= 40 && (p.B << L_) < 4 * p.n) L_++;
        if (L_ < 1 || L_ > 40) throw std::invalid_argument("unsupported tree height");
        leaves_ = 1ULL << L_;
        first_leaf_ = leaves_ - 1;
        elem_size_ = p.key_bytes + p.prio_bytes;
        dcap_ = p.B / 2;  // capacity of a delete buffer
        internal_plain_ = p.B * elem_size_ + dcap_ * p.key_bytes;
        leaf_plain_ = p.B * elem_size_;
        create_tree();
    }

    Buffer root;  // E_rho and D_rho, held by the client

    unsigned height() const { return L_; }
    uint64_t route(uint64_t key) const { return prf_.eval(key) & (leaves_ - 1); }
    uint64_t overflows() const { return overflows_; }
    uint64_t peak_client_records() const { return memory_.peak(); }
    uint64_t encryption_ns() const { return crypto_ns_; }
    uint64_t prf_evaluations() const { return prf_.evals(); }

    // Records that the client holds outside the tree's buckets (root, and for Appendix B the snapshot
    // and the front). Every memory update adds the buckets currently held.
    void set_base_records(uint64_t r) {
        base_records_ = r;
        update_memory();
    }

    // Overflow rule of the paper: an overfull buffer keeps only as many entries as its capacity, B for
    // an element buffer and B/2 for a delete buffer (here: the smallest keys). Every truncation is counted.
    void truncate(Buffer& b) {
        if (b.E.size() > p_.B) {
            b.E.resize(p_.B);
            overflows_++;
        }
        if (b.D.size() > dcap_) {
            b.D.resize(dcap_);
            overflows_++;
        }
    }
    uint64_t delete_capacity() const { return dcap_; }
    void count_overflow() { overflows_++; }

    // ---------------------------------------------------------------- relocations
    void relocate_cached(uint64_t J) {
        Path v = path_of(J);
        std::vector<uint64_t> nodes, versions;
        for (unsigned i = 1; i <= L_; i++) {
            for (uint64_t u : {v[i], sibling(v[i])}) {
                nodes.push_back(u);
                versions.push_back(version_before(u, J));
            }
        }
        load(nodes, versions);
        update_memory();
        for (unsigned d = 0; d < L_; d++) push_down_at(v[d], d);
        for (int d = static_cast<int>(L_) - 1; d >= 0; d--) pull_up_at(v[d]);
        store_back(nodes, 2 * J + 2);
        update_memory();
    }

    unsigned calls_per_relocation() const { return 2 * L_; }

    // The l-th call of relocation J, for l = 1, ..., 2L.
    void streaming_call(uint64_t J, unsigned l) {
        if (l < 1 || l > 2 * L_) throw std::logic_error("invalid call index");
        Path v = path_of(J);
        if (l <= L_) {
            // PushDown(v_d). Afterwards v_d is empty, so it is not written back now: PullUp(v_d)
            // starts from an empty buffer and writes v_d later as a child.
            unsigned d = l - 1;
            uint64_t on = v[d + 1], off = sibling(v[d + 1]);
            load({on, off}, {version_before(on, J), version_before(off, J)});
            update_memory();
            push_down_at(v[d], d);
            if (d > 0) held_.erase(v[d]);
            if (d + 1 < L_) store_back({off}, 2 * J + 1);  // keep both leaves at the last step
        } else {
            // PullUp(v_d).
            unsigned d = 2 * L_ - l;
            uint64_t on = v[d + 1], off = sibling(v[d + 1]);
            if (d + 1 < L_) load({off}, {2 * J + 1});
            if (d > 0) held_[v[d]].clear();
            update_memory();
            pull_up_at(v[d]);
            store_back({on, off}, 2 * J + 2);
            if (d == 0 && !held_.empty()) throw std::logic_error("relocation left buckets in memory");
        }
        update_memory();
    }

    // ---------------------------------------------------------------- level-wise baseline
    // The schedule of Jafargholi et al. (SODA'21) on the same tree and buffers, as a baseline: after the
    // M-th block of a operations (M = 1, 2, ...), with i the largest integer such that 2^i divides M and
    // i* = min(i, L-1), it executes PushDown on the levels 0, ..., i* and then PullUp on the levels
    // i*, ..., 0, each level node by node from left to right. As in their description, a call loads its
    // node and the node's two children, so the client holds at most three buckets besides the root, and
    // every call is one request. The nodes of depth d >= 1 are written exactly by the maintenances M
    // that 2^(d-1) divides; maintenance M writes the temporary version 2M+1 during PushDown and the final
    // version 2M+2 during PullUp.
    static unsigned levelwise_depth(uint64_t M, unsigned L) {
        unsigned i = 0;
        while (i + 1 < L && ((M >> i) & 1) == 0) i++;
        return i;  // i* = min(number of trailing zeros of M, L-1)
    }

    void levelwise_maintain(uint64_t M) {
        if (M == 0) throw std::logic_error("maintenances are numbered from 1");
        unsigned istar = levelwise_depth(M, L_);
        for (unsigned d = 0; d <= istar; d++) {
            uint64_t first = (1ULL << d) - 1;
            for (uint64_t v = first; v < first + (1ULL << d); v++) {
                uint64_t c0 = 2 * v + 1, c1 = 2 * v + 2;
                std::vector<uint64_t> nodes, versions;
                if (v != 0) {  // written as a child by PushDown at its parent in this maintenance
                    nodes.push_back(v);
                    versions.push_back(2 * M + 1);
                }
                for (uint64_t c : {c0, c1}) {
                    nodes.push_back(c);
                    versions.push_back(levelwise_version_before(c, M));
                }
                load(nodes, versions);
                update_memory();
                push_down_at(v, d);
                if (v != 0) held_.erase(v);  // empty now; PullUp(v) starts from an empty buffer and writes v
                store_back({c0, c1}, 2 * M + 1);
                update_memory();
            }
        }
        for (int d = static_cast<int>(istar); d >= 0; d--) {
            uint64_t first = (1ULL << d) - 1;
            // The children were last written by PushDown (deepest level) or by PullUp one level below.
            uint64_t child_version = static_cast<unsigned>(d) == istar ? 2 * M + 1 : 2 * M + 2;
            for (uint64_t v = first; v < first + (1ULL << d); v++) {
                uint64_t c0 = 2 * v + 1, c1 = 2 * v + 2;
                load({c0, c1}, {child_version, child_version});
                if (v != 0) held_[v].clear();
                update_memory();
                pull_up_at(v);
                std::vector<uint64_t> out = {c0, c1};
                if (v != 0) out.push_back(v);
                store_back(out, 2 * M + 2);
                update_memory();
            }
        }
        if (!held_.empty()) throw std::logic_error("maintenance left buckets in memory");
    }

   private:
    using Path = std::vector<uint64_t>;

    // Version of the bucket of node u (u != root) before maintenance M of the level-wise baseline.
    uint64_t levelwise_version_before(uint64_t u, uint64_t M) const {
        uint64_t m = 1ULL << (depth_of(u) - 1);
        uint64_t last = ((M - 1) / m) * m;  // the last maintenance before M that wrote u, or 0
        return last == 0 ? 0 : 2 * last + 2;
    }

    // ---------------------------------------------------------------- tree geometry
    bool is_leaf(uint64_t u) const { return u >= first_leaf_; }
    static unsigned depth_of(uint64_t u) {
        unsigned d = 0;
        while (u > 0) {
            u = (u - 1) / 2;
            d++;
        }
        return d;
    }
    static uint64_t sibling(uint64_t u) { return (u % 2 == 1) ? u + 1 : u - 1; }
    ObjRef ref_of(uint64_t u) const { return is_leaf(u) ? ObjRef{1, u - first_leaf_} : ObjRef{0, u - 1}; }

    // Label c(u): bit (i-1) is the direction taken into depth i on the path from the root to u.
    static uint64_t label_of(uint64_t u) {
        unsigned d = depth_of(u);
        uint64_t c = 0;
        for (unsigned i = d; i >= 1; i--) {
            c |= static_cast<uint64_t>((u + 1) & 1) << (i - 1);
            u = (u - 1) / 2;
        }
        return c;
    }

    Path path_of(uint64_t J) const {
        Path v(L_ + 1);
        v[0] = 0;
        for (unsigned i = 1; i <= L_; i++) v[i] = 2 * v[i - 1] + 1 + ((J >> (i - 1)) & 1);
        return v;
    }

    // Version of the bucket of node u (u != root) before relocation J starts: the last relocation
    // j < J that wrote u is the last one that maintained the parent of u (Lemma 5).
    uint64_t version_before(uint64_t u, uint64_t J) const {
        unsigned d = depth_of(u);
        uint64_t m = 1ULL << (d - 1);
        uint64_t c = label_of(u) & (m - 1);
        if (J <= c) return 0;  // never written since creation
        uint64_t j_last = c + ((J - 1 - c) / m) * m;
        return 2 * j_last + 2;
    }

    // ---------------------------------------------------------------- storage of buckets
    void create_tree() {
        instance_ = random_key();
        uint64_t internal_count = leaves_ - 2;  // internal nodes other than the root
        store_.create(p_.name, {TableSpec{internal_plain_ + Aead::kOverhead, internal_count},
                                TableSpec{leaf_plain_ + Aead::kOverhead, leaves_}});
        std::vector<ObjRef> refs;
        std::vector<std::string> data;
        Buffer empty;
        uint64_t batch_bytes = 0;
        for (uint64_t u = 1; u < first_leaf_ + leaves_; u++) {
            refs.push_back(ref_of(u));
            data.push_back(seal_bucket(u, empty, 0));
            batch_bytes += data.back().size();
            if (batch_bytes >= kInitBatchBytes) {
                store_.write(refs, data);
                refs.clear();
                data.clear();
                batch_bytes = 0;
            }
        }
        if (!refs.empty()) store_.write(refs, data);
        store_.reset_stats();  // initialization is excluded from the measurements
        crypto_ns_ = 0;
    }

    Aead::Ad ad_of(uint64_t u, uint64_t version) const {
        ObjRef r = ref_of(u);
        return Aead::make_ad(instance_, r.table, r.id, version);
    }

    // Plaintext: B element slots (key, priority), then for internal nodes B/2 marker slots (key);
    // real entries first, sorted by key, then dummy slots with key 0.
    std::string seal_bucket(uint64_t u, const Buffer& b, uint64_t version) {
        if (b.E.size() > p_.B || b.D.size() > dcap_) throw std::logic_error("sealing an overfull buffer");
        if (is_leaf(u) && !b.D.empty()) throw std::logic_error("leaf with markers");
        std::string obj(Aead::kOverhead + (is_leaf(u) ? leaf_plain_ : internal_plain_), '\0');
        char* q = obj.data() + Aead::kOverhead;
        for (const Entry& e : b.E) {
            write_le(q, e.key, p_.key_bytes);
            write_le(q + p_.key_bytes, e.prio, p_.prio_bytes);
            q += elem_size_;
        }
        if (!is_leaf(u)) {
            q = obj.data() + Aead::kOverhead + p_.B * elem_size_;
            for (uint64_t k : b.D) {
                write_le(q, k, p_.key_bytes);
                q += p_.key_bytes;
            }
        }
        ScopedTimer t(crypto_ns_);
        aead_.seal(obj, ad_of(u, version));
        return obj;
    }

    void open_bucket(uint64_t u, std::string& obj, uint64_t version, Buffer& b) {
        if (obj.size() != Aead::kOverhead + (is_leaf(u) ? leaf_plain_ : internal_plain_))
            throw std::runtime_error("bucket of wrong size");
        {
            ScopedTimer t(crypto_ns_);
            aead_.open(obj, ad_of(u, version));
        }
        b.clear();
        const char* q = obj.data() + Aead::kOverhead;
        for (uint64_t i = 0; i < p_.B; i++, q += elem_size_) {
            uint64_t k = read_le(q, p_.key_bytes);
            if (k != 0) b.E.push_back(Entry{k, read_le(q + p_.key_bytes, p_.prio_bytes), 0});
        }
        if (!is_leaf(u)) {
            for (uint64_t i = 0; i < dcap_; i++, q += p_.key_bytes) {
                uint64_t k = read_le(q, p_.key_bytes);
                if (k != 0) b.D.push_back(k);
            }
        }
        auto key_less = [](const Entry& x, const Entry& y) { return x.key < y.key; };
        if (!std::is_sorted(b.E.begin(), b.E.end(), key_less) ||
            std::adjacent_find(b.E.begin(), b.E.end(), [](const Entry& x, const Entry& y) { return x.key == y.key; }) !=
                b.E.end() ||
            !std::is_sorted(b.D.begin(), b.D.end()) || std::adjacent_find(b.D.begin(), b.D.end()) != b.D.end())
            throw std::runtime_error("malformed bucket");
        keys_.resize(b.E.size());
        for (size_t i = 0; i < b.E.size(); i++) keys_[i] = b.E[i].key;
        hashes_.resize(keys_.size());
        prf_.eval_many(keys_.data(), hashes_.data(), keys_.size());
        for (size_t i = 0; i < b.E.size(); i++) b.E[i].h = hashes_[i] & (leaves_ - 1);
    }

    void load(const std::vector<uint64_t>& nodes, const std::vector<uint64_t>& versions) {
        std::vector<ObjRef> refs;
        for (uint64_t u : nodes) refs.push_back(ref_of(u));
        std::vector<std::string> data = store_.read(refs);
        for (size_t i = 0; i < nodes.size(); i++) open_bucket(nodes[i], data[i], versions[i], held_[nodes[i]]);
    }

    void store_back(const std::vector<uint64_t>& nodes, uint64_t version) {
        std::vector<ObjRef> refs;
        std::vector<std::string> data;
        for (uint64_t u : nodes) {
            refs.push_back(ref_of(u));
            data.push_back(seal_bucket(u, held_.at(u), version));
            held_.erase(u);
        }
        store_.write(refs, data);
    }

    Buffer& node(uint64_t u) { return u == 0 ? root : held_.at(u); }

    uint64_t held_slots() const {
        uint64_t s = 0;
        for (const auto& kv : held_) s += is_leaf(kv.first) ? p_.B : p_.B + dcap_;
        return s;
    }
    void update_memory() { memory_.set(base_records_ + held_slots()); }

    // ---------------------------------------------------------------- local procedures (Figure 1)
    void push_down_at(uint64_t v, unsigned d) {
        uint64_t c0 = 2 * v + 1, c1 = 2 * v + 2;
        push_down(node(v), d, held_.at(c0), held_.at(c1), !is_leaf(c0));
    }
    void pull_up_at(uint64_t v) { pull_up(node(v), held_.at(2 * v + 1), held_.at(2 * v + 2)); }

    // PushDown(v): apply the markers of v to the children, then merge the pairs of v into them.
    void push_down(Buffer& v, unsigned d, Buffer& c0, Buffer& c1, bool children_internal) {
        // Split the markers and the pairs of v by the direction of their keys (both stay sorted).
        hashes_.resize(v.D.size());
        prf_.eval_many(v.D.data(), hashes_.data(), v.D.size());
        Buffer* child[2] = {&c0, &c1};
        for (int s = 0; s < 2; s++) {
            marks_.clear();
            for (size_t i = 0; i < v.D.size(); i++)
                if (static_cast<int>((hashes_[i] >> d) & 1) == s) marks_.push_back(v.D[i]);
            pairs_.clear();
            for (const Entry& e : v.E)
                if (static_cast<int>((e.h >> d) & 1) == s) pairs_.push_back(e);
            Buffer& c = *child[s];
            // c.E <- (c.E minus marked keys) merged with the pairs of v, keeping the smaller priority.
            merged_.clear();
            size_t i = 0, m = 0, j = 0;
            while (i < c.E.size() || j < pairs_.size()) {
                if (j == pairs_.size() || (i < c.E.size() && c.E[i].key < pairs_[j].key)) {
                    const Entry& e = c.E[i++];
                    while (m < marks_.size() && marks_[m] < e.key) m++;
                    if (m < marks_.size() && marks_[m] == e.key) continue;  // deleted by a marker
                    merged_.push_back(e);
                } else if (i == c.E.size() || pairs_[j].key < c.E[i].key) {
                    merged_.push_back(pairs_[j++]);  // a pair of v is newer than a marker of v
                } else {
                    Entry e = pairs_[j++];
                    const Entry& old = c.E[i++];
                    // The old pair counts only if no marker deletes it.
                    while (m < marks_.size() && marks_[m] < old.key) m++;
                    bool deleted = m < marks_.size() && marks_[m] == old.key;
                    if (!deleted && old.prio < e.prio) e.prio = old.prio;
                    merged_.push_back(e);
                }
            }
            c.E.swap(merged_);
            if (children_internal && !marks_.empty()) {
                ids_.clear();
                std::set_union(c.D.begin(), c.D.end(), marks_.begin(), marks_.end(), std::back_inserter(ids_));
                c.D.swap(ids_);
            }
        }
        v.clear();
        truncate(c0);
        truncate(c1);
    }

    // PullUp(v): move the B/2 smallest pairs of both children (or all of them) into the empty v.
    void pull_up(Buffer& v, Buffer& c0, Buffer& c1) {
        if (!v.E.empty() || !v.D.empty()) throw std::logic_error("PullUp precondition violated");
        size_t n0 = c0.E.size(), total = n0 + c1.E.size();
        size_t take = std::min<size_t>(p_.B / 2, total);
        auto at = [&](uint32_t i) -> const Entry& { return i < n0 ? c0.E[i] : c1.E[i - n0]; };
        chosen_.assign(total, 0);
        if (take == total) {
            std::fill(chosen_.begin(), chosen_.end(), 1);
        } else if (take > 0) {
            order_.resize(total);
            for (uint32_t i = 0; i < total; i++) order_[i] = i;
            std::nth_element(order_.begin(), order_.begin() + static_cast<long>(take), order_.end(),
                             [&](uint32_t x, uint32_t y) {
                                 const Entry& a = at(x);
                                 const Entry& b = at(y);
                                 return a.prio != b.prio ? a.prio < b.prio : a.key < b.key;
                             });
            for (size_t i = 0; i < take; i++) chosen_[order_[i]] = 1;
        }
        // v.E: the chosen pairs of both children, merged by key; the children keep the others.
        v.E.reserve(take);
        size_t i = 0, j = 0;
        while (i < n0 || j < c1.E.size()) {
            while (i < n0 && !chosen_[i]) i++;
            while (j < c1.E.size() && !chosen_[n0 + j]) j++;
            if (i == n0 && j == c1.E.size()) break;
            if (j == c1.E.size() || (i < n0 && c0.E[i].key < c1.E[j].key)) {
                v.E.push_back(c0.E[i++]);
            } else if (i == n0 || c1.E[j].key < c0.E[i].key) {
                v.E.push_back(c1.E[j++]);
            } else {
                throw std::logic_error("key present in both children");
            }
        }
        size_t w = 0;
        for (size_t r = 0; r < n0; r++)
            if (!chosen_[r]) c0.E[w++] = c0.E[r];
        c0.E.resize(w);
        w = 0;
        for (size_t r = 0; r < c1.E.size(); r++)
            if (!chosen_[n0 + r]) c1.E[w++] = c1.E[r];
        c1.E.resize(w);
    }

    static constexpr uint64_t kInitBatchBytes = 1 << 20;

    Store& store_;
    Params p_;
    Prf prf_;
    Aead aead_;
    Key instance_{};
    unsigned L_ = 0;
    uint64_t leaves_ = 0, first_leaf_ = 0;
    uint64_t dcap_ = 0, elem_size_ = 0, internal_plain_ = 0, leaf_plain_ = 0;
    std::unordered_map<uint64_t, Buffer> held_;  // buckets currently held by the client
    uint64_t base_records_ = 0;
    uint64_t overflows_ = 0;
    uint64_t crypto_ns_ = 0;
    PeakCounter memory_;
    // Scratch space, reused across calls to avoid allocations.
    std::vector<uint64_t> keys_, hashes_, marks_, ids_;
    std::vector<Entry> pairs_, merged_;
    std::vector<uint32_t> order_;
    std::vector<char> chosen_;
};

}  // namespace opq
