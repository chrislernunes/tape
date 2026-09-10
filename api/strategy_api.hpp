#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  StrategyAPI: clean interface for strategies to interact with HydraExchange
//
//  Hides all internal plumbing. A strategy only needs to:
//    1. Inherit from StrategyBase
//    2. Implement on_quote(), on_trade(), on_fill()
//    3. Call buy(), sell(), cancel() in response
//
//  Thread-safety: all calls must be made from the sim event loop thread.
// ─────────────────────────────────────────────────────────────────────────────

#include "../engine/matching_engine/trade_event.cpp"
#include "../engine/orderbook/limit_order_book.cpp"
#include "../include/hydra/types.hpp"

#include <string>
#include <functional>
#include <optional>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  Quote: simplified L1 view
// ─────────────────────────────────────────────────────────────────────────────
struct Quote {
    InstrumentId instrument_id{0};
    double       bid{0.0};
    double       ask{0.0};
    double       mid{0.0};
    double       spread{0.0};
    uint64_t     bid_qty{0};
    uint64_t     ask_qty{0};
    int64_t      timestamp{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Fill: simplified fill notification
// ─────────────────────────────────────────────────────────────────────────────
struct Fill {
    uint64_t order_id{0};
    bool     is_buy{false};
    double   price{0.0};
    uint64_t quantity{0};
    uint64_t remaining{0};
    bool     fully_filled{false};
    int64_t  timestamp{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  StrategyContext: injected into strategy, provides action methods
// ─────────────────────────────────────────────────────────────────────────────
class StrategyContext {
public:
    using OrderSubmitFn = std::function<uint64_t(InstrumentId, bool is_buy,
                                                   bool is_market, double price,
                                                   uint64_t qty)>;
    using CancelFn      = std::function<bool(uint64_t order_id)>;
    using BookQueryFn   = std::function<Quote(InstrumentId)>;

    void set_order_submit_fn(OrderSubmitFn fn) { submit_fn_ = std::move(fn); }
    void set_cancel_fn(CancelFn fn)            { cancel_fn_ = std::move(fn); }
    void set_book_query_fn(BookQueryFn fn)     { book_fn_   = std::move(fn); }

    // ── Order entry ───────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t buy_limit(InstrumentId id, double price, uint64_t qty) {
        return submit_fn_ ? submit_fn_(id, true, false, price, qty) : 0;
    }
    [[nodiscard]] uint64_t sell_limit(InstrumentId id, double price, uint64_t qty) {
        return submit_fn_ ? submit_fn_(id, false, false, price, qty) : 0;
    }
    [[nodiscard]] uint64_t buy_market(InstrumentId id, uint64_t qty) {
        return submit_fn_ ? submit_fn_(id, true, true, 0.0, qty) : 0;
    }
    [[nodiscard]] uint64_t sell_market(InstrumentId id, uint64_t qty) {
        return submit_fn_ ? submit_fn_(id, false, true, 0.0, qty) : 0;
    }
    [[nodiscard]] bool cancel(uint64_t order_id) {
        return cancel_fn_ ? cancel_fn_(order_id) : false;
    }

    // ── Book query ────────────────────────────────────────────────────────────
    [[nodiscard]] Quote quote(InstrumentId id) {
        return book_fn_ ? book_fn_(id) : Quote{};
    }

private:
    OrderSubmitFn submit_fn_;
    CancelFn      cancel_fn_;
    BookQueryFn   book_fn_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  StrategyBase: all user-defined strategies inherit from this
// ─────────────────────────────────────────────────────────────────────────────
class StrategyBase {
public:
    explicit StrategyBase(StrategyContext* ctx) noexcept : ctx_(ctx) {}
    virtual ~StrategyBase() = default;

    virtual void on_start()  {}
    virtual void on_stop()   {}
    virtual void on_quote(const Quote&)  {}
    virtual void on_trade(const TradeEvent&) {}
    virtual void on_fill(const Fill&)    {}
    virtual void on_tick(int64_t timestamp_ns) { (void)timestamp_ns; }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    void set_name(std::string n) noexcept { name_ = std::move(n); }

    [[nodiscard]] double realized_pnl() const noexcept { return realized_pnl_; }
    [[nodiscard]] int64_t position()    const noexcept { return position_; }

protected:
    // Helpers that delegate to context
    uint64_t buy_limit(InstrumentId id, double price, uint64_t qty) {
        return ctx_->buy_limit(id, price, qty);
    }
    uint64_t sell_limit(InstrumentId id, double price, uint64_t qty) {
        return ctx_->sell_limit(id, price, qty);
    }
    uint64_t buy_market(InstrumentId id, uint64_t qty) {
        return ctx_->buy_market(id, qty);
    }
    uint64_t sell_market(InstrumentId id, uint64_t qty) {
        return ctx_->sell_market(id, qty);
    }
    bool cancel(uint64_t oid) { return ctx_->cancel(oid); }
    Quote quote(InstrumentId id) { return ctx_->quote(id); }

    StrategyContext* ctx_;
    std::string      name_{"unnamed_strategy"};
    int64_t          position_{0};
    double           realized_pnl_{0.0};
};

} // namespace hydra


// ─────────────────────────────────────────────────────────────────────────────
//  Python Bindings (pybind11)
//  File: python_bindings.cpp
//
//  Exposes HydraExchange to Python for research workflows.
//  Install:  pip install pybind11
//  Build:    cmake --build . --target hydra_py
// ─────────────────────────────────────────────────────────────────────────────

/*
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "strategy_api.hpp"
#include "../hydra_simulation.hpp"

namespace py = pybind11;
using namespace hydra;

PYBIND11_MODULE(hydra_py, m) {
    m.doc() = "HydraExchange Python bindings";

    // ── Types ──────────────────────────────────────────────────────────────
    py::enum_<Side>(m, "Side")
        .value("Buy",  Side::Buy)
        .value("Sell", Side::Sell);

    py::class_<Quote>(m, "Quote")
        .def_readwrite("bid",       &Quote::bid)
        .def_readwrite("ask",       &Quote::ask)
        .def_readwrite("mid",       &Quote::mid)
        .def_readwrite("spread",    &Quote::spread)
        .def_readwrite("bid_qty",   &Quote::bid_qty)
        .def_readwrite("ask_qty",   &Quote::ask_qty)
        .def_readwrite("timestamp", &Quote::timestamp);

    py::class_<Fill>(m, "Fill")
        .def_readwrite("order_id",      &Fill::order_id)
        .def_readwrite("is_buy",        &Fill::is_buy)
        .def_readwrite("price",         &Fill::price)
        .def_readwrite("quantity",      &Fill::quantity)
        .def_readwrite("remaining",     &Fill::remaining)
        .def_readwrite("fully_filled",  &Fill::fully_filled);

    // ── StrategyContext ────────────────────────────────────────────────────
    py::class_<StrategyContext>(m, "StrategyContext")
        .def("buy_limit",   &StrategyContext::buy_limit)
        .def("sell_limit",  &StrategyContext::sell_limit)
        .def("buy_market",  &StrategyContext::buy_market)
        .def("sell_market", &StrategyContext::sell_market)
        .def("cancel",      &StrategyContext::cancel)
        .def("quote",       &StrategyContext::quote);

    // ── StrategyBase (trampoline for Python subclassing) ───────────────────
    py::class_<StrategyBase>(m, "Strategy")
        .def(py::init<StrategyContext*>())
        .def("on_start",  &StrategyBase::on_start)
        .def("on_stop",   &StrategyBase::on_stop)
        .def("on_quote",  &StrategyBase::on_quote)
        .def("on_trade",  &StrategyBase::on_trade)
        .def("on_fill",   &StrategyBase::on_fill)
        .def("on_tick",   &StrategyBase::on_tick)
        .def_property_readonly("realized_pnl", &StrategyBase::realized_pnl)
        .def_property_readonly("position",     &StrategyBase::position);

    // ── SimulationConfig ───────────────────────────────────────────────────
    py::class_<SimulationConfig>(m, "SimulationConfig")
        .def(py::init<>())
        .def_readwrite("duration_seconds",    &SimulationConfig::duration_seconds)
        .def_readwrite("num_market_makers",   &SimulationConfig::num_market_makers)
        .def_readwrite("num_noise_traders",   &SimulationConfig::num_noise_traders)
        .def_readwrite("verbose",             &SimulationConfig::verbose);

    // ── HydraSimulation ────────────────────────────────────────────────────
    py::class_<HydraSimulation>(m, "HydraSimulation")
        .def(py::init<SimulationConfig>())
        .def("run", &HydraSimulation::run);
}
*/
