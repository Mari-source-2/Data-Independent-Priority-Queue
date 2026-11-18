#pragma once

// Command-line options shared by the drivers, and construction of stores and queues.

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

#include "../common/crypto.hpp"
#include "../common/store.hpp"
#include "../pq/buffered_queue.hpp"
#include "../pq/spread_queue.hpp"
#include "../poh/path_heap.hpp"

namespace opq {

// Options of make_store and make_queue; drivers add their own.
inline const std::set<std::string> kStoreAndQueueOptions = {
    "store", "host", "port", "rtt-us", "bandwidth-mbit", "queue", "name", "dir", "keyfile", "n", "B",
    "mode", "a", "z", "seed", "no-key-map", "key-bytes", "prio-bytes"};

class Options {
   public:
    // Parses --name value and --flag arguments; rejects options outside kStoreAndQueueOptions and `extra`,
    // so that a misspelled option is an error rather than silently ignored.
    Options(int argc, char* argv[], const std::set<std::string>& extra) {
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a.rfind("--", 0) != 0) throw std::invalid_argument("unexpected argument " + a);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                values_[a.substr(2)] = argv[++i];
            else
                values_[a.substr(2)] = "1";  // flag
        }
        for (const auto& kv : values_)
            if (!kStoreAndQueueOptions.count(kv.first) && !extra.count(kv.first))
                throw std::invalid_argument("unknown option --" + kv.first);
    }
    void set_default(const std::string& k, const std::string& v) { values_.emplace(k, v); }
    bool has(const std::string& k) const { return values_.count(k) > 0; }
    std::string str(const std::string& k, const std::string& def = "") const {
        auto it = values_.find(k);
        return it == values_.end() ? def : it->second;
    }
    uint64_t u64(const std::string& k, uint64_t def) const {
        auto it = values_.find(k);
        return it == values_.end() ? def : std::stoull(it->second);
    }
    std::string require(const std::string& k) const {
        auto it = values_.find(k);
        if (it == values_.end()) throw std::invalid_argument("missing option --" + k);
        return it->second;
    }

   private:
    std::map<std::string, std::string> values_;
};

inline std::unique_ptr<Store> make_store(const Options& o) {
    std::string kind = o.str("store", "local");
    if (kind == "local") return std::make_unique<LocalStore>(o.u64("rtt-us", 0), o.u64("bandwidth-mbit", 0));
    if (kind == "remote") return std::make_unique<RemoteStore>(o.str("host", "127.0.0.1"), static_cast<int>(o.u64("port", 5001)));
    throw std::invalid_argument("--store must be local or remote");
}

// --queue pq   : --n --B --mode cached|streaming|levelwise --a (relocation interval, default B/8, at most B/4);
//                levelwise is the baseline schedule of Jafargholi et al. on the same tree
// --queue spread: --n --B (Appendix B: relocations spread over the operations)
// --queue poh  : --n --z --seed --no-key-map (only for workloads of fresh insertions and extractions)
// common       : --key-bytes --prio-bytes --name --keyfile (persistent encryption key, optional)
inline std::unique_ptr<PriorityQueue> make_queue(const Options& o, Store& store) {
    std::string kind = o.require("queue");
    std::string name = o.str("name", o.str("dir", ".") + "/" + kind);
    // With --keyfile, the encryption key and the PRF key are both derived from the persistent master
    // key; otherwise both are fresh. Objects of different sessions are told apart by the random
    // instance identifier in their associated data, not by the keys.
    Key enc_key, prf_key;
    if (o.has("keyfile")) {
        auto master = load_or_create_key(o.str("keyfile"));
        enc_key = derive_key(master, 1);
        prf_key = derive_key(master, 2);
    } else {
        enc_key = random_key();
        prf_key = random_key();
    }
    if (o.str("store", "local") == "remote") {
        // The server places the tables in its own storage directory; only pass a plain name.
        auto slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
    }
    if (o.has("a") && kind != "pq") throw std::invalid_argument("--a applies only to --queue pq");
    if (kind == "pq") {
        BufferedQueue::Params p;
        p.n = o.u64("n", 0);
        p.B = o.u64("B", 0);
        p.key_bytes = o.u64("key-bytes", 3);
        p.prio_bytes = o.u64("prio-bytes", 3);
        std::string mode = o.str("mode", "streaming");
        if (mode != "cached" && mode != "streaming" && mode != "levelwise")
            throw std::invalid_argument("--mode must be cached, streaming or levelwise");
        p.mode = mode == "cached" ? RelocationMode::Cached : mode == "levelwise" ? RelocationMode::Levelwise : RelocationMode::Streaming;
        p.a = o.u64("a", 0);
        if (o.has("a") && (p.a < 1 || p.a > p.B / 4))  // checked before the tables are created
            throw std::invalid_argument("the relocation interval a must satisfy 1 <= a <= B/4");
        p.name = name;
        p.enc_key = enc_key;
        p.prf_key = prf_key;
        return std::make_unique<BufferedQueue>(store, p);
    }
    if (kind == "spread") {
        SpreadQueue::Params p;
        p.n = o.u64("n", 0);
        p.B = o.u64("B", 0);
        p.key_bytes = o.u64("key-bytes", 3);
        p.prio_bytes = o.u64("prio-bytes", 3);
        p.name = name;
        p.enc_key = enc_key;
        p.prf_key = prf_key;
        return std::make_unique<SpreadQueue>(store, p);
    }
    if (kind == "poh") {
        PathHeap::Params p;
        p.n = o.u64("n", 0);
        p.z = o.u64("z", 2);
        p.key_bytes = o.u64("key-bytes", 3);
        p.prio_bytes = o.u64("prio-bytes", 3);
        p.name = name;
        p.enc_key = enc_key;
        p.seed = o.u64("seed", 0);
        p.key_map = !o.has("no-key-map");
        return std::make_unique<PathHeap>(store, p);
    }
    throw std::invalid_argument("--queue must be pq, spread or poh");
}

// Prints the statistics; encryption time and PRF evaluations are counted from `base` on.
inline void print_stats(const PriorityQueue& q, const Store& store, const QueueStats& base = QueueStats{}) {
    QueueStats qs = q.stats();
    qs.encryption_ns -= base.encryption_ns;
    qs.prf_evaluations -= base.prf_evaluations;
    const StoreStats& ss = store.stats();
    std::cout << "queue=" << q.describe() << "\n"
              << "operations=" << qs.operations << "\n"
              << "round_trips=" << ss.round_trips << "\n"
              << "write_messages=" << ss.write_messages << "\n"
              << "objects_read=" << ss.objects_read << "\n"
              << "objects_written=" << ss.objects_written << "\n"
              << "bytes_down=" << ss.bytes_down << "\n"
              << "bytes_up=" << ss.bytes_up << "\n"
              << "trace_digest=" << ss.trace_digest << "\n"
              << "overflows=" << qs.overflows << "\n"
              << "peak_client_records=" << qs.peak_client_records << "\n"
              << "peak_stash=" << qs.peak_stash << "\n"
              << "position_map_entries=" << qs.position_map_entries << "\n"
              << "peak_client_records_incl_map=" << qs.peak_client_records + qs.position_map_entries << "\n"
              << "peak_rss_bytes=" << peak_rss_bytes() << "\n"
              << "store_ms=" << ss.store_ns / 1e6 << "\n"
              << "network_ms=" << ss.network_ns / 1e6 << "\n"
              << "encryption_ms=" << qs.encryption_ns / 1e6 << "\n"
              << "prf_evaluations=" << qs.prf_evaluations << "\n";
}

}  // namespace opq
