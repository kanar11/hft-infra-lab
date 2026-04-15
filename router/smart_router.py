#!/usr/bin/env python3
"""
Smart Order Router (SOR) for HFT Infrastructure Lab

Routes orders to the best venue based on price, latency, available liquidity,
and fill probability. Supports multiple routing strategies:

- BEST_PRICE: Route to venue with best bid/ask (default)
- LOWEST_LATENCY: Route to fastest venue
- SPLIT: Split large orders across venues to minimize market impact

Pipeline integration:
  Strategy (signals) → Smart Router (venue selection) → OMS (risk checks) → Fill
"""
import time
from enum import Enum
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


class RoutingStrategy(Enum):
    """Order routing strategy."""
    BEST_PRICE = "BEST_PRICE"
    LOWEST_LATENCY = "LOWEST_LATENCY"
    SPLIT = "SPLIT"


@dataclass
class Venue:
    """Trading venue with pricing and performance characteristics.

    Attributes:
        name: Venue identifier (e.g., 'NYSE', 'NASDAQ')
        latency_ns: Average round-trip latency in nanoseconds
        fee_per_share: Trading fee per share (negative = rebate)
        best_bid: Current best bid price
        best_ask: Current best ask price
        bid_size: Available shares at best bid
        ask_size: Available shares at best ask
        is_active: Whether venue is currently accepting orders
    """
    name: str
    latency_ns: int
    fee_per_share: float
    best_bid: float = 0.0
    best_ask: float = 0.0
    bid_size: int = 0
    ask_size: int = 0
    is_active: bool = True


@dataclass
class RouteDecision:
    """Result of a routing decision.

    Attributes:
        venue: Target venue name
        price: Expected execution price
        quantity: Number of shares to send
        latency_ns: Decision latency in nanoseconds
        reason: Human-readable explanation
    """
    venue: str
    price: float
    quantity: int
    latency_ns: int
    reason: str


@dataclass
class SplitOrder:
    """A portion of a split order routed to one venue."""
    venue: str
    quantity: int
    price: float


@dataclass
class RouterStats:
    """Performance statistics for the router."""
    total_routes: int = 0
    routes_by_venue: Dict[str, int] = field(default_factory=dict)
    routes_by_strategy: Dict[str, int] = field(default_factory=dict)
    total_latency_ns: int = 0
    rejected: int = 0

    @property
    def avg_latency_ns(self) -> float:
        if self.total_routes == 0:
            return 0.0
        return self.total_latency_ns / self.total_routes


