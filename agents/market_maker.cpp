#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Agent Framework
//
//  All simulated market participants inherit from Agent.
//  Each agent:
//    1. Receives market data updates (L1/L2/trades)
//    2. Maintains internal state
//    3. Submits OrderRequests to the gateway
//    4. Receives execution reports back
//
//  The simulation loop calls agent.on_market_data() → agent submits orders
//  → engine processes → agent.on_exec_report() is called.
// ─────────────────────────────────────────────────────────────────────────────

#include "../engine/matching_engine/trade_event.cpp"
#include "../engine/gateway/order_gateway.cpp"
#include "../simulation/latency_model/network_delay.cpp"
#include "../infrastructure/timestamp.hpp"

#include <string>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <cmath>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  Agent base class
// ─────────────────────────────────────────────────────────────────────────────
class Agent {
public:
    explicit Agent(ClientId cid, InstrumentId iid, OrderGateway* gw,
                   SimClock* clk)
        : client_id_(cid), instrument_id_(iid), gateway_(gw), clock_(clk)
    {}

    virtual ~Agent() = default;

    // Called by simulation loop with each new market data event
    virtual void on_market_data(const MarketDataUpdate& mdu) = 0;

    // Called when an execution report arrives for our orders
    virtual void on_exec_report(const ExecutionReport& er) = 0;

    // Name for logging
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    [[nodiscard]] ClientId     client_id()     const noexcept { return client_id_; }
    [[nodiscard]] InstrumentId instrument_id() const noexcept { return instrument_id_; }

    // Stats
    [[nodiscard]] double realized_pnl()   const noexcept { return realized_pnl_; }
    [[nodiscard]] double unrealized_pnl() const noexcept { return unrealized_pnl_; }
    [[nodiscard]] double avg_entry()      const noexcept { return avg_cost_basis_; }
    [[nodiscard]] int64_t net_position()  const noexcept { return net_position_; }

    void mark_to_market(Price mid) noexcept {
        if (net_position_ == 0 || mid == 0 || mid == INVALID_PRICE) {
            unrealized_pnl_ = 0.0;
            return;
        }
        double m = price_to_double(mid);
        unrealized_pnl_ = static_cast<double>(net_position_) * (m - avg_cost_basis_);
    }

protected:
    // ── Order helpers ─────────────────────────────────────────────────────────
    void submit_limit(Side side, Price price, Quantity qty,
                      TimeInForce tif = TimeInForce::GTC) {
        OrderRequest req;
        req.action       = OrderRequest::Action::New;
        req.order_id     = next_local_id_++;
        req.client_id    = client_id_;
        req.instrument_id = instrument_id_;
        req.side         = side;
        req.type         = OrderType::Limit;
        req.tif          = tif;
        req.price        = price;
        req.quantity     = qty;
        req.timestamp    = clock_->now();
        gateway_->submit(req);
        ++stat_orders_sent_;
    }

    void submit_market(Side side, Quantity qty) {
        OrderRequest req;
        req.action       = OrderRequest::Action::New;
        req.order_id     = next_local_id_++;
        req.client_id    = client_id_;
        req.instrument_id = instrument_id_;
        req.side         = side;
        req.type         = OrderType::Market;
        req.quantity     = qty;
        req.timestamp    = clock_->now();
        gateway_->submit(req);
        ++stat_orders_sent_;
    }

    void cancel_order(OrderId oid) {
        OrderRequest req;
        req.action       = OrderRequest::Action::Cancel;
        req.order_id     = oid;
        req.client_id    = client_id_;
        req.instrument_id = instrument_id_;
        req.timestamp    = clock_->now();
        gateway_->submit(req);
    }

