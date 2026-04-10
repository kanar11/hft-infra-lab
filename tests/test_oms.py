import sys
sys.path.insert(0, '/home/kanar11/hft-infra-lab')
from oms.oms import OMS, Side, OrderStatus

def test_submit_order():
    oms = OMS(max_position=1000, max_order_value=100000)
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    assert order is not None
    assert order.status == OrderStatus.SENT
    assert order.symbol == "AAPL"
    print("  PASS: test_submit_order")

def test_risk_check_value():
    oms = OMS(max_position=1000, max_order_value=10000)
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)  # 15000 > 10000
    assert order is None
    print("  PASS: test_risk_check_value")

def test_risk_check_position():
    oms = OMS(max_position=50, max_order_value=100000)
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)  # 100 > 50
    assert order is None
    print("  PASS: test_risk_check_position")

def test_fill_order():
    oms = OMS()
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(order.order_id, 100, 150.00)
    assert order.status == OrderStatus.FILLED
    assert order.filled_qty == 100
    print("  PASS: test_fill_order")

def test_partial_fill():
    oms = OMS()
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(order.order_id, 50, 150.00)
    assert order.status == OrderStatus.PARTIALLY_FILLED
    assert order.filled_qty == 50
    print("  PASS: test_partial_fill")

def test_cancel_order():
    oms = OMS()
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.cancel_order(order.order_id)
    assert order.status == OrderStatus.CANCELLED
    print("  PASS: test_cancel_order")

def test_position_tracking():
    oms = OMS()
    order = oms.submit_order("AAPL", Side.BUY, 150.00, 100)
    oms.fill_order(order.order_id, 100, 150.00)
    pos = oms.positions["AAPL"]
    assert pos.net_qty == 100
    print("  PASS: test_position_tracking")

if __name__ == '__main__':
    print("=== OMS Unit Tests ===\n")
    tests = [
        test_submit_order,
        test_risk_check_value,
        test_risk_check_position,
        test_fill_order,
        test_partial_fill,
        test_cancel_order,
        test_position_tracking,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"  FAIL: {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests passed")
