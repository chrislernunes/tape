# Benchmarks

How the numbers in the README were produced, and how to reproduce them on your box.

## Command

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/tape --bench
./build/tape --bench --json        # one JSON object per line
```

`--json` is the machine-readable form. Each line:

```
{"name":"...","iterations":N,"wall_sec":...,"throughput_mops":...,
 "latency_ns":{"min":...,"p50":...,"p95":...,"p99":...,"p999":...,"max":...,"mean":...}}
```

## What is timed

| Name | Path | In the timed loop | Out of the timed loop |
|---|---|---|---|
| LOB Insert + Cancel | `LimitOrderBook::add_order` + `cancel_order` | insert, cancel | pool construct, RNG, book ctor |
| Matching Engine E2E | `MatchingEngine::process` | New (and whatever match/rest it does) | instrument setup |
| Memory Pool Alloc | `MemoryPool::construct` / `destroy` | alloc + free | pool ctor |
| Best Bid/Ask Query | `best_bid` + `best_ask` | the two queries | book fill |

Clock is `Clock::now_mono()` (CLOCK_MONOTONIC / QPC), not RDTSC, despite an older comment. Units are nanoseconds.

## Flags baked into a Release build

`-O3 -march=native -mtune=native -ffast-math -funroll-loops -DNDEBUG`

`-march=native` means the binary is tied to the CPU it was compiled on. Do not compare a number from an AVX-512 box to one from a laptop and call it a regression.

## What the number is not

- Not a wire-to-wire exchange latency. No NIC, no kernel, no cache-cold first-touch isolated.
- Not comparable to FPGA / kernel-bypass matching engines.
- p50 of ~0 ns on the pool and best-price benches means the clock granularity is coarser than the op. Treat those as throughput numbers, not latency numbers.

## Suggested methodology if you publish a new table

1. Quiet machine, no `--sim` running beside it.
2. Release build, same flags as above.
3. Record `lscpu`, compiler id/version, and the `--json` lines.
4. Run the suite three times, report the median throughput.
5. Put hardware + compiler next to the table.
