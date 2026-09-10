#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <functional>
#include <cmath>
#include <stdexcept>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal JSON helpers — no external dependencies
// ─────────────────────────────────────────────────────────────────────────────
struct JsonExtract {

    // Get quoted string value: "key":"value" -> "value"
    static std::string str(const std::string& j, const std::string& key) {
        std::string search = "\"" + key + "\":\"";
        auto pos = j.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = j.find('"', pos);
        return end == std::string::npos ? "" : j.substr(pos, end - pos);
    }

    // Get numeric value (Binance sends numbers as quoted strings)
    static double num(const std::string& j, const std::string& key) {
        // Try quoted first: "key":"123.45"
        std::string sq = "\"" + key + "\":\"";
        auto pos = j.find(sq);
        if (pos != std::string::npos) {
            pos += sq.size();
            auto end = j.find('"', pos);
            if (end != std::string::npos) {
                try { return std::stod(j.substr(pos, end - pos)); } catch (...) {}
            }
        }
        // Try unquoted: "key":123.45
        std::string su = "\"" + key + "\":";
        pos = j.find(su);
        if (pos == std::string::npos) return 0.0;
        pos += su.size();
        while (pos < j.size() && j[pos] == ' ') ++pos;
        size_t end = pos;
        while (end < j.size() && (std::isdigit(j[end]) || j[end]=='.' ||
               j[end]=='-' || j[end]=='e' || j[end]=='E' || j[end]=='+')) ++end;
        try { return std::stod(j.substr(pos, end - pos)); } catch (...) { return 0.0; }
    }

    static bool boolean(const std::string& j, const std::string& key) {
        std::string search = "\"" + key + "\":";
        auto pos = j.find(search);
        if (pos == std::string::npos) return false;
        pos += search.size();
        while (pos < j.size() && j[pos] == ' ') ++pos;
        return pos + 4 <= j.size() && j.substr(pos, 4) == "true";
    }

    // Parse array of ["price","qty"] pairs — fixed bracket-depth parser
    static std::vector<std::pair<double,double>>
    price_levels(const std::string& j, const std::string& key) {
        std::vector<std::pair<double,double>> result;

        // Find "b":[ or "a":[
        std::string search = "\"" + key + "\":[";
        auto outer = j.find(search);
        if (outer == std::string::npos) return result;

        size_t i = outer + search.size();  // points to first char inside outer [

        while (i < j.size()) {
            // Skip whitespace and commas between entries
            while (i < j.size() && (j[i]==' '||j[i]==','||j[i]=='\n'||j[i]=='\r'||j[i]=='\t'))
                ++i;

            if (i >= j.size()) break;
            if (j[i] == ']') break;   // end of outer array
            if (j[i] != '[') { ++i; continue; }  // skip unexpected chars

            ++i;  // skip opening [ of entry

            // Read price string (format: "70750.00")
            while (i < j.size() && (j[i]==' '||j[i]=='"')) ++i;
            size_t px_start = i;
            while (i < j.size() && j[i] != '"') ++i;
            if (i >= j.size()) break;
            std::string px_str = j.substr(px_start, i - px_start);
            ++i;  // skip closing "

            // Skip comma between price and qty
            while (i < j.size() && (j[i]==','||j[i]==' ')) ++i;

            // Read qty string (format: "0.12345")
            if (i < j.size() && j[i]=='"') ++i;  // skip opening "
            size_t qty_start = i;
            while (i < j.size() && j[i] != '"') ++i;
            if (i >= j.size()) break;
            std::string qty_str = j.substr(qty_start, i - qty_start);
            ++i;  // skip closing "

            // Skip to ] closing this entry
            while (i < j.size() && j[i] != ']') ++i;
            if (i < j.size()) ++i;  // skip ]

            // Parse and store
            if (!px_str.empty() && !qty_str.empty()) {
                try {
                    result.push_back({std::stod(px_str), std::stod(qty_str)});
                } catch (...) {}
            }
        }
        return result;
    }

