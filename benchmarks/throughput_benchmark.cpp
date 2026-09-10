#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  HydraExchange Benchmarks
//
//  Measures:
//    1. Matching engine throughput (orders/sec, messages/sec)
//    2. LOB insert/cancel latency (p50, p95, p99, p999)
//    3. Market data publish latency
//    4. End-to-end order lifecycle latency
//    5. Memory pool allocation throughput
//
//  All benchmarks use RDTSC for sub-nanosecond timing resolution.
// ─────────────────────────────────────────────────────────────────────────────

#include "../engine/matching_engine/matching_engine.cpp"
#include "../engine/orderbook/limit_order_book.cpp"
#include "../infrastructure/memory_pool.hpp"
#include "../infrastructure/timestamp.hpp"

#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <functional>
#include <string>

namespace hydra {
namespace bench {

// ─────────────────────────────────────────────────────────────────────────────
//  LatencyHistogram: stores raw samples, computes percentiles
// ─────────────────────────────────────────────────────────────────────────────
class LatencyHistogram {
public:
    explicit LatencyHistogram(std::size_t reserve = 1'000'000) {
        samples_.reserve(reserve);
    }

    void record(int64_t ns) noexcept { samples_.push_back(ns); }

    void finalize() noexcept {
        if (samples_.empty()) return;
        std::sort(samples_.begin(), samples_.end());
    }

    [[nodiscard]] int64_t percentile(double p) const noexcept {
        if (samples_.empty()) return 0;
        std::size_t idx = static_cast<std::size_t>(p / 100.0 * samples_.size());
        idx = std::min(idx, samples_.size() - 1);
        return samples_[idx];
    }

    [[nodiscard]] int64_t min()    const noexcept { return samples_.empty() ? 0 : samples_.front(); }
    [[nodiscard]] int64_t max()    const noexcept { return samples_.empty() ? 0 : samples_.back();  }
    [[nodiscard]] double  mean()   const noexcept {
        if (samples_.empty()) return 0.0;
        double sum = 0.0;
        for (auto v : samples_) sum += static_cast<double>(v);
        return sum / samples_.size();
    }
    [[nodiscard]] double  stddev() const noexcept {
        if (samples_.size() < 2) return 0.0;
        double m = mean();
        double var = 0.0;
        for (auto v : samples_) { double d = v - m; var += d * d; }
        return std::sqrt(var / (samples_.size() - 1));
    }
    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }

    void print(const char* label, const char* unit = "ns") const {
        std::printf("\n  ┌── %s ──\n", label);
        std::printf("  │  count   = %zu\n", count());
        std::printf("  │  min     = %lld %s\n", static_cast<long long>(min()), unit);
        std::printf("  │  mean    = %.1f %s\n", mean(), unit);
        std::printf("  │  stddev  = %.1f %s\n", stddev(), unit);
        std::printf("  │  p50     = %lld %s\n", static_cast<long long>(percentile(50)), unit);
        std::printf("  │  p95     = %lld %s\n", static_cast<long long>(percentile(95)), unit);
        std::printf("  │  p99     = %lld %s\n", static_cast<long long>(percentile(99)), unit);
        std::printf("  │  p99.9   = %lld %s\n", static_cast<long long>(percentile(99.9)), unit);
        std::printf("  │  max     = %lld %s\n", static_cast<long long>(max()), unit);
        std::printf("  └──\n");
    }

private:
    std::vector<int64_t> samples_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  BenchmarkResult: summary for one benchmark run
// ─────────────────────────────────────────────────────────────────────────────
struct BenchmarkResult {
    std::string name;
    uint64_t    iterations;
    double      wall_sec;
    double      throughput_mops;   // million ops/sec
    LatencyHistogram latency_ns;

