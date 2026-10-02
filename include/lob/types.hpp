// include/lob/types.hpp
//
// Basic types shared by every part of the order book.

#pragma once   // include this file only once per compilation, even if
               // several other files #include it

#include <cstdint>   // int32_t, uint32_t, uint64_t, uint8_t

namespace lob {      // everything in our project lives in namespace "lob",
                     // so our names never clash with other libraries

// ---------------------------------------------------------------------
// Scalar types
// ---------------------------------------------------------------------

using Price   = int32_t;   // price in ticks (e.g. Rs 100.25 with tick 0.05 = 2005)
using Qty     = uint32_t;  // number of shares/contracts; never negative
using OrderId = uint64_t;  // ID chosen by the client or exchange
using Handle  = uint32_t;  // our own internal index into the order pool

// Handle 0 is reserved to mean "no order", like nullptr for pointers.
// The order pool will never hand out slot 0.
inline constexpr Handle kNullHandle = 0;

// Cache line size: the chunk of memory the CPU moves at once.
// Apple Silicon uses 128 bytes; most x86 CPUs use 64.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLineSize = 128;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

// ---------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------

// "enum class" = a type with a fixed set of named values.
// ": uint8_t" makes it take exactly 1 byte.
enum class Side : uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : uint8_t {
    Limit,     // rest in the book if not fully filled
    Market,    // trade at any price, never rest
    IOC,       // Immediate-Or-Cancel: fill what you can now, cancel the rest
    FOK,       // Fill-Or-Kill: fill completely right now, or do nothing
    PostOnly,  // only rest in the book; reject if it would trade immediately
};

// The other side of the book: a buy trades against sells and vice versa.
constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Can an incoming order at price `incoming` trade with a resting order
// at price `resting`?
//   Buyer pays up to `incoming`, so it trades if incoming >= resting ask.
//   Seller accepts down to `incoming`, so it trades if incoming <= resting bid.
constexpr bool crosses(Side incoming_side, Price incoming, Price resting) noexcept {
    return incoming_side == Side::Buy ? incoming >= resting
                                      : incoming <= resting;
}

// ---------------------------------------------------------------------
// Order: one resting order in the book
// ---------------------------------------------------------------------
//
// Orders at the same price wait in a queue (first come, first served).
// Each order stores the handles of its neighbours in that queue
// (prev/next). This is called an "intrusive linked list": the links live
// inside the object itself, so removing an order (a cancel) takes
// constant time without searching the queue.
//
// Fields are ordered largest-first to avoid wasted padding bytes.
// alignas(32) makes every Order start at a multiple of 32 bytes, so an
// order never straddles two cache lines (one order = one memory fetch).

struct alignas(32) Order {
    OrderId id    = 0;              // 8 bytes  (offset 0)
    Price   price = 0;              // 4 bytes  (offset 8)
    Qty     qty   = 0;              // 4 bytes  (offset 12)  remaining quantity
    Handle  prev  = kNullHandle;    // 4 bytes  (offset 16)  older order at same price
    Handle  next  = kNullHandle;    // 4 bytes  (offset 20)  newer order at same price
    Side    side  = Side::Buy;      // 1 byte   (offset 24)
                                    // 7 bytes padding -> total 32
};

// ---------------------------------------------------------------------
// Level: the queue of orders at one price
// ---------------------------------------------------------------------
//
// Kept small (16 bytes) so many levels fit in one cache line: the
// matching engine walks neighbouring price levels a lot.

struct Level {
    Handle   head      = kNullHandle;  // oldest order (matched first)
    Handle   tail      = kNullHandle;  // newest order (new orders go here)
    uint32_t count     = 0;            // number of orders at this price
    Qty      total_qty = 0;            // sum of their quantities
};

// ---------------------------------------------------------------------
// Compile-time checks
// ---------------------------------------------------------------------
// static_assert is checked by the compiler. If someone later changes a
// field and the size grows, the build fails immediately instead of the
// code silently getting slower.

static_assert(sizeof(Order) == 32,  "Order should be exactly 32 bytes");
static_assert(alignof(Order) == 32, "Order should be 32-byte aligned");
static_assert(kCacheLineSize % sizeof(Order) == 0,
              "Orders must tile cache lines exactly");
static_assert(sizeof(Level) == 16,  "Level should be exactly 16 bytes");

}  // namespace lob