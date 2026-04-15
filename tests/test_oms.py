#!/usr/bin/env python3
"""Unit tests for Order Management System."""
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from oms.oms import OMS, Side, OrderStatus

def test_submit_order():
    oms = OMS(max_position=1000, max_order_value=100000)
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    assert order is not None
    assert order.status == OrderStatus.SENT
    assert order.symbol == "AAPL"
    print("  PASS: test_submit_order")

def test_risk_check_value():
    oms = OMS(max_position=1000, max_order_value=10000)
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)  # 15000 > 10000
    assert order is None
    print("  PASS: test_risk_check_value")

def test_risk_check_position():
    oms = OMS(max_position=50, max_order_value=100000)
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)  # 100 > 50
    assert order is None
    print("  PASS: test_risk_check_position")

def test_fill_order():
    oms = OMS()
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(order.order_id, 100, 150.00)
    assert order.status == OrderStatus.FILLED
    assert order.filled_qty == 100
    print("  PASS: test_fill_order")

def test_partial_fill():
    oms = OMS()
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(order.order_id, 50, 150.00)
    assert order.status == OrderStatus.PARTIALLY_FILLED
    assert order.filled_qty == 50
    print("  PASS: test_partial_fill")

def test_cancel_order():
    oms = OMS()
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.cancel_order(order.order_id)
    assert order.status == OrderStatus.CANCELLED
    print("  PASS: test_cancel_order")

def test_position_tracking():
    oms = OMS()
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(order.order_id, 100, 150.00)
    pos = oms.positions["AAPL"]
    assert pos.net_qty == 100
    print("  PASS: test_position_tracking")

def test_pnl_basic():
    """Buy 100 @ 150, sell 50 @ 155 → P&L = 250, avg stays 150."""
    oms = OMS(max_position=10000, max_order_value=1000000)
    o1 = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(o1.order_id, 100, 150.00)
    o2 = oms.submit_order("AAPL", Side.SELL, 155.00, 50)
    oms.fill_order(o2.order_id, 50, 155.00)
    pos = oms.positions["AAPL"]
    assert pos.net_qty == 50
    assert abs(pos.avg_price - 150.00) < 0.01, f"avg_price {pos.avg_price} != 150.00"
    assert abs(pos.realized_pnl - 250.00) < 0.01, f"pnl {pos.realized_pnl} != 250.00"
    print("  PASS: test_pnl_basic")

def test_pnl_full_close():
    """Buy 100 @ 150, sell all 100 @ 160 → P&L = 1000, position flat."""
    oms = OMS(max_position=10000, max_order_value=1000000)
    o1 = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(o1.order_id, 100, 150.00)
    o2 = oms.submit_order("AAPL", Side.SELL, 160.00, 100)
    oms.fill_order(o2.order_id, 100, 160.00)
    pos = oms.positions["AAPL"]
    assert pos.net_qty == 0
    assert abs(pos.avg_price) < 0.01, f"avg_price {pos.avg_price} != 0"
    assert abs(pos.total_cost) < 0.01, f"total_cost {pos.total_cost} != 0"
    assert abs(pos.realized_pnl - 1000.00) < 0.01, f"pnl {pos.realized_pnl} != 1000.00"
    print("  PASS: test_pnl_full_close")

def test_pnl_multiple_buys():
    """Buy 100 @ 150 + 100 @ 160, sell 100 @ 170 → avg=155, P&L=1500."""
    oms = OMS(max_position=10000, max_order_value=1000000)
    o1 = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(o1.order_id, 100, 150.00)
    o2 = oms.submit_order("AAPL", Side.BUY, 160.00, 100)
    oms.fill_order(o2.order_id, 100, 160.00)
    pos = oms.positions["AAPL"]
    assert abs(pos.avg_price - 155.00) < 0.01, f"avg_price {pos.avg_price} != 155.00"
    o3 = oms.submit_order("AAPL", Side.SELL, 170.00, 100)
    oms.fill_order(o3.order_id, 100, 170.00)
    assert pos.net_qty == 100
    assert abs(pos.realized_pnl - 1500.00) < 0.01, f"pnl {pos.realized_pnl} != 1500.00"
    print("  PASS: test_pnl_multiple_buys")

if __name__ == '__main__':
    print("=== OMS Unit Tests ===\n")
    tests = [
        test_submit_order,
        test_risk_check_value,
        test_risk_check_position,
        test_fill_order,
        test_partial_fill,
        test_cancel_order,
        test_position_tracking,
        test_pnl_basic,
        test_pnl_full_close,
        test_pnl_multiple_buys,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests passed")
