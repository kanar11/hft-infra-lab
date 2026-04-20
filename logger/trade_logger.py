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
from threading import Lock, Thread
from typing import Dict, List, Optional, Any, Tuple
from dataclasses import dataclass, field, asdict
from enum import Enum
# collections.deque: double-ended queue — O(1) append and popleft, perfect for ring buffers
# collections.deque: kolejka dwustronna — O(1) append i popleft, idealna do buforów pierścieniowych
from collections import deque
# queue.Queue: thread-safe FIFO queue — used to decouple hot path from disk I/O
# queue.Queue: bezpieczna wątkowo kolejka FIFO — oddziela gorącą ścieżkę od I/O dyskowego
from queue import Queue, Empty
import statistics
import atexit


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

    def __init__(self, log_file: Optional[str] = None, max_events: int = 0):
        """
        Initialize the trade logger.
        Inicjalizacja loggera transakcji.

        Args:
            log_file:   optional path to write CSV audit file
                        opcjonalna ścieżka do zapisu pliku audytu CSV
                        If None, events are only kept in memory.
                        Jeśli None, zdarzenia są przechowywane tylko w pamięci.
            max_events: maximum number of events to keep in memory (0 = unlimited).
                        When the buffer is full, oldest events are dropped (ring buffer).
                        maksymalna liczba zdarzeń w pamięci (0 = bez limitu).
                        Gdy bufor jest pełny, najstarsze zdarzenia są usuwane (bufor pierścieniowy).
        """
        # _max_events: ring buffer capacity (0 means unlimited, use plain list)
        # _max_events: pojemność bufora pierścieniowego (0 = bez limitu, użyj zwykłej listy)
        self._max_events: int = max_events

        # _events: stores all TradeEvent objects — our in-memory audit trail
        # When max_events > 0, uses deque with maxlen for automatic ring buffer behavior
        # _events: przechowuje wszystkie obiekty TradeEvent — nasza ścieżka audytu w pamięci
        # Gdy max_events > 0, używa deque z maxlen dla automatycznego zachowania bufora pierścieniowego
        if max_events > 0:
            self._events: deque = deque(maxlen=max_events)
        else:
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

        # _total_logged: total events ever logged (doesn't decrease when ring buffer wraps)
        # _total_logged: łączna liczba zalogowanych zdarzeń (nie maleje gdy bufor pierścieniowy się zawija)
        self._total_logged: int = 0

        # _counters: dict tracking how many of each event type we've seen
        # Like 'wc -l' but per event type — useful for summary stats
        # _counters: dict śledzący ile każdego typu zdarzenia widzieliśmy
        # Jak 'wc -l' ale per typ zdarzenia — przydatne do statystyk podsumowujących
        self._counters: Dict[str, int] = {}

        # _write_queue: thread-safe queue for async file writes
        # The hot path (log()) just puts a row on the queue — no disk I/O under the lock
        # _write_queue: bezpieczna wątkowo kolejka do asynchronicznych zapisów do pliku
        # Gorąca ścieżka (log()) tylko wstawia wiersz do kolejki — bez I/O dyskowego pod lockiem
        self._write_queue: Optional[Queue] = None
        self._flush_thread: Optional[Thread] = None

        # If log file specified, write CSV header and start async flush thread
        # Jeśli podano plik logu, zapisz nagłówek CSV i uruchom wątek asynchronicznego zrzutu
        if self._log_file:
            with open(self._log_file, 'w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow([
                    'sequence', 'timestamp_ns', 'event_type', 'order_id',
                    'symbol', 'side', 'quantity', 'price', 'details'
                ])
            self._write_queue = Queue()
            self._flush_thread = Thread(target=self._flush_worker, daemon=True,
                                        name='logger-flush')
            self._flush_thread.start()
            # Register cleanup so pending writes are flushed on exit
            atexit.register(self.flush)

    def _flush_worker(self) -> None:
        """
        Background thread that drains the write queue and appends rows to CSV.
        Runs as a daemon thread — exits when the main process ends.
        This keeps disk I/O completely off the hot path.

        Wątek w tle który opróżnia kolejkę zapisu i dopisuje wiersze do CSV.
        Działa jako wątek daemon — kończy się gdy główny proces się kończy.
        Dzięki temu I/O dyskowe jest całkowicie poza gorącą ścieżką.
        """
        batch = []
        while True:
            try:
                # Block up to 50ms waiting for first item, then drain greedily
                # Czekaj do 50ms na pierwszy element, potem opróżniaj zachłannie
                row = self._write_queue.get(timeout=0.05)
                batch.append(row)
                count = 1
                # Drain everything currently in the queue (non-blocking)
                while True:
                    try:
                        batch.append(self._write_queue.get_nowait())
                        count += 1
                    except Empty:
                        break
                # Write entire batch in one open() call
                with open(self._log_file, 'a', newline='') as f:
                    writer = csv.writer(f)
                    writer.writerows(batch)
                # Mark all items as done so flush()/join() can unblock
                for _ in range(count):
                    self._write_queue.task_done()
                batch.clear()
            except Empty:
                continue
            except Exception:
                # If file write fails, don't crash the logger — mark done and drop
                for _ in range(len(batch)):
                    self._write_queue.task_done()
                batch.clear()
                continue

    def flush(self) -> None:
        """
        Flush all pending writes to disk. Call before shutdown if you need
        to guarantee all events are persisted.
        Zrzuć wszystkie oczekujące zapisy na dysk.
        """
        if self._write_queue is None:
            return
        # Wait until queue is drained
        self._write_queue.join()

    def log(self, event_type: EventType, order_id: int = 0,
            symbol: str = '', side: str = '', quantity: int = 0,
            price: float = 0.0, details: str = '') -> TradeEvent:
        """
        Record a trade event. Thread-safe via mutex lock.
        File I/O is deferred to an async flush thread — the lock only covers
        in-memory operations for minimal latency.

        Zapisz zdarzenie handlowe. Bezpieczne wątkowo przez blokadę mutex.
        I/O plikowe jest odroczone do wątku asynchronicznego zrzutu — lock
        obejmuje tylko operacje w pamięci dla minimalnej latencji.

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
        # ONLY in-memory operations are done under the lock — no disk I/O!
        # with self._lock: przejmuje mutex — jak 'flock /tmp/lockfile' w bashu
        # TYLKO operacje w pamięci pod lockiem — bez I/O dyskowego!
        with self._lock:
            self._sequence += 1
            self._total_logged += 1
            self._events.append(event)

            # Update counter for this event type
            # Aktualizuj licznik dla tego typu zdarzenia
            key = event_type.value
            self._counters[key] = self._counters.get(key, 0) + 1
            seq = self._sequence

        # Enqueue CSV row for async write — OUTSIDE the lock
        # Wstaw wiersz CSV do kolejki asynchronicznego zapisu — POZA lockiem
        if self._write_queue is not None:
            self._write_queue.put([
                seq, timestamp, event_type.value,
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
            results = list(self._events)

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

            events = list(self._events)
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

    def total_logged(self) -> int:
        """
        Total number of events ever logged (includes events dropped by ring buffer).
        Łączna liczba kiedykolwiek zalogowanych zdarzeń (w tym usunięte przez bufor pierścieniowy).
        """
        return self._total_logged

    def events_in_buffer(self) -> int:
        """
        Number of events currently in the memory buffer.
        Liczba zdarzeń aktualnie w buforze pamięci.
        """
        return len(self._events)

    def buffer_full(self) -> bool:
        """
        Whether the ring buffer is at capacity (always False if max_events=0).
        Czy bufor pierścieniowy jest pełny (zawsze False jeśli max_events=0).
        """
        if self._max_events <= 0:
            return False
        return len(self._events) >= self._max_events

    # --- JSON Export ---

    def event_to_dict(self, event: TradeEvent) -> Dict[str, Any]:
        """
        Convert a TradeEvent to a plain dictionary (JSON-serializable).
        Konwertuj TradeEvent na zwykły słownik (serializowalny do JSON).
        """
        return {
            'timestamp_ns': event.timestamp_ns,
            'event_type': event.event_type.value,
            'order_id': event.order_id,
            'symbol': event.symbol,
            'side': event.side,
            'quantity': event.quantity,
            'price': event.price,
            'details': event.details
        }

    def export_json(self, filepath: Optional[str] = None, indent: int = 2) -> str:
        """
        Export all events as JSON. Optionally write to a file.
        Eksportuj wszystkie zdarzenia jako JSON. Opcjonalnie zapisz do pliku.

        Args:
            filepath: if provided, writes JSON to this file path
            indent:   JSON indentation (default 2 spaces)

        Returns:
            JSON string of all events.
        """
        with self._lock:
            events = list(self._events)

        data = {
            'total_logged': self._total_logged,
            'events_in_buffer': len(events),
            'events': [self.event_to_dict(e) for e in events]
        }

        json_str = json.dumps(data, indent=indent)

        if filepath:
            with open(filepath, 'w') as f:
                f.write(json_str)

        return json_str

    def export_jsonl(self, filepath: Optional[str] = None) -> str:
        """
        Export events as JSON Lines (one JSON object per line).
        Ideal for streaming / log aggregation pipelines (like ELK, Splunk).
        Eksportuj zdarzenia jako JSON Lines (jeden obiekt JSON na linię).

        Args:
            filepath: if provided, writes JSONL to this file path

        Returns:
            JSONL string.
        """
        with self._lock:
            events = list(self._events)

        lines = [json.dumps(self.event_to_dict(e)) for e in events]
        jsonl_str = '\n'.join(lines)

        if filepath:
            with open(filepath, 'w') as f:
                f.write(jsonl_str)
                f.write('\n')

        return jsonl_str

    # --- Time-Range Queries ---

    def get_events_in_range(self, start_ns: int, end_ns: int,
                            event_type: Optional[EventType] = None,
                            symbol: Optional[str] = None) -> List[TradeEvent]:
        """
        Query events within a nanosecond timestamp range.
        Zapytaj o zdarzenia w zakresie znaczników czasu (nanosekundy).

        Args:
            start_ns: start of time window (inclusive)
            end_ns:   end of time window (inclusive)
            event_type: optional filter by event type
            symbol:     optional filter by symbol

        Returns:
            List of TradeEvent objects within the time range.
        """
        with self._lock:
            events = list(self._events)

        results = [e for e in events if start_ns <= e.timestamp_ns <= end_ns]

        if event_type is not None:
            results = [e for e in results if e.event_type == event_type]
        if symbol is not None:
            results = [e for e in results if e.symbol == symbol]

        return results

    def get_events_last_ms(self, ms: float,
                           event_type: Optional[EventType] = None) -> List[TradeEvent]:
        """
        Get events from the last N milliseconds.
        Pobierz zdarzenia z ostatnich N milisekund.

        Args:
            ms: how many milliseconds back to look
            event_type: optional filter

        Returns:
            List of recent TradeEvent objects.
        """
        now = time.time_ns()
        start_ns = now - int(ms * 1_000_000)
        return self.get_events_in_range(start_ns, now, event_type=event_type)

    # --- Latency Statistics ---

    def get_inter_event_latencies_ns(self) -> List[int]:
        """
        Compute nanosecond gaps between consecutive events.
        Useful for detecting stalls, bursts, or gaps in logging.
        Oblicz przerwy w nanosekundach między kolejnymi zdarzeniami.

        Returns:
            List of inter-event latencies in nanoseconds.
        """
        with self._lock:
            events = list(self._events)

        if len(events) < 2:
            return []

        return [events[i].timestamp_ns - events[i - 1].timestamp_ns
                for i in range(1, len(events))]

    def get_latency_stats(self) -> Dict[str, Any]:
        """
        Compute percentile statistics for inter-event latencies.
        Oblicz statystyki percentylowe dla opóźnień między zdarzeniami.

        Returns:
            Dict with min, max, mean, p50, p90, p99 in nanoseconds.
        """
        latencies = self.get_inter_event_latencies_ns()
        if not latencies:
            return {'count': 0, 'min_ns': 0, 'max_ns': 0, 'mean_ns': 0,
                    'p50_ns': 0, 'p90_ns': 0, 'p99_ns': 0}

        latencies_sorted = sorted(latencies)
        n = len(latencies_sorted)

        return {
            'count': n,
            'min_ns': latencies_sorted[0],
            'max_ns': latencies_sorted[-1],
            'mean_ns': int(statistics.mean(latencies_sorted)),
            'p50_ns': latencies_sorted[n // 2],
            'p90_ns': latencies_sorted[int(n * 0.90)],
            'p99_ns': latencies_sorted[min(int(n * 0.99), n - 1)]
        }

    def get_order_lifecycle_ns(self, order_id: int) -> Optional[int]:
        """
        Get the total lifecycle duration of an order in nanoseconds
        (from first event to last event for that order_id).
        Pobierz łączny czas cyklu życia zlecenia w nanosekundach.

        Returns:
            Duration in ns, or None if order not found / only one event.
        """
        events = self.get_events(order_id=order_id)
        if len(events) < 2:
            return None
        return events[-1].timestamp_ns - events[0].timestamp_ns

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

    # --- New features demo ---

    # JSON export
    print("\n--- JSON Export (first 2 events) ---")
    json_data = json.loads(logger.export_json())
    for ev in json_data['events'][:2]:
        print(f"  {ev['event_type']:15s}  {ev['symbol']}  {ev['side']}  qty={ev['quantity']}")
    print(f"  ... ({json_data['events_in_buffer']} events total)")

    # Latency stats
    print("\n--- Inter-Event Latency Stats ---")
    lstats = logger.get_latency_stats()
    if lstats['count'] > 0:
        print(f"  Events:  {lstats['count']}")
        print(f"  Min:     {lstats['min_ns']} ns")
        print(f"  p50:     {lstats['p50_ns']} ns")
        print(f"  p99:     {lstats['p99_ns']} ns")
        print(f"  Max:     {lstats['max_ns']} ns")

    # Order lifecycle
    print("\n--- Order Lifecycle Durations ---")
    for oid in [1, 2, 3]:
        dur = logger.get_order_lifecycle_ns(oid)
        if dur is not None:
            print(f"  Order #{oid}: {dur} ns ({dur / 1_000_000:.3f} ms)")
        else:
            print(f"  Order #{oid}: single event (no lifecycle)")

    # Ring buffer demo
    print("\n--- Ring Buffer Demo (max_events=5) ---")
    ring_logger = TradeLogger(max_events=5)
    for i in range(8):
        ring_logger.log(EventType.ORDER_SUBMIT, order_id=i + 1, symbol='TEST',
                        side='BUY', quantity=100, price=1.0)
    print(f"  Total logged:     {ring_logger.total_logged()}")
    print(f"  Events in buffer: {ring_logger.events_in_buffer()}")
    print(f"  Buffer full:      {ring_logger.buffer_full()}")
    buffered = ring_logger.get_events()
    print(f"  Buffered order IDs: {[e.order_id for e in buffered]}")


if __name__ == '__main__':
    demo()
