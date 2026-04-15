import os
import sys
import importlib.util

# fix-protocol has a hyphen — can't use normal import
_spec = importlib.util.spec_from_file_location(
    "fix_parser",
    os.path.join(os.path.dirname(__file__), '..', 'fix-protocol', 'fix_parser.py')
)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
FIXMessage = _mod.FIXMessage


def test_parse_new_order():
    msg = FIXMessage()
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100")
    assert msg.get_msg_type() == 'D'
    assert msg.get_symbol() == 'AAPL'
    assert msg.get_side() == 'BUY'
    assert msg.get_price() == 150.25
    assert msg.get_quantity() == 100
    print("  PASS: test_parse_new_order")


def test_parse_sell():
    msg = FIXMessage()
    msg.parse("8=FIX.4.2|35=D|55=MSFT|54=2|44=380.50|38=50")
    assert msg.get_side() == 'SELL'
    assert msg.get_symbol() == 'MSFT'
    print("  PASS: test_parse_sell")


def test_parse_cancel():
    msg = FIXMessage()
    msg.parse("8=FIX.4.2|35=F|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100")
    assert msg.get_msg_type() == 'F'
    print("  PASS: test_parse_cancel")


def test_parse_execution():
    msg = FIXMessage()
    msg.parse("8=FIX.4.2|35=8|49=EXCHANGE|56=TRADER1|55=AAPL|54=1|44=150.25|38=100")
    assert msg.get_msg_type() == '8'
    print("  PASS: test_parse_execution")


def test_malformed_tags():
    """Ensure malformed tags don't crash the parser"""
    msg = FIXMessage()
    msg.parse("abc=xyz|35=D|=empty|55=AAPL|bad|44=100.0")
    assert msg.get_msg_type() == 'D'
    assert msg.get_symbol() == 'AAPL'
    assert msg.get_price() == 100.0
    print("  PASS: test_malformed_tags")


def test_parse_speed():
    msg = FIXMessage()
    ns = msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100")
    assert ns < 1_000_000  # under 1ms
    print(f"  PASS: test_parse_speed ({ns} ns)")


def test_empty_message():
    msg = FIXMessage()
    ns = msg.parse("")
    assert msg.get_msg_type() == 'UNKNOWN'
    print("  PASS: test_empty_message")


if __name__ == '__main__':
    print("=== FIX Protocol Unit Tests ===\n")
    tests = [
        test_parse_new_order,
        test_parse_sell,
        test_parse_cancel,
        test_parse_execution,
        test_malformed_tags,
        test_parse_speed,
        test_empty_message,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests passed")
