import os
import sys
import struct
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from itch_parser.itch_parser import ITCHMessage

def test_parse_add_order():
    msg = struct.pack('!c q q c I 8s I',
        b'A', 1000000, 1001, b'B', 100, b'AAPL    ', 1502500)
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['type'] == 'ADD_ORDER'
    assert result['stock'] == 'AAPL'
    assert result['side'] == 'BUY'
    assert result['shares'] == 100
    assert result['price'] == 150.25
    print("  PASS: test_parse_add_order")

def test_parse_sell_order():
    msg = struct.pack('!c q q c I 8s I',
        b'A', 2000000, 1002, b'S', 50, b'MSFT    ', 3805000)
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['side'] == 'SELL'
    assert result['stock'] == 'MSFT'
    assert result['price'] == 380.50
    print("  PASS: test_parse_sell_order")

def test_parse_delete():
    msg = struct.pack('!c q q', b'D', 3000000, 1001)
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['type'] == 'DELETE_ORDER'
    assert result['order_ref'] == 1001
    print("  PASS: test_parse_delete")

def test_parse_trade():
    msg = struct.pack('!c q q c I 8s I q',
        b'P', 4000000, 1001, b'B', 100, b'AAPL    ', 1502500, 5001)
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['type'] == 'TRADE'
    assert result['match_number'] == 5001
    print("  PASS: test_parse_trade")

def test_parse_replace_order():
    msg = struct.pack('!c q q q I I',
        b'U', 5000000, 1002, 1003, 50, 3810000)
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['type'] == 'REPLACE_ORDER'
    assert result['orig_order_ref'] == 1002
    assert result['new_order_ref'] == 1003
    assert result['shares'] == 50
    assert result['price'] == 381.0
    print("  PASS: test_parse_replace_order")

def test_parse_order_executed():
    msg = struct.pack('!c q q I q',
        b'E', 6000000, 1001, 100, 5001)
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['type'] == 'ORDER_EXECUTED'
    assert result['order_ref'] == 1001
    assert result['shares'] == 100
    assert result['match_number'] == 5001
    print("  PASS: test_parse_order_executed")

def test_parse_order_cancelled():
    msg = struct.pack('!c q q I',
        b'C', 7000000, 1002, 25)
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['type'] == 'ORDER_CANCELLED'
    assert result['order_ref'] == 1002
    assert result['cancelled_shares'] == 25
    print("  PASS: test_parse_order_cancelled")

def test_parse_system_event():
    msg = struct.pack('!c q c',
        b'S', 8000000, b'Q')
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['type'] == 'SYSTEM_EVENT'
    assert result['event_code'] == 'START_OF_MARKET_HOURS'
    print("  PASS: test_parse_system_event")

def test_parse_stock_directory():
    msg = struct.pack('!c q 8s c',
        b'R', 9000000, b'AAPL    ', b'Q')
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['type'] == 'STOCK_DIRECTORY'
    assert result['stock'] == 'AAPL'
    assert result['market_category'] == 'Q'
    print("  PASS: test_parse_stock_directory")

def test_parse_speed():
    msg = struct.pack('!c q q c I 8s I',
        b'A', 1000000, 1001, b'B', 100, b'AAPL    ', 1502500)
    parser = ITCHMessage()
    result = parser.parse(msg)
    assert result['parse_time_ns'] < 1_000_000  # under 1ms
    print(f"  PASS: test_parse_speed ({result['parse_time_ns']} ns)")

if __name__ == '__main__':
    print("=== ITCH Parser Unit Tests ===\n")
    tests = [
        test_parse_add_order,
        test_parse_sell_order,
        test_parse_delete,
        test_parse_trade,
        test_parse_replace_order,
        test_parse_order_executed,
        test_parse_order_cancelled,
        test_parse_system_event,
        test_parse_stock_directory,
        test_parse_speed,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests passed")
