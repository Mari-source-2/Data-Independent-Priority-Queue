// Dijkstra's algorithm on a DIMACS graph (".gr" format, arcs "a u v w"), using one of the two
// queues, and checked against a plain Dijkstra with std::priority_queue.
//
// As in the old driver, the graph and the distance array are kept at the client, so this run does
// not respect a small client-memory budget; it benchmarks the queue under a Dijkstra workload.
//
// --profiles name:rtt_us:mbit,... (in-process store) reports the total time for several emulated networks
// from one run: the measured time plus one round-trip time per read request plus the transfer time of all
// bytes, as charged by the emulated store; it prints <name>.total_ms for every profile.
//
// Example:
//   dijkstra --queue pq --B 2048 --graph USA-road-d.NY.gr --source 1 --key-bytes 4 --prio-bytes 5

#include <chrono>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <vector>

#include "factory.hpp"

using namespace opq;

int main(int argc, char* argv[]) {
    try {
        Options o(argc, argv, {"graph", "source", "profiles"});
        std::ifstream in(o.require("graph"));
        if (!in.good()) throw std::runtime_error("cannot open graph");
        std::vector<std::vector<std::pair<uint64_t, uint64_t>>> adj;
        std::string line;
        uint64_t arcs = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            char c;
            ss >> c;
            if (c == 'p') {
                std::string sp;
                uint64_t nv, na;
                ss >> sp >> nv >> na;
                adj.assign(nv + 1, {});
            } else if (c == 'a') {
                uint64_t u, v, w;
                ss >> u >> v >> w;
                if (u >= adj.size() || v >= adj.size()) throw std::runtime_error("arc with invalid vertex");
                adj[u].push_back({v, w});
                arcs++;
            }
        }
        uint64_t V = adj.empty() ? 0 : adj.size() - 1;
        uint64_t source = o.u64("source", 1);
        if (V == 0 || source == 0 || source > V) throw std::runtime_error("empty graph or invalid source");

        // Reference distances.
        const uint64_t INF = ~0ULL;
        std::vector<uint64_t> ref(V + 1, INF);
        {
            std::priority_queue<std::pair<uint64_t, uint64_t>, std::vector<std::pair<uint64_t, uint64_t>>, std::greater<>> pq;
            ref[source] = 0;
            pq.push({0, source});
            while (!pq.empty()) {
                auto [d, u] = pq.top();
                pq.pop();
                if (d != ref[u]) continue;
                for (auto [v, w] : adj[u])
                    if (d + w < ref[v]) {
                        ref[v] = d + w;
                        pq.push({ref[v], v});
                    }
            }
        }

        // The queue needs n >= the number of vertices that can be present at once; default n = V.
        auto store = make_store(o);
        o.set_default("n", std::to_string(V));
        auto queue = make_queue(o, *store);

        std::vector<uint64_t> dist(V + 1, INF);
        std::vector<char> done(V + 1, 0);
        uint64_t ops = 0;
        auto t0 = std::chrono::steady_clock::now();
        queue->insert(source, 0);
        ops++;
        while (true) {
            auto m = queue->extract_min();
            ops++;
            if (!m) break;
            auto [u, d] = *m;
            done[u] = 1;
            dist[u] = d;
            for (auto [v, w] : adj[u]) {
                if (done[v]) continue;
                queue->insert(v, d + w);  // decrease-key by key
                ops++;
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        uint64_t wrong = 0;
        for (uint64_t v = 1; v <= V; v++)
            if (dist[v] != ref[v]) wrong++;
        print_stats(*queue, *store);
        double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "vertices=" << V << "\narcs=" << arcs << "\nqueue_operations=" << ops << "\n"
                  << "total_ms=" << total_ms << "\n"
                  << "wrong_distances=" << wrong << "\n";
        // Emulated networks: the measured time (with the store's own charges removed) plus the charges of
        // each profile for all round trips and bytes of the run.
        const StoreStats& ss = store->stats();
        double compute_ms = total_ms - ss.network_ns / 1e6;
        std::stringstream ps(o.str("profiles", ""));
        std::string item;
        while (std::getline(ps, item, ',')) {
            if (item.empty()) continue;
            std::stringstream is(item);
            std::string name, rtt, mbit;
            if (!std::getline(is, name, ':') || !std::getline(is, rtt, ':') || !std::getline(is, mbit))
                throw std::invalid_argument("--profiles expects name:rtt_us:mbit,...");
            uint64_t r = std::stoull(rtt), b = std::stoull(mbit);
            double net_ms = (ss.round_trips * r * 1000.0 + (b > 0 ? (ss.bytes_up + ss.bytes_down) * 8.0 * 1000.0 / b : 0)) / 1e6;
            std::cout << name << ".total_ms=" << compute_ms + net_ms << "\n" << name << ".network_ms=" << net_ms << "\n";
        }
        store->close();
        if (wrong > 0) return 2;
        return queue->stats().overflows == 0 ? 0 : 3;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
