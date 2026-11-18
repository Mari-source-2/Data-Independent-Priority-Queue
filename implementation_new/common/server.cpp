// Generic bucket server: stores tables of fixed-size encrypted objects in memory-mapped files and
// answers the read/write requests of the protocol described in store.hpp. All queues use it.
// It serves one client at a time and validates every size it receives.
//
// Usage: server <port> <storage_directory> [--once]

#include <csignal>
#include <iostream>

#include "store.hpp"

using namespace opq;

namespace {

std::vector<ObjRef> recv_refs(int client, const FileTables& files, uint64_t& total) {
    uint64_t n = recv_u64(client);
    if (n > kMaxObjectsPerRequest) throw std::length_error("too many objects in one request");
    std::string hdr(16 * n, '\0');
    recv_all(client, hdr.data(), hdr.size());
    std::vector<ObjRef> refs(n);
    total = 0;
    for (uint64_t i = 0; i < n; i++) {
        refs[i] = ObjRef{get_be64(hdr.data() + 16 * i), get_be64(hdr.data() + 16 * i + 8)};
        total = checked_add(total, files.check(refs[i]));
    }
    return refs;
}

void serve(int client, const std::string& dir) {
    FileTables files;
    std::string buf;
    while (true) {
        char op;
        try {
            recv_all(client, &op, 1);
        } catch (...) {
            return;  // client disconnected
        }
        if (op == 'Q') return;
        if (op == 'C') {
            uint64_t nt = recv_u64(client);
            if (nt == 0 || nt > kMaxTables) throw std::invalid_argument("invalid number of tables");
            std::vector<TableSpec> tables(nt);
            for (auto& t : tables) {
                t.slot_size = recv_u64(client);
                t.count = recv_u64(client);
            }
            uint64_t len = recv_u64(client);
            if (len == 0 || len > kMaxNameLength) throw std::invalid_argument("invalid table name length");
            std::string name(len, '\0');
            recv_all(client, name.data(), len);
            if (name.find('/') != std::string::npos || name.find("..") != std::string::npos ||
                name.find('\0') != std::string::npos)
                throw std::invalid_argument("invalid table name");
            files.create(dir + "/" + name, tables);  // validates the table sizes
            send_all(client, std::string(1, 'K'));
        } else if (op == 'R') {
            uint64_t total = 0;
            std::vector<ObjRef> refs = recv_refs(client, files, total);
            buf.resize(total);
            uint64_t off = 0;
            for (const ObjRef& r : refs) {
                uint64_t size = files.tables()[r.table].slot_size;
                std::memcpy(buf.data() + off, files.object(r), size);
                off += size;
            }
            send_all(client, buf);  // the whole reply in one send
        } else if (op == 'W') {
            uint64_t total = 0;
            std::vector<ObjRef> refs = recv_refs(client, files, total);
            buf.resize(total);
            recv_all(client, buf.data(), buf.size());  // all payloads at once
            uint64_t off = 0;
            for (const ObjRef& r : refs) {
                uint64_t size = files.tables()[r.table].slot_size;
                std::memcpy(files.object(r), buf.data() + off, size);
                off += size;
            }
        } else {
            throw std::runtime_error(std::string("unknown request '") + op + "'");
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            std::cerr << "usage: server <port> <storage_directory> [--once]\n";
            return 1;
        }
        std::signal(SIGPIPE, SIG_IGN);
        int port = std::stoi(argv[1]);
        std::string dir = argv[2];
        bool once = argc > 3 && std::string(argv[3]) == "--once";

        int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) throw std::runtime_error("socket failed");
        int yes = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listener, 1) != 0)
            throw std::runtime_error("cannot listen on port " + std::to_string(port));
        std::cerr << "server listening on port " << port << "\n";
        do {
            int client = ::accept(listener, nullptr, nullptr);
            if (client < 0) continue;
            configure_socket(client);
            try {
                serve(client, dir);
            } catch (const std::exception& e) {
                std::cerr << "server error: " << e.what() << "\n";
            }
            ::close(client);
        } while (!once);
        ::close(listener);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "server: " << e.what() << "\n";
        return 1;
    }
}
