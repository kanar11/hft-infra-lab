"""
test_pyhft.py — unit tests for the pyhft bindings. Standalone (no pytest),
returns exit code 0/1 so CI can wire it in without installing extra packages.

Covers what demo.py does not: error handling, edge cases, overflow,
rejection paths. Goal: confidence that the bindings react sensibly to bad inputs.

Run from repo root after building:
    python3 bindings/setup.py build_ext --inplace
    python3 bindings/test_pyhft.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import pyhft


PASSED = 0
FAILED = 0


def check(cond, label):
    global PASSED, FAILED
    if cond:
        PASSED += 1
        print(f"  PASS: {label}")
    else:
        FAILED += 1
        print(f"  FAIL: {label}")


def test_oms_zero_quantity():
    """submit_order with qty=0 should be rejected or return None."""
    oms = pyhft.OMS(max_position=1000, max_order_value=100_000.0)
    o = oms.submit_order("AAPL", pyhft.Side.BUY, 150.0, 0)
    check(o is None or o.quantity == 0, "oms_zero_qty_handled_gracefully")


def test_oms_partial_then_overfill():
    """Over-fill should be clamped to remaining_qty (critical hardening test)."""
    oms = pyhft.OMS(max_position=1000, max_order_value=100_000.0)
    o = oms.submit_order("AAPL", pyhft.Side.BUY, 150.0, 100)
    assert o is not None
    oms.fill_order(o.order_id, 60, 150.0)
    # Over-fill attempt: ask for 200, get <=40
    oms.fill_order(o.order_id, 200, 150.0)
    o2 = oms.get_order(o.order_id)
    check(o2.filled_qty == 100, "oms_overfill_clamped_via_python")
    check(o2.status == pyhft.OrderStatus.FILLED, "oms_overfill_status_filled")


def test_oms_unknown_order_id():
    """fill_order for an unknown order_id should not crash."""
    oms = pyhft.OMS(max_position=1000, max_order_value=100_000.0)
    try:
        oms.fill_order(999999, 100, 150.0)
        check(True, "oms_unknown_id_no_crash")
    except Exception as e:
        check(False, f"oms_unknown_id_threw {e}")


def test_risk_kill_switch_blocks():
    """Kill switch (manual) must reject orders."""
    risk = pyhft.RiskManager()
    risk.activate_kill_switch()
    r = risk.check_order("AAPL", pyhft.Side.BUY, 10.0, 1)
    check(r.action == pyhft.RiskAction.REJECT, "risk_kill_blocks_orders")
    check(risk.is_kill_switch_active(), "risk_kill_is_active")


def test_risk_circuit_breaker():
    """The daily loss limit trips the kill switch."""
    lim = pyhft.RiskLimits()
    lim.max_daily_loss = 1000
    risk = pyhft.RiskManager(lim)
    risk.update_pnl(-2000.0)
    r = risk.check_order("AAPL", pyhft.Side.BUY, 10.0, 1)
    check(r.action == pyhft.RiskAction.REJECT, "risk_circuit_breaker_trips")
    check(risk.is_kill_switch_active(), "risk_circuit_breaker_killed_after_loss")


def test_risk_unknown_symbol():
    """get_position for an unloaded symbol returns 0 (no crash)."""
    risk = pyhft.RiskManager()
    pos = risk.get_position("UNKNOWN")
    check(pos == 0, "risk_unknown_symbol_returns_zero")


def test_orderbook_basic_match():
    """FlatOrderBook: add buy + add sell at the same price → 1 trade."""
    book = pyhft.FlatOrderBook()
    book.add_buy(10100, 100)
    book.add_sell(10100, 100)
    check(book.trades() == 1, "orderbook_matches_at_same_price")


def test_orderbook_no_cross_no_trade():
    """Bid < Ask → no match."""
    book = pyhft.FlatOrderBook()
    book.add_buy(10050, 100)
    book.add_sell(10100, 100)
    check(book.trades() == 0, "orderbook_no_cross_no_trade")
    check(book.best_bid() == 10050, "orderbook_best_bid_correct")
    check(book.best_ask() == 10100, "orderbook_best_ask_correct")


def test_oms_get_position_after_fills():
    """Net qty + avg cost after two fills."""
    oms = pyhft.OMS(max_position=1000, max_order_value=1_000_000.0)
    o1 = oms.submit_order("MSFT", pyhft.Side.BUY, 400.0, 50)
    oms.fill_order(o1.order_id, 50, 400.0)
    o2 = oms.submit_order("MSFT", pyhft.Side.BUY, 410.0, 50)
    oms.fill_order(o2.order_id, 50, 410.0)
    pos = oms.get_position("MSFT")
    # avg = (400*50 + 410*50)/100 = 405, in ticks: 4050000
    check(pos.net_qty == 100, "oms_net_qty_after_2_fills")
    check(abs(pos.avg_price - 4050000) < 100, "oms_avg_price_correct")


def main():
    print("=== pyhft binding tests ===")
    test_oms_zero_quantity()
    test_oms_partial_then_overfill()
    test_oms_unknown_order_id()
    test_risk_kill_switch_blocks()
    test_risk_circuit_breaker()
    test_risk_unknown_symbol()
    test_orderbook_basic_match()
    test_orderbook_no_cross_no_trade()
    test_oms_get_position_after_fills()

    total = PASSED + FAILED
    print(f"\n{PASSED}/{total} passed")
    if FAILED > 0:
        print(f"  ({FAILED} FAILED)")
        sys.exit(1)


if __name__ == "__main__":
    main()
