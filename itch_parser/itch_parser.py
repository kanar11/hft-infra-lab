#!/usr/bin/env python3
"""
NASDAQ ITCH 5.0 Binary Protocol Parser

Parses all 8 ITCH message types: ADD_ORDER, ADD_ORDER_MPID, DELETE_ORDER,
REPLACE_ORDER, ORDER_EXECUTED, ORDER_CANCELLED, TRADE, SYSTEM_EVENT,
and STOCK_DIRECTORY. Used for processing direct market data feeds.

Parsuje wszystkie 8 typów wiadomości ITCH: ADD_ORDER, ADD_ORDER_MPID, DELETE_ORDER,
REPLACE_ORDER, ORDER_EXECUTED, ORDER_CANCELLED, TRADE, SYSTEM_EVENT
i STOCK_DIRECTORY. Używane do przetwarzania bezpośrednich kanałów danych rynkowych.
"""
# The 'struct' module reads raw binary data and converts it to Python values
# Like Linux tools 'xxd' or 'od' that show hex/binary data - struct unpacks it into numbers
import struct
import time
import os
import logging
from typing import Dict, List, Any, Optional

logger = logging.getLogger('itch')


# A class is like a folder of related commands and data that work together
# Think of it like a system tool that has multiple functions (parse_add_order, parse_delete_order, etc.)
class ITCHMessage:
    """NASDAQ ITCH 5.0 protocol parser (simplified).
    Parser protokołu NASDAQ ITCH 5.0 (uproszczony)."""

    # Class variable: shared by ALL instances of ITCHMessage (like a global variable)
    # All copies of ITCHMessage use this same dictionary - not individual copies
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

    # @staticmethod: this function doesn't need 'self' (the object reference)
    # Like a standalone script that doesn't depend on the class - it's self-contained
    @staticmethod
    def parse_add_order(data: bytes) -> Dict[str, Any]:
        """Parse Add Order (A) message.
        Parsuje wiadomość Add Order (A).
        Fields: msg_type(1) + timestamp(8) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4) = 34 bytes
        """
        if len(data) < 34:
            return {'type': 'ERROR', 'reason': f'ADD_ORDER too short ({len(data)} < 34)'}
        # Format string: '!c q q c I 8s I'
        # ! = network byte order (big-endian: most significant byte first, like reading left-to-right)
        # c = char (1 byte, single character)
        # q = 8-byte integer (64-bit number, like 'long' in C)
        # I = 4-byte unsigned integer (32-bit unsigned number)
        # 8s = 8-byte string (fixed-length text, 8 characters)
        # So: 1 char + 8-byte int + 8-byte int + 1 char + 4-byte int + 8-byte string + 4-byte int = 34 bytes total
        fmt = '!c q q c I 8s I'
        fields = struct.unpack(fmt, data[:34])
        return {
            'type': 'ADD_ORDER',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            # b'B' is a bytes literal: the 'b' prefix means raw binary data, not text
            # It represents the actual byte value of the ASCII character 'B' (0x42)
            'side': 'BUY' if fields[3] == b'B' else 'SELL',
            'shares': fields[4],
            # .decode('ascii', errors='replace'): convert bytes to text string
            # 'ascii' means use ASCII character mapping (standard 7-bit text)
            # errors='replace': if there's a bad byte, replace it with '?' instead of crashing
            # .strip(): remove whitespace from both ends (like 'AAPL    ' becomes 'AAPL')
            'stock': fields[5].decode('ascii', errors='replace').strip(),
            'price': fields[6] / 10000.0
        }

    # @staticmethod: standalone function, doesn't need 'self'
    @staticmethod
    def parse_delete_order(data: bytes) -> Dict[str, Any]:
        """Parse Delete Order (D) message.
        Parsuje wiadomość Delete Order (D).
        Fields: msg_type(1) + timestamp(8) + order_ref(8) = 17 bytes
        """
        if len(data) < 17:
            return {'type': 'ERROR', 'reason': f'DELETE_ORDER too short ({len(data)} < 17)'}
        fmt = '!c q q'
        fields = struct.unpack(fmt, data[:17])
        return {
            'type': 'DELETE_ORDER',
            'timestamp_ns': fields[1],
            'order_ref': fields[2]
        }

    # @staticmethod: standalone function, doesn't need 'self'
    @staticmethod
    def parse_replace_order(data: bytes) -> Dict[str, Any]:
        """Parse Replace Order (U) message.
        Parsuje wiadomość Replace Order (U).
        Fields: msg_type(1) + timestamp(8) + orig_ref(8) + new_ref(8) + shares(4) + price(4) = 33 bytes
        """
        if len(data) < 33:
            return {'type': 'ERROR', 'reason': f'REPLACE_ORDER too short ({len(data)} < 33)'}
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

    # @staticmethod: standalone function, doesn't need 'self'
    @staticmethod
    def parse_order_executed(data: bytes) -> Dict[str, Any]:
        """Parse Order Executed (E) message.
        Parsuje wiadomość Order Executed (E).
        Fields: msg_type(1) + timestamp(8) + order_ref(8) + shares(4) + match_number(8) = 29 bytes
        """
        if len(data) < 29:
            return {'type': 'ERROR', 'reason': f'ORDER_EXECUTED too short ({len(data)} < 29)'}
        fmt = '!c q q I q'
        fields = struct.unpack(fmt, data[:29])
        return {
            'type': 'ORDER_EXECUTED',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'shares': fields[3],
            'match_number': fields[4]
        }

    # @staticmethod: standalone function, doesn't need 'self'
    @staticmethod
    def parse_order_cancelled(data: bytes) -> Dict[str, Any]:
        """Parse Order Cancelled (C) message.
        Parsuje wiadomość Order Cancelled (C).
        Fields: msg_type(1) + timestamp(8) + order_ref(8) + cancelled_shares(4) = 21 bytes
        """
        if len(data) < 21:
            return {'type': 'ERROR', 'reason': f'ORDER_CANCELLED too short ({len(data)} < 21)'}
        fmt = '!c q q I'
        fields = struct.unpack(fmt, data[:21])
        return {
            'type': 'ORDER_CANCELLED',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'cancelled_shares': fields[3]
        }

    # @staticmethod: standalone function, doesn't need 'self'
    @staticmethod
    def parse_trade(data: bytes) -> Dict[str, Any]:
        """Parse Trade (P) message.
        Parsuje wiadomość Trade (P).
        Fields: msg_type(1) + timestamp(8) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4) + match(8) = 42 bytes
        """
        if len(data) < 42:
            return {'type': 'ERROR', 'reason': f'TRADE too short ({len(data)} < 42)'}
        fmt = '!c q q c I 8s I q'
        fields = struct.unpack(fmt, data[:42])
        return {
            'type': 'TRADE',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'side': 'BUY' if fields[3] == b'B' else 'SELL',
            'shares': fields[4],
            # .decode('ascii', errors='replace'): convert bytes to text string
            # .strip(): remove whitespace from both ends
            'stock': fields[5].decode('ascii', errors='replace').strip(),
            'price': fields[6] / 10000.0,
            'match_number': fields[7]
        }

    # @staticmethod: standalone function, doesn't need 'self'
    @staticmethod
    def parse_system_event(data: bytes) -> Dict[str, Any]:
        """Parse System Event (S) message.
        Parsuje wiadomość System Event (S).
        Fields: msg_type(1) + timestamp(8) + event_code(1) = 10 bytes
        """
        if len(data) < 10:
            return {'type': 'ERROR', 'reason': f'SYSTEM_EVENT too short ({len(data)} < 10)'}
        # This is a dictionary: maps bytes keys (b'O', b'S', etc.) to string values
        # Like creating a lookup table or translation table in shell using associative arrays
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

    # @staticmethod: standalone function, doesn't need 'self'
    @staticmethod
    def parse_stock_directory(data: bytes) -> Dict[str, Any]:
        """Parse Stock Directory (R) message.
        Parsuje wiadomość Stock Directory (R).
        Fields: msg_type(1) + timestamp(8) + stock(8) + market_category(1) = 18 bytes
        """
        if len(data) < 18:
            return {'type': 'ERROR', 'reason': f'STOCK_DIRECTORY too short ({len(data)} < 18)'}
        fmt = '!c q 8s c'
        fields = struct.unpack(fmt, data[:18])
        return {
            'type': 'STOCK_DIRECTORY',
            'timestamp_ns': fields[1],
            # .decode('ascii', errors='replace'): convert bytes to text string
            # .strip(): remove whitespace from both ends
            'stock': fields[2].decode('ascii', errors='replace').strip(),
            # .decode without .strip() - keep whitespace as-is
            'market_category': fields[3].decode('ascii', errors='replace')
        }

    def parse(self, data: bytes) -> Dict[str, Any]:
        """Parse any ITCH message by type byte.
        Parsuje dowolną wiadomość ITCH na podstawie bajtu typu."""
        start = time.time_ns()
        msg_type = data[0:1]

        # This is a dictionary mapping byte keys to function objects (methods)
        # Used to dispatch (route) to the correct parsing function based on message type
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
    """Generate sample ITCH binary messages for all implemented types.
    Generuje przykładowe wiadomości binarne ITCH dla wszystkich zaimplementowanych typów."""
    # List: ordered collection of items (like an array in shell)
    messages = []

    # Add Order: BUY 100 AAPL @ 150.2500
    # .append(): add an item to the end of the list (like pushing to shell array)
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
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
    from config_loader import setup_logging
    setup_logging()

    logger.info("=== NASDAQ ITCH 5.0 Parser ===")

    parser = ITCHMessage()
    messages = create_test_messages()

    total_ns = 0
    # for loop: iterate over each item in messages list
    # enumerate(): provides both index (i) and value (raw) - like 'for i in "${!array[@]}"' in bash
    for i, raw in enumerate(messages):
        result = parser.parse(raw)
        parse_ns = result.pop('parse_time_ns')
        total_ns += parse_ns

        logger.info(f"Message {i+1} ({len(raw)} bytes):")
        # for loop: iterate over dictionary items
        # .items(): returns pairs of (key, value) - like iterating over associative array in bash
        for k, v in result.items():
            logger.info(f"  {k}: {v}")
        logger.info(f"  parse_time: {parse_ns} ns")

    avg = total_ns // len(messages)
    logger.info(f"Average parse time: {avg} ns")
    logger.info(f"Throughput: ~{1_000_000_000 // avg:,} messages/sec (theoretical)")


if __name__ == '__main__':
    main()
