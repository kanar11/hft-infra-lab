#!/usr/bin/env python3
"""
gen_fixture.py — generates a synthetic LOBSTER message CSV.

LOBSTER has free sample days but downloading them requires registration and they're
very small. This script produces a larger deterministic fixture
(10000 events by default) with realistic distributions — something between
a "20-event sample" and a "real AAPL day with 500k+ events".

LOBSTER format (headerless CSV):
    timestamp,event_type,order_id,size,price,direction

    timestamp   — seconds since 09:30:00 ET (float, 9 digits of precision)
    event_type  — 1=submit, 2=partial_cancel, 3=full_delete, 4=execute_visible
                  5=hidden_exec, 6=cross_trade, 7=trading_halt
    order_id    — 64-bit exchange ID (LOBSTER → internal OMS mapping)
    size        — number of shares
    price       — price × 10000 (e.g. 2238100 = $223.81)
    direction   — +1 (BUY) or -1 (SELL)

Distributions (heuristics chosen to keep the book stable):
    - inter-arrival: exponential, ~10 ms on average (Poisson process)
    - event mix: 50% submit, 20% partial cancel, 15% full delete,
                 14% execute, 1% halt
    - sizes: discrete {10, 25, 50, 100, 200, 500, 1000} favoring smaller ones
    - prices: random walk around base_price ($223.81), ±$0.20 spread,
            occasional $0.05 jumps (1 in 1000 events)

Usage:
    python3 replay/gen_fixture.py                    # default: 10000 events, AAPL
    python3 replay/gen_fixture.py -n 50000           # 50k events
    python3 replay/gen_fixture.py -n 100000 -s MSFT  # 100k MSFT
    python3 replay/gen_fixture.py -o my_day.csv      # custom output

Deterministic with a fixed seed (default=42) — CI always gets the same
file, the diff doesn't explode.
"""

import argparse
import random
import sys
from typing import List, Tuple


EVENT_SUBMIT          = 1
EVENT_CANCEL_PARTIAL  = 2
EVENT_DELETE          = 3
EVENT_EXECUTE_VISIBLE = 4
EVENT_HALT            = 7

# Event probability distribution (sums to 1.0).
EVENT_WEIGHTS = [
    (EVENT_SUBMIT,          0.50),
    (EVENT_CANCEL_PARTIAL,  0.20),
    (EVENT_DELETE,          0.15),
    (EVENT_EXECUTE_VISIBLE, 0.14),
    (EVENT_HALT,            0.01),
]

# Order sizes — we prefer small (round-lot 100 is the real-world median).
SIZE_CHOICES  = [10, 25, 50, 100, 200, 500, 1000]
SIZE_WEIGHTS  = [10,  8,  5,  20,  10,   5,    2]  # smaller more frequent than larger


def weighted_choice(rng: random.Random, items: List[Tuple[int, float]]) -> int:
    """Weighted choice — for a list of (value, weight). weight need not sum to 1."""
    total = sum(w for _, w in items)
    r = rng.random() * total
    acc = 0.0
    for value, weight in items:
        acc += weight
        if r < acc:
            return value
    return items[-1][0]


