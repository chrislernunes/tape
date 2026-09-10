// ─────────────────────────────────────────────────────────────────────────────
//  test_cancel_modify.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"

using namespace hydra;
using hydra_test::EngineHarness;

TEST_CASE("Cancel removes a resting order from the book and index", "[cancel]") {
    EngineHarness h;
    OrderId id = h.new_limit(1, Side::Buy, 100, 10);
    REQUIRE(h.book()->find_order(id) != nullptr);

    h.cancel(id, 1);

    CHECK(h.book()->find_order(id) == nullptr);
    CHECK(h.book()->total_orders() == 0);
    CHECK(h.book()->best_bid() == INVALID_PRICE);

    auto reports = h.reports_for(id);
    REQUIRE(reports.size() == 2);   // New, Cancelled
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::Cancelled);
    CHECK(reports[1].leaves_qty == 0);
}

TEST_CASE("Cancelling an unknown order id is a silent no-op", "[cancel]") {
    EngineHarness h;
    h.new_limit(1, Side::Buy, 100, 10);
    std::size_t before = h.exec_reports().size();

    h.cancel(/*id=*/999999, 1);   // never existed

    CHECK(h.exec_reports().size() == before);   // no report emitted at all
    CHECK(h.book()->total_orders() == 1);       // untouched
}

TEST_CASE("Cancelling an already-fully-filled order is a silent no-op", "[cancel]") {
    EngineHarness h;
    OrderId maker = h.new_limit(1, Side::Sell, 100, 5);
    h.new_limit(2, Side::Buy, 100, 5);           // fully fills and removes `maker`
    REQUIRE(h.book()->find_order(maker) == nullptr);

    std::size_t before = h.exec_reports().size();
    h.cancel(maker, 1);
    CHECK(h.exec_reports().size() == before);    // no spurious Cancelled report
}

TEST_CASE("Partial cancel: cancelling a partially-filled order reports correct cum_qty", "[cancel]") {
    EngineHarness h;
    OrderId maker = h.new_limit(1, Side::Sell, 100, 10);
    h.new_limit(2, Side::Buy, 100, 4);            // partial fill: 4 of 10

    h.cancel(maker, 1);

    auto reports = h.reports_for(maker);
    // New, PartialFill (from the taker's match), Cancelled
    REQUIRE(reports.size() == 3);
    CHECK(reports[1].exec_type == ExecutionReport::ExecType::PartialFill);
    CHECK(reports[2].exec_type == ExecutionReport::ExecType::Cancelled);
    CHECK(reports[2].cum_qty == 4);               // preserves what actually filled
    CHECK(h.book()->find_order(maker) == nullptr);
}

TEST_CASE("Modify (cancel-replace) removes the old order and creates a NEW engine id", "[modify]") {
    EngineHarness h;
    OrderId original = h.new_limit(1, Side::Buy, 100, 10);

    OrderRequest mod;
    mod.action        = OrderRequest::Action::Modify;
    mod.order_id      = original;
    mod.client_id     = 1;
    mod.instrument_id = 1;
    mod.side          = Side::Buy;
    mod.type          = OrderType::Limit;
    mod.tif           = TimeInForce::GTC;
    mod.price         = 101;
    mod.quantity      = 7;
    std::size_t before = h.exec_reports().size();
    h.engine().process(mod);

    CHECK(h.book()->find_order(original) == nullptr);   // old id is gone for good

    std::vector<ExecutionReport> emitted(h.exec_reports().begin() + static_cast<long>(before),
                                          h.exec_reports().end());
    // handle_modify = handle_cancel (Cancelled for `original`) then
    // handle_new (New for a freshly engine-assigned id) — 2 reports, 2
    // different order ids, documenting that a Modify does NOT preserve
    // order identity in the current API.
    REQUIRE(emitted.size() == 2);
    CHECK(emitted[0].exec_type == ExecutionReport::ExecType::Cancelled);
    CHECK(emitted[0].order_id == original);
    CHECK(emitted[1].exec_type == ExecutionReport::ExecType::New);
    CHECK(emitted[1].order_id != original);

    OrderId replacement = emitted[1].order_id;
    Order* r = h.book()->find_order(replacement);
    REQUIRE(r != nullptr);
    CHECK(r->price == 101);
    CHECK(r->leaves_qty == 7);
}

TEST_CASE("Modify loses queue priority even when price and quantity are unchanged", "[modify]") {
    OrderId first  = 0, second = 0;
    EngineHarness h;
    first  = h.new_limit(1, Side::Buy, 100, 5);
    second = h.new_limit(2, Side::Buy, 100, 5);   // resting behind `first`

    // "Modify" the FIRST order to the exact same price/qty. A cancel-replace
    // always re-appends at the tail of its price level, so it must now sit
    // BEHIND `second`, even though nothing about the order's terms changed.
    OrderRequest mod;
    mod.action = OrderRequest::Action::Modify;
    mod.order_id = first; mod.client_id = 1; mod.instrument_id = 1;
    mod.side = Side::Buy; mod.type = OrderType::Limit; mod.tif = TimeInForce::GTC;
    mod.price = 100; mod.quantity = 5;
    h.engine().process(mod);

    // An incoming sell for 5 should now match `second` (never modified),
    // not the replacement of `first` — proving priority was actually lost.
    OrderId taker = h.new_limit(3, Side::Sell, 100, 5);
    auto fills_for_taker = h.trades();
    REQUIRE(fills_for_taker.size() == 1);
    CHECK(fills_for_taker[0].maker_order_id == second);
    (void)taker;
}
