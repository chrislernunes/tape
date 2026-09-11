#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MicrostructureAnalytics
//
//  Computes real-time and rolling microstructure metrics:
//    - Spread distribution (time-weighted, vol-adjusted)
//    - Order Flow Imbalance (OFI) — Lee-Ready signed
//    - Queue length distribution per level
//    - Cancel rate (cancelled / total)
//    - Price impact (Amihud illiquidity, Kyle's lambda)
//    - Alpha decay of order flow signals
//    - Realized volatility (various estimators)
// ─────────────────────────────────────────────────────────────────────────────

#include "tape/events.hpp"
#include "tape/detail/ring_buffer.hpp"

#include <vector>
#include <numeric>
#include <cmath>
#include <array>
#include <deque>

namespace tape {

// ─────────────────────────────────────────────────────────────────────────────
//  RunningStats: online Welford mean/variance
// ─────────────────────────────────────────────────────────────────────────────
class RunningStats {
public:
    void update(double x) noexcept {
        ++n_;
        double delta = x - mean_;
        mean_ += delta / n_;
        double delta2 = x - mean_;
        M2_ += delta * delta2;
        if (x < min_) min_ = x;
        if (x > max_) max_ = x;
    }

    [[nodiscard]] double mean()     const noexcept { return mean_; }
    [[nodiscard]] double variance() const noexcept { return n_ > 1 ? M2_ / (n_ - 1) : 0.0; }
    [[nodiscard]] double stddev()   const noexcept { return std::sqrt(variance()); }
    [[nodiscard]] double min()      const noexcept { return min_; }
    [[nodiscard]] double max()      const noexcept { return max_; }
    [[nodiscard]] uint64_t count()  const noexcept { return n_; }

private:
    uint64_t n_{0};
    double mean_{0.0}, M2_{0.0};
    double min_{std::numeric_limits<double>::max()};
    double max_{std::numeric_limits<double>::lowest()};
};

// ─────────────────────────────────────────────────────────────────────────────
//  SpreadAnalytics
// ─────────────────────────────────────────────────────────────────────────────
class SpreadAnalytics {
public:
    void on_quote(Price bid, Price ask, Timestamp ts) noexcept {
        if (bid == INVALID_PRICE || ask == INVALID_PRICE) return;

        double spread = static_cast<double>(ask - bid);
        spread_stats_.update(spread);

        // Time-weighted spread (area under spread curve)
        if (last_ts_ > 0) {
            double dt = static_cast<double>(ts - last_ts_) / 1e9;  // seconds
            tw_spread_sum_    += spread * dt;
            tw_time_sum_      += dt;
        }
        last_ts_ = ts;

        // Relative spread (% of mid)
        double mid = static_cast<double>(bid + ask) / 2.0;
        if (mid > 0) rel_spread_stats_.update(spread / mid * 100.0);

        // Half-spread is MM revenue per round-trip
        mm_revenue_ += spread / 2.0;
        ++quote_count_;
    }

    [[nodiscard]] double mean_spread()    const noexcept { return spread_stats_.mean(); }
    [[nodiscard]] double max_spread()     const noexcept { return spread_stats_.max(); }
    [[nodiscard]] double stddev_spread()  const noexcept { return spread_stats_.stddev(); }
    [[nodiscard]] double tw_spread()      const noexcept {
        return (tw_time_sum_ > 0) ? tw_spread_sum_ / tw_time_sum_ : 0.0;
    }
    [[nodiscard]] double mean_rel_spread() const noexcept { return rel_spread_stats_.mean(); }
    [[nodiscard]] double mm_gross_pnl()   const noexcept { return mm_revenue_; }

private:
    RunningStats spread_stats_;
    RunningStats rel_spread_stats_;
    double   tw_spread_sum_{0.0};
    double   tw_time_sum_{0.0};
    double   mm_revenue_{0.0};
    Timestamp last_ts_{0};
    uint64_t  quote_count_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  OrderFlowImbalance
//
//  OFI = Σ(ΔV_bid - ΔV_ask) over a window
//  Sign: positive = buying pressure, negative = selling pressure
//
//  Each L2 update contributes a signed delta based on whether it
//  adds or removes liquidity from the best bid/ask.
// ─────────────────────────────────────────────────────────────────────────────
class OrderFlowImbalance {
    static constexpr std::size_t WINDOW = 256;

public:
    void on_l1(Price bid, Quantity bid_qty, Price ask, Quantity ask_qty) noexcept {
        if (bid == INVALID_PRICE || ask == INVALID_PRICE) return;
        if (!have_l1_) {
            last_bid_ = bid; last_bid_qty_ = bid_qty;
            last_ask_ = ask; last_ask_qty_ = ask_qty;
            have_l1_ = true;
            return;
        }

        double e = 0.0;
        if (bid > last_bid_)      e += static_cast<double>(bid_qty);
        else if (bid < last_bid_) e -= static_cast<double>(last_bid_qty_);
        else                      e += static_cast<double>(bid_qty)
                                     - static_cast<double>(last_bid_qty_);

        if (ask < last_ask_)      e -= static_cast<double>(ask_qty);
        else if (ask > last_ask_) e += static_cast<double>(last_ask_qty_);
        else                      e -= static_cast<double>(ask_qty)
                                     - static_cast<double>(last_ask_qty_);

        last_book_ofi_ = e;
        push(e);

        last_bid_ = bid; last_bid_qty_ = bid_qty;
        last_ask_ = ask; last_ask_qty_ = ask_qty;
    }

