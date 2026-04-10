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
