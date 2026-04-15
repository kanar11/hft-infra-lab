#!/usr/bin/env python3
"""
Standalone Risk Manager for HFT Infrastructure Lab

Enforces risk limits independently of the OMS. Designed to sit between
the strategy/router and the OMS as a pre-trade risk gate.

Features:
- Per-symbol and portfolio-wide position limits
- Daily P&L loss limit (circuit breaker)
- Order rate limiting (max orders per second)
- Kill switch (halt all trading instantly)
- Drawdown tracking from peak P&L

Pipeline integration:
  Strategy → Router → **Risk Manager** → OMS → Fill Engine → P&L
"""
import time
from enum import Enum
from dataclasses import dataclass, field
from typing import Dict, Optional


class RiskAction(Enum):
    """Result of a risk check."""
    ALLOW = "ALLOW"
    REJECT = "REJECT"
    KILL = "KILL"


@dataclass
class RiskCheckResult:
    """Detailed result of a risk check.

    Attributes:
        action: ALLOW, REJECT, or KILL
        reason: Human-readable explanation
        latency_ns: Time taken for the risk check
    """
    action: RiskAction
    reason: str
    latency_ns: int


@dataclass
class RiskLimits:
    """Configurable risk limits.

    Attributes:
        max_position_per_symbol: Max shares in any single symbol
        max_portfolio_exposure: Max total absolute exposure across all symbols
        max_daily_loss: Max allowed daily P&L loss before circuit breaker trips
        max_orders_per_second: Rate limit on order submissions
        max_order_value: Max notional value of a single order
        max_drawdown_pct: Max drawdown from peak P&L as percentage (0-100)
    """
    max_position_per_symbol: int = 5000
    max_portfolio_exposure: int = 50000
    max_daily_loss: float = 100000.0
    max_orders_per_second: int = 1000
    max_order_value: float = 500000.0
    max_drawdown_pct: float = 5.0


@dataclass
class RiskState:
    """Live risk state tracking."""
    positions: Dict[str, int] = field(default_factory=dict)
    daily_pnl: float = 0.0
    peak_pnl: float = 0.0
    order_timestamps: list = field(default_factory=list)
    kill_switch_active: bool = False
    total_checks: int = 0
    total_rejects: int = 0
    total_latency_ns: int = 0
    reject_reasons: Dict[str, int] = field(default_factory=dict)


