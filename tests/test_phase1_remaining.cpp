// ─────────────────────────────────────────────────────────────────────────────
//  test_phase1_remaining.cpp
//
//  Regression coverage for the remaining Phase-1 agent / gateway / telemetry
//  / analytics bugs. Matching-engine price-time behaviour is unchanged.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "test_helpers.hpp"

#include <memory>
#include <vector>
#include <cmath>

using namespace hydra;
using hydra_test::EngineHarness;

TEST_CASE("New ack reports leaves_qty, side, and instrument", "[telemetry][new]") {
    EngineHarness h;
    OrderId id = h.new_limit(7, Side::Buy, 100, 15);
    auto reports = h.reports_for(id);
    REQUIRE_FALSE(reports.empty());
    CHECK(reports[0].exec_type == ExecutionReport::ExecType::New);
    CHECK(reports[0].order_id == id);
    CHECK(reports[0].client_id == 7);
    CHECK(reports[0].instrument_id == 1);
    CHECK(reports[0].side == Side::Buy);
    CHECK(reports[0].leaves_qty == 15);
    CHECK(reports[0].cum_qty == 0);
}

TEST_CASE("Taker fill report carries last_px, not zero", "[telemetry][fill]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 125, 10);
    OrderId taker = h.new_market(2, Side::Buy, 10);
    auto reports = h.reports_for(taker);
    REQUIRE(reports.size() >= 2);
    CHECK(reports[1].is_fill());
    CHECK(reports[1].last_qty == 10);
    CHECK(reports[1].last_px == 125);
    CHECK(reports[1].avg_px == 125);
    CHECK(reports[1].leaves_qty == 0);
}

TEST_CASE("Taker last_px is VWAP across a two-level sweep", "[telemetry][fill]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 4);
    h.new_limit(1, Side::Sell, 110, 6);
    OrderId taker = h.new_market(2, Side::Buy, 10);
    auto reports = h.reports_for(taker);
    REQUIRE(reports.size() >= 2);
    CHECK(reports[1].is_fill());
    CHECK(reports[1].last_qty == 10);
    CHECK(reports[1].last_px == 106);   // (100*4 + 110*6) / 10
    CHECK(h.trades().size() == 2);
}

TEST_CASE("Maker fill report last_px is the trade price", "[telemetry][fill]") {
    EngineHarness h;
    OrderId maker = h.new_limit(1, Side::Sell, 100, 10);
    h.new_limit(2, Side::Buy, 100, 4);
    auto reports = h.reports_for(maker);
    REQUIRE(reports.size() == 2);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::PartialFill);
    CHECK(reports[1].last_px == 100);
    CHECK(reports[1].last_qty == 4);
    CHECK(reports[1].leaves_qty == 6);
}

TEST_CASE("Gateway forwards Cancel requests that carry quantity 0", "[gateway][cancel]") {
    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);
    SimClock clock(0);
    engine.set_clock(&clock);
    RiskEngine risk;
    OrderGateway gw(&engine, &risk, &clock);

    OrderRequest neo;
    neo.action = OrderRequest::Action::New;
    neo.client_id = 1;
    neo.instrument_id = 1;
    neo.side = Side::Buy;
    neo.type = OrderType::Limit;
    neo.tif = TimeInForce::GTC;
    neo.price = 100;
    neo.quantity = 10;
    REQUIRE(gw.submit(neo));
    REQUIRE(engine.book(1)->total_orders() == 1);

    OrderRequest cxl;
    cxl.action = OrderRequest::Action::Cancel;
    cxl.order_id = 1;
    cxl.client_id = 1;
    cxl.instrument_id = 1;
    REQUIRE(cxl.quantity == 0);
    REQUIRE(gw.submit(cxl));
    CHECK(engine.total_cancels() >= 1);
    CHECK(engine.book(1)->total_orders() == 0);
}

