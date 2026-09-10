#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MatchingEngine
//
//  Orchestrates the full order lifecycle:
//    1. Receives OrderRequest from Gateway
//    2. Allocates Order from pool
//    3. Runs pre-match against LOB
//    4. Rests order if not fully filled
//    5. Generates TradeEvents + ExecutionReports
//    6. Publishes MarketDataUpdates to feed
//
//  Price-time priority, deterministic, single-threaded.
//  All I/O is done via callbacks to decouple from network layer.
// ─────────────────────────────────────────────────────────────────────────────

#include "trade_event.cpp"
#include "../orderbook/limit_order_book.cpp"
#include "../../infrastructure/memory_pool.hpp"
#include "../../infrastructure/ring_buffer.hpp"
#include "../../infrastructure/timestamp.hpp"

#include <functional>
#include <unordered_map>
#include <memory>
#include <vector>
#include <cstdint>
#include <cassert>
#include <cstdio>

namespace hydra {

class MatchingEngine {
    static constexpr std::size_t ORDER_POOL_SIZE = 1'000'000;
    static constexpr std::size_t MD_RING_SIZE    = 65536;

public:
    // ── Callbacks ─────────────────────────────────────────────────────────────
    using ExecReportCb  = std::function<void(const ExecutionReport&)>;
    using TradeEventCb  = std::function<void(const TradeEvent&)>;
    using MarketDataCb  = std::function<void(const MarketDataUpdate&)>;

    MatchingEngine() = default;

    // ── Configuration ─────────────────────────────────────────────────────────
    void set_exec_report_cb (ExecReportCb  cb) { exec_report_cb_  = std::move(cb); }
    void set_trade_event_cb (TradeEventCb  cb) { trade_event_cb_  = std::move(cb); }
    void set_market_data_cb (MarketDataCb  cb) { market_data_cb_  = std::move(cb); }

    // Register an instrument (creates a LOB for it)
    void add_instrument(InstrumentId id) {
        if (!books_.count(id))
            books_.emplace(id, std::make_unique<LimitOrderBook>(id));
    }

    void set_market_state(InstrumentId id, MarketState state) {
        market_states_[id] = state;
    }

    void set_clock(SimClock* clk) noexcept { sim_clock_ = clk; }

    // ── Main entry point: process one OrderRequest ────────────────────────────
    void process(const OrderRequest& req) {
        switch (req.action) {
            case OrderRequest::Action::New:    handle_new(req);    break;
            case OrderRequest::Action::Cancel: handle_cancel(req); break;
            case OrderRequest::Action::Modify: handle_modify(req); break;
        }
    }

    // ── LOB accessors (for agents and analytics) ──────────────────────────────
    [[nodiscard]] LimitOrderBook* book(InstrumentId id) noexcept {
        auto it = books_.find(id);
        return (it != books_.end()) ? it->second.get() : nullptr;
    }

    [[nodiscard]] const LimitOrderBook* book(InstrumentId id) const noexcept {
        auto it = books_.find(id);
        return (it != books_.end()) ? it->second.get() : nullptr;
    }

    // ── Counters ──────────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t total_orders()  const noexcept { return stat_orders_;  }
    [[nodiscard]] uint64_t total_trades()  const noexcept { return stat_trades_;  }
    [[nodiscard]] uint64_t total_cancels() const noexcept { return stat_cancels_; }

private:
    // ── SimClock integration ──────────────────────────────────────────────────
    [[nodiscard]] Timestamp now() const noexcept {
        return sim_clock_ ? sim_clock_->now() : Clock::now_wall();
    }

    // ── Order ID / Trade ID generators ───────────────────────────────────────
    [[nodiscard]] OrderId  next_order_id()  noexcept { return ++order_seq_; }
    [[nodiscard]] TradeId  next_trade_id()  noexcept { return ++trade_seq_; }
    [[nodiscard]] SequenceNum next_seq()    noexcept { return ++msg_seq_;   }

