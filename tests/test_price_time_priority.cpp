// ─────────────────────────────────────────────────────────────────────────────
//  test_price_time_priority.cpp
//
//  Exercises LimitOrderBook::match() directly (no MatchingEngine, no pool) —
//  the narrowest possible surface for the single most important invariant
//  in the whole system: strict price priority, then strict time (FIFO)
//  priority within a price level, preserved across partial fills.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"

using namespace hydra;
using hydra_test::OrderArena;

TEST_CASE("FIFO within a single price level: earlier order fills first", "[matching][price-time-priority]") {
    OrderArena arena;
    LimitOrderBook book(1);

    // Two resting sells at the same price, in submission order.
    Order* s1 = arena.make_limit(1, /*cid=*/1, Side::Sell, 100, 5);
    Order* s2 = arena.make_limit(2, /*cid=*/1, Side::Sell, 100, 5);
    REQUIRE(book.add_order(s1));
    REQUIRE(book.add_order(s2));

    // Incoming buy for less than one full level: must take from s1 first.
    Order* taker = arena.make_limit(3, /*cid=*/2, Side::Buy, 100, 3);
    auto fills = book.match(taker);

    REQUIRE(fills.size() == 1);
    CHECK(fills[0].maker_order == s1);
    CHECK(fills[0].fill_qty == 3);
    CHECK(fills[0].fill_price == 100);

    CHECK(s1->leaves_qty == 2);   // s1 partially filled, still at the head
    CHECK(s2->leaves_qty == 5);   // s2 untouched
    CHECK(book.find_order(s1->id) == s1);  // s1 still resting
    CHECK(book.best_ask_qty() == 7);       // 2 (s1) + 5 (s2)
}

TEST_CASE("A resting order keeps its queue position across a partial fill", "[matching][price-time-priority]") {
    // This is the behavioural claim the IOC scenario in main.cpp's own demo
    // depends on: a maker that gets partially filled by one taker must still
    // be filled *before* an order that was resting behind it, when a second,
    // later taker arrives — reduce_qty() must not reshuffle the queue.
    OrderArena arena;
    LimitOrderBook book(1);

    Order* s1 = arena.make_limit(1, 1, Side::Sell, 100, 8);
    Order* s2 = arena.make_limit(2, 1, Side::Sell, 100, 4);
    book.add_order(s1);
    book.add_order(s2);

    Order* taker1 = arena.make_limit(3, 2, Side::Buy, 100, 6);
    auto fills1 = book.match(taker1);
    REQUIRE(fills1.size() == 1);
    CHECK(fills1[0].maker_order == s1);
    CHECK(s1->leaves_qty == 2);  // partially filled, NOT removed

    Order* taker2 = arena.make_limit(4, 2, Side::Buy, 100, 20);
    auto fills2 = book.match(taker2);
    REQUIRE(fills2.size() == 2);
    CHECK(fills2[0].maker_order == s1);   // s1's remaining 2 first ...
    CHECK(fills2[0].fill_qty == 2);
    CHECK(fills2[1].maker_order == s2);   // ... then s2, never reordered
    CHECK(fills2[1].fill_qty == 4);
    CHECK(taker2->filled_qty == 6);
    CHECK(taker2->leaves_qty == 14);
}

TEST_CASE("Price priority beats time priority: better price fills first even if submitted later", "[matching][price-time-priority]") {
    OrderArena arena;
    LimitOrderBook book(1);

    Order* s_far  = arena.make_limit(1, 1, Side::Sell, 102, 5);  // submitted first, worse price
    Order* s_near = arena.make_limit(2, 1, Side::Sell, 101, 5);  // submitted second, better price

    book.add_order(s_far);
    book.add_order(s_near);

    Order* taker = arena.make_limit(3, 2, Side::Buy, 105, 5);  // marketable through both
    auto fills = book.match(taker);

    REQUIRE(fills.size() == 1);
    CHECK(fills[0].maker_order == s_near);   // best price (101) fills first
    CHECK(fills[0].fill_price == 101);
}

TEST_CASE("Bid-side price priority: highest bid fills first for an incoming sell", "[matching][price-time-priority]") {
    OrderArena arena;
    LimitOrderBook book(1);

    Order* b_low  = arena.make_limit(1, 1, Side::Buy, 98, 5);
    Order* b_high = arena.make_limit(2, 1, Side::Buy, 99, 5);
    book.add_order(b_low);
    book.add_order(b_high);

    Order* taker = arena.make_limit(3, 2, Side::Sell, 95, 5);
    auto fills = book.match(taker);

    REQUIRE(fills.size() == 1);
    CHECK(fills[0].maker_order == b_high);
    CHECK(fills[0].fill_price == 99);
}

TEST_CASE("A limit order does not match through its own limit price", "[matching][price-time-priority]") {
    OrderArena arena;
    LimitOrderBook book(1);

    Order* ask = arena.make_limit(1, 1, Side::Sell, 105, 10);
    book.add_order(ask);

    Order* taker = arena.make_limit(2, 2, Side::Buy, 104, 10);  // below the ask
    auto fills = book.match(taker);

    REQUIRE(fills.empty());
    CHECK(taker->leaves_qty == 10);
    CHECK(book.best_ask() == 105);
}

TEST_CASE("A limit order matches exactly at its own limit price", "[matching][price-time-priority]") {
    OrderArena arena;
    LimitOrderBook book(1);

    Order* ask = arena.make_limit(1, 1, Side::Sell, 105, 10);
    book.add_order(ask);

    Order* taker = arena.make_limit(2, 2, Side::Buy, 105, 10);  // exactly at the ask
    auto fills = book.match(taker);

    REQUIRE(fills.size() == 1);
    CHECK(fills[0].fill_qty == 10);
    CHECK(taker->fully_filled());
}
