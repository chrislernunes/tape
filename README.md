
# HydraExchange

Deterministic C++20 exchange simulator. Matching engine, LOB, agent sim, microstructure metrics. Optional live Binance feed (OpenSSL).

## Numbers

Measured on the machine that produced them. `-march=native`, so don't treat these as portable.

| | Throughput | p50 | p99 |
|---|---|---|---|
| LOB insert + cancel | 4.51 M/s | 100 ns | 4.1 µs |
| Matching engine E2E | 4.30 M/s | 200 ns | 600 ns |
| Pool alloc | 27.86 M/s | ~0 ns | 100 ns |
| Best bid/ask | 27.03 M/s | ~0 ns | 100 ns |

`--sim` (60s, seed 12345, after Phase 1 accounting fixes): 15766 orders, 7592 trades, 6037 cancels.

## What it does

- Price-time CDA. Limit GTC, Market, IOC, FOK, Post-Only. Partial fills.
- LOB: `std::map` levels, intrusive queues. O(1) insert/cancel/best. `MemoryPool<Order, 1M>` on the engine (heap; `sizeof(Order) == 128`).
- Agents: inventory-skewed MM, noise, EMA momentum, latency sniper.
- Analytics: Cont L1 BookOFI, trade OFI, VWAP, Kyle λ (EW OLS), spread, RV, alpha-decay IC.
- Latency / exec models: log-normal network, M/M/1, cancel-race closed form, queue-position fill prob, Almgren-Chriss impact.
- Live: public Binance `@depth@100ms` + `@trade`, local book, no API key.

FOK is checked before `match()`. Cancels use engine ids, not the agent's local counter. Taker fills carry `last_px`.

## Layout

```
engine/          matching_engine, LOB, gateway, risk, MD
simulation/      latency, queue-position / impact, CST order flow
agents/          all four agents (market_maker.cpp)
analytics/       OFI, Kyle, spread, RV
binance/         TLS WS + JSON + dashboard
infrastructure/  pool, SPSC/MPSC queues, clock
include/hydra/   types, Order
tests/           Catch2
main.cpp         --example --exec-model --sim --bench
```

`.cpp` files are headers (`#pragma once`, included from `main.cpp`). Research-repo choice. Phase 2 splits them.

## Build

CMake 3.20+, GCC 10+ or Clang 12+, C++20. OpenSSL only for `hydra_live`.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Windows (MSYS2 UCRT64):

```bash
pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-openssl
cmake -B build -DCMAKE_BUILD_TYPE=Release -G "Unix Makefiles"
cmake --build build -j4
```

## Tests

Off by default. Fetches pinned Catch2 once.

```bash
cmake -B build -DHYDRA_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

73 cases / 3790 assertions. CI runs Debug+ASan+UBSan and Release. Notes: `docs/PHASE1_SUMMARY.md`.

## Run

```bash
./build/hydra --example      # hand-walked match
./build/hydra --exec-model   # queue / impact / cancel race
./build/hydra --sim          # 3 MM + noise + momentum
./build/hydra --bench
./build/hydra_live BTCUSDT   # also ETHUSDT SOLUSDT BNBUSDT
```

## Known gaps

NoiseTrader ignores `arrival_rate_per_sec`. LatencyArb never gets L1, so it never trades. CST cancels guess ids. No fees. Default MMs often sit behind the seed spread.

## License

MIT.