    struct TakerFillAgg {
        Price    last_px{0};
        Quantity last_qty{0};
        int64_t  notional_ticks{0};
        Quantity total_qty{0};
    };

    // ── Handle new order ─────────────────────────────────────────────────────
    void handle_new(const OrderRequest& req) {
        ++stat_orders_;
        Timestamp ts = now();

        auto* bk = book(req.instrument_id);
        if (!bk) {
            send_reject(req, RejectReason::UnknownInstrument, ts);
            return;
        }

        // Market state check
        auto ms_it = market_states_.find(req.instrument_id);
        if (ms_it != market_states_.end() &&
            ms_it->second != MarketState::Open) {
            send_reject(req, RejectReason::MarketHalted, ts);
            return;
        }

        // Basic validation
        if (req.quantity == 0) {
            send_reject(req, RejectReason::InvalidQuantity, ts);
            return;
        }
        if (req.type == OrderType::Limit && req.price <= 0) {
            send_reject(req, RejectReason::InvalidPrice, ts);
            return;
        }

        // Allocate order from pool
        Order* o = pool_->construct();
        o->id            = next_order_id();
        o->client_id     = req.client_id;
        o->instrument_id = req.instrument_id;
        o->price         = req.price;
        o->original_qty  = req.quantity;
        o->leaves_qty    = req.quantity;
        o->filled_qty    = 0;
        o->side          = req.side;
        o->type          = req.type;
        o->tif           = req.tif;
        o->status        = OrderStatus::New;
        o->submit_time   = req.timestamp;
        o->entry_time    = ts;
        o->last_update   = ts;
        o->prev = o->next = nullptr;

        // PostOnly: reject if would cross
        if (req.type == OrderType::PostOnly) {
            bool would_cross = (req.side == Side::Buy  && bk->has_ask() && req.price >= bk->best_ask()) ||
                               (req.side == Side::Sell && bk->has_bid() && req.price <= bk->best_bid());
            if (would_cross) {
                pool_->destroy(o);
                send_reject(req, RejectReason::WouldCross, ts);
                return;
            }
        }

        // ── Send ACK ────────────────────────────────────────────────────────
        send_ack(o, ts);

        // ── Market order: match immediately ──────────────────────────────────
        if (req.type == OrderType::Market) {
            auto fills = bk->match(o);
            TakerFillAgg agg;
            for (auto& f : fills)
                emit_trade(bk, o, f.maker_order, f.fill_qty, f.fill_price, ts, agg);
            send_exec_report_taker(o, ts, agg);
            if (o->leaves_qty > 0) {
                // Unfilled market order is cancelled
                o->cancel();
                send_exec_report_cancel(o, ts);
            }
            pool_->destroy(o);
            publish_l1(bk, ts);
            return;
        }

        // ── FOK: all-or-nothing, checked BEFORE touching the book ─────────────
        // bk->match() has real, permanent side effects — it consumes resting
        // maker liquidity and emits real TradeEvents. Running it first and
        // only inspecting afterwards whether the taker happened to end up
        // fully filled (the previous logic) is IOC semantics, not FOK: an
        // FOK order that cannot fully fill must never execute against so
        // much as one resting order. Check feasibility on the untouched book
        // and reject the whole thing up front if it would not fully fill.
        if (req.tif == TimeInForce::FOK &&
            !bk->can_fully_fill(o->side, o->price, o->leaves_qty, o->is_market())) {
            o->cancel();
            send_exec_report_cancel(o, ts);
            pool_->destroy(o);
            publish_l1(bk, ts);
            return;
        }

        // ── Limit order: try match then rest ──────────────────────────────────
        auto fills = bk->match(o);
        TakerFillAgg agg;
        for (auto& f : fills)
            emit_trade(bk, o, f.maker_order, f.fill_qty, f.fill_price, ts, agg);

        if (o->fully_filled()) {
            send_exec_report_taker(o, ts, agg);
            pool_->destroy(o);
            publish_l1(bk, ts);
            return;
        }

        // IOC: cancel remainder
        if (req.tif == TimeInForce::IOC) {
            if (o->filled_qty > 0) send_exec_report_taker(o, ts, agg);
            o->cancel();
            send_exec_report_cancel(o, ts);
            pool_->destroy(o);
            publish_l1(bk, ts);
            return;
        }

        // FOK is unreachable here now: an infeasible FOK already returned
        // above against the untouched book, and a feasible one is guaranteed
        // fully filled by the same can_fully_fill() check, so it already
        // returned via the o->fully_filled() branch above. Kept as a
        // defensive invariant check, not a live code path.
        if (req.tif == TimeInForce::FOK) {
            assert(false && "FOK order partially matched despite passing can_fully_fill()");
            o->cancel();
            send_exec_report_cancel(o, ts);
            pool_->destroy(o);
            return;
        }

        // Rest in book
        if (o->filled_qty > 0) send_exec_report_taker(o, ts, agg);
        bk->add_order(o);
        publish_l1(bk, ts);
        publish_l2_add(bk, o, ts);
    }

