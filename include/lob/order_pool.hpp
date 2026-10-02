// include/lob/order_pool.hpp
//
// A fixed-size pool of Order slots, allocated once at startup.
// Orders are handed out and returned by Handle (a slot index).
// No heap allocation ever happens after construction.

#pragma once

#include <cassert>   // assert: checks that are active only in Debug builds
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

class OrderPool {
public:
    // Creates `capacity` usable slots (plus the reserved slot 0).
    explicit OrderPool(std::size_t capacity)
        : orders_(capacity + 1)        // value-initialized: every page touched now
#ifndef NDEBUG
        , in_use_(capacity + 1, false)
#endif
    {
        assert(capacity < 0xFFFFFFFFu && "capacity must fit in a 32-bit Handle");

        // Chain slots 1..capacity into the free list, in order,
        // so the first allocations hand out 1, 2, 3, ...
        for (std::size_t i = 1; i <= capacity; ++i) {
            orders_[i].next = (i < capacity) ? static_cast<Handle>(i + 1) : kNullHandle;
        }
        free_head_ = (capacity > 0) ? 1 : kNullHandle;
    }

    // Copying a pool would silently duplicate megabytes of memory,
    // so forbid it. "= delete" makes any attempt a compile error.
    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    // Take a free slot. Returns kNullHandle if the pool is full.
    // The caller must fill in every field of the returned Order.
    Handle allocate() noexcept {
        const Handle h = free_head_;
        if (h == kNullHandle) {
            return kNullHandle;
        }
        free_head_ = orders_[h].next;
        ++used_;
#ifndef NDEBUG
        assert(!in_use_[h] && "free list corrupted: slot already in use");
        in_use_[h] = true;
#endif
        return h;
    }

    // Give a slot back. It becomes the next one handed out (LIFO).
    void release(Handle h) noexcept {
        assert(h != kNullHandle && h < orders_.size() && "invalid handle");
#ifndef NDEBUG
        assert(in_use_[h] && "double release, or slot was never allocated");
        in_use_[h] = false;
#endif
        orders_[h].next = free_head_;
        free_head_ = h;
        --used_;
    }

    // Access the order in a slot: pool[h].
    Order& operator[](Handle h) noexcept {
        assert(h != kNullHandle && h < orders_.size() && "invalid handle");
        return orders_[h];
    }
    const Order& operator[](Handle h) const noexcept {
        assert(h != kNullHandle && h < orders_.size() && "invalid handle");
        return orders_[h];
    }

    std::size_t capacity() const noexcept { return orders_.size() - 1; }
    std::size_t size() const noexcept { return used_; }      // slots in use
    bool        full() const noexcept { return free_head_ == kNullHandle; }

private:
    std::vector<Order> orders_;   // slot 0 is reserved and never handed out
#ifndef NDEBUG
    std::vector<bool>  in_use_;   // Debug only: which slots are allocated
#endif
    Handle      free_head_ = kNullHandle;
    std::size_t used_ = 0;
};

}  // namespace lob