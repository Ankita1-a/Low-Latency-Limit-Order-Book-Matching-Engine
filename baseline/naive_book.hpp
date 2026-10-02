// baseline/naive_book.hpp
//
// The reference order book: simple, obviously correct, and slow.
// Every optimized version must emit exactly the same events as this one.
// The rules are described in docs/DESIGN.md (see the comments below too).

#pragma once

#include <algorithm>      // std::min
#include <cstddef>        // std::size_t
#include <cstdint>
#include <cstdio>         // std::FILE, std::fprintf
#include <functional>     // std::greater
#include <iterator>       // std::prev
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

// A class template: NaiveBook<RecordingListener>, NaiveBook<NullListener>, ...
// The compiler builds a separate version for each listener type.
template <Listener L>
class NaiveBook {
public:
    // Constructor: runs when a NaiveBook is created.
    // The part after ':' (the "member initializer list") sets the fields.
    NaiveBook(L& listener, Price min_price, Price max_price, std::size_t max_orders)
        : out_(listener),
          min_price_(min_price),
          max_price_(max_price),
          max_orders_(max_orders) {}

    // ------------------------------------------------------------------
    // Commands
    // ------------------------------------------------------------------

    void add(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        // ---- 1. Validation (order of checks matters!) ----
        if (qty == 0) {
            out_.on_rejected({id, RejectReason::ZeroQuantity});
            return;
        }
        if (type != OrderType::Market && !in_band(price)) {
            out_.on_rejected({id, RejectReason::PriceOutOfBand});
            return;
        }
        if (index_.contains(id)) {
            out_.on_rejected({id, RejectReason::DuplicateOrderId});
            return;
        }
        if (type == OrderType::PostOnly && would_cross(side, price)) {
            out_.on_rejected({id, RejectReason::WouldCross});
            return;
        }
        if (can_rest(type) && index_.size() >= max_orders_) {
            out_.on_rejected({id, RejectReason::BookFull});
            return;
        }

        out_.on_accepted({id, side, type, price, qty});

        // ---- 2. Matching ----
        // A market order accepts any price: use the far edge of the band.
        // Every resting order is inside the band, so this matches everything.
        const Price limit = (type == OrderType::Market)
                                ? (side == Side::Buy ? max_price_ : min_price_)
                                : price;

        // FOK: all or nothing. Check before touching the book.
        if (type == OrderType::FOK && available(side, limit) < qty) {
            out_.on_cancelled({id, qty, CancelReason::Unfilled});
            return;
        }

        const Qty remaining = match(id, side, limit, qty);
        if (remaining == 0) {
            return;
        }

        // ---- 3. Leftover ----
        if (can_rest(type)) {
            rest(id, side, price, remaining);
        } else {
            out_.on_cancelled({id, remaining, CancelReason::Unfilled});
        }
    }

    void cancel(OrderId id) {
        auto found = index_.find(id);
        if (found == index_.end()) {
            out_.on_rejected({id, RejectReason::UnknownOrderId});
            return;
        }
        const Qty qty = found->second.it->qty;
        remove(found);
        out_.on_cancelled({id, qty, CancelReason::UserRequested});
    }

    void modify(OrderId id, Price new_price, Qty new_qty) {
        auto found = index_.find(id);
        if (found == index_.end()) {
            out_.on_rejected({id, RejectReason::UnknownOrderId});
            return;
        }
        if (new_qty == 0) {
            out_.on_rejected({id, RejectReason::ZeroQuantity});
            return;
        }
        if (!in_band(new_price)) {
            out_.on_rejected({id, RejectReason::PriceOutOfBand});
            return;
        }

        Locator& loc = found->second;

        // Same price, not bigger: change in place and keep queue position.
        if (new_price == loc.price && new_qty <= loc.it->qty) {
            loc.it->qty = new_qty;
            out_.on_modified({id, new_price, new_qty, true});
            return;
        }

        // Otherwise: remove, then re-enter as a new limit order.
        const Side side = loc.side;
        remove(found);   // after this, `found` and `loc` are invalid: don't use them
        out_.on_modified({id, new_price, new_qty, false});

        const Qty remaining = match(id, side, new_price, new_qty);
        if (remaining > 0) {
            rest(id, side, new_price, remaining);
        }
    }

    // ------------------------------------------------------------------
    // Queries (const = these functions don't change the book)
    // ------------------------------------------------------------------

    // std::optional: either holds a value, or is empty (no bids at all).
    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    // Total quantity resting at one price on one side.
    uint64_t volume_at(Side side, Price price) const {
        return side == Side::Buy ? level_volume(bids_, price)
                                 : level_volume(asks_, price);
    }

    // Order IDs at one price, front of queue first.
    std::vector<OrderId> queue_at(Side side, Price price) const {
        return side == Side::Buy ? level_ids(bids_, price)
                                 : level_ids(asks_, price);
    }

    std::size_t order_count() const { return index_.size(); }

    void print(std::FILE* f) const {
        std::fprintf(f, "  ASKS\n");
        if (asks_.empty()) std::fprintf(f, "    (empty)\n");
        // Asks printed highest first, so the best ask sits next to the bids.
        for (auto it = asks_.rbegin(); it != asks_.rend(); ++it) {
            print_level(f, it->first, it->second);
        }
        std::fprintf(f, "  BIDS\n");
        if (bids_.empty()) std::fprintf(f, "    (empty)\n");
        for (const auto& [price, queue] : bids_) {
            print_level(f, price, queue);
        }
    }

private:
    // ------------------------------------------------------------------
    // Internal types
    // ------------------------------------------------------------------

