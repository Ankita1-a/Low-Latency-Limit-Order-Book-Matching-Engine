// bench/workload.hpp
//
// Builds a realistic, reproducible stream of order book operations,
// generated in advance so generation cost is never measured.

#pragma once

#include <algorithm>     // std::min, std::max
#include <cstddef>
#include <cstdint>
#include <limits>        // std::numeric_limits
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

#include "lob/events.hpp"
#include "naive_book.hpp"
#include "random_ops.hpp"   // Op, OpKind, Rng (shared with the fuzzer)

namespace lob::bench {

template <class Book>
void apply(Book& book, const Op& op) {
    switch (op.kind) {
        case OpKind::Add:    book.add(op.id, op.side, op.type, op.price, op.qty); break;
        case OpKind::Cancel: book.cancel(op.id); break;
        case OpKind::Modify: book.modify(op.id, op.price, op.qty); break;
    }
}

struct WorkloadConfig {
    uint64_t    seed          = 42;
    Price       mid           = 100'000;     // starting mid price, in ticks
    Price       band          = 5'000;       // allowed prices: mid +/- band
    std::size_t target_orders = 10'000;      // book size to hover around
    std::size_t num_ops       = 1'000'000;   // measured operations
};

struct Workload {
    Price min_price = 0, max_price = 0;
    std::vector<Op> prefill;   // builds the starting book (not timed)
    std::vector<Op> ops;       // the measured operations
    uint64_t adds = 0, aggressive = 0, cancels = 0, modifies = 0;

    // Book size during the measured ops: a sanity check on the workload.
    std::size_t book_min = std::numeric_limits<std::size_t>::max();
    std::size_t book_max = 0;
    double      book_avg = 0;
};

// ---------------------------------------------------------------------
// Tracks resting orders (learned from events) and picks random ones fast.
// ids_ is a plain array for O(1) random picks; map_ finds an order's slot.
// Removal swaps the last ID into the hole ("swap-remove"), also O(1).
// ---------------------------------------------------------------------
class LiveOrders {
public:
    struct Entry {
        std::size_t pos;     // index in ids_
        Price       price;
        Qty         qty;
    };

    void observe(const std::vector<Event>& events) {
        for (const Event& ev : events) {
            std::visit([this](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, Accepted>) {
                    if (e.type == OrderType::Limit || e.type == OrderType::PostOnly) {
                        set(e.id, e.price, e.qty);
                    }
                } else if constexpr (std::is_same_v<T, Trade>) {
                    reduce(e.maker_id, e.qty);
                    reduce(e.taker_id, e.qty);
                } else if constexpr (std::is_same_v<T, Cancelled>) {
                    erase(e.id);
                } else if constexpr (std::is_same_v<T, Modified>) {
                    set(e.id, e.price, e.qty);
                }
            }, ev);
        }
    }

    std::size_t size() const { return ids_.size(); }
    OrderId pick(Rng& rng) const { return ids_[rng.below(ids_.size())]; }
    Entry get(OrderId id) const { return map_.at(id); }

private:
    void set(OrderId id, Price price, Qty qty) {
        auto it = map_.find(id);
        if (it == map_.end()) {
            map_.emplace(id, Entry{ids_.size(), price, qty});
            ids_.push_back(id);
        } else {
            it->second.price = price;
            it->second.qty   = qty;
        }
    }

    void reduce(OrderId id, Qty qty) {
        auto it = map_.find(id);
        if (it == map_.end()) return;
        it->second.qty -= qty;
        if (it->second.qty == 0) erase(id);
    }

    void erase(OrderId id) {
        auto it = map_.find(id);
        if (it == map_.end()) return;
        const std::size_t pos = it->second.pos;
        const OrderId last = ids_.back();
        ids_[pos] = last;            // move the last ID into the hole
        map_.at(last).pos = pos;
        ids_.pop_back();
        map_.erase(id);
    }

    std::vector<OrderId> ids_;
    std::unordered_map<OrderId, Entry> map_;
};

