# Models

What each model actually computes, and what it is not. Code links are to the public headers.

## Latency — `include/tape/latency.hpp`

End-to-end sample used by `--exec-model`:

```
T = T_net + T_queue + T_proc
T_net   ~ LogNormal(μ, σ)   with mean 40 µs, cv = 0.15  (default)
T_queue ~ M/M/1 wait         service rate 5e6 msg/s
T_proc    = 2 µs constant
```

Log-normal parameters are derived from the requested mean and coefficient of variation, not entered as raw μ,σ.

**Limitations.** The main `--sim` loop does **not** draw from `FullLatencyModel`. It advances `SimClock` by the fixed constants in `GatewayLatencyProfile` (40 µs client→gateway, 2 µs gateway, 5 µs bus, 2 µs engine). Two different latency stories live in the tree. The log-normal model is demonstrated, not wired into the agent sim.

Cancellation race (`CancellationRaceModel`): closed form

```
P(cancel wins) = λ_c / (λ_c + λ_f)
```

with optional Monte Carlo using exponential clocks. This is a two-horse race, not a queue-position-aware race.

## Queue position — `include/tape/execution.hpp`

```
P(fill) = max(0, V − Q_ahead) / Q_order
```

`V` is contra-side volume that walks the queue. Deterministic, no hidden randomness. It does not know about queue jumping, hidden liquidity, or priority reset on modify.

## Almgren–Chriss impact — same header

Square-root temporary impact

```
Δs = σ * sign(q) * sqrt(|q| / V)
```

as implemented. Permanent impact / the full AC trade schedule is **not** implemented. Do not cite this as a calibrated AC estimator.

Partial-fill helper draws a Beta(α,β) fraction in `[0,1]` when you ask it to. That helper is not used by the matching engine; the engine fills against resting size exactly.

## CST synthetic flow — `include/tape/synthetic_flow.hpp`

Cont–Stoikov–Talreja style Poisson arrivals:

- limit intensity at level k: `λ_limit * exp(−α k)`
- market intensity `μ`
- cancel intensity `θ * levels * 2`, target ids guessed sequentially from 1

Events are pre-generated then `stable_sort`ed by timestamp (stable so equal-ns ties keep generation order).

**Limitations.** Cancel target ids are not the live engine ids. A cancel that misses is still counted in `MatchingEngine::total_cancels()` (attempts). Mid is not updated from the live book; intensities do not react to spread.

## Agents — `include/tape/agents.hpp`

| Agent | Decision | Caveat |
|---|---|---|
| MarketMaker | bid = mid − half_spread − inventory*skew, ask symmetric. Requote on fill (next L1) or mid move ≥ stale_threshold. Cancels use **engine** order ids from the New ack. | Quotes the advertised L1, does not join inside the seeded book. |
| NoiseTrader | On L1, Poisson arrivals at `arrival_rate_per_sec` (exp interarrival). Random side, qty ~ U{1,5}. | Rate is now sampled. |
| Momentum | Fast/slow EMA on trades, cooldown 10 ticks. | Warm-up 20 trades. |
| LatencyArb | Latches L1 from `on_market_data`; snipes if a trade is ≥ threshold through that quote. Default population includes one. | Still a toy stale-quote rule. |

PnL is average-cost. Unrealized = pos × (mark − avg). No fees.

## Analytics — `include/tape/analytics.hpp`

- **Trade OFI:** rolling signed aggressor volume, window 256.
- **BookOFI:** Cont L1 increment on successive quotes (`Δq_bid − Δq_ask` with level-shift rules). L2 sizes are not deltas and are not fed into OFI.
- **Kyle λ:** through-origin EW OLS, `λ = Sxy / Sxx`, forget 0.99. Updated on L1 with `(book_ofi, Δmid)`. R² is the uncentered through-origin value.
- **VWAP:** cumulative `Σ px q / Σ q` on prints.

## Matching engine

Price-time CDA. FOK is checked with `can_fully_fill()` **before** `match()`. Taker Fill `last_px` is the VWAP of that match. New ack carries `leaves_qty`, side, instrument. Gateway risk does not apply the qty=0 rule to Cancel.
