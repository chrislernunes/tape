# Tape

Deterministic C++20 matching engine and LOB simulator. Optional live Binance feed (OpenSSL). Not a live exchange.

v2.0.0. Engine is non-reentrant. Nested agent orders queue until the active `handle_*` returns.

## Build

CMake 3.20+, GCC 10+ or Clang 12+. OpenSSL only for `tape_live`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

./build/tape --example
./build/tape --exec-model
./build/tape --sim
./build/tape --sim --config examples/sim.cfg
./build/tape --bench
./build/tape --bench --json
./build/tape_offline examples/sim.cfg
```

Tests (fetches pinned Catch2 once):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTAPE_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

86 cases. CI: Debug+ASan+UBSan and Release.

## Layout

```
include/tape/          public headers
include/tape/detail/   pool, queues, ring
src/                    tape_core compile units
examples/               offline_sim + sim.cfg; live_binance
tests/
docs/                   PHASE1_SUMMARY, MODELS, BENCHMARKS, PHASES
```

`#include "tape/matching_engine.hpp"` or `tape/tape.hpp`. Do not include `.cpp`. Binance stays under `binance/` and is not in `tape_core`.

## Models / benches

- Equations and limitations: `docs/MODELS.md`
- Reproducing throughput numbers: `docs/BENCHMARKS.md`

Bench figures are `-march=native` and machine-specific. Re-run `--bench --json` locally.

## Known gaps

No fees. CST is exogenous Poisson, not a state-dependent book. Two latency stories (`--sim` vs `--exec-model`). Kyle λ on this flow is not usable. Default MMs often sit behind the seed spread.

## License

MIT.
