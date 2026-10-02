// bench/timer.hpp
//
// Timing utilities: a raw hardware tick counter, calibration to
// nanoseconds, and percentile summaries.

#pragma once

#include <algorithm>   // std::sort
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>   // pthread_set_qos_class_self_np
#endif

namespace lob::bench {

// ---------------------------------------------------------------------
// Raw tick counter: the cheapest clock the CPU offers.
// ---------------------------------------------------------------------
inline uint64_t read_ticks() noexcept {
#if defined(__aarch64__)
    // ARM (your Mac): the virtual counter. isb stops it being read early.
    uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
#elif defined(__x86_64__)
    // x86 (Linux later): rdtscp waits for earlier instructions to finish.
    uint32_t lo, hi, aux;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) : : "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// ---------------------------------------------------------------------
// How many ticks per nanosecond? Measured against the OS clock, so it
// works on any CPU without trusting what the hardware claims.
// ---------------------------------------------------------------------
inline double ticks_per_ns() {
    using clock = std::chrono::steady_clock;
    const auto c0 = clock::now();
    const uint64_t t0 = read_ticks();
    while (clock::now() - c0 < std::chrono::milliseconds(200)) {
        // busy-wait: also wakes the CPU up to full speed
    }
    const auto c1 = clock::now();
    const uint64_t t1 = read_ticks();
    const double ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
    return static_cast<double>(t1 - t0) / ns;
}

// ---------------------------------------------------------------------
// Ask macOS for a performance core (macOS doesn't allow core pinning).
// ---------------------------------------------------------------------
inline void prefer_performance_core() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// ---------------------------------------------------------------------
// Percentiles: sort the samples and read off positions.
// p50 = median: half the samples are faster than this.
// p99 = 99% are faster; only the slowest 1% are above it.
// ---------------------------------------------------------------------
struct Summary {
    double mean, p50, p90, p99, p999, max;
};

inline Summary summarize(std::vector<double> samples) {   // a copy: we sort it
    std::sort(samples.begin(), samples.end());
    auto at = [&](double q) {
        return samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))];
    };
    double sum = 0;
    for (double s : samples) sum += s;
    return Summary{sum / static_cast<double>(samples.size()),
                   at(0.50), at(0.90), at(0.99), at(0.999), samples.back()};
}

}  // namespace lob::bench