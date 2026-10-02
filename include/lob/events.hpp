// include/lob/events.hpp
//
// Messages the order book emits, and the "listeners" that receive them.

#pragma once

#include <cstdint>
#include <cstdio>        // std::FILE, std::fprintf
#include <type_traits>   // std::is_same_v, std::decay_t
#include <variant>       // std::variant, std::visit
#include <vector>

#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------
// Reasons
// ---------------------------------------------------------------------

enum class RejectReason : uint8_t {
    ZeroQuantity,      // order for 0 shares
    PriceOutOfBand,    // price outside the allowed range
    DuplicateOrderId,  // an order with this ID is already in the book
    UnknownOrderId,    // cancel/modify for an order that isn't in the book
    WouldCross,        // post-only order that would trade immediately
    BookFull,          // no free slots left in the order pool
};

enum class CancelReason : uint8_t {
    UserRequested,     // the client sent a cancel
    Unfilled,          // IOC / Market / FOK leftover that can't rest
};

// ---------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------
// "= default" on operator== asks the compiler to write the comparison
// for us: two events are equal if every field is equal (C++20).
// Tests rely on this to compare the outputs of two order books.

// The order passed validation and entered the engine.
struct Accepted {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;
    Qty       qty;
    bool operator==(const Accepted&) const = default;
};

// A trade between an incoming (taker) order and a resting (maker) order.
// price is always the maker's price.
struct Trade {
    OrderId taker_id;
    OrderId maker_id;
    Side    taker_side;
    Price   price;
    Qty     qty;
    bool operator==(const Trade&) const = default;
};

// Quantity removed from the book or from an incoming order.
struct Cancelled {
    OrderId      id;
    Qty          qty;      // how much was cancelled
    CancelReason reason;
    bool operator==(const Cancelled&) const = default;
};

// The request was refused; nothing changed in the book.
struct Rejected {
    OrderId      id;
    RejectReason reason;
    bool operator==(const Rejected&) const = default;
};

// An existing order's price and/or quantity changed.
// kept_priority = true only when quantity was reduced at the same price.
struct Modified {
    OrderId id;
    Price   price;
    Qty     qty;
    bool    kept_priority;
    bool operator==(const Modified&) const = default;
};

// A variant holds exactly ONE of the listed types at a time, like a box
// labelled with which kind of event is inside. Used only for recording.
using Event = std::variant<Accepted, Trade, Cancelled, Rejected, Modified>;

// ---------------------------------------------------------------------
// The Listener "concept" (C++20)
// ---------------------------------------------------------------------
// A concept is a compile-time checklist. Any type that has these five
// functions counts as a Listener. If you pass something that doesn't,
// the compiler gives a clear error naming the missing function.

template <class L>
concept Listener = requires(L& l, const Accepted& a, const Trade& t,
                            const Cancelled& c, const Rejected& r,
                            const Modified& m) {
    l.on_accepted(a);
    l.on_trade(t);
    l.on_cancelled(c);
    l.on_rejected(r);
    l.on_modified(m);
};

// ---------------------------------------------------------------------
// Listeners
// ---------------------------------------------------------------------

// Does nothing. With optimizations on, calls to it vanish entirely.
struct NullListener {
    void on_accepted(const Accepted&) noexcept {}
    void on_trade(const Trade&) noexcept {}
    void on_cancelled(const Cancelled&) noexcept {}
    void on_rejected(const Rejected&) noexcept {}
    void on_modified(const Modified&) noexcept {}
};

// Counts events. Cheap, and proves to the compiler that work matters.
struct CountingListener {
    uint64_t accepted = 0, trades = 0, traded_qty = 0;
    uint64_t cancelled = 0, rejected = 0, modified = 0;

    void on_accepted(const Accepted&) noexcept { ++accepted; }
    void on_trade(const Trade& t) noexcept { ++trades; traded_qty += t.qty; }
    void on_cancelled(const Cancelled&) noexcept { ++cancelled; }
    void on_rejected(const Rejected&) noexcept { ++rejected; }
    void on_modified(const Modified&) noexcept { ++modified; }
};

// Stores every event. push_back can allocate memory, so this listener is
// for tests only, never for benchmarks.
struct RecordingListener {
    std::vector<Event> events;

    void on_accepted(const Accepted& e) { events.push_back(e); }
    void on_trade(const Trade& e) { events.push_back(e); }
    void on_cancelled(const Cancelled& e) { events.push_back(e); }
    void on_rejected(const Rejected& e) { events.push_back(e); }
    void on_modified(const Modified& e) { events.push_back(e); }

    void clear() { events.clear(); }
};

static_assert(Listener<NullListener>);
static_assert(Listener<CountingListener>);
static_assert(Listener<RecordingListener>);

// ---------------------------------------------------------------------
// Printing (for tests and debugging; not performance critical)
// ---------------------------------------------------------------------

inline const char* to_string(Side s) noexcept {
    return s == Side::Buy ? "Buy" : "Sell";
}

inline const char* to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit:    return "Limit";
        case OrderType::Market:   return "Market";
        case OrderType::IOC:      return "IOC";
        case OrderType::FOK:      return "FOK";
        case OrderType::PostOnly: return "PostOnly";
    }
    return "?";
}

inline const char* to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::ZeroQuantity:     return "ZeroQuantity";
        case RejectReason::PriceOutOfBand:   return "PriceOutOfBand";
        case RejectReason::DuplicateOrderId: return "DuplicateOrderId";
        case RejectReason::UnknownOrderId:   return "UnknownOrderId";
        case RejectReason::WouldCross:       return "WouldCross";
        case RejectReason::BookFull:         return "BookFull";
    }
    return "?";
}

inline const char* to_string(CancelReason r) noexcept {
    switch (r) {
        case CancelReason::UserRequested: return "UserRequested";
        case CancelReason::Unfilled:      return "Unfilled";
    }
    return "?";
}

// std::visit opens the variant box and calls the lambda with whatever
// event is inside. "if constexpr" picks the right branch at compile time.
inline void print_event(std::FILE* out, const Event& ev) {
    std::visit([out](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, Accepted>) {
            std::fprintf(out, "ACCEPTED   id=%llu %s %s qty=%u @ %d\n",
                         static_cast<unsigned long long>(e.id),
                         to_string(e.side), to_string(e.type), e.qty, e.price);
        } else if constexpr (std::is_same_v<T, Trade>) {
            std::fprintf(out, "TRADE      taker=%llu (%s) maker=%llu qty=%u @ %d\n",
                         static_cast<unsigned long long>(e.taker_id),
                         to_string(e.taker_side),
                         static_cast<unsigned long long>(e.maker_id), e.qty, e.price);
        } else if constexpr (std::is_same_v<T, Cancelled>) {
            std::fprintf(out, "CANCELLED  id=%llu qty=%u reason=%s\n",
                         static_cast<unsigned long long>(e.id), e.qty, to_string(e.reason));
        } else if constexpr (std::is_same_v<T, Rejected>) {
            std::fprintf(out, "REJECTED   id=%llu reason=%s\n",
                         static_cast<unsigned long long>(e.id), to_string(e.reason));
        } else if constexpr (std::is_same_v<T, Modified>) {
            std::fprintf(out, "MODIFIED   id=%llu qty=%u @ %d priority=%s\n",
                         static_cast<unsigned long long>(e.id), e.qty, e.price,
                         e.kept_priority ? "kept" : "lost");
        }
    }, ev);
}

}  // namespace lob