    struct RestingOrder {
        OrderId id;
        Qty     qty;
    };

    using Queue     = std::list<RestingOrder>;
    using BidLevels = std::map<Price, Queue, std::greater<Price>>;  // high -> low
    using AskLevels = std::map<Price, Queue>;                       // low -> high

    // Where an order lives, so cancel/modify can find it without searching.
    struct Locator {
        Side                     side;
        Price                    price;
        typename Queue::iterator it;   // bookmark into the price level's list
    };

    using Index = std::unordered_map<OrderId, Locator>;

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    bool in_band(Price p) const noexcept {
        return p >= min_price_ && p <= max_price_;
    }

    static bool can_rest(OrderType t) noexcept {
        return t == OrderType::Limit || t == OrderType::PostOnly;
    }

    bool would_cross(Side side, Price price) const {
        if (side == Side::Buy) {
            return !asks_.empty() && crosses(side, price, asks_.begin()->first);
        }
        return !bids_.empty() && crosses(side, price, bids_.begin()->first);
    }

    // How much could an incoming order on `side` with price `limit` fill?
    uint64_t available(Side side, Price limit) const {
        return side == Side::Buy ? available_in(asks_, side, limit)
                                 : available_in(bids_, side, limit);
    }

    // "template <class Levels>" lets one function work for both BidLevels
    // and AskLevels, which are different types because of their sorting.
    template <class Levels>
    static uint64_t available_in(const Levels& levels, Side side, Price limit) {
        uint64_t total = 0;
        for (const auto& [price, queue] : levels) {      // best price first
            if (!crosses(side, limit, price)) break;
            for (const RestingOrder& o : queue) total += o.qty;
        }
        return total;
    }

    // Match an incoming order against the opposite side.
    // Returns the quantity that did NOT trade.
    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        return side == Side::Buy ? match_against(asks_, taker, side, limit, qty)
                                 : match_against(bids_, taker, side, limit, qty);
    }

    template <class Levels>
    Qty match_against(Levels& levels, OrderId taker, Side side, Price limit, Qty remaining) {
        while (remaining > 0 && !levels.empty()) {
            auto level = levels.begin();                 // best opposite price
            const Price price = level->first;
            if (!crosses(side, limit, price)) break;     // no more acceptable prices

            Queue& queue = level->second;
            while (remaining > 0 && !queue.empty()) {
                RestingOrder& maker = queue.front();     // oldest order first
                const Qty fill = std::min(remaining, maker.qty);
                out_.on_trade({taker, maker.id, side, price, fill});
                remaining -= fill;
                maker.qty -= fill;
                if (maker.qty == 0) {                    // fully filled: remove it
                    index_.erase(maker.id);
                    queue.pop_front();
                }
            }
            if (queue.empty()) {
                levels.erase(level);                     // price level is now empty
            }
        }
        return remaining;
    }

    // Put an order at the back of its price level's queue.
    void rest(OrderId id, Side side, Price price, Qty qty) {
        // map[price] finds the level, or creates an empty one if missing.
        Queue& queue = (side == Side::Buy) ? bids_[price] : asks_[price];
        queue.push_back({id, qty});
        index_.emplace(id, Locator{side, price, std::prev(queue.end())});
    }

    // Remove a resting order (used by cancel and modify).
    void remove(typename Index::iterator found) {
        const Locator loc = found->second;
        if (loc.side == Side::Buy) {
            erase_from(bids_, loc);
        } else {
            erase_from(asks_, loc);
        }
        index_.erase(found);
    }

    template <class Levels>
    static void erase_from(Levels& levels, const Locator& loc) {
        auto level = levels.find(loc.price);
        level->second.erase(loc.it);
        if (level->second.empty()) {
            levels.erase(level);
        }
    }

    template <class Levels>
    static uint64_t level_volume(const Levels& levels, Price price) {
        auto level = levels.find(price);
        if (level == levels.end()) return 0;
        uint64_t total = 0;
        for (const RestingOrder& o : level->second) total += o.qty;
        return total;
    }

    template <class Levels>
    static std::vector<OrderId> level_ids(const Levels& levels, Price price) {
        std::vector<OrderId> ids;
        auto level = levels.find(price);
        if (level == levels.end()) return ids;
        for (const RestingOrder& o : level->second) ids.push_back(o.id);
        return ids;
    }

    static void print_level(std::FILE* f, Price price, const Queue& queue) {
        std::fprintf(f, "    %d |", price);
        for (const RestingOrder& o : queue) {
            std::fprintf(f, " #%llu:%u", static_cast<unsigned long long>(o.id), o.qty);
        }
        std::fprintf(f, "\n");
    }

    // ------------------------------------------------------------------
    // Data (trailing underscore = "this is a member variable")
    // ------------------------------------------------------------------

    L&          out_;          // where events go (a reference: we don't own it)
    Price       min_price_;
    Price       max_price_;
    std::size_t max_orders_;

    BidLevels bids_;
    AskLevels asks_;
    Index     index_;
};

}  // namespace lob