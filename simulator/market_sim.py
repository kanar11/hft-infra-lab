#!/usr/bin/env python3
"""
HFT Market Data Simulator — End-to-End Pipeline Demo

Generates synthetic ITCH market data, parses it through the ITCH parser,
routes orders through the OMS with risk checks, and tracks P&L.

Pipeline: ITCH Generator → ITCH Parser → Strategy (optional) → OMS (risk checks) → Fill Engine → P&L

This proves all modules work together as a complete trading system.
Use --strategy flag to enable mean reversion strategy.
"""
import os
import sys
import time
import struct
import random
from typing import List, Dict, Any, Tuple

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from itch_parser.itch_parser import ITCHMessage
from oms.oms import OMS, Side
from strategy.mean_reversion import MeanReversionStrategy


# --- Market Data Generator ---

class MarketDataGenerator:
    """Generates realistic ITCH 5.0 binary messages for simulation."""

    STOCKS = ['AAPL', 'MSFT', 'GOOGL', 'AMZN', 'TSLA', 'META', 'NVDA', 'JPM']
    BASE_PRICES = {
        'AAPL': 175.00, 'MSFT': 410.00, 'GOOGL': 155.00, 'AMZN': 185.00,
        'TSLA': 245.00, 'META': 500.00, 'NVDA': 880.00, 'JPM': 195.00,
    }

    def __init__(self, seed: int = 42) -> None:
        self.rng = random.Random(seed)
        self.seq = 0
        self.order_ref = 1000
        self.active_orders: Dict[int, Dict[str, Any]] = {}

    def _next_ts(self) -> int:
        """Generate incrementing nanosecond timestamp."""
        self.seq += 1
        return 34200_000_000_000 + self.seq * 1_000_000  # 9:30 AM + seq ms

    def generate_add_order(self) -> bytes:
        """Generate a random Add Order (A) message."""
        stock = self.rng.choice(self.STOCKS)
        base = self.BASE_PRICES[stock]
        price = round(base + self.rng.uniform(-2.0, 2.0), 2)
        side = self.rng.choice([b'B', b'S'])
        shares = self.rng.choice([10, 25, 50, 100, 200, 500])
        self.order_ref += 1
        ref = self.order_ref

        self.active_orders[ref] = {
            'stock': stock, 'price': price,
            'side': 'BUY' if side == b'B' else 'SELL', 'shares': shares
        }

        return struct.pack('!c q q c I 8s I',
            b'A', self._next_ts(), ref, side, shares,
            stock.encode().ljust(8), int(price * 10000))

    def generate_execute(self) -> bytes:
        """Generate Order Executed (E) message for a random active order."""
        if not self.active_orders:
            return self.generate_add_order()
        ref = self.rng.choice(list(self.active_orders.keys()))
        order = self.active_orders[ref]
        exec_shares = min(order['shares'], self.rng.choice([10, 25, 50]))
        match_num = self.rng.randint(10000, 99999)

        order['shares'] -= exec_shares
        if order['shares'] <= 0:
            del self.active_orders[ref]

        return struct.pack('!c q q I q',
            b'E', self._next_ts(), ref, exec_shares, match_num)

    def generate_cancel(self) -> bytes:
        """Generate Order Cancelled (C) message."""
        if not self.active_orders:
            return self.generate_add_order()
        ref = self.rng.choice(list(self.active_orders.keys()))
        order = self.active_orders.pop(ref)

        return struct.pack('!c q q I',
            b'C', self._next_ts(), ref, order['shares'])

    def generate_trade(self) -> bytes:
        """Generate Trade (P) message."""
        stock = self.rng.choice(self.STOCKS)
        base = self.BASE_PRICES[stock]
        price = round(base + self.rng.uniform(-1.0, 1.0), 2)
        shares = self.rng.choice([100, 200, 500])
        side = self.rng.choice([b'B', b'S'])
        match_num = self.rng.randint(10000, 99999)

        return struct.pack('!c q q c I 8s I q',
            b'P', self._next_ts(), 0, side, shares,
            stock.encode().ljust(8), int(price * 10000), match_num)

    def generate_system_event(self, event: bytes = b'Q') -> bytes:
        """Generate System Event (S) message."""
        return struct.pack('!c q c', b'S', self._next_ts(), event)

    def generate_stream(self, num_messages: int = 1000) -> List[bytes]:
        """Generate a realistic stream of mixed ITCH messages."""
        messages = []

        # Start of day
        messages.append(self.generate_system_event(b'O'))  # start of messages
        messages.append(self.generate_system_event(b'S'))  # start of system hours
        messages.append(self.generate_system_event(b'Q'))  # start of market hours

        for _ in range(num_messages):
            roll = self.rng.random()
            if roll < 0.45:
                messages.append(self.generate_add_order())
            elif roll < 0.70:
                messages.append(self.generate_execute())
            elif roll < 0.85:
                messages.append(self.generate_trade())
            else:
                messages.append(self.generate_cancel())

        # End of day
        messages.append(self.generate_system_event(b'M'))  # end of market hours
        messages.append(self.generate_system_event(b'E'))  # end of system hours
        messages.append(self.generate_system_event(b'C'))  # end of messages

        return messages


# --- Pipeline Runner ---

