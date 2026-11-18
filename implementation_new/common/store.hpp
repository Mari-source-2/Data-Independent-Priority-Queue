#pragma once

// Outsourced storage. The server stores a few tables of fixed-size objects (encrypted buckets)
// and supports exactly two requests:
//   read(list of objects)          -> one round trip, returns the objects in the given order;
//   write(list of objects, data)   -> one message without reply.
// Every request is recorded in the statistics, including a digest of the access trace.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "bytes.hpp"

namespace opq {

// Adds the time between construction and destruction to a counter.
class ScopedTimer {
   public:
    explicit ScopedTimer(uint64_t& acc) : acc_(acc), start_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() {
        acc_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count());
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

   private:
    uint64_t& acc_;
    std::chrono::steady_clock::time_point start_;
};

struct ObjRef {
    uint64_t table;
    uint64_t id;
};

struct TableSpec {
    uint64_t slot_size;  // bytes per object
    uint64_t count;      // number of objects
};

// Protocol limits, checked by both sides (the server must not trust the client's sizes).
constexpr uint64_t kMaxTables = 16;
constexpr uint64_t kMaxNameLength = 4096;
constexpr uint64_t kMaxObjectsPerRequest = 1ULL << 24;
constexpr uint64_t kMaxTableBytes = 1ULL << 46;  // 64 TiB

// Bytes of the request header of a read or write of `n` objects: opcode, count, (table, id) per object.
inline uint64_t request_header_bytes(uint64_t n) { return 1 + 8 + 16 * n; }

inline uint64_t checked_mul(uint64_t a, uint64_t b) {
    uint64_t r;
    if (__builtin_mul_overflow(a, b, &r)) throw std::length_error("size overflow");
    return r;
}
inline uint64_t checked_add(uint64_t a, uint64_t b) {
    uint64_t r;
    if (__builtin_add_overflow(a, b, &r)) throw std::length_error("size overflow");
    return r;
}

inline void validate_tables(const std::vector<TableSpec>& tables) {
    if (tables.empty() || tables.size() > kMaxTables) throw std::invalid_argument("invalid number of tables");
    for (const TableSpec& t : tables) {
        if (t.slot_size == 0 || t.count == 0 || checked_mul(t.slot_size, t.count) > kMaxTableBytes)
            throw std::invalid_argument("invalid table size");
    }
}

struct StoreStats {
    uint64_t round_trips = 0;      // read requests (each waits for a reply)
    uint64_t write_messages = 0;   // write requests (no reply)
    uint64_t objects_read = 0;
    uint64_t objects_written = 0;
    uint64_t bytes_down = 0;       // server -> client
    uint64_t bytes_up = 0;         // client -> server, including request headers
    uint64_t trace_digest = 1469598103934665603ULL;  // FNV-1a over every request: its kind, size and objects
    uint64_t store_ns = 0;         // time spent in read and write requests (network, server)
    uint64_t network_ns = 0;       // emulated network delay (LocalStore only), not included in store_ns

    void record_request(char kind, const std::vector<ObjRef>& refs) {
        mix(static_cast<uint64_t>(kind));
        mix(refs.size());
        for (const ObjRef& r : refs) {
            mix(r.table);
            mix(r.id);
        }
    }

   private:
    void mix(uint64_t v) {
        for (int i = 0; i < 8; i++) {
            trace_digest ^= (v >> (8 * i)) & 0xFF;
            trace_digest *= 1099511628211ULL;
        }
    }
};

class Store {
   public:
    virtual ~Store() = default;
    // Creates fresh tables (all objects zero-filled; the client initializes them afterwards).
    virtual void create(const std::string& name, const std::vector<TableSpec>& tables) = 0;
    virtual std::vector<std::string> read(const std::vector<ObjRef>& refs) = 0;
    virtual void write(const std::vector<ObjRef>& refs, const std::vector<std::string>& data) = 0;
    virtual void close() {}

    const StoreStats& stats() const { return stats_; }
    void reset_stats() { stats_ = StoreStats{}; }

   protected:
    uint64_t check_refs(const std::vector<ObjRef>& refs) const {  // returns the total payload size
        if (refs.size() > kMaxObjectsPerRequest) throw std::length_error("too many objects in one request");
        uint64_t total = 0;
        for (const ObjRef& r : refs) {
            if (r.table >= tables_.size() || r.id >= tables_[r.table].count)
                throw std::out_of_range("object reference out of range");
            total += tables_[r.table].slot_size;
        }
        return total;
    }
    void check_write(const std::vector<ObjRef>& refs, const std::vector<std::string>& data) const {
        if (refs.size() != data.size()) throw std::invalid_argument("write: size mismatch");
        for (size_t i = 0; i < refs.size(); i++)
            if (data[i].size() != tables_[refs[i].table].slot_size)
                throw std::invalid_argument("write: object of wrong size");
    }
    StoreStats stats_;
    std::vector<TableSpec> tables_;
};

// ---------------------------------------------------------------------------------------------
// File-backed tables, memory-mapped, used by the server and by the in-process store. Reading or
// writing an object is a memory copy, as for data in the untrusted memory of a TEE.
// ---------------------------------------------------------------------------------------------
class FileTables {
   public:
    FileTables() = default;
    FileTables(const FileTables&) = delete;
    FileTables& operator=(const FileTables&) = delete;
    ~FileTables() { close(); }

