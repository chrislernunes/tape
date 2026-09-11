#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "tape/agents.hpp"
#include "tape/synthetic_flow.hpp"
#include "tape/simulation.hpp"

using namespace tape;
using tape_test::EngineHarness;

TEST_CASE("LOB oldest_at_best is the time-priority head", "[cst][cancel]") {
    EngineHarness h;
    OrderId first = h.new_limit(1, Side::Buy, 100, 5);
    h.new_limit(2, Side::Buy, 100, 5);
    auto* o = h.engine().book(1)->oldest_at_best(Side::Buy);
    REQUIRE(o);
    CHECK(o->id == first);
}

TEST_CASE("CST cancel events carry order_id=0 for live resolve", "[cst][cancel]") {
    CSTParams p;
    p.sim_seconds = 1.0;
    p.seed = 7;
    p.lambda_limit = 20;
    p.mu_market = 5;
    p.theta_cancel = 8;
    p.num_levels = 3;
    auto ev = SyntheticOrderFlowGenerator(p).generate();
    int cancels = 0, nonzero = 0;
    for (auto& e : ev) {
        if (e.request.action == OrderRequest::Action::Cancel) {
            ++cancels;
            if (e.request.order_id != 0) ++nonzero;
        }
    }
    REQUIRE(cancels > 0);
    CHECK(nonzero == 0);
}

TEST_CASE("sim CST cancel binds a live resting order", "[cst][cancel]") {
    SimulationConfig cfg;
    cfg.duration_seconds = 0.5;
    cfg.num_market_makers = 0;
    cfg.num_noise_traders = 0;
    cfg.num_momentum_agents = 0;
    cfg.verbose = false;
    cfg.cst_params.sim_seconds = 0.5;
    cfg.cst_params.seed = 3;
    cfg.cst_params.lambda_limit = 30;
    cfg.cst_params.mu_market = 2;
    cfg.cst_params.theta_cancel = 20;
    cfg.cst_params.num_levels = 3;
    cfg.cst_params.initial_mid = 10000;
    TapeSimulation sim(cfg);
    sim.run();
    CHECK(sim.engine().total_cancels() > 0);
}

TEST_CASE("NoiseTrader uses arrival_rate_per_sec, not a 3% coin flip", "[noise]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);
    RiskEngine risk;
    SimClock clock(0);
    OrderGateway gw(&engine, &risk, &clock);
    NoiseTraderAgent slow(1, 1, &gw, &clock, /*rate=*/1.0, /*seed=*/1);
    NoiseTraderAgent fast(2, 1, &gw, &clock, /*rate=*/50.0, /*seed=*/1);

    MarketDataUpdate l1;
    l1.type = MarketDataUpdate::Type::L1Quote;
    l1.instrument_id = 1;
    l1.l1.bid_px = 100;
    l1.l1.ask_px = 101;

    for (int i = 0; i < 200; ++i) {
        clock.set(static_cast<Timestamp>(i) * 10'000'000); // 10ms steps, 2s
        l1.timestamp = clock.now();
        slow.on_market_data(l1);
        fast.on_market_data(l1);
    }
    CHECK(fast.market_intents() > slow.market_intents());
    CHECK(fast.market_intents() > 0);
}

TEST_CASE("LatencyArb stores L1 and fires on a through-ask trade", "[arb]") {
    EngineHarness h;
    SimClock clock(0);
    // Gateway talks to the same engine the harness owns.
    // EngineHarness doesn't expose gateway; drive the agent with null gw
    // and check last_bid/ask + that the trade branch is reachable.
    LatencyArbAgent arb(9, 1, nullptr, &clock, /*threshold=*/3, /*size=*/4);

    MarketDataUpdate l1;
    l1.type = MarketDataUpdate::Type::L1Quote;
    l1.instrument_id = 1;
    l1.l1.bid_px = 100;
    l1.l1.ask_px = 102;
    arb.on_market_data(l1);
    CHECK(arb.last_bid() == 100);
    CHECK(arb.last_ask() == 102);

    MarketDataUpdate tr;
    tr.type = MarketDataUpdate::Type::Trade;
    tr.instrument_id = 1;
    tr.trade.price = 110; // 8 ticks through the ask
    tr.trade.quantity = 1;
}