// ---------------------------------------------------------------------
// The generator
// ---------------------------------------------------------------------
inline Workload make_workload(const WorkloadConfig& cfg) {
    Workload w;
    w.min_price = cfg.mid - cfg.band;
    w.max_price = cfg.mid + cfg.band;

    RecordingListener rec;
    NaiveBook<RecordingListener> book(rec, w.min_price, w.max_price, cfg.target_orders * 4);
    Rng rng(cfg.seed);
    LiveOrders live;
    OrderId next_id = 1;

    auto clamp = [&](Price p) {
        return p < w.min_price ? w.min_price : (p > w.max_price ? w.max_price : p);
    };

    // The book's real mid price (or the starting price if a side is empty).
    auto current_mid = [&]() -> Price {
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        if (bid && ask) return (*bid + *ask) / 2;
        if (bid) return *bid;
        if (ask) return *ask;
        return cfg.mid;
    };

    // Apply to the generation book, learn from the events, and save the op.
    auto emit = [&](const Op& op, std::vector<Op>& out) {
        apply(book, op);
        live.observe(rec.events);
        rec.clear();
        out.push_back(op);
    };

    // A resting order that can never cross: buys go strictly below the best
    // ask, sells strictly above the best bid. Each extra tick of distance has
    // a 75% chance, so most orders sit near the top of the book.
    auto passive_add = [&]() {
        Op op;
        op.kind = OpKind::Add;
        op.id   = next_id++;
        op.side = rng.percent(50) ? Side::Buy : Side::Sell;
        op.type = OrderType::Limit;
        Price distance = 1;
        while (rng.percent(75) && distance < 500) ++distance;
        if (op.side == Side::Buy) {
            const auto ask = book.best_ask();
            const Price top = ask ? *ask - 1 : current_mid();
            op.price = clamp(top - (distance - 1));
        } else {
            const auto bid = book.best_bid();
            const Price bottom = bid ? *bid + 1 : current_mid();
            op.price = clamp(bottom + (distance - 1));
        }
        op.qty = rng.percent(95) ? static_cast<Qty>(rng.between(1, 100))
                                 : static_cast<Qty>(rng.between(101, 1000));
        return op;
    };

    // ---- 1. Pre-fill the book (not timed) ----
    while (live.size() < cfg.target_orders) {
        emit(passive_add(), w.prefill);
    }

    // ---- 2. The measured stream ----
    while (w.ops.size() < cfg.num_ops) {
        const uint64_t roll = rng.below(100);
        Op op;

        if (roll < 3) {
            // Aggressive IOC: hits the best price on the other side,
            // sometimes sweeping a level or two. This is what moves prices.
            op.kind = OpKind::Add;
            op.id   = next_id++;
            op.side = rng.percent(50) ? Side::Buy : Side::Sell;
            op.type = OrderType::IOC;
            const auto best = (op.side == Side::Buy) ? book.best_ask() : book.best_bid();
            const Price base = best ? *best : current_mid();
            const Price slip = static_cast<Price>(rng.between(0, 2));
            op.price = clamp(op.side == Side::Buy ? base + slip : base - slip);
            op.qty   = static_cast<Qty>(rng.between(1, 300));
            ++w.aggressive;
        } else if (roll < 10 && live.size() > 0) {
            // Modify: usually reduce size (keeps priority), sometimes reprice.
            op.kind = OpKind::Modify;
            op.id   = live.pick(rng);
            const LiveOrders::Entry e = live.get(op.id);
            if (e.qty > 1 && rng.percent(70)) {
                op.price = e.price;
                op.qty   = static_cast<Qty>(rng.between(1, e.qty - 1));
            } else {
                op.price = clamp(e.price + static_cast<Price>(rng.between(-3, 3)));
                op.qty   = e.qty;
            }
            ++w.modifies;
        } else if (live.size() == 0 ||
                   rng.percent(live.size() < cfg.target_orders ? 60 : 40)) {
            // Adds lean ahead when the book is small, cancels when it's big,
            // so the book hovers around target_orders.
            op = passive_add();
            ++w.adds;
        } else {
            op.kind = OpKind::Cancel;
            op.id   = live.pick(rng);
            ++w.cancels;
        }
        emit(op, w.ops);

        const std::size_t size = live.size();
        w.book_min = std::min(w.book_min, size);
        w.book_max = std::max(w.book_max, size);
        w.book_avg += static_cast<double>(size);
    }
    w.book_avg /= static_cast<double>(w.ops.size());
    return w;
}

}  // namespace lob::bench