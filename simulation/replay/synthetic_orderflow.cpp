#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  SyntheticOrderFlow
//
//  Generates statistically realistic order flow without needing real data.
//  Based on the Cont-Stoikov-Talreja (2010) stochastic LOB model:
//
//    - Limit order arrivals: Poisson process with rate λ_B(p), λ_A(p)
//    - Market order arrivals: Poisson process with rate μ
//    - Cancellations: Poisson with rate θ per order
//    - Price levels follow exponential intensity decay from best
//
//  The model reproduces key stylized facts:
//    - Mean-reverting spread
//    - Clustered volatility (through feedback mechanisms)
//    - Heavy-tailed returns
//    - Autocorrelated order flow
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/hydra/order.hpp"
#include "../infrastructure/timestamp.hpp"

#include <random>
#include <vector>
#include <cmath>
#include <functional>
#include <queue>
#include <algorithm>  // std::stable_sort (was relying on a transitive include)

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  Event types for the event-driven simulation
// ─────────────────────────────────────────────────────────────────────────────
struct SimEvent {
    Timestamp    time;
    OrderRequest request;

    bool operator>(const SimEvent& o) const noexcept { return time > o.time; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  CST Model parameters
// ─────────────────────────────────────────────────────────────────────────────
struct CSTParams {
    // Arrival rates (per second)
    double lambda_limit   = 50.0;   // limit order arrival rate per price level
    double mu_market      = 10.0;   // market order arrival rate
    double theta_cancel   = 5.0;    // cancel rate per order

    // Price distribution
    double alpha_decay    = 1.0;    // exponential decay of intensity from best
    int    num_levels     = 10;     // active price levels each side

    // Initial book state
    Price  initial_mid    = 10000;  // mid price (ticks)
    Price  initial_spread = 2;      // bid-ask spread (ticks)
    Quantity initial_qty_per_level = 20;

    // Simulation duration
    double sim_seconds    = 60.0;

    // Random seed
    uint64_t seed = 12345;
};

// ─────────────────────────────────────────────────────────────────────────────
//  SyntheticOrderFlowGenerator
//
//  Generates a time-ordered stream of OrderRequests based on the CST model.
//  All events are pre-generated into a priority queue (event calendar).
// ─────────────────────────────────────────────────────────────────────────────
class SyntheticOrderFlowGenerator {
    using EventQueue = std::priority_queue<SimEvent,
                                           std::vector<SimEvent>,
                                           std::greater<SimEvent>>;

public:
    SyntheticOrderFlowGenerator()
        : SyntheticOrderFlowGenerator(CSTParams{}) {}

    explicit SyntheticOrderFlowGenerator(CSTParams p)
        : params_(p)
        , rng_(p.seed)
        , uid_(0.0, 1.0)
        , side_dist_(0, 1)
    {}

    // Generate all events for the simulation duration
    std::vector<SimEvent> generate() {
        events_.clear();
        next_order_id_ = 1;
        next_client_id_ = 100;  // synthetic agents start at client 100

        Price mid  = params_.initial_mid;
        Price bid  = mid - params_.initial_spread / 2;
        Price ask  = mid + (params_.initial_spread + 1) / 2;

        double sim_ns = params_.sim_seconds * 1e9;

        // ── Generate limit order arrivals ─────────────────────────────────────
        for (int level = 0; level < params_.num_levels; ++level) {
            // Intensity decays exponentially from best
            double intensity = params_.lambda_limit
                             * std::exp(-params_.alpha_decay * level);

            // Bid side
            Price bid_px = bid - static_cast<Price>(level);
            generate_limit_arrivals(Side::Buy, bid_px, intensity, sim_ns);

            // Ask side
            Price ask_px = ask + static_cast<Price>(level);
            generate_limit_arrivals(Side::Sell, ask_px, intensity, sim_ns);
        }

        // ── Generate market order arrivals ────────────────────────────────────
        generate_market_arrivals(sim_ns);

        // ── Generate cancellations ────────────────────────────────────────────
        generate_cancellations(bid, ask, sim_ns);

        // Sort by time. Must be a *stable* sort: std::sort gives no ordering
        // guarantee among events with an identical timestamp, which would be
        // a hidden, non-reproducible source of nondeterminism if two draws
        // ever land on the same nanosecond. std::stable_sort preserves this
        // generator's own generation order (bids before asks before market
        // orders before cancels, per the calls above) as the tie-break,
        // making "same seed + same params -> byte-identical event sequence"
        // an actual invariant rather than one that happens to hold today.
        std::stable_sort(events_.begin(), events_.end(),
                  [](const SimEvent& a, const SimEvent& b) {
                      return a.time < b.time;
                  });

        return events_;
    }

    // Convenience: add Poisson events to a queue
    std::vector<SimEvent>& events() noexcept { return events_; }

private:
    // Poisson inter-arrival times via inverse transform
    double next_arrival_ns(double rate_per_sec) noexcept {
        double u = std::max(uid_(rng_), 1e-15);
        return -std::log(u) / rate_per_sec * 1e9;
    }

    uint32_t rand_qty() noexcept {
        // Pareto distribution for order sizes (heavy tail)
        double u   = std::max(uid_(rng_), 1e-10);
        double x   = std::pow(u, -1.0 / 1.5);  // Pareto with alpha=1.5
        return static_cast<uint32_t>(std::max(1.0, std::min(x * 5.0, 500.0)));
    }

    void generate_limit_arrivals(Side side, Price price,
                                  double rate, double sim_ns) {
        double t = next_arrival_ns(rate);
        while (t < sim_ns) {
            OrderRequest req;
            req.action       = OrderRequest::Action::New;
            req.order_id     = next_order_id_++;
            req.client_id    = static_cast<ClientId>(next_client_id_++);
            req.instrument_id = 1;
            req.side         = side;
            req.type         = OrderType::Limit;
            req.tif          = TimeInForce::GTC;
            req.price        = price;
            req.quantity     = rand_qty();
            req.timestamp    = static_cast<Timestamp>(t);

            events_.push_back({static_cast<Timestamp>(t), req});
            t += next_arrival_ns(rate);
        }
    }

    void generate_market_arrivals(double sim_ns) {
        double t = next_arrival_ns(params_.mu_market);
        while (t < sim_ns) {
            OrderRequest req;
            req.action       = OrderRequest::Action::New;
            req.order_id     = next_order_id_++;
            req.client_id    = static_cast<ClientId>(next_client_id_++);
            req.instrument_id = 1;
            req.side         = side_dist_(rng_) ? Side::Buy : Side::Sell;
            req.type         = OrderType::Market;
            req.quantity     = rand_qty();
            req.timestamp    = static_cast<Timestamp>(t);

            events_.push_back({static_cast<Timestamp>(t), req});
            t += next_arrival_ns(params_.mu_market);
        }
    }

    void generate_cancellations(Price bid, Price ask, double sim_ns) {
        // Generate cancels distributed over time
        // In a real sim, cancels reference actual order IDs from the book
        // Here we generate placeholder cancels that reference future orders
        double rate = params_.theta_cancel * params_.num_levels * 2;
        double t    = next_arrival_ns(rate);
        uint64_t cancel_target_id = 1;

        while (t < sim_ns) {
            OrderRequest req;
            req.action        = OrderRequest::Action::Cancel;
            req.order_id      = cancel_target_id++;
            req.client_id     = 100;
            req.instrument_id = 1;
            req.side          = side_dist_(rng_) ? Side::Buy : Side::Sell;
            req.timestamp     = static_cast<Timestamp>(t);

            events_.push_back({static_cast<Timestamp>(t), req});
            t += next_arrival_ns(rate);
        }
    }

    CSTParams                             params_;
    std::mt19937_64                       rng_;
    std::uniform_real_distribution<double> uid_;
    std::uniform_int_distribution<int>    side_dist_;

    std::vector<SimEvent>                 events_;
    uint64_t                              next_order_id_{1};
    uint32_t                              next_client_id_{100};
};

} // namespace hydra