    // L2 qty is an absolute level size, not Δq. Kept so call sites compile.
    void on_book_update(Side /*side*/, bool /*is_add*/, Quantity /*delta_qty*/,
                         Timestamp /*ts*/) noexcept {}

    void on_trade(Side aggressor, Quantity qty) noexcept {
        double signed_qty = (aggressor == Side::Buy)
                          ? static_cast<double>(qty)
                          : -static_cast<double>(qty);
        push(signed_qty);
    }

    [[nodiscard]] double ofi()         const noexcept { return rolling_ofi_; }
    [[nodiscard]] double book_ofi()    const noexcept { return last_book_ofi_; }
    [[nodiscard]] double normalized_ofi() const noexcept {
        if (window_.empty()) return 0.0;
        double total = 0.0;
        for (double v : window_) total += std::abs(v);
        return (total > 0.0) ? rolling_ofi_ / total : 0.0;
    }

    [[nodiscard]] bool is_bullish() const noexcept { return rolling_ofi_ > 0; }

private:
    void push(double v) noexcept {
        window_.push_back(v);
        if (window_.size() > WINDOW) window_.pop_front();
        rolling_ofi_ = std::accumulate(window_.begin(), window_.end(), 0.0);
    }

    std::deque<double> window_;
    double             rolling_ofi_{0.0};
    double             last_book_ofi_{0.0};
    bool               have_l1_{false};
    Price              last_bid_{0}, last_ask_{0};
    Quantity           last_bid_qty_{0}, last_ask_qty_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  PriceImpactModel
//
//  Kyle's Lambda: regresses mid-price change on signed order flow
//    Δp_t = λ * OFI_t + ε_t
//
//  Online OLS with exponential forgetting.
// ─────────────────────────────────────────────────────────────────────────────
class KyleLambda {
public:
    explicit KyleLambda(double forget = 0.99) noexcept : forget_(forget) {}

    // Update with observed (ofi, price_change) pair
    void update(double ofi, double price_change) noexcept {
        ++n_;
        // Exponentially weighted OLS: sufficient stats
        Sxx_ = forget_ * Sxx_ + ofi * ofi;
        Sxy_ = forget_ * Sxy_ + ofi * price_change;
        Syy_ = forget_ * Syy_ + price_change * price_change;
        Sx_  = forget_ * Sx_  + ofi;
        Sy_  = forget_ * Sy_  + price_change;

        if (Sxx_ > 1e-12)
            lambda_ = Sxy_ / Sxx_;
    }

    [[nodiscard]] double lambda()   const noexcept { return lambda_; }
    [[nodiscard]] uint64_t count()  const noexcept { return n_; }

    // R² of the regression
    [[nodiscard]] double r_squared() const noexcept {
        if (Syy_ < 1e-14) return 0.0;
        double ss_res = Syy_ - lambda_ * Sxy_;
        return std::max(0.0, 1.0 - ss_res / Syy_);
    }

private:
    double forget_;
    double Sxx_{0}, Sxy_{0}, Syy_{0}, Sx_{0}, Sy_{0};
    double lambda_{0};
    uint64_t n_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  RealizedVolatility
//
//  Multiple estimators:
//    1. Close-to-close log returns
//    2. Parkinson (High-Low) estimator (more efficient, 5.2x)
//    3. Rogers-Satchell (drift-robust)
//    4. Yang-Zhang (handles overnight gaps)
// ─────────────────────────────────────────────────────────────────────────────
class RealizedVolatility {
    static constexpr std::size_t MAX_WINDOW = 252;  // trading days

public:
    struct OHLC {
        double open, high, low, close;
    };

    void add_bar(OHLC bar) {
        bars_.push_back(bar);
        if (bars_.size() > MAX_WINDOW)
            bars_.pop_front();
    }

