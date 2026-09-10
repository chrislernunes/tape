// ─────────────────────────────────────────────────────────────────────────────
//  test_known_answer_scenarios.cpp
//
//  Larger, hand-computed scenarios with a fully pre-derived expected fill
//  sequence and residual book state — not just "it didn't crash". Each
//  expected value in this file was computed by hand from the order
//  sequence, then cross-checked against an actual run before being written
//  down as an assertion.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"

using namespace hydra;
using hydra_test::EngineHarness;

TEST_CASE("Known-answer: forward sweep consuming and erasing multiple ask levels", "[known-answer][matching]") {
    // Book (asks, best->worst):
    //   100 x 5  (A1)
    //   100 x 3  (A2, same level, behind A1)
    //   101 x 10 (A3)
    //   102 x 4  (A4)
    // Incoming: BUY LIMIT price=101 qty=15
    //
    // Hand-derived expected fills:
    //   A1: 5 @ 100   (level 100 has 8 total; A1 goes first)
    //   A2: 3 @ 100   (level 100 now fully consumed -> erased)
    //   A3: 7 @ 101   (15 - 5 - 3 = 7 needed; A3 has 10, partial fill)
    //   taker fully filled at 5+3+7=15; A4 @ 102 never touched (taker done
    //   anyway, and 102 > 101 limit regardless).
    EngineHarness h;
    h.new_limit(1, Side::Sell, 100, 5);
    h.new_limit(1, Side::Sell, 100, 3);
    h.new_limit(1, Side::Sell, 101, 10);
    h.new_limit(1, Side::Sell, 102, 4);

    OrderId taker = h.new_limit(2, Side::Buy, 101, 15);

    REQUIRE(h.trades().size() == 3);
    CHECK(h.trades()[0].price == 100); CHECK(h.trades()[0].quantity == 5);
    CHECK(h.trades()[1].price == 100); CHECK(h.trades()[1].quantity == 3);
    CHECK(h.trades()[2].price == 101); CHECK(h.trades()[2].quantity == 7);

    Order* taker_o = h.book()->find_order(taker);
    // Fully filled orders are erased from the index entirely (and their
    // pool slot freed - see test_memory_lifecycle.cpp), so absence here
    // itself confirms full completion.
    CHECK(taker_o == nullptr);

    CHECK(h.book()->ask_levels() == 2);           // 100 erased; 101, 102 remain
    CHECK(h.book()->best_ask() == 101);
    CHECK(h.book()->best_ask_qty() == 3);          // A3: 10 - 7 = 3 left
    CHECK(h.book()->total_ask_qty() == 3 + 4);     // A3 remainder + untouched A4
}

TEST_CASE("Known-answer: reverse sweep erasing multiple consecutive bid levels", "[known-answer][matching]") {
    // This is the highest-risk code path in the whole engine: erasing while
    // reverse-iterating a std::map (bids_.rbegin()/rend()), potentially
    // erasing MULTIPLE levels in a single match() call.
    //
    // Book (bids, best->worst):
    //   200 x 4  (B1)
    //   199 x 6  (B2)
    //   199 x 2  (B3, same level, behind B2)
    //   198 x 10 (B4)
    // Incoming: SELL LIMIT price<=198, qty=13 (use a marketable limit sell
    // at price 1 so it is guaranteed to sweep through 198).
    //
    // Hand-derived expected fills:
    //   B1: 4 @ 200   (level 200 fully consumed -> erased, 1st erase)
    //   B2: 6 @ 199   (9 needed after B1; level 199 has 8 total)
    //   B3: 2 @ 199   (3 needed after B2; B3 has 2, fully consumes it ->
    //                  level 199 now empty -> erased, 2nd erase)
    //   B4: 1 @ 198   (1 needed after B3; B4 has 10, partial fill, stays)
    //   taker fully filled at 4+6+2+1=13.
    EngineHarness h;
    h.new_limit(1, Side::Buy, 200, 4);
    h.new_limit(1, Side::Buy, 199, 6);
    h.new_limit(1, Side::Buy, 199, 2);
    h.new_limit(1, Side::Buy, 198, 10);

    OrderId taker = h.new_limit(2, Side::Sell, 1, 13);

    REQUIRE(h.trades().size() == 4);
    CHECK(h.trades()[0].price == 200); CHECK(h.trades()[0].quantity == 4);
    CHECK(h.trades()[1].price == 199); CHECK(h.trades()[1].quantity == 6);
    CHECK(h.trades()[2].price == 199); CHECK(h.trades()[2].quantity == 2);
    CHECK(h.trades()[3].price == 198); CHECK(h.trades()[3].quantity == 1);

    CHECK(h.book()->find_order(taker) == nullptr);   // taker fully filled

    CHECK(h.book()->bid_levels() == 1);              // 200 and 199 both erased
    CHECK(h.book()->best_bid() == 198);
    CHECK(h.book()->best_bid_qty() == 9);             // B4: 10 - 1 = 9 left
    CHECK(h.book()->total_orders() == 1);             // only B4 remains anywhere
}

