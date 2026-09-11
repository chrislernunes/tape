// ─────────────────────────────────────────────────────────────────────────────
//  test_order_types.cpp
//
//  Market, IOC, FOK, and Post-Only behavior at the MatchingEngine level,
//  including the exact ExecutionReport sequence each path produces — that
//  sequence is as much a part of the public contract as the fills are, since
//  a real client OMS drives its state machine off it.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"

using namespace tape;
using tape_test::EngineHarness;

// ── Market orders ────────────────────────────────────────────────────────────

TEST_CASE("Market order fully filled: [New, Fill], no Cancelled", "[order-types][market]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 10);

    OrderId taker = h.new_market(2, Side::Buy, 10);
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 2);
    CHECK(reports[0].exec_type == ExecutionReport::ExecType::New);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::Fill);
    CHECK(reports[1].last_qty == 10);
    CHECK(reports[1].leaves_qty == 0);
    CHECK(h.trades().size() == 1);
}

TEST_CASE("Market order with partial liquidity: [New, PartialFill, Cancelled]", "[order-types][market]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 4);   // only 4 available

    OrderId taker = h.new_market(2, Side::Buy, 10);  // wants 10
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 3);
    CHECK(reports[0].exec_type == ExecutionReport::ExecType::New);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::PartialFill);
    CHECK(reports[1].last_qty == 4);
    CHECK(reports[2].exec_type == ExecutionReport::ExecType::Cancelled);
    CHECK(reports[2].cum_qty == 4);       // the 4 that did fill are preserved in cum_qty
    CHECK(h.book()->total_orders() == 0); // the resting sell was fully consumed
}

TEST_CASE("Market order against an empty book: [New, PartialFill(0), Cancelled]", "[order-types][market]") {
    // Documents current (slightly unusual) behavior rather than prescribing
    // it: with zero fill, send_exec_report_taker() still reports
    // "PartialFill" (last_qty=0) purely because fully_filled() is false,
    // immediately followed by Cancelled. See Phase 1 summary for the
    // analogous IOC case, which skips this zero-qty report instead.
    EngineHarness h;
    OrderId taker = h.new_market(1, Side::Buy, 5);
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 3);
    CHECK(reports[0].exec_type == ExecutionReport::ExecType::New);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::PartialFill);
    CHECK(reports[1].last_qty == 0);
    CHECK(reports[2].exec_type == ExecutionReport::ExecType::Cancelled);
    CHECK(h.trades().empty());
}

// ── IOC ───────────────────────────────────────────────────────────────────────

TEST_CASE("IOC fully filled behaves like any fully-filled limit order: [New, Fill]", "[order-types][ioc]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 10);

    OrderId taker = h.new_limit(2, Side::Buy, 100, 10, TimeInForce::IOC);
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 2);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::Fill);
    CHECK(h.book()->total_orders() == 0);
}

TEST_CASE("IOC with partial liquidity: [New, PartialFill, Cancelled], remainder never rests", "[order-types][ioc]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 4);

    OrderId taker = h.new_limit(2, Side::Buy, 100, 10, TimeInForce::IOC);
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 3);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::PartialFill);
    CHECK(reports[1].last_qty == 4);
    CHECK(reports[2].exec_type == ExecutionReport::ExecType::Cancelled);
    CHECK(h.book()->find_order(taker) == nullptr);   // IOC remainder never rests
    CHECK(h.book()->total_orders() == 0);
}

TEST_CASE("IOC against an empty book: [New, Cancelled] — no zero-qty PartialFill", "[order-types][ioc]") {
    EngineHarness h;
    OrderId taker = h.new_limit(1, Side::Buy, 100, 5, TimeInForce::IOC);
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 2);
    CHECK(reports[0].exec_type == ExecutionReport::ExecType::New);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::Cancelled);
}

// ── FOK ───────────────────────────────────────────────────────────────────────
//
//  These pin the Phase 1 fix: FOK must be true all-or-nothing. Before the
//  fix, bk->match() ran unconditionally and only the *unfilled remainder*
//  was cancelled after the fact — meaning an FOK order that could not fully
//  fill still permanently, partially consumed real resting orders and
//  published real trades. See the Phase 1 summary for the empirical
//  before/after proof.

TEST_CASE("FOK that cannot fully fill touches nothing: zero trades, book unchanged", "[order-types][fok]") {
    EngineHarness h;
    OrderId resting = h.new_limit(1, Side::Sell, 100, 5);

    OrderId taker = h.new_limit(2, Side::Buy, 100, 10, TimeInForce::FOK);  // needs 10, only 5 exist
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 2);
    CHECK(reports[0].exec_type == ExecutionReport::ExecType::New);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::Cancelled);
    CHECK(reports[1].cum_qty == 0);

    CHECK(h.trades().empty());                              // no phantom trade
    CHECK(h.book()->best_ask_qty() == 5);                    // resting order untouched...
    Order* still_resting = h.book()->find_order(resting);
    REQUIRE(still_resting != nullptr);
    CHECK(still_resting->leaves_qty == 5);                   // ...at its full original size
    CHECK(still_resting->status == OrderStatus::New);        // never even partially filled
}