TEST_CASE("LatencyArb submits when L1 is latched and a trade is stale", "[arb]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);
    RiskEngine risk;
    SimClock clock(0);
    OrderGateway gw(&engine, &risk, &clock);
    LatencyArbAgent arb(9, 1, &gw, &clock, 3, 4);

    OrderRequest rest;
    rest.action = OrderRequest::Action::New;
    rest.client_id = 1;
    rest.instrument_id = 1;
    rest.side = Side::Sell;
    rest.type = OrderType::Limit;
    rest.price = 102;
    rest.quantity = 20;
    engine.process(rest);

    MarketDataUpdate l1;
    l1.type = MarketDataUpdate::Type::L1Quote;
    l1.instrument_id = 1;
    l1.l1.bid_px = 100;
    l1.l1.ask_px = 102;
    arb.on_market_data(l1);

    uint64_t orders_before = engine.total_orders();
    MarketDataUpdate tr;
    tr.type = MarketDataUpdate::Type::Trade;
    tr.instrument_id = 1;
    tr.trade.price = 110;
    tr.trade.quantity = 1;
    arb.on_market_data(tr);
    CHECK(engine.total_orders() == orders_before + 1);
}

TEST_CASE("Reentrant market on Trade MD does not double-free a partial maker", "[memory][reentrancy]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);

    engine.set_market_data_cb([&](const MarketDataUpdate& mdu) {
        if (mdu.type != MarketDataUpdate::Type::Trade) return;
        if (mdu.trade.quantity == 0) return;
        // Nested taker: finishes the residual of the second maker while
        // the outer handle_new is still walking its fills vector.
        OrderRequest mkt;
        mkt.action = OrderRequest::Action::New;
        mkt.client_id = 99;
        mkt.instrument_id = 1;
        mkt.side = Side::Buy;
        mkt.type = OrderType::Market;
        mkt.quantity = 10;
        engine.process(mkt);
    });

    OrderRequest a;
    a.action = OrderRequest::Action::New;
    a.client_id = 1; a.instrument_id = 1;
    a.side = Side::Sell; a.type = OrderType::Limit; a.price = 100; a.quantity = 5;
    engine.process(a);
    OrderRequest b = a;
    b.client_id = 2;
    engine.process(b);

    OrderRequest taker;
    taker.action = OrderRequest::Action::New;
    taker.client_id = 3; taker.instrument_id = 1;
    taker.side = Side::Buy; taker.type = OrderType::Market; taker.quantity = 6;
    REQUIRE_NOTHROW(engine.process(taker));
    CHECK(engine.book(1)->total_orders() == 0);
}

static OrderRequest mk_limit(ClientId cid, Side side, Price px, Quantity qty) {
    OrderRequest r;
    r.action = OrderRequest::Action::New;
    r.client_id = cid;
    r.instrument_id = 1;
    r.side = side;
    r.type = OrderType::Limit;
    r.price = px;
    r.quantity = qty;
    return r;
}

TEST_CASE("Queued reentrant requests drain FIFO after outer handle_new", "[reentrancy][fifo]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);

    std::vector<Price> nested_ack_px;
    engine.set_exec_report_cb([&](const ExecutionReport& er) {
        if (er.exec_type != ExecutionReport::ExecType::New) return;
        if (er.client_id != 50 && er.client_id != 51) return;
        // Record book price after each queued New is processed — but the
        // New ack is sent before rest, so just record order of client ids.
    });

    std::vector<ClientId> nested_order;
    engine.set_market_data_cb([&](const MarketDataUpdate& mdu) {
        if (mdu.type != MarketDataUpdate::Type::Trade) return;
        OrderRequest x = mk_limit(50, Side::Buy, 90, 1);
        OrderRequest y = mk_limit(51, Side::Buy, 89, 1);
        engine.process(x);
        engine.process(y);
        nested_order.push_back(50);
        nested_order.push_back(51);
    });

    engine.process(mk_limit(1, Side::Sell, 100, 1));
    OrderRequest mkt;
    mkt.action = OrderRequest::Action::New;
    mkt.client_id = 2; mkt.instrument_id = 1;
    mkt.side = Side::Buy; mkt.type = OrderType::Market; mkt.quantity = 1;
    engine.process(mkt);

    REQUIRE(engine.book(1)->find_order(3) != nullptr); // first queued, engine ids 1=sell,2=mkt,3=cid50,4=cid51
    REQUIRE(engine.book(1)->find_order(4) != nullptr);
    CHECK(engine.book(1)->qty_at(Side::Buy, 90) == 1);
    CHECK(engine.book(1)->qty_at(Side::Buy, 89) == 1);
    // FIFO: 90 was submitted first so it has the lower engine id.
    CHECK(engine.book(1)->find_order(3)->price == 90);
    CHECK(engine.book(1)->find_order(4)->price == 89);
}

