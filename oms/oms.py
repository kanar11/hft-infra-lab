#!/usr/bin/env python3
"""
Order Management System (OMS) with Pre-Trade Risk Checks and P&L Tracking

Handles order lifecycle (submit → risk check → fill → P&L), position tracking
with correct average cost basis, and configurable risk limits.
Obsługuje cykl życia zamówienia (przesłanie → kontrola ryzyka → wypełnienie → P&L), śledzenie pozycji
z poprawną średnią ceną kosztów i konfigurowalnymi limitami ryzyka.
"""
import os
import time
import logging
from enum import Enum
from dataclasses import dataclass, field
from typing import Dict, Optional

logger = logging.getLogger('oms')


class OrderStatus(Enum):
    NEW = "NEW"
    SENT = "SENT"
    FILLED = "FILLED"
    PARTIALLY_FILLED = "PARTIAL"
    CANCELLED = "CANCELLED"
    REJECTED = "REJECTED"


class Side(Enum):
    BUY = "BUY"
    SELL = "SELL"


@dataclass
class Order:
    order_id: int
    symbol: str
    side: Side
    price: float
    quantity: int
    filled_qty: int = 0
    status: OrderStatus = OrderStatus.NEW
    created_ns: int = 0
    sent_ns: int = 0
    filled_ns: int = 0


@dataclass
class Position:
    symbol: str
    net_qty: int = 0
    avg_price: float = 0.0
    realized_pnl: float = 0.0
    total_cost: float = 0.0


