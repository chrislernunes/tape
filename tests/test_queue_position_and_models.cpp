// ─────────────────────────────────────────────────────────────────────────────
//  test_queue_position_and_models.cpp
//
//  QueuePositionModel gets exact, hand-computed table values (it's pure
//  integer/threshold arithmetic, so exact values are trustworthy and
//  double as a check on main.cpp's own --exec-model demo printout).
//
//  MarketImpactModel and CancellationRaceModel are floating-point formulas;
//  rather than hardcode many-decimal-place numbers computed by hand (easy
//  to get subtly wrong when transcribing), each formula is independently
//  re-derived from its documented equation right here in the test and
//  compared against the implementation within a tight epsilon. This catches
//  implementation drift from the documented model without the test itself
//  being a second, error-prone place the same magic numbers could go stale.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "simulation/execution_model/queue_position.cpp"
#include <cmath>
#include <deque>

using namespace hydra;

// ── QueuePositionModel ────────────────────────────────────────────────────────

TEST_CASE("QueuePositionModel: exact table matching the shipped --exec-model demo", "[models][queue-position]") {
    // Mirrors main.cpp's run_execution_model_demo() exactly: qty_ahead=100,
    // order_qty=20. Values hand-derived and documented inline.
    QueuePositionModel::Position pos;
    pos.qty_ahead = 100;
    pos.order_qty = 20;

    struct Case { Quantity volume; double expected_prob; Quantity expected_fill; };
    const Case cases[] = {
        {50,  0.0, 0},    // volume < qty_ahead: no fill at all
        {100, 0.0, 0},    // volume == qty_ahead exactly: still zero (boundary)
        {110, 0.5, 10},   // 10 of 20 fillable  -> 50%
        {120, 1.0, 20},   // exactly enough for full fill
        {150, 1.0, 20},   // more than enough -> capped at 1.0 / order_qty
    };

    for (auto& c : cases) {
        INFO("volume = " << c.volume);
        CHECK(pos.fill_probability(c.volume) == Catch::Approx(c.expected_prob));
        CHECK(pos.expected_fill(c.volume) == c.expected_fill);
    }
}

TEST_CASE("QueuePositionModel::update_fill reduces qty_ahead but floors at zero", "[models][queue-position]") {
    QueuePositionModel::Position pos;
    pos.qty_ahead = 50;

    QueuePositionModel::update_fill(pos, 20);
    CHECK(pos.qty_ahead == 30);

    QueuePositionModel::update_fill(pos, 1000);   // far more than remains
    CHECK(pos.qty_ahead == 0);                    // floored, not underflowed
}

TEST_CASE("QueuePositionModel::snapshot reads qty_ahead from the resting book state", "[models][queue-position]") {
    LimitOrderBook book(1);
    // Pre-existing resting liquidity our hypothetical order arrives behind.
    // std::deque keeps addresses stable across push_back, which the book
    // requires since it stores raw Order*.
    std::deque<Order> storage;
    storage.push_back(Order::make_limit(1, 1, 1, Side::Buy, 100, 30, 0));
    storage.push_back(Order::make_limit(2, 1, 1, Side::Buy, 100, 15, 0));
    book.add_order(&storage[0]);
    book.add_order(&storage[1]);

    QueuePositionModel model;
    auto pos = model.snapshot(book, /*price=*/100, Side::Buy, /*my_qty=*/10);
    CHECK(pos.qty_ahead == 45);   // 30 + 15 already resting ahead of us
    CHECK(pos.order_qty == 10);
    CHECK(pos.price == 100);
}

// ── MarketImpactModel ─────────────────────────────────────────────────────────

