#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <limits>

namespace hydra {

// ─────────────────────────────────────────────
//  Fundamental scalar types
// ─────────────────────────────────────────────
using OrderId   = uint64_t;
using TradeId   = uint64_t;
using Price     = int64_t;      // fixed-point; 1 tick = 1 unit
using Quantity  = uint64_t;
using Timestamp = int64_t;      // nanoseconds since epoch
using SequenceNum = uint64_t;
using ClientId  = uint32_t;
using InstrumentId = uint32_t;

// ─────────────────────────────────────────────
//  Sentinel values
// ─────────────────────────────────────────────
static constexpr OrderId    INVALID_ORDER_ID    = 0;
static constexpr Price      INVALID_PRICE       = std::numeric_limits<Price>::max();
static constexpr Quantity   INVALID_QTY         = 0;

// ─────────────────────────────────────────────
//  Order side
// ─────────────────────────────────────────────
enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1
};

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

// ─────────────────────────────────────────────
//  Order type
// ─────────────────────────────────────────────
enum class OrderType : uint8_t {
    Limit       = 0,
    Market      = 1,
    IOC         = 2,   // Immediate Or Cancel
    FOK         = 3,   // Fill Or Kill
    PostOnly    = 4
};

constexpr std::string_view to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit:    return "LIMIT";
        case OrderType::Market:   return "MARKET";
        case OrderType::IOC:      return "IOC";
        case OrderType::FOK:      return "FOK";
        case OrderType::PostOnly: return "POST_ONLY";
        default:                  return "UNKNOWN";
    }
}

// ─────────────────────────────────────────────
//  Order status
// ─────────────────────────────────────────────
enum class OrderStatus : uint8_t {
    New            = 0,
    PartiallyFilled = 1,
    Filled         = 2,
    Cancelled      = 3,
    Rejected       = 4,
    PendingCancel  = 5
};

// ─────────────────────────────────────────────
//  Rejection reason
// ─────────────────────────────────────────────
enum class RejectReason : uint8_t {
    None              = 0,
    InvalidPrice      = 1,
    InvalidQuantity   = 2,
    ExceedsRiskLimit  = 3,
    DuplicateOrderId  = 4,
    UnknownInstrument = 5,
    InsufficientFunds = 6,
    WouldCross        = 7,   // PostOnly crossed
    MarketHalted      = 8
};

// ─────────────────────────────────────────────
//  Time-in-force (embedded in OrderType above,
//  but kept separate for clarity)
// ─────────────────────────────────────────────
enum class TimeInForce : uint8_t {
    GTC = 0,   // Good Till Cancelled
    IOC = 1,   // Immediate Or Cancel
    FOK = 2,   // Fill Or Kill
    GTD = 3    // Good Till Date
};

// ─────────────────────────────────────────────
//  Market state
// ─────────────────────────────────────────────
enum class MarketState : uint8_t {
    PreOpen   = 0,
    Open      = 1,
    Halted    = 2,
    Closed    = 3,
    Auction   = 4
};

// ─────────────────────────────────────────────
//  Tick-size helpers (1 tick = 1 Price unit)
// ─────────────────────────────────────────────
constexpr Price price_from_double(double p, int64_t tick_denom = 100) noexcept {
    return static_cast<Price>(p * tick_denom + 0.5);
}

constexpr double price_to_double(Price p, int64_t tick_denom = 100) noexcept {
    return static_cast<double>(p) / tick_denom;
}

} // namespace hydra
