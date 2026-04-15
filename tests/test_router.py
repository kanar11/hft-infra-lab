#!/usr/bin/env python3
"""Unit tests for Smart Order Router."""
import os
import sys
import time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from router.smart_router import SmartOrderRouter, RoutingStrategy, Venue


def _make_router() -> SmartOrderRouter:
    """Helper: create router with 3 venues and live quotes."""
    router = SmartOrderRouter(strategy=RoutingStrategy.BEST_PRICE, split_threshold=500)
    router.add_venue(Venue(name='NYSE', latency_ns=500, fee_per_share=0.003))
    router.add_venue(Venue(name='NASDAQ', latency_ns=200, fee_per_share=-0.002))
    router.add_venue(Venue(name='BATS', latency_ns=150, fee_per_share=-0.001))
    # NASDAQ has best ask (150.01), NYSE second (150.02), BATS worst (150.05)
    router.update_quote('NYSE', 149.98, 150.02, 500, 500)
    router.update_quote('NASDAQ', 149.99, 150.01, 300, 300)
    router.update_quote('BATS', 149.95, 150.05, 200, 200)
    return router


def test_best_price_buy():
    """BUY should route to venue with lowest ask (NASDAQ @ 150.01)."""
    router = _make_router()
    decision = router.route_order('BUY', 100)
    assert decision is not None
    assert decision.venue == 'NASDAQ', f"Expected NASDAQ, got {decision.venue}"
    assert decision.price == 150.01
    print("  PASS: test_best_price_buy")


def test_best_price_sell():
    """SELL should route to venue with highest bid (NASDAQ @ 149.99)."""
    router = _make_router()
    decision = router.route_order('SELL', 100)
    assert decision is not None
    assert decision.venue == 'NASDAQ', f"Expected NASDAQ, got {decision.venue}"
    assert decision.price == 149.99
    print("  PASS: test_best_price_sell")


def test_lowest_latency():
    """LOWEST_LATENCY should route to BATS (150ns)."""
    router = _make_router()
    decision = router.route_order('BUY', 100, strategy=RoutingStrategy.LOWEST_LATENCY)
    assert decision is not None
    assert decision.venue == 'BATS', f"Expected BATS, got {decision.venue}"
    print("  PASS: test_lowest_latency")


def test_split_order():
    """Large order (1000 shares) should split across venues."""
    router = _make_router()
    decision = router.route_order('BUY', 1000, strategy=RoutingStrategy.SPLIT)
    assert decision is not None
    assert 'SPLIT' in decision.reason
    # Total available: NASDAQ 300 + NYSE 500 + BATS 200 = 1000
    assert decision.quantity == 1000, f"Expected 1000 filled, got {decision.quantity}"
    print("  PASS: test_split_order")


def test_split_partial_fill():
    """Split with insufficient total liquidity fills what's available."""
    router = _make_router()
    # Request 2000 but only 1000 available across all venues
    decision = router.route_order('BUY', 2000, strategy=RoutingStrategy.SPLIT)
    assert decision is not None
    assert decision.quantity == 1000, f"Expected 1000 partial, got {decision.quantity}"
    assert '1000 unfilled' in decision.reason
    print("  PASS: test_split_partial_fill")


def test_no_venues():
    """No active venues → returns None."""
    router = SmartOrderRouter()
    decision = router.route_order('BUY', 100)
    assert decision is None
    assert router.stats.rejected == 1
    print("  PASS: test_no_venues")


def test_inactive_venue_skipped():
    """Inactive venue should be skipped."""
    router = _make_router()
    router.venues['NASDAQ'].is_active = False
    decision = router.route_order('BUY', 100)
    assert decision is not None
    assert decision.venue != 'NASDAQ', "Should skip inactive NASDAQ"
    # NYSE has next best ask (150.02)
    assert decision.venue == 'NYSE'
    print("  PASS: test_inactive_venue_skipped")


def test_fee_tiebreaker():
    """Equal prices → venue with lower fee wins."""
    router = SmartOrderRouter()
    router.add_venue(Venue(name='A', latency_ns=200, fee_per_share=0.003))
    router.add_venue(Venue(name='B', latency_ns=200, fee_per_share=-0.002))
    router.update_quote('A', 150.00, 150.01, 500, 500)
    router.update_quote('B', 150.00, 150.01, 500, 500)  # same price
    decision = router.route_order('BUY', 100)
    assert decision is not None
    assert decision.venue == 'B', f"Expected B (lower fee), got {decision.venue}"
    print("  PASS: test_fee_tiebreaker")


def test_routing_speed():
    """Routing decision should be fast (<50 microseconds)."""
    router = _make_router()
    start = time.time_ns()
    for _ in range(1000):
        router.route_order('BUY', 100)
    total_ns = time.time_ns() - start
    avg_ns = total_ns / 1000  # per-route in nanoseconds
    assert avg_ns < 50000, f"Too slow: {avg_ns:.0f} ns/route"
    print(f"  PASS: test_routing_speed ({avg_ns:.0f} ns)")


def test_stats_tracking():
    """Stats should accurately track routes."""
    router = _make_router()
    for _ in range(10):
        router.route_order('BUY', 100)
    assert router.stats.total_routes == 10
    assert sum(router.stats.routes_by_venue.values()) == 10
    assert router.stats.avg_latency_ns > 0
    print("  PASS: test_stats_tracking")


if __name__ == '__main__':
    print("=== Smart Order Router Unit Tests ===\n")
    tests = [
        test_best_price_buy,
        test_best_price_sell,
        test_lowest_latency,
        test_split_order,
        test_split_partial_fill,
        test_no_venues,
        test_inactive_venue_skipped,
        test_fee_tiebreaker,
        test_routing_speed,
        test_stats_tracking,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests passed")