    // Track PnL from fills
    void record_fill(Side side, Quantity qty, Price fill_px) noexcept {
        if (qty == 0) return;
        double price = price_to_double(fill_px);
        int64_t delta = (side == Side::Buy) ? static_cast<int64_t>(qty)
                                            : -static_cast<int64_t>(qty);
        int64_t prev = net_position_;
        int64_t next = prev + delta;

        if ((prev > 0 && delta < 0) || (prev < 0 && delta > 0)) {
            int64_t close = std::min(std::abs(prev), std::abs(delta));
            double pnl = (prev > 0)
                       ? (price - avg_cost_basis_) * static_cast<double>(close)
                       : (avg_cost_basis_ - price) * static_cast<double>(close);
            realized_pnl_ += pnl;
        }

        if (next == 0) {
            avg_cost_basis_ = 0.0;
        } else if (prev == 0 || (prev > 0) != (next > 0)) {
            avg_cost_basis_ = price;
        } else if (std::abs(next) > std::abs(prev)) {
            double prev_abs = static_cast<double>(std::abs(prev));
            double add_abs  = static_cast<double>(std::abs(delta));
            avg_cost_basis_ = (avg_cost_basis_ * prev_abs + price * add_abs)
                            / (prev_abs + add_abs);
        }

        net_position_ = next;
        ++stat_fills_received_;
    }

    ClientId     client_id_;
    InstrumentId instrument_id_;
    OrderGateway* gateway_;
    SimClock*    clock_;

    uint64_t next_local_id_{1};
    int64_t  net_position_{0};
    double   realized_pnl_{0.0};
    double   avg_cost_basis_{0.0};
    double   unrealized_pnl_{0.0};

    uint64_t stat_orders_sent_{0};
    uint64_t stat_fills_received_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  MarketMakerAgent
//
//  Strategy:
//    1. Post bid at mid - half_spread, ask at mid + half_spread
//    2. Adjust for inventory: skew quotes toward reducing position
//    3. On fill: immediately requote
//    4. Cancel stale quotes if mid moves by > stale_threshold ticks
//
//  This replicates a simple Avellaneda-Stoikov style market maker.
// ─────────────────────────────────────────────────────────────────────────────
class MarketMakerAgent : public Agent {
public:
    struct Params {
        Price    half_spread    = 2;
        Price    skew_per_lot   = 1;
        Quantity quote_qty      = 10;
        int64_t  max_inventory  = 50;
        Price    stale_threshold= 3;
    };

    // No-params overload
    MarketMakerAgent(ClientId cid, InstrumentId iid,
                     OrderGateway* gw, SimClock* clk)
        : MarketMakerAgent(cid, iid, gw, clk, Params{}) {}

    MarketMakerAgent(ClientId cid, InstrumentId iid,
                     OrderGateway* gw, SimClock* clk, Params p)
        : Agent(cid, iid, gw, clk), params_(p) {}

    [[nodiscard]] std::string_view name() const noexcept override {
        return "MarketMaker";
    }

    [[nodiscard]] OrderId bid_order_id() const noexcept { return bid_order_id_; }
    [[nodiscard]] OrderId ask_order_id() const noexcept { return ask_order_id_; }
    [[nodiscard]] bool    quoting_bid()  const noexcept { return has_bid_; }
    [[nodiscard]] bool    quoting_ask()  const noexcept { return has_ask_; }

    void on_market_data(const MarketDataUpdate& mdu) override {
        if (mdu.instrument_id != instrument_id_) return;

        if (mdu.type == MarketDataUpdate::Type::L1Quote) {
            Price bid = mdu.l1.bid_px;
            Price ask = mdu.l1.ask_px;

            if (bid == INVALID_PRICE || ask == INVALID_PRICE) return;

            Price mid = (bid + ask) / 2;
            last_mid_ = mid;
            mark_to_market(mid);

            // Requote if deferred fill happened, mid moved, or no quotes
            if (needs_requote_
                || std::abs(mid - last_quoted_mid_) >= params_.stale_threshold
                || !has_bid_ || !has_ask_) {
                requote(mid);
                last_quoted_mid_ = mid;
            }
        }

        if (mdu.type == MarketDataUpdate::Type::Trade) {
            last_trade_px_ = mdu.trade.price;
        }
    }