TEST_CASE("RiskEngine::check accepts Cancel with quantity 0", "[risk][cancel]") {
    RiskEngine risk;
    OrderRequest cxl;
    cxl.action = OrderRequest::Action::Cancel;
    cxl.order_id = 42;
    cxl.client_id = 1;
    cxl.instrument_id = 1;
    CHECK(risk.check(cxl) == RejectReason::None);
}

namespace {

struct MMWorld {
    MatchingEngine engine;
    SimClock       clock{0};
    RiskEngine     risk;
    OrderGateway   gw;
    std::vector<std::unique_ptr<MarketMakerAgent>> mms;

    MMWorld()
        : gw(&engine, &risk, &clock)
    {
        engine.add_instrument(1);
        engine.set_market_state(1, MarketState::Open);
        engine.set_clock(&clock);
        engine.set_exec_report_cb([this](const ExecutionReport& er) {
            gw.on_exec_report(er);
        });
        gw.set_exec_report_cb([this](ClientId cid, const ExecutionReport& er) {
            for (auto& a : mms)
                if (a->client_id() == cid) a->on_exec_report(er);
        });
    }

    MarketMakerAgent* add_mm(ClientId cid, MarketMakerAgent::Params p = {}) {
        mms.push_back(std::make_unique<MarketMakerAgent>(cid, 1, &gw, &clock, p));
        return mms.back().get();
    }

    void tick(Price bid, Price ask) {
        MarketDataUpdate mdu;
        mdu.type = MarketDataUpdate::Type::L1Quote;
        mdu.instrument_id = 1;
        mdu.timestamp = clock.now();
        mdu.l1.bid_px = bid;
        mdu.l1.ask_px = ask;
        mdu.l1.bid_qty = 1;
        mdu.l1.ask_qty = 1;
        for (auto& a : mms) a->on_market_data(mdu);
    }
};

} // namespace

TEST_CASE("MarketMaker binds exchange order ids from the New ack", "[agent][mm][id]") {
    MMWorld w;
    auto* mm = w.add_mm(1);
    w.tick(9999, 10001);

    REQUIRE(mm->quoting_bid());
    REQUIRE(mm->quoting_ask());
    CHECK(mm->bid_order_id() != 0);
    CHECK(mm->ask_order_id() != 0);
    CHECK(mm->bid_order_id() != mm->ask_order_id());
    CHECK(w.engine.book(1)->find_order(mm->bid_order_id()) != nullptr);
    CHECK(w.engine.book(1)->find_order(mm->ask_order_id()) != nullptr);
}

TEST_CASE("Single market maker requote cancels its own exchange id", "[agent][mm][requote]") {
    MMWorld w;
    MarketMakerAgent::Params p;
    p.half_spread = 2;
    p.stale_threshold = 3;
    p.quote_qty = 10;
    auto* mm = w.add_mm(1, p);

    w.tick(10000, 10004);
    REQUIRE(w.engine.book(1)->total_orders() == 2);
    OrderId old_bid = mm->bid_order_id();
    OrderId old_ask = mm->ask_order_id();
    REQUIRE(w.engine.book(1)->find_order(old_bid) != nullptr);
    REQUIRE(w.engine.book(1)->find_order(old_ask) != nullptr);

    w.tick(10008, 10012);

    CHECK(w.engine.book(1)->find_order(old_bid) == nullptr);
    CHECK(w.engine.book(1)->find_order(old_ask) == nullptr);
    CHECK(mm->bid_order_id() != old_bid);
    CHECK(mm->ask_order_id() != old_ask);
    CHECK(w.engine.book(1)->find_order(mm->bid_order_id()) != nullptr);
    CHECK(w.engine.book(1)->find_order(mm->ask_order_id()) != nullptr);
    CHECK(w.engine.book(1)->total_orders() == 2);
    CHECK(w.engine.total_cancels() == 2);
}

