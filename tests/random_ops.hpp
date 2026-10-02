// tests/random_ops.hpp
//
// Generates a reproducible stream of random order book operations.
// The same seed always produces the same stream, on every machine.
// The generator watches the book's events, so it knows which orders are
// live and can aim cancels and modifies at them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>      // std::next
#include <map>
#include <type_traits>
#include <variant>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------
// One operation on the book
// ---------------------------------------------------------------------

enum class OpKind : uint8_t { Add, Cancel, Modify };

struct Op {
    OpKind    kind  = OpKind::Add;
    OrderId   id    = 0;
    Side      side  = Side::Buy;          // used by Add
    OrderType type  = OrderType::Limit;   // used by Add
    Price     price = 0;                  // used by Add and Modify
    Qty       qty   = 0;                  // used by Add and Modify
};

// ---------------------------------------------------------------------
// Random number generator: splitmix64 (same results on every platform)
// ---------------------------------------------------------------------

class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed) {}

    uint64_t next() noexcept {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    uint64_t below(uint64_t n) noexcept { return next() % n; }   // 0 .. n-1

    int64_t between(int64_t lo, int64_t hi) noexcept {           // lo .. hi
        return lo + static_cast<int64_t>(below(static_cast<uint64_t>(hi - lo + 1)));
    }

    bool percent(uint64_t p) noexcept { return below(100) < p; }

private:
    uint64_t state_;
};

// ---------------------------------------------------------------------
// What the generator believes is resting in the book
// ---------------------------------------------------------------------

struct LiveOrder {
    Price price;
    Qty   qty;
    bool operator==(const LiveOrder&) const = default;
};

// ---------------------------------------------------------------------
// Operation generator
// ---------------------------------------------------------------------

class OpGenerator {
public:
    // add_percent: share of operations that are adds. The rest is split
    // evenly between cancels and modifies.
    OpGenerator(uint64_t seed, Price min_price, Price max_price, uint64_t add_percent = 55)
        : rng_(seed),
          min_(min_price),
          max_(max_price),
          mid_((min_price + max_price) / 2),
          add_percent_(add_percent) {}
    Op next() {
        drift_mid();
        const uint64_t roll = rng_.below(100);
        const uint64_t cancel_end = add_percent_ + (100 - add_percent_) / 2;
        if (recent_.empty() || roll < add_percent_) return make_add();
        if (roll < cancel_end) return make_cancel();
        return make_modify();
    }

    // Learn from the last operation's events which orders are resting
    // and how much each has left.
    void observe(const std::vector<Event>& events) {
        for (const Event& ev : events) {
            std::visit([this](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, Accepted>) {
                    // Only limit and post-only orders can end up resting.
                    if (e.type == OrderType::Limit || e.type == OrderType::PostOnly) {
                        live_[e.id] = LiveOrder{e.price, e.qty};
                    }
                } else if constexpr (std::is_same_v<T, Trade>) {
                    reduce(e.maker_id, e.qty);
                    reduce(e.taker_id, e.qty);   // a limit taker may rest its leftover
                } else if constexpr (std::is_same_v<T, Cancelled>) {
                    live_.erase(e.id);
                } else if constexpr (std::is_same_v<T, Modified>) {
                    live_[e.id] = LiveOrder{e.price, e.qty};   // trades after it reduce it
                }
                // Rejected: nothing changed in the book.
            }, ev);
        }
    }

    // std::map (sorted), not unordered_map: its order is identical on
    // every platform, so random picks from it stay reproducible.
    const std::map<OrderId, LiveOrder>& live_orders() const { return live_; }

