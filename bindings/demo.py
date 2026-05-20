"""
demo.py — exercise the pyhft bindings.

Run from repo root after building:
    python3 bindings/setup.py build_ext --inplace
    python3 bindings/demo.py
"""

import os
import sys

# Make sure we can find the freshly-built pyhft*.so
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import pyhft


def section(title: str) -> None:
    print(f"\n--- {title} ---")


def demo_oms() -> None:
    section("OMS — submit, fill, position")
    oms = pyhft.OMS(max_position=1000, max_order_value=100_000.0)

    order = oms.submit_order("AAPL", pyhft.Side.BUY, 150.25, 100)
    assert order is not None
    assert order.status == pyhft.OrderStatus.SENT
    print(f"  submitted order_id={order.order_id} {order.symbol} {order.quantity} shares")

    oms.fill_order(order.order_id, 100, 150.25)
    pos = oms.get_position("AAPL")
    assert pos.net_qty == 100
    print(f"  position: {pos.symbol} net={pos.net_qty} avg_px={pos.avg_price/10000.0:.4f}")


def demo_risk() -> None:
    section("RiskManager — pending exposure rejection")
    lim = pyhft.RiskLimits()
    lim.max_position_per_symbol = 100
    risk = pyhft.RiskManager(lim)

    r1 = risk.check_order("AAPL", pyhft.Side.BUY, 10.0, 100)
    print(f"  first  100 BUY  → {r1.action.name}  ({r1.reason})")
    assert r1.action == pyhft.RiskAction.ALLOW

    risk.on_order_sent("AAPL", pyhft.Side.BUY, 100)
    r2 = risk.check_order("AAPL", pyhft.Side.BUY, 10.0, 1)
    print(f"  second 1   BUY  → {r2.action.name}  ({r2.reason})")
    assert r2.action == pyhft.RiskAction.REJECT


def demo_orderbook() -> None:
    section("FlatOrderBook — submit + cancel + modify by ID")
    book = pyhft.FlatOrderBook()

    book.submit_with_id(101, 15000, 10, True)   # BUY  10 @ 150.00
    book.submit_with_id(102, 15010, 8,  False)  # SELL  8 @ 150.10
    print(f"  best_bid={book.best_bid()/100:.2f}  best_ask={book.best_ask()/100:.2f}")
    print(f"  tracked orders: {book.tracked_orders()}")

    book.cancel(101)
    print(f"  after cancel(101): best_bid={book.best_bid()} (NO_BID=-1)")

    book.modify(102, 15005, 16)  # move ask 150.10 → 150.05, double qty
    print(f"  after modify(102, 15005, 16): ask_qty@15005 = {book.ask_qty_at(15005)}")


if __name__ == "__main__":
    demo_oms()
    demo_risk()
    demo_orderbook()
    print("\nAll demos OK")