    // Simple close-to-close vol (annualized, assuming 252 trading days)
    [[nodiscard]] double close_to_close(int window = 20) const noexcept {
        if (static_cast<int>(bars_.size()) < window + 1) return 0.0;
        std::vector<double> returns;
        auto it = bars_.end() - window - 1;
        double prev = it->close;
        for (++it; it != bars_.end(); ++it) {
            returns.push_back(std::log(it->close / prev));
            prev = it->close;
        }
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double var  = 0.0;
        for (double r : returns) var += (r - mean) * (r - mean);
        var /= (returns.size() - 1);
        return std::sqrt(var * 252.0);
    }

    // Parkinson: more efficient than close-to-close
    [[nodiscard]] double parkinson(int window = 20) const noexcept {
        if (static_cast<int>(bars_.size()) < window) return 0.0;
        double sum = 0.0;
        auto it = bars_.end() - window;
        for (; it != bars_.end(); ++it) {
            double hl = std::log(it->high / it->low);
            sum += hl * hl;
        }
        return std::sqrt(sum / (4.0 * std::log(2.0) * window) * 252.0);
    }

    // Rogers-Satchell: handles drift without open-to-close bias
    [[nodiscard]] double rogers_satchell(int window = 20) const noexcept {
        if (static_cast<int>(bars_.size()) < window) return 0.0;
        double sum = 0.0;
        auto it = bars_.end() - window;
        for (; it != bars_.end(); ++it) {
            double u = std::log(it->high / it->open);
            double d = std::log(it->low  / it->open);
            double c = std::log(it->close / it->open);
            sum += u * (u - c) + d * (d - c);
        }
        return std::sqrt(sum / window * 252.0);
    }

private:
    std::deque<OHLC> bars_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  QueueAnalytics
//
//  Tracks queue statistics per price level:
//    - arrival rate (Poisson λ)
//    - cancellation rate
//    - fill rate (execution probability per level touch)
// ─────────────────────────────────────────────────────────────────────────────
class QueueAnalytics {
public:
    void on_order_new   () noexcept { ++arrivals_; }
    void on_order_cancel() noexcept { ++cancels_;  }
    void on_order_fill  () noexcept { ++fills_;    }

    // Observe current queue depth
    void observe_depth(uint32_t n_orders, Quantity total_qty) noexcept {
        depth_stats_.update(static_cast<double>(n_orders));
        qty_stats_.update(static_cast<double>(total_qty));
    }

    [[nodiscard]] double cancel_rate() const noexcept {
        uint64_t total = arrivals_;
        return total > 0 ? static_cast<double>(cancels_) / total : 0.0;
    }

    [[nodiscard]] double fill_rate() const noexcept {
        uint64_t total = arrivals_;
        return total > 0 ? static_cast<double>(fills_) / total : 0.0;
    }

    [[nodiscard]] double mean_queue_depth() const noexcept { return depth_stats_.mean(); }
    [[nodiscard]] double mean_queue_qty()   const noexcept { return qty_stats_.mean(); }

    [[nodiscard]] uint64_t arrivals()  const noexcept { return arrivals_; }
    [[nodiscard]] uint64_t cancels()   const noexcept { return cancels_; }
    [[nodiscard]] uint64_t fills()     const noexcept { return fills_; }

private:
    uint64_t     arrivals_{0}, cancels_{0}, fills_{0};
    RunningStats depth_stats_, qty_stats_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  AlphaDecayAnalyzer
//
//  Measures how fast a signal's predictive power decays over time.
//  Given a signal (e.g., OFI) at time t, compute correlation with
//  price returns over horizons [1s, 5s, 30s, 60s, 300s].
// ─────────────────────────────────────────────────────────────────────────────
class AlphaDecayAnalyzer {
    static constexpr int N_HORIZONS = 5;
    // Horizons in seconds
    static constexpr std::array<double, N_HORIZONS> HORIZONS = {1.0, 5.0, 30.0, 60.0, 300.0};

public:
    struct SignalSnapshot {
        double    signal;
        double    mid_price;
        Timestamp timestamp;
    };

    void record_signal(double ofi, double mid_px, Timestamp ts) {
        history_.push_back({ofi, mid_px, ts});
        if (history_.size() > 10000) history_.pop_front();
    }

    // Compute IC (Information Coefficient = rank correlation) for each horizon
    struct DecayProfile {
        std::array<double, N_HORIZONS> ic;
        std::array<double, N_HORIZONS> horizon_sec;
    };

