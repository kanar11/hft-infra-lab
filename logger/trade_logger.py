#!/usr/bin/env python3
"""
Trade Logger / Audit Trail for HFT Infrastructure Lab

Logs every trade event (order submit, fill, cancel, risk reject) to a
structured audit file. In real HFT firms this is required by regulators
(SEC, MiFID II) — every action must be traceable with nanosecond timestamps.

Logger transakcji / Ścieżka audytu dla HFT Infrastructure Lab

Loguje każde zdarzenie handlowe (zlecenie, realizacja, anulowanie, odrzucenie ryzyka)
do ustrukturyzowanego pliku audytu. W prawdziwych firmach HFT jest to wymagane
przez regulatorów (SEC, MiFID II) — każda akcja musi być śledzona z nanosekundowymi
znacznikami czasu.

WHY THIS EXISTS:
If something goes wrong (flash crash, rogue algorithm, wrong fills), regulators
will ask: "Show me exactly what happened, in order, with timestamps."
This module creates that record.

DLACZEGO TO ISTNIEJE:
Jeśli coś pójdzie nie tak (flash crash, zbuntowany algorytm, złe realizacje),
regulatorzy zapytają: "Pokaż dokładnie co się stało, w kolejności, ze znacznikami czasu."
Ten moduł tworzy taki zapis.

Pipeline integration / Integracja potoku:
  Strategy → Router → Risk → OMS → **Logger** (records everything)
"""
import os
import sys
import time
import json
# csv module: reads/writes CSV files (comma-separated values) — like a simple spreadsheet format
# moduł csv: czyta/zapisuje pliki CSV (wartości rozdzielone przecinkami) — jak prosty format arkusza kalkulacyjnego
import csv
# io.StringIO: creates a "fake file" in memory — we can write to it like a file but it's just a string
# io.StringIO: tworzy "fałszywy plik" w pamięci — możemy pisać do niego jak do pliku, ale to tylko ciąg znaków
from io import StringIO
# threading.Lock: a mutex (mutual exclusion) — only one thread can hold it at a time
# Like a single-key lock on a shared room: if someone is inside, others must wait
# threading.Lock: mutex (wzajemne wykluczanie) — tylko jeden wątek może go trzymać naraz
# Jak zamek na jednym kluczu do wspólnego pokoju: jeśli ktoś jest w środku, inni muszą czekać
from threading import Lock
from typing import Dict, List, Optional, Any
from dataclasses import dataclass, field
from enum import Enum


class EventType(Enum):
    """
    Types of events we log. Each maps to a stage in the order lifecycle.
    Typy zdarzeń, które logujemy. Każdy odpowiada etapowi w cyklu życia zlecenia.

    ORDER_SUBMIT  → strategy sends an order (zlecenie wysłane przez strategię)
    RISK_ACCEPT   → risk manager approved it (menedżer ryzyka zatwierdził)
    RISK_REJECT   → risk manager blocked it (menedżer ryzyka zablokował)
    ORDER_FILL    → exchange filled the order (giełda zrealizowała zlecenie)
    ORDER_PARTIAL → partial fill received (częściowa realizacja)
    ORDER_CANCEL  → order was cancelled (zlecenie anulowane)
    KILL_SWITCH   → emergency stop activated (wyłącznik awaryjny aktywowany)
    SYSTEM_START  → trading session started (sesja handlowa rozpoczęta)
    SYSTEM_STOP   → trading session ended (sesja handlowa zakończona)
    """
    ORDER_SUBMIT  = 'ORDER_SUBMIT'
    RISK_ACCEPT   = 'RISK_ACCEPT'
    RISK_REJECT   = 'RISK_REJECT'
    ORDER_FILL    = 'ORDER_FILL'
    ORDER_PARTIAL = 'ORDER_PARTIAL'
    ORDER_CANCEL  = 'ORDER_CANCEL'
    KILL_SWITCH   = 'KILL_SWITCH'
    SYSTEM_START  = 'SYSTEM_START'
    SYSTEM_STOP   = 'SYSTEM_STOP'


