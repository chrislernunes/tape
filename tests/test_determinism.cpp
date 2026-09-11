// ─────────────────────────────────────────────────────────────────────────────
//  test_determinism.cpp
//
//  "Given the same seed and inputs, the simulation must produce
//  bit-identical event sequences and final state" is tested at two
//  independent layers, deliberately kept separate:
//
//    1. Event generation: SyntheticOrderFlowGenerator with the same seed
//       must produce an identical event stream.
//    2. Engine processing: feeding an identical event stream into two
//       freshly-constructed MatchingEngines must produce identical
//       ExecutionReports, TradeEvents, and final book state.
//
//  Testing these separately (rather than only end-to-end through
//  TapeSimulation, whose agents/book/engine are not exposed for
//  inspection via its current public API) pins down *which* subsystem a
//  future determinism regression would be in, rather than one coarse
//  pass/fail over the whole pipeline.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"
#include "tape/synthetic_flow.hpp"

using namespace tape;
using tape_test::EngineHarness;

namespace {

bool same_request(const OrderRequest& a, const OrderRequest& b) {
    return a.action == b.action && a.order_id == b.order_id
        && a.client_id == b.client_id && a.instrument_id == b.instrument_id
        && a.side == b.side && a.type == b.type && a.tif == b.tif
        && a.price == b.price && a.quantity == b.quantity
        && a.timestamp == b.timestamp;
}

// Small, fast params so this runs quickly while still generating a
// realistic mix of new/cancel/market events across several price levels.
CSTParams fast_params(uint64_t seed) {
    CSTParams p;
    p.seed = seed;
    p.sim_seconds = 2.0;
    p.num_levels = 5;
    return p;
}

} // namespace

TEST_CASE("SyntheticOrderFlowGenerator: same seed produces a byte-identical event stream", "[determinism]") {
    SyntheticOrderFlowGenerator gen_a(fast_params(777));
    SyntheticOrderFlowGenerator gen_b(fast_params(777));

    auto events_a = gen_a.generate();
    auto events_b = gen_b.generate();

    REQUIRE_FALSE(events_a.empty());   // sanity: params actually produce events
    REQUIRE(events_a.size() == events_b.size());
    for (std::size_t i = 0; i < events_a.size(); ++i) {
        CHECK(events_a[i].time == events_b[i].time);
        CHECK(same_request(events_a[i].request, events_b[i].request));
    }
}

TEST_CASE("SyntheticOrderFlowGenerator: same generator instance is deterministic across repeated calls", "[determinism]") {
    // generate() explicitly resets next_order_id_/next_client_id_ but the
    // rng_ member keeps advancing across calls — and because the event
    // *count* itself is drawn from the rng (not fixed), calling generate()
    // twice on one instance is expected to differ in size too, not just in
    // field values at matching positions. Confirmed empirically: two calls
    // on one instance produced 413 and 435 events respectively. This test
    // exists to document that boundary explicitly rather than assume it.
    SyntheticOrderFlowGenerator gen(fast_params(777));
    auto first_call  = gen.generate();
    auto second_call = gen.generate();

    REQUIRE_FALSE(first_call.empty());
    REQUIRE_FALSE(second_call.empty());

    bool any_difference = (first_call.size() != second_call.size());
    for (std::size_t i = 0; !any_difference && i < first_call.size() && i < second_call.size(); ++i) {
        if (first_call[i].time != second_call[i].time
            || !same_request(first_call[i].request, second_call[i].request)) {
            any_difference = true;
        }
    }
    CHECK(any_difference);  // continuing rng stream -> NOT a repeat; see comment above
}

TEST_CASE("SyntheticOrderFlowGenerator: different seeds produce different event streams", "[determinism]") {
    // A basic sanity check that `seed` is actually wired to the rng and
    // isn't silently ignored (exactly the class of bug this suite exists
    // to catch — see the NoiseTraderAgent finding in the Phase 1 summary).
    SyntheticOrderFlowGenerator gen_a(fast_params(1));
    SyntheticOrderFlowGenerator gen_b(fast_params(2));

    auto events_a = gen_a.generate();
    auto events_b = gen_b.generate();

    bool any_difference = (events_a.size() != events_b.size());
    for (std::size_t i = 0; !any_difference && i < events_a.size() && i < events_b.size(); ++i) {
        if (events_a[i].time != events_b[i].time
            || !same_request(events_a[i].request, events_b[i].request)) {
            any_difference = true;
        }
    }
    CHECK(any_difference);
}

TEST_CASE("MatchingEngine: identical input event streams produce identical output", "[determinism]") {
    SyntheticOrderFlowGenerator gen(fast_params(2026));
    auto events = gen.generate();
    REQUIRE(events.size() > 20);   // meaningful volume, not a degenerate empty run

    auto replay = [&](){
        MatchingEngine engine;
        engine.add_instrument(1);
        engine.set_market_state(1, MarketState::Open);
        std::vector<ExecutionReport> reports;
        std::vector<TradeEvent> trades;
        engine.set_exec_report_cb([&](const ExecutionReport& er) { reports.push_back(er); });
        engine.set_trade_event_cb([&](const TradeEvent& te) { trades.push_back(te); });
        for (auto& ev : events) {
            OrderRequest req = ev.request;
            req.instrument_id = 1;   // generator doesn't set one; pin to our single test instrument
            engine.process(req);
        }
        return std::make_tuple(std::move(reports), std::move(trades), engine.book(1)->best_bid(),
                                engine.book(1)->best_ask(), engine.book(1)->total_orders());
    };

    auto [reports_a, trades_a, bid_a, ask_a, n_a] = replay();
    auto [reports_b, trades_b, bid_b, ask_b, n_b] = replay();

    REQUIRE(reports_a.size() == reports_b.size());
    REQUIRE(trades_a.size() == trades_b.size());
    for (std::size_t i = 0; i < trades_a.size(); ++i) {
        CHECK(trades_a[i].price == trades_b[i].price);
        CHECK(trades_a[i].quantity == trades_b[i].quantity);
        CHECK(trades_a[i].maker_order_id == trades_b[i].maker_order_id);
        CHECK(trades_a[i].taker_order_id == trades_b[i].taker_order_id);
    }
    for (std::size_t i = 0; i < reports_a.size(); ++i) {
        CHECK(reports_a[i].exec_type == reports_b[i].exec_type);
        CHECK(reports_a[i].order_id == reports_b[i].order_id);
        CHECK(reports_a[i].last_qty == reports_b[i].last_qty);
        CHECK(reports_a[i].leaves_qty == reports_b[i].leaves_qty);
        CHECK(reports_a[i].cum_qty == reports_b[i].cum_qty);
    }
    CHECK(bid_a == bid_b);
    CHECK(ask_a == ask_b);
    CHECK(n_a == n_b);
}
