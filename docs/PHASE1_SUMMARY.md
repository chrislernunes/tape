# Phase 1 Summary: Correctness & Testing

Status: **Phase 1 complete and green** (57 test cases / 3,675 assertions passing
in both Debug+ASan+UBSan and Release). Phases 2-5 (architecture, benchmarks,
model docs, usability) are **not started** — see "What's next" below. Per the
original brief ("do not move to a later phase until the previous one is
solid"), this pass went deep on Phase 1 rather than shallow across all five.

Every claim below was checked by actually building and running code in this
repo, not inferred from reading alone. Where a bug is described as "proven,"
a standalone probe program reproduced the failure against the unmodified
code first, then was rerun against the fix to confirm it was resolved.

## Bugs found and fixed

### 1. Every cancelled or maker-filled order permanently leaked its memory pool slot

**Where:** `engine/matching_engine/matching_engine.cpp`, `handle_cancel()` and
`emit_trade()`.

**What was wrong:** `MatchingEngine` owns a `MemoryPool<Order, 1'000'000>`.
Of the six places the pool is touched, one `construct()` and five
`destroy()` calls existed — but every `destroy()` was for the *taker*
order allocated at the top of `handle_new()`. Nothing ever returned a
resting order's slot when it was cancelled, and nothing ever returned a
*maker's* slot when a match fully filled it (the LOB unlinks it from its
own book and index, but doesn't own the pool, so nobody freed it).

**Impact:** Harmless for the shipped demo (60s / ~18k orders, far under the
1,000,000-slot pool), but any longer-running simulation, or any simulation
with a realistic cancel rate, would eventually throw `std::bad_alloc` and
crash — even though far fewer than 1,000,000 orders were ever resting at
once.

**Proof (before):** a probe performing `New` then `Cancel` in a loop threw
`std::bad_alloc` after roughly 800,000-1,000,000 round trips against the
unmodified engine.

**Proof (after):** the identical probe, recompiled against the fix, completed
1,000,050 round trips with zero exceptions. Both scenarios are now permanent
regression tests in `tests/test_memory_lifecycle.cpp` (one for the cancel
path, one for the maker-fill path, run past the known pool size as a
black-box check — the pool size isn't part of the public API, so the test
doesn't hardcode it beyond "comfortably past 1,000,000").

**Fix:** `handle_cancel()` now destroys the order's pool slot once its
Cancelled report has been sent; `emit_trade()` now destroys a maker's slot
once it's confirmed `fully_filled()` and its terminal Fill report has been
sent. Two call sites, ~10 lines total. As a side effect this also resolved a
pre-existing `-Wunused-result` warning on `cancel_order()`'s `[[nodiscard]]`
return value, which is now captured and checked via `assert`.

### 2. FOK orders were not actually all-or-nothing

**Where:** `engine/matching_engine/matching_engine.cpp` (`handle_new()`),
`engine/orderbook/limit_order_book.cpp` (new `can_fully_fill()` method).

**What was wrong:** for every limit order regardless of `TimeInForce`,
`handle_new()` called `bk->match(o)` — which has real, permanent side
effects (consumes resting liquidity, emits real `TradeEvent`s) —
*unconditionally*, and only checked afterwards whether the result happened
to be a full fill. For `FOK`, if it wasn't, the code cancelled the taker's
*unfilled remainder* — but any partial execution that had already happened
against real resting orders was not, and could not be, undone. This made
`FOK` behave like `IOC` with extra steps, not Fill-Or-Kill.

**Impact:** this is a market-semantics bug, not just a bookkeeping one. A
strategy or backtest relying on "an FOK order either fills completely or
never trades at all" would be silently wrong, and the market-data feed
would show real trade prints for an order that was nominally "killed."

**Proof (before):** a resting sell for 5 lots; an incoming `FOK` buy for 10
(infeasible). The unmodified engine emitted a real trade for 5 lots and
reduced the resting order to 0 before cancelling the taker's remaining 5.

**Proof (after):** the identical scenario, rebuilt against the fix, emits
zero trades and leaves the resting order at its original size, untouched.
Both this case and a multi-level variant (feasible only if you sum multiple
price levels — confirming the check doesn't stop at one level) are
permanent tests in `tests/test_order_types.cpp`, alongside a case
confirming a *feasible* FOK still executes normally.

**Fix:** added `LimitOrderBook::can_fully_fill(side, price, qty, is_market)`
— a `const`, non-mutating method that walks the same price levels `match()`
would, summing each level's already-tracked `total_qty()`, without touching
any order or queue position. `handle_new()` now calls this first for `FOK`
orders and rejects the whole thing before ever calling `match()` if it
would not fully fill. This is additive to `LimitOrderBook`'s public API
(no existing signature changed) and mirrors `match()`'s own crossing
condition by design — the two are cross-referenced in comments so they
don't drift apart.

### 3. Hardening: non-deterministic tie-break in event ordering

**Where:** `simulation/replay/synthetic_orderflow.cpp`.

`SyntheticOrderFlowGenerator::generate()` used `std::sort` to order
generated events by timestamp. `std::sort` gives no ordering guarantee for
equal keys; with continuous-valued inter-arrival draws cast to integer
nanoseconds, an exact tie is astronomically unlikely but not impossible,
and would have been a genuine (if rare) violation of "same seed -> bit
identical sequence." Changed to `std::stable_sort` (and added the
previously-transitive `#include <algorithm>` explicitly). Zero behavioral
change in the overwhelming common case; removes the latent edge case
outright.

## Verified correct (not just assumed)

- Price-time priority, including that a partial fill does **not** reshuffle
  a resting order's queue position (checked against a second, later taker).
- The reverse-iterator erase-while-scanning idiom on the bid side
  (`bids_.rbegin()` / `std::next(it).base()`) — hand-traced and confirmed
  correct, then given a dedicated known-answer test that erases **two**
  consecutive bid levels in one `match()` call, which is the scenario most
  likely to expose an off-by-one if that logic ever regresses.
- `main.cpp`'s own manual matching walkthrough — its printed output was
  captured from the built binary, hand-verified, and turned into a
  permanent regression test (`test_known_answer_scenarios.cpp`).
- `QueuePositionModel`'s fill-probability table reproduces the exact
  numbers in the `--exec-model` demo.
- `MarketImpactModel` and `CancellationRaceModel` match their own
  documented closed-form equations (re-derived independently in the test,
  not copied from the implementation) and, for the cancellation race, that
  the Monte Carlo simulation agrees with the closed form within sampling
  error.
- The full `hydra --sim` demo's behavior-relevant output (fills, trades,
  book state) is byte-identical before and after every fix in this pass —
  confirmed by diffing actual runs. Only wall-clock timing lines differ,
  as expected.

## Found, deliberately NOT fixed in this pass (flagged for a decision)

These live in the **agent / gateway layer**, not the matching engine or
LOB, so they sit outside Phase 1's explicit charter ("the matching engine
and limit order book must be demonstrably correct"). Fixing them means
making design decisions (id-scheme, PnL accounting convention) rather than
applying a locally-obvious fix, so they're reported rather than silently
changed.

1. **`MarketMakerAgent`'s cancel-to-requote is broken whenever more than one
   order source shares the book — i.e., in every real simulation this
   project ships.** `Agent::submit_limit()` sends a request tagged with the
   agent's own *local* counter (`next_local_id_`, starting at 1,
   independent per agent instance); the engine ignores that field for `New`
   and assigns its own *global*, engine-wide sequential id. Nothing in
   `MarketMakerAgent::on_exec_report()` ever reads the real id back off the
   `New` ack (`if (!er.is_fill()) return;` skips it). The two id spaces only
   coincidentally match in a single-agent, no-other-traffic scenario — my
   first probe used exactly that scenario and passed, which is itself a
   trap worth noting. With two market makers sharing one book (still far
   simpler than the shipped 20-agent default population), 15 requote cycles
   each produced 114 total order submissions and left **58** resting when
   at most 4 should remain — i.e., the large majority of cancels are either
   silently missing their target or hitting a **different agent's**
   resting order (`handle_cancel` has no ownership check tying a cancel's
   `client_id` to the order's original owner, which compounds this).
   Recommended direction: give `OrderRequest`/`ExecutionReport` a real
   client-order-id vs. exchange-order-id distinction (the standard
   industry pattern for exactly this problem), or have agents capture the
   real id synchronously off their own `New` ack.
2. **`NoiseTraderAgent`'s `arrival_rate_per_sec` constructor parameter is
   dead.** The class declares and constructs an `arrival_dist_` member
   implying a Poisson arrival process at the configured rate, but
   `on_market_data()` never uses it — the actual trigger is a hardcoded
   3%-per-L1-tick check, completely decoupled from the parameter every
   caller (including `build_default_agents()`) passes in.
3. **`LatencyArbAgent` can never trade as currently wired.** Its
   `last_bid_`/`last_ask_` are only ever set by `on_market_data_l1()`, a
   method that is never called anywhere — the simulation loop only invokes
   the base `on_market_data(mdu)` override, which handles `Trade` events
   and never populates those fields. The trading guard
   (`last_bid_ > 0 && last_ask_ > 0`) can therefore never pass. Currently
   latent since `build_default_agents()` doesn't instantiate this agent,
   but silently inert for anyone who does.
4. **`RiskEngine::on_fill()` computes but discards realized PnL** (`(void)
   close_qty;`) — `realized_pnl` is permanently 0.0, so the daily-loss risk
   check is driven only by an `unrealized_pnl` that itself doesn't net
   against cost basis (`net_position * mid_price`, not `net_position *
   (mid_price - avg_cost)`). Notably, `Agent::record_fill()` in the same
   codebase already implements correct average-cost-basis realized PnL —
   a working reference sits right next to the non-working one.

## Minor documentation corrections

- **README says "Order struct (96 bytes, 2 cache lines)."** Measured on
  this project's actual toolchain (GCC 13.3, the flags it actually builds
  with): `sizeof(Order) == 128`. "2 cache lines" is still correct (128 = 2
  x 64-byte lines; 96 wouldn't even be a whole number of lines) — the
  specific byte count is stale. `tests/test_invariants.cpp` now pins the
  real, measured value.
- `LimitOrderBook`'s class-level comment claims it owns
  `order_pool: MemoryPool<Order, 1M>`; the pool actually lives in
  `MatchingEngine` (confirmed against the README's own design notes). Not
  fixed in this pass (comment-only, zero behavioral risk either way, and
  touching it felt like scope creep for a one-line docstring) but worth
  fixing whenever that file is next touched.
- `FullLatencyModel` (log-normal network + M/M/1 queue) is implemented and
  demonstrated in `--exec-model`, but the main simulation loop advances its
  clock using fixed constants from `GatewayLatencyProfile`, not draws from
  this model. Not a bug, but worth being explicit about in Phase 4's model
  documentation, since the two could easily be assumed to be the same
  thing.
- One remaining `-Wunused-result` warning, structurally identical to the
  one fixed in `matching_engine.cpp`: `benchmarks/throughput_benchmark.cpp:163`
  discards `cancel_order()`'s `[[nodiscard]]` return value. Not fixed here
  (benchmarks are Phase 3), but a one-line fix when that file is next
  touched.

## Test suite

Catch2 v3.16.0, fetched via pinned `FetchContent` (not a distro package, so
the exact test-framework version is identical on every machine and in CI —
see `CMakeLists.txt` for the rationale). Off by default
(`HYDRA_BUILD_TESTS=OFF`), matching this project's existing convention for
optional dependencies (same pattern as `BUILD_PYTHON_BINDINGS`); opt in
explicitly:

```
cmake -B build -DHYDRA_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

9 files under `tests/`, one binary (`hydra_tests`), 57 `TEST_CASE`s, 3,675
assertions:

| File | Covers |
|---|---|
| `test_invariants.cpp` | Struct layout, sentinel values, `Order` state transitions |
| `test_price_time_priority.cpp` | FIFO within a level, priority across levels, priority survives partial fills |
| `test_partial_fills.cpp` | Quantity conservation through partial/full fills and level removal |
| `test_order_types.cpp` | Market, IOC, FOK (incl. the Phase 1 fix), Post-Only — exact exec-report sequences |
| `test_cancel_modify.cpp` | Cancel semantics, unknown/already-filled ids, modify's loss of priority and identity |
| `test_known_answer_scenarios.cpp` | Hand-verified multi-level sweeps (incl. the bid-side reverse-iterator-erase path), the `main.cpp` demo replayed as a regression |
| `test_memory_lifecycle.cpp` | The pool-leak regression (both fixed paths) |
| `test_determinism.cpp` | Generator-level and engine-replay-level determinism |
| `test_queue_position_and_models.cpp` | `QueuePositionModel`, `MarketImpactModel`, `CancellationRaceModel` |

`tests/test_helpers.hpp` provides a small `EngineHarness` (wraps a
`MatchingEngine`, records every exec report / trade / market-data callback
in order) and `OrderArena` (stable-address `Order` storage for LOB-only
tests). These are test scaffolding around the *existing* public API, not
changes to it.

**A structural note for Phase 2:** this project's `.cpp`-as-header pattern
(`#pragma once` + direct `#include` of `.cpp` files) has never before been
exercised with more than one such file included per binary — `hydra`,
`hydra_bench`, and `hydra_live` are each built from exactly one `.cpp` that
does the including. Multiple test files in one binary is a new
configuration. Verified empirically before writing any test that this
links cleanly (everything in the chain turns out to be a class member,
`constexpr`, or otherwise implicitly inline — there are no stray
non-inline free functions or globals to collide across translation units).
When Phase 2 splits these into real `.hpp`/`.cpp` pairs, only the
`#include` lines at the top of each test file need to change — the test
bodies do not.

## CI

`.github/workflows/ci.yml`: matrix over Debug/Release, pinned to
`ubuntu-24.04` explicitly (not the floating `ubuntu-latest`) so the CI
environment can't silently drift from what's documented here. Builds with
`-DHYDRA_BUILD_TESTS=ON`, runs `ctest --output-on-failure`, and — Release
leg only — an extra end-to-end check that runs the actual `hydra --sim`
binary twice and diffs the output (excluding the three wall-clock-timing
lines, which legitimately vary run to run and were never part of the
determinism claim).

I can't trigger GitHub's own Actions runners from this environment, so the
workflow itself hasn't executed on GitHub's infrastructure — but every
command in it was run in this sandbox exactly as written (same flags, same
compiler invocation, a real `ctest` invocation from a clean build
directory) and produced the expected result before being written down here.

## What's next (Phases 2-5, not started)

- **Phase 2:** real `.hpp`/`.cpp` separation, a `hydra_core` library target,
  clean public headers. The test suite above is designed to survive this
  mechanically (update includes, not test bodies) and should be kept green
  throughout the migration rather than rewritten after it.
- **Phase 3:** the README's throughput/latency numbers were not
  independently re-benchmarked in this pass (measuring them meaningfully
  needs a quiet, dedicated machine, not a shared 1-core sandbox) and
  `-march=native` means they're inherently machine-specific; Phase 3 should
  pin down exactly what "reproduce this number" means on different
  hardware.
- **Phase 4:** model documentation, plus writing up the four agent-layer
  findings above with actual fixes and their own tests.
- **Phase 5:** the offline example and parameter-externalization goals are
  largely already true today (`hydra --sim` runs with no network access;
  `hydra_live` is already conditionally compiled out without OpenSSL) —
  worth confirming explicitly rather than assuming, when that phase starts.
