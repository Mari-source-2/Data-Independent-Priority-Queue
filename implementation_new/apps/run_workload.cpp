// Runs a workload produced by the generator against one of the two queues, checks every answer
// against the reference answers, and prints timing, communication and memory statistics.
//
// Example:
//   run_workload --queue pq --n 1048576 --B 2048 --mode streaming --workload w.txt --answers a.txt
//   run_workload --queue poh --n 1048576 --z 2 --store remote --port 5001 --workload w.txt --answers a.txt
//
// Besides the totals, it prints percentiles of the per-operation time and a histogram with buckets
// [2^k, 2^(k+1)) microseconds; --latency-file writes every operation's time in microseconds, one per line.
//
// --warmup K executes the first K operations without measuring them (and, for the in-process store,
// without the emulated round-trip time), so that measurements start from a filled queue. With
// --ready-file R --go-file G, the runner creates R after the warmup and waits until G exists, so that a
// script can, for example, enable network emulation in between. All answers are always checked.
//
// --profiles name:rtt_us:mbit,... (in-process store only) evaluates several emulated networks in one run:
// every operation's computation time, round trips and transferred bytes are recorded once, and its time
// in a profile is the computation time plus one round-trip time per read request plus the transfer time
// of its bytes, as charged by the emulated store. Per profile, it prints <name>.avg_op_ms, <name>.worst_op_ms,
// <name>.p50_op_ms, ..., <name>.total_ms and <name>.network_ms, and with --latency-file F writes F-<name>.txt.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "factory.hpp"

using namespace opq;

namespace {

struct Profile {
    std::string name;
    uint64_t rtt_us = 0, mbit = 0;
};

std::vector<Profile> parse_profiles(const std::string& spec) {
    std::vector<Profile> out;
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        std::stringstream is(item);
        Profile p;
        std::string rtt, mbit;
        if (!std::getline(is, p.name, ':') || !std::getline(is, rtt, ':') || !std::getline(is, mbit) || p.name.empty())
            throw std::invalid_argument("--profiles expects name:rtt_us:mbit,...");
        p.rtt_us = std::stoull(rtt);
        p.mbit = std::stoull(mbit);
        out.push_back(p);
    }
    return out;
}

double percentile_ms(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t i = static_cast<size_t>(std::ceil(p / 100.0 * sorted.size()));
    return sorted[std::min(sorted.size() - 1, i == 0 ? 0 : i - 1)] / 1e6;
}