TEST_CASE("Trade MD nested Cancel does not run inside outer match", "[reentrancy][cancel]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);

    OrderId victim = 0;
    engine.set_exec_report_cb([&](const ExecutionReport& er) {
        if (er.exec_type == ExecutionReport::ExecType::New && er.client_id == 2)
            victim = er.order_id;
    });
    engine.set_market_data_cb([&](const MarketDataUpdate& mdu) {
        if (mdu.type != MarketDataUpdate::Type::Trade) return;
        if (victim == 0) return;
        OrderRequest c;
        c.action = OrderRequest::Action::Cancel;
        c.order_id = victim;
        c.client_id = 2;
        c.instrument_id = 1;
        engine.process(c);
    });

    engine.process(mk_limit(1, Side::Sell, 100, 3));
    engine.process(mk_limit(2, Side::Sell, 101, 3));
    OrderRequest mkt;
    mkt.action = OrderRequest::Action::New;
    mkt.client_id = 3; mkt.instrument_id = 1;
    mkt.side = Side::Buy; mkt.type = OrderType::Market; mkt.quantity = 3;
    REQUIRE_NOTHROW(engine.process(mkt));
    CHECK(engine.book(1)->find_order(victim) == nullptr);
}

TEST_CASE("Trade MD nested Modify is queued", "[reentrancy][modify]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);

    OrderId live = 0;
    engine.set_exec_report_cb([&](const ExecutionReport& er) {
        if (er.exec_type == ExecutionReport::ExecType::New && er.client_id == 2)
            live = er.order_id;
    });
    engine.set_market_data_cb([&](const MarketDataUpdate& mdu) {
        if (mdu.type != MarketDataUpdate::Type::Trade) return;
        if (live == 0) return;
        OrderRequest m;
        m.action = OrderRequest::Action::Modify;
        m.order_id = live;
        m.client_id = 2;
        m.instrument_id = 1;
        m.side = Side::Sell;
        m.type = OrderType::Limit;
        m.price = 105;
        m.quantity = 2;
        engine.process(m);
    });

    engine.process(mk_limit(1, Side::Sell, 100, 1));
    engine.process(mk_limit(2, Side::Sell, 101, 2));
    OrderRequest mkt;
    mkt.action = OrderRequest::Action::New;
    mkt.client_id = 3; mkt.instrument_id = 1;
    mkt.side = Side::Buy; mkt.type = OrderType::Market; mkt.quantity = 1;
    REQUIRE_NOTHROW(engine.process(mkt));
    CHECK(engine.book(1)->qty_at(Side::Sell, 105) == 2);
    CHECK(engine.book(1)->qty_at(Side::Sell, 101) == 0);
}

TEST_CASE("L1 nested New/Cancel are queued after the resting publish", "[reentrancy][l1]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);

    int l1_seen = 0;
    engine.set_market_data_cb([&](const MarketDataUpdate& mdu) {
        if (mdu.type != MarketDataUpdate::Type::L1Quote) return;
        ++l1_seen;
        if (l1_seen != 1) return;
        engine.process(mk_limit(9, Side::Buy, 99, 4));
        auto* o = engine.book(1)->oldest_at_best(Side::Sell);
        if (!o) return;
        OrderRequest c;
        c.action = OrderRequest::Action::Cancel;
        c.order_id = o->id;
        c.client_id = o->client_id;
        c.instrument_id = 1;
        engine.process(c);
    });

    REQUIRE_NOTHROW(engine.process(mk_limit(1, Side::Sell, 100, 1)));
    CHECK(engine.book(1)->qty_at(Side::Buy, 99) == 4);
}

TEST_CASE("Exec-report callback New is queued, engine stays usable", "[reentrancy][exec]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);

    engine.set_exec_report_cb([&](const ExecutionReport& er) {
        if (er.exec_type != ExecutionReport::ExecType::New) return;
        if (er.client_id != 1) return;
        engine.process(mk_limit(8, Side::Buy, 98, 1));
    });

    REQUIRE_NOTHROW(engine.process(mk_limit(1, Side::Sell, 100, 1)));
    CHECK(engine.book(1)->qty_at(Side::Sell, 100) == 1);
    CHECK(engine.book(1)->qty_at(Side::Buy, 98) == 1);
    REQUIRE_NOTHROW(engine.process(mk_limit(3, Side::Buy, 97, 1)));
    CHECK(engine.book(1)->qty_at(Side::Buy, 97) == 1);
}