TEST_CASE("FOK across multiple levels: infeasible even though SOME price levels would cross", "[order-types][fok]") {
    // 5 @ 100 + 3 @ 101 = 8 available; requesting 9 must still be rejected
    // whole, even though the first two levels alone are individually
    // marketable — can_fully_fill() must sum across levels, not stop at one.
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 5);
    h.new_limit(2, Side::Sell, 101, 3);

    OrderId taker = h.new_limit(3, Side::Buy, 101, 9, TimeInForce::FOK);
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 2);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::Cancelled);
    CHECK(h.trades().empty());
    CHECK(h.book()->total_bid_qty() + h.book()->total_ask_qty() == 8);  // nothing consumed
}

TEST_CASE("FOK that CAN fully fill executes exactly like a normal marketable limit order", "[order-types][fok]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 5);
    h.new_limit(2, Side::Sell, 101, 3);   // total available at <=101: 8

    OrderId taker = h.new_limit(3, Side::Buy, 101, 8, TimeInForce::FOK);
    auto reports = h.reports_for(taker);

    REQUIRE(reports.size() == 2);
    CHECK(reports[0].exec_type == ExecutionReport::ExecType::New);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::Fill);
    CHECK(reports[1].leaves_qty == 0);
    CHECK(h.trades().size() == 2);            // one fill per resting level consumed
    CHECK(h.book()->total_orders() == 0);     // both makers fully consumed
}

TEST_CASE("FOK market-order variant: is_market bypasses the limit-price crossing check", "[order-types][fok]") {
    // can_fully_fill()'s `!is_market` gate exists specifically so a marketable
    // (price-less) FOK request sums ALL levels rather than stopping at one
    // implied by a limit price. There is no OrderType::Market + TimeInForce::FOK
    // path in MatchingEngine today (Market orders are handled in their own
    // branch before the FOK check is ever reached) — this test exercises
    // can_fully_fill() directly so the `is_market` branch itself has coverage
    // independent of whether/when a caller wires it up that way.
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 5);
    h.new_limit(2, Side::Sell, 200, 5);   // far away, but a "market" query must still see it

    LimitOrderBook* bk = h.book();
    CHECK(bk->can_fully_fill(Side::Buy, /*limit_price=*/0, /*qty=*/10, /*is_market=*/true));
    CHECK_FALSE(bk->can_fully_fill(Side::Buy, /*limit_price=*/100, /*qty=*/10, /*is_market=*/false));
}

// ── Post-Only ─────────────────────────────────────────────────────────────────

TEST_CASE("PostOnly rejected outright when it would cross: no ack, just Rejected(WouldCross)", "[order-types][post-only]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 5);

    OrderRequest req;
    req.action = OrderRequest::Action::New;
    req.client_id = 2;
    req.instrument_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::PostOnly;
    req.price = 100;    // at the ask -> would cross
    req.quantity = 5;
    std::size_t before = h.exec_reports().size();
    h.engine().process(req);

    std::vector<ExecutionReport> emitted(h.exec_reports().begin() + static_cast<long>(before),
                                          h.exec_reports().end());
    REQUIRE(emitted.size() == 1);          // no separate "New" ack before the reject
    CHECK(emitted[0].exec_type == ExecutionReport::ExecType::Rejected);
    CHECK(emitted[0].reject_reason == RejectReason::WouldCross);
    CHECK(h.book()->best_ask_qty() == 5);  // untouched
}

TEST_CASE("PostOnly accepted and rests when it would not cross", "[order-types][post-only]") {
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 5);

    OrderRequest req;
    req.action = OrderRequest::Action::New;
    req.client_id = 2;
    req.instrument_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::PostOnly;
    req.price = 99;     // below the ask -> does not cross
    req.quantity = 5;
    std::size_t before = h.exec_reports().size();
    h.engine().process(req);

    std::vector<ExecutionReport> emitted(h.exec_reports().begin() + static_cast<long>(before),
                                          h.exec_reports().end());
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0].exec_type == ExecutionReport::ExecType::New);
    CHECK(h.book()->best_bid() == 99);
    CHECK(h.book()->best_bid_qty() == 5);
    CHECK(h.trades().empty());
}

TEST_CASE("PostOnly exactly at the opposing best price counts as crossing", "[order-types][post-only]") {
    // req.price >= best_ask (for a buy) is the documented would_cross rule —
    // pin the boundary explicitly rather than only testing comfortably
    // inside/outside it.
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 5);

    OrderRequest at_touch;
    at_touch.action = OrderRequest::Action::New;
    at_touch.client_id = 2;
    at_touch.instrument_id = 1;
    at_touch.side = Side::Buy;
    at_touch.type = OrderType::PostOnly;
    at_touch.price = 100;   // exactly the ask
    at_touch.quantity = 1;
    h.engine().process(at_touch);

    CHECK(h.exec_reports().back().exec_type == ExecutionReport::ExecType::Rejected);
    CHECK(h.exec_reports().back().reject_reason == RejectReason::WouldCross);
}
