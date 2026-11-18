#pragma once

// Fixed-width little-endian encodings (inside encrypted objects and as cipher inputs) and big-endian
// encodings (network protocol), independent of the host byte order.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace opq {

// Largest value representable in `bytes` bytes.
inline uint64_t max_value(size_t bytes) {
    if (bytes == 0 || bytes > 8) throw std::invalid_argument("field width must be 1..8 bytes");
    return bytes == 8 ? ~0ULL : (1ULL << (8 * bytes)) - 1;
}

// Writes the `len` low-order bytes of `value` to p, least significant first (len <= 8).
inline void write_le(void* p, uint64_t value, size_t len) {
    auto* q = static_cast<unsigned char*>(p);
    for (size_t i = 0; i < len; i++, value >>= 8) q[i] = static_cast<unsigned char>(value & 0xFF);
}

inline uint64_t read_le(const void* p, size_t len) {
    const auto* q = static_cast<const unsigned char*>(p);
    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) value |= static_cast<uint64_t>(q[i]) << (8 * i);
    return value;
}

inline void store_le64(void* p, uint64_t v) { write_le(p, v, 8); }
inline uint64_t load_le64(const void* p) { return read_le(p, 8); }

inline void put_be64(std::string& out, uint64_t value) {
    for (int i = 7; i >= 0; i--) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

inline uint64_t get_be64(const void* p) {
    const auto* q = static_cast<const unsigned char*>(p);
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | q[i];
    return value;
}

}  // namespace opq
