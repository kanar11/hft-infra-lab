import os
import sys
import struct
import importlib.util

# ouch-protocol has a hyphen — can't use normal import
_spec = importlib.util.spec_from_file_location(
    "ouch_sender",
    os.path.join(os.path.dirname(__file__), '..', 'ouch-protocol', 'ouch_sender.py')
)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
OUCHMessage = _mod.OUCHMessage


def test_enter_order_encoding():
    ouch = OUCHMessage()
    msg = ouch.enter_order("ORD001", "B", 100, "AAPL", 150.25)
    assert msg[0:1] == b'O'
    assert len(msg) == 33  # 1+14+1+4+8+4+1
    print("  PASS: test_enter_order_encoding")


def test_cancel_order_encoding():
    ouch = OUCHMessage()
    msg = ouch.cancel_order("ORD001", 0)
    assert msg[0:1] == b'X'
    assert len(msg) == 19  # 1+14+4
    print("  PASS: test_cancel_order_encoding")


def test_replace_order_encoding():
    ouch = OUCHMessage()
    msg = ouch.replace_order("ORD001", "ORD002", 50, 151.00)
    assert msg[0:1] == b'U'
    assert len(msg) == 37  # 1+14+14+4+4
    print("  PASS: test_replace_order_encoding")


def test_parse_accepted():
    ouch = OUCHMessage()
    data = struct.pack('!c 14s c I 8s I c q',
        b'A',
        b'ORD001        ',
        b'B',
        100,
        b'AAPL    ',
        1502500,
        b'D',
        999
    )
    result = ouch.parse_response(data)
    assert result['type'] == 'ACCEPTED'
    assert result['token'] == 'ORD001'
    assert result['side'] == 'BUY'
    assert result['shares'] == 100
    assert result['stock'] == 'AAPL'
    assert result['price'] == 150.25
    print("  PASS: test_parse_accepted")


def test_parse_truncated():
    ouch = OUCHMessage()
    result = ouch.parse_response(b'A' + b'\x00' * 5)
    assert result['type'] == 'ERROR'
    assert 'too short' in result['reason']
    print("  PASS: test_parse_truncated")


def test_parse_empty():
    ouch = OUCHMessage()
    result = ouch.parse_response(b'')
    assert result['type'] == 'ERROR'
    print("  PASS: test_parse_empty")


def test_price_encoding_precision():
    ouch = OUCHMessage()
    msg = ouch.enter_order("ORD001", "B", 1, "TEST", 99.9999)
    price_raw = struct.unpack('!I', msg[28:32])[0]
    assert price_raw == 999999  # 99.9999 * 10000
    print("  PASS: test_price_encoding_precision")


if __name__ == '__main__':
    print("=== OUCH Protocol Unit Tests ===\n")
    tests = [
        test_enter_order_encoding,
        test_cancel_order_encoding,
        test_replace_order_encoding,
        test_parse_accepted,
        test_parse_truncated,
        test_parse_empty,
        test_price_encoding_precision,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests passed")
