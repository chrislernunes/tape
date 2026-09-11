// ─────────────────────────────────────────────────────────────────────────────
//  test_invariants.cpp
//
//  Cheap, foundational checks that have to hold before anything else in the
//  suite is worth trusting: struct layout claims, sentinel values, and basic
//  enum/helper correctness.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "tape/order.hpp"
#include "tape/limit_order_book.hpp"

using namespace tape;

TEST_CASE("Order struct size: pin the ACTUAL size, correcting a stale README claim", "[invariants]") {
    // The README says "Order struct (96 bytes, 2 cache lines)" and
    // order.hpp's own comment targets "<= 128 bytes (two cache lines)",
    // enforced by a static_assert(sizeof(Order) <= 128). Measured on this
    // toolchain (GCC 13.3, x86-64, the flags this project actually builds
    // with): sizeof(Order) is exactly 128, not 96 — i.e. the struct sits
    // at the documented ceiling, not the smaller number the README quotes.
    // "2 cache lines" is still correct (128 = 2 x 64-byte lines); "96
    // bytes" is stale — 96 wouldn't even BE 2 whole cache lines. Pin the
    // real, measured number here instead of the aspirational one, so a
    // future field addition that silently grows the struct past the
    // documented ceiling is caught here rather than only in a benchmark.
    STATIC_REQUIRE(sizeof(Order) == 128);
    STATIC_REQUIRE(alignof(Order) == 64);
    STATIC_REQUIRE(sizeof(Order) <= 128);
}

TEST_CASE("Side/opposite/to_string helpers", "[invariants]") {
    REQUIRE(opposite(Side::Buy)  == Side::Sell);
    REQUIRE(opposite(Side::Sell) == Side::Buy);
    REQUIRE(to_string(Side::Buy)  == "BUY");
    REQUIRE(to_string(Side::Sell) == "SELL");
}

TEST_CASE("price_from_double / price_to_double round-trip at 2dp tick size", "[invariants]") {
    REQUIRE(price_from_double(100.00) == 10000);
    REQUIRE(price_from_double(100.01) == 10001);
    REQUIRE(price_to_double(Price{10000}) == Catch::Approx(100.00));
    REQUIRE(price_to_double(Price{10001}) == Catch::Approx(100.01));
}

TEST_CASE("Order::make_limit produces the documented default-valid state", "[invariants]") {
    Order o = Order::make_limit(7, /*cid=*/1, /*iid=*/1, Side::Buy, /*price=*/100, /*qty=*/10, /*ts=*/500);
    REQUIRE(o.id == 7);
    REQUIRE(o.price == 100);
    REQUIRE(o.original_qty == 10);
    REQUIRE(o.leaves_qty == 10);
    REQUIRE(o.filled_qty == 0);
    REQUIRE(o.type == OrderType::Limit);
    REQUIRE(o.tif == TimeInForce::GTC);
    REQUIRE(o.status == OrderStatus::New);
    REQUIRE(o.is_active());
    REQUIRE_FALSE(o.fully_filled());
    REQUIRE(o.submit_time == 500);
    REQUIRE(o.entry_time == 500);
}

TEST_CASE("Order::fill transitions status correctly at each boundary", "[invariants]") {
    Order o = Order::make_limit(1, 1, 1, Side::Buy, 100, 10, 0);

    o.fill(4);
    REQUIRE(o.leaves_qty == 6);
    REQUIRE(o.filled_qty == 4);
    REQUIRE(o.status == OrderStatus::PartiallyFilled);
    REQUIRE(o.is_active());
    REQUIRE_FALSE(o.fully_filled());

    o.fill(6);
    REQUIRE(o.leaves_qty == 0);
    REQUIRE(o.filled_qty == 10);
    REQUIRE(o.status == OrderStatus::Filled);
    REQUIRE_FALSE(o.is_active());
    REQUIRE(o.fully_filled());
}

TEST_CASE("Order::make_market sets marketable-through-any-price semantics", "[invariants]") {
    Order buy_mkt  = Order::make_market(1, 1, 1, Side::Buy,  5, 0);
    Order sell_mkt = Order::make_market(2, 1, 1, Side::Sell, 5, 0);
    REQUIRE(buy_mkt.type == OrderType::Market);
    REQUIRE(buy_mkt.is_market());
    REQUIRE(buy_mkt.price  == std::numeric_limits<Price>::max());
    REQUIRE(sell_mkt.price == std::numeric_limits<Price>::min());
}

TEST_CASE("Empty book reports INVALID_PRICE / zero consistently", "[invariants]") {
    LimitOrderBook book(1);
    REQUIRE(book.best_bid() == INVALID_PRICE);
    REQUIRE(book.best_ask() == INVALID_PRICE);
    REQUIRE(book.spread()   == INVALID_PRICE);
    REQUIRE(book.mid_price() == INVALID_PRICE);
    REQUIRE(book.best_bid_qty() == 0);
    REQUIRE(book.best_ask_qty() == 0);
    REQUIRE_FALSE(book.has_bid());
    REQUIRE_FALSE(book.has_ask());
    REQUIRE(book.total_orders() == 0);
    REQUIRE(book.bid_levels() == 0);
    REQUIRE(book.ask_levels() == 0);
    REQUIRE(book.ofi() == 0.0);
}
