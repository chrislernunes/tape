#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Execution Simulation Models
//
//  Critical for realistic strategy testing. Models:
//    1. Queue position tracking — where does your order sit in the queue?
//    2. Fill probability — given position, what's prob of fill on a move?
//    3. Partial fill logic — how much gets filled on each event?
//    4. Market impact — price impact of your own order flow
// ─────────────────────────────────────────────────────────────────────────────

#include "../../include/hydra/types.hpp"
#include "../../engine/orderbook/limit_order_book.cpp"

#include <cmath>
#include <random>
#include <algorithm>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  QueuePositionModel
//
//  Tracks an order's position in the price level queue.
//  Models the key microstructure question: "are you filled on a touch?"
//
//  Rule: you fill only if the cumulative volume that trades at your
//  level exceeds the quantity ahead of you in queue.
//
//  fill_probability = max(0, vol_at_level - qty_ahead) / order_qty
// ─────────────────────────────────────────────────────────────────────────────
class QueuePositionModel {
public:
    struct Position {
        Quantity qty_ahead{0};    // competing liquidity ahead in queue
        Quantity qty_behind{0};   // competing liquidity behind
        Quantity order_qty{0};    // this order's size
        Price    price{0};

        // Probability of full fill given `volume` traded at level
        [[nodiscard]] double fill_probability(Quantity volume) const noexcept {
            if (qty_ahead >= volume) return 0.0;
            double fillable = static_cast<double>(volume - qty_ahead);
            return std::min(fillable / static_cast<double>(order_qty), 1.0);
        }

        // Expected fill qty given `volume` traded at this level
        [[nodiscard]] Quantity expected_fill(Quantity volume) const noexcept {
            if (qty_ahead >= volume) return 0;
            Quantity available = volume - qty_ahead;
            return std::min(available, order_qty);
        }
    };

    // Snapshot position when order enters queue
    [[nodiscard]] Position snapshot(const LimitOrderBook& book,
                                    Price price, Side side,
                                    Quantity my_qty) const noexcept {
        Position pos;
        pos.price     = price;
        pos.order_qty = my_qty;

        // Qty ahead = existing qty at this level (we arrive after them)
        if (side == Side::Buy) {
            auto snap = book.snapshot(0, 50);
            for (auto& lvl : snap.bids) {
                if (lvl.price == price) {
                    pos.qty_ahead = lvl.qty;  // we're at the back
                    break;
                }
            }
        } else {
            auto snap = book.snapshot(0, 50);
            for (auto& lvl : snap.asks) {
                if (lvl.price == price) {
                    pos.qty_ahead = lvl.qty;
                    break;
                }
            }
        }

        return pos;
    }

