// tests/fuzz_differential.cpp
//
// Feeds the same random operations to two books and, after EVERY
// operation, checks that:
//   1. both emitted exactly the same events,
//   2. both hold exactly the same orders,
//   3. both satisfy every invariant.
//
// Usage: ./build/fuzz_differential [first_seed] [num_seeds] [ops_per_seed]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>       // std::strtoull
#include <deque>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "invariants.hpp"
#include "lob/events.hpp"
#include "lob/snapshot.hpp"
#include "naive_book.hpp"
#include "random_ops.hpp"
#include <map>

using namespace lob;

// The two books under comparison. Both are the naive book for now, which
// checks determinism. When the fast book exists, change BookB.
using BookA = NaiveBook<RecordingListener>;
using BookB = NaiveBook<RecordingListener>;

constexpr Price kMin = 1000;
constexpr Price kMax = 1100;
constexpr std::size_t kHistory = 20;   // ops shown in a failure report

// ---------------------------------------------------------------------
// Applying and printing operations
// ---------------------------------------------------------------------

template <class Book>
void apply(Book& book, const Op& op) {
    switch (op.kind) {
        case OpKind::Add:    book.add(op.id, op.side, op.type, op.price, op.qty); break;
        case OpKind::Cancel: book.cancel(op.id); break;
        case OpKind::Modify: book.modify(op.id, op.price, op.qty); break;
    }
}

void print_op(std::FILE* f, const Op& op) {
    const auto id = static_cast<unsigned long long>(op.id);
    switch (op.kind) {
        case OpKind::Add:
            std::fprintf(f, "  ADD    id=%llu %s %s qty=%u @ %d\n", id,
                         to_string(op.side), to_string(op.type), op.qty, op.price);
            break;
        case OpKind::Cancel:
            std::fprintf(f, "  CANCEL id=%llu\n", id);
            break;
        case OpKind::Modify:
            std::fprintf(f, "  MODIFY id=%llu qty=%u @ %d\n", id, op.qty, op.price);
            break;
    }
}
// The book's real contents as id -> {price, qty}, to compare with what
// the events say it should contain.
std::map<OrderId, LiveOrder> orders_by_id(const BookSnapshot& s) {
    std::map<OrderId, LiveOrder> out;
    for (const LevelView& level : s.bids) {
        for (const OrderView& o : level.orders) out[o.id] = LiveOrder{level.price, o.qty};
    }
    for (const LevelView& level : s.asks) {
        for (const OrderView& o : level.orders) out[o.id] = LiveOrder{level.price, o.qty};
    }
    return out;
}
// ---------------------------------------------------------------------
// Coverage statistics: how often did each rule actually get exercised?
// ---------------------------------------------------------------------

struct Stats {
    uint64_t ops = 0, adds = 0, cancels = 0, modifies = 0;
    uint64_t accepted = 0, trades = 0, traded_qty = 0;
    uint64_t cancelled_user = 0, cancelled_unfilled = 0;
    uint64_t modified_kept = 0, modified_lost = 0;
    uint64_t rejected[6] = {};          // one counter per RejectReason
    std::size_t largest_book = 0;
};

void tally(Stats& s, const std::vector<Event>& events) {
    for (const Event& ev : events) {
        std::visit([&s](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, Accepted>) {
                ++s.accepted;
            } else if constexpr (std::is_same_v<T, Trade>) {
                ++s.trades;
                s.traded_qty += e.qty;
            } else if constexpr (std::is_same_v<T, Cancelled>) {
                if (e.reason == CancelReason::UserRequested) ++s.cancelled_user;
                else ++s.cancelled_unfilled;
            } else if constexpr (std::is_same_v<T, Rejected>) {
                ++s.rejected[static_cast<int>(e.reason)];
            } else if constexpr (std::is_same_v<T, Modified>) {
                if (e.kept_priority) ++s.modified_kept;
                else ++s.modified_lost;
            }
        }, ev);
    }
}

void print_stats(const Stats& s) {
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("\n== Coverage (all seeds) ==\n");
    std::printf("operations     : %llu (add %llu, cancel %llu, modify %llu)\n",
                u(s.ops), u(s.adds), u(s.cancels), u(s.modifies));
    std::printf("accepted       : %llu\n", u(s.accepted));
    std::printf("trades         : %llu (total qty %llu)\n", u(s.trades), u(s.traded_qty));
    std::printf("cancelled      : user %llu, unfilled %llu\n",
                u(s.cancelled_user), u(s.cancelled_unfilled));
    std::printf("modified       : kept priority %llu, lost priority %llu\n",
                u(s.modified_kept), u(s.modified_lost));
    std::printf("rejected       :");
    for (int i = 0; i < 6; ++i) {
        std::printf(" %s=%llu", to_string(static_cast<RejectReason>(i)), u(s.rejected[i]));
    }
    std::printf("\nlargest book   : %zu orders\n", s.largest_book);
}

