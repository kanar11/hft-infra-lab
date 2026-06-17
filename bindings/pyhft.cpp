/*
 * pyhft — pybind11 bindings exposing the lab's C++ core to Python.
 *
 * Wraps the three modules that are most useful from a research/notebook
 * workflow: OMS, RiskManager, and the FlatOrderBook. Everything else can
 * be added as needed.
 *
 * Build (from repo root):
 *   pip install pybind11 setuptools
 *   python3 bindings/setup.py build_ext --inplace
 *
 * Use:
 *   import sys; sys.path.insert(0, '.')
 *   import pyhft
 *   oms = pyhft.OMS(max_position=1000, max_order_value=100000.0)
 *   order = oms.submit_order("AAPL", pyhft.Side.BUY, 150.25, 100)
 *   oms.fill_order(order.order_id, 100, 150.25)
 *   print(oms.get_position("AAPL").net_qty)
 */
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "../oms/oms.hpp"
#include "../risk/risk_manager.hpp"
#include "../orderbook/orderbook_flat.hpp"

namespace py = pybind11;

using FlatBook = orderbook::FlatOrderBook<16384>;


PYBIND11_MODULE(pyhft, m) {
    m.doc() = "Python bindings for hft-infra-lab — OMS, RiskManager, FlatOrderBook";

    // ----- Side enum -----
    py::enum_<Side>(m, "Side")
        .value("BUY",  Side::BUY)
        .value("SELL", Side::SELL)
        .export_values();

    // ----- OrderStatus enum -----
    py::enum_<OrderStatus>(m, "OrderStatus")
        .value("NEW",       OrderStatus::NEW)
        .value("SENT",      OrderStatus::SENT)
        .value("FILLED",    OrderStatus::FILLED)
        .value("PARTIAL",   OrderStatus::PARTIAL)
        .value("CANCELLED", OrderStatus::CANCELLED)
        .value("REJECTED",  OrderStatus::REJECTED)
        .export_values();

    // ----- Order struct -----
    py::class_<Order>(m, "Order")
        .def_readonly("order_id",   &Order::order_id)
        .def_readonly("side",       &Order::side)
        .def_readonly("price",      &Order::price)    // fixed-point int64 (× 10000)
        .def_readonly("quantity",   &Order::quantity)
        .def_readonly("filled_qty", &Order::filled_qty)
        .def_readonly("status",     &Order::status)
        .def_property_readonly("symbol",
            [](const Order& o) { return std::string(o.symbol); });

    // ----- Position struct -----
    py::class_<Position>(m, "Position")
        .def_readonly("net_qty",      &Position::net_qty)
        .def_readonly("pending_qty",  &Position::pending_qty)
        .def_readonly("avg_price",    &Position::avg_price)
        .def_readonly("realized_pnl", &Position::realized_pnl)
        .def_readonly("total_cost",   &Position::total_cost)
        .def_readonly("fees",         &Position::fees)
        .def_property_readonly("net_pnl", &Position::net_pnl)
        .def_property_readonly("symbol",
            [](const Position& p) { return std::string(p.symbol); });

    // ----- OMS class -----
    py::class_<OMS>(m, "OMS")
        .def(py::init<int32_t, double>(),
             py::arg("max_position") = 1000,
             py::arg("max_order_value") = 100000.0)
        .def("submit_order", &OMS::submit_order,
             py::return_value_policy::reference_internal,
             py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("quantity"))
        .def("fill_order",   &OMS::fill_order,
             py::arg("order_id"), py::arg("fill_qty"), py::arg("fill_price"))
        .def("cancel_order", &OMS::cancel_order, py::arg("order_id"))
        .def("get_order",    &OMS::get_order,
             py::return_value_policy::reference_internal, py::arg("order_id"))
        .def("get_position", &OMS::get_position,
             py::return_value_policy::reference_internal, py::arg("symbol"))
        .def("order_count",    &OMS::order_count)
        .def("position_count", &OMS::position_count);

    // ----- RiskAction / RiskCheckResult / RiskLimits / RiskManager -----
    py::enum_<RiskAction>(m, "RiskAction")
        .value("ALLOW",  RiskAction::ALLOW)
        .value("REJECT", RiskAction::REJECT)
        .value("KILL",   RiskAction::KILL)
        .export_values();

    py::class_<RiskCheckResult>(m, "RiskCheckResult")
        .def_readonly("action",     &RiskCheckResult::action)
        .def_readonly("latency_ns", &RiskCheckResult::latency_ns)
        .def_property_readonly("reason",
            [](const RiskCheckResult& r) { return std::string(r.reason); });

    py::class_<RiskLimits>(m, "RiskLimits")
        .def(py::init<>())
        .def_readwrite("max_position_per_symbol", &RiskLimits::max_position_per_symbol)
        .def_readwrite("max_portfolio_exposure",  &RiskLimits::max_portfolio_exposure)
        .def_readwrite("max_daily_loss",          &RiskLimits::max_daily_loss)
        .def_readwrite("max_orders_per_second",   &RiskLimits::max_orders_per_second)
        .def_readwrite("max_order_value",         &RiskLimits::max_order_value)
        .def_readwrite("max_drawdown_pct",        &RiskLimits::max_drawdown_pct);

    py::class_<RiskManager>(m, "RiskManager")
        .def(py::init<const RiskLimits&>(), py::arg("limits") = RiskLimits{})
        .def("check_order",        &RiskManager::check_order,
             py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("quantity"))
        .def("on_order_sent",      &RiskManager::on_order_sent)
        .def("on_order_cancelled", &RiskManager::on_order_cancelled)
        .def("update_position",    &RiskManager::update_position)
        .def("update_pnl",         &RiskManager::update_pnl)
        .def("get_position",       &RiskManager::get_position)
        .def("get_pending",        &RiskManager::get_pending)
        .def("get_daily_pnl",      &RiskManager::get_daily_pnl)
        .def("is_kill_switch_active",   &RiskManager::is_kill_switch_active)
        .def("activate_kill_switch",    &RiskManager::activate_kill_switch)
        .def("deactivate_kill_switch",  &RiskManager::deactivate_kill_switch)
        .def("reset_daily",             &RiskManager::reset_daily);

    // ----- FlatOrderBook<16384> -----
    py::class_<FlatBook>(m, "FlatOrderBook")
        .def(py::init<>())
        .def("add_buy",         &FlatBook::add_buy,
             py::arg("price"), py::arg("qty"))
        .def("add_sell",        &FlatBook::add_sell,
             py::arg("price"), py::arg("qty"))
        .def("submit_with_id",  &FlatBook::submit_with_id,
             py::arg("id"), py::arg("price"), py::arg("qty"), py::arg("is_buy"))
        .def("cancel",          &FlatBook::cancel, py::arg("id"))
        .def("modify",          &FlatBook::modify,
             py::arg("id"), py::arg("new_price"), py::arg("new_qty"))
        .def("best_bid",        &FlatBook::best_bid)
        .def("best_ask",        &FlatBook::best_ask)
        .def("trades",          &FlatBook::trades)
        .def("empty",           &FlatBook::empty)
        .def("tracked_orders",  &FlatBook::tracked_orders)
        .def("bid_qty_at",      &FlatBook::bid_qty_at, py::arg("price"))
        .def("ask_qty_at",      &FlatBook::ask_qty_at, py::arg("price"));
}
