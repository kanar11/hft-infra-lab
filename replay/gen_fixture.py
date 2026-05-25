#!/usr/bin/env python3
"""
gen_fixture.py — generuje syntetyczny LOBSTER message CSV.

LOBSTER ma free sample days ale ich pobranie wymaga rejestracji i są
bardzo małe. Ten skrypt produkuje większą deterministyczną fixture
(domyślnie 10000 eventów) z realistycznymi rozkładami — coś między
"sample 20 eventów" a "prawdziwy dzień AAPL 500k+ eventów".

Format LOBSTER (CSV bez nagłówka):
    timestamp,event_type,order_id,size,price,direction

    timestamp   — sekundy od 09:30:00 ET (float, 9 cyfr precyzji)
    event_type  — 1=submit, 2=partial_cancel, 3=full_delete, 4=execute_visible
                  5=hidden_exec, 6=cross_trade, 7=trading_halt
    order_id    — 64-bit ID giełdowe (LOBSTER → wewnętrzny OMS mapping)
    size        — ilość akcji
    price       — cena × 10000 (np. 2238100 = $223.81)
    direction   — +1 (BUY) lub -1 (SELL)

Rozkłady (heurystyki dobrane żeby księga była stabilna):
    - inter-arrival: wykładniczy, średnio ~10 ms (Poisson process)
    - event mix: 50% submit, 20% partial cancel, 15% full delete,
                 14% execute, 1% halt
    - rozmiary: dyskretne {10, 25, 50, 100, 200, 500, 1000} z preferencją mniejszych
    - ceny: random walk wokół base_price ($223.81), ±$0.20 spread,
            sporadyczne skoki o $0.05 (1 na 1000 eventów)

Użycie:
    python3 replay/gen_fixture.py                    # default: 10000 eventów, AAPL
    python3 replay/gen_fixture.py -n 50000           # 50k eventów
    python3 replay/gen_fixture.py -n 100000 -s MSFT  # 100k MSFT
    python3 replay/gen_fixture.py -o my_day.csv      # custom output

Deterministyczny przy stałym seed (default=42) — CI dostaje zawsze ten sam
plik, dyf nie eksploduje.
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

# Rozkład prawdopodobieństw eventów (suma = 1.0).
EVENT_WEIGHTS = [
    (EVENT_SUBMIT,          0.50),
    (EVENT_CANCEL_PARTIAL,  0.20),
    (EVENT_DELETE,          0.15),
    (EVENT_EXECUTE_VISIBLE, 0.14),
    (EVENT_HALT,            0.01),
]

# Rozmiary zleceń — preferujemy małe (round-lot 100 to median real-world).
SIZE_CHOICES  = [10, 25, 50, 100, 200, 500, 1000]
SIZE_WEIGHTS  = [10,  8,  5,  20,  10,   5,    2]  # mniejsze częstsze niż większe


def weighted_choice(rng: random.Random, items: List[Tuple[int, float]]) -> int:
    """Wybór ważony — dla listy (value, weight). weight nie musi sumować się do 1."""
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
    """Generuje listę linii CSV. start_seconds = 34200.0 (9:30:00 ET).

    Trzyma aktywne order_id w buforze "active_book" — eventy 2/3/4 wskazują
    na losowo wybrane aktywne zlecenie (inaczej kasujemy nieistniejące, parser
    by się tym nie zmartwił ale demo byłoby mniej realistyczne).
    """
    rng = random.Random(seed)
    base_price_ticks = int(round(base_price_dollars * 10000))  # $223.81 → 2238100

    lines: List[str] = []
    active_book: List[Tuple[int, int, int]] = []  # (order_id, size, direction)
    next_order_id  = 11885113   # podobne do bundled sample_aapl.csv
    next_timestamp = start_seconds
    current_mid_ticks = base_price_ticks

    for _ in range(num_events):
        # Inter-arrival — wykładniczy, średnio 10 ms.
        next_timestamp += rng.expovariate(100.0)  # lambda=100/s → mean 10ms

        # Rzadkie skoki ceny (jitter mid-price'a).
        if rng.random() < 0.001:
            current_mid_ticks += rng.choice([-500, -200, 200, 500])  # ±$0.02-0.05

        event = weighted_choice(rng,
            [(EVENT_SUBMIT, 0.50), (EVENT_CANCEL_PARTIAL, 0.20),
             (EVENT_DELETE, 0.15), (EVENT_EXECUTE_VISIBLE, 0.14),
             (EVENT_HALT, 0.01)])

        if event == EVENT_SUBMIT or not active_book:
            # SUBMIT — nowe zlecenie z ceną wokół mida ±20 ticków ($0.20 spread).
            direction = rng.choice([1, -1])
            offset    = rng.randint(1, 20)
            # BUY poniżej mida (chcemy kupić tanio), SELL powyżej.
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
            # Trading halt — order_id i price są ignorowane przez parser.
            lines.append(f"{next_timestamp:.9f},{EVENT_HALT},0,0,0,0")

        else:
            # CANCEL_PARTIAL / DELETE / EXECUTE — wybieramy losowe aktywne zlecenie.
            idx = rng.randrange(len(active_book))
            order_id, size, direction = active_book[idx]

            if event == EVENT_EXECUTE_VISIBLE:
                exec_size = min(size, weighted_choice(rng,
                    [(10, 5), (25, 3), (50, 2), (100, 1)]))
                # Cena egzekucji = pierwotna cena (LOBSTER raportuje tak).
                # Dla uproszczenia wpisujemy current_mid_ticks ± mały noise.
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
                # Cena w cancel/delete to oryginalna cena zlecenia.
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
    ap = argparse.ArgumentParser(description="Generuj LOBSTER fixture CSV.")
    ap.add_argument("-n", "--num-events", type=int, default=10000,
                    help="liczba eventów (default: 10000)")
    ap.add_argument("-s", "--symbol", default="AAPL",
                    help="ticker — używany TYLKO w nazwie pliku domyślnego (default: AAPL)")
    ap.add_argument("-p", "--base-price", type=float, default=223.81,
                    help="bazowa cena w dolarach (default: 223.81)")
    ap.add_argument("--seed", type=int, default=42,
                    help="seed RNG dla deterministycznego wyniku (default: 42)")
    ap.add_argument("-o", "--output", default=None,
                    help="plik wyjściowy (default: replay/generated_<SYM>_day.csv)")
    args = ap.parse_args()

    output_path = args.output or f"replay/generated_{args.symbol.lower()}_day.csv"

    print(f"Generuję {args.num_events} eventów dla {args.symbol} "
          f"(base=${args.base_price:.2f}, seed={args.seed})...", file=sys.stderr)

    lines = generate(args.num_events, args.symbol, args.base_price,
                     args.seed, start_seconds=34200.0)

    with open(output_path, "w", encoding="utf-8") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Zapisano {len(lines)} linii do {output_path}", file=sys.stderr)
    print(f"Uruchom:  ./replay/lobster_demo {output_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
