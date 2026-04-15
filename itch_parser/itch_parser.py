import struct
import time

class ITCHMessage:
    """NASDAQ ITCH 5.0 protocol parser (simplified)"""
    
    MSG_TYPES = {
        b'A': 'ADD_ORDER',
        b'D': 'DELETE_ORDER',
        b'U': 'REPLACE_ORDER',
        b'E': 'ORDER_EXECUTED',
        b'C': 'ORDER_CANCELLED',
        b'P': 'TRADE',
        b'S': 'SYSTEM_EVENT',
        b'R': 'STOCK_DIRECTORY',
    }

    @staticmethod
    def parse_add_order(data):
        """
        Add Order message format (simplified ITCH 5.0):
        - msg_type: 1 byte
        - timestamp: 8 bytes (nanoseconds)
        - order_ref: 8 bytes
        - side: 1 byte (B=buy, S=sell)
        - shares: 4 bytes
        - stock: 8 bytes (right-padded with spaces)
        - price: 4 bytes (fixed point, 4 decimal places)
        """
        fmt = '!c q q c I 8s I'
        fields = struct.unpack(fmt, data[:34])
        return {
            'type': 'ADD_ORDER',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'side': 'BUY' if fields[3] == b'B' else 'SELL',
            'shares': fields[4],
            'stock': fields[5].decode('ascii', errors='replace').strip(),
            'price': fields[6] / 10000.0
        }

    @staticmethod
    def parse_delete_order(data):
        fmt = '!c q q'
        fields = struct.unpack(fmt, data[:17])
        return {
            'type': 'DELETE_ORDER',
            'timestamp_ns': fields[1],
            'order_ref': fields[2]
        }

    @staticmethod
    def parse_trade(data):
        fmt = '!c q q c I 8s I q'
        fields = struct.unpack(fmt, data[:42])
        return {
            'type': 'TRADE',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'side': 'BUY' if fields[3] == b'B' else 'SELL',
            'shares': fields[4],
            'stock': fields[5].decode('ascii', errors='replace').strip(),
            'price': fields[6] / 10000.0,
            'match_number': fields[7]
        }

    def parse(self, data):
        start = time.time_ns()
        msg_type = data[0:1]
        
        result = None
        if msg_type == b'A':
            result = self.parse_add_order(data)
        elif msg_type == b'D':
            result = self.parse_delete_order(data)
        elif msg_type == b'P':
            result = self.parse_trade(data)
        else:
            result = {
                'type': self.MSG_TYPES.get(msg_type, f'UNKNOWN({msg_type})'),
                'raw_size': len(data)
            }
        
        elapsed = time.time_ns() - start
        result['parse_time_ns'] = elapsed
        return result


def create_test_messages():
    """Generate sample ITCH binary messages"""
    messages = []
    
    # Add Order: BUY 100 AAPL @ 150.2500
    msg = struct.pack('!c q q c I 8s I',
        b'A',
        1000000000,      # timestamp 1s in ns
        1001,            # order ref
        b'B',            # buy
        100,             # shares
        b'AAPL    ',     # stock (8 bytes padded)
        1502500          # price * 10000
    )
    messages.append(msg)
    
    # Add Order: SELL 50 MSFT @ 380.5000
    msg = struct.pack('!c q q c I 8s I',
        b'A',
        1000001000,
        1002,
        b'S',
        50,
        b'MSFT    ',
        3805000
    )
    messages.append(msg)
    
    # Trade: BUY 100 AAPL @ 150.2500
    msg = struct.pack('!c q q c I 8s I q',
        b'P',
        1000002000,
        1001,
        b'B',
        100,
        b'AAPL    ',
        1502500,
        5001            # match number
    )
    messages.append(msg)
    
    # Delete Order
    msg = struct.pack('!c q q',
        b'D',
        1000003000,
        1002
    )
    messages.append(msg)
    
    return messages


def main():
    print("=== NASDAQ ITCH 5.0 Parser ===\n")
    
    parser = ITCHMessage()
    messages = create_test_messages()
    
    total_ns = 0
    for i, raw in enumerate(messages):
        result = parser.parse(raw)
        parse_ns = result.pop('parse_time_ns')
        total_ns += parse_ns
        
        print(f"Message {i+1} ({len(raw)} bytes):")
        for k, v in result.items():
            print(f"  {k}: {v}")
        print(f"  parse_time: {parse_ns} ns\n")
    
    avg = total_ns // len(messages)
    print(f"Average parse time: {avg} ns")
    print(f"Throughput: ~{1_000_000_000 // avg:,} messages/sec (theoretical)")


if __name__ == '__main__':
    main()