private:
    static constexpr std::size_t kRecentWindow = 512;

    void reduce(OrderId id, Qty qty) {
        auto it = live_.find(id);
        if (it == live_.end()) return;           // e.g. an IOC taker: never live
        it->second.qty -= qty;
        if (it->second.qty == 0) live_.erase(it);
    }

    void drift_mid() {
        if (!rng_.percent(10)) return;
        mid_ += static_cast<Price>(rng_.between(-3, 3));
        if (mid_ < min_ + 10) mid_ = min_ + 10;
        if (mid_ > max_ - 10) mid_ = max_ - 10;
    }

    Price clamp(Price p) const { return p < min_ ? min_ : (p > max_ ? max_ : p); }

    Price near_mid() { return mid_ + static_cast<Price>(rng_.between(-8, 8)); }

    // Away from the other side, so the order rests instead of trading.
    Price passive_price(Side side) {
        const Price gap = static_cast<Price>(rng_.between(1, 15));
        return clamp(side == Side::Buy ? mid_ - gap : mid_ + gap);
    }

    Price edge_price() {
        switch (rng_.below(5)) {
            case 0:  return min_ - 1;
            case 1:  return min_;
            case 2:  return max_;
            case 3:  return max_ + 1;
            default: return static_cast<Price>(rng_.between(min_, max_));
        }
    }

    Price pick_price() { return rng_.percent(3) ? edge_price() : near_mid(); }

    Qty pick_qty() {
        if (rng_.percent(2)) return 0;
        if (rng_.percent(70)) return static_cast<Qty>(rng_.between(1, 50));
        return static_cast<Qty>(rng_.between(51, 500));
    }

    OrderType pick_type() {
        const uint64_t roll = rng_.below(100);
        if (roll < 60) return OrderType::Limit;
        if (roll < 75) return OrderType::PostOnly;
        if (roll < 85) return OrderType::IOC;
        if (roll < 95) return OrderType::FOK;
        return OrderType::Market;
    }

    // 80%: a live order. Otherwise half never-existed, half probably-dead.
    OrderId pick_id() {
        if (!live_.empty() && rng_.percent(80)) {
            const auto steps = static_cast<std::ptrdiff_t>(rng_.below(live_.size()));
            return std::next(live_.begin(), steps)->first;
        }
        if (rng_.percent(50)) return next_id_ + 1'000'000;
        return recent_[rng_.below(recent_.size())];
    }

    void remember(OrderId id) {
        recent_.push_back(id);
        if (recent_.size() > kRecentWindow) recent_.pop_front();
    }

    Op make_add() {
        Op op;
        op.kind = OpKind::Add;
        const bool reuse = !recent_.empty() && rng_.percent(3);   // duplicate ID test
        op.id   = reuse ? recent_[rng_.below(recent_.size())] : next_id_++;
        op.side = rng_.percent(50) ? Side::Buy : Side::Sell;
        op.type = pick_type();
        if (op.type == OrderType::Market) {
            op.price = 0;
        } else if (rng_.percent(50)) {
            op.price = passive_price(op.side);   // builds up the book
        } else {
            op.price = pick_price();             // may cross and trade
        }
        op.qty = pick_qty();
        if (!reuse) remember(op.id);
        return op;
    }

    Op make_cancel() {
        Op op;
        op.kind = OpKind::Cancel;
        op.id   = pick_id();
        return op;
    }

    Op make_modify() {
        Op op;
        op.kind = OpKind::Modify;
        op.id   = pick_id();
        auto live = live_.find(op.id);
        if (live != live_.end() && rng_.percent(40)) {
            // Same price, usually smaller: the "keep priority" path.
            op.price = live->second.price;
            op.qty = rng_.percent(80)
                         ? static_cast<Qty>(rng_.between(1, live->second.qty))
                         : pick_qty();
        } else {
            op.price = pick_price();
            op.qty   = pick_qty();
        }
        return op;
    }

    Rng     rng_;
    Price   min_;
    Price   max_;
    Price   mid_;
    uint64_t add_percent_;
    OrderId next_id_ = 1;
    std::deque<OrderId> recent_;             // recently issued IDs
    std::map<OrderId, LiveOrder> live_;      // orders we believe are resting
};

}  // namespace lob