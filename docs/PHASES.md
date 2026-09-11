# Phases 2–5

What landed after Phase 1. Matching semantics were not changed.

## Phase 2 — architecture

- Public surface is `include/tape/*.hpp`. Templates live in `include/tape/detail/`.
- `tape_core` is a static library (`src/*.cpp`). Those TUs exist so every public header compiles in isolation; method bodies that were already inline in the class stay inline (same codegen as the old unity includes).
- Binaries: `tape`, `tape_bench`, `tape_offline`. `tape_live` links OpenSSL only, never `tape_core`.
- Tests include `tape/...` headers and link `tape_core`. No `#include "*.cpp"` on the live path.

Old `.cpp`-as-header files are still in the tree as leftovers and are not compiled.

## Phase 3 — benches

- `./build/tape --bench --json` emits one JSON object per line.
- Methodology: `docs/BENCHMARKS.md`.
- Clock is `Clock::now_mono()`. `-march=native` is called out.

## Phase 4 — models

- `docs/MODELS.md` states the equations and the lies (sim loop ≠ FullLatencyModel, AC is square-root temp impact only, LatencyArb is inert, NoiseTrader rate is dead).

## Phase 5 — usability

- `tape_offline` + `examples/sim.cfg`. No network.
- `--config path` on `tape --sim`.
- `load_simulation_config` covered by `tests/test_config.cpp`.
- Live feed remains a separate target.

## Tests

74 cases / 3797 assertions (Phase 1 suite + config load). Same seed still produces the same `--sim` accounting.