    // Update position as trades consume liquidity ahead of us
    static void update_fill(Position& pos, Quantity traded_at_level) noexcept {
        if (traded_at_level <= pos.qty_ahead) {
            pos.qty_ahead -= traded_at_level;
        } else {
            pos.qty_ahead = 0;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  PartialFillModel
//
//  In real markets, fills can be partial even when volume exists.
//  This models the "last trade" size distribution and whether
//  your order gets full or partial fill.
// ─────────────────────────────────────────────────────────────────────────────
class PartialFillModel {
public:
    explicit PartialFillModel(uint64_t seed = 777)
        : rng_(seed), uniform_(0.0, 1.0) {}

    // Given fill opportunity, return actual fill qty
    // size_dist_alpha: shape of Pareto (trade size distribution)
    [[nodiscard]] Quantity simulate_fill(Quantity max_fillable,
                                         double fill_prob = 1.0) noexcept {
        if (uniform_(rng_) > fill_prob) return 0;
        // Most fills are full; partial fill probability declines with size
        if (max_fillable <= 1) return max_fillable;

        // Use beta distribution to model fill fraction
        // For simplicity: 80% chance of full fill, 20% partial
        if (uniform_(rng_) < 0.80) return max_fillable;
        double frac = uniform_(rng_);  // uniform partial
        return static_cast<Quantity>(std::ceil(frac * max_fillable));
    }

private:
    std::mt19937_64             rng_;
    std::uniform_real_distribution<double> uniform_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  MarketImpactModel
//
//  Estimate price impact of a market order or aggressive limit.
//
//  Uses Almgren-Chriss square-root law:
//    impact = η * σ * sqrt(Q / ADV)
//
//  where:
//    η   = permanent impact coefficient (~0.1 for liquid futures)
//    σ   = daily volatility (in price ticks)
//    Q   = order size
//    ADV = average daily volume
// ─────────────────────────────────────────────────────────────────────────────
class MarketImpactModel {
public:
    struct ImpactParams {
        double eta         = 0.10;
        double sigma_ticks = 50.0;
        double adv         = 1'000'000.0;
        double temp_coeff  = 0.05;
    };

    MarketImpactModel() noexcept : params_(ImpactParams{}) {}
    explicit MarketImpactModel(ImpactParams p) noexcept : params_(p) {}

    // Permanent price impact in ticks
    [[nodiscard]] double permanent_impact_ticks(Quantity qty) const noexcept {
        return params_.eta * params_.sigma_ticks
             * std::sqrt(static_cast<double>(qty) / params_.adv);
    }

    // Temporary impact (realized price slippage per unit)
    [[nodiscard]] double temporary_impact_ticks(Quantity qty,
                                                 double participation_rate) const noexcept {
        return params_.temp_coeff * params_.sigma_ticks
             * std::sqrt(participation_rate)
             * std::sqrt(static_cast<double>(qty) / params_.adv);
    }

    // Total slippage in ticks for a single child order
    [[nodiscard]] double total_impact_ticks(Quantity qty) const noexcept {
        return permanent_impact_ticks(qty) + temporary_impact_ticks(qty, 0.05);
    }

    // Adjusted fill price given impact
    [[nodiscard]] Price adjusted_fill_price(Price mid, Side side,
                                            Quantity qty) const noexcept {
        double impact = total_impact_ticks(qty);
        if (side == Side::Buy)
            return mid + static_cast<Price>(impact + 0.5);
        else
            return mid - static_cast<Price>(impact + 0.5);
    }

    // VWAP estimate for executing qty over N periods
    [[nodiscard]] double vwap_cost_ticks(Quantity total_qty,
                                          int n_periods) const noexcept {
        double perm  = permanent_impact_ticks(total_qty);
        double child = static_cast<double>(total_qty) / n_periods;
        double temp  = temporary_impact_ticks(static_cast<Quantity>(child), 0.02);
        return perm + temp * n_periods;
    }

private:
    ImpactParams params_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  CancellationRaceModel
//
//  Models the race between a cancel request and a fill.
//  In real markets, cancel requests can arrive "too late" if the
//  matching engine has already matched the order.
//
//  cancel_success_prob = P(cancel reaches engine before fill)
//                      ≈ 1 - P(fill_latency < cancel_latency)
// ─────────────────────────────────────────────────────────────────────────────
class CancellationRaceModel {
public:
    CancellationRaceModel(double cancel_latency_us = 45.0,
                          double fill_latency_us   = 42.0,
                          double jitter_us         = 5.0,
                          uint64_t seed            = 999)
        : rng_(seed)
        , cancel_dist_(cancel_latency_us * 1000.0, jitter_us * 1000.0)
        , fill_dist_(fill_latency_us * 1000.0, jitter_us * 1000.0)
    {}

    // Returns true if cancel wins the race (order successfully cancelled)
    [[nodiscard]] bool cancel_wins() noexcept {
        double t_cancel = std::max(cancel_dist_(rng_), 1000.0);
        double t_fill   = std::max(fill_dist_(rng_), 1000.0);
        return t_cancel < t_fill;
    }

    [[nodiscard]] double cancel_success_probability() const noexcept {
        // Closed form: P(C < F) where both ~ Normal
        // P(C - F < 0) = Φ(−(µC − µF) / sqrt(σC² + σF²))
        double mu_diff = cancel_dist_.mean() - fill_dist_.mean();
        double sig     = std::sqrt(cancel_dist_.stddev() * cancel_dist_.stddev()
                                 + fill_dist_.stddev() * fill_dist_.stddev());
        return 0.5 * std::erfc(mu_diff / (sig * std::sqrt(2.0)));
    }

private:
    std::mt19937_64 rng_;
    std::normal_distribution<double> cancel_dist_;
    std::normal_distribution<double> fill_dist_;
};

} // namespace hydra