    // ── Handle cancel ─────────────────────────────────────────────────────────
    void handle_cancel(const OrderRequest& req) {
        ++stat_cancels_;
        Timestamp ts = now();

        auto* bk = book(req.instrument_id);
        if (!bk) return;

        Order* o = bk->find_order(req.order_id);
        if (!o) return;

        Price    px               = o->price;
        Side     side             = o->side;
        Quantity filled_at_cancel = o->filled_qty;

        // find_order() just succeeded against the same book/index, and the
        // engine is single-threaded, so this cannot fail; assert the
        // invariant rather than silently discarding the [[nodiscard]] result.
        [[maybe_unused]] bool removed = bk->cancel_order(req.order_id);
        assert(removed && "order_index_ and find_order() disagree");

        ExecutionReport er;
        er.exec_type    = ExecutionReport::ExecType::Cancelled;
        er.order_id     = req.order_id;
        er.client_id    = req.client_id;
        er.instrument_id = req.instrument_id;
        er.leaves_qty   = 0;
        er.cum_qty      = filled_at_cancel;
        er.status       = OrderStatus::Cancelled;
        er.timestamp    = ts;
        er.seq_num      = next_seq();
        if (exec_report_cb_) exec_report_cb_(er);

        // Publish L2 delta (level removed or qty reduced)
        publish_l2_remove(bk, side, px, ts);
        publish_l1(bk, ts);

        // `o` has now fully left the book (unlinked from its PriceLevel and
        // erased from order_index_ by cancel_order() above) and every report
        // that needed its fields has already been sent. Return its slot —
        // without this, every cancelled order permanently leaks a pool slot
        // and a long-running or high-cancel-rate simulation eventually fails
        // with std::bad_alloc even though far fewer than ORDER_POOL_SIZE
        // orders are ever resting at once. See tests/test_memory_lifecycle.cpp.
        pool_->destroy(o);
    }

    // ── Handle modify (cancel-replace) ────────────────────────────────────────
    void handle_modify(const OrderRequest& req) {
        // Cancel-replace: cancel old, insert new
        OrderRequest cancel_req = req;
        cancel_req.action = OrderRequest::Action::Cancel;
        handle_cancel(cancel_req);

        OrderRequest new_req = req;
        new_req.action = OrderRequest::Action::New;
        handle_new(new_req);
    }

