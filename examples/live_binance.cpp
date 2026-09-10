// HydraExchange — Live Binance Feed (simple scrolling output)

#include "../binance/ws_client.hpp"
#include "../binance/binance_stream.hpp"
#include "../binance/live_analytics.hpp"

#include <cstdio>
#include <string>
#include <atomic>
#include <csignal>
#include <algorithm>

static std::atomic<bool> g_running{true};

#ifdef _WIN32
#include <windows.h>
static BOOL WINAPI ctrl_handler(DWORD) { g_running = false; return TRUE; }
#else
static void sig_handler(int) { g_running = false; }
#endif

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}
static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

int main(int argc, char** argv) {
    std::string symbol_upper = (argc >= 2) ? to_upper(argv[1]) : "BTCUSDT";
    std::string symbol_lower = to_lower(symbol_upper);

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);
#endif

    std::printf("\n=================================================================\n");
    std::printf("  HydraExchange | Binance Live Feed | %s\n", symbol_upper.c_str());
    std::printf("=================================================================\n");
    std::printf("  Connecting to stream.binance.com:9443 ...\n\n");
    std::fflush(stdout);

    hydra::LocalOrderBook book(symbol_upper);
    hydra::LiveAnalytics  analytics;
    hydra::BinanceStream  parser;

    uint64_t msg_count  = 0;
    uint64_t print_every = 5;   // print one line every 20 messages (~2 sec)

    parser.set_depth_cb([&](const hydra::BinanceDepthUpdate& upd) {
        book.apply_update(upd);
        analytics.on_depth(book);
    });

    parser.set_trade_cb([&](const hydra::BinanceTrade& t) {
        analytics.on_trade(t);
    });

    std::string path = "/stream?streams="
                     + symbol_lower + "@depth@100ms/"
                     + symbol_lower + "@trade";

    int reconnect_sec = 1;

    while (g_running) {
        hydra::WsClient ws;
        ws.set_error_cb([](const std::string& e) {
            std::fprintf(stderr, "[WS ERROR] %s\n", e.c_str());
        });

        if (!ws.connect("stream.binance.com", path, 9443)) {
            std::fprintf(stderr, "  Connection failed. Retrying in %ds...\n", reconnect_sec);
            for (int i = 0; i < reconnect_sec * 10 && g_running; ++i) {
#ifdef _WIN32
                Sleep(100);
#else
                usleep(100000);
#endif
            }
            reconnect_sec = std::min(reconnect_sec * 2, 30);
            continue;
        }

        reconnect_sec = 1;
        std::printf("  Connected! Receiving %s data...\n\n", symbol_upper.c_str());
        std::fflush(stdout);

        ws.set_message_cb([&](const std::string& msg) {
            if (!g_running) return;

            // Debug: print first 3 raw messages so we can see the format
            if (msg_count < 3) {
                std::printf("\n[RAW MSG #%llu len=%zu]:\n%s\n\n",
                    (unsigned long long)msg_count,
                    msg.size(),
                    msg.substr(0, 400).c_str());
                std::fflush(stdout);
            }

            parser.parse(msg);
            ++msg_count;

            // Print a snapshot every N messages
            if (msg_count % print_every == 0) {
                std::printf("--- %s  msgs=%-6llu  trades=%-6llu ---\n",
                    symbol_upper.c_str(),
                    (unsigned long long)msg_count,
                    (unsigned long long)analytics.total_trades());

                std::printf("  BID  %10.2f  qty=%.4f  |  MID  %10.2f  |  ASK  %10.2f  qty=%.4f\n",
                    book.best_bid(), book.best_bid_qty(),
                    book.mid(),
                    book.best_ask(), book.best_ask_qty());

                std::printf("  Spread=%.4f USDT (%.3f bps)\n",
                    book.spread(), book.spread_bps());

                // Top 3 levels
                auto asks = book.top_asks(3);
                auto bids = book.top_bids(3);

                std::printf("  ASK: ");
                for (int i = (int)asks.size()-1; i >= 0; --i)
                    std::printf("%.2f x %.4f  ", asks[i].first, asks[i].second);
                std::printf("\n  BID: ");
                for (auto& [px, qty] : bids)
                    std::printf("%.2f x %.4f  ", px, qty);

                std::printf("\n  VWAP=%.2f  AvgSz=%.5f BTC  Buy=%.1f%%  Sell=%.1f%%\n",
                    analytics.vwap(),
                    analytics.avg_trade_size(),
                    analytics.buy_sell_ratio() * 100.0,
                    (1.0 - analytics.buy_sell_ratio()) * 100.0);

                std::printf("  OFI=%+.3f (norm=%+.3f)  BookOFI=%+.3f  Lambda=%.6f\n",
                    analytics.rolling_ofi(),
                    analytics.normalized_ofi(),
                    analytics.book_ofi(),
                    analytics.kyle_lambda());

                std::printf("  Signal: [ %s ]\n\n",
                    analytics.flow_signal_str());

                std::fflush(stdout);
            }
        });

        ws.run_loop();

        if (g_running) {
            std::fprintf(stderr, "  Disconnected. Reconnecting in %ds...\n", reconnect_sec);
            for (int i = 0; i < reconnect_sec * 10 && g_running; ++i) {
#ifdef _WIN32
                Sleep(100);
#else
                usleep(100000);
#endif
            }
            reconnect_sec = std::min(reconnect_sec * 2, 30);
        }
    }

    std::printf("\n  Stopped. Total messages: %llu  Trades: %llu\n",
        (unsigned long long)msg_count,
        (unsigned long long)analytics.total_trades());
    return 0;
}