#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  TapeSimulation: top-level orchestrator
//
//  Wires together:
//    SimClock → MatchingEngine → OrderGateway → Agents → Analytics
//
//  The main loop:
//    1. Pop next event from calendar (time-ordered priority queue)
//    2. Advance sim clock to event time
//    3. Deliver event to agent or process OrderRequest
//    4. Route market data to all agents
//    5. Collect analytics
//    6. Repeat
// ─────────────────────────────────────────────────────────────────────────────

#include "tape/matching_engine.hpp"
#include "tape/gateway.hpp"
#include "tape/risk.hpp"
#include "tape/agents.hpp"
#include "tape/analytics.hpp"
#include "tape/synthetic_flow.hpp"
#include "tape/timestamp.hpp"

#include <vector>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <functional>

namespace tape {

// ─────────────────────────────────────────────────────────────────────────────
//  SimulationConfig
// ─────────────────────────────────────────────────────────────────────────────
struct SimulationConfig {
    double    duration_seconds   = 60.0;
    int       num_market_makers  = 3;
    int       num_noise_traders  = 10;
    int       num_momentum_agents = 2;
    bool      enable_latency_model = true;
    bool      verbose             = false;
    InstrumentId instrument_id    = 1;

    // Market maker params
    MarketMakerAgent::Params mm_params;

    // Risk limits
    RiskLimits risk_limits;

    // Synthetic flow
    CSTParams cst_params;
};

// ─────────────────────────────────────────────────────────────────────────────
//  TapeSimulation
// ─────────────────────────────────────────────────────────────────────────────
class TapeSimulation {
public:
    TapeSimulation()
        : TapeSimulation(SimulationConfig{}) {}

    explicit TapeSimulation(SimulationConfig cfg)
        : cfg_(std::move(cfg))
        , sim_clock_(0)
        , risk_(cfg_.risk_limits)
        , engine_()
        , gateway_(&engine_, &risk_, &sim_clock_)
        , analytics_(cfg_.instrument_id)
    {
        // Register instrument
        engine_.add_instrument(cfg_.instrument_id);
        engine_.set_market_state(cfg_.instrument_id, MarketState::Open);
        engine_.set_clock(&sim_clock_);

        // Wire exec reports back through gateway
        engine_.set_exec_report_cb([this](const ExecutionReport& er) {
            gateway_.on_exec_report(er);
        });

        // Wire market data to analytics + agents.
        // Agents must not be re-entered from inside their own submit():
        // process → publish_l1 → on_market_data → requote → process → … is
        // unbounded once cancels actually reach the book. Analytics still
        // sees every event.
        engine_.set_market_data_cb([this](const MarketDataUpdate& mdu) {
            dispatch_market_data(mdu);
        });

        // Wire exec reports to agents
        gateway_.set_exec_report_cb([this](ClientId cid, const ExecutionReport& er) {
            for (auto& agent : agents_) {
                if (agent->client_id() == cid)
                    agent->on_exec_report(er);
            }
        });
    }

    // ── Add agents ────────────────────────────────────────────────────────────
    void add_agent(std::unique_ptr<Agent> a) {
        agents_.push_back(std::move(a));
    }

    // ── Build default agent population ────────────────────────────────────────
    void build_default_agents() {
        ClientId next_cid = 1;

        // Market makers
        for (int i = 0; i < cfg_.num_market_makers; ++i) {
            auto mm_p = cfg_.mm_params;
            mm_p.half_spread = cfg_.mm_params.half_spread + static_cast<Price>(i);
            agents_.push_back(std::make_unique<MarketMakerAgent>(
                next_cid++, cfg_.instrument_id, &gateway_, &sim_clock_, mm_p));
        }

        // Noise traders
        for (int i = 0; i < cfg_.num_noise_traders; ++i) {
            agents_.push_back(std::make_unique<NoiseTraderAgent>(
                next_cid++, cfg_.instrument_id, &gateway_, &sim_clock_,
                2.0, static_cast<uint64_t>(42 + i)));
        }

        // Momentum agents
        for (int i = 0; i < cfg_.num_momentum_agents; ++i) {
            agents_.push_back(std::make_unique<MomentumAgent>(
                next_cid++, cfg_.instrument_id, &gateway_, &sim_clock_,
                5 + i * 3, 20 + i * 5));
        }

        agents_.push_back(std::make_unique<LatencyArbAgent>(
            next_cid++, cfg_.instrument_id, &gateway_, &sim_clock_));
    }

