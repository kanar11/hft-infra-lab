#!/usr/bin/env python3
"""
HFT Market Data Simulator — End-to-End Pipeline Demo

Generates synthetic ITCH market data, parses it through the ITCH parser,
routes orders through the OMS with risk checks, and tracks P&L.

Pipeline: ITCH Generator → ITCH Parser → Strategy (optional) → Router (optional) → OMS (risk checks) → Fill Engine → P&L
Potok: Generator ITCH → Parser ITCH → Strategia (opcjonalnie) → Router (opcjonalnie) → OMS (sprawdzenia ryzyka) → Silnik Realizacji → P&L

This proves all modules work together as a complete trading system.
Use --strategy flag to enable mean reversion strategy.
Use --router flag to enable smart order routing across venues.
"""
import os
import sys
import time
import struct
import random
import logging
from typing import List, Dict, Any

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import importlib.util

# itch-parser has a hyphen — can't use normal import
_spec = importlib.util.spec_from_file_location(
    "itch_parser",
    os.path.join(os.path.dirname(__file__), '..', 'itch-parser', 'itch_parser.py')
)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
ITCHMessage = _mod.ITCHMessage

from oms.oms import OMS, Side
from strategy.mean_reversion import MeanReversionStrategy
from router.smart_router import SmartOrderRouter, RoutingStrategy, Venue

logger = logging.getLogger('simulator')
cfg = None  # Will be initialized in main()


# --- Market Data Generator ---
# --- Generator Danych Rynkowych ---

class MarketDataGenerator:
    """Generates realistic ITCH 5.0 binary messages for simulation.
    Generuje realistyczne binarne wiadomości ITCH 5.0 do symulacji."""

    STOCKS = ['AAPL', 'MSFT', 'GOOGL', 'AMZN', 'TSLA', 'META', 'NVDA', 'JPM']
    BASE_PRICES = {
        'AAPL': 175.00, 'MSFT': 410.00, 'GOOGL': 155.00, 'AMZN': 185.00,
        'TSLA': 245.00, 'META': 500.00, 'NVDA': 880.00, 'JPM': 195.00,
    }

    def __init__(self, seed: int = None) -> None:
        if seed is None:
            seed = 42
        self.rng = random.Random(seed)
        self.seq = 0
        self.order_ref = 1000
        self.active_orders: Dict[int, Dict[str, Any]] = {}

    def _next_ts(self) -> int:
        """Generate incrementing nanosecond timestamp.
        Generuj inkrementacyjny znacznik czasu w nanosekundach."""
        self.seq += 1
        return 34200_000_000_000 + self.seq * 1_000_000  # 9:30 AM + seq ms

    def generate_add_order(self) -> bytes:
        """Generate a random Add Order (A) message.
        Generuj losową wiadomość Dodaj Zlecenie (A)."""
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
        """Generate Order Executed (E) message for a random active order.
        Generuj wiadomość Zlecenie Wykonane (E) dla losowego aktywnego zlecenia."""
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
        """Generate Order Cancelled (C) message.
        Generuj wiadomość Zlecenie Anulowane (C)."""
        if not self.active_orders:
            return self.generate_add_order()
        ref = self.rng.choice(list(self.active_orders.keys()))
        order = self.active_orders.pop(ref)

        return struct.pack('!c q q I',
            b'C', self._next_ts(), ref, order['shares'])

    def generate_trade(self) -> bytes:
        """Generate Trade (P) message.
        Generuj wiadomość Handel (P)."""
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
        """Generate System Event (S) message.
        Generuj wiadomość Zdarzenie Systemowe (S)."""
        return struct.pack('!c q c', b'S', self._next_ts(), event)

    def generate_stream(self, num_messages: int = 1000) -> List[bytes]:
        """Generate a realistic stream of mixed ITCH messages.
        Generuj realistyczny strumień mieszanych wiadomości ITCH."""
        messages = []

        # Start of day
        # Początek dnia
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
        # Koniec dnia
        messages.append(self.generate_system_event(b'M'))  # end of market hours
        messages.append(self.generate_system_event(b'E'))  # end of system hours
        messages.append(self.generate_system_event(b'C'))  # end of messages

        return messages


