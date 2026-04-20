#!/usr/bin/env python3
"""
Unit tests for Trade Logger / Audit Trail.
Testy jednostkowe dla Loggera transakcji / Ścieżki audytu.
"""
import os
import sys
import tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from logger.trade_logger import TradeLogger, EventType


def test_log_and_retrieve():
    """Test basic event logging and retrieval."""
    """Testuje podstawowe logowanie i pobieranie zdarzeń."""
    logger = TradeLogger()
    logger.log(EventType.ORDER_SUBMIT, order_id=1, symbol='AAPL',
               side='BUY', quantity=100, price=150.25)
    events = logger.get_events()
    assert len(events) == 1
    assert events[0].event_type == EventType.ORDER_SUBMIT
    assert events[0].symbol == 'AAPL'
    assert events[0].price == 150.25
    print("  PASS: test_log_and_retrieve")


def test_filter_by_order_id():
    """Test filtering events by order ID."""
    """Testuje filtrowanie zdarzeń po ID zlecenia."""
    logger = TradeLogger()
    logger.log(EventType.ORDER_SUBMIT, order_id=1, symbol='AAPL')
    logger.log(EventType.ORDER_SUBMIT, order_id=2, symbol='TSLA')
    logger.log(EventType.ORDER_FILL, order_id=1, symbol='AAPL')
    events = logger.get_events(order_id=1)
    assert len(events) == 2
    assert all(e.order_id == 1 for e in events)
    print("  PASS: test_filter_by_order_id")


def test_filter_by_event_type():
    """Test filtering events by type."""
    """Testuje filtrowanie zdarzeń po typie."""
    logger = TradeLogger()
    logger.log(EventType.ORDER_SUBMIT, order_id=1)
    logger.log(EventType.RISK_ACCEPT, order_id=1)
    logger.log(EventType.ORDER_FILL, order_id=1)
    logger.log(EventType.ORDER_SUBMIT, order_id=2)
    fills = logger.get_events(event_type=EventType.ORDER_FILL)
    assert len(fills) == 1
    submits = logger.get_events(event_type=EventType.ORDER_SUBMIT)
    assert len(submits) == 2
    print("  PASS: test_filter_by_event_type")


def test_filter_by_symbol():
    """Test filtering events by stock symbol."""
    """Testuje filtrowanie zdarzeń po symbolu akcji."""
    logger = TradeLogger()
    logger.log(EventType.ORDER_SUBMIT, order_id=1, symbol='AAPL')
    logger.log(EventType.ORDER_SUBMIT, order_id=2, symbol='TSLA')
    logger.log(EventType.ORDER_SUBMIT, order_id=3, symbol='AAPL')
    aapl = logger.get_events(symbol='AAPL')
    assert len(aapl) == 2
    print("  PASS: test_filter_by_symbol")


def test_order_trail():
    """Test complete order lifecycle trail."""
    """Testuje pełny cykl życia zlecenia."""
    logger = TradeLogger()
    logger.log(EventType.ORDER_SUBMIT, order_id=1, symbol='AAPL',
               side='BUY', quantity=100, price=150.25)
    logger.log(EventType.RISK_ACCEPT, order_id=1, symbol='AAPL',
               details='passed')
    logger.log(EventType.ORDER_FILL, order_id=1, symbol='AAPL',
               side='BUY', quantity=100, price=150.25,
               details='venue=NASDAQ')
    trail = logger.get_order_trail(1)
    assert len(trail) == 3
    assert trail[0]['event'] == 'ORDER_SUBMIT'
    assert trail[1]['event'] == 'RISK_ACCEPT'
    assert trail[2]['event'] == 'ORDER_FILL'
    assert trail[2]['details'] == 'venue=NASDAQ'
    print("  PASS: test_order_trail")


def test_summary_stats():
    """Test summary statistics calculation."""
    """Testuje obliczanie statystyk podsumowujących."""
    logger = TradeLogger()
    logger.log(EventType.SYSTEM_START)
    logger.log(EventType.ORDER_SUBMIT, order_id=1, symbol='AAPL')
    logger.log(EventType.ORDER_SUBMIT, order_id=2, symbol='TSLA')
    logger.log(EventType.RISK_REJECT, order_id=2, symbol='TSLA')
    logger.log(EventType.ORDER_FILL, order_id=1, symbol='AAPL')
    summary = logger.get_summary()
    assert summary['total_events'] == 5
    assert summary['unique_orders'] == 2
    assert summary['unique_symbols'] == 2
    assert summary['counters']['ORDER_SUBMIT'] == 2
    assert summary['counters']['RISK_REJECT'] == 1
    print("  PASS: test_summary_stats")


def test_empty_logger():
    """Test summary on empty logger returns zeros."""
    """Testuje że podsumowanie pustego loggera zwraca zera."""
    logger = TradeLogger()
    summary = logger.get_summary()
    assert summary['total_events'] == 0
    assert summary['unique_orders'] == 0
    print("  PASS: test_empty_logger")


def test_csv_output():
    """Test that CSV audit file is written correctly."""
    """Testuje że plik audytu CSV jest zapisywany poprawnie."""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.csv', delete=False) as f:
        csv_path = f.name

    try:
        logger = TradeLogger(log_file=csv_path)
        logger.log(EventType.ORDER_SUBMIT, order_id=1, symbol='AAPL',
                   side='BUY', quantity=100, price=150.25)
        logger.log(EventType.ORDER_FILL, order_id=1, symbol='AAPL',
                   side='BUY', quantity=100, price=150.25)

        # Flush async write queue before reading the file
        logger.flush()

        with open(csv_path, 'r') as f:
            lines = f.readlines()
        # Header + 2 data rows
        assert len(lines) == 3
        assert 'sequence' in lines[0]
        assert 'ORDER_SUBMIT' in lines[1]
        assert 'ORDER_FILL' in lines[2]
        print("  PASS: test_csv_output")
    finally:
        os.unlink(csv_path)


def test_log_speed():
    """Test that logging one event takes under 1 millisecond."""
    """Testuje że logowanie jednego zdarzenia zajmuje mniej niż 1 milisekundę."""
    logger = TradeLogger()
    speed = logger.get_log_speed_ns()
    assert speed < 1_000_000  # under 1ms
    print(f"  PASS: test_log_speed ({speed} ns)")


def test_kill_switch_event():
    """Test kill switch event logging."""
    """Testuje logowanie zdarzenia wyłącznika awaryjnego."""
    logger = TradeLogger()
    logger.log(EventType.KILL_SWITCH, details='manual_trigger')
    events = logger.get_events(event_type=EventType.KILL_SWITCH)
    assert len(events) == 1
    assert events[0].details == 'manual_trigger'
    print("  PASS: test_kill_switch_event")


if __name__ == '__main__':
    print("=== Trade Logger Unit Tests ===\n")
    tests = [
        test_log_and_retrieve,
        test_filter_by_order_id,
        test_filter_by_event_type,
        test_filter_by_symbol,
        test_order_trail,
        test_summary_stats,
        test_empty_logger,
        test_csv_output,
        test_log_speed,
        test_kill_switch_event,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests passed")
