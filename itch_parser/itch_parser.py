import struct
import time
from typing import Dict, List, Any, Optional


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
    def parse_add_order(data: bytes) -> Dict[str, Any]:
        """Parse Add Order (A) message.
        Fields: msg_type(1) + timestamp(8) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4) = 34 bytes
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
    def parse_delete_order(data: bytes) -> Dict[str, Any]:
        """Parse Delete Order (D) message.
        Fields: msg_type(1) + timestamp(8) + order_ref(8) = 17 bytes
        """
        fmt = '!c q q'
        fields = struct.unpack(fmt, data[:17])
        return {
            'type': 'DELETE_ORDER',
            'timestamp_ns': fields[1],
            'order_ref': fields[2]
        }

    @staticmethod
    def parse_replace_order(data: bytes) -> Dict[str, Any]:
        """Parse Replace Order (U) message.
        Fields: msg_type(1) + timestamp(8) + orig_ref(8) + new_ref(8) + shares(4) + price(4) = 33 bytes
        """
        fmt = '!c q q q I I'
        fields = struct.unpack(fmt, data[:33])
        return {
            'type': 'REPLACE_ORDER',
            'timestamp_ns': fields[1],
            'orig_order_ref': fields[2],
            'new_order_ref': fields[3],
            'shares': fields[4],
            'price': fields[5] / 10000.0
        }

    @staticmethod
    def parse_order_executed(data: bytes) -> Dict[str, Any]:
        """Parse Order Executed (E) message.
        Fields: msg_type(1) + timestamp(8) + order_ref(8) + shares(4) + match_number(8) = 29 bytes
        """
        fmt = '!c q q I q'
        fields = struct.unpack(fmt, data[:29])
        return {
            'type': 'ORDER_EXECUTED',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'shares': fields[3],
            'match_number': fields[4]
        }

    @staticmethod
    def parse_order_cancelled(data: bytes) -> Dict[str, Any]:
        """Parse Order Cancelled (C) message.
        Fields: msg_type(1) + timestamp(8) + order_ref(8) + cancelled_shares(4) = 21 bytes
        """
        fmt = '!c q q I'
        fields = struct.unpack(fmt, data[:21])
        return {
            'type': 'ORDER_CANCELLED',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'cancelled_shares': fields[3]
        }

    @staticmethod
    def parse_trade(data: bytes) -> Dict[str, Any]:
        """Parse Trade (P) message.
        Fields: msg_type(1) + timestamp(8) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4) + match(8) = 42 bytes
        """
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

    @staticmethod
    def parse_system_event(data: bytes) -> Dict[str, Any]:
        """Parse System Event (S) message.
        Fields: msg_type(1) + timestamp(8) + event_code(1) = 10 bytes
        """
        EVENTS = {b'O': 'START_OF_MESSAGES', b'S': 'START_OF_SYSTEM_HOURS',
                  b'Q': 'START_OF_MARKET_HOURS', b'M': 'END_OF_MARKET_HOURS',
                  b'E': 'END_OF_SYSTEM_HOURS', b'C': 'END_OF_MESSAGES'}
        fmt = '!c q c'
        fields = struct.unpack(fmt, data[:10])
        return {
            'type': 'SYSTEM_EVENT',
            'timestamp_ns': fields[1],
            'event_code': EVENTS.get(fields[2], f'UNKNOWN({fields[2]})')
        }

    @staticmethod
    def parse_stock_directory(data: bytes) -> Dict[str, Any]:
        """Parse Stock Directory (R) message.
        Fields: msg_type(1) + timestamp(8) + stock(8) + market_category(1) = 18 bytes
        """
        fmt = '!c q 8s c'
        fields = struct.unpack(fmt, data[:18])
        return {
            'type': 'STOCK_DIRECTORY',
            'timestamp_ns': fields[1],
            'stock': fields[2].decode('ascii', errors='replace').strip(),
            'market_category': fields[3].decode('ascii', errors='replace')
        }

    def parse(self, data: bytes) -> Dict[str, Any]:
        """Parse any ITCH message by type byte."""
        start = time.time_ns()
        msg_type = data[0:1]

        parsers = {
            b'A': self.parse_add_order,
            b'D': self.parse_delete_order,
            b'U': self.parse_replace_order,
            b'E': self.parse_order_executed,
            b'C': self.parse_order_cancelled,
            b'P': self.parse_trade,
            b'S': self.parse_system_event,
            b'R': self.parse_stock_directory,
        }

        parser = parsers.get(msg_type)
        if parser:
            result = parser(data)
        else:
            result = {
                'type': f'UNKNOWN({msg_type})',
                'raw_size': len(data)
            }

        elapsed = time.time_ns() - start
        result['parse_time_ns'] = elapsed
        return result


def create_test_messages() -> List[bytes]:
    """Generate sample ITCH binary messages for all implemented types."""
    messages = []

    # Add Order: BUY 100 AAPL @ 150.2500
    messages.append(struct.pack('!c q q c I 8s I',
        b'A', 1000000000, 1001, b'B', 100, b'AAPL    ', 1502500))

    # Add Order: SELL 50 MSFT @ 380.5000
    messages.append(struct.pack('!c q q c I 8s I',
        b'A', 1000001000, 1002, b'S', 50, b'MSFT    ', 3805000))

    # Order Executed: 100 shares of order 1001
    messages.append(struct.pack('!c q q I q',
        b'E', 1000002000, 1001, 100, 5001))

    # Order Cancelled: 25 shares of order 1002
    messages.append(struct.pack('!c q q I',
        b'C', 1000003000, 1002, 25))

    # Replace Order: order 1002 -> new ref 1003, 50 shares @ 381.0000
    messages.append(struct.pack('!c q q q I I',
        b'U', 1000004000, 1002, 1003, 50, 3810000))

    # Trade: BUY 100 AAPL @ 150.2500
    messages.append(struct.pack('!c q q c I 8s I q',
        b'P', 1000005000, 1001, b'B', 100, b'AAPL    ', 1502500, 5001))

    # System Event: Start of market hours
    messages.append(struct.pack('!c q c',
        b'S', 1000006000, b'Q'))

    # Stock Directory: AAPL on NASDAQ
    messages.append(struct.pack('!c q 8s c',
        b'R', 1000007000, b'AAPL    ', b'Q'))

    # Delete Order
    messages.append(struct.pack('!c q q',
        b'D', 1000008000, 1002))

    return messages


def main() -> None:
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