    void create(const std::string& name, const std::vector<TableSpec>& tables) {
        close();
        validate_tables(tables);
        for (size_t t = 0; t < tables.size(); t++) {
            std::string path = name + ".t" + std::to_string(t);
            uint64_t bytes = tables[t].slot_size * tables[t].count;  // validated above
            int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) throw std::runtime_error("cannot create " + path + ": " + std::strerror(errno));
            if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
                int e = errno;
                ::close(fd);
                throw std::runtime_error("cannot allocate " + path + ": " + std::strerror(e));
            }
            void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);  // the mapping keeps the file open
            if (p == MAP_FAILED) throw std::runtime_error("cannot map " + path + ": " + std::strerror(errno));
            maps_.push_back(Map{static_cast<char*>(p), bytes});
        }
        tables_ = tables;
    }

    const char* object(const ObjRef& r) const { return maps_[r.table].base + r.id * tables_[r.table].slot_size; }
    char* object(const ObjRef& r) { return maps_[r.table].base + r.id * tables_[r.table].slot_size; }

    // Throws unless r refers to an existing object; returns its size.
    uint64_t check(const ObjRef& r) const {
        if (r.table >= tables_.size() || r.id >= tables_[r.table].count)
            throw std::out_of_range("object reference out of range");
        return tables_[r.table].slot_size;
    }

    const std::vector<TableSpec>& tables() const { return tables_; }

    void close() {
        for (const Map& m : maps_) ::munmap(m.base, m.bytes);
        maps_.clear();
        tables_.clear();
    }

   private:
    struct Map {
        char* base;
        uint64_t bytes;
    };
    std::vector<TableSpec> tables_;
    std::vector<Map> maps_;
};

// ---------------------------------------------------------------------------------------------
// In-process store: same interface and statistics, no network (the "TEE" setting). Optionally, it
// emulates a network without waiting: every read request is charged a round-trip time, and every
// request the transfer time of its bytes at a given bandwidth (also writes, conservatively, although
// a real client does not wait for them). The charged delay is accumulated in stats().network_ns, and
// run_workload adds it to the time of the operation that issued the request. Charging instead of
// sleeping makes the emulation exact and independent of the operating system's timer resolution.
// ---------------------------------------------------------------------------------------------
class LocalStore : public Store {
   public:
    explicit LocalStore(uint64_t rtt_us = 0, uint64_t bandwidth_mbit = 0) : rtt_us_(rtt_us), mbit_(bandwidth_mbit) {}
    void set_network(uint64_t rtt_us, uint64_t bandwidth_mbit) {
        rtt_us_ = rtt_us;
        mbit_ = bandwidth_mbit;
    }

    void create(const std::string& name, const std::vector<TableSpec>& tables) override {
        files_.create(name, tables);
        tables_ = tables;
    }

    std::vector<std::string> read(const std::vector<ObjRef>& refs) override {
        uint64_t payload = check_refs(refs);
        ScopedTimer timer(stats_.store_ns);
        uint64_t header = request_header_bytes(refs.size());
        charge(rtt_us_, header + payload);
        stats_.record_request('R', refs);
        std::vector<std::string> out;
        out.reserve(refs.size());
        for (const ObjRef& r : refs) out.emplace_back(files_.object(r), tables_[r.table].slot_size);
        stats_.round_trips++;
        stats_.objects_read += refs.size();
        stats_.bytes_up += header;
        stats_.bytes_down += payload;
        return out;
    }

    void write(const std::vector<ObjRef>& refs, const std::vector<std::string>& data) override {
        uint64_t payload = check_refs(refs);
        check_write(refs, data);
        ScopedTimer timer(stats_.store_ns);
        uint64_t header = request_header_bytes(refs.size());
        charge(0, header + payload);
        stats_.record_request('W', refs);
        for (size_t i = 0; i < refs.size(); i++) std::memcpy(files_.object(refs[i]), data[i].data(), data[i].size());
        stats_.write_messages++;
        stats_.objects_written += refs.size();
        stats_.bytes_up += header + payload;
    }

