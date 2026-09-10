//─────────────────────────────────────────────────────────────────────────────
//  HydraExchange — main.cpp
//
//  Entry point. Accepts command-line mode flags:
//    --sim       : Run full agent-based market simulation (default)
//    --bench     : Run throughput/latency benchmarks
//    --example   : Run a simple worked example showing all engine features
//    --microstructure : Run microstructure experiment
//─────────────────────────────────────────────────────────────────────────────

#include "hydra_simulation.hpp"
#include "benchmarks/throughput_benchmark.cpp"
#include "engine/market_data/feed_publisher.cpp"
#include "simulation/execution_model/queue_position.cpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace hydra;

// ─────────────────────────────────────────────────────────────────────────────
//  Example 1: Simple manual matching demonstration
// ─────────────────────────────────────────────────────────────────────────────
void run_simple_example() {
    std::printf("\n╔══════════════════════════════════════════════════╗\n");
    std::printf("║  Example: Manual Order Matching                 ║\n");
    std::printf("╚══════════════════════════════════════════════════╝\n\n");

    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);

    // Wire callbacks
    engine.set_exec_report_cb([](const ExecutionReport& er) {
        const char* type_str = "";
        switch (er.exec_type) {
            case ExecutionReport::ExecType::New:         type_str = "NEW";         break;
            case ExecutionReport::ExecType::PartialFill: type_str = "PARTIAL_FILL"; break;
            case ExecutionReport::ExecType::Fill:        type_str = "FILL";        break;
            case ExecutionReport::ExecType::Cancelled:   type_str = "CANCELLED";   break;
            case ExecutionReport::ExecType::Rejected:    type_str = "REJECTED";    break;
            default: type_str = "?"; break;
        }
        std::printf("  [EXEC] order=%llu  type=%-12s  side=%s  "
                    "last_px=%lld  last_qty=%llu  leaves=%llu\n",
                    static_cast<unsigned long long>(er.order_id),
                    type_str,
                    er.side == Side::Buy ? "BUY " : "SELL",
                    static_cast<long long>(er.last_px),
                    static_cast<unsigned long long>(er.last_qty),
                    static_cast<unsigned long long>(er.leaves_qty));
    });

    engine.set_trade_event_cb([](const TradeEvent& te) {
        std::printf("  [TRADE] trade_id=%llu  px=%lld  qty=%llu  "
                    "aggressor=%s  maker=%llu  taker=%llu\n",
                    static_cast<unsigned long long>(te.trade_id),
                    static_cast<long long>(te.price),
                    static_cast<unsigned long long>(te.quantity),
                    te.aggressor_side == Side::Buy ? "BUY" : "SELL",
                    static_cast<unsigned long long>(te.maker_order_id),
                    static_cast<unsigned long long>(te.taker_order_id));
    });

    // ── Step 1: Post resting limit orders ────────────────────────────────────
    std::printf("Step 1: Post resting limit orders\n");
    std::printf("────────────────────────────────────\n");

    auto send = [&](uint64_t id, bool is_buy, Price px, Quantity qty,
                    OrderType type = OrderType::Limit) {
        OrderRequest req;
        req.action        = OrderRequest::Action::New;
        req.order_id      = id;
        req.client_id     = static_cast<ClientId>(is_buy ? 1 : 2);
        req.instrument_id = 1;
        req.side          = is_buy ? Side::Buy : Side::Sell;
        req.type          = type;
        req.tif           = TimeInForce::GTC;
        req.price         = px;
        req.quantity      = qty;
        req.timestamp     = 0;
        engine.process(req);
    };

    //                  id   buy?  price  qty
    send(1,  true,  9998,  5);   // BUY  5 @ 9998
    send(2,  true,  9999,  10);  // BUY  10 @ 9999
    send(3,  true,  9997,  3);   // BUY  3 @ 9997
    send(4,  false, 10001, 8);   // SELL 8 @ 10001
    send(5,  false, 10002, 12);  // SELL 12 @ 10002
    send(6,  false, 10001, 4);   // SELL 4 @ 10001 (second at same level)

    auto* bk = engine.book(1);
    std::printf("\n  Book state after resting orders:\n");
    std::printf("    Best bid: %lld (%llu qty)\n",
                static_cast<long long>(bk->best_bid()),
                static_cast<unsigned long long>(bk->best_bid_qty()));
    std::printf("    Best ask: %lld (%llu qty)\n",
                static_cast<long long>(bk->best_ask()),
                static_cast<unsigned long long>(bk->best_ask_qty()));
    std::printf("    Spread:   %lld ticks\n\n",
                static_cast<long long>(bk->spread()));

    // ── Step 2: Aggressive buy — partial match ────────────────────────────────
    std::printf("Step 2: Aggressive BUY limit order — crosses ask\n");
    std::printf("──────────────────────────────────────────────────\n");
    send(10, true, 10001, 6);   // BUY 6 @ 10001 — should fill against order 4 (8 lots)

    std::printf("\n  After aggressive buy (6 lots @ 10001):\n");
    std::printf("    Best ask: %lld (%llu qty remaining)\n\n",
                static_cast<long long>(bk->best_ask()),
                static_cast<unsigned long long>(bk->best_ask_qty()));

    // ── Step 3: Market order — sweeps multiple levels ─────────────────────────
    std::printf("Step 3: Market SELL — sweeps bids\n");
    std::printf("──────────────────────────────────\n");

    OrderRequest mkt;
    mkt.action        = OrderRequest::Action::New;
    mkt.order_id      = 20;
    mkt.client_id     = 3;
    mkt.instrument_id = 1;
    mkt.side          = Side::Sell;
    mkt.type          = OrderType::Market;
    mkt.quantity      = 12;  // Will sweep 9999 (10) + 9998 (5) levels
    mkt.timestamp     = 0;
    engine.process(mkt);

    std::printf("\n  After market SELL (12 lots):\n");
    std::printf("    Best bid: %lld  open_orders=%zu\n\n",
                static_cast<long long>(bk->best_bid()),
                bk->total_orders());

    // ── Step 4: IOC order ─────────────────────────────────────────────────────
    std::printf("Step 4: IOC BUY @ 10001 for 20 lots (only 6 available)\n");
    std::printf("────────────────────────────────────────────────────────\n");
    OrderRequest ioc;
    ioc.action        = OrderRequest::Action::New;
    ioc.order_id      = 30;
    ioc.client_id     = 1;
    ioc.instrument_id = 1;
    ioc.side          = Side::Buy;
    ioc.type          = OrderType::Limit;
    ioc.tif           = TimeInForce::IOC;
    ioc.price         = 10001;
    ioc.quantity      = 20;
    ioc.timestamp     = 0;
    engine.process(ioc);

    std::printf("\n  Summary: orders=%llu  trades=%llu\n",
                static_cast<unsigned long long>(engine.total_orders()),
                static_cast<unsigned long long>(engine.total_trades()));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Example 2: Microstructure experiment