    void on_exec_report(const ExecutionReport& er) override {
        // Bind the engine-assigned id from the New ack. submit_limit() stamps
        // a local counter onto OrderRequest::order_id; the engine ignores that
        // field on New and issues a global id. Cancels look up the engine id.
        if (er.exec_type == ExecutionReport::ExecType::New) {
            if (er.side == Side::Buy) {
                bid_order_id_ = er.order_id;
                has_bid_ = true;
            } else {
                ask_order_id_ = er.order_id;
                has_ask_ = true;
            }
            return;
        }

        if (er.exec_type == ExecutionReport::ExecType::Cancelled ||
            er.exec_type == ExecutionReport::ExecType::Rejected) {
            if (er.order_id == bid_order_id_) {
                has_bid_ = false;
                bid_order_id_ = 0;
            }
            if (er.order_id == ask_order_id_) {
                has_ask_ = false;
                ask_order_id_ = 0;
            }
            return;
        }

        if (!er.is_fill()) return;

        record_fill(er.side, er.last_qty, er.last_px);

        if (er.leaves_qty == 0) {
            if (er.order_id == bid_order_id_ || er.side == Side::Buy) {
                has_bid_ = false;
                bid_order_id_ = 0;
            }
            if (er.order_id == ask_order_id_ || er.side == Side::Sell) {
                has_ask_ = false;
                ask_order_id_ = 0;
            }
        }

        // Mark dirty: do NOT call requote() here.
        needs_requote_ = true;
    }

private:
    void requote(Price mid) {
        // Guard against reentrant calls
        if (in_requote_) return;
        in_requote_ = true;
        needs_requote_ = false;

        // Inventory skew: push quotes away from current position
        Price skew = static_cast<Price>(net_position_) * params_.skew_per_lot;

        Price new_bid = mid - params_.half_spread - skew;
        Price new_ask = mid + params_.half_spread - skew;

        // Cancel existing quotes
        if (has_bid_ && bid_order_id_ != 0) {
            cancel_order(bid_order_id_);
        }
        if (has_ask_ && ask_order_id_ != 0) {
            cancel_order(ask_order_id_);
        }

        if (net_position_ < params_.max_inventory) {
            submit_limit(Side::Buy, std::max(new_bid, Price{1}), params_.quote_qty);
        }
        if (net_position_ > -params_.max_inventory) {
            submit_limit(Side::Sell, std::max(new_ask, Price{1}), params_.quote_qty);
        }

        in_requote_ = false;
    }

    Params   params_;
    Price    last_mid_{0};
    Price    last_quoted_mid_{0};
    Price    last_trade_px_{0};

    OrderId  bid_order_id_{0};
    OrderId  ask_order_id_{0};
    bool     has_bid_{false};
    bool     has_ask_{false};
    bool     needs_requote_{false};
    bool     in_requote_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
//  NoiseTraderAgent
//
//  Submits random market orders to generate turnover.
//  Follows a Poisson arrival process.
// ─────────────────────────────────────────────────────────────────────────────
class NoiseTraderAgent : public Agent {
public:
    NoiseTraderAgent(ClientId cid, InstrumentId iid,
                     OrderGateway* gw, SimClock* clk,
                     double arrival_rate_per_sec = 2.0,
                     uint64_t seed = 42)
        : Agent(cid, iid, gw, clk)
        , rng_(seed)
        , arrival_dist_(arrival_rate_per_sec / 1e9)  // per nanosecond
        , side_dist_(0, 1)
        , qty_dist_(1, 5)
    {}

    [[nodiscard]] std::string_view name() const noexcept override {
        return "NoiseTrader";
    }

    void on_market_data(const MarketDataUpdate& mdu) override {
        if (mdu.instrument_id != instrument_id_) return;
        if (mdu.type != MarketDataUpdate::Type::L1Quote) return;
        if (in_action_) return;   // reentrant guard

        // For continuous sim: trade with probability p per tick
        if (std::uniform_real_distribution<>(0.0, 1.0)(rng_) < 0.03) {
            in_action_ = true;
            Side     side = side_dist_(rng_) ? Side::Buy : Side::Sell;
            Quantity qty  = static_cast<Quantity>(qty_dist_(rng_));
            submit_market(side, qty);
            in_action_ = false;
        }
    }

    void on_exec_report(const ExecutionReport& er) override {
        if (!er.is_fill()) return;
        record_fill(er.side, er.last_qty, er.last_px);
    }

private:
    std::mt19937_64 rng_;
    std::exponential_distribution<double> arrival_dist_;
    std::uniform_int_distribution<int>    side_dist_;
    std::uniform_int_distribution<int>    qty_dist_;
    bool in_action_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
//  MomentumAgent
//
//  Trades in the direction of recent price moves.
//  Uses a short exponential moving average vs long EMA crossover.
// ─────────────────────────────────────────────────────────────────────────────
class MomentumAgent : public Agent {
public:
    MomentumAgent(ClientId cid, InstrumentId iid,
                  OrderGateway* gw, SimClock* clk,
                  int fast_period = 5, int slow_period = 20,
                  Quantity size = 5)
        : Agent(cid, iid, gw, clk)
        , fast_alpha_(2.0 / (fast_period + 1))
        , slow_alpha_(2.0 / (slow_period + 1))
        , trade_size_(size)
    {}

