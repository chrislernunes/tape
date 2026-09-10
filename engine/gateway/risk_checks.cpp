#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  RiskEngine: pre-trade risk checks applied at the gateway layer
//
//  Checks performed in order (fail-fast):
//    1. Order size limits (per-order)
//    2. Notional limits (per-order)
//    3. Open order count per client
//    4. Gross position limits per client per instrument
//    5. Daily loss limit (PnL check — requires execution feedback)
//
//  All checks are O(1) using per-client state tables.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/hydra/order.hpp"
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace hydra {

struct RiskLimits {
    Quantity max_order_qty       = 10'000;
    double   max_notional        = 10'000'000.0;   // USD
    uint32_t max_open_orders     = 500;
    Quantity max_gross_position  = 100'000;
    double   max_daily_loss      = -50'000.0;       // USD; negative
    int64_t  tick_denom          = 100;             // for price→double
};

struct ClientRiskState {
    uint32_t open_order_count{0};
    int64_t  net_position{0};       // + = long
    uint64_t gross_position{0};     // abs(long) + abs(short) traded today
    double   avg_cost{0.0};
    double   realized_pnl{0.0};
    double   unrealized_pnl{0.0};
    double   daily_pnl{0.0};
    Price    last_mark{0};
};

class RiskEngine {
public:
    RiskEngine() : limits_(RiskLimits{}) {}
    explicit RiskEngine(RiskLimits limits) : limits_(limits) {}

    // ── Set / get limits ──────────────────────────────────────────────────────
    void set_limits(const RiskLimits& l) noexcept { limits_ = l; }
    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

    // ── Pre-trade check ───────────────────────────────────────────────────────
    [[nodiscard]] RejectReason check(const OrderRequest& req) const noexcept {
        // Cancels identify an existing order and carry no size/price.
        // Treating them as New orders (quantity==0 → reject) dropped every
        // agent and synthetic cancel at the gateway.
        if (req.action == OrderRequest::Action::Cancel)
            return RejectReason::None;

        // 1. Quantity
        if (req.quantity == 0 || req.quantity > limits_.max_order_qty)
            return RejectReason::InvalidQuantity;

        // 2. Price
        if (req.type == OrderType::Limit && req.price <= 0)
            return RejectReason::InvalidPrice;

        // 3. Notional
        if (req.type == OrderType::Limit) {
            double notional = price_to_double(req.price, limits_.tick_denom)
                            * static_cast<double>(req.quantity);
            if (notional > limits_.max_notional)
                return RejectReason::ExceedsRiskLimit;
        }

        auto it = client_state_.find(req.client_id);
        if (it != client_state_.end()) {
            const auto& state = it->second;

            // 4. Open order count
            if (state.open_order_count >= limits_.max_open_orders)
                return RejectReason::ExceedsRiskLimit;

            // 5. Gross position
            if (state.gross_position + req.quantity > limits_.max_gross_position)
                return RejectReason::ExceedsRiskLimit;

            // 6. Daily loss
            if (state.daily_pnl < limits_.max_daily_loss)
                return RejectReason::ExceedsRiskLimit;
        }

        return RejectReason::None;
    }

    // ── State updates (called by execution report handler) ───────────────────
    void on_order_new(ClientId cid) noexcept {
        client_state_[cid].open_order_count++;
    }

    void on_order_cancel(ClientId cid) noexcept {
        auto& s = client_state_[cid];
        if (s.open_order_count > 0) --s.open_order_count;
    }

    void on_fill(ClientId cid, Side side, Quantity qty,
                 Price px, int64_t tick_denom = 100) noexcept {
        auto& s = client_state_[cid];
        if (s.open_order_count > 0) --s.open_order_count;

        int64_t signed_qty = (side == Side::Buy) ? static_cast<int64_t>(qty)
                                                  : -static_cast<int64_t>(qty);
        int64_t prev_pos   = s.net_position;
        int64_t next_pos   = prev_pos + signed_qty;
        s.gross_position  += qty;

        double fill_px = price_to_double(px, tick_denom);

        if ((prev_pos > 0 && signed_qty < 0) ||
            (prev_pos < 0 && signed_qty > 0)) {
            int64_t close_qty = std::min(std::abs(prev_pos),
                                         std::abs(signed_qty));
            double pnl = (prev_pos > 0)
                       ? (fill_px - s.avg_cost) * static_cast<double>(close_qty)
                       : (s.avg_cost - fill_px) * static_cast<double>(close_qty);
            s.realized_pnl += pnl;
        }

        if (next_pos == 0) {
            s.avg_cost = 0.0;
        } else if (prev_pos == 0 || (prev_pos > 0) != (next_pos > 0)) {
            s.avg_cost = fill_px;
        } else if (std::abs(next_pos) > std::abs(prev_pos)) {
            double prev_abs = static_cast<double>(std::abs(prev_pos));
            double add_abs  = static_cast<double>(std::abs(signed_qty));
            s.avg_cost = (s.avg_cost * prev_abs + fill_px * add_abs)
                       / (prev_abs + add_abs);
        }

        s.net_position = next_pos;
        if (s.last_mark != 0) {
            double mid = price_to_double(s.last_mark, tick_denom);
            s.unrealized_pnl = static_cast<double>(s.net_position)
                             * (mid - s.avg_cost);
        }
        s.daily_pnl = s.realized_pnl + s.unrealized_pnl;
    }

    void mark_to_market(ClientId cid, Price mid_px,
                        int64_t tick_denom = 100) noexcept {
        auto it = client_state_.find(cid);
        if (it == client_state_.end()) return;
        auto& s = it->second;
        s.last_mark = mid_px;
        double mid = price_to_double(mid_px, tick_denom);
        s.unrealized_pnl = static_cast<double>(s.net_position) * (mid - s.avg_cost);
        s.daily_pnl = s.realized_pnl + s.unrealized_pnl;
    }

    [[nodiscard]] const ClientRiskState* state(ClientId cid) const noexcept {
        auto it = client_state_.find(cid);
        return (it != client_state_.end()) ? &it->second : nullptr;
    }

    void reset_daily(ClientId cid) noexcept {
        auto it = client_state_.find(cid);
        if (it == client_state_.end()) return;
        it->second.realized_pnl   = 0.0;
        it->second.unrealized_pnl = 0.0;
        it->second.daily_pnl      = 0.0;
        it->second.gross_position = 0;
    }

private:
    RiskLimits                                         limits_;
    mutable std::unordered_map<ClientId, ClientRiskState> client_state_;
};

} // namespace hydra
