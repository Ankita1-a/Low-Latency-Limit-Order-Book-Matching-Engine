// tests/check_types.cpp
//
// Quick sanity check for types.hpp: prints sizes and tries the helpers.
// (Proper unit tests come later; this just confirms the header works.)

#include <cstdio>
#include "lob/types.hpp"

int main() {
    using namespace lob;   // lets us write Order instead of lob::Order here

    std::printf("== Sizes ==\n");
    std::printf("sizeof(Order) = %zu bytes, alignof = %zu\n", sizeof(Order), alignof(Order));
    std::printf("sizeof(Level) = %zu bytes\n", sizeof(Level));
    std::printf("Cache line    = %zu bytes\n", kCacheLineSize);
    std::printf("Orders per cache line = %zu\n", kCacheLineSize / sizeof(Order));
    std::printf("Levels per cache line = %zu\n", kCacheLineSize / sizeof(Level));

    // Price as ticks: Rs 100.25 with tick size Rs 0.05
    const int paise = 10025, tick_paise = 5;
    const Price p = paise / tick_paise;
    std::printf("\n== Prices ==\nRs 100.25 -> %d ticks\n", p);

    // C++20 designated initializers: name the fields you set.
    // Unnamed fields (prev, next) keep their default values.
    Order o{.id = 42, .price = 2005, .qty = 100, .side = Side::Buy};
    std::printf("\n== Order ==\nid=%llu price=%d qty=%u side=%s prev=%u next=%u\n",
                static_cast<unsigned long long>(o.id), o.price, o.qty,
                o.side == Side::Buy ? "Buy" : "Sell", o.prev, o.next);

    std::printf("\n== Helpers ==\n");
    std::printf("opposite(Buy) is Sell : %s\n", opposite(Side::Buy) == Side::Sell ? "yes" : "NO");
    std::printf("Buy @2005 vs ask 2004 : %s\n", crosses(Side::Buy, 2005, 2004) ? "trades" : "no trade");
    std::printf("Buy @2005 vs ask 2006 : %s\n", crosses(Side::Buy, 2005, 2006) ? "trades" : "no trade");
    std::printf("Sell @2005 vs bid 2005: %s\n", crosses(Side::Sell, 2005, 2005) ? "trades" : "no trade");
    std::printf("Sell @2005 vs bid 2004: %s\n", crosses(Side::Sell, 2005, 2004) ? "trades" : "no trade");
    return 0;
}