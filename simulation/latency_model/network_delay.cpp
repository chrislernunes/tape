#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Latency Models
//
//  Real exchange latency has three components:
//    1. Network delay      — NIC → exchange gateway
//    2. Queue delay        — time spent in gateway queue
//    3. Processing delay   — matching engine cycle time
//
//  Each model produces a sample latency in nanoseconds.
//  We use log-normal distributions for network (fat-tail), and
//  exponential/gamma for queue delay (M/M/1 queue approximation).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cmath>
#include <random>
#include <algorithm>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  BaseLatencyModel: deterministic mean only (for replay mode)
// ─────────────────────────────────────────────────────────────────────────────
class ConstantLatency {
public:
    explicit ConstantLatency(int64_t delay_ns) noexcept : delay_ns_(delay_ns) {}
    [[nodiscard]] int64_t sample() const noexcept { return delay_ns_; }
    [[nodiscard]] int64_t mean_ns() const noexcept { return delay_ns_; }

private:
    int64_t delay_ns_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  LogNormalLatency: models network jitter (microsecond-scale)
//
//  Parameters: mean_us, stddev_factor
//  Example: mean=40µs, stddev≈8µs → p99≈65µs, p999≈100µs
// ─────────────────────────────────────────────────────────────────────────────
class LogNormalLatency {
public:
    // mean_us: mean in microseconds, cv: coefficient of variation (stddev/mean)
    LogNormalLatency(double mean_us, double cv = 0.2,
                     uint64_t seed = 42)
        : rng_(seed)
    {
        // Log-normal: σ² = ln(cv² + 1),  µ = ln(mean) - σ²/2
        double variance = std::log(cv * cv + 1.0);
        sigma_ = std::sqrt(variance);
        mu_    = std::log(mean_us * 1000.0) - variance / 2.0;  // in ns
        dist_  = std::lognormal_distribution<double>(mu_, sigma_);
    }

    [[nodiscard]] int64_t sample() noexcept {
        double ns = dist_(rng_);
        // Floor at 100 ns (physical minimum)
        return static_cast<int64_t>(std::max(ns, 100.0));
    }

    [[nodiscard]] double mean_ns() const noexcept {
        return std::exp(mu_ + sigma_ * sigma_ / 2.0);
    }

    // Percentile estimate
    [[nodiscard]] double percentile_ns(double p) noexcept {
        // X = exp(µ + σ * Φ⁻¹(p))
        // Φ⁻¹(p) via normal quantile approximation (Beasley-Springer-Moro)
        double z = normal_quantile(p);
        return std::exp(mu_ + sigma_ * z);
    }

private:
    static double normal_quantile(double p) noexcept {
        // Rational approximation (Abramowitz & Stegun 26.2.17)
        static constexpr double c[] = {2.515517, 0.802853, 0.010328};
        static constexpr double d[] = {1.432788, 0.189269, 0.001308};
        double t = std::sqrt(-2.0 * std::log(std::min(p, 1.0 - p)));
        double num = c[0] + t * (c[1] + t * c[2]);
        double den = 1.0 + t * (d[0] + t * (d[1] + t * d[2]));
        double q   = t - num / den;
        return (p < 0.5) ? -q : q;
    }

    std::mt19937_64                     rng_;
    double                              mu_, sigma_;
    std::lognormal_distribution<double> dist_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  QueueDelay: M/M/1 queue model
//
//  W = 1 / (μ - λ)   where μ = service rate, λ = arrival rate
//  Service time is exponentially distributed.
// ─────────────────────────────────────────────────────────────────────────────
class QueueLatency {
public:
    // service_rate_msg_per_us: engine throughput
    // utilization: λ/μ (0 < rho < 1)
    QueueLatency(double service_rate_msg_per_us, double utilization,
                 uint64_t seed = 123)
        : rng_(seed)
    {
        double mu_ns = 1.0 / (service_rate_msg_per_us / 1000.0);  // ns per msg
        double rho   = std::min(utilization, 0.99);  // clamp for stability
        // Mean waiting time = rho * service_time / (1 - rho)
        double mean_wait_ns = rho * mu_ns / (1.0 - rho);
        wait_dist_ = std::exponential_distribution<double>(
                         1.0 / std::max(mean_wait_ns, 100.0));
        service_dist_ = std::exponential_distribution<double>(1.0 / mu_ns);
    }

    [[nodiscard]] int64_t sample_wait() noexcept {
        return static_cast<int64_t>(wait_dist_(rng_));
    }

    [[nodiscard]] int64_t sample_service() noexcept {
        return static_cast<int64_t>(service_dist_(rng_));
    }

    [[nodiscard]] int64_t sample_total() noexcept {
        return sample_wait() + sample_service();
    }

private:
    std::mt19937_64                        rng_;
    std::exponential_distribution<double>  wait_dist_;
    std::exponential_distribution<double>  service_dist_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  FullLatencyModel: composes all components into a single sample
//
//  Total observed latency = network + queue_wait + engine_processing
// ─────────────────────────────────────────────────────────────────────────────
struct LatencyComponents {
    int64_t network_ns{};
    int64_t queue_ns{};
    int64_t processing_ns{};
    [[nodiscard]] int64_t total_ns() const noexcept {
        return network_ns + queue_ns + processing_ns;
    }
};

class FullLatencyModel {
public:
    FullLatencyModel(
        double network_mean_us     = 40.0,   // client → gateway
        double network_cv          = 0.15,
        double engine_rate_msg_us  = 5.0,    // msgs per µs = 5M/sec
        double utilization         = 0.30,   // 30% load
        double processing_mean_us  = 2.0     // engine hot-path
    )
        : network_(network_mean_us, network_cv)
        , queue_(engine_rate_msg_us, utilization)
        , processing_(static_cast<int64_t>(processing_mean_us * 1000.0))
    {}

    [[nodiscard]] LatencyComponents sample() noexcept {
        return {
            network_.sample(),
            queue_.sample_wait(),
            processing_.sample()
        };
    }

    [[nodiscard]] int64_t sample_total_ns() noexcept {
        return sample().total_ns();
    }

private:
    LogNormalLatency network_;
    QueueLatency     queue_;
    ConstantLatency  processing_;
};

} // namespace hydra