    [[nodiscard]] MatchingEngine&       engine()    noexcept { return engine_; }
    [[nodiscard]] const MatchingEngine& engine()    const noexcept { return engine_; }
    [[nodiscard]] InstrumentAnalytics&  analytics() noexcept { return analytics_; }
    [[nodiscard]] std::size_t           agent_count() const noexcept { return agents_.size(); }
    Agent* agent_at(std::size_t i) noexcept {
        return i < agents_.size() ? agents_[i].get() : nullptr;
    }

    // ── Run the simulation ────────────────────────────────────────────────────
    void run() {
        int64_t start_mono = Clock::now_mono();

        if (agents_.empty()) build_default_agents();

        // ── Bootstrap: seed the book with initial orders ──────────────────────
        seed_initial_book();

        // ── Generate synthetic order flow ─────────────────────────────────────
        auto& cst_p = cfg_.cst_params;
        cst_p.sim_seconds = cfg_.duration_seconds;
        SyntheticOrderFlowGenerator gen(cst_p);
        auto events = gen.generate();

        if (cfg_.verbose)
            std::printf("[TAPE] Generated %zu synthetic events over %.1fs\n",
                        events.size(), cfg_.duration_seconds);

        // ── Main event loop ───────────────────────────────────────────────────
        uint64_t event_count = 0;
        for (auto& ev : events) {
            sim_clock_.set(ev.time);

            // Agents observe current book state periodically
            if (event_count % 100 == 0) {
                publish_l1_snapshot();
            }

            // CST cancels arrive with order_id=0. Bind to the live touch
            // so we cancel an actual resting order owned by its real client.
            if (ev.request.action == OrderRequest::Action::Cancel &&
                ev.request.order_id == 0) {
                auto* bk = engine_.book(ev.request.instrument_id);
                Order* live = bk ? bk->oldest_at_best(ev.request.side) : nullptr;
                if (!live) continue;
                ev.request.order_id  = live->id;
                ev.request.client_id = live->client_id;
            }

            gateway_.submit(ev.request);
            ++event_count;
        }

        // ── Final L1 tick so agents see end state ─────────────────────────────
        publish_l1_snapshot();

        int64_t elapsed_ns  = Clock::now_mono() - start_mono;
        double  wall_sec    = static_cast<double>(elapsed_ns) / 1e9;
        double  sim_sec     = static_cast<double>(sim_clock_.now()) / 1e9;
        double  speedup     = sim_sec / wall_sec;

        // ── Results ───────────────────────────────────────────────────────────
        std::printf("\n╔════════════════════════════════════════════╗\n");
        std::printf("║         Tape Simulation           ║\n");
        std::printf("╚════════════════════════════════════════════╝\n");
        std::printf("\n  Sim time:    %.2f seconds\n", sim_sec);
        std::printf("  Wall time:   %.3f seconds\n", wall_sec);
        std::printf("  Speedup:     %.0fx real-time\n", speedup);
        std::printf("  Events:      %llu\n", static_cast<unsigned long long>(event_count));
        std::printf("  Throughput:  %.2f M msg/sec\n",
                    static_cast<double>(event_count) / wall_sec / 1e6);
        std::printf("  Orders:      %llu\n",
                    static_cast<unsigned long long>(engine_.total_orders()));
        std::printf("  Trades:      %llu\n",
                    static_cast<unsigned long long>(engine_.total_trades()));
        std::printf("  Cancels:     %llu\n",
                    static_cast<unsigned long long>(engine_.total_cancels()));

        // Book state
        auto* bk = engine_.book(cfg_.instrument_id);
        if (bk) {
            std::printf("\n  Book State:  bid=%lld  ask=%lld  spread=%lld\n",
                        static_cast<long long>(bk->best_bid()),
                        static_cast<long long>(bk->best_ask()),
                        static_cast<long long>(bk->spread()));
            std::printf("  Open Orders: %zu  (bid_levels=%zu, ask_levels=%zu)\n",
                        bk->total_orders(), bk->bid_levels(), bk->ask_levels());
        }

        // Agent PnL
        std::printf("\n  Agent PnL:\n");
        for (auto& agent : agents_) {
            std::printf("    %-20s  pos=%+5lld  rpnl=%+10.2f  upnl=%+10.2f\n",
                        std::string(agent->name()).c_str(),
                        static_cast<long long>(agent->net_position()),
                        agent->realized_pnl(),
                        agent->unrealized_pnl());
        }

        // Analytics
        analytics_.print_summary("INST-1");
    }

private:
    // Seed initial book state so agents have a quote to respond to
    void seed_initial_book() {
        Price mid = cfg_.cst_params.initial_mid;
        Quantity qty = cfg_.cst_params.initial_qty_per_level;
        int levels   = std::min(cfg_.cst_params.num_levels, 5);

        ClientId seed_client = 99;  // special seeder client
        uint64_t oid = 900000000;

        for (int i = 0; i < levels; ++i) {
            OrderRequest bid_req;
            bid_req.action       = OrderRequest::Action::New;
            bid_req.order_id     = oid++;
            bid_req.client_id    = seed_client;
            bid_req.instrument_id = cfg_.instrument_id;
            bid_req.side         = Side::Buy;
            bid_req.type         = OrderType::Limit;
            bid_req.tif          = TimeInForce::GTC;
            bid_req.price        = mid - 1 - static_cast<Price>(i);
            bid_req.quantity     = qty;
            engine_.process(bid_req);

            OrderRequest ask_req = bid_req;
            ask_req.order_id     = oid++;
            ask_req.side         = Side::Sell;
            ask_req.price        = mid + 1 + static_cast<Price>(i);
            engine_.process(ask_req);
        }
    }