//  Measures how spread, OFI, and Kyle's lambda evolve during simulation
// ─────────────────────────────────────────────────────────────────────────────
void run_microstructure_experiment() {
    std::printf("\n╔══════════════════════════════════════════════════╗\n");
    std::printf("║  Microstructure Experiment                      ║\n");
    std::printf("╚══════════════════════════════════════════════════╝\n\n");

    SimulationConfig cfg;
    cfg.duration_seconds     = 120.0;
    cfg.num_market_makers    = 5;
    cfg.num_noise_traders    = 20;
    cfg.num_momentum_agents  = 3;
    cfg.verbose              = true;

    cfg.mm_params.half_spread   = 2;
    cfg.mm_params.quote_qty     = 15;
    cfg.mm_params.max_inventory = 100;

    cfg.cst_params.initial_mid         = 10000;
    cfg.cst_params.initial_spread      = 4;
    cfg.cst_params.lambda_limit        = 30.0;
    cfg.cst_params.mu_market           = 8.0;
    cfg.cst_params.num_levels          = 8;
    cfg.cst_params.initial_qty_per_level = 25;

    HydraSimulation sim(cfg);
    sim.run();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Example 3: Queue position + execution model demo
// ─────────────────────────────────────────────────────────────────────────────
void run_execution_model_demo() {
    std::printf("\n╔══════════════════════════════════════════════════╗\n");
    std::printf("║  Execution Model Demo                           ║\n");
    std::printf("╚══════════════════════════════════════════════════╝\n\n");

    // Queue position
    QueuePositionModel::Position pos;
    pos.qty_ahead  = 100;
    pos.order_qty  = 20;
    pos.price      = 10000;

    std::printf("  Queue Position Model:\n");
    std::printf("    Qty ahead = 100,  order_qty = 20\n");
    for (Quantity vol : {50ULL, 100ULL, 110ULL, 120ULL, 150ULL}) {
        std::printf("    vol_at_level = %4llu  →  fill_prob = %.1f%%  expected_fill = %llu\n",
                    static_cast<unsigned long long>(vol),
                    pos.fill_probability(vol) * 100.0,
                    static_cast<unsigned long long>(pos.expected_fill(vol)));
    }

    // Market impact
    std::printf("\n  Market Impact (Almgren-Chriss sqrt-law):\n");
    MarketImpactModel::ImpactParams params;
    params.eta         = 0.10;
    params.sigma_ticks = 50.0;
    params.adv         = 500'000.0;
    MarketImpactModel impact(params);

    std::printf("    %-12s  %-15s  %-12s\n", "Order Size", "Impact (ticks)", "Adj Price");
    for (Quantity qty : {100ULL, 500ULL, 1000ULL, 5000ULL, 10000ULL}) {
        double imp = impact.total_impact_ticks(qty);
        Price  adj = impact.adjusted_fill_price(10000, Side::Buy, qty);
        std::printf("    %-12llu  %-15.3f  %-12lld\n",
                    static_cast<unsigned long long>(qty), imp,
                    static_cast<long long>(adj));
    }

    // Cancel race
    std::printf("\n  Cancellation Race Model:\n");
    CancellationRaceModel race(45.0, 42.0, 5.0);
    std::printf("    Theoretical cancel success probability: %.1f%%\n",
                race.cancel_success_probability() * 100.0);

    int wins = 0, total = 10000;
    for (int i = 0; i < total; ++i)
        if (race.cancel_wins()) ++wins;
    std::printf("    Simulated  cancel success probability: %.1f%%  (n=%d)\n",
                100.0 * wins / total, total);

    // Latency model
    std::printf("\n  Full Latency Model (1000 samples):\n");
    FullLatencyModel lat_model(40.0, 0.15, 5.0, 0.30, 2.0);
    bench::LatencyHistogram hist(1000);
    for (int i = 0; i < 1000; ++i)
        hist.record(lat_model.sample_total_ns());
    hist.finalize();
    hist.print("End-to-end order latency");
}

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    std::printf("╔══════════════════════════════════════════════════╗\n");
    std::printf("║         HydraExchange v1.0                      ║\n");
    std::printf("║  High-Performance Electronic Exchange Simulator  ║\n");
    std::printf("╚══════════════════════════════════════════════════╝\n");

    bool run_bench      = false;
    bool run_sim        = false;
    bool run_example    = false;
    bool run_ms_exp     = false;
    bool run_exec_demo  = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bench")          == 0) run_bench     = true;
        if (std::strcmp(argv[i], "--sim")            == 0) run_sim       = true;
        if (std::strcmp(argv[i], "--example")        == 0) run_example   = true;
        if (std::strcmp(argv[i], "--microstructure") == 0) run_ms_exp    = true;
        if (std::strcmp(argv[i], "--exec-model")     == 0) run_exec_demo = true;
    }

    // Default: run everything if no flags
    if (!run_bench && !run_sim && !run_example && !run_ms_exp && !run_exec_demo) {
        run_example   = true;
        run_exec_demo = true;
        run_sim       = true;
        run_bench     = true;
    }

    if (run_example)   run_simple_example();
    if (run_exec_demo) run_execution_model_demo();
    if (run_ms_exp)    run_microstructure_experiment();

    if (run_sim) {
        SimulationConfig cfg;
        cfg.duration_seconds     = 60.0;
        cfg.num_market_makers    = 3;
        cfg.num_noise_traders    = 15;
        cfg.num_momentum_agents  = 2;
        cfg.verbose              = true;
        cfg.cst_params.initial_mid = 10000;
        cfg.cst_params.initial_qty_per_level = 20;
        HydraSimulation sim(cfg);
        sim.run();
    }

    if (run_bench) {
        bench::run_all_benchmarks();
    }

    return 0;
}
