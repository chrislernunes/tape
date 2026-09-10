#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  OrderGateway
//
//  The boundary between "outside world" and the matching engine.
//  Responsibilities:
//    1. Receive OrderRequests from clients/agents
//    2. Apply pre-trade risk checks
//    3. Model network + processing latency (in sim mode)
//    4. Forward to MatchingEngine
//    5. Route execution reports back to originating client
//
//  In simulation mode all latency is applied via SimClock advancement
//  so the ordering of events remains deterministic.
// ─────────────────────────────────────────────────────────────────────────────

#include "risk_checks.cpp"
#include "../matching_engine/matching_engine.cpp"
#include "../../infrastructure/lockfree_queue.hpp"
#include "../../infrastructure/timestamp.hpp"

#include <unordered_map>
#include <functional>

namespace hydra {

// Default latency profile (microseconds)
struct GatewayLatencyProfile {
    double client_to_gateway_us = 40.0;    // network
    double gateway_processing_us = 2.0;    // risk + routing
    double gateway_to_engine_us  = 5.0;    // internal bus
    double engine_processing_us  = 2.0;    // matching
    double market_data_out_us    = 30.0;   // MD publish
};

class OrderGateway {
    static constexpr std::size_t QUEUE_SIZE = 65536;

public:
    using ExecReportCb = std::function<void(ClientId, const ExecutionReport&)>;

    OrderGateway(MatchingEngine* engine,
                 RiskEngine*     risk,
                 SimClock*       clock = nullptr)
        : engine_(engine), risk_(risk), sim_clock_(clock) {}

    void set_exec_report_cb(ExecReportCb cb) { exec_report_cb_ = std::move(cb); }
    void set_latency(const GatewayLatencyProfile& p) noexcept { latency_ = p; }

    // ── Submit order (client API) ─────────────────────────────────────────────
    // Returns false if pre-risk rejected before engine
    bool submit(const OrderRequest& req) {
        // Advance clock to model client→gateway latency
        advance_clock(latency_.client_to_gateway_us);

        // Pre-trade risk
        RejectReason rr = risk_->check(req);
        if (rr != RejectReason::None) {
            ++stat_risk_rejected_;
            ExecutionReport er;
            er.exec_type     = ExecutionReport::ExecType::Rejected;
            er.order_id      = req.order_id;
            er.client_id     = req.client_id;
            er.instrument_id = req.instrument_id;
            er.reject_reason = rr;
            er.status        = OrderStatus::Rejected;
            er.timestamp     = current_time();
            route_exec_report(er);
            return false;
        }

        // Risk state tracking
        if (req.action == OrderRequest::Action::New)
            risk_->on_order_new(req.client_id);

        // Model gateway processing latency
        advance_clock(latency_.gateway_processing_us);
        advance_clock(latency_.gateway_to_engine_us);

        // Forward to engine
        engine_->process(req);

        ++stat_forwarded_;
        return true;
    }

    // ── Receive execution report from engine (route to client) ───────────────
    void on_exec_report(const ExecutionReport& er) {
        // Update risk state
        if (er.exec_type == ExecutionReport::ExecType::Fill ||
            er.exec_type == ExecutionReport::ExecType::PartialFill) {
            risk_->on_fill(er.client_id, er.side, er.last_qty, er.last_px);
        }
        if (er.exec_type == ExecutionReport::ExecType::Cancelled)
            risk_->on_order_cancel(er.client_id);

        route_exec_report(er);
    }

    // ── Stats ─────────────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t forwarded()     const noexcept { return stat_forwarded_; }
    [[nodiscard]] uint64_t risk_rejected() const noexcept { return stat_risk_rejected_; }

private:
    void route_exec_report(const ExecutionReport& er) {
        if (exec_report_cb_) exec_report_cb_(er.client_id, er);
    }

    void advance_clock(double us) noexcept {
        if (sim_clock_) sim_clock_->advance_us(us);
    }

    [[nodiscard]] Timestamp current_time() const noexcept {
        return sim_clock_ ? sim_clock_->now() : Clock::now_wall();
    }

    MatchingEngine*         engine_;
    RiskEngine*             risk_;
    SimClock*               sim_clock_;
    GatewayLatencyProfile   latency_;
    ExecReportCb            exec_report_cb_;

    uint64_t stat_forwarded_{0};
    uint64_t stat_risk_rejected_{0};
};

} // namespace hydra
