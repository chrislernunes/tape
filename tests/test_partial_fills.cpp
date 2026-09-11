// ─────────────────────────────────────────────────────────────────────────────
//  test_partial_fills.cpp
//
//  Partial-fill bookkeeping: does PriceLevel::total_qty() (and therefore
//  LimitOrderBook's L1/L2 views) stay exactly consistent with the sum of
//  each resting order's leaves_qty, through every combination of partial
//  fill, full fill, and level removal?
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "test_helpers.hpp"

using namespace tape;
using tape_test::OrderArena;

TEST_CASE("Partial fill reduces level total_qty by exactly the fill amount", "[matching][partial-fill]") {
    OrderArena arena;
    LimitOrderBook book(1);

    Order* ask = arena.make_limit(1, 1, Side::Sell, 100, 10);
    book.add_order(ask);
    REQUIRE(book.best_ask_qty() == 10);

    Order* taker = arena.make_limit(2, 2, Side::Buy, 100, 4);
    auto fills = book.match(taker);

    REQUIRE(fills.size() == 1);
    CHECK(fills[0].fill_qty == 4);
    CHECK(ask->leaves_qty == 6);
    CHECK(ask->filled_qty == 4);
    CHECK(ask->status == OrderStatus::PartiallyFilled);
    CHECK(book.best_ask_qty() == 6);          // level total tracks the resting order exactly
    CHECK(book.total_orders() == 1);          // still resting, not removed
    CHECK(book.ask_levels() == 1);
}

TEST_CASE("A sequence of partial fills eventually fully consumes an order and removes its level", "[matching][partial-fill]") {
    OrderArena arena;
    LimitOrderBook book(1);

    Order* ask = arena.make_limit(1, 1, Side::Sell, 100, 10);
    book.add_order(ask);

    // 3 + 4 + 3 = 10: three separate takers, each partially chipping away.
    for (Quantity q : {3ULL, 4ULL, 3ULL}) {
        Order* taker = arena.make_limit(100 + q, 2, Side::Buy, 100, q);
        auto fills = book.match(taker);
        REQUIRE(fills.size() == 1);
        CHECK(fills[0].fill_qty == q);
    }

    CHECK(ask->fully_filled());
    CHECK(ask->filled_qty == 10);
    CHECK(book.find_order(ask->id) == nullptr);   // erased from the index
    CHECK(book.total_orders() == 0);
    CHECK(book.ask_levels() == 0);                 // level removed once empty
    CHECK(book.best_ask() == INVALID_PRICE);
}

TEST_CASE("Multi-order sweep at one level: quantity is conserved exactly across all fills", "[matching][partial-fill]") {
    OrderArena arena;
    LimitOrderBook book(1);

    // Three resting sells at the same price, total 5+3+7=15.
    Order* s1 = arena.make_limit(1, 1, Side::Sell, 100, 5);
    Order* s2 = arena.make_limit(2, 1, Side::Sell, 100, 3);
    Order* s3 = arena.make_limit(3, 1, Side::Sell, 100, 7);
    book.add_order(s1);
    book.add_order(s2);
    book.add_order(s3);
    REQUIRE(book.best_ask_qty() == 15);

    // Buy for 9: fully consumes s1 (5) and s2 (3), partially fills s3 (1 of 7).
    Order* taker = arena.make_limit(4, 2, Side::Buy, 100, 9);
    auto fills = book.match(taker);

    REQUIRE(fills.size() == 3);
    Quantity total_filled = 0;
    for (auto& f : fills) total_filled += f.fill_qty;
    CHECK(total_filled == 9);                 // conservation: nothing gained or lost

    CHECK(s1->fully_filled());
    CHECK(s2->fully_filled());
    CHECK_FALSE(s3->fully_filled());
    CHECK(s3->leaves_qty == 6);

    CHECK(book.total_orders() == 1);          // only s3 remains
    CHECK(book.best_ask_qty() == 6);          // exactly s3's remaining qty
    CHECK(taker->fully_filled());
}

TEST_CASE("Taker partially filled and left resting still reports correct leaves/filled", "[matching][partial-fill]") {
    OrderArena arena;
    LimitOrderBook book(1);

    Order* ask = arena.make_limit(1, 1, Side::Sell, 100, 4);
    book.add_order(ask);

    Order* taker = arena.make_limit(2, 2, Side::Buy, 100, 10);  // more than available
    auto fills = book.match(taker);

    REQUIRE(fills.size() == 1);
    CHECK(fills[0].fill_qty == 4);
    CHECK(taker->filled_qty == 4);
    CHECK(taker->leaves_qty == 6);
    CHECK_FALSE(taker->fully_filled());
    CHECK(taker->status == OrderStatus::PartiallyFilled);

    // MatchingEngine would now rest the remainder (GTC) or cancel it
    // (IOC/FOK) — LimitOrderBook::match() itself does neither; it only
    // reports what matched. Confirmed separately in test_order_types.cpp.
}

TEST_CASE("total_bid_qty / total_ask_qty sum across multiple levels correctly", "[matching][partial-fill]") {
    OrderArena arena;
    LimitOrderBook book(1);

    book.add_order(arena.make_limit(1, 1, Side::Buy, 99, 10));
    book.add_order(arena.make_limit(2, 1, Side::Buy, 98, 5));
    book.add_order(arena.make_limit(3, 1, Side::Sell, 101, 7));
    book.add_order(arena.make_limit(4, 1, Side::Sell, 102, 3));

    CHECK(book.total_bid_qty() == 15);
    CHECK(book.total_ask_qty() == 10);
    CHECK(book.ofi() == Catch::Approx((15.0 - 10.0) / (15.0 + 10.0)));
}
