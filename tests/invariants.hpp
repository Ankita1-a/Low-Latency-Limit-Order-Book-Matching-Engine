// tests/invariants.hpp
//
// Properties that must hold after EVERY operation on ANY order book.
// Each check returns "" if all is well, or a description of the first
// problem found.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "lob/snapshot.hpp"
#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------
// Part 1: the structure of a snapshot on its own
// ---------------------------------------------------------------------
inline std::string check_snapshot(const BookSnapshot& s, Price min_price, Price max_price) {
    std::unordered_set<OrderId> seen;   // every order ID we've met so far

    // A lambda that checks one side. "descending" = bids (high to low).
    // [&] lets it use seen, min_price and max_price from outside.
    auto check_side = [&](const std::vector<LevelView>& levels, bool descending) -> std::string {
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const LevelView& level = levels[i];
            const std::string where = " at price " + std::to_string(level.price);

            if (level.price < min_price || level.price > max_price) {
                return "level outside the price band" + where;
            }
            if (level.orders.empty()) {
                return "empty level left in the book" + where;
            }
            if (i > 0) {
                const Price prev = levels[i - 1].price;
                const bool sorted = descending ? prev > level.price : prev < level.price;
                if (!sorted) return "levels out of order or duplicated" + where;
            }
            for (const OrderView& o : level.orders) {
                if (o.qty == 0) {
                    return "order " + std::to_string(o.id) + " has zero quantity" + where;
                }
                // insert() returns {position, inserted?}; .second is false
                // if the ID was already in the set.
                if (!seen.insert(o.id).second) {
                    return "order " + std::to_string(o.id) + " appears twice" + where;
                }
            }
        }
        return "";
    };

    // "if (init; condition)" (C++17): declare err and test it in one line.
    if (std::string err = check_side(s.bids, true); !err.empty())  return "bids: " + err;
    if (std::string err = check_side(s.asks, false); !err.empty()) return "asks: " + err;

    if (!s.bids.empty() && !s.asks.empty() && s.bids.front().price >= s.asks.front().price) {
        return "book is crossed: best bid " + std::to_string(s.bids.front().price) +
               " >= best ask " + std::to_string(s.asks.front().price);
    }
    return "";
}

// ---------------------------------------------------------------------
// Part 2: the book's query functions must agree with its snapshot
// ---------------------------------------------------------------------
// A template, so it works for any book type that has these functions.
template <class Book>
std::string check_invariants(const Book& book) {
    const BookSnapshot s = book.snapshot();

    if (std::string err = check_snapshot(s, book.min_price(), book.max_price()); !err.empty()) {
        return err;
    }

    // Order count and capacity
    std::size_t total = 0;
    for (const LevelView& level : s.bids) total += level.orders.size();
    for (const LevelView& level : s.asks) total += level.orders.size();
    if (book.order_count() != total) {
        return "order_count() says " + std::to_string(book.order_count()) +
               " but the book holds " + std::to_string(total);
    }
    if (book.order_count() > book.capacity()) {
        return "more orders than capacity";
    }

    // Best prices
    const std::optional<Price> bid =
        s.bids.empty() ? std::nullopt : std::optional<Price>(s.bids.front().price);
    const std::optional<Price> ask =
        s.asks.empty() ? std::nullopt : std::optional<Price>(s.asks.front().price);
    if (book.best_bid() != bid) return "best_bid() disagrees with the book contents";
    if (book.best_ask() != ask) return "best_ask() disagrees with the book contents";

    // Per-level queries
    auto check_levels = [&](Side side, const std::vector<LevelView>& levels) -> std::string {
        for (const LevelView& level : levels) {
            uint64_t volume = 0;
            std::vector<OrderId> ids;
            for (const OrderView& o : level.orders) {
                volume += o.qty;
                ids.push_back(o.id);
            }
            const std::string where = " at price " + std::to_string(level.price);
            if (book.volume_at(side, level.price) != volume) return "volume_at() wrong" + where;
            if (book.queue_at(side, level.price) != ids)     return "queue_at() wrong" + where;
        }
        return "";
    };
    if (std::string err = check_levels(Side::Buy, s.bids); !err.empty())  return "bids: " + err;
    if (std::string err = check_levels(Side::Sell, s.asks); !err.empty()) return "asks: " + err;

    return "";
}

}  // namespace lob