    [[nodiscard]] DecayProfile compute_decay() const {
        DecayProfile dp;
        dp.horizon_sec = HORIZONS;
        dp.ic.fill(0.0);

        for (int h = 0; h < N_HORIZONS; ++h) {
            double horizon_ns = HORIZONS[h] * 1e9;
            std::vector<double> signals, returns;

            for (std::size_t i = 0; i < history_.size(); ++i) {
                auto& s = history_[i];
                // Find price at signal_time + horizon
                for (std::size_t j = i + 1; j < history_.size(); ++j) {
                    if (history_[j].timestamp - s.timestamp >= static_cast<int64_t>(horizon_ns)) {
                        double ret = (history_[j].mid_price - s.mid_price) / s.mid_price;
                        signals.push_back(s.signal);
                        returns.push_back(ret);
                        break;
                    }
                }
            }

            dp.ic[h] = pearson_correlation(signals, returns);
        }

        return dp;
    }

private:
    static double pearson_correlation(const std::vector<double>& x,
                                       const std::vector<double>& y) noexcept {
        if (x.size() < 2 || x.size() != y.size()) return 0.0;
        double n    = static_cast<double>(x.size());
        double sx   = std::accumulate(x.begin(), x.end(), 0.0);
        double sy   = std::accumulate(y.begin(), y.end(), 0.0);
        double sxy  = 0.0, sxx = 0.0, syy = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            sxy += x[i] * y[i];
            sxx += x[i] * x[i];
            syy += y[i] * y[i];
        }
        double num = n * sxy - sx * sy;
        double den = std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
        return (den > 1e-14) ? num / den : 0.0;
    }

    std::deque<SignalSnapshot> history_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  FullAnalytics: composite collector for a single instrument
// ─────────────────────────────────────────────────────────────────────────────
class InstrumentAnalytics {
public:
    explicit InstrumentAnalytics(InstrumentId id) noexcept : id_(id) {}

    void on_market_data(const MarketDataUpdate& mdu) {
        if (mdu.type == MarketDataUpdate::Type::L1Quote) {
            spread_.on_quote(mdu.l1.bid_px, mdu.l1.ask_px, mdu.timestamp);
            ofi_.on_l1(mdu.l1.bid_px, mdu.l1.bid_qty,
                       mdu.l1.ask_px, mdu.l1.ask_qty);
            double mid = (static_cast<double>(mdu.l1.bid_px) +
                          static_cast<double>(mdu.l1.ask_px)) / 2.0;
            if (have_mid_) {
                kyle_.update(ofi_.book_ofi(), mid - last_mid_);
            }
            last_mid_ = mid;
            have_mid_ = true;
            alpha_.record_signal(ofi_.ofi(), mid, mdu.timestamp);
        }
        if (mdu.type == MarketDataUpdate::Type::Trade) {
            ofi_.on_trade(mdu.trade.aggressor_side, mdu.trade.quantity);
            double px = static_cast<double>(mdu.trade.price);
            double q  = static_cast<double>(mdu.trade.quantity);
            vwap_num_ += px * q;
            vwap_den_ += q;
            trade_stats_.update(q);
            ++trade_count_;
        }
    }

    void print_summary(const char* label = "") const {
        std::printf("\n═══ Microstructure Analytics: %s (inst=%u) ═══\n",
                    label, id_);
        std::printf("  Spread:     mean=%.2f  TWAS=%.2f  stddev=%.2f\n",
                    spread_.mean_spread(), spread_.tw_spread(), spread_.stddev_spread());
        std::printf("  Rel Spread: %.4f%%\n", spread_.mean_rel_spread());
        std::printf("  OFI:        %.1f (normalized=%.3f)  BookOFI=%.1f\n",
                    ofi_.ofi(), ofi_.normalized_ofi(), ofi_.book_ofi());
        std::printf("  Trades:     %llu  avg_size=%.1f  VWAP=%.2f\n",
                    static_cast<unsigned long long>(trade_count_),
                    trade_stats_.mean(), vwap());
        std::printf("  Kyle λ:     %.6f  R²=%.3f\n",
                    kyle_.lambda(), kyle_.r_squared());
    }

    [[nodiscard]] double   vwap()        const noexcept {
        return (vwap_den_ > 0.0) ? vwap_num_ / vwap_den_ : 0.0;
    }
    [[nodiscard]] uint64_t trade_count() const noexcept { return trade_count_; }

    [[nodiscard]] SpreadAnalytics&      spread() noexcept { return spread_; }
    [[nodiscard]] OrderFlowImbalance&   ofi()    noexcept { return ofi_;    }
    [[nodiscard]] KyleLambda&           kyle()   noexcept { return kyle_;   }
    [[nodiscard]] AlphaDecayAnalyzer&   alpha()  noexcept { return alpha_;  }

private:
    InstrumentId         id_;
    SpreadAnalytics      spread_;
    OrderFlowImbalance   ofi_;
    KyleLambda           kyle_;
    AlphaDecayAnalyzer   alpha_;
    RunningStats         trade_stats_;
    uint64_t             trade_count_{0};
    double               vwap_num_{0.0};
    double               vwap_den_{0.0};
    double               last_mid_{0.0};
    bool                 have_mid_{false};
};

} // namespace tape