def generate(num_events: int, symbol: str, base_price_dollars: float,
             seed: int, start_seconds: float) -> List[str]:
    """Generates a list of CSV lines. start_seconds = 34200.0 (9:30:00 ET).

    Keeps active order_ids in an "active_book" buffer — events 2/3/4 point
    at a randomly chosen active order (otherwise we'd cancel non-existent ones, the parser
    wouldn't mind but the demo would be less realistic).
    """
    rng = random.Random(seed)
    base_price_ticks = int(round(base_price_dollars * 10000))  # $223.81 → 2238100

    lines: List[str] = []
    active_book: List[Tuple[int, int, int]] = []  # (order_id, size, direction)
    next_order_id  = 11885113   # similar to the bundled sample_aapl.csv
    next_timestamp = start_seconds
    current_mid_ticks = base_price_ticks

    for _ in range(num_events):
        # Inter-arrival — exponential, 10 ms on average.
        next_timestamp += rng.expovariate(100.0)  # lambda=100/s → mean 10ms

        # Rare price jumps (mid-price jitter).
        if rng.random() < 0.001:
            current_mid_ticks += rng.choice([-500, -200, 200, 500])  # ±$0.02-0.05

        event = weighted_choice(rng,
            [(EVENT_SUBMIT, 0.50), (EVENT_CANCEL_PARTIAL, 0.20),
             (EVENT_DELETE, 0.15), (EVENT_EXECUTE_VISIBLE, 0.14),
             (EVENT_HALT, 0.01)])

        if event == EVENT_SUBMIT or not active_book:
            # SUBMIT — a new order priced around the mid ±20 ticks ($0.20 spread).
            direction = rng.choice([1, -1])
            offset    = rng.randint(1, 20)
            # BUY below the mid (we want to buy cheap), SELL above.
            price_ticks = current_mid_ticks - offset if direction == 1 \
                          else current_mid_ticks + offset
            size = weighted_choice(rng,
                [(s, w) for s, w in zip(SIZE_CHOICES, SIZE_WEIGHTS)])
            order_id = next_order_id
            next_order_id += 1

            lines.append(f"{next_timestamp:.9f},{EVENT_SUBMIT},{order_id},"
                         f"{size},{price_ticks},{direction}")
            active_book.append((order_id, size, direction))

        elif event == EVENT_HALT:
            # Trading halt — order_id and price are ignored by the parser.
            lines.append(f"{next_timestamp:.9f},{EVENT_HALT},0,0,0,0")

        else:
            # CANCEL_PARTIAL / DELETE / EXECUTE — we pick a random active order.
            idx = rng.randrange(len(active_book))
            order_id, size, direction = active_book[idx]

            if event == EVENT_EXECUTE_VISIBLE:
                exec_size = min(size, weighted_choice(rng,
                    [(10, 5), (25, 3), (50, 2), (100, 1)]))
                # Execution price = the original price (LOBSTER reports it that way).
                # For simplicity we write current_mid_ticks ± a small noise.
                exec_price = current_mid_ticks + rng.randint(-5, 5)
                lines.append(f"{next_timestamp:.9f},{EVENT_EXECUTE_VISIBLE},"
                             f"{order_id},{exec_size},{exec_price},{direction}")
                remaining = size - exec_size
                if remaining <= 0:
                    active_book.pop(idx)
                else:
                    active_book[idx] = (order_id, remaining, direction)

            elif event == EVENT_CANCEL_PARTIAL:
                cancel_size = max(1, size // 2)
                # The price in cancel/delete is the order's original price.
                lines.append(f"{next_timestamp:.9f},{EVENT_CANCEL_PARTIAL},"
                             f"{order_id},{cancel_size},{current_mid_ticks},{direction}")
                remaining = size - cancel_size
                if remaining <= 0:
                    active_book.pop(idx)
                else:
                    active_book[idx] = (order_id, remaining, direction)

            else:  # EVENT_DELETE
                lines.append(f"{next_timestamp:.9f},{EVENT_DELETE},"
                             f"{order_id},{size},{current_mid_ticks},{direction}")
                active_book.pop(idx)

    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate a LOBSTER fixture CSV.")
    ap.add_argument("-n", "--num-events", type=int, default=10000,
                    help="number of events (default: 10000)")
    ap.add_argument("-s", "--symbol", default="AAPL",
                    help="ticker — used ONLY in the default file name (default: AAPL)")
    ap.add_argument("-p", "--base-price", type=float, default=223.81,
                    help="base price in dollars (default: 223.81)")
    ap.add_argument("--seed", type=int, default=42,
                    help="RNG seed for a deterministic result (default: 42)")
    ap.add_argument("-o", "--output", default=None,
                    help="output file (default: replay/generated_<SYM>_day.csv)")
    args = ap.parse_args()

    output_path = args.output or f"replay/generated_{args.symbol.lower()}_day.csv"

    print(f"Generating {args.num_events} events for {args.symbol} "
          f"(base=${args.base_price:.2f}, seed={args.seed})...", file=sys.stderr)

    lines = generate(args.num_events, args.symbol, args.base_price,
                     args.seed, start_seconds=34200.0)

    with open(output_path, "w", encoding="utf-8") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Wrote {len(lines)} lines to {output_path}", file=sys.stderr)
    print(f"Run:  ./replay/lobster_demo {output_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
