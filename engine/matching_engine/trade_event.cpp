#pragma once

#include "../../include/hydra/types.hpp"
#include <string>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  TradeEvent: published to all market data subscribers after each match
//
//  Contains both sides for the trade tape, plus aggressor info.
// ─────────────────────────────────────────────────────────────────────────────
struct TradeEvent {
    TradeId      trade_id{0};
    InstrumentId instrument_id{0};

    // ── Parties ───────────────────────────────────────────────────────────────
    OrderId      maker_order_id{0};
    OrderId      taker_order_id{0};
    ClientId     maker_client_id{0};
    ClientId     taker_client_id{0};

    // ── Terms ─────────────────────────────────────────────────────────────────
    Price        price{0};
    Quantity     quantity{0};
    Side         aggressor_side{Side::Buy};  // taker side

    // ── Timing ───────────────────────────────────────────────────────────────
    Timestamp    timestamp{0};
    SequenceNum  seq_num{0};

    // ── Helpers ───────────────────────────────────────────────────────────────
    [[nodiscard]] double notional(int64_t tick_denom = 100) const noexcept {
        return price_to_double(price, tick_denom) * static_cast<double>(quantity);
    }

    [[nodiscard]] std::string to_string() const {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "TRADE[%llu] inst=%u px=%lld qty=%llu side=%s maker=%llu taker=%llu",
            static_cast<unsigned long long>(trade_id),
            instrument_id,
            static_cast<long long>(price),
            static_cast<unsigned long long>(quantity),
            aggressor_side == Side::Buy ? "BUY" : "SELL",
            static_cast<unsigned long long>(maker_order_id),
            static_cast<unsigned long long>(taker_order_id));
        return buf;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ExecutionReport: sent back to the client (both maker and taker)
// ─────────────────────────────────────────────────────────────────────────────
struct ExecutionReport {
    enum class ExecType : uint8_t {
        New         = 0,
        PartialFill = 1,
        Fill        = 2,
        Cancelled   = 3,
        Rejected    = 4,
        Replaced    = 5
    };

    ExecType     exec_type{ExecType::New};
    OrderId      order_id{0};
    TradeId      trade_id{0};      // 0 if not a fill
    ClientId     client_id{0};
    InstrumentId instrument_id{0};

    Price        last_px{0};       // price of this fill
    Quantity     last_qty{0};      // qty of this fill
    Quantity     leaves_qty{0};    // remaining
    Quantity     cum_qty{0};       // total filled so far

    Price        avg_px{0};        // volume-weighted avg fill price
    Side         side{Side::Buy};
    OrderStatus  status{OrderStatus::New};
    RejectReason reject_reason{RejectReason::None};

    Timestamp    timestamp{0};
    SequenceNum  seq_num{0};

    [[nodiscard]] bool is_fill() const noexcept {
        return exec_type == ExecType::PartialFill ||
               exec_type == ExecType::Fill;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  MarketDataUpdate: L1 / L2 / trade in a single discriminated union
// ─────────────────────────────────────────────────────────────────────────────
struct MarketDataUpdate {
    enum class Type : uint8_t {
        L1Quote    = 0,
        L2Update   = 1,
        Trade      = 2,
        Heartbeat  = 3,
        StatusMsg  = 4
    };

    Type         type{Type::L1Quote};
    InstrumentId instrument_id{0};
    SequenceNum  seq{0};
    Timestamp    timestamp{0};

    struct L1 {
        Price    bid_px{0};
        Quantity bid_qty{0};
        Price    ask_px{0};
        Quantity ask_qty{0};
        Price    last_trade_px{0};
        Quantity last_trade_qty{0};
    };

    struct L2Delta {
        Side     side{Side::Buy};
        Price    price{0};
        Quantity qty{0};       // 0 = level deleted
        bool     is_delete{false};
    };

    union {
        L1      l1;
        L2Delta l2;
        TradeEvent trade;
    };

    MarketDataUpdate() noexcept : l1{} {}
};

} // namespace hydra
