// Checks that the client of our queues detects a malicious server that
//   - swaps two buckets,
//   - replays an older version of a bucket (within a session), or
//   - serves a bucket of an earlier session under the same keys (as with --keyfile),
// in every relocation mode, and that an honest server is accepted.

#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "../pq/buffered_queue.hpp"
#include "../pq/spread_queue.hpp"

using namespace opq;

namespace {

enum class Attack { None, Swap, Replay, CrossSession };

// A store that records what the client writes and lets the test interfere with what it reads.
class EvilStore : public LocalStore {
   public:
    Attack attack = Attack::None;
    std::map<std::pair<uint64_t, uint64_t>, std::string>* earlier_session = nullptr;  // last object per location
    std::map<std::pair<uint64_t, uint64_t>, std::string> first_seen;
    uint64_t substitutions = 0;

    std::vector<std::string> read(const std::vector<ObjRef>& refs) override {
        std::vector<std::string> out = LocalStore::read(refs);
        for (size_t i = 0; i < refs.size(); i++) {
            auto loc = std::make_pair(refs[i].table, refs[i].id);
            first_seen.emplace(loc, out[i]);
            if (attack == Attack::Replay && first_seen.at(loc) != out[i]) {
                out[i] = first_seen.at(loc);
                substitutions++;
            } else if (attack == Attack::Swap && refs[i].table == 0 && refs[i].id == 0) {
                out[i] = LocalStore::read({ObjRef{0, 1}})[0];
                substitutions++;
            } else if (attack == Attack::CrossSession && earlier_session && earlier_session->count(loc)) {
                out[i] = earlier_session->at(loc);
                substitutions++;
            }
        }
        return out;
    }
    void write(const std::vector<ObjRef>& refs, const std::vector<std::string>& data) override {
        for (size_t i = 0; i < refs.size(); i++) written[{refs[i].table, refs[i].id}] = data[i];
        LocalStore::write(refs, data);
    }
    std::map<std::pair<uint64_t, uint64_t>, std::string> written;
};

const Key kEnc = random_key(), kPrf = random_key();  // shared by all sessions, as with --keyfile

std::unique_ptr<PriorityQueue> make(const std::string& kind, Store& store, const std::string& dir) {
    TreeQueueParams base;
    base.n = 1024;
    base.B = 64;
    base.key_bytes = 2;
    base.prio_bytes = 2;
    base.name = dir + "/tamper_" + kind;
    base.enc_key = kEnc;
    base.prf_key = kPrf;
    if (kind == "spread") {
        SpreadQueue::Params p;
        static_cast<TreeQueueParams&>(p) = base;
        return std::make_unique<SpreadQueue>(store, p);
    }
    BufferedQueue::Params p;
    static_cast<TreeQueueParams&>(p) = base;
    p.mode = kind == "cached" ? RelocationMode::Cached : RelocationMode::Streaming;
    return std::make_unique<BufferedQueue>(store, p);
}

// Runs a session; returns true if the client reported an authentication failure.
bool session(const std::string& kind, Attack attack, const std::string& dir, uint64_t first_key,
             std::map<std::pair<uint64_t, uint64_t>, std::string>* earlier, EvilStore* keep = nullptr) {
    EvilStore local;
    EvilStore& store = keep ? *keep : local;
    store.earlier_session = earlier;
    auto q = make(kind, store, dir);
    uint64_t k = first_key;
    try {
        for (int i = 0; i < 64; i++) q->insert(k++, 100 + i);  // honest phase
        store.attack = attack;
        for (int i = 0; i < 256; i++) q->insert(k++, 100 + i);
        while (q->extract_min()) {
        }
    } catch (const std::runtime_error& e) {
        return std::string(e.what()).find("authentication failed") != std::string::npos;
    }
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string dir = argc > 1 ? argv[1] : ".";
    bool ok = true;
    for (std::string kind : {"cached", "streaming", "spread"}) {
        EvilStore first;
        bool honest = !session(kind, Attack::None, dir, 1, nullptr, &first);
        bool swap = session(kind, Attack::Swap, dir, 1, nullptr);
        bool replay = session(kind, Attack::Replay, dir, 1, nullptr);
        bool cross = session(kind, Attack::CrossSession, dir, 5000, &first.written);
        std::cout << kind << ": honest run passes " << honest << ", swap detected " << swap << ", replay detected "
                  << replay << ", earlier session detected " << cross << "\n";
        ok = ok && honest && swap && replay && cross;
    }
    return ok ? 0 : 1;
}