// ---------------------------------------------------------------------
// One seed
// ---------------------------------------------------------------------

bool run_seed(uint64_t seed, uint64_t num_ops, Stats& stats) {
    // Every 4th seed uses a tiny book so BookFull gets tested.
    const std::size_t capacity = (seed % 4 == 0) ? 32 : 100'000;

    RecordingListener rec_a, rec_b;
    BookA a(rec_a, kMin, kMax, capacity);
    BookB b(rec_b, kMin, kMax, capacity);
    // Vary the market between seeds: heavy churn (small book),
    // medium, and steady build-up (deep book).
    const uint64_t add_percent = (seed % 3 == 0) ? 55 : (seed % 3 == 1) ? 70 : 85;
    OpGenerator gen(seed, kMin, kMax, add_percent);    std::deque<Op> history;

    for (uint64_t step = 0; step < num_ops; ++step) {
        const Op op = gen.next();
        history.push_back(op);
        if (history.size() > kHistory) history.pop_front();

        apply(a, op);
        apply(b, op);
        gen.observe(rec_a.events);   // the generator learns what happened

        // Check everything; stop at the first problem.
        const BookSnapshot snap_a = a.snapshot();
        std::string problem;
        if (rec_a.events != rec_b.events) {
            problem = "the two books emitted different events";
        } else if (snap_a != b.snapshot()) {
            problem = "the two books hold different orders";
        } else if (gen.live_orders() != orders_by_id(snap_a)) {
            problem = "the events don't add up to what the book holds";
        } else if (std::string e = check_invariants(a); !e.empty()) {
            problem = "book A: " + e;
        } else if (std::string e2 = check_invariants(b); !e2.empty()) {
            problem = "book B: " + e2;
        }

        if (!problem.empty()) {
            std::fprintf(stderr, "\nFAIL  seed=%llu step=%llu: %s\n",
                         static_cast<unsigned long long>(seed),
                         static_cast<unsigned long long>(step), problem.c_str());
            std::fprintf(stderr, "\nLast operations (oldest first; the last one caused it):\n");
            for (const Op& h : history) print_op(stderr, h);
            std::fprintf(stderr, "\nEvents from book A for the last operation:\n");
            for (const Event& e : rec_a.events) print_event(stderr, e);
            std::fprintf(stderr, "\nEvents from book B for the last operation:\n");
            for (const Event& e : rec_b.events) print_event(stderr, e);
            std::fprintf(stderr, "\nBook A after the last operation:\n");
            a.print(stderr);
            std::fprintf(stderr, "\nReproduce with: ./build/fuzz_differential %llu 1 %llu\n",
                         static_cast<unsigned long long>(seed),
                         static_cast<unsigned long long>(step + 1));
            return false;
        }

        // Record coverage, then clear the events for the next operation.
        ++stats.ops;
        if (op.kind == OpKind::Add) ++stats.adds;
        else if (op.kind == OpKind::Cancel) ++stats.cancels;
        else ++stats.modifies;
        tally(stats, rec_a.events);
        if (a.order_count() > stats.largest_book) stats.largest_book = a.order_count();
        rec_a.clear();
        rec_b.clear();
    }
    return true;
}

// ---------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------

int main(int argc, char** argv) {
    // argv holds the command-line words: argv[1] is the first argument, etc.
    const uint64_t first_seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
    const uint64_t num_seeds  = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 200;
    const uint64_t num_ops    = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 2000;

    std::printf("Fuzzing seeds %llu..%llu with %llu operations each\n",
                static_cast<unsigned long long>(first_seed),
                static_cast<unsigned long long>(first_seed + num_seeds - 1),
                static_cast<unsigned long long>(num_ops));

    Stats stats;
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t seed = first_seed; seed < first_seed + num_seeds; ++seed) {
        if (!run_seed(seed, num_ops, stats)) {
            return 1;   // non-zero exit code = failure (CTest checks this)
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    print_stats(stats);
    std::printf("\nPASS: %llu operations in %.1f s; identical outputs; all invariants held\n",
                static_cast<unsigned long long>(stats.ops), seconds);
    return 0;
}