    // ── Trade emission ────────────────────────────────────────────────────────
    void emit_trade(LimitOrderBook* bk,
                    Order* taker, Order* maker,
                    Quantity qty, Price px, Timestamp ts,
                    TakerFillAgg& taker_agg) {
        ++stat_trades_;
        taker_agg.last_px  = px;
        taker_agg.last_qty = qty;
        taker_agg.notional_ticks += static_cast<int64_t>(px) * static_cast<int64_t>(qty);
        taker_agg.total_qty += qty;

        TradeEvent te;
        te.trade_id       = next_trade_id();
        te.instrument_id  = taker->instrument_id;
        te.maker_order_id = maker->id;
        te.taker_order_id = taker->id;
        te.maker_client_id = maker->client_id;
        te.taker_client_id = taker->client_id;
        te.price          = px;
        te.quantity       = qty;
        te.aggressor_side = taker->side;
        te.timestamp      = ts;
        te.seq_num        = next_seq();

        if (trade_event_cb_) trade_event_cb_(te);

        // Exec report for maker
        ExecutionReport maker_er;
        maker_er.exec_type    = maker->fully_filled()
                                 ? ExecutionReport::ExecType::Fill
                                 : ExecutionReport::ExecType::PartialFill;
        maker_er.order_id     = maker->id;
        maker_er.trade_id     = te.trade_id;
        maker_er.client_id    = maker->client_id;
        maker_er.instrument_id = maker->instrument_id;
        maker_er.last_px      = px;
        maker_er.last_qty     = qty;
        maker_er.leaves_qty   = maker->leaves_qty;
        maker_er.cum_qty      = maker->filled_qty;
        maker_er.side         = maker->side;
        maker_er.status       = maker->status;
        maker_er.timestamp    = ts;
        maker_er.seq_num      = next_seq();
        bool maker_done = maker->fully_filled();
        if (exec_report_cb_) exec_report_cb_(maker_er);

        // LimitOrderBook::match() already unlinked a fully-filled maker from
        // its PriceLevel and erased it from order_index_ before returning
        // here, but never touched the pool it was allocated from (the LOB
        // doesn't own it). Its terminal Fill report has now been sent above,
        // so this is the single place its slot can be safely returned —
        // otherwise every maker-side fill permanently leaks a pool slot.
        // See tests/test_memory_lifecycle.cpp.
        if (maker_done) pool_->destroy(maker);

        // Publish trade to market data
        MarketDataUpdate mdu;
        mdu.type          = MarketDataUpdate::Type::Trade;
        mdu.instrument_id = te.instrument_id;
        mdu.seq           = next_seq();
        mdu.timestamp     = ts;
        mdu.trade         = te;
        if (market_data_cb_) market_data_cb_(mdu);
    }

    // ── Helpers: send reports ─────────────────────────────────────────────────
    void send_ack(const Order* o, Timestamp ts) {
        ExecutionReport er;
        er.exec_type     = ExecutionReport::ExecType::New;
        er.order_id      = o->id;
        er.client_id     = o->client_id;
        er.instrument_id = o->instrument_id;
        er.leaves_qty    = o->leaves_qty;
        er.cum_qty       = 0;
        er.side          = o->side;
        er.status        = OrderStatus::New;
        er.timestamp     = ts;
        er.seq_num       = next_seq();
        if (exec_report_cb_) exec_report_cb_(er);
    }

    void send_reject(const OrderRequest& req, RejectReason reason, Timestamp ts) {
        ExecutionReport er;
        er.exec_type     = ExecutionReport::ExecType::Rejected;
        er.order_id      = req.order_id;
        er.client_id     = req.client_id;
        er.instrument_id = req.instrument_id;
        er.reject_reason = reason;
        er.status        = OrderStatus::Rejected;
        er.timestamp     = ts;
        er.seq_num       = next_seq();
        if (exec_report_cb_) exec_report_cb_(er);
    }

    void send_exec_report_taker(Order* o, Timestamp ts, const TakerFillAgg& agg) {
        ExecutionReport er;
        er.exec_type    = o->fully_filled()
                           ? ExecutionReport::ExecType::Fill
                           : ExecutionReport::ExecType::PartialFill;
        er.order_id     = o->id;
        er.client_id    = o->client_id;
        er.instrument_id = o->instrument_id;
        er.last_px      = (agg.total_qty > 0)
                            ? static_cast<Price>(agg.notional_ticks
                                / static_cast<int64_t>(agg.total_qty))
                            : Price{0};
        er.last_qty     = o->filled_qty;
        er.avg_px       = er.last_px;
        er.leaves_qty   = o->leaves_qty;
        er.cum_qty      = o->filled_qty;
        er.side         = o->side;
        er.status       = o->status;
        er.timestamp    = ts;
        er.seq_num      = next_seq();
        if (exec_report_cb_) exec_report_cb_(er);
    }