class SmartOrderRouter:
    """Smart Order Router — selects optimal venue for each order.

    Args:
        strategy: Default routing strategy (BEST_PRICE, LOWEST_LATENCY, SPLIT)
        split_threshold: Minimum order size before splitting across venues
    """

    def __init__(self, strategy: RoutingStrategy = RoutingStrategy.BEST_PRICE,
                 split_threshold: int = 500) -> None:
        self.default_strategy = strategy
        self.split_threshold = split_threshold
        self.venues: Dict[str, Venue] = {}
        self.stats = RouterStats()

    def add_venue(self, venue: Venue) -> None:
        """Register a trading venue."""
        self.venues[venue.name] = venue

    def update_quote(self, venue_name: str, bid: float, ask: float,
                     bid_size: int, ask_size: int) -> None:
        """Update best bid/ask for a venue (called on every market data tick).

        Args:
            venue_name: Venue identifier
            bid: Best bid price
            ask: Best ask price
            bid_size: Shares available at best bid
            ask_size: Shares available at best ask
        """
        venue = self.venues.get(venue_name)
        if venue:
            venue.best_bid = bid
            venue.best_ask = ask
            venue.bid_size = bid_size
            venue.ask_size = ask_size

    def route_order(self, side: str, quantity: int,
                    strategy: Optional[RoutingStrategy] = None) -> Optional[RouteDecision]:
        """Route an order to the best venue.

        Args:
            side: 'BUY' or 'SELL'
            quantity: Number of shares
            strategy: Override default routing strategy
        Returns:
            RouteDecision if a venue is found, None if no venue available
        """
        start = time.time_ns()
        strat = strategy or self.default_strategy

        active = [v for v in self.venues.values() if v.is_active]
        if not active:
            self.stats.rejected += 1
            return None

        # Filter venues with sufficient liquidity
        if side == 'BUY':
            candidates = [v for v in active if v.best_ask > 0 and v.ask_size > 0]
        else:
            candidates = [v for v in active if v.best_bid > 0 and v.bid_size > 0]

        if not candidates:
            self.stats.rejected += 1
            return None

        # Select venue based on strategy
        if strat == RoutingStrategy.BEST_PRICE:
            best = self._best_price(candidates, side)
        elif strat == RoutingStrategy.LOWEST_LATENCY:
            best = self._lowest_latency(candidates)
        elif strat == RoutingStrategy.SPLIT and quantity >= self.split_threshold:
            return self._split_order(candidates, side, quantity, start)
        else:
            best = self._best_price(candidates, side)

        price = best.best_ask if side == 'BUY' else best.best_bid
        elapsed = time.time_ns() - start

        # Update stats
        self.stats.total_routes += 1
        self.stats.total_latency_ns += elapsed
        self.stats.routes_by_venue[best.name] = self.stats.routes_by_venue.get(best.name, 0) + 1
        strat_name = strat.value
        self.stats.routes_by_strategy[strat_name] = self.stats.routes_by_strategy.get(strat_name, 0) + 1

        return RouteDecision(
            venue=best.name,
            price=price,
            quantity=quantity,
            latency_ns=elapsed,
            reason=f'{strat.value}: {best.name} @ ${price:.2f} '
                   f'(latency={best.latency_ns}ns, fee=${best.fee_per_share:.4f}/sh)'
        )

    def _best_price(self, candidates: List[Venue], side: str) -> Venue:
        """Select venue with best price (lowest ask for BUY, highest bid for SELL).

        Ties broken by: lower fee → lower latency → first in list.
        """
        if side == 'BUY':
            candidates.sort(key=lambda v: (v.best_ask, v.fee_per_share, v.latency_ns))
        else:
            candidates.sort(key=lambda v: (-v.best_bid, v.fee_per_share, v.latency_ns))
        return candidates[0]

    def _lowest_latency(self, candidates: List[Venue]) -> Venue:
        """Select venue with lowest round-trip latency."""
        candidates.sort(key=lambda v: v.latency_ns)
        return candidates[0]

    def _split_order(self, candidates: List[Venue], side: str,
                     quantity: int, start: int) -> Optional[RouteDecision]:
        """Split large order across venues proportional to available liquidity.

        Allocates shares to each venue based on its share of total available
        liquidity, ensuring no venue receives more than its displayed size.
        """
        # Sort by price (best first)
        if side == 'BUY':
            candidates.sort(key=lambda v: (v.best_ask, v.fee_per_share))
            sizes = [(v, v.ask_size, v.best_ask) for v in candidates]
        else:
            candidates.sort(key=lambda v: (-v.best_bid, v.fee_per_share))
            sizes = [(v, v.bid_size, v.best_bid) for v in candidates]

        # Allocate across venues
        splits: List[SplitOrder] = []
        remaining = quantity
        for venue, available, price in sizes:
            if remaining <= 0:
                break
            alloc = min(remaining, available)
            if alloc > 0:
                splits.append(SplitOrder(venue=venue.name, quantity=alloc, price=price))
                remaining -= alloc

        if not splits:
            self.stats.rejected += 1
            return None

        # Use best price as representative
        avg_price = sum(s.price * s.quantity for s in splits) / sum(s.quantity for s in splits)
        filled_qty = sum(s.quantity for s in splits)
        elapsed = time.time_ns() - start

        # Update stats
        self.stats.total_routes += 1
        self.stats.total_latency_ns += elapsed
        for s in splits:
            self.stats.routes_by_venue[s.venue] = self.stats.routes_by_venue.get(s.venue, 0) + 1
        self.stats.routes_by_strategy['SPLIT'] = self.stats.routes_by_strategy.get('SPLIT', 0) + 1

        venue_list = ', '.join(f'{s.venue}:{s.quantity}' for s in splits)
        return RouteDecision(
            venue=splits[0].venue,  # primary venue
            price=round(avg_price, 2),
            quantity=filled_qty,
            latency_ns=elapsed,
            reason=f'SPLIT across {len(splits)} venues: [{venue_list}] '
                   f'avg=${avg_price:.2f} ({quantity - filled_qty} unfilled)'
        )

    def print_stats(self) -> None:
        """Print routing performance summary."""
        print(f"\n=== Router Statistics ===")
        print(f"  Total routes: {self.stats.total_routes}")
        print(f"  Rejected: {self.stats.rejected}")
        if self.stats.routes_by_venue:
            print(f"  By venue:")
            for venue, count in sorted(self.stats.routes_by_venue.items()):
                pct = count / self.stats.total_routes * 100 if self.stats.total_routes > 0 else 0
                print(f"    {venue}: {count} ({pct:.1f}%)")
        if self.stats.routes_by_strategy:
            print(f"  By strategy:")
            for strat, count in sorted(self.stats.routes_by_strategy.items()):
                print(f"    {strat}: {count}")
        print(f"  Avg routing latency: {self.stats.avg_latency_ns:.0f} ns")