class RiskManager:
    """Standalone pre-trade risk manager with circuit breaker and kill switch.

    Args:
        limits: Configurable risk limits (position, loss, rate, etc.)
    """

    def __init__(self, limits: Optional[RiskLimits] = None) -> None:
        self.limits = limits or RiskLimits()
        self.state = RiskState()

    def check_order(self, symbol: str, side: str, price: float,
                    quantity: int) -> RiskCheckResult:
        """Run all pre-trade risk checks on a proposed order.

        Args:
            symbol: Instrument symbol
            side: 'BUY' or 'SELL'
            price: Order price
            quantity: Number of shares
        Returns:
            RiskCheckResult with ALLOW, REJECT, or KILL
        """
        start = time.time_ns()
        self.state.total_checks += 1

        # Kill switch — reject everything
        if self.state.kill_switch_active:
            return self._reject('KILL_SWITCH', 'Kill switch is active — all trading halted', start)

        # Order value check
        order_value = price * quantity
        if order_value > self.limits.max_order_value:
            return self._reject('ORDER_VALUE',
                                f'Order value ${order_value:,.0f} > limit ${self.limits.max_order_value:,.0f}',
                                start)

        # Position limit per symbol
        current_pos = self.state.positions.get(symbol, 0)
        projected = current_pos + (quantity if side == 'BUY' else -quantity)
        if abs(projected) > self.limits.max_position_per_symbol:
            return self._reject('POSITION_LIMIT',
                                f'{symbol} projected position {projected} > limit {self.limits.max_position_per_symbol}',
                                start)

        # Portfolio exposure check
        test_positions = dict(self.state.positions)
        test_positions[symbol] = projected
        total_exposure = sum(abs(v) for v in test_positions.values())
        if total_exposure > self.limits.max_portfolio_exposure:
            return self._reject('PORTFOLIO_EXPOSURE',
                                f'Total exposure {total_exposure} > limit {self.limits.max_portfolio_exposure}',
                                start)

        # Daily loss limit (circuit breaker)
        if self.state.daily_pnl < -self.limits.max_daily_loss:
            self.state.kill_switch_active = True
            return self._reject('CIRCUIT_BREAKER',
                                f'Daily loss ${-self.state.daily_pnl:,.0f} > limit ${self.limits.max_daily_loss:,.0f} — KILL SWITCH ACTIVATED',
                                start)

        # Drawdown check
        if self.state.peak_pnl > 0:
            drawdown_pct = (self.state.peak_pnl - self.state.daily_pnl) / self.state.peak_pnl * 100
            if drawdown_pct > self.limits.max_drawdown_pct:
                self.state.kill_switch_active = True
                return self._reject('DRAWDOWN',
                                    f'Drawdown {drawdown_pct:.1f}% > limit {self.limits.max_drawdown_pct}% — KILL SWITCH ACTIVATED',
                                    start)

        # Rate limiting
        now = time.time_ns()
        one_sec_ago = now - 1_000_000_000
        self.state.order_timestamps = [t for t in self.state.order_timestamps if t > one_sec_ago]
        if len(self.state.order_timestamps) >= self.limits.max_orders_per_second:
            return self._reject('RATE_LIMIT',
                                f'{len(self.state.order_timestamps)} orders/sec > limit {self.limits.max_orders_per_second}',
                                start)
        self.state.order_timestamps.append(now)

        elapsed = time.time_ns() - start
        self.state.total_latency_ns += elapsed
        return RiskCheckResult(action=RiskAction.ALLOW, reason='All checks passed', latency_ns=elapsed)

    def update_position(self, symbol: str, side: str, quantity: int) -> None:
        """Update position after a fill.

        Args:
            symbol: Instrument symbol
            side: 'BUY' or 'SELL'
            quantity: Filled quantity
        """
        current = self.state.positions.get(symbol, 0)
        if side == 'BUY':
            self.state.positions[symbol] = current + quantity
        else:
            self.state.positions[symbol] = current - quantity

    def update_pnl(self, pnl_change: float) -> None:
        """Update daily P&L (called after each fill).

        Args:
            pnl_change: Realized P&L from this fill
        """
        self.state.daily_pnl += pnl_change
        if self.state.daily_pnl > self.state.peak_pnl:
            self.state.peak_pnl = self.state.daily_pnl

    def activate_kill_switch(self) -> None:
        """Manually activate kill switch — halts all trading."""
        self.state.kill_switch_active = True

    def deactivate_kill_switch(self) -> None:
        """Deactivate kill switch — resume trading."""
        self.state.kill_switch_active = False

    def reset_daily(self) -> None:
        """Reset daily state (call at start of trading day)."""
        self.state.daily_pnl = 0.0
        self.state.peak_pnl = 0.0
        self.state.order_timestamps.clear()
        self.state.kill_switch_active = False

    def _reject(self, reason_code: str, reason: str, start: int) -> RiskCheckResult:
        """Record a rejection and return result."""
        elapsed = time.time_ns() - start
        self.state.total_rejects += 1
        self.state.total_latency_ns += elapsed
        self.state.reject_reasons[reason_code] = self.state.reject_reasons.get(reason_code, 0) + 1
        return RiskCheckResult(action=RiskAction.REJECT, reason=reason, latency_ns=elapsed)

    def print_stats(self) -> None:
        """Print risk manager statistics."""
        print(f"\n=== Risk Manager Statistics ===")
        print(f"  Total checks: {self.state.total_checks}")
        print(f"  Allowed: {self.state.total_checks - self.state.total_rejects}")
        print(f"  Rejected: {self.state.total_rejects}")
        if self.state.reject_reasons:
            print(f"  Reject breakdown:")
            for reason, count in sorted(self.state.reject_reasons.items()):
                print(f"    {reason}: {count}")
        avg = self.state.total_latency_ns / self.state.total_checks if self.state.total_checks > 0 else 0
        print(f"  Avg check latency: {avg:.0f} ns")
        print(f"  Kill switch: {'ACTIVE' if self.state.kill_switch_active else 'inactive'}")
        print(f"  Daily P&L: ${self.state.daily_pnl:,.2f}")
        if self.state.positions:
            print(f"  Positions:")
            for sym, qty in sorted(self.state.positions.items()):
                print(f"    {sym}: {qty:+d}")


def demo() -> None:
    """Run standalone demo showing risk checks, circuit breaker, and kill switch."""
    import random

    print("=== Risk Manager Demo ===\n")
    print("Limits: position=5000/sym, portfolio=50000, daily_loss=$100K, rate=1000/sec\n")

    limits = RiskLimits(
        max_position_per_symbol=5000,
        max_portfolio_exposure=50000,
        max_daily_loss=100000,
        max_orders_per_second=1000,
        max_order_value=500000,
    )
    rm = RiskManager(limits=limits)
    rng = random.Random(42)

    stocks = ['AAPL', 'MSFT', 'GOOGL', 'TSLA']
    allowed = 0
    rejected = 0

    for i in range(200):
        stock = rng.choice(stocks)
        side = rng.choice(['BUY', 'SELL'])
        price = round(rng.uniform(100, 500), 2)
        qty = rng.choice([100, 200, 500, 1000, 2000])

        result = rm.check_order(stock, side, price, qty)

        if result.action == RiskAction.ALLOW:
            allowed += 1
            rm.update_position(stock, side, qty)
            # Simulate random P&L
            pnl = rng.uniform(-500, 500)
            rm.update_pnl(pnl)
            if i < 10:
                print(f"  [{i:3d}] ALLOW {side:4s} {qty:4d} {stock} @ ${price:.2f}")
        else:
            rejected += 1
            if rejected <= 10:
                print(f"  [{i:3d}] REJECT {side:4s} {qty:4d} {stock} — {result.reason}")

        if rm.state.kill_switch_active:
            print(f"\n  !!! KILL SWITCH ACTIVATED at order {i} !!!")
            break

    if allowed + rejected > 20:
        print(f"  ... ({allowed + rejected - 20} more orders)")

    rm.print_stats()


if __name__ == '__main__':
    demo()
