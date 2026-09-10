#pragma once

#include "hydra/types.hpp"
#include <cstring>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  Order: immutable identity + mutable fill state
//
//  Kept deliberately flat to fit in a single or two cache lines.
//  Hot fields: id, price, qty, leaves_qty, side, type, status
//  Cold fields: client_id, instrument_id, timestamps
//
//  Size target: ≤ 128 bytes (two cache lines)
// ─────────────────────────────────────────────────────────────────────────────

struct alignas(64) Order {
    // ── Identity (immutable after submission) ─────────────────────────────────
    OrderId      id{INVALID_ORDER_ID};
    ClientId     client_id{0};
    InstrumentId instrument_id{0};

    // ── Price / size ──────────────────────────────────────────────────────────
    Price        price{0};            // limit price (ignored for market orders)
    Quantity     original_qty{0};     // quantity at submission
    Quantity     leaves_qty{0};       // remaining unfilled
    Quantity     filled_qty{0};       // cumulative fills

    // ── Classification ────────────────────────────────────────────────────────
    Side         side{Side::Buy};
    OrderType    type{OrderType::Limit};
    TimeInForce  tif{TimeInForce::GTC};
    OrderStatus  status{OrderStatus::New};

    // ── Timestamps (nanoseconds) ──────────────────────────────────────────────
    Timestamp    submit_time{0};      // when client submitted
    Timestamp    entry_time{0};       // when engine received
    Timestamp    last_update{0};      // last status change

    // ── Queue linkage (intrusive doubly-linked list at price level) ───────────
    Order*       prev{nullptr};       // previous in price level queue
    Order*       next{nullptr};       // next in price level queue

    // ── Helpers ───────────────────────────────────────────────────────────────
    [[nodiscard]] bool is_buy()     const noexcept { return side == Side::Buy; }
    [[nodiscard]] bool is_sell()    const noexcept { return side == Side::Sell; }
    [[nodiscard]] bool is_active()  const noexcept {
        return status == OrderStatus::New ||
               status == OrderStatus::PartiallyFilled;
    }
    [[nodiscard]] bool is_market()  const noexcept { return type == OrderType::Market; }
    [[nodiscard]] bool fully_filled() const noexcept { return leaves_qty == 0; }

    // Average fill price (requires tracking in strategy layer)
    [[nodiscard]] double avg_fill_price(Price total_value_ticks) const noexcept {
        if (filled_qty == 0) return 0.0;
        return static_cast<double>(total_value_ticks) / filled_qty;
    }

    void fill(Quantity qty) noexcept {
        leaves_qty  -= qty;
        filled_qty  += qty;
        status = (leaves_qty == 0) ? OrderStatus::Filled
                                   : OrderStatus::PartiallyFilled;
    }

    void cancel() noexcept { status = OrderStatus::Cancelled; }

    // Create a default valid limit order
    static Order make_limit(OrderId id, ClientId cid, InstrumentId iid,
                            Side side, Price price, Quantity qty,
                            Timestamp ts = 0) noexcept {
        Order o;
        o.id            = id;
        o.client_id     = cid;
        o.instrument_id = iid;
        o.price         = price;
        o.original_qty  = qty;
        o.leaves_qty    = qty;
        o.filled_qty    = 0;
        o.side          = side;
        o.type          = OrderType::Limit;
        o.tif           = TimeInForce::GTC;
        o.status        = OrderStatus::New;
        o.submit_time   = ts;
        o.entry_time    = ts;
        o.last_update   = ts;
        return o;
    }

    static Order make_market(OrderId id, ClientId cid, InstrumentId iid,
                             Side side, Quantity qty, Timestamp ts = 0) noexcept {
        Order o = make_limit(id, cid, iid, side, 0, qty, ts);
        o.type  = OrderType::Market;
        o.price = (side == Side::Buy) ? std::numeric_limits<Price>::max()
                                      : std::numeric_limits<Price>::min();
        return o;
    }
};

static_assert(sizeof(Order) <= 128, "Order struct exceeds two cache lines");

// ─────────────────────────────────────────────────────────────────────────────
//  OrderRequest: wire-format message from client → gateway
//  Smaller than Order; gateway inflates it before engine insertion.
// ─────────────────────────────────────────────────────────────────────────────
struct OrderRequest {
    enum class Action : uint8_t { New = 0, Cancel = 1, Modify = 2 };

    Action       action{Action::New};
    OrderId      order_id{0};
    ClientId     client_id{0};
    InstrumentId instrument_id{0};
    Side         side{Side::Buy};
    OrderType    type{OrderType::Limit};
    TimeInForce  tif{TimeInForce::GTC};
    Price        price{0};
    Quantity     quantity{0};
    Timestamp    timestamp{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  OrderAck: engine → gateway → client
// ─────────────────────────────────────────────────────────────────────────────
struct OrderAck {
    enum class Type : uint8_t { Accepted = 0, Rejected = 1, Cancelled = 2 };

    Type         type{Type::Accepted};
    OrderId      order_id{0};
    ClientId     client_id{0};
    RejectReason reason{RejectReason::None};
    Timestamp    timestamp{0};
    SequenceNum  seq{0};
};

} // namespace hydra