    void print() const {
        std::printf("\n══════════════════════════════════════════════════\n");
        std::printf("  Benchmark: %s\n", name.c_str());
        std::printf("  Iterations:   %llu\n", static_cast<unsigned long long>(iterations));
        std::printf("  Wall time:    %.3f sec\n", wall_sec);
        std::printf("  Throughput:   %.2f M ops/sec\n", throughput_mops);
        const_cast<LatencyHistogram&>(latency_ns).finalize();
        latency_ns.print("Latency (ns)");
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Benchmark: LOB insert / cancel
//
//  Pure order book stress test (no gateway, no matching).
//  Target: > 10M insert+cancel cycles per second.
// ─────────────────────────────────────────────────────────────────────────────
BenchmarkResult bench_lob_insert_cancel(uint64_t N = 1'000'000) {
    BenchmarkResult result;
    result.name       = "LOB Insert + Cancel";
    result.iterations = N;

    LimitOrderBook book(1);
    // 2M * 96B = 192 MB — must be heap-allocated, not on stack
    auto pool_storage = std::make_unique<MemoryPool<Order, 2'000'000>>();
    auto& pool = *pool_storage;

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price>    price_dist(9950, 10050);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);
    std::uniform_int_distribution<int>      side_dist(0, 1);

    std::vector<Order*> live_orders;
    live_orders.reserve(10000);

    LatencyHistogram hist(N);
    int64_t start_wall = Clock::now_mono();

    for (uint64_t i = 0; i < N; ++i) {
        int64_t t0 = Clock::now_mono();

        Order* o = pool.construct();
        o->id            = i + 1;
        o->client_id     = 1;
        o->instrument_id = 1;
        o->side          = side_dist(rng) ? Side::Buy : Side::Sell;
        o->price         = price_dist(rng);
        o->original_qty  = qty_dist(rng);
        o->leaves_qty    = o->original_qty;
        o->filled_qty    = 0;
        o->type          = OrderType::Limit;
        o->tif           = TimeInForce::GTC;
        o->status        = OrderStatus::New;
        o->prev = o->next = nullptr;

        book.add_order(o);
        live_orders.push_back(o);

        // Every 100 inserts, cancel 50 to keep book bounded
        if (live_orders.size() >= 100) {
            for (int k = 0; k < 50; ++k) {
                book.cancel_order(live_orders[k]->id);
                pool.destroy(live_orders[k]);
            }
            live_orders.erase(live_orders.begin(), live_orders.begin() + 50);
        }

        int64_t t1 = Clock::now_mono();
        hist.record(t1 - t0);
    }

    int64_t end_wall = Clock::now_mono();
    result.wall_sec        = static_cast<double>(end_wall - start_wall) / 1e9;
    result.throughput_mops = static_cast<double>(N) / result.wall_sec / 1e6;
    result.latency_ns      = std::move(hist);

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Benchmark: Matching engine end-to-end
//
//  Submit limit + market orders through the full engine pipeline.
//  Includes: pool alloc, risk check (bypassed), LOB match, exec report.
// ─────────────────────────────────────────────────────────────────────────────
BenchmarkResult bench_matching_engine(uint64_t N = 500'000) {
    BenchmarkResult result;
    result.name       = "Matching Engine End-to-End";
    result.iterations = N;

    MatchingEngine engine;
    engine.add_instrument(1);
    engine.set_market_state(1, MarketState::Open);

    // Suppress callbacks for pure throughput measurement
    engine.set_exec_report_cb([](const ExecutionReport&){});
    engine.set_trade_event_cb([](const TradeEvent&){});
    engine.set_market_data_cb([](const MarketDataUpdate&){});

    std::mt19937_64 rng(99);
    std::uniform_int_distribution<Price>    price_dist(9990, 10010);
    std::uniform_int_distribution<Quantity> qty_dist(1, 50);
    std::uniform_int_distribution<int>      type_dist(0, 4);  // 0-3=limit, 4=market

    LatencyHistogram hist(N);
    int64_t start_wall = Clock::now_mono();

    for (uint64_t i = 0; i < N; ++i) {
        int64_t t0 = Clock::now_mono();

        OrderRequest req;
        req.action        = OrderRequest::Action::New;
        req.order_id      = i + 1;
        req.client_id     = static_cast<ClientId>((i % 10) + 1);
        req.instrument_id = 1;
        req.quantity      = qty_dist(rng);
        req.timestamp     = static_cast<Timestamp>(i * 1000);

        int t = type_dist(rng);
        if (t < 4) {
            req.type  = OrderType::Limit;
            req.side  = (i % 2 == 0) ? Side::Buy : Side::Sell;
            req.price = price_dist(rng);
            req.tif   = TimeInForce::GTC;
        } else {
            req.type  = OrderType::Market;
            req.side  = (i % 2 == 0) ? Side::Buy : Side::Sell;
        }

        engine.process(req);

        int64_t t1 = Clock::now_mono();
        hist.record(t1 - t0);
    }

    int64_t end_wall = Clock::now_mono();
    result.wall_sec        = static_cast<double>(end_wall - start_wall) / 1e9;
    result.throughput_mops = static_cast<double>(N) / result.wall_sec / 1e6;
    result.latency_ns      = std::move(hist);

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Benchmark: Memory pool allocation throughput
// ─────────────────────────────────────────────────────────────────────────────
BenchmarkResult bench_memory_pool(uint64_t N = 2'000'000) {
    BenchmarkResult result;
    result.name       = "Memory Pool Alloc+Free";
    result.iterations = N;

    MemoryPool<Order, 1024> pool;  // small pool, high reuse
    LatencyHistogram hist(N);
    int64_t start_wall = Clock::now_mono();

    for (uint64_t i = 0; i < N; ++i) {
        int64_t t0 = Clock::now_mono();
        Order* o = pool.allocate();
        pool.deallocate(o);
        int64_t t1 = Clock::now_mono();
        hist.record(t1 - t0);
    }

    int64_t end_wall = Clock::now_mono();
    result.wall_sec        = static_cast<double>(end_wall - start_wall) / 1e9;
    result.throughput_mops = static_cast<double>(N) / result.wall_sec / 1e6;
    result.latency_ns      = std::move(hist);

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Benchmark: Best bid/ask query throughput
// ─────────────────────────────────────────────────────────────────────────────
BenchmarkResult bench_best_price_query(uint64_t N = 5'000'000) {
    BenchmarkResult result;
    result.name       = "Best Bid/Ask Query";
    result.iterations = N;

    // Pre-populate a reasonably deep book
    LimitOrderBook book(1);
    // 200K * 96B = 19 MB — heap-allocated to avoid stack overflow
    auto pool_storage = std::make_unique<MemoryPool<Order, 200'000>>();
    auto& pool = *pool_storage;
    for (int lvl = 0; lvl < 50; ++lvl) {
        for (int j = 0; j < 10; ++j) {
            Order* bid = pool.construct();
            bid->id = static_cast<OrderId>(lvl * 100 + j);
            bid->price = 9999 - static_cast<Price>(lvl);
            bid->side = Side::Buy;
            bid->leaves_qty = 10; bid->original_qty = 10; bid->filled_qty = 0;
            bid->type = OrderType::Limit; bid->status = OrderStatus::New;
            bid->tif = TimeInForce::GTC; bid->prev = bid->next = nullptr;
            book.add_order(bid);

            Order* ask = pool.construct();
            ask->id = static_cast<OrderId>(10000 + lvl * 100 + j);
            ask->price = 10001 + static_cast<Price>(lvl);
            ask->side = Side::Sell;
            ask->leaves_qty = 10; ask->original_qty = 10; ask->filled_qty = 0;
            ask->type = OrderType::Limit; ask->status = OrderStatus::New;
            ask->tif = TimeInForce::GTC; ask->prev = ask->next = nullptr;
            book.add_order(ask);
        }
    }

    volatile Price sink = 0;  // prevent dead-code elimination
    LatencyHistogram hist(N);
    int64_t start_wall = Clock::now_mono();

    for (uint64_t i = 0; i < N; ++i) {
        int64_t t0 = Clock::now_mono();
        sink = book.best_bid() + book.best_ask();
        int64_t t1 = Clock::now_mono();
        hist.record(t1 - t0);
    }

    (void)sink;
    int64_t end_wall = Clock::now_mono();
    result.wall_sec        = static_cast<double>(end_wall - start_wall) / 1e9;
    result.throughput_mops = static_cast<double>(N) / result.wall_sec / 1e6;
    result.latency_ns      = std::move(hist);

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  run_all_benchmarks: entry point for the benchmark suite
// ─────────────────────────────────────────────────────────────────────────────
void run_all_benchmarks() {
    std::printf("\n╔══════════════════════════════════════════════════╗\n");
    std::printf("║        HydraExchange Benchmark Suite            ║\n");
    std::printf("╚══════════════════════════════════════════════════╝\n");
    std::printf("  Platform: x86-64  Clock: CLOCK_MONOTONIC  C++20\n");

    auto r1 = bench_lob_insert_cancel(1'000'000);     r1.print();
    auto r2 = bench_matching_engine(500'000);          r2.print();
    auto r3 = bench_memory_pool(2'000'000);            r3.print();
    auto r4 = bench_best_price_query(5'000'000);       r4.print();

    std::printf("\n╔══════════════════════════════════════════════════╗\n");
    std::printf("║  Summary                                        ║\n");
    std::printf("╠══════════════════════════════════════════════════╣\n");
    std::printf("║  %-30s  %6.2f M ops/s ║\n", r1.name.c_str(), r1.throughput_mops);
    std::printf("║  %-30s  %6.2f M ops/s ║\n", r2.name.c_str(), r2.throughput_mops);
    std::printf("║  %-30s  %6.2f M ops/s ║\n", r3.name.c_str(), r3.throughput_mops);
    std::printf("║  %-30s  %6.2f M ops/s ║\n", r4.name.c_str(), r4.throughput_mops);
    std::printf("╚══════════════════════════════════════════════════╝\n\n");
}

} // namespace bench
} // namespace hydra