class OMS:
    """Order Management System with pre-trade risk checks and P&L tracking.
    System zarządzania zamówieniami z kontrolami ryzyka przed transakcją i śledzeniem P&L."""

    def __init__(self, max_position: int = 1000, max_order_value: float = 100000) -> None:
        self.orders: Dict[int, Order] = {}
        self.positions: Dict[str, Position] = {}
        self.next_id: int = 1
        self.max_position: int = max_position
        self.max_order_value: float = max_order_value

    def submit_order(self, symbol: str, side: Side, price: float,
                     quantity: int) -> Optional[Order]:
        """Submit new order with pre-trade risk checks.
        Przesłanie nowego zamówienia z kontrolami ryzyka przed transakcją.
        Args:
            symbol: Instrument symbol (e.g., 'AAPL')
            side: Side.BUY or Side.SELL
            price: Order price (must be positive, non-NaN)
            quantity: Number of shares (must be positive int)
        Returns:
            Order object if accepted, None if rejected
        """
        start = time.time_ns()

        # Input validation
        # Walidacja danych wejściowych
        if not isinstance(price, (int, float)) or price != price:  # NaN check
            logger.warning(f"REJECTED: invalid price {price}")
            return None
        if price <= 0:
            logger.warning(f"REJECTED: price must be positive, got {price}")
            return None
        if not isinstance(quantity, int) or quantity <= 0:
            logger.warning(f"REJECTED: quantity must be positive int, got {quantity}")
            return None
        if not isinstance(symbol, str) or len(symbol) == 0:
            logger.warning(f"REJECTED: invalid symbol")
            return None

        # Risk check: order value
        # Kontrola ryzyka: wartość zamówienia
        order_value = price * quantity
        if order_value > self.max_order_value:
            logger.warning(f"REJECTED: order value ${order_value:,.0f} > limit ${self.max_order_value:,.0f}")
            return None

        # Risk check: position limit
        # Kontrola ryzyka: limit pozycji
        pos = self.positions.get(symbol, Position(symbol))
        projected = pos.net_qty + (quantity if side == Side.BUY else -quantity)
        if abs(projected) > self.max_position:
            logger.warning(f"REJECTED: position {projected} > limit {self.max_position}")
            return None

        order = Order(
            order_id=self.next_id,
            symbol=symbol,
            side=side,
            price=price,
            quantity=quantity,
            created_ns=start
        )
        self.orders[self.next_id] = order
        self.next_id += 1

        order.status = OrderStatus.SENT
        order.sent_ns = time.time_ns()

        latency = (order.sent_ns - order.created_ns) / 1000
        logger.info(f"ORDER #{order.order_id}: {side.value} {quantity} {symbol} @ {price} [{latency:.1f}us]")
        return order

    def fill_order(self, order_id: int, fill_qty: int, fill_price: float) -> None:
        """Process execution report (fill).
        Przetwarzanie raportu wykonania (wypełnienie).
        Args:
            order_id: ID of the order being filled
            fill_qty: Number of shares filled
            fill_price: Execution price
        """
        order = self.orders.get(order_id)
        if not order:
            return

        order.filled_qty += fill_qty
        if order.filled_qty >= order.quantity:
            order.status = OrderStatus.FILLED
        else:
            order.status = OrderStatus.PARTIALLY_FILLED
        order.filled_ns = time.time_ns()

        # Update position
        # Aktualizuj pozycję
        if order.symbol not in self.positions:
            self.positions[order.symbol] = Position(order.symbol)
        pos = self.positions[order.symbol]

        if order.side == Side.BUY:
            pos.total_cost += fill_qty * fill_price
            pos.net_qty += fill_qty
        else:
            if pos.net_qty > 0:
                pos.realized_pnl += fill_qty * (fill_price - pos.avg_price)
                pos.total_cost -= fill_qty * pos.avg_price
            pos.net_qty -= fill_qty

        if pos.net_qty > 0:
            pos.avg_price = pos.total_cost / pos.net_qty
        elif pos.net_qty == 0:
            pos.avg_price = 0.0
            pos.total_cost = 0.0

        logger.info(f"FILL: #{order_id} {fill_qty}@{fill_price} status={order.status.value}")

    def cancel_order(self, order_id: int) -> None:
        """Cancel an active or partially filled order.
        Anuluj aktywne lub częściowo wypełnione zamówienie."""
        order = self.orders.get(order_id)
        if order and order.status in (OrderStatus.SENT, OrderStatus.PARTIALLY_FILLED):
            order.status = OrderStatus.CANCELLED
            logger.info(f"CANCEL: #{order_id}")

    def print_positions(self) -> None:
        """Print current positions and realized P&L.
        Wydrukuj bieżące pozycje i zrealizowany P&L."""
        logger.info("\n=== POSITIONS ===")
        for sym, pos in self.positions.items():
            logger.info(f"  {sym}: qty={pos.net_qty} avg={pos.avg_price:.2f} realized_pnl=${pos.realized_pnl:.2f}")

    def print_orders(self) -> None:
        """Print all orders and their status.
        Wydrukuj wszystkie zamówienia i ich status."""
        logger.info("\n=== ORDERS ===")
        for oid, order in self.orders.items():
            logger.info(f"  #{oid}: {order.side.value} {order.quantity} {order.symbol} @ {order.price} [{order.status.value}] filled={order.filled_qty}")


def main() -> None:
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
    from config_loader import load_config, setup_logging
    cfg = load_config()
    setup_logging()
    oms_cfg = cfg['oms']

    logger.info("=== HFT Order Management System ===")
    oms = OMS(max_position=oms_cfg.get('max_position', 500), max_order_value=oms_cfg.get('max_order_value', 50000))

    # Simulate trading session
    # Symuluj sesję transakcyjną
    logger.info("--- Submitting orders ---")
    o1 = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    o2 = oms.submit_order("AAPL", Side.BUY, 150.50, 200)
    o3 = oms.submit_order("MSFT", Side.SELL, 380.00, 50)

    # Risk check: too large
    logger.info("--- Risk check: large order ---")
    oms.submit_order("AAPL", Side.BUY, 200.00, 300)

    # Fills
    logger.info("--- Processing fills ---")
    oms.fill_order(1, 100, 150.00)
    oms.fill_order(2, 100, 150.50)
    oms.fill_order(2, 100, 150.45)
    oms.fill_order(3, 50, 380.00)

    # Cancel
    logger.info("--- Cancel ---")
    o4 = oms.submit_order("AAPL", Side.SELL, 151.00, 50)
    oms.cancel_order(4)

    oms.print_orders()
    oms.print_positions()


if __name__ == '__main__':
    main()
