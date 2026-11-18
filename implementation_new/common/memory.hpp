#pragma once

#include <sys/resource.h>

#include <cstdint>

namespace opq {

// Peak resident set size of this process in bytes (getrusage reports bytes on macOS, KiB on Linux).
inline uint64_t peak_rss_bytes() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

// Tracks the peak of a quantity that the client updates explicitly, for example the number of
// records it holds in memory.
class PeakCounter {
   public:
    void set(uint64_t value) {
        current_ = value;
        if (value > peak_) peak_ = value;
    }
    uint64_t current() const { return current_; }
    uint64_t peak() const { return peak_; }

   private:
    uint64_t current_ = 0;
    uint64_t peak_ = 0;
};

}  // namespace opq
