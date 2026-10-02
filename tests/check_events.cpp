// tests/check_events.cpp
//
// Emits a hand-written event sequence into each listener and prints the
// results. The real engine will produce exactly this kind of stream.

#include <cstdio>
#include "lob/events.hpp"

using namespace lob;

// "template <Listener L>" means: works with ANY type L that satisfies the
// Listener concept. The compiler makes a separate, fully inlined copy for
// each listener type we use. The order book will work the same way.
template <Listener L>
void scenario(L& out) {
    // 1. Order 1: SELL 100 @ 2005 (limit). Nothing to trade with, so it rests.
    out.on_accepted({.id = 1, .side = Side::Sell, .type = OrderType::Limit,
                     .price = 2005, .qty = 100});

    // 2. Order 2: BUY 150 @ 2006 (IOC). It trades 100 at 2005, the resting
    //    seller's price. The other 50 can't rest (IOC), so it is cancelled.
    out.on_accepted({.id = 2, .side = Side::Buy, .type = OrderType::IOC,
                     .price = 2006, .qty = 150});
    out.on_trade({.taker_id = 2, .maker_id = 1, .taker_side = Side::Buy,
                  .price = 2005, .qty = 100});
    out.on_cancelled({.id = 2, .qty = 50, .reason = CancelReason::Unfilled});

    // 3. A cancel request for order 99, which doesn't exist.
    out.on_rejected({.id = 99, .reason = RejectReason::UnknownOrderId});
}

int main() {
    // Recording: keeps everything, so we can print it.
    RecordingListener rec;
    scenario(rec);
    std::printf("== RecordingListener: %zu events ==\n", rec.events.size());
    for (const Event& e : rec.events) {
        print_event(stdout, e);
    }

    // Counting: just tallies.
    CountingListener cnt;
    scenario(cnt);
    std::printf("\n== CountingListener ==\n");
    std::printf("accepted=%llu trades=%llu traded_qty=%llu cancelled=%llu rejected=%llu\n",
                static_cast<unsigned long long>(cnt.accepted),
                static_cast<unsigned long long>(cnt.trades),
                static_cast<unsigned long long>(cnt.traded_qty),
                static_cast<unsigned long long>(cnt.cancelled),
                static_cast<unsigned long long>(cnt.rejected));

    // Null: does nothing, by design.
    NullListener null;
    scenario(null);
    std::printf("\n== NullListener ==\nran the scenario, kept nothing\n");

    // Comparing two recordings: the heart of differential testing.
    // Later: record the simple book and the fast book on the same input
    // and require the event lists to be identical.
    RecordingListener a, b;
    scenario(a);
    scenario(b);
    std::printf("\n== Comparison ==\n");
    std::printf("same scenario twice    -> equal: %s\n", a.events == b.events ? "yes" : "NO");
    b.events.pop_back();
    std::printf("after dropping 1 event -> equal: %s\n", a.events == b.events ? "yes" : "NO");
    return 0;
}