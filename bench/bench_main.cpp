// bench/bench_main.cpp
//
// Measures order book speed on a realistic pre-generated workload.
// Reports throughput per run and the distribution of batch latencies.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "lob/events.hpp"
#include "naive_book.hpp"
#include "timer.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;

constexpr std::size_t kBatch    = 256;          // ops per timed batch (timer step ~42 ns)
constexpr int         kRuns     = 5;
constexpr std::size_t kCapacity = 1'000'000;    // plenty of room for every book

template <class Book>
void run_benchmark(const char* name, const Workload& w, double tpn) {
    std::printf("\n== %s: %d runs x %zu ops ==\n", name, kRuns, w.ops.size());
    std::vector<double> all_batches;   // ns per op, one sample per batch

    for (int run = 1; run <= kRuns; ++run) {
        CountingListener counter;
        Book book(counter, w.min_price, w.max_price, kCapacity);
        for (const Op& op : w.prefill) apply(book, op);     // starting book, not timed

        std::vector<double> batches;
        batches.reserve(w.ops.size() / kBatch + 1);         // allocate before timing

        const uint64_t start = read_ticks();
        for (std::size_t i = 0; i < w.ops.size(); i += kBatch) {
            const std::size_t end = std::min(i + kBatch, w.ops.size());
            const uint64_t t0 = read_ticks();
            for (std::size_t j = i; j < end; ++j) {
                apply(book, w.ops[j]);
            }
            const uint64_t t1 = read_ticks();
            batches.push_back(static_cast<double>(t1 - t0) / tpn /
                              static_cast<double>(end - i));
        }
        const uint64_t stop = read_ticks();

        const double total_ns = static_cast<double>(stop - start) / tpn;
        const double ops = static_cast<double>(w.ops.size());
        // Printing the counters keeps the work "visible", so the
        // compiler can't optimize the benchmark away.
        std::printf("run %d: %7.1f ms | %6.2f M ops/s | %6.1f ns/op | trades=%llu resting=%zu\n",
                    run, total_ns / 1e6, ops / total_ns * 1e3, total_ns / ops,
                    static_cast<unsigned long long>(counter.trades), book.order_count());

        all_batches.insert(all_batches.end(), batches.begin(), batches.end());
    }

    const Summary s = summarize(all_batches);
    std::printf("\nns per op, averaged over %zu-op batches (%zu batches, all runs):\n",
                kBatch, all_batches.size());
    std::printf("  mean %.1f | p50 %.1f | p90 %.1f | p99 %.1f | p99.9 %.1f | max %.1f\n",
                s.mean, s.p50, s.p90, s.p99, s.p999, s.max);
}

int main() {
    prefer_performance_core();

#if !defined(__OPTIMIZE__)
    std::printf("WARNING: this is not an optimized build. Numbers are meaningless.\n");
#endif

    std::printf("== Environment ==\n");
    std::printf("compiler : %s\n", __VERSION__);
    const double tpn = ticks_per_ns();
    std::printf("timer    : %.3f ticks per ns (calibrated)\n", tpn);
    std::printf("batch    : %zu ops per timed sample\n", kBatch);

    std::printf("\nGenerating workload (not timed)...\n");
    const WorkloadConfig cfg;
    const Workload w = make_workload(cfg);

    const double n = static_cast<double>(w.ops.size());
    std::printf("== Workload ==\n");
    std::printf("prefill  : %zu orders\n", w.prefill.size());
    std::printf("measured : %zu ops | add %.1f%% | cancel %.1f%% | modify %.1f%% | aggressive IOC %.1f%%\n",
                w.ops.size(),
                100.0 * static_cast<double>(w.adds) / n,
                100.0 * static_cast<double>(w.cancels) / n,
                100.0 * static_cast<double>(w.modifies) / n,
                100.0 * static_cast<double>(w.aggressive) / n);
    std::printf("book     : min %zu | avg %.0f | max %zu resting orders during measured ops\n",
                w.book_min, w.book_avg, w.book_max);

    run_benchmark<NaiveBook<CountingListener>>("naive book (std::map + std::list)", w, tpn);
    return 0;
}