TEST_CASE("Two market makers requote without leaving stale orders", "[agent][mm][requote]") {
    MMWorld w;
    MarketMakerAgent::Params p1;
    p1.half_spread = 2;
    p1.stale_threshold = 3;
    p1.quote_qty = 10;
    MarketMakerAgent::Params p2 = p1;
    p2.half_spread = 8;
    auto* mm1 = w.add_mm(1, p1);
    auto* mm2 = w.add_mm(2, p2);

    w.tick(10000, 10004);
    REQUIRE(mm1->quoting_bid());
    REQUIRE(mm2->quoting_bid());
    CHECK(w.engine.book(1)->total_orders() == 4);

    OrderId mm1_bid = mm1->bid_order_id();
    OrderId mm2_bid = mm2->bid_order_id();
    CHECK(mm1_bid != mm2_bid);

    w.tick(10008, 10012);

    CHECK(w.engine.book(1)->find_order(mm1_bid) == nullptr);
    CHECK(w.engine.book(1)->find_order(mm2_bid) == nullptr);
    CHECK(w.engine.book(1)->find_order(mm1->bid_order_id()) != nullptr);
    CHECK(w.engine.book(1)->find_order(mm2->bid_order_id()) != nullptr);
    CHECK(w.engine.book(1)->total_orders() == 4);
    CHECK(w.engine.total_cancels() >= 4);
}

TEST_CASE("MarketMaker requote cancels do not hit another agent's orders", "[agent][mm][requote]") {
    MMWorld w;
    MarketMakerAgent::Params p1;
    p1.half_spread = 2;
    p1.stale_threshold = 1;
    MarketMakerAgent::Params p2 = p1;
    p2.half_spread = 10;
    auto* mm1 = w.add_mm(1, p1);
    auto* mm2 = w.add_mm(2, p2);
    w.tick(10000, 10004);

    for (int i = 1; i <= 15; ++i) {
        Price mid_off = static_cast<Price>(i * 2);
        w.tick(10000 + mid_off, 10004 + mid_off);
    }

    CHECK(w.engine.book(1)->total_orders() == 4);
    CHECK(mm1->quoting_bid());
    CHECK(mm2->quoting_bid());
    CHECK(w.engine.total_cancels() > 0);
}

TEST_CASE("record_fill: open, increase, partial close, full close, flip", "[agent][pnl]") {
    MMWorld w;
    auto* mm = w.add_mm(1);

    ExecutionReport er;
    er.exec_type = ExecutionReport::ExecType::Fill;
    er.side = Side::Buy;
    er.last_qty = 10;
    er.last_px = 10000;
    er.leaves_qty = 0;
    er.order_id = 1;
    mm->on_exec_report(er);
    CHECK(mm->net_position() == 10);
    CHECK(mm->avg_entry() == Catch::Approx(100.0));
    CHECK(mm->realized_pnl() == Catch::Approx(0.0));

    er.last_qty = 5;
    er.last_px = 11000;
    er.order_id = 2;
    mm->on_exec_report(er);
    CHECK(mm->net_position() == 15);
    CHECK(mm->avg_entry() == Catch::Approx(103.333333).margin(1e-4));

    er.side = Side::Sell;
    er.last_qty = 5;
    er.last_px = 12000;
    er.order_id = 3;
    mm->on_exec_report(er);
    CHECK(mm->net_position() == 10);
    CHECK(mm->avg_entry() == Catch::Approx(103.333333).margin(1e-4));
    CHECK(mm->realized_pnl() == Catch::Approx((120.0 - 103.333333) * 5).margin(1e-3));

    double rpnl_before = mm->realized_pnl();
    er.last_qty = 15;
    er.last_px = 9000;
    er.order_id = 4;
    mm->on_exec_report(er);
    CHECK(mm->net_position() == -5);
    CHECK(mm->avg_entry() == Catch::Approx(90.0));
    CHECK(mm->realized_pnl() == Catch::Approx(rpnl_before + (90.0 - 103.333333) * 10).margin(1e-3));

    mm->mark_to_market(8800);
    CHECK(mm->unrealized_pnl() == Catch::Approx(10.0));
}

