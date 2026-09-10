#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FeedPublisher: market data dissemination layer
//
//  Replicates real exchange feed architecture:
//    - Multicast incremental feed (L2 deltas + trades)
//    - Snapshot feed for recovery (full book every N seq#s)
//    - Sequence numbers for gap detection
//    - Heartbeats to detect feed outage
//
//  Consumers track their own cursor in the RingBuffer.
//  If they fall behind > RING_SIZE updates, they must use snapshot recovery.
// ─────────────────────────────────────────────────────────────────────────────

#include "../engine/matching_engine/trade_event.cpp"
#include "../engine/orderbook/limit_order_book.cpp"
#include "../infrastructure/ring_buffer.hpp"

#include <functional>
#include <unordered_map>
#include <vector>
#include <cstdio>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  FeedSubscriber: interface for market data consumers
// ─────────────────────────────────────────────────────────────────────────────
class FeedSubscriber {
public:
    virtual ~FeedSubscriber() = default;
    virtual void on_l1(InstrumentId, Price bid, Quantity bid_qty,
                       Price ask, Quantity ask_qty, Timestamp) noexcept = 0;
    virtual void on_l2_delta(InstrumentId, Side, Price, Quantity,
                              bool is_delete, SequenceNum, Timestamp) noexcept = 0;
    virtual void on_trade(const TradeEvent&) noexcept = 0;
    virtual void on_snapshot(const BookSnapshot&) noexcept = 0;
    virtual void on_heartbeat(Timestamp) noexcept {}
    virtual void on_sequence_gap(SequenceNum expected,
                                  SequenceNum received) noexcept {
        std::fprintf(stderr, "[FEED] Sequence gap: expected %llu got %llu\n",
                     static_cast<unsigned long long>(expected),
                     static_cast<unsigned long long>(received));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  FeedPublisher
// ─────────────────────────────────────────────────────────────────────────────
class FeedPublisher {
    static constexpr std::size_t RING_SIZE        = 65536;
    static constexpr uint32_t    SNAPSHOT_INTERVAL = 10000; // seq msgs between snapshots
    static constexpr int64_t     HEARTBEAT_INTERVAL_NS = 1'000'000'000LL; // 1 second

public:
    using SnapshotSource = std::function<BookSnapshot(InstrumentId, Timestamp)>;

    explicit FeedPublisher(SnapshotSource snap_fn) noexcept
        : snap_fn_(std::move(snap_fn))
    {}

    // ── Subscriber management ─────────────────────────────────────────────────
    void subscribe(FeedSubscriber* sub) {
        subscribers_.push_back(sub);
        // Deliver current snapshot immediately on subscribe
        cursors_[sub] = RingBuffer<MarketDataUpdate, RING_SIZE>::Cursor{};
    }

    void unsubscribe(FeedSubscriber* sub) {
        subscribers_.erase(
            std::remove(subscribers_.begin(), subscribers_.end(), sub),
            subscribers_.end());
        cursors_.erase(sub);
    }

    // ── Called by MatchingEngine callbacks ────────────────────────────────────
    void publish(const MarketDataUpdate& mdu) {
        SequenceNum seq = ring_.publish(mdu);
        ++msg_count_;

        // Deliver to all in-process subscribers immediately
        for (auto* sub : subscribers_) {
            deliver(sub, mdu, seq);
        }

        // Periodic full snapshot
        if (seq % SNAPSHOT_INTERVAL == 0 && snap_fn_) {
            auto snap = snap_fn_(mdu.instrument_id, mdu.timestamp);
            for (auto* sub : subscribers_)
                sub->on_snapshot(snap);
        }
    }

    void publish_heartbeat(Timestamp ts) {
        MarketDataUpdate hb;
        hb.type      = MarketDataUpdate::Type::Heartbeat;
        hb.timestamp = ts;
        hb.seq       = ++msg_count_;

        for (auto* sub : subscribers_)
            sub->on_heartbeat(ts);
    }

    // ── Stats ─────────────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t msg_count()       const noexcept { return msg_count_; }
    [[nodiscard]] uint64_t subscriber_count() const noexcept { return subscribers_.size(); }

    // ── Replay from ring for late/reconnecting subscriber ─────────────────────
    void replay_from(FeedSubscriber* sub, SequenceNum from_seq) {
        auto& cursor = cursors_[sub];
        cursor.read_seq = from_seq;

        RingBuffer<MarketDataUpdate, RING_SIZE>::Envelope env;
        while (ring_.try_read(cursor, env)) {
            if (ring_.is_overrun(cursor)) {
                sub->on_sequence_gap(from_seq, ring_.published_count());
                // Force snapshot recovery
                if (snap_fn_) {
                    auto snap = snap_fn_(env.data.instrument_id, env.data.timestamp);
                    sub->on_snapshot(snap);
                }
                return;
            }
            deliver(sub, env.data, env.sequence);
        }
    }

private:
    void deliver(FeedSubscriber* sub, const MarketDataUpdate& mdu,
                 SequenceNum /*seq*/) noexcept {
        switch (mdu.type) {
            case MarketDataUpdate::Type::L1Quote:
                sub->on_l1(mdu.instrument_id,
                            mdu.l1.bid_px, mdu.l1.bid_qty,
                            mdu.l1.ask_px, mdu.l1.ask_qty,
                            mdu.timestamp);
                break;

            case MarketDataUpdate::Type::L2Update:
                sub->on_l2_delta(mdu.instrument_id,
                                  mdu.l2.side, mdu.l2.price, mdu.l2.qty,
                                  mdu.l2.is_delete, mdu.seq, mdu.timestamp);
                break;

            case MarketDataUpdate::Type::Trade:
                sub->on_trade(mdu.trade);
                break;

            default:
                break;
        }
    }

    RingBuffer<MarketDataUpdate, RING_SIZE>              ring_;
    std::vector<FeedSubscriber*>                         subscribers_;
    std::unordered_map<FeedSubscriber*,
        RingBuffer<MarketDataUpdate, RING_SIZE>::Cursor>  cursors_;
    SnapshotSource                                        snap_fn_;
    uint64_t                                              msg_count_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  LocalBookBuilder: subscriber that rebuilds a local LOB from feed
//  (Strategy-side book reconstruction from raw market data)
// ─────────────────────────────────────────────────────────────────────────────
class LocalBookBuilder : public FeedSubscriber {
public:
    LocalBookBuilder() {}

    void set_instrument(InstrumentId id) noexcept {
        instrument_id_ = id;
        local_bid_map_.clear();
        local_ask_map_.clear();
    }

    // ── FeedSubscriber interface ───────────────────────────────────────────────
    void on_l1(InstrumentId, Price bid, Quantity, Price ask, Quantity,
               Timestamp) noexcept override {
        last_bid_ = bid;
        last_ask_ = ask;
    }

    void on_l2_delta(InstrumentId, Side side, Price price, Quantity qty,
                     bool is_delete, SequenceNum seq, Timestamp ts) noexcept override {
        if (seq != last_seq_ + 1 && last_seq_ != 0) {
            // Gap detected — need snapshot recovery
            needs_recovery_ = true;
            return;
        }
        last_seq_ = seq;

        // Update local bid/ask tracking map
        auto& side_map = (side == Side::Buy) ? local_bid_map_ : local_ask_map_;
        if (is_delete || qty == 0)
            side_map.erase(price);
        else
            side_map[price] = qty;
    }

    void on_trade(const TradeEvent& te) noexcept override {
        last_trade_px_  = te.price;
        last_trade_qty_ = te.quantity;
        ++trade_count_;
    }

    void on_snapshot(const BookSnapshot& snap) noexcept override {
        local_bid_map_.clear();
        local_ask_map_.clear();
        for (auto& lvl : snap.bids) local_bid_map_[lvl.price] = lvl.qty;
        for (auto& lvl : snap.asks) local_ask_map_[lvl.price] = lvl.qty;
        needs_recovery_ = false;
    }

    // ── Accessors ─────────────────────────────────────────────────────────────
    [[nodiscard]] Price best_bid() const noexcept {
        return local_bid_map_.empty() ? INVALID_PRICE : local_bid_map_.rbegin()->first;
    }
    [[nodiscard]] Price best_ask() const noexcept {
        return local_ask_map_.empty() ? INVALID_PRICE : local_ask_map_.begin()->first;
    }
    [[nodiscard]] Price mid() const noexcept {
        Price b = best_bid(), a = best_ask();
        if (b == INVALID_PRICE || a == INVALID_PRICE) return INVALID_PRICE;
        return (b + a) / 2;
    }
    [[nodiscard]] bool needs_recovery() const noexcept { return needs_recovery_; }

    [[nodiscard]] Quantity qty_at(Side side, Price price) const noexcept {
        const auto& m = (side == Side::Buy) ? local_bid_map_ : local_ask_map_;
        auto it = m.find(price);
        return (it != m.end()) ? it->second : 0;
    }

    void print_top(int levels = 5) const {
        std::printf("\n  ── Local Book ──\n");
        int n = 0;
        for (auto it = local_ask_map_.begin(); it != local_ask_map_.end() && n < levels; ++it, ++n)
            std::printf("  ASK  %6lld  @ %6lld\n",
                static_cast<long long>(it->second), static_cast<long long>(it->first));
        std::printf("  ───────────────\n");
        n = 0;
        for (auto it = local_bid_map_.rbegin(); it != local_bid_map_.rend() && n < levels; ++it, ++n)
            std::printf("  BID  %6lld  @ %6lld\n",
                static_cast<long long>(it->second), static_cast<long long>(it->first));
    }

private:
    InstrumentId   instrument_id_{0};

    std::map<Price, Quantity> local_bid_map_;  // ascending
    std::map<Price, Quantity> local_ask_map_;  // ascending

    Price      last_bid_{INVALID_PRICE};
    Price      last_ask_{INVALID_PRICE};
    Price      last_trade_px_{0};
    Quantity   last_trade_qty_{0};
    SequenceNum last_seq_{0};
    uint64_t   trade_count_{0};
    bool       needs_recovery_{false};
};

} // namespace hydra