   private:
    void charge(uint64_t rtt_us, uint64_t bytes) {
        // At 1 Mbit/s, a bit takes 1 microsecond = 1000 ns.
        stats_.network_ns += rtt_us * 1000 + (mbit_ > 0 ? bytes * 8 * 1000 / mbit_ : 0);
    }
    uint64_t rtt_us_ = 0, mbit_ = 0;
    FileTables files_;
};

// ---------------------------------------------------------------------------------------------
// Network protocol (all integers big-endian u64):
//   'C' ntables {slot_size count}* namelen name   -> server replies 'K' when the tables exist
//   'R' n {table id}*                             -> server replies with the n objects, concatenated
//   'W' n {table id}* payloads                    -> no reply
//   'Q'                                           -> server closes the connection
// ---------------------------------------------------------------------------------------------
inline void send_all(int sock, const char* data, size_t len) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;  // a closed connection raises an error, not SIGPIPE
#else
    constexpr int kFlags = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock, data + sent, len - sent, kFlags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("send failed");
        sent += static_cast<size_t>(n);
    }
}
inline void send_all(int sock, const std::string& buf) { send_all(sock, buf.data(), buf.size()); }

inline void recv_all(int sock, char* out, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(sock, out + got, len - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("connection closed while receiving");
        got += static_cast<size_t>(n);
    }
}

inline uint64_t recv_u64(int sock) {
    char b[8];
    recv_all(sock, b, 8);
    return get_be64(b);
}

// Socket options for both ends: no Nagle delay, and no SIGPIPE on macOS.
inline void configure_socket(int sock) {
    int flag = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#ifdef SO_NOSIGPIPE
    ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
#endif
}

class RemoteStore : public Store {
   public:
    RemoteStore(const std::string& host, int port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) throw std::runtime_error("invalid address " + host);
        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) throw std::runtime_error("socket failed");
        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(sock_);
            sock_ = -1;
            throw std::runtime_error("cannot connect to " + host + ":" + std::to_string(port));
        }
        configure_socket(sock_);
    }
    RemoteStore(const RemoteStore&) = delete;
    RemoteStore& operator=(const RemoteStore&) = delete;
    ~RemoteStore() override { close(); }

    void create(const std::string& name, const std::vector<TableSpec>& tables) override {
        validate_tables(tables);
        if (name.size() > kMaxNameLength) throw std::invalid_argument("table name too long");
        std::string msg(1, 'C');
        put_be64(msg, tables.size());
        for (const TableSpec& t : tables) {
            put_be64(msg, t.slot_size);
            put_be64(msg, t.count);
        }
        put_be64(msg, name.size());
        msg += name;
        send_all(sock_, msg);
        char ack;
        recv_all(sock_, &ack, 1);
        if (ack != 'K') throw std::runtime_error("server failed to create tables");
        tables_ = tables;
    }

    std::vector<std::string> read(const std::vector<ObjRef>& refs) override {
        uint64_t payload = check_refs(refs);
        ScopedTimer timer(stats_.store_ns);
        std::string msg = header('R', refs);
        stats_.record_request('R', refs);
        send_all(sock_, msg);
        std::string reply(payload, '\0');
        recv_all(sock_, reply.data(), reply.size());  // the whole reply at once
        std::vector<std::string> out;
        out.reserve(refs.size());
        uint64_t off = 0;
        for (const ObjRef& r : refs) {
            uint64_t size = tables_[r.table].slot_size;
            out.emplace_back(reply.data() + off, size);
            off += size;
        }
        stats_.round_trips++;
        stats_.objects_read += refs.size();
        stats_.bytes_up += msg.size();
        stats_.bytes_down += payload;
        return out;
    }

    void write(const std::vector<ObjRef>& refs, const std::vector<std::string>& data) override {
        uint64_t payload = check_refs(refs);
        check_write(refs, data);
        ScopedTimer timer(stats_.store_ns);
        std::string msg = header('W', refs);
        stats_.record_request('W', refs);
        msg.reserve(msg.size() + payload);
        for (const std::string& d : data) msg += d;
        send_all(sock_, msg);  // one message, no reply
        stats_.write_messages++;
        stats_.objects_written += refs.size();
        stats_.bytes_up += msg.size();
    }

    void close() override {
        if (sock_ >= 0) {
            try {
                send_all(sock_, std::string(1, 'Q'));
            } catch (...) {
            }
            ::close(sock_);
            sock_ = -1;
        }
    }

   private:
    static std::string header(char op, const std::vector<ObjRef>& refs) {
        std::string msg;
        msg.reserve(request_header_bytes(refs.size()));
        msg.push_back(op);
        put_be64(msg, refs.size());
        for (const ObjRef& r : refs) {
            put_be64(msg, r.table);
            put_be64(msg, r.id);
        }
        return msg;
    }
    int sock_ = -1;
};

}  // namespace opq