def run_pipeline(num_messages: int = 1000, use_strategy: bool = False) -> Dict[str, Any]:
    """Run full pipeline: generate → parse → (strategy) → OMS → stats.

    Args:
        num_messages: Number of market data messages to simulate
        use_strategy: Enable mean reversion strategy for signal generation
    Returns:
        Dict with pipeline statistics
    """
    mode = "Strategy Mode (Mean Reversion)" if use_strategy else "Direct Mode (all orders)"
    print(f"=== HFT Market Data Simulator ===")
    print(f"Pipeline: ITCH Generator → Parser → {'Strategy → ' if use_strategy else ''}OMS → P&L")
    print(f"Mode: {mode}")
    print(f"Messages: {num_messages:,}\n")

    # Initialize components
    generator = MarketDataGenerator()
    parser = ITCHMessage()
    oms = OMS(max_position=10000, max_order_value=1_000_000)
    strategy = MeanReversionStrategy(window=20, threshold_pct=0.1) if use_strategy else None

    # Generate market data
    print("[1/4] Generating ITCH market data stream...")
    gen_start = time.time_ns()
    messages = generator.generate_stream(num_messages)
    gen_elapsed = (time.time_ns() - gen_start) / 1_000_000
    print(f"  Generated {len(messages)} messages in {gen_elapsed:.1f}ms")
    print(f"  Active orders at close: {len(generator.active_orders)}")

    # Parse all messages
    print("\n[2/4] Parsing ITCH messages...")
    parse_start = time.time_ns()
    parsed = []
    msg_counts: Dict[str, int] = {}
    for raw in messages:
        result = parser.parse(raw)
        parsed.append(result)
        msg_type = result['type']
        msg_counts[msg_type] = msg_counts.get(msg_type, 0) + 1
    parse_elapsed = (time.time_ns() - parse_start) / 1_000_000

    print(f"  Parsed {len(parsed)} messages in {parse_elapsed:.1f}ms")
    print(f"  Throughput: {len(parsed) / (parse_elapsed / 1000):,.0f} msg/sec")
    print(f"  Message breakdown:")
    for mtype, count in sorted(msg_counts.items()):
        print(f"    {mtype}: {count}")

    # Route through OMS (with optional strategy)
    step = "Strategy → OMS" if strategy else "OMS"
    print(f"\n[3/4] Routing through {step} (risk checks + P&L)...")
    import io, contextlib
    oms_start = time.time_ns()
    orders_submitted = 0
    orders_filled = 0
    orders_rejected = 0

    with contextlib.redirect_stdout(io.StringIO()):
        for msg in parsed:
            price = msg.get('price')
            stock = msg.get('stock')

            if strategy and stock and price:
                # Strategy mode: feed price to strategy, only trade on signals
                signal = strategy.on_market_data(stock, price, msg.get('timestamp_ns', 0))
                if signal:
                    side = Side.BUY if signal.side == 'BUY' else Side.SELL
                    order = oms.submit_order(signal.stock, side, signal.price, signal.quantity)
                    if order:
                        orders_submitted += 1
                        oms.fill_order(order.order_id, signal.quantity, signal.price)
                        orders_filled += 1
                    else:
                        orders_rejected += 1
            elif not strategy:
                # Direct mode: route all ADD_ORDER and TRADE through OMS
                if msg['type'] in ('ADD_ORDER', 'TRADE'):
                    side = Side.BUY if msg['side'] == 'BUY' else Side.SELL
                    order = oms.submit_order(stock, side, price, msg['shares'])
                    if order:
                        orders_submitted += 1
                        oms.fill_order(order.order_id, msg['shares'], price)
                        orders_filled += 1
                    else:
                        orders_rejected += 1

    oms_elapsed = (time.time_ns() - oms_start) / 1_000_000
    print(f"  Submitted: {orders_submitted:,}")
    print(f"  Filled: {orders_filled:,}")
    print(f"  Rejected (risk): {orders_rejected:,}")
    if oms_elapsed > 0:
        print(f"  OMS throughput: {orders_submitted / (oms_elapsed / 1000):,.0f} orders/sec")

    if strategy:
        strategy.print_stats()

    # Final stats
    total_elapsed = gen_elapsed + parse_elapsed + oms_elapsed
    print(f"\n[4/4] Pipeline Summary")
    print(f"  Total time: {total_elapsed:.1f}ms")
    print(f"  End-to-end throughput: {len(messages) / (total_elapsed / 1000):,.0f} msg/sec")

    print(f"\n  === Positions ===")
    for sym, pos in sorted(oms.positions.items()):
        print(f"    {sym}: qty={pos.net_qty:+d}  avg=${pos.avg_price:.2f}  pnl=${pos.realized_pnl:.2f}")

    total_pnl = sum(p.realized_pnl for p in oms.positions.values())
    print(f"\n  Total Realized P&L: ${total_pnl:,.2f}")

    return {
        'messages': len(messages),
        'parse_ms': parse_elapsed,
        'oms_ms': oms_elapsed,
        'total_ms': total_elapsed,
        'orders_submitted': orders_submitted,
        'orders_filled': orders_filled,
        'orders_rejected': orders_rejected,
        'total_pnl': total_pnl,
    }


def main() -> None:
    num = 10000
    use_strategy = False
    for arg in sys.argv[1:]:
        if arg == '--strategy':
            use_strategy = True
        elif arg.isdigit():
            num = int(arg)
    run_pipeline(num_messages=num, use_strategy=use_strategy)


if __name__ == '__main__':
    main()
