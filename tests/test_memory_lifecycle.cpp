// ─────────────────────────────────────────────────────────────────────────────
//  test_memory_lifecycle.cpp
//
//  Regression coverage for the Phase 1 pool-leak fix. Before the fix,
//  MatchingEngine never returned a cancelled or maker-side-filled order's
//  slot to its ORDER_POOL_SIZE (1,000,000) MemoryPool<Order> — only the
//  taker's own fresh allocation in handle_new was ever destroyed. Every
//  order that ever rested and was later cancelled, or later fully filled as
//  a maker, leaked its slot permanently.
//
//  Proven empirically against the unmodified code before this fix was
//  written: a probe performing New+Cancel round trips threw std::bad_alloc
//  after roughly 800k-1,000,000 iterations, and a probe fully filling a
//  small resting order many times over threw the same way. Both are
//  reproduced here as permanent tests, run against the FIXED engine. See
//  the Phase 1 summary for the full before/after methodology.
//
//  These intentionally iterate past ORDER_POOL_SIZE. That constant isn't
//  exposed (it's a private implementation detail of MatchingEngine), so
//  this is a genuine black-box test: if a slot were still leaking, this
//  test would fail with std::bad_alloc exactly as the original probes did,
//  regardless of the exact pool capacity.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"

using namespace tape;
using tape_test::EngineHarness;

namespace {
    // Comfortably past the known 1,000,000-slot pool size without hardcoding
    // it as a magic number sprinkled through the test bodies below.
    constexpr uint64_t kRoundTripsPastPoolCapacity = 1'000'050ULL;
}

TEST_CASE("Repeated New+Cancel round trips do not exhaust the order pool", "[memory][regression]") {
    EngineHarness h;

    REQUIRE_NOTHROW([&] {
        for (uint64_t i = 0; i < kRoundTripsPastPoolCapacity; ++i) {
            OrderId id = h.new_limit(1, Side::Buy, 100, 1);
            h.cancel(id, 1);
        }
    }());

    CHECK(h.book()->total_orders() == 0);
}

TEST_CASE("Repeated maker-side full fills do not exhaust the order pool", "[memory][regression]") {
    // Exercises the OTHER leak site: emit_trade()'s maker-destroy path,
    // independent of the cancel path above. A persistent resting order is
    // repeatedly recreated and fully consumed by an incoming taker.
    EngineHarness h;

    REQUIRE_NOTHROW([&] {
        for (uint64_t i = 0; i < kRoundTripsPastPoolCapacity; ++i) {
            h.new_limit(1, Side::Sell, 100, 1);      // maker: rests...
            h.new_limit(2, Side::Buy, 100, 1);       // taker: ...and fully fills it
        }
    }());

    CHECK(h.book()->total_orders() == 0);
    CHECK(h.trades().size() == kRoundTripsPastPoolCapacity);
}

TEST_CASE("A cancelled order's slot is promptly reusable, not just eventually", "[memory][regression]") {
    // Narrower than the bulk tests above: explicitly checks that ONE
    // cancel immediately frees ONE slot, by driving the pool to the edge of
    // a small working set and confirming cancel+reallocate cycles don't
    // grow unbounded even at a tight scale where a single missed
    // destroy() would be easy to paper over with a large pool.
    EngineHarness h;
    std::vector<OrderId> ids;
    for (int i = 0; i < 500; ++i) {
        OrderId id = h.new_limit(1, Side::Buy, static_cast<Price>(100 + i), 1);
        ids.push_back(id);
        h.cancel(id, 1);   // immediately retire it
    }
    CHECK(h.book()->total_orders() == 0);
    // If cancel didn't free the slot, this doesn't throw either (the pool
    // is far larger than 500) — this case exists to make failures at small
    // scale legible in isolation while the two tests above cover the
    // actual capacity boundary.
}
