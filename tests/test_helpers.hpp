#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  test_helpers.hpp — shared test-only utilities.
//
//  Every function/class here is a class member, constexpr, or explicitly
//  `inline`, so this header stays safe to include from multiple test .cpp
//  files that get linked into one binary (same reasoning as the production
//  "*.cpp as header" files — see tests/CMakeLists.txt).
//
//  These are test scaffolding, not production code: they exist to make the
//  existing public APIs of LimitOrderBook / MatchingEngine easy to drive and
//  observe from a test, without changing those APIs themselves.
// ─────────────────────────────────────────────────────────────────────────────

#include "hydra_simulation.hpp"
#include <deque>
#include <vector>

namespace hydra_test {

using namespace hydra;

// ── OrderArena ─────────────────────────────────────────────────────────────
//  LimitOrderBook::add_order()/match() operate on raw Order* with no
//  ownership model of their own (that lifecycle is MatchingEngine's job, via
//  its MemoryPool). For LOB-only tests we don't want a full MatchingEngine
//  in the way, so this gives out stable-address Order objects a test can
//  build directly and feed straight to the book.
//
//  std::deque guarantees no reallocation-on-growth, so pointers handed to
//  the book/PriceLevel remain valid for the arena's lifetime — unlike
//  std::vector, which could invalidate every outstanding Order* on regrowth.
class OrderArena {
public:
    Order* make_limit(OrderId id, ClientId cid, Side side, Price price, Quantity qty,
                       InstrumentId iid = 1, Timestamp ts = 0) {
        storage_.push_back(Order::make_limit(id, cid, iid, side, price, qty, ts));
        return &storage_.back();
    }

    Order* make_market(OrderId id, ClientId cid, Side side, Quantity qty,
                        InstrumentId iid = 1, Timestamp ts = 0) {
        storage_.push_back(Order::make_market(id, cid, iid, side, qty, ts));
        return &storage_.back();
    }

private:
    std::deque<Order> storage_;
};

// ── EngineHarness ────────────────────────────────────────────────────────
//  Wraps a MatchingEngine plus one instrument and records every
//  ExecutionReport / TradeEvent / MarketDataUpdate it emits, in emission
//  order, so a test can assert on the exact sequence produced by a request
//  — not just the book's final state.
class EngineHarness {
public:
    explicit EngineHarness(InstrumentId iid = 1) : iid_(iid) {
        engine_.add_instrument(iid_);
        engine_.set_market_state(iid_, MarketState::Open);
        engine_.set_exec_report_cb([this](const ExecutionReport& er) {
            exec_reports_.push_back(er);
        });
        engine_.set_trade_event_cb([this](const TradeEvent& te) {
            trades_.push_back(te);
        });
        engine_.set_market_data_cb([this](const MarketDataUpdate& mdu) {
            market_data_.push_back(mdu);
        });
    }

    // Submit a New limit order; returns the engine-assigned OrderId (the
    // engine ignores req.order_id on New and assigns its own sequential id
    // starting at 1 — see MatchingEngine::next_order_id()).
    OrderId new_limit(ClientId cid, Side side, Price price, Quantity qty,
                       TimeInForce tif = TimeInForce::GTC, Timestamp ts = 0) {
        std::size_t before = exec_reports_.size();
        OrderRequest req;
        req.action        = OrderRequest::Action::New;
        req.client_id     = cid;
        req.instrument_id = iid_;
        req.side          = side;
        req.type          = OrderType::Limit;
        req.tif           = tif;
        req.price         = price;
        req.quantity      = qty;
        req.timestamp     = ts;
        engine_.process(req);
        // The first ExecutionReport emitted for a brand-new order is always
        // its ack (New, or Rejected before an id is even meaningful); for an
        // accepted order that ack carries the engine-assigned id.
        return (exec_reports_.size() > before) ? exec_reports_[before].order_id
                                                : INVALID_ORDER_ID;
    }

    OrderId new_market(ClientId cid, Side side, Quantity qty, Timestamp ts = 0) {
        std::size_t before = exec_reports_.size();
        OrderRequest req;
        req.action        = OrderRequest::Action::New;
        req.client_id     = cid;
        req.instrument_id = iid_;
        req.side          = side;
        req.type          = OrderType::Market;
        req.quantity      = qty;
        req.timestamp     = ts;
        engine_.process(req);
        return (exec_reports_.size() > before) ? exec_reports_[before].order_id
                                                : INVALID_ORDER_ID;
    }

    void cancel(OrderId id, ClientId cid, Timestamp ts = 0) {
        OrderRequest req;
        req.action        = OrderRequest::Action::Cancel;
        req.order_id      = id;
        req.client_id     = cid;
        req.instrument_id = iid_;
        req.timestamp     = ts;
        engine_.process(req);
    }

    MatchingEngine&      engine()  { return engine_; }
    LimitOrderBook*      book()    { return engine_.book(iid_); }

    const std::vector<ExecutionReport>&  exec_reports() const { return exec_reports_; }
    const std::vector<TradeEvent>&       trades()       const { return trades_; }
    const std::vector<MarketDataUpdate>& market_data()  const { return market_data_; }

    // Convenience: exec reports for a single order id, in emission order.
    std::vector<ExecutionReport> reports_for(OrderId id) const {
        std::vector<ExecutionReport> out;
        for (auto& er : exec_reports_)
            if (er.order_id == id) out.push_back(er);
        return out;
    }

private:
    InstrumentId iid_;
    MatchingEngine engine_;
    std::vector<ExecutionReport>  exec_reports_;
    std::vector<TradeEvent>       trades_;
    std::vector<MarketDataUpdate> market_data_;
};

} // namespace hydra_test