std::string format(const std::optional<Pair>& r) {
    if (!r) return "empty";
    return "(" + std::to_string(r->first) + ", " + std::to_string(r->second) + ")";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        Options o(argc, argv, {"workload", "answers", "max-ops", "warmup", "ready-file", "go-file", "latency-file", "profiles"});
        std::ifstream workload(o.require("workload"));
        std::ifstream answers(o.require("answers"));
        if (!workload.good() || !answers.good()) throw std::runtime_error("cannot open workload or answers");
        uint64_t max_ops = o.u64("max-ops", ~0ULL);

        auto store = make_store(o);
        auto queue = make_queue(o, *store);
        uint64_t warmup = o.u64("warmup", 0);
        auto* local = dynamic_cast<LocalStore*>(store.get());
        std::vector<Profile> profiles = parse_profiles(o.str("profiles", ""));
        const bool multi = !profiles.empty();
        if (multi && !local) throw std::invalid_argument("--profiles needs the in-process store");
        if ((warmup > 0 || multi) && local) local->set_network(0, 0);
        QueueStats base;

        uint64_t count[5] = {0, 0, 0, 0, 0};
        uint64_t mismatches = 0;
        std::string line, expected;
        auto t0 = std::chrono::steady_clock::now();
        uint64_t done = 0;
        uint64_t worst_ns = 0;
        uint64_t max_objects = 0, max_round_trips = 0;  // per operation
        std::vector<uint64_t> latency_ns;
        std::vector<uint64_t> op_round_trips, op_bytes;  // per measured operation, with --profiles
        uint64_t seen = 0;  // operations executed, including the warmup
        while (done < max_ops && std::getline(workload, line)) {
            if (line.empty()) continue;
            if (warmup > 0 && seen == warmup) {
                if (o.has("ready-file")) {
                    std::ofstream(o.str("ready-file")) << "ready\n";
                    while (!std::ifstream(o.require("go-file")).good())
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (local && !multi) local->set_network(o.u64("rtt-us", 0), o.u64("bandwidth-mbit", 0));
                store->reset_stats();
                base = queue->stats();
                for (auto& c : count) c = 0;
                done = 0;
                worst_ns = max_objects = max_round_trips = 0;
                latency_ns.clear();
                op_round_trips.clear();
                op_bytes.clear();
                t0 = std::chrono::steady_clock::now();
            }
            seen++;
            // Parse before timing, so that only the queue operation is measured.
            int op = line[0] - '0';
            uint64_t k = 0, p = 0;
            if (op < 0 || op > 4) throw std::runtime_error("malformed workload line: " + line);
            if (op <= 2) {
                std::stringstream ss(line);
                uint64_t code;
                char sep;
                ss >> code >> sep >> k;
                if (op != 2) ss >> sep >> p;
                if (!ss) throw std::runtime_error("malformed workload line: " + line);
            }
            StoreStats before = store->stats();
            auto s0 = std::chrono::steady_clock::now();
            std::optional<Pair> result;
            if (op == 0 || op == 1)
                queue->insert(k, p);
            else if (op == 2)
                queue->erase(k);
            else
                result = op == 3 ? queue->extract_min() : queue->find_min();
            auto s1 = std::chrono::steady_clock::now();
            const StoreStats& after = store->stats();
            // Measured time plus the network delay charged by an emulated network (zero otherwise).
            uint64_t op_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count() +
                             (after.network_ns - before.network_ns);
            worst_ns = std::max(worst_ns, op_ns);
            latency_ns.push_back(op_ns);
            max_objects = std::max(max_objects, after.objects_read + after.objects_written - before.objects_read -
                                                    before.objects_written);
            max_round_trips = std::max(max_round_trips, after.round_trips - before.round_trips);
            if (multi) {
                op_round_trips.push_back(after.round_trips - before.round_trips);
                op_bytes.push_back(after.bytes_up + after.bytes_down - before.bytes_up - before.bytes_down);
            }
            if (op >= 3) {
                if (!std::getline(answers, expected)) throw std::runtime_error("answers file too short");
                if (format(result) != expected) {
                    if (mismatches < 10)
                        std::cerr << "wrong answer at operation " << done << ": got " << format(result) << ", expected "
                                  << expected << "\n";
                    mismatches++;
                }
            }
            count[op]++;
            if (warmup == 0 || seen > warmup) done++;
        }
        auto t1 = std::chrono::steady_clock::now();
        double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        uint64_t sum_ns = 0;
        for (uint64_t ns : latency_ns) sum_ns += ns;
        double total_ms = sum_ns / 1e6;  // time spent in queue operations

        print_stats(*queue, *store, base);
        std::cout << "warmup_ops=" << std::min(seen, warmup) << "\nmeasured_ops=" << done << "\n";
        if (o.has("latency-file") && !multi) {
            std::ofstream lf(o.str("latency-file"));
            for (uint64_t ns : latency_ns) lf << ns / 1000.0 << "\n";
        }
        std::vector<uint64_t> sorted = latency_ns;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) -> double {
            if (sorted.empty()) return 0;
            size_t i = static_cast<size_t>(std::ceil(p / 100.0 * sorted.size()));
            return sorted[std::min(sorted.size() - 1, i == 0 ? 0 : i - 1)] / 1e6;
        };
        std::cout << "p50_op_ms=" << pct(50) << "\np90_op_ms=" << pct(90) << "\np99_op_ms=" << pct(99)
                  << "\np999_op_ms=" << pct(99.9) << "\n";
        for (const Profile& pr : profiles) {
            // Same charges as LocalStore::charge, per operation instead of per request.
            std::vector<uint64_t> t(latency_ns.size());
            uint64_t net = 0, sum = 0, worst = 0;
            for (size_t i = 0; i < t.size(); i++) {
                uint64_t c = op_round_trips[i] * pr.rtt_us * 1000 + (pr.mbit > 0 ? op_bytes[i] * 8 * 1000 / pr.mbit : 0);
                t[i] = latency_ns[i] + c;
                net += c;
                sum += t[i];
                worst = std::max(worst, t[i]);
            }
            if (o.has("latency-file")) {
                std::ofstream lf(o.str("latency-file") + "-" + pr.name + ".txt");
                for (uint64_t ns : t) lf << ns / 1000.0 << "\n";
            }
            std::vector<uint64_t> st = t;
            std::sort(st.begin(), st.end());
            const std::string& n = pr.name;
            std::cout << n << ".total_ms=" << sum / 1e6 << "\n" << n << ".network_ms=" << net / 1e6 << "\n"
                      << n << ".avg_op_ms=" << (t.empty() ? 0 : sum / 1e6 / t.size()) << "\n"
                      << n << ".worst_op_ms=" << worst / 1e6 << "\n" << n << ".p50_op_ms=" << percentile_ms(st, 50) << "\n"
                      << n << ".p90_op_ms=" << percentile_ms(st, 90) << "\n" << n << ".p99_op_ms=" << percentile_ms(st, 99) << "\n"
                      << n << ".p999_op_ms=" << percentile_ms(st, 99.9) << "\n";
        }
        std::vector<uint64_t> hist;
        for (uint64_t ns : latency_ns) {
            size_t k = 0;
            for (uint64_t us = ns / 1000; us > 1; us >>= 1) k++;
            if (hist.size() <= k) hist.resize(k + 1, 0);
            hist[k]++;
        }
        std::cout << "histogram_us=";
        for (size_t k = 0; k < hist.size(); k++)
            if (hist[k]) std::cout << "[" << (k == 0 ? 0 : (1ULL << k)) << "," << (2ULL << k) << "):" << hist[k] << " ";
        std::cout << "\n";
        std::cout << "inserts=" << count[0] << "\ndecrease_keys=" << count[1] << "\ndeletes=" << count[2]
                  << "\nextract_mins=" << count[3] << "\nfind_mins=" << count[4] << "\n"
                  << "total_ms=" << total_ms << "\n"
                  << "wall_ms=" << wall_ms << "\n"
                  << "avg_op_ms=" << (done ? total_ms / done : 0) << "\n"
                  << "worst_op_ms=" << worst_ns / 1e6 << "\n"
                  << "max_objects_per_op=" << max_objects << "\n"
                  << "max_round_trips_per_op=" << max_round_trips << "\n"
                  << "mismatches=" << mismatches << "\n";
        store->close();
        if (mismatches > 0) return 2;
        return queue->stats().overflows == 0 ? 0 : 3;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
