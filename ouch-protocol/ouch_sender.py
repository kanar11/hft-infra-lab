import struct
import time
import socket

class OUCHMessage:
    """NASDAQ OUCH 4.2 protocol message builder (simplified)"""
    
    MSG_TYPES = {
        'ENTER_ORDER': b'O',
        'REPLACE_ORDER': b'U',
        'CANCEL_ORDER': b'X',
    }

    @staticmethod
    def enter_order(token, side, shares, stock, price, tif='D'):
        """
        Enter Order message:
        - type: 1 byte (O)
        - token: 14 bytes (order token, right-padded)
        - side: 1 byte (B=buy, S=sell)
        - shares: 4 bytes
        - stock: 8 bytes (right-padded)
        - price: 4 bytes (fixed point, 4 decimals)
        - tif: 1 byte (D=day, I=IOC, S=system)
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
    def cancel_order(token, shares=0):
        """
        Cancel Order message:
        - type: 1 byte (X)
        - token: 14 bytes
        - shares: 4 bytes (0 = cancel all)
        """
        msg = struct.pack('!c 14s I',
            b'X',
            token.encode().ljust(14),
            shares
        )
        return msg

    @staticmethod
    def replace_order(existing_token, new_token, shares, price):
        """
        Replace Order message:
        - type: 1 byte (U)
        - existing_token: 14 bytes
        - new_token: 14 bytes
        - shares: 4 bytes
        - price: 4 bytes
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
    def parse_response(data):
        """Parse exchange response with bounds checking"""
        if not data or len(data) < 1:
            return {'type': 'ERROR', 'reason': 'empty response'}

        msg_type = data[0:1]
        if msg_type == b'A':  # Accepted
            if len(data) < 41:
                return {'type': 'ERROR', 'reason': f'ACCEPTED too short: {len(data)} < 41 bytes'}
            fields = struct.unpack('!c 14s c I 8s I c q', data[:41])
            return {
                'type': 'ACCEPTED',
                'token': fields[1].decode().strip(),
                'side': 'BUY' if fields[2] == b'B' else 'SELL',
                'shares': fields[3],
                'stock': fields[4].decode().strip(),
                'price': fields[5] / 10000.0,
                'order_ref': fields[7]
            }
        elif msg_type == b'C':  # Cancelled
            if len(data) < 20:
                return {'type': 'ERROR', 'reason': f'CANCELLED too short: {len(data)} < 20 bytes'}
            fields = struct.unpack('!c 14s I c', data[:20])
            return {
                'type': 'CANCELLED',
                'token': fields[1].decode().strip(),
                'shares': fields[2],
                'reason': fields[3].decode()
            }
        elif msg_type == b'E':  # Executed
            if len(data) < 31:
                return {'type': 'ERROR', 'reason': f'EXECUTED too short: {len(data)} < 31 bytes'}
            fields = struct.unpack('!c 14s I I q', data[:31])
            return {
                'type': 'EXECUTED',
                'token': fields[1].decode().strip(),
                'shares': fields[2],
                'price': fields[3] / 10000.0,
                'match_number': fields[4]
            }
        return {'type': 'UNKNOWN', 'raw': data.hex()}


def benchmark_encoding(iterations=100000):
    """Benchmark OUCH message encoding speed"""
    ouch = OUCHMessage()
    
    start = time.time_ns()
    for i in range(iterations):
        ouch.enter_order(f"ORD{i:010d}", "B", 100, "AAPL", 150.25)
    elapsed = time.time_ns() - start
    
    per_msg = elapsed / iterations
    throughput = 1_000_000_000 / per_msg
    return per_msg, throughput


def main():
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
