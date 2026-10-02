// include/lob/snapshot.hpp
//
// A plain copy of an entire order book in a common format.
// Every book type can produce one, so tests can inspect and compare books
// without knowing how they store data internally.
// Slow (copies everything): for tests and debugging only.

#pragma once

#include <vector>

#include "lob/types.hpp"

namespace lob {

struct OrderView {
    OrderId id;
    Qty     qty;
    bool operator==(const OrderView&) const = default;
};

struct LevelView {
    Price                  price;
    std::vector<OrderView> orders;   // front of the queue first
    bool operator==(const LevelView&) const = default;
};

struct BookSnapshot {
    std::vector<LevelView> bids;     // best (highest) price first
    std::vector<LevelView> asks;     // best (lowest) price first
    bool operator==(const BookSnapshot&) const = default;
};

}  // namespace lob