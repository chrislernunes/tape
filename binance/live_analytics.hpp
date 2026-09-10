#pragma once

#include "binance_stream.hpp"
#include <deque>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>

namespace hydra {

class LiveAnalytics {
    static constexpr size_t SHORT_WINDOW  = 50;
    static constexpr size_t MEDIUM_WINDOW = 500;

public:
    void on_trade(const BinanceTrade& t) {
        ++total_trades_;
        total_volume_ += t.quantity;
        if (t.is_buy_aggressor()) buy_volume_  += t.quantity;
        else                      sell_volume_ += t.quantity;

        double sq = t.is_buy_aggressor() ? t.quantity : -t.quantity;
        ofi_buffer_.push_back(sq);
        if (ofi_buffer_.size() > MEDIUM_WINDOW) ofi_buffer_.pop_front();

        size_buffer_.push_back(t.quantity);
        if (size_buffer_.size() > MEDIUM_WINDOW) size_buffer_.pop_front();

        vwap_num_ += t.price * t.quantity;
        vwap_den_ += t.quantity;
        if (trade_prices_.size() >= MEDIUM_WINDOW) {
            vwap_num_ -= trade_prices_.front().first * trade_prices_.front().second;
            vwap_den_ -= trade_prices_.front().second;
            trade_prices_.pop_front();
        }
        trade_prices_.push_back({t.price, t.quantity});

        if (last_trade_price_ > 0) {
            double dp = t.price - last_trade_price_;
            kyle_sxx_ = 0.99 * kyle_sxx_ + sq * sq;
            kyle_sxy_ = 0.99 * kyle_sxy_ + sq * dp;
            if (kyle_sxx_ > 1e-12) kyle_lambda_ = kyle_sxy_ / kyle_sxx_;
        }
        last_trade_price_ = t.price;
    }

    void on_depth(const LocalOrderBook& book) {
        if (book.mid() <= 0) return;
        tw_spread_sum_ += book.spread();
        ++tw_spread_count_;
        spread_sum_ += book.spread();
        ++spread_count_;
        last_mid_      = book.mid();
        last_spread_   = book.spread();
        last_ofi_book_ = book.ofi_top();
        ++depth_updates_;
    }

    double rolling_ofi(size_t w = SHORT_WINDOW) const {
        double s = 0.0;
        size_t n = std::min(w, ofi_buffer_.size());
        for (size_t i = ofi_buffer_.size() - n; i < ofi_buffer_.size(); ++i) s += ofi_buffer_[i];
        return s;
    }
    double normalized_ofi(size_t w = SHORT_WINDOW) const {
        double as = 0.0;
        size_t n = std::min(w, ofi_buffer_.size());
        for (size_t i = ofi_buffer_.size() - n; i < ofi_buffer_.size(); ++i) as += std::abs(ofi_buffer_[i]);
        return as < 1e-12 ? 0.0 : rolling_ofi(w) / as;
    }
    double vwap()           const { return vwap_den_ > 0 ? vwap_num_ / vwap_den_ : 0.0; }
    double buy_sell_ratio() const { double t = buy_volume_+sell_volume_; return t>1e-12 ? buy_volume_/t : 0.5; }
    double avg_trade_size() const {
        if (size_buffer_.empty()) return 0.0;
        return std::accumulate(size_buffer_.begin(), size_buffer_.end(), 0.0) / size_buffer_.size();
    }
    double mean_spread()    const { return spread_count_ > 0 ? spread_sum_ / spread_count_ : 0.0; }
    double tw_spread()      const { return tw_spread_count_ > 0 ? tw_spread_sum_ / tw_spread_count_ : 0.0; }
    double kyle_lambda()    const { return kyle_lambda_; }
    double last_mid()       const { return last_mid_; }
    double book_ofi()       const { return last_ofi_book_; }
    uint64_t total_trades() const { return total_trades_; }
    uint64_t depth_updates()const { return depth_updates_; }
    double buy_volume()     const { return buy_volume_; }
    double sell_volume()    const { return sell_volume_; }

    const char* flow_signal_str() const {
        double ofi = normalized_ofi();
        if (ofi > 0.25)  return "^^ BUY  PRESSURE ^^";
        if (ofi < -0.25) return "vv SELL PRESSURE vv";
        return "==   BALANCED   ==";
    }

private:
    uint64_t total_trades_{0}, depth_updates_{0};
    double   total_volume_{0}, buy_volume_{0}, sell_volume_{0};
    std::deque<double> ofi_buffer_, size_buffer_;
    std::deque<std::pair<double,double>> trade_prices_;
    double   vwap_num_{0}, vwap_den_{0};
    double   tw_spread_sum_{0};  uint64_t tw_spread_count_{0};
    double   spread_sum_{0};     uint64_t spread_count_{0};
    double   kyle_sxx_{0}, kyle_sxy_{0}, kyle_lambda_{0};
    double   last_mid_{0}, last_spread_{0}, last_ofi_book_{0}, last_trade_price_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Dashboard — scrolling print, no screen clear, works in any terminal
// ─────────────────────────────────────────────────────────────────────────────
class Dashboard {
public:
    explicit Dashboard(const std::string& symbol) : symbol_(symbol) {}

    void render(const LocalOrderBook& book, const LiveAnalytics& a,
                uint64_t msg_count, double uptime_sec) {

        // Print a separator so each update is clearly visible
        std::printf("\n-------- %s  uptime=%.0fs  msgs=%llu  trades=%llu --------\n",
                    symbol_.c_str(), uptime_sec,
                    (unsigned long long)msg_count,
                    (unsigned long long)a.total_trades());

        std::printf("  BID  %10.2f  (%.4f BTC)  |  MID %10.2f  |  ASK  %10.2f  (%.4f BTC)\n",
                    book.best_bid(), book.best_bid_qty(),
                    book.mid(),
                    book.best_ask(), book.best_ask_qty());

        std::printf("  Spread: %.4f USDT (%.3f bps)\n",
                    book.spread(), book.spread_bps());

        // Top 3 levels each side
        auto asks = book.top_asks(3);
        auto bids = book.top_bids(3);
        std::printf("  ASKS: ");
        for (int i = (int)asks.size()-1; i >= 0; --i)
            std::printf("%.2f x %.4f  ", asks[i].first, asks[i].second);
        std::printf("\n  BIDS: ");
        for (auto& [px, qty] : bids)
            std::printf("%.2f x %.4f  ", px, qty);
        std::printf("\n");

        std::printf("  VWAP=%.2f  AvgSz=%.4f BTC  Buy%%=%.1f%%  Sell%%=%.1f%%\n",
                    a.vwap(), a.avg_trade_size(),
                    a.buy_sell_ratio()*100.0, (1.0-a.buy_sell_ratio())*100.0);

        std::printf("  OFI=%+.3f (norm=%+.3f)  BookOFI=%+.3f  Kyle-Lambda=%.6f\n",
                    a.rolling_ofi(), a.normalized_ofi(),
                    a.book_ofi(), a.kyle_lambda());

        std::printf("  Signal: [ %s ]\n", a.flow_signal_str());

        std::fflush(stdout);
    }

private:
    std::string symbol_;
};

} // namespace hydra