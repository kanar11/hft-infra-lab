import os
import sys
import time
import struct
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from itch_parser.itch_parser import ITCHMessage
from oms.oms import OMS, Side

def benchmark_itch(iterations=100000):
    parser = ITCHMessage()
    msg = struct.pack('!c q q c I 8s I',
        b'A', 1000000, 1001, b'B', 100, b'AAPL    ', 1502500)
    
    start = time.time_ns()
    for _ in range(iterations):
        parser.parse(msg)
    elapsed = time.time_ns() - start
    
    per_msg = elapsed / iterations
    throughput = 1_000_000_000 / per_msg
    print(f"ITCH Parser:")
    print(f"  {iterations:,} messages in {elapsed/1_000_000:.1f} ms")
    print(f"  {per_msg:.0f} ns/msg")
    print(f"  {throughput:,.0f} msg/sec\n")

def benchmark_oms(iterations=50000):
    oms = OMS(max_position=10_000_000, max_order_value=10_000_000)
    
    start = time.time_ns()
    for i in range(iterations):
        side = Side.BUY if i % 2 == 0 else Side.SELL
        order = oms.submit_order("AAPL", side, 150.00, 1)
        if order:
            oms.fill_order(order.order_id, 1, 150.00)
    elapsed = time.time_ns() - start
    
    per_order = elapsed / iterations
    throughput = 1_000_000_000 / per_order
    print(f"OMS (submit + fill):")
    print(f"  {iterations:,} orders in {elapsed/1_000_000:.1f} ms")
    print(f"  {per_order:.0f} ns/order")
    print(f"  {throughput:,.0f} orders/sec\n")

if __name__ == '__main__':
    print("=== HFT Infrastructure Benchmarks ===\n")
    benchmark_itch()
    benchmark_oms()