TEST_CASE("MarketImpactModel matches its own documented Almgren-Chriss formula", "[models][market-impact]") {
    MarketImpactModel::ImpactParams params;
    params.eta = 0.10; params.sigma_ticks = 50.0; params.adv = 500'000.0; params.temp_coeff = 0.05;
    MarketImpactModel impact(params);

    for (Quantity qty : {100ULL, 500ULL, 1000ULL, 5000ULL, 10000ULL}) {
        double expected_perm = params.eta * params.sigma_ticks * std::sqrt(double(qty) / params.adv);
        CHECK(impact.permanent_impact_ticks(qty) == Catch::Approx(expected_perm));

        double expected_temp_5pct = params.temp_coeff * params.sigma_ticks
                                   * std::sqrt(0.05) * std::sqrt(double(qty) / params.adv);
        CHECK(impact.temporary_impact_ticks(qty, 0.05) == Catch::Approx(expected_temp_5pct));
        CHECK(impact.total_impact_ticks(qty) == Catch::Approx(expected_perm + expected_temp_5pct));
    }
}

TEST_CASE("MarketImpactModel: impact grows with size but sub-linearly (square-root law)", "[models][market-impact]") {
    MarketImpactModel impact;
    double i1 = impact.total_impact_ticks(1000);
    double i4 = impact.total_impact_ticks(4000);   // 4x the size
    CHECK(i4 > i1);                                 // bigger order -> more impact...
    CHECK(i4 < i1 * 4.0);                           // ...but less than proportionally more
}

TEST_CASE("MarketImpactModel: adjusted_fill_price moves price against the taker's side", "[models][market-impact]") {
    MarketImpactModel impact;
    Price mid = 10000;
    Price buy_px  = impact.adjusted_fill_price(mid, Side::Buy, 1000);
    Price sell_px = impact.adjusted_fill_price(mid, Side::Sell, 1000);
    CHECK(buy_px  >= mid);    // buying pushes the effective price up
    CHECK(sell_px <= mid);    // selling pushes it down
}

// ── CancellationRaceModel ─────────────────────────────────────────────────────

TEST_CASE("CancellationRaceModel: closed-form matches an independently-derived normal-CDF calculation", "[models][cancel-race]") {
    CancellationRaceModel race(45.0, 42.0, 5.0);

    double mu_diff = (45.0 - 42.0) * 1000.0;                    // ns
    double sig = std::sqrt(2.0) * (5.0 * 1000.0);               // ns, independent stddevs combine in quadrature
    double expected = 0.5 * std::erfc(mu_diff / (sig * std::sqrt(2.0)));

    CHECK(race.cancel_success_probability() == Catch::Approx(expected).epsilon(1e-9));
}

TEST_CASE("CancellationRaceModel: Monte Carlo frequency agrees with the closed form within sampling error", "[models][cancel-race]") {
    CancellationRaceModel race(45.0, 42.0, 5.0, /*seed=*/999);
    double theoretical = race.cancel_success_probability();

    const int trials = 20000;
    int wins = 0;
    for (int i = 0; i < trials; ++i) if (race.cancel_wins()) ++wins;
    double empirical = static_cast<double>(wins) / trials;

    // Binomial standard error at n=20000; 5 sigma is an extremely loose,
    // non-flaky bound while still catching a formula that's actually wrong
    // (as opposed to merely sampling noise).
    double se = std::sqrt(theoretical * (1.0 - theoretical) / trials);
    CHECK(std::abs(empirical - theoretical) < 5.0 * se);
}

TEST_CASE("CancellationRaceModel: symmetric latencies give ~50% cancel success", "[models][cancel-race]") {
    CancellationRaceModel race(/*cancel=*/40.0, /*fill=*/40.0, /*jitter=*/5.0);
    CHECK(race.cancel_success_probability() == Catch::Approx(0.5).margin(1e-9));
}

TEST_CASE("CancellationRaceModel: a much slower cancel path rarely wins the race", "[models][cancel-race]") {
    CancellationRaceModel slow_cancel(/*cancel=*/500.0, /*fill=*/40.0, /*jitter=*/5.0);
    CHECK(slow_cancel.cancel_success_probability() < 0.01);
}