    void publish_l1_snapshot() {
        auto* bk = engine_.book(cfg_.instrument_id);
        if (!bk) return;

        MarketDataUpdate mdu;
        mdu.type          = MarketDataUpdate::Type::L1Quote;
        mdu.instrument_id = cfg_.instrument_id;
        mdu.timestamp     = sim_clock_.now();
        mdu.l1.bid_px     = bk->best_bid();
        mdu.l1.bid_qty    = bk->best_bid_qty();
        mdu.l1.ask_px     = bk->best_ask();
        mdu.l1.ask_qty    = bk->best_ask_qty();

        dispatch_market_data(mdu);
    }

    void dispatch_market_data(const MarketDataUpdate& mdu) {
        analytics_.on_market_data(mdu);
        if (dispatching_md_) return;
        dispatching_md_ = true;
        Price mid = 0;
        if (mdu.type == MarketDataUpdate::Type::L1Quote &&
            mdu.l1.bid_px != INVALID_PRICE && mdu.l1.ask_px != INVALID_PRICE) {
            mid = (mdu.l1.bid_px + mdu.l1.ask_px) / 2;
        }
        for (auto& agent : agents_) {
            if (mid != 0) agent->mark_to_market(mid);
            agent->on_market_data(mdu);
        }
        dispatching_md_ = false;
    }

    SimulationConfig        cfg_;
    SimClock                sim_clock_;
    RiskEngine              risk_;
    MatchingEngine          engine_;
    OrderGateway            gateway_;
    InstrumentAnalytics     analytics_;

    std::vector<std::unique_ptr<Agent>> agents_;
    bool dispatching_md_{false};
};

} // namespace tape