@dataclass
class TradeEvent:
    """
    A single logged event — one row in the audit trail.
    Pojedyncze zalogowane zdarzenie — jeden wiersz w ścieżce audytu.

    Fields / Pola:
        timestamp_ns: nanosecond timestamp from time.time_ns()
                      znacznik czasu w nanosekundach z time.time_ns()
        event_type:   what happened (see EventType enum)
                      co się stało (patrz enum EventType)
        order_id:     unique order identifier
                      unikalny identyfikator zlecenia
        symbol:       stock ticker (e.g., 'AAPL')
                      symbol giełdowy (np. 'AAPL')
        side:         'BUY' or 'SELL'
                      'BUY' (kupno) lub 'SELL' (sprzedaż)
        quantity:     number of shares
                      liczba akcji
        price:        price per share (0.0 if not applicable)
                      cena za akcję (0.0 jeśli nie dotyczy)
        details:      extra info (reject reason, fill venue, etc.)
                      dodatkowe info (powód odrzucenia, miejsce realizacji, itp.)
    """
    timestamp_ns: int
    event_type: EventType
    order_id: int
    symbol: str = ''
    side: str = ''
    quantity: int = 0
    price: float = 0.0
    details: str = ''


class TradeLogger:
    """
    Thread-safe trade event logger with in-memory buffer and file output.
    Bezpieczny wątkowo logger zdarzeń handlowych z buforem w pamięci i zapisem do pliku.

    In real HFT / W prawdziwym HFT:
    - Logs go to shared memory or a lock-free ring buffer (no disk I/O on hot path)
      Logi idą do pamięci współdzielonej lub bezblokadowego bufora pierścieniowego
    - A separate process flushes to disk asynchronously
      Oddzielny proces zrzuca na dysk asynchronicznie
    - Here we use a simple list + mutex for clarity
      Tutaj używamy prostej listy + mutex dla przejrzystości

    Usage / Użycie:
        logger = TradeLogger()
        logger.log(EventType.ORDER_SUBMIT, order_id=1, symbol='AAPL',
                   side='BUY', quantity=100, price=150.25)
        logger.log(EventType.RISK_ACCEPT, order_id=1)
        logger.log(EventType.ORDER_FILL, order_id=1, symbol='AAPL',
                   side='BUY', quantity=100, price=150.25,
                   details='venue=NASDAQ')
        print(logger.get_summary())
    """

    def __init__(self, log_file: Optional[str] = None):
        """
        Initialize the trade logger.
        Inicjalizacja loggera transakcji.

        Args:
            log_file: optional path to write CSV audit file
                      opcjonalna ścieżka do zapisu pliku audytu CSV
                      If None, events are only kept in memory.
                      Jeśli None, zdarzenia są przechowywane tylko w pamięci.
        """
        # _events: list that stores all TradeEvent objects — our in-memory audit trail
        # _events: lista przechowująca wszystkie obiekty TradeEvent — nasza ścieżka audytu w pamięci
        self._events: List[TradeEvent] = []

        # _lock: mutex that prevents two threads from writing simultaneously
        # Like 'flock' in bash — ensures only one process writes to the log at a time
        # _lock: mutex zapobiegający jednoczesnemu zapisowi przez dwa wątki
        # Jak 'flock' w bashu — zapewnia, że tylko jeden proces pisze do logu naraz
        self._lock: Lock = Lock()

        # _log_file: path where we write CSV output (or None for memory-only)
        # _log_file: ścieżka gdzie zapisujemy CSV (lub None dla trybu tylko w pamięci)
        self._log_file: Optional[str] = log_file

        # _sequence: monotonically increasing counter — guarantees event ordering
        # Even if two events have the same timestamp, sequence number tells us which came first
        # _sequence: monotonicznie rosnący licznik — gwarantuje kolejność zdarzeń
        # Nawet jeśli dwa zdarzenia mają ten sam timestamp, numer sekwencji mówi, które było pierwsze
        self._sequence: int = 0

        # _counters: dict tracking how many of each event type we've seen
        # Like 'wc -l' but per event type — useful for summary stats
        # _counters: dict śledzący ile każdego typu zdarzenia widzieliśmy
        # Jak 'wc -l' ale per typ zdarzenia — przydatne do statystyk podsumowujących
        self._counters: Dict[str, int] = {}

        # If log file specified, write CSV header
        # Jeśli podano plik logu, zapisz nagłówek CSV
        if self._log_file:
            with open(self._log_file, 'w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow([
                    'sequence', 'timestamp_ns', 'event_type', 'order_id',
                    'symbol', 'side', 'quantity', 'price', 'details'
                ])

    def log(self, event_type: EventType, order_id: int = 0,
            symbol: str = '', side: str = '', quantity: int = 0,
            price: float = 0.0, details: str = '') -> TradeEvent:
        """
        Record a trade event. Thread-safe via mutex lock.
        Zapisz zdarzenie handlowe. Bezpieczne wątkowo przez blokadę mutex.

        Args:
            event_type: what happened (EventType enum value)
            order_id:   order identifier (0 for system events)
            symbol:     stock ticker
            side:       'BUY' or 'SELL'
            quantity:   number of shares
            price:      price per share
            details:    extra context string

        Returns:
            The created TradeEvent object.
            Utworzony obiekt TradeEvent.
        """
        # time.time_ns(): returns current time in nanoseconds (billionths of a second)
        # In HFT, microsecond/nanosecond precision is critical for event ordering
        # time.time_ns(): zwraca bieżący czas w nanosekundach (miliardowych części sekundy)
        # W HFT precyzja mikro/nanosekundowa jest krytyczna dla kolejności zdarzeń
        timestamp = time.time_ns()

        event = TradeEvent(
            timestamp_ns=timestamp,
            event_type=event_type,
            order_id=order_id,
            symbol=symbol,
            side=side,
            quantity=quantity,
            price=price,
            details=details
        )

        # with self._lock: acquires the mutex — like 'flock /tmp/lockfile' in bash
        # Any other thread calling log() at the same time will block here until we're done
        # with self._lock: przejmuje mutex — jak 'flock /tmp/lockfile' w bashu
        # Każdy inny wątek wywołujący log() w tym samym czasie zablokuje się tutaj aż skończymy
        with self._lock:
            self._sequence += 1
            self._events.append(event)

            # Update counter for this event type
            # Aktualizuj licznik dla tego typu zdarzenia
            key = event_type.value
            self._counters[key] = self._counters.get(key, 0) + 1

            # Append to CSV file if configured
            # Dopisz do pliku CSV jeśli skonfigurowany
            if self._log_file:
                with open(self._log_file, 'a', newline='') as f:
                    writer = csv.writer(f)
                    writer.writerow([
                        self._sequence, timestamp, event_type.value,
                        order_id, symbol, side, quantity, price, details
                    ])

        return event

    def get_events(self, order_id: Optional[int] = None,
                   event_type: Optional[EventType] = None,
                   symbol: Optional[str] = None) -> List[TradeEvent]:
        """
        Query events with optional filters. Like 'grep' for the audit trail.
        Zapytaj o zdarzenia z opcjonalnymi filtrami. Jak 'grep' dla ścieżki audytu.

        Args:
            order_id:   filter by specific order (None = all orders)
            event_type: filter by event type (None = all types)
            symbol:     filter by stock symbol (None = all symbols)

        Returns:
            List of matching TradeEvent objects.
        """
        with self._lock:
            results = self._events[:]

        if order_id is not None:
            results = [e for e in results if e.order_id == order_id]
        if event_type is not None:
            results = [e for e in results if e.event_type == event_type]
        if symbol is not None:
            results = [e for e in results if e.symbol == symbol]

        return results

    def get_order_trail(self, order_id: int) -> List[Dict[str, Any]]:
        """
        Get complete lifecycle of a single order — from submit to fill/cancel.
        Pobierz pełny cykl życia jednego zlecenia — od wysłania do realizacji/anulowania.

        This is what regulators ask for: "Show me everything that happened to order #1234."
        To jest to, o co pytają regulatorzy: "Pokaż mi wszystko, co stało się ze zleceniem #1234."

        Returns:
            List of dicts, each with timestamp, event, and details.
        """
        events = self.get_events(order_id=order_id)
        trail = []
        for e in events:
            trail.append({
                'timestamp_ns': e.timestamp_ns,
                'event': e.event_type.value,
                'symbol': e.symbol,
                'side': e.side,
                'quantity': e.quantity,
                'price': e.price,
                'details': e.details
            })
        return trail

    def get_summary(self) -> Dict[str, Any]:
        """
        Summary statistics of all logged events.
        Statystyki podsumowujące wszystkich zalogowanych zdarzeń.

        Returns dict with:
            total_events:  how many events total
            counters:      count per event type
            unique_orders: how many different orders
            unique_symbols: how many different stocks
            time_span_ms:  time from first to last event in milliseconds
        """
        with self._lock:
            if not self._events:
                return {
                    'total_events': 0,
                    'counters': {},
                    'unique_orders': 0,
                    'unique_symbols': 0,
                    'time_span_ms': 0.0
                }

            events = self._events[:]
            counters = dict(self._counters)

        order_ids = set(e.order_id for e in events if e.order_id > 0)
        symbols = set(e.symbol for e in events if e.symbol)
        time_span_ns = events[-1].timestamp_ns - events[0].timestamp_ns
        time_span_ms = time_span_ns / 1_000_000

        return {
            'total_events': len(events),
            'counters': counters,
            'unique_orders': len(order_ids),
            'unique_symbols': len(symbols),
            'time_span_ms': round(time_span_ms, 3)
        }

    def get_log_speed_ns(self) -> int:
        """
        Benchmark: how fast can we log one event? Returns time in nanoseconds.
        Benchmark: jak szybko możemy zalogować jedno zdarzenie? Zwraca czas w nanosekundach.
        """
        start = time.time_ns()
        self.log(EventType.ORDER_SUBMIT, order_id=0, symbol='BENCH',
                 side='BUY', quantity=1, price=1.0, details='benchmark')
        end = time.time_ns()

        # Remove the benchmark event so it doesn't pollute real data
        # Usuń zdarzenie benchmarkowe żeby nie zanieczyszczało prawdziwych danych
        with self._lock:
            if self._events and self._events[-1].details == 'benchmark':
                self._events.pop()
                self._sequence -= 1
                self._counters['ORDER_SUBMIT'] = max(0, self._counters.get('ORDER_SUBMIT', 1) - 1)

        return end - start