    [[nodiscard]] std::string_view name() const noexcept override {
        return "MomentumAgent";
    }

    void on_market_data(const MarketDataUpdate& mdu) override {
        if (mdu.instrument_id != instrument_id_) return;
        if (mdu.type != MarketDataUpdate::Type::Trade) return;

        double px = price_to_double(mdu.trade.price);
        ++tick_count_;

        if (tick_count_ == 1) {
            fast_ema_ = slow_ema_ = px;
            return;
        }

        fast_ema_ = fast_alpha_ * px + (1.0 - fast_alpha_) * fast_ema_;
        slow_ema_ = slow_alpha_ * px + (1.0 - slow_alpha_) * slow_ema_;

        if (tick_count_ < 20) return;  // warm-up period

        bool bullish = fast_ema_ > slow_ema_ + price_to_double(1);
        bool bearish = fast_ema_ < slow_ema_ - price_to_double(1);

        if (bullish && net_position_ <= 0 && !in_cooldown_ && !in_action_) {
            in_action_ = true;
            submit_market(Side::Buy, trade_size_);
            in_action_ = false;
            in_cooldown_ = true;
            cooldown_ticks_ = 10;
        } else if (bearish && net_position_ >= 0 && !in_cooldown_ && !in_action_) {
            in_action_ = true;
            submit_market(Side::Sell, trade_size_);
            in_action_ = false;
            in_cooldown_ = true;
            cooldown_ticks_ = 10;
        }

        if (in_cooldown_) {
            if (--cooldown_ticks_ <= 0) in_cooldown_ = false;
        }
    }

    void on_exec_report(const ExecutionReport& er) override {
        if (!er.is_fill()) return;
        record_fill(er.side, er.last_qty, er.last_px);
    }

private:
    double   fast_alpha_, slow_alpha_;
    double   fast_ema_{0.0}, slow_ema_{0.0};
    Quantity trade_size_;
    uint64_t tick_count_{0};
    bool     in_cooldown_{false};
    int      cooldown_ticks_{0};
    bool     in_action_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
//  LatencyArbitrageAgent
//
//  Watches for stale quotes (price has moved but old quote still on book).
//  Attempts to snipe the stale order before the maker can cancel it.
// ─────────────────────────────────────────────────────────────────────────────
class LatencyArbAgent : public Agent {
public:
    LatencyArbAgent(ClientId cid, InstrumentId iid,
                    OrderGateway* gw, SimClock* clk,
                    Price staleness_threshold = 3,
                    Quantity size = 10)
        : Agent(cid, iid, gw, clk)
        , threshold_(staleness_threshold)
        , size_(size)
    {}

    [[nodiscard]] std::string_view name() const noexcept override {
        return "LatencyArb";
    }

    void on_market_data(const MarketDataUpdate& mdu) override {
        if (mdu.instrument_id != instrument_id_) return;
        if (mdu.type != MarketDataUpdate::Type::Trade) return;

        Price last_px = mdu.trade.price;

        // If a trade just happened well above best ask, the ask was stale
        if (last_bid_ > 0 && last_ask_ > 0) {
            if (last_px - last_ask_ >= threshold_) {
                // Stale ask detected: buy aggressively
                submit_market(Side::Buy, size_);
            } else if (last_bid_ - last_px >= threshold_) {
                // Stale bid: sell aggressively
                submit_market(Side::Sell, size_);
            }
        }
    }

    void on_market_data_l1(Price bid, Price ask) noexcept {
        last_bid_ = bid;
        last_ask_ = ask;
    }

    void on_exec_report(const ExecutionReport& er) override {
        if (!er.is_fill()) return;
        record_fill(er.side, er.last_qty, er.last_px);
    }

private:
    Price    threshold_;
    Quantity size_;
    Price    last_bid_{0};
    Price    last_ask_{0};
};

} // namespace hydra