    void send_exec_report_cancel(Order* o, Timestamp ts) {
        ExecutionReport er;
        er.exec_type    = ExecutionReport::ExecType::Cancelled;
        er.order_id     = o->id;
        er.client_id    = o->client_id;
        er.instrument_id = o->instrument_id;
        er.leaves_qty   = 0;
        er.cum_qty      = o->filled_qty;
        er.status       = OrderStatus::Cancelled;
        er.timestamp    = ts;
        er.seq_num      = next_seq();
        if (exec_report_cb_) exec_report_cb_(er);
    }

    // ── Market data helpers ───────────────────────────────────────────────────
    void publish_l1(LimitOrderBook* bk, Timestamp ts) {
        if (!market_data_cb_) return;
        MarketDataUpdate mdu;
        mdu.type          = MarketDataUpdate::Type::L1Quote;
        mdu.instrument_id = bk->instrument_id();
        mdu.seq           = next_seq();
        mdu.timestamp     = ts;
        mdu.l1.bid_px     = bk->best_bid();
        mdu.l1.bid_qty    = bk->best_bid_qty();
        mdu.l1.ask_px     = bk->best_ask();
        mdu.l1.ask_qty    = bk->best_ask_qty();
        market_data_cb_(mdu);
    }

    void publish_l2_add(LimitOrderBook* bk, Order* o, Timestamp ts) {
        if (!market_data_cb_) return;
        MarketDataUpdate mdu;
        mdu.type          = MarketDataUpdate::Type::L2Update;
        mdu.instrument_id = bk->instrument_id();
        mdu.seq           = next_seq();
        mdu.timestamp     = ts;
        mdu.l2.side       = o->side;
        mdu.l2.price      = o->price;
        mdu.l2.qty        = o->leaves_qty;
        mdu.l2.is_delete  = false;
        market_data_cb_(mdu);
    }

    void publish_l2_remove(LimitOrderBook* bk, Side side, Price px, Timestamp ts) {
        if (!market_data_cb_) return;
        Quantity qty = bk->qty_at(side, px);
        MarketDataUpdate mdu;
        mdu.type          = MarketDataUpdate::Type::L2Update;
        mdu.instrument_id = bk->instrument_id();
        mdu.seq           = next_seq();
        mdu.timestamp     = ts;
        mdu.l2.side       = side;
        mdu.l2.price      = px;
        mdu.l2.qty        = qty;
        mdu.l2.is_delete  = (qty == 0);
        market_data_cb_(mdu);
    }

    // ── Members ───────────────────────────────────────────────────────────────
    // NOTE: Pool is heap-allocated. At 1M orders × 96B = 96 MB it cannot live
    // on the stack.  unique_ptr ensures automatic cleanup.
    std::unique_ptr<MemoryPool<Order, ORDER_POOL_SIZE>>  pool_{
        std::make_unique<MemoryPool<Order, ORDER_POOL_SIZE>>()
    };
    std::unordered_map<InstrumentId,
        std::unique_ptr<LimitOrderBook>>                 books_;
    std::unordered_map<InstrumentId, MarketState>        market_states_;
    SimClock*                                            sim_clock_{nullptr};

    ExecReportCb  exec_report_cb_;
    TradeEventCb  trade_event_cb_;
    MarketDataCb  market_data_cb_;

    // Sequence generators
    uint64_t order_seq_{0};
    uint64_t trade_seq_{0};
    uint64_t msg_seq_{0};

    // Stats
    uint64_t stat_orders_{0};
    uint64_t stat_trades_{0};
    uint64_t stat_cancels_{0};
};

} // namespace hydra
