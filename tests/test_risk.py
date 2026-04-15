#!/usr/bin/env python3
"""Unit tests for Risk Manager."""
import os
import sys
import time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from risk.risk_manager import RiskManager, RiskLimits, RiskAction


def _make_rm() -> RiskManager:
    """Helper: create risk manager with tight limits for testing."""
    limits = RiskLimits(
        max_position_per_symbol=500,
        max_portfolio_exposure=2000,
        max_daily_loss=1000.0,
        max_orders_per_second=100,
        max_order_value=50000.0,
        max_drawdown_pct=10.0,
    )
    return RiskManager(limits=limits)


def test_allow_normal_order():
    """Normal order within all limits should be allowed."""
    rm = _make_rm()
    result = rm.check_order('AAPL', 'BUY', 150.00, 100)
    assert result.action == RiskAction.ALLOW
    print("  PASS: test_allow_normal_order")


def test_reject_order_value():
    """Order exceeding max_order_value should be rejected."""
    rm = _make_rm()
    result = rm.check_order('AAPL', 'BUY', 200.00, 300)  # 60000 > 50000
    assert result.action == RiskAction.REJECT
    assert 'ORDER_VALUE' in rm.state.reject_reasons
    print("  PASS: test_reject_order_value")


def test_reject_position_limit():
    """Order exceeding per-symbol position limit should be rejected."""
    rm = _make_rm()
    rm.state.positions['AAPL'] = 400
    result = rm.check_order('AAPL', 'BUY', 150.00, 200)  # 400+200=600 > 500
    assert result.action == RiskAction.REJECT
    assert 'POSITION_LIMIT' in rm.state.reject_reasons
    print("  PASS: test_reject_position_limit")


def test_reject_portfolio_exposure():
    """Order exceeding total portfolio exposure should be rejected."""
    rm = _make_rm()
    rm.state.positions['AAPL'] = 400
    rm.state.positions['MSFT'] = 400
    rm.state.positions['GOOGL'] = 400
    rm.state.positions['TSLA'] = 400
    rm.state.positions['META'] = 400
    # Total = 2000, adding 100 JPM → 2100 > 2000 (but JPM position 100 < 500 limit)
    result = rm.check_order('JPM', 'BUY', 50.00, 100)
    assert result.action == RiskAction.REJECT, f"Expected REJECT, got {result.action}: {result.reason}"
    assert 'PORTFOLIO_EXPOSURE' in rm.state.reject_reasons
    print("  PASS: test_reject_portfolio_exposure")


def test_circuit_breaker():
    """Exceeding daily loss limit should activate kill switch."""
    rm = _make_rm()
    rm.update_pnl(-1500.0)  # loss exceeds $1000 limit
    result = rm.check_order('AAPL', 'BUY', 150.00, 100)
    assert result.action == RiskAction.REJECT
    assert rm.state.kill_switch_active
    assert 'CIRCUIT_BREAKER' in rm.state.reject_reasons
    print("  PASS: test_circuit_breaker")


def test_kill_switch_manual():
    """Manual kill switch should reject all orders."""
    rm = _make_rm()
    rm.activate_kill_switch()
    result = rm.check_order('AAPL', 'BUY', 150.00, 100)
    assert result.action == RiskAction.REJECT
    assert 'KILL_SWITCH' in rm.state.reject_reasons
    print("  PASS: test_kill_switch_manual")


def test_kill_switch_deactivate():
    """Deactivating kill switch should allow orders again."""
    rm = _make_rm()
    rm.activate_kill_switch()
    rm.deactivate_kill_switch()
    result = rm.check_order('AAPL', 'BUY', 150.00, 100)
    assert result.action == RiskAction.ALLOW
    print("  PASS: test_kill_switch_deactivate")


def test_drawdown_limit():
    """Exceeding drawdown from peak should activate kill switch."""
    rm = _make_rm()
    rm.update_pnl(10000.0)   # peak = 10000
    rm.update_pnl(-5000.0)   # pnl = 5000, drawdown = 50% > 10%
    result = rm.check_order('AAPL', 'BUY', 150.00, 100)
    assert result.action == RiskAction.REJECT
    assert rm.state.kill_switch_active
    assert 'DRAWDOWN' in rm.state.reject_reasons
    print("  PASS: test_drawdown_limit")


def test_reset_daily():
    """reset_daily should clear PnL, kill switch, and rate limiter."""
    rm = _make_rm()
    rm.update_pnl(-2000.0)
    rm.activate_kill_switch()
    rm.reset_daily()
    assert rm.state.daily_pnl == 0.0
    assert rm.state.kill_switch_active is False
    result = rm.check_order('AAPL', 'BUY', 150.00, 100)
    assert result.action == RiskAction.ALLOW
    print("  PASS: test_reset_daily")


def test_check_speed():
    """Risk check should be fast (<50 microseconds)."""
    rm = _make_rm()
    start = time.time_ns()
    for _ in range(1000):
        rm.check_order('AAPL', 'BUY', 150.00, 100)
    total_ns = time.time_ns() - start
    avg_ns = total_ns / 1000
    assert avg_ns < 50000, f"Too slow: {avg_ns:.0f} ns/check"
    print(f"  PASS: test_check_speed ({avg_ns:.0f} ns)")


if __name__ == '__main__':
    print("=== Risk Manager Unit Tests ===\n")
    tests = [
        test_allow_normal_order,
        test_reject_order_value,
        test_reject_position_limit,
        test_reject_portfolio_exposure,
        test_circuit_breaker,
        test_kill_switch_manual,
        test_kill_switch_deactivate,
        test_drawdown_limit,
        test_reset_daily,
        test_check_speed,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests passed")
