// tests/test_order_pool.cpp
//
// Unit tests for OrderPool.

#include <cstdint>
#include <vector>

#include "doctest.h"
#include "lob/order_pool.hpp"
#include "random_ops.hpp"   // Rng

using namespace lob;

TEST_CASE("a new pool is empty and has the requested capacity") {
    OrderPool pool(8);
    CHECK(pool.capacity() == 8);
    CHECK(pool.size() == 0);
    CHECK_FALSE(pool.full());
}

TEST_CASE("allocate hands out 1..N in order, then reports full") {
    OrderPool pool(4);
    CHECK(pool.allocate() == 1);
    CHECK(pool.allocate() == 2);
    CHECK(pool.allocate() == 3);
    CHECK(pool.allocate() == 4);
    CHECK(pool.size() == 4);
    CHECK(pool.full());
    CHECK(pool.allocate() == kNullHandle);   // no slots left
    CHECK(pool.size() == 4);                 // a failed allocate changes nothing
}

TEST_CASE("released slots are reused most-recent-first (LIFO)") {
    OrderPool pool(4);
    const Handle a = pool.allocate();
    const Handle b = pool.allocate();
    const Handle c = pool.allocate();
    pool.release(a);
    pool.release(c);
    CHECK(pool.size() == 1);                 // only b is in use
    CHECK(pool.allocate() == c);             // released last, reused first
    CHECK(pool.allocate() == a);
    CHECK(pool.allocate() == 4);             // then the never-used slot
    CHECK(pool.allocate() == kNullHandle);
    CHECK(b == 2);
}

TEST_CASE("data written through a handle stays there") {
    OrderPool pool(2);
    const Handle h = pool.allocate();
    pool[h] = Order{.id = 42, .price = 2005, .qty = 100, .side = Side::Sell};
    const OrderPool& view = pool;            // read through the const version too
    CHECK(view[h].id == 42);
    CHECK(view[h].price == 2005);
    CHECK(view[h].qty == 100);
    CHECK(view[h].side == Side::Sell);
    // pool.release(h);
    // pool.release(h);   // bug: releasing the same slot twice
}

TEST_CASE("slots are 32-byte aligned and packed back to back") {
    OrderPool pool(16);
    for (Handle h = 1; h <= 16; ++h) {
        const auto address = reinterpret_cast<std::uintptr_t>(&pool[h]);
        CHECK(address % 32 == 0);
    }
    // Neighbouring slots are exactly one Order apart: no gaps, no headers.
    const auto first  = reinterpret_cast<std::uintptr_t>(&pool[1]);
    const auto second = reinterpret_cast<std::uintptr_t>(&pool[2]);
    CHECK(second - first == sizeof(Order));
}

TEST_CASE("a zero-capacity pool never hands out a slot") {
    OrderPool pool(0);
    CHECK(pool.full());
    CHECK(pool.allocate() == kNullHandle);
}

TEST_CASE("random allocate/release never hands out a slot twice") {
    constexpr std::size_t kCapacity = 1000;
    OrderPool pool(kCapacity);
    Rng rng(7);
    std::vector<Handle> live;                        // handles we hold
    std::vector<bool> taken(kCapacity + 1, false);   // our own record

    for (int step = 0; step < 100'000; ++step) {
        if (!live.empty() && rng.percent(50)) {
            // Release a random live handle (swap-remove from our list).
            const std::size_t i = rng.below(live.size());
            const Handle h = live[i];
            live[i] = live.back();
            live.pop_back();
            taken[h] = false;
            pool.release(h);
        } else {
            const Handle h = pool.allocate();
            if (live.size() == kCapacity) {
                REQUIRE(h == kNullHandle);           // must be full
            } else {
                REQUIRE(h != kNullHandle);
                REQUIRE(h <= kCapacity);
                REQUIRE_FALSE(taken[h]);             // never given out twice
                taken[h] = true;
                live.push_back(h);
            }
        }
        REQUIRE(pool.size() == live.size());
    }
}