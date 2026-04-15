#!/usr/bin/env python3
"""
NASDAQ OUCH 4.2 Order Entry Protocol

Encodes and decodes OUCH messages for order entry (Enter, Cancel, Replace)
and server responses (Accepted, Replaced, Cancelled, Executed).
"""
import struct
import time
from typing import Dict, Any, Tuple


class OUCHMessage:
    """NASDAQ OUCH 4.2 protocol message builder (simplified)."""

    MSG_TYPES = {
        'ENTER_ORDER': b'O',
        'REPLACE_ORDER': b'U',
        'CANCEL_ORDER': b'X',
    }

    @staticmethod
    def enter_order(token: str, side: str, shares: int, stock: str,
                    price: float, tif: str = 'D') -> bytes:
        """Build Enter Order message (33 bytes).
        Args:
            token: Order token (up to 14 chars)
            side: 'B' for buy, 'S' for sell
            shares: Number of shares
            stock: Stock symbol (up to 8 chars)
            price: Price in dollars (converted to fixed-point internally)
            tif: Time-in-force ('D'=day, 'I'=IOC, 'S'=system)
        """
        msg = struct.pack('!c 14s c I 8s I c',
            b'O',
            token.encode().ljust(14),
            side.encode(),
            shares,
            stock.encode().ljust(8),
            int(price * 10000),
            tif.encode()
        )
        return msg

    @staticmethod
    def cancel_order(token: str, shares: int = 0) -> bytes:
        """Build Cancel Order message (19 bytes).
        Args:
            token: Order token to cancel
            shares: Shares to cancel (0 = cancel all)
        """
        msg = struct.pack('!c 14s I',
            b'X',
            token.encode().ljust(14),
            shares
        )
        return msg

    @staticmethod
    def replace_order(existing_token: str, new_token: str,
                      shares: int, price: float) -> bytes:
        """Build Replace Order message (37 bytes).
        Args:
            existing_token: Token of order to replace
            new_token: New order token
            shares: New share quantity
            price: New price in dollars
        """
        msg = struct.pack('!c 14s 14s I I',
            b'U',
            existing_token.encode().ljust(14),
            new_token.encode().ljust(14),
            shares,
            int(price * 10000)
        )
        return msg

    @staticmethod
    def parse_response(data: bytes) -> Dict[str, Any]:
        """Parse exchange response with bounds checking.
        Args:
            data: Raw bytes from exchange
        Returns:
            Dict with parsed message fields, or error dict if malformed
        """
        if not data or len(data) < 1:
            return {'type': 'ERROR', 'reason': 'empty response'}

        msg_type = data[0:1]
        if msg_type == b'A':  # Accepted
            if len(data) < 41:
                return {'type': 'ERROR', 'reason': f'ACCEPTED too short: {len(data)} < 41 bytes'}
            fields = struct.unpack('!c 14s c I 8s I c q', data[:41])
            return {
                'type': 'ACCEPTED',
                'token': fields[1].decode('ascii', errors='replace').strip(),
                'side': 'BUY' if fields[2] == b'B' else 'SELL',
                'shares': fields[3],
                'stock': fields[4].decode('ascii', errors='replace').strip(),
                'price': fields[5] / 10000.0,
                'order_ref': fields[7]
            }
        elif msg_type == b'C':  # Cancelled
            if len(data) < 20:
                return {'type': 'ERROR', 'reason': f'CANCELLED too short: {len(data)} < 20 bytes'}
            fields = struct.unpack('!c 14s I c', data[:20])
            return {
                'type': 'CANCELLED',
                'token': fields[1].decode('ascii', errors='replace').strip(),
                'shares': fields[2],
                'reason': fields[3].decode('ascii', errors='replace')
            }
        elif msg_type == b'E':  # Executed
            if len(data) < 31:
                return {'type': 'ERROR', 'reason': f'EXECUTED too short: {len(data)} < 31 bytes'}
            fields = struct.unpack('!c 14s I I q', data[:31])
            return {
                'type': 'EXECUTED',
                'token': fields[1].decode('ascii', errors='replace').strip(),
                'shares': fields[2],
                'price': fields[3] / 10000.0,
                'match_number': fields[4]
            }
        return {'type': 'UNKNOWN', 'raw': data.hex()}


def benchmark_encoding(iterations: int = 100000) -> Tuple[float, float]:
    """Benchmark OUCH message encoding speed."""
    ouch = OUCHMessage()

    start = time.time_ns()
    for i in range(iterations):
        ouch.enter_order(f"ORD{i:010d}", "B", 100, "AAPL", 150.25)
    elapsed = time.time_ns() - start

    per_msg = elapsed / iterations
    throughput = 1_000_000_000 / per_msg
    return per_msg, throughput


def main() -> None:
    print("=== NASDAQ OUCH 4.2 Protocol ===\n")
    ouch = OUCHMessage()

    # Demo messages
    print("--- Building Orders ---")

    msg1 = ouch.enter_order("ORD001", "B", 100, "AAPL", 150.25)
    print(f"  ENTER BUY  100 AAPL @ 150.25 ({len(msg1)} bytes): {msg1.hex()}")

    msg2 = ouch.enter_order("ORD002", "S", 50, "MSFT", 380.50, "I")
    print(f"  ENTER SELL  50 MSFT @ 380.50 IOC ({len(msg2)} bytes): {msg2.hex()}")

    msg3 = ouch.cancel_order("ORD001")
    print(f"  CANCEL ORD001 ({len(msg3)} bytes): {msg3.hex()}")

    msg4 = ouch.replace_order("ORD002", "ORD003", 75, 381.00)
    print(f"  REPLACE ORD002->ORD003 75@381.00 ({len(msg4)} bytes): {msg4.hex()}")

    # Benchmark
    print("\n--- Benchmark ---")
    per_msg, throughput = benchmark_encoding()
    print(f"  Encoding: {per_msg:.0f} ns/msg")
    print(f"  Throughput: {throughput:,.0f} msg/sec")

    # ITCH vs OUCH comparison
    print("\n--- ITCH vs OUCH ---")
    print("  ITCH: binary market data FROM exchange (read-only)")
    print("  OUCH: binary order entry TO exchange (write)")
    print("  FIX:  text-based, both directions (slower)")


if __name__ == '__main__':
    main()
