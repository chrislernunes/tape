#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PriceLevel: all orders at a single price point
//
//  Implemented as an intrusive doubly-linked list so that:
//    - Insert tail: O(1) — append to list tail
//    - Delete:      O(1) — unlink using prev/next pointers stored in Order
//    - Front peek:  O(1) — head pointer
//
//  PriceLevel itself is owned by the order book's price map.
// ─────────────────────────────────────────────────────────────────────────────

#include "hydra/order.hpp"
#include <cstdint>
#include <cassert>

namespace hydra {

class PriceLevel {
public:
    PriceLevel() noexcept = default;

    // Non-copyable; linked list pointers would alias
    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    // ── Queue operations ──────────────────────────────────────────────────────

    // Append order at the back (time priority)
    void push_back(Order* o) noexcept {
        assert(o);
        o->prev = tail_;
        o->next = nullptr;

        if (tail_) tail_->next = o;
        else       head_ = o;

        tail_ = o;
        ++order_count_;
        total_qty_ += o->leaves_qty;
    }

    // Remove an order from anywhere in the list (cancel / fill)
    void remove(Order* o) noexcept {
        assert(o);
        assert(order_count_ > 0);

        if (o->prev) o->prev->next = o->next;
        else         head_ = o->next;

        if (o->next) o->next->prev = o->prev;
        else         tail_ = o->prev;

        o->prev = nullptr;
        o->next = nullptr;

        --order_count_;
        total_qty_ -= o->leaves_qty;
    }

    // Reduce quantity (partial fill at head order)
    void reduce_qty(Order* o, Quantity fill_qty) noexcept {
        assert(fill_qty <= o->leaves_qty);
        total_qty_ -= fill_qty;
        o->fill(fill_qty);
        if (o->fully_filled()) remove(o);
    }

    // ── Accessors ─────────────────────────────────────────────────────────────
    [[nodiscard]] Order*    head()        const noexcept { return head_; }
    [[nodiscard]] Order*    tail()        const noexcept { return tail_; }
    [[nodiscard]] bool      empty()       const noexcept { return head_ == nullptr; }
    [[nodiscard]] uint32_t  order_count() const noexcept { return order_count_; }
    [[nodiscard]] Quantity  total_qty()   const noexcept { return total_qty_; }

    // Iterate all orders (oldest first)
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (Order* o = head_; o; o = o->next) fn(o);
    }

private:
    Order*   head_{nullptr};
    Order*   tail_{nullptr};
    uint32_t order_count_{0};
    Quantity total_qty_{0};
};

} // namespace hydra
