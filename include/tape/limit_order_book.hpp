#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  LimitOrderBook
//
//  The central data structure of Tape.
//
//  Design choices:
//  ┌──────────────────────────────────────────────────────────────────┐
//  │  price_map   : std::map<Price, PriceLevel>                       │
//  │              → O(log N) level insert/remove (rare)               │
//  │              → O(1) best price via rbegin/begin                  │
//  │                                                                  │
//  │  order_index : std::unordered_map<OrderId, Order*>               │
//  │              → O(1) cancel / modify lookup                       │
//  │                                                                  │
//  │  order_pool  : MemoryPool<Order, 1M>                             │
//  │              → zero allocation on hot path                       │
//  └──────────────────────────────────────────────────────────────────┘
//
//  Matching is done externally by the MatchingEngine which calls
//  insert_and_match() to perform price-time matching.
// ─────────────────────────────────────────────────────────────────────────────

#include "tape/price_level.hpp"
#include "tape/detail/memory_pool.hpp"
#include "tape/order.hpp"

#include <map>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <optional>
#include <vector>

namespace tape {

// ─────────────────────────────────────────────────────────────────────────────
//  Book-level snapshot (L2)
// ─────────────────────────────────────────────────────────────────────────────
struct PriceLevelSnapshot {
    Price    price;
    Quantity qty;
    uint32_t order_count;
};

struct BookSnapshot {
    InstrumentId instrument_id;
    Timestamp    timestamp;
    std::vector<PriceLevelSnapshot> bids;  // sorted high→low
    std::vector<PriceLevelSnapshot> asks;  // sorted low→high
};

// ─────────────────────────────────────────────────────────────────────────────
//  LimitOrderBook
// ─────────────────────────────────────────────────────────────────────────────
class LimitOrderBook {
    static constexpr std::size_t POOL_SIZE = 1'000'000;

public:
    // Callback signatures
    using OnFill   = std::function<void(OrderId maker, OrderId taker,
                                        Price, Quantity, Timestamp)>;
    using OnCancel = std::function<void(OrderId, Quantity leaves)>;

    explicit LimitOrderBook(InstrumentId id) noexcept : instrument_id_(id) {}

    LimitOrderBook(const LimitOrderBook&)            = delete;
    LimitOrderBook& operator=(const LimitOrderBook&) = delete;

    // ── Insert resting limit order ────────────────────────────────────────────
    // Returns: true if order rests in book, false if immediately consumed (IOC)
    bool add_order(Order* o) {
        assert(o);
        assert(o->type == OrderType::Limit || o->type == OrderType::PostOnly);

        if (order_index_.count(o->id)) return false;  // duplicate

        auto& level_map = (o->is_buy()) ? bids_ : asks_;
        level_map[o->price].push_back(o);
        order_index_[o->id] = o;
        return true;
    }

    // ── Cancel a resting order ────────────────────────────────────────────────
    [[nodiscard]] bool cancel_order(OrderId id) noexcept {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) return false;

        Order* o = it->second;
        auto& level_map = o->is_buy() ? bids_ : asks_;
        auto level_it   = level_map.find(o->price);

        if (level_it != level_map.end()) {
            level_it->second.remove(o);
            if (level_it->second.empty())
                level_map.erase(level_it);
        }

        o->cancel();
        order_index_.erase(it);
        return true;
    }

    // ── Lookup ────────────────────────────────────────────────────────────────
    [[nodiscard]] Order* find_order(OrderId id) const noexcept {
        auto it = order_index_.find(id);
        return (it != order_index_.end()) ? it->second : nullptr;
    }

    // ── Best bid / ask ────────────────────────────────────────────────────────
    [[nodiscard]] Price best_bid() const noexcept {
        return bids_.empty() ? INVALID_PRICE : bids_.rbegin()->first;
    }

    [[nodiscard]] Price best_ask() const noexcept {
        return asks_.empty() ? INVALID_PRICE : asks_.begin()->first;
    }

    [[nodiscard]] bool has_bid() const noexcept { return !bids_.empty(); }
    [[nodiscard]] bool has_ask() const noexcept { return !asks_.empty(); }

    [[nodiscard]] Price mid_price() const noexcept {
        if (!has_bid() || !has_ask()) return INVALID_PRICE;
        return (best_bid() + best_ask()) / 2;
    }

    [[nodiscard]] Price spread() const noexcept {
        if (!has_bid() || !has_ask()) return INVALID_PRICE;
        return best_ask() - best_bid();
    }

    // ── L1 quantities ─────────────────────────────────────────────────────────
    [[nodiscard]] Quantity best_bid_qty() const noexcept {
        if (bids_.empty()) return 0;
        return bids_.rbegin()->second.total_qty();
    }

    [[nodiscard]] Quantity best_ask_qty() const noexcept {
        if (asks_.empty()) return 0;
        return asks_.begin()->second.total_qty();
    }

    // Oldest resting order at the touch. Used by the CST cancel resolver so
    // cancels hit a real live id, not a guessed counter.
    [[nodiscard]] Order* oldest_at_best(Side side) const noexcept {
        if (side == Side::Buy) {
            if (bids_.empty()) return nullptr;
            return bids_.rbegin()->second.head();
        }
        if (asks_.empty()) return nullptr;
        return asks_.begin()->second.head();
    }

    [[nodiscard]] Quantity qty_at(Side side, Price px) const noexcept {
        const auto& level_map = (side == Side::Buy) ? bids_ : asks_;
        auto it = level_map.find(px);
        return (it == level_map.end()) ? 0 : it->second.total_qty();
    }

