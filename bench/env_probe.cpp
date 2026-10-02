// bench/env_probe.cpp
//
// Prints facts about this machine that matter for benchmarking:
// CPU model, core counts, cache sizes, page size, and how precise
// our timer is. Run it once and paste the output into the README.

#include <algorithm>    // std::sort
#include <cstdint>      // fixed-size integer types: uint64_t, int64_t
#include <cstdio>       // std::printf, std::snprintf
#include <vector>       // std::vector (a growable array)

#include <pthread.h>    // pthread_set_qos_class_self_np (macOS thread priority)
#include <sys/sysctl.h> // sysctlbyname: ask macOS about the hardware

#if !defined(__aarch64__)
#error "env_probe supports Apple Silicon (arm64) only"
#endif

// ---------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------

// Read the ARM "virtual counter": a hardware clock that ticks at a fixed
// rate, independent of CPU speed. This is the closest thing on a Mac to
// x86's rdtsc instruction.
//   isb  = "instruction synchronization barrier": makes the CPU finish the
//          instructions before it, so the clock isn't read too early.
//   mrs  = "move from system register": copy the counter into a variable.
static inline uint64_t read_ticks() {
    uint64_t value;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) : : "memory");
    return value;
}

// How many times per second the counter ticks.
static inline uint64_t ticks_per_second() {
    uint64_t value;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

// ---------------------------------------------------------------------
// Hardware info via sysctl
// ---------------------------------------------------------------------

// Read a numeric hardware property. Returns -1 if macOS doesn't know it.
static long long sysctl_int(const char* name) {
    int64_t value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
        return -1;
    }
    return static_cast<long long>(value);
}

// Read a text hardware property into `out`.
static void sysctl_string(const char* name, char* out, size_t capacity) {
    size_t size = capacity;
    if (sysctlbyname(name, out, &size, nullptr, 0) != 0) {
        std::snprintf(out, capacity, "unknown");
    }
}

// ---------------------------------------------------------------------
// A known workload to test batch timing
// ---------------------------------------------------------------------

// The result goes into a volatile global so the compiler can't decide
// the work is unused and delete it.
volatile uint64_t g_sink = 0;

// N additions where each depends on the previous result, so the CPU must
// do them one after another. The empty asm line tells the compiler
// "x might have changed", which stops it replacing the loop with a formula.
static uint64_t dependent_adds(uint64_t n) {
    uint64_t x = 0;
    for (uint64_t i = 0; i < n; ++i) {
        x += i;
        asm volatile("" : "+r"(x));
    }
    return x;
}

int main() {
    // Ask macOS to run us on a performance core. macOS doesn't allow
    // pinning a thread to a specific core, so this is the best we can do.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    // ---- Machine ----
    char cpu[128];
    sysctl_string("machdep.cpu.brand_string", cpu, sizeof(cpu));

    std::printf("== Machine ==\n");
    std::printf("CPU                 : %s\n", cpu);
    std::printf("Performance cores   : %lld\n", sysctl_int("hw.perflevel0.physicalcpu"));
    std::printf("Efficiency cores    : %lld\n", sysctl_int("hw.perflevel1.physicalcpu"));
    std::printf("Cache line size     : %lld bytes\n", sysctl_int("hw.cachelinesize"));
    std::printf("L1 data cache (P)   : %lld KB\n", sysctl_int("hw.perflevel0.l1dcachesize") / 1024);
    std::printf("L2 cache (P)        : %lld MB\n", sysctl_int("hw.perflevel0.l2cachesize") / (1024 * 1024));
    std::printf("Page size           : %lld KB\n", sysctl_int("hw.pagesize") / 1024);

    // ---- Timer ----
    const uint64_t hz = ticks_per_second();
    const double ns_per_tick = 1e9 / static_cast<double>(hz);

    std::printf("\n== Timer ==\n");
    std::printf("Counter frequency   : %.1f MHz\n", static_cast<double>(hz) / 1e6);
    std::printf("Nominal resolution  : %.1f ns per tick (what the hardware claims)\n", ns_per_tick);

    // Read the timer a million times in a row and look at the gaps.
    // If most gaps are 0, one read is faster than one tick, which means
    // the clock is too coarse to time very short operations directly.
    constexpr int kReads = 1'000'000;
    uint64_t gap_zero = 0, gap_one = 0, gap_more = 0;
    uint64_t min_step = UINT64_MAX;
    const uint64_t start = read_ticks();
    uint64_t prev = start;
    for (int i = 0; i < kReads; ++i) {
        const uint64_t now = read_ticks();
        const uint64_t gap = now - prev;
        if (gap > 0 && gap < min_step) min_step = gap;
        if (gap == 0)      ++gap_zero;
        else if (gap == 1) ++gap_one;
        else               ++gap_more;
        prev = now;
    }
    const double read_cost_ns = static_cast<double>(prev - start) * ns_per_tick / kReads;
    std::printf("Observed step       : %llu ticks = %.1f ns (real resolution)\n",
                static_cast<unsigned long long>(min_step),
                static_cast<double>(min_step) * ns_per_tick);
    std::printf("Cost of one read    : %.1f ns\n", read_cost_ns);
    std::printf("Back-to-back gaps   : %.1f%% zero ticks, %.1f%% one tick, %.2f%% more\n",
                100.0 * gap_zero / kReads, 100.0 * gap_one / kReads, 100.0 * gap_more / kReads);

    // ---- Batch timing ----
    // Time the same workload at different sizes, 20 runs each.
    // Small batches will look jumpy (stuck to whole ticks); large ones
    // give stable per-iteration numbers. This tells us how big a batch
    // must be before we can trust a measurement.
    std::printf("\n== Batch timing of a known workload (20 runs each) ==\n");
    std::printf("%10s %10s %10s %10s %14s\n", "loop size", "min ns", "median ns", "max ns", "ns per iter");

    g_sink = dependent_adds(1'000'000);   // warm-up: wake the CPU up to full speed

    for (uint64_t n : {10ull, 100ull, 1'000ull, 10'000ull, 100'000ull}) {
        std::vector<double> samples;
        for (int run = 0; run < 20; ++run) {
            const uint64_t t0 = read_ticks();
            const uint64_t result = dependent_adds(n);
            const uint64_t t1 = read_ticks();
            g_sink = result;
            samples.push_back(static_cast<double>(t1 - t0) * ns_per_tick);
        }
        std::sort(samples.begin(), samples.end());
        const double median = samples[samples.size() / 2];
        std::printf("%10llu %10.0f %10.0f %10.0f %14.3f\n",
                    static_cast<unsigned long long>(n),
                    samples.front(), median, samples.back(),
                    median / static_cast<double>(n));
    }
    return 0;
}