def demo() -> None:
    """Run standalone demo with 3 venues and mixed order flow."""
    import random

    print("=== Smart Order Router Demo ===\n")

    # Set up venues
    router = SmartOrderRouter(strategy=RoutingStrategy.BEST_PRICE, split_threshold=500)
    router.add_venue(Venue(name='NYSE', latency_ns=500, fee_per_share=0.003))
    router.add_venue(Venue(name='NASDAQ', latency_ns=200, fee_per_share=-0.002))  # rebate
    router.add_venue(Venue(name='BATS', latency_ns=150, fee_per_share=-0.001))

    rng = random.Random(42)
    base_price = 150.00

    print(f"Venues: NYSE (500ns, $0.003/sh), NASDAQ (200ns, -$0.002/sh rebate), BATS (150ns, -$0.001/sh)")
    print(f"Strategies: BEST_PRICE, LOWEST_LATENCY, SPLIT (threshold=500)\n")

    strategies = [RoutingStrategy.BEST_PRICE, RoutingStrategy.LOWEST_LATENCY, RoutingStrategy.SPLIT]
    decisions: List[RouteDecision] = []

    for i in range(200):
        # Simulate varying quotes across venues
        mid = base_price + rng.gauss(0, 0.5)
        spread_nyse = rng.uniform(0.01, 0.05)
        spread_nasdaq = rng.uniform(0.01, 0.04)
        spread_bats = rng.uniform(0.01, 0.06)

        router.update_quote('NYSE',
                            round(mid - spread_nyse, 2), round(mid + spread_nyse, 2),
                            rng.randint(100, 1000), rng.randint(100, 1000))
        router.update_quote('NASDAQ',
                            round(mid - spread_nasdaq, 2), round(mid + spread_nasdaq, 2),
                            rng.randint(100, 800), rng.randint(100, 800))
        router.update_quote('BATS',
                            round(mid - spread_bats, 2), round(mid + spread_bats, 2),
                            rng.randint(50, 500), rng.randint(50, 500))

        side = rng.choice(['BUY', 'SELL'])
        qty = rng.choice([100, 200, 300, 500, 1000])
        strat = rng.choice(strategies)

        decision = router.route_order(side, qty, strategy=strat)
        if decision:
            decisions.append(decision)
            if len(decisions) <= 10:
                print(f"  [{i:3d}] {side:4s} {qty:4d} → {decision.reason}")

    if len(decisions) > 10:
        print(f"  ... ({len(decisions) - 10} more routes)")

    router.print_stats()


if __name__ == '__main__':
    demo()