TEST_CASE("RiskEngine on_fill realizes PnL and marks against avg cost", "[risk][pnl]") {
    RiskEngine risk;
    risk.on_fill(1, Side::Buy, 10, 10000);
    const auto* s = risk.state(1);
    REQUIRE(s != nullptr);
    CHECK(s->net_position == 10);
    CHECK(s->avg_cost == Catch::Approx(100.0));
    CHECK(s->realized_pnl == Catch::Approx(0.0));

    risk.on_fill(1, Side::Sell, 10, 11000);
    s = risk.state(1);
    CHECK(s->net_position == 0);
    CHECK(s->realized_pnl == Catch::Approx(100.0));
    CHECK(s->avg_cost == Catch::Approx(0.0));

    risk.mark_to_market(1, 12000);
    s = risk.state(1);
    CHECK(s->unrealized_pnl == Catch::Approx(0.0));
}

TEST_CASE("BookOFI follows Cont L1 increment rules", "[analytics][ofi]") {
    OrderFlowImbalance ofi;
    ofi.on_l1(100, 10, 102, 10);
    CHECK(ofi.book_ofi() == Catch::Approx(0.0));

    ofi.on_l1(100, 15, 102, 10);
    CHECK(ofi.book_ofi() == Catch::Approx(5.0));

    ofi.on_l1(100, 15, 102, 18);
    CHECK(ofi.book_ofi() == Catch::Approx(-8.0));

    ofi.on_l1(101, 7, 102, 18);
    CHECK(ofi.book_ofi() == Catch::Approx(7.0));
}

TEST_CASE("InstrumentAnalytics VWAP, trade count, and Kyle update", "[analytics][kyle][vwap]") {
    InstrumentAnalytics a(1);

    MarketDataUpdate l1;
    l1.type = MarketDataUpdate::Type::L1Quote;
    l1.instrument_id = 1;
    l1.l1.bid_px = 100;
    l1.l1.ask_px = 102;
    l1.l1.bid_qty = 10;
    l1.l1.ask_qty = 10;
    a.on_market_data(l1);

    MarketDataUpdate tr;
    tr.type = MarketDataUpdate::Type::Trade;
    tr.instrument_id = 1;
    tr.trade.price = 101;
    tr.trade.quantity = 4;
    tr.trade.aggressor_side = Side::Buy;
    a.on_market_data(tr);

    tr.trade.price = 103;
    tr.trade.quantity = 6;
    a.on_market_data(tr);

    CHECK(a.trade_count() == 2);
    CHECK(a.vwap() == Catch::Approx((101.0 * 4 + 103.0 * 6) / 10.0));

    l1.l1.bid_px = 104;
    l1.l1.ask_px = 106;
    a.on_market_data(l1);
    CHECK(a.kyle().count() >= 1);
}

TEST_CASE("Kyle through-origin lambda and R² on a constructed series", "[analytics][kyle]") {
    KyleLambda k(1.0);
    k.update(1.0, 2.0);
    k.update(2.0, 4.0);
    k.update(-1.0, -2.0);
    CHECK(k.lambda() == Catch::Approx(2.0));
    CHECK(k.r_squared() == Catch::Approx(1.0).margin(1e-9));
}

TEST_CASE("Short HydraSimulation reports non-zero cancels and coherent PnL", "[sim][cancel]") {
    SimulationConfig cfg;
    cfg.duration_seconds    = 0.25;
    cfg.num_market_makers   = 2;
    cfg.num_noise_traders   = 4;
    cfg.num_momentum_agents = 1;
    cfg.verbose             = false;
    cfg.cst_params.sim_seconds = 0.25;
    cfg.cst_params.seed = 12345;

    HydraSimulation sim(cfg);
    sim.build_default_agents();
    sim.run();

    CHECK(sim.engine().total_cancels() > 0);
    CHECK(sim.engine().total_orders() > 0);
    for (std::size_t i = 0; i < sim.agent_count(); ++i) {
        Agent* a = sim.agent_at(i);
        REQUIRE(a != nullptr);
        CHECK(std::isfinite(a->realized_pnl()));
        CHECK(std::isfinite(a->unrealized_pnl()));
    }
}