TEST_CASE("Known-answer: reproduces the shipped main.cpp --example walkthrough", "[known-answer][regression]") {
    // Mirrors the exact order sequence in main.cpp's run_manual_matching_example(),
    // whose printed output was captured from the built `hydra --example`
    // binary and hand-checked before being turned into these assertions —
    // this pins the shipped demo's own numbers as a regression test.
    EngineHarness h;

    OrderId o1 = h.new_limit(1, Side::Buy,  9998, 5);
    OrderId o2 = h.new_limit(1, Side::Buy,  9999, 10);
    OrderId o4 = h.new_limit(2, Side::Sell, 10001, 8);
    OrderId o6 = h.new_limit(2, Side::Sell, 10001, 4);
    (void)o2;

    // Step: BUY 6 @ 10001 -> takes from o4 first (time priority at 10001).
    OrderId o7 = h.new_limit(3, Side::Buy, 10001, 6);
    {
        auto reports = h.reports_for(o7);
        REQUIRE(reports.size() == 2);
        CHECK(reports[1].exec_type == ExecutionReport::ExecType::Fill);
        CHECK(reports[1].last_qty == 6);
    }
    Order* o4_ptr = h.book()->find_order(o4);
    REQUIRE(o4_ptr != nullptr);
    CHECK(o4_ptr->leaves_qty == 2);        // 8 - 6

    // Step: MARKET SELL 12 -> sweeps o2 (10@9999) then o1 (2 of 5 @9998).
    OrderId mkt = h.new_market(4, Side::Sell, 12);
    {
        auto reports = h.reports_for(mkt);
        REQUIRE(reports.size() == 2);
        CHECK(reports[1].exec_type == ExecutionReport::ExecType::Fill);
    }
    CHECK(h.book()->find_order(o2) == nullptr);     // fully consumed
    Order* o1_ptr = h.book()->find_order(o1);
    REQUIRE(o1_ptr != nullptr);
    CHECK(o1_ptr->leaves_qty == 3);                 // 5 - 2

    // Step: IOC BUY 20 @ 10001 -> takes o4's remaining 2, then o6's 4 = 6;
    // remaining 14 is cancelled (IOC), never rests.
    OrderId ioc = h.new_limit(5, Side::Buy, 10001, 20, TimeInForce::IOC);
    {
        auto reports = h.reports_for(ioc);
        REQUIRE(reports.size() == 3);
        CHECK(reports[1].exec_type == ExecutionReport::ExecType::PartialFill);
        CHECK(reports[1].last_qty == 6);
        CHECK(reports[2].exec_type == ExecutionReport::ExecType::Cancelled);
    }
    CHECK(h.book()->find_order(o4) == nullptr);
    CHECK(h.book()->find_order(o6) == nullptr);
    CHECK(h.book()->best_ask() == INVALID_PRICE);   // ask side fully swept
    CHECK(h.book()->best_bid() == 9998);
    CHECK(h.book()->best_bid_qty() == 3);
}