# --- Pipeline Runner ---
# --- Uruchamiacz Potoku ---

def run_pipeline(num_messages: int = 1000, use_strategy: bool = False,
                  use_router: bool = False) -> Dict[str, Any]:
    """Run full pipeline: generate → parse → (strategy) → (router) → OMS → stats.
    Uruchom pełny potok: generuj → przeanalizuj → (strategia) → (router) → OMS → statystyki.

    Args:
        num_messages: Number of market data messages to simulate
        use_strategy: Enable mean reversion strategy for signal generation
        use_router: Enable smart order routing across multiple venues
    Returns:
        Dict with pipeline statistics
    """
    parts = []
    if use_strategy:
        parts.append('Strategy')
    if use_router:
        parts.append('Router')
    mode = ' + '.join(parts) if parts else 'Direct Mode (all orders)'
    pipeline_str = ' → '.join(filter(None, [
        'ITCH Generator', 'Parser',
        'Strategy' if use_strategy else None,
        'Router' if use_router else None,
        'OMS', 'P&L'
    ]))
    logger.info(f"=== HFT Market Data Simulator ===")
    logger.info(f"Pipeline: {pipeline_str}")
    logger.info(f"Mode: {mode}")
    logger.info(f"Messages: {num_messages:,}\n")

    # Initialize components
    # Zainicjalizuj komponenty
    generator = MarketDataGenerator(seed=cfg.get('simulator', {}).get('seed', 42))
    parser = ITCHMessage()
    oms = OMS(max_position=cfg['oms']['max_position'], max_order_value=cfg['oms']['max_order_value'])
    strategy = MeanReversionStrategy(window=cfg['strategy']['window'], threshold_pct=cfg['strategy']['threshold_pct']) if use_strategy else None
    router = None
    if use_router:
        router = SmartOrderRouter(strategy=RoutingStrategy.BEST_PRICE, split_threshold=500)
        for venue_cfg in cfg['router']['venues']:
            router.add_venue(Venue(name=venue_cfg['name'], latency_ns=venue_cfg['latency_ns'], fee_per_share=venue_cfg['fee_per_share']))

    # Generate market data
    # Generuj dane rynkowe
    logger.info("[1/4] Generating ITCH market data stream...")
    gen_start = time.time_ns()
    messages = generator.generate_stream(num_messages)
    gen_elapsed = (time.time_ns() - gen_start) / 1_000_000
    logger.info(f"  Generated {len(messages)} messages in {gen_elapsed:.1f}ms")
    logger.info(f"  Active orders at close: {len(generator.active_orders)}")

    # Parse all messages
    # Przeanalizuj wszystkie wiadomości
    logger.info("\n[2/4] Parsing ITCH messages...")
    parse_start = time.time_ns()
    parsed = []
    msg_counts: Dict[str, int] = {}
    for raw in messages:
        result = parser.parse(raw)
        parsed.append(result)
        msg_type = result['type']
        msg_counts[msg_type] = msg_counts.get(msg_type, 0) + 1
    parse_elapsed = (time.time_ns() - parse_start) / 1_000_000

    logger.info(f"  Parsed {len(parsed)} messages in {parse_elapsed:.1f}ms")
    logger.info(f"  Throughput: {len(parsed) / (parse_elapsed / 1000):,.0f} msg/sec")
    logger.info(f"  Message breakdown:")
    for mtype, count in sorted(msg_counts.items()):
        logger.info(f"    {mtype}: {count}")

    # Route through OMS (with optional strategy and router)
    # Trasuj przez OMS (z opcjonalną strategią i routerem)
    steps = ' → '.join(filter(None, [
        'Strategy' if strategy else None,
        'Router' if router else None,
        'OMS'
    ]))
    logger.info(f"\n[3/4] Routing through {steps} (risk checks + P&L)...")
    import io, contextlib
    oms_start = time.time_ns()
    orders_submitted = 0
    orders_filled = 0
    orders_rejected = 0

    with contextlib.redirect_stdout(io.StringIO()):
        for msg in parsed:
            price = msg.get('price')
            stock = msg.get('stock')

            # Update router quotes from market data
            # Zaktualizuj notowania routera z danych rynkowych
            if router and stock and price:
                spread = price * 0.0002  # 0.02% spread
                for venue_name in ['NYSE', 'NASDAQ', 'BATS']:
                    router.update_quote(venue_name,
                                        round(price - spread, 2), round(price + spread, 2),
                                        random.randint(100, 500), random.randint(100, 500))

            if strategy and stock and price:
                # Strategy mode: feed price to strategy, only trade on signals
                # Tryb strategii: podaj cenę strategii, handluj tylko na sygnałach
                signal = strategy.on_market_data(stock, price, msg.get('timestamp_ns', 0))
                if signal:
                    side_str = signal.side
                    fill_price = signal.price
                    # Route through SOR if enabled
                    if router:
                        route = router.route_order(side_str, signal.quantity)
                        if route:
                            fill_price = route.price
                    side = Side.BUY if side_str == 'BUY' else Side.SELL
                    order = oms.submit_order(signal.stock, side, fill_price, signal.quantity)
                    if order:
                        orders_submitted += 1
                        oms.fill_order(order.order_id, signal.quantity, fill_price)
                        orders_filled += 1
                    else:
                        orders_rejected += 1
            elif not strategy:
                # Direct mode: route all ADD_ORDER and TRADE through OMS
                # Tryb bezpośredni: trasuj wszystkie ADD_ORDER i TRADE przez OMS
                if msg['type'] in ('ADD_ORDER', 'TRADE'):
                    side_str = msg['side']
                    fill_price = price
                    if router:
                        route = router.route_order(side_str, msg['shares'])
                        if route:
                            fill_price = route.price
                    side = Side.BUY if side_str == 'BUY' else Side.SELL
                    order = oms.submit_order(stock, side, fill_price, msg['shares'])
                    if order:
                        orders_submitted += 1
                        oms.fill_order(order.order_id, msg['shares'], fill_price)
                        orders_filled += 1
                    else:
                        orders_rejected += 1

    oms_elapsed = (time.time_ns() - oms_start) / 1_000_000
    logger.info(f"  Submitted: {orders_submitted:,}")
    logger.info(f"  Filled: {orders_filled:,}")
    logger.info(f"  Rejected (risk): {orders_rejected:,}")
    if oms_elapsed > 0:
        logger.info(f"  OMS throughput: {orders_submitted / (oms_elapsed / 1000):,.0f} orders/sec")

    if strategy:
        strategy.print_stats()
    if router:
        router.print_stats()

    # Final stats
    # Statystyki końcowe
    total_elapsed = gen_elapsed + parse_elapsed + oms_elapsed
    logger.info(f"\n[4/4] Pipeline Summary")
    logger.info(f"  Total time: {total_elapsed:.1f}ms")
    logger.info(f"  End-to-end throughput: {len(messages) / (total_elapsed / 1000):,.0f} msg/sec")

    logger.info(f"\n  === Positions ===")
    for sym, pos in sorted(oms.positions.items()):
        logger.info(f"    {sym}: qty={pos.net_qty:+d}  avg=${pos.avg_price:.2f}  pnl=${pos.realized_pnl:.2f}")

    total_pnl = sum(p.realized_pnl for p in oms.positions.values())
    logger.info(f"\n  Total Realized P&L: ${total_pnl:,.2f}")

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
    global cfg
    from config_loader import load_config, setup_logging
    cfg = load_config()
    setup_logging()

    num = cfg['simulator'].get('num_messages', 10000)
    use_strategy = False
    use_router = False
    for arg in sys.argv[1:]:
        if arg == '--strategy':
            use_strategy = True
        elif arg == '--router':
            use_router = True
        elif arg.isdigit():
            num = int(arg)
    run_pipeline(num_messages=num, use_strategy=use_strategy, use_router=use_router)


if __name__ == '__main__':
    main()