# === Demo / Demonstracja ===
def demo():
    """
    Simulate a trading session with full audit trail.
    Symuluj sesję handlową z pełną ścieżką audytu.
    """
    print("=== Trade Logger Demo ===\n")

    logger = TradeLogger()

    # System start
    logger.log(EventType.SYSTEM_START, details='session_open')

    # Order 1: successful buy
    logger.log(EventType.ORDER_SUBMIT, order_id=1, symbol='AAPL',
               side='BUY', quantity=100, price=150.25)
    logger.log(EventType.RISK_ACCEPT, order_id=1, symbol='AAPL',
               details='all_checks_passed')
    logger.log(EventType.ORDER_FILL, order_id=1, symbol='AAPL',
               side='BUY', quantity=100, price=150.25,
               details='venue=NASDAQ latency_us=45')

    # Order 2: risk rejected
    logger.log(EventType.ORDER_SUBMIT, order_id=2, symbol='TSLA',
               side='BUY', quantity=5000, price=250.00)
    logger.log(EventType.RISK_REJECT, order_id=2, symbol='TSLA',
               details='order_value $1,250,000 > limit $1,000,000')

    # Order 3: partial fill then cancel
    logger.log(EventType.ORDER_SUBMIT, order_id=3, symbol='MSFT',
               side='SELL', quantity=200, price=380.50)
    logger.log(EventType.RISK_ACCEPT, order_id=3, symbol='MSFT',
               details='all_checks_passed')
    logger.log(EventType.ORDER_PARTIAL, order_id=3, symbol='MSFT',
               side='SELL', quantity=150, price=380.50,
               details='venue=NYSE filled=150/200')
    logger.log(EventType.ORDER_CANCEL, order_id=3, symbol='MSFT',
               details='remaining 50 shares cancelled by strategy')

    # System stop
    logger.log(EventType.SYSTEM_STOP, details='session_close')

    # Print audit trail for order 1
    print("--- Order #1 Audit Trail ---")
    trail = logger.get_order_trail(1)
    for step in trail:
        print(f"  {step['event']:15s}  {step['symbol']}  {step['side']}  "
              f"qty={step['quantity']}  px={step['price']}  {step['details']}")

    # Print audit trail for order 2 (rejected)
    print("\n--- Order #2 Audit Trail (rejected) ---")
    trail = logger.get_order_trail(2)
    for step in trail:
        print(f"  {step['event']:15s}  {step['symbol']}  {step['side']}  "
              f"qty={step['quantity']}  px={step['price']}  {step['details']}")

    # Print summary
    print("\n--- Session Summary ---")
    summary = logger.get_summary()
    print(f"  Total events:   {summary['total_events']}")
    print(f"  Unique orders:  {summary['unique_orders']}")
    print(f"  Unique symbols: {summary['unique_symbols']}")
    print(f"  Time span:      {summary['time_span_ms']} ms")
    print(f"  Event counts:")
    for event_name, count in sorted(summary['counters'].items()):
        print(f"    {event_name:20s} {count}")

    # Benchmark
    speed = logger.get_log_speed_ns()
    print(f"\n  Log speed: {speed} ns/event")


if __name__ == '__main__':
    demo()