    // Extract inner object after "data":{ in combined streams
    static std::string extract_data_object(const std::string& j) {
        auto pos = j.find("\"data\":{");
        if (pos == std::string::npos) return j;
        size_t start = pos + 7;  // points to {
        int depth = 0;
        for (size_t i = start; i < j.size(); ++i) {
            if (j[i] == '{') ++depth;
            else if (j[i] == '}') { --depth; if (depth == 0) return j.substr(start, i - start + 1); }
        }
        return j.substr(start);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Binance message types
// ─────────────────────────────────────────────────────────────────────────────
struct BinanceTrade {
    uint64_t    trade_id{0};
    std::string symbol;
    double      price{0.0};
    double      quantity{0.0};
    int64_t     timestamp_ms{0};
    bool        buyer_is_maker{false};
    bool is_buy_aggressor() const { return !buyer_is_maker; }
};

struct BinanceDepthUpdate {
    std::string symbol;
    uint64_t    first_update_id{0};
    uint64_t    last_update_id{0};
    int64_t     event_time_ms{0};
    std::vector<std::pair<double,double>> bids;
    std::vector<std::pair<double,double>> asks;
};

// ─────────────────────────────────────────────────────────────────────────────
//  BinanceStream — parses raw JSON messages
// ─────────────────────────────────────────────────────────────────────────────
class BinanceStream {
public:
    using OnTrade = std::function<void(const BinanceTrade&)>;
    using OnDepth = std::function<void(const BinanceDepthUpdate&)>;

    void set_trade_cb(OnTrade cb) { on_trade_ = std::move(cb); }
    void set_depth_cb(OnDepth cb) { on_depth_ = std::move(cb); }

    void parse(const std::string& raw) {
        // Combined stream: unwrap {"stream":"...","data":{...}}
        std::string json = raw;
        if (raw.find("\"data\":{") != std::string::npos)
            json = JsonExtract::extract_data_object(raw);

        std::string event = JsonExtract::str(json, "e");
        if      (event == "trade")       parse_trade(json);
        else if (event == "depthUpdate") parse_depth(json);
    }

private:
    void parse_trade(const std::string& j) {
        if (!on_trade_) return;
        BinanceTrade t;
        t.trade_id       = static_cast<uint64_t>(JsonExtract::num(j, "t"));
        t.symbol         = JsonExtract::str(j, "s");
        t.price          = JsonExtract::num(j, "p");
        t.quantity       = JsonExtract::num(j, "q");
        t.timestamp_ms   = static_cast<int64_t>(JsonExtract::num(j, "T"));
        t.buyer_is_maker = JsonExtract::boolean(j, "m");
        if (t.price > 0 && t.quantity > 0) on_trade_(t);
    }

    void parse_depth(const std::string& j) {
        if (!on_depth_) return;
        BinanceDepthUpdate d;
        d.symbol          = JsonExtract::str(j, "s");
        d.event_time_ms   = static_cast<int64_t>(JsonExtract::num(j, "E"));
        d.first_update_id = static_cast<uint64_t>(JsonExtract::num(j, "U"));
        d.last_update_id  = static_cast<uint64_t>(JsonExtract::num(j, "u"));
        d.bids            = JsonExtract::price_levels(j, "b");
        d.asks            = JsonExtract::price_levels(j, "a");
        on_depth_(d);
    }

    OnTrade on_trade_;
    OnDepth on_depth_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  LocalOrderBook — reconstructs L2 from Binance depth stream
// ─────────────────────────────────────────────────────────────────────────────
class LocalOrderBook {
public:
    explicit LocalOrderBook(std::string symbol = "BTCUSDT")
        : symbol_(std::move(symbol)) {}

    void apply_update(const BinanceDepthUpdate& upd) {
        for (auto& [px, qty] : upd.bids) {
            if (qty == 0.0) bids_.erase(px);
            else            bids_[px] = qty;
        }
        for (auto& [px, qty] : upd.asks) {
            if (qty == 0.0) asks_.erase(px);
            else            asks_[px] = qty;
        }
        last_update_id_ = upd.last_update_id;
        ++update_count_;
    }

    double best_bid()     const { return bids_.empty() ? 0.0 : bids_.rbegin()->first; }
    double best_ask()     const { return asks_.empty() ? 0.0 : asks_.begin()->first; }
    double best_bid_qty() const { return bids_.empty() ? 0.0 : bids_.rbegin()->second; }
    double best_ask_qty() const { return asks_.empty() ? 0.0 : asks_.begin()->second; }
    double mid()    const { double b=best_bid(),a=best_ask(); return (b>0&&a>0)?(b+a)/2.0:0.0; }
    double spread() const { double b=best_bid(),a=best_ask(); return (b>0&&a>0)?(a-b):0.0; }
    double spread_bps() const { double m=mid(); return m>0?spread()/m*10000.0:0.0; }

    double ofi_top() const {
        double b=best_bid_qty(), a=best_ask_qty();
        return (b+a>0) ? (b-a)/(b+a) : 0.0;
    }

    // top N ask levels, best (lowest) first
    std::vector<std::pair<double,double>> top_asks(int n=5) const {
        std::vector<std::pair<double,double>> r;
        int c=0;
        for (auto it=asks_.begin(); it!=asks_.end()&&c<n; ++it,++c)
            r.push_back({it->first, it->second});
        return r;
    }

    // top N bid levels, best (highest) first
    std::vector<std::pair<double,double>> top_bids(int n=5) const {
        std::vector<std::pair<double,double>> r;
        int c=0;
        for (auto it=bids_.rbegin(); it!=bids_.rend()&&c<n; ++it,++c)
            r.push_back({it->first, it->second});
        return r;
    }

    bool     empty()          const { return bids_.empty()&&asks_.empty(); }
    uint64_t last_update_id() const { return last_update_id_; }
    uint64_t update_count()   const { return update_count_; }
    const std::string& symbol() const { return symbol_; }

private:
    std::string symbol_;
    std::map<double,double> bids_;   // ascending; best = rbegin
    std::map<double,double> asks_;   // ascending; best = begin
    uint64_t last_update_id_{0};
    uint64_t update_count_{0};
};

} // namespace hydra