    // ── L2 snapshot (top N levels) ────────────────────────────────────────────
    [[nodiscard]] BookSnapshot snapshot(Timestamp ts, int depth = 10) const {
        BookSnapshot snap;
        snap.instrument_id = instrument_id_;
        snap.timestamp     = ts;

        int n = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && n < depth; ++it, ++n)
            snap.bids.push_back({it->first, it->second.total_qty(), it->second.order_count()});

        n = 0;
        for (auto it = asks_.begin(); it != asks_.end() && n < depth; ++it, ++n)
            snap.asks.push_back({it->first, it->second.total_qty(), it->second.order_count()});

        return snap;
    }

    // ── Try to match a taker order against the book ───────────────────────────
    // Returns vector of (maker_order*, fill_qty) pairs in match order.
    // Taker order's leaves_qty is NOT modified here — MatchingEngine does it.
    struct MatchResult {
        Order*   maker_order;
        Quantity fill_qty;
        Price    fill_price;
    };

    std::vector<MatchResult> match(Order* taker) {
        std::vector<MatchResult> fills;
        if (!taker || !taker->is_active()) return fills;

        // For a BUY taker, walk asks from lowest price upward
        // For a SELL taker, walk bids from highest price downward
        if (taker->is_buy()) {
            for (auto it = asks_.begin();
                 it != asks_.end() && taker->leaves_qty > 0; ) {

                if (it->first > taker->price && !taker->is_market()) break;

                PriceLevel& level = it->second;
                while (!level.empty() && taker->leaves_qty > 0) {
                    Order* maker = level.head();
                    Quantity fill = std::min(maker->leaves_qty, taker->leaves_qty);
                    fills.push_back({maker, fill, it->first});
                    level.reduce_qty(maker, fill);
                    taker->leaves_qty -= fill;
                    taker->filled_qty += fill;
                    if (maker->fully_filled())
                        order_index_.erase(maker->id);
                }

                auto next = std::next(it);
                if (level.empty()) asks_.erase(it);
                it = next;
            }
        } else {
            for (auto it = bids_.rbegin();
                 it != bids_.rend() && taker->leaves_qty > 0; ) {

                if (it->first < taker->price && !taker->is_market()) break;

                PriceLevel& level = it->second;
                while (!level.empty() && taker->leaves_qty > 0) {
                    Order* maker = level.head();
                    Quantity fill = std::min(maker->leaves_qty, taker->leaves_qty);
                    fills.push_back({maker, fill, it->first});
                    level.reduce_qty(maker, fill);
                    taker->leaves_qty -= fill;
                    taker->filled_qty += fill;
                    if (maker->fully_filled())
                        order_index_.erase(maker->id);
                }

                if (level.empty()) {
                    it = decltype(it)(bids_.erase(std::next(it).base()));
                } else {
                    ++it;
                }
            }
        }

        if (taker->fully_filled())
            taker->status = OrderStatus::Filled;
        else if (taker->filled_qty > 0)
            taker->status = OrderStatus::PartiallyFilled;

        return fills;
    }

    // ── FOK / all-or-none feasibility check (does NOT mutate the book) ────────
    // Walks the same price levels match() would, in the same order, summing
    // available quantity via each level's O(1) total_qty() — never touching
    // an Order or a queue position. Lets a caller ask "would match() fully
    // fill this?" before deciding whether match() should run at all, which
    // is exactly what true Fill-Or-Kill semantics require: either the whole
    // order executes, or none of it does. (This method's own crossing
    // condition intentionally mirrors match()'s; if that condition ever
    // changes, this one must change with it — see test_order_types.cpp's
    // FOK cases, which pin both together.)
    [[nodiscard]] bool can_fully_fill(Side side, Price limit_price,
                                       Quantity qty, bool is_market) const noexcept {
        Quantity available = 0;
        if (side == Side::Buy) {
            for (auto it = asks_.begin(); it != asks_.end(); ++it) {
                if (it->first > limit_price && !is_market) break;
                available += it->second.total_qty();
                if (available >= qty) return true;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                if (it->first < limit_price && !is_market) break;
                available += it->second.total_qty();
                if (available >= qty) return true;
            }
        }
        return available >= qty;
    }

    // ── Statistics ────────────────────────────────────────────────────────────
    [[nodiscard]] std::size_t total_orders() const noexcept {
        return order_index_.size();
    }

    [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }

    [[nodiscard]] InstrumentId instrument_id() const noexcept {
        return instrument_id_;
    }

    // Compute total bid/ask depth
    [[nodiscard]] Quantity total_bid_qty() const noexcept {
        Quantity q = 0;
        for (auto& [p, lvl] : bids_) q += lvl.total_qty();
        return q;
    }

    [[nodiscard]] Quantity total_ask_qty() const noexcept {
        Quantity q = 0;
        for (auto& [p, lvl] : asks_) q += lvl.total_qty();
        return q;
    }

    // Order flow imbalance: (bid_qty - ask_qty) / (bid_qty + ask_qty)
    [[nodiscard]] double ofi() const noexcept {
        double b = static_cast<double>(total_bid_qty());
        double a = static_cast<double>(total_ask_qty());
        if (b + a == 0.0) return 0.0;
        return (b - a) / (b + a);
    }

private:
    InstrumentId instrument_id_;

    // Bids: sorted ascending → best bid at rbegin
    std::map<Price, PriceLevel> bids_;
    // Asks: sorted ascending → best ask at begin
    std::map<Price, PriceLevel> asks_;

    // O(1) cancel lookup
    std::unordered_map<OrderId, Order*> order_index_;
};

} // namespace tape
