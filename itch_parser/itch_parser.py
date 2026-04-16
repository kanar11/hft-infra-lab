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
    """NASDAQ ITCH 5.0 protocol parser — all 9 message types.
    Parser protokołu NASDAQ ITCH 5.0 — wszystkie 9 typów wiadomości."""

    # Class variable: shared by ALL instances of ITCHMessage (like a global variable)
    # All copies of ITCHMessage use this same dictionary - not individual copies
    MSG_TYPES = {
        b'A': 'ADD_ORDER',
        b'F': 'ADD_ORDER_MPID',
        b'D': 'DELETE_ORDER',
        b'U': 'REPLACE_ORDER',
        b'E': 'ORDER_EXECUTED',
        b'C': 'ORDER_CANCELLED',
        b'P': 'TRADE',
        b'S': 'SYSTEM_EVENT',
        b'R': 'STOCK_DIRECTORY',
    }

    # Pre-compiled struct formats — struct.Struct compiles the format once at class load time
    # Wstępnie skompilowane formaty struct — kompilacja formatu raz przy ładowaniu klasy
    # This avoids re-parsing the format string on every call (faster by ~30%)
    # Unika ponownego parsowania ciągu formatu przy każdym wywołaniu (szybciej o ~30%)
    _FMT_ADD_ORDER       = struct.Struct('!c q q c I 8s I')       # 34 bytes
    _FMT_ADD_ORDER_MPID  = struct.Struct('!c q q c I 8s I 4s')   # 38 bytes
    _FMT_DELETE_ORDER    = struct.Struct('!c q q')                 # 17 bytes
    _FMT_REPLACE_ORDER   = struct.Struct('!c q q q I I')           # 33 bytes
    _FMT_ORDER_EXECUTED  = struct.Struct('!c q q I q')             # 29 bytes
    _FMT_ORDER_CANCELLED = struct.Struct('!c q q I')               # 21 bytes
    _FMT_TRADE           = struct.Struct('!c q q c I 8s I q')     # 42 bytes
    _FMT_SYSTEM_EVENT    = struct.Struct('!c q c')                 # 10 bytes
    _FMT_STOCK_DIRECTORY = struct.Struct('!c q 8s c')              # 18 bytes

    # @staticmethod: this function doesn't need 'self' (the object reference)
    # Like a standalone script that doesn't depend on the class - it's self-contained
    @staticmethod
    def parse_add_order(data: bytes) -> Dict[str, Any]:
        """Parse Add Order (A) message — 34 bytes.
        Parsuje wiadomość Add Order (A) — 34 bajty.
        Layout: msg_type(1) + timestamp(8) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4)
        """
        if len(data) < 34:
            return {'type': 'ERROR', 'reason': f'ADD_ORDER too short ({len(data)} < 34)'}
        # unpack_from: uses pre-compiled format (faster than struct.unpack with string)
        # unpack_from: używa wstępnie skompilowanego formatu (szybciej niż struct.unpack ze stringiem)
        fields = ITCHMessage._FMT_ADD_ORDER.unpack_from(data)
        return {
            'type': 'ADD_ORDER',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            # b'B' is a bytes literal: the 'b' prefix means raw binary data, not text
            # It represents the actual byte value of the ASCII character 'B' (0x42)
            'side': 'BUY' if fields[3] == b'B' else 'SELL',
            'shares': fields[4],
            # .decode('ascii', errors='replace'): convert bytes to text string
            # .strip(): remove padding whitespace (like 'AAPL    ' becomes 'AAPL')
            'stock': fields[5].decode('ascii', errors='replace').strip(),
            'price': fields[6] / 10000.0
        }

    @staticmethod
    def parse_add_order_mpid(data: bytes) -> Dict[str, Any]:
        """Parse Add Order with MPID (F) message — 38 bytes.
        Parsuje wiadomość Add Order z MPID (F) — 38 bajtów.
        Like ADD_ORDER but includes 4-byte Market Participant ID (who placed the order).
        Jak ADD_ORDER ale zawiera 4-bajtowy identyfikator uczestnika rynku.
        """
        if len(data) < 38:
            return {'type': 'ERROR', 'reason': f'ADD_ORDER_MPID too short ({len(data)} < 38)'}
        fields = ITCHMessage._FMT_ADD_ORDER_MPID.unpack_from(data)
        return {
            'type': 'ADD_ORDER_MPID',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'side': 'BUY' if fields[3] == b'B' else 'SELL',
            'shares': fields[4],
            'stock': fields[5].decode('ascii', errors='replace').strip(),
            'price': fields[6] / 10000.0,
            'mpid': fields[7].decode('ascii', errors='replace').strip()
        }

    @staticmethod
    def parse_delete_order(data: bytes) -> Dict[str, Any]:
        """Parse Delete Order (D) message — 17 bytes.
        Parsuje wiadomość Delete Order (D) — 17 bajtów."""
        if len(data) < 17:
            return {'type': 'ERROR', 'reason': f'DELETE_ORDER too short ({len(data)} < 17)'}
        fields = ITCHMessage._FMT_DELETE_ORDER.unpack_from(data)
        return {
            'type': 'DELETE_ORDER',
            'timestamp_ns': fields[1],
            'order_ref': fields[2]
        }

    @staticmethod
    def parse_replace_order(data: bytes) -> Dict[str, Any]:
        """Parse Replace Order (U) message — 33 bytes.
        Parsuje wiadomość Replace Order (U) — 33 bajty."""
        if len(data) < 33:
            return {'type': 'ERROR', 'reason': f'REPLACE_ORDER too short ({len(data)} < 33)'}
        fields = ITCHMessage._FMT_REPLACE_ORDER.unpack_from(data)
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
        """Parse Order Executed (E) message — 29 bytes.
        Parsuje wiadomość Order Executed (E) — 29 bajtów."""
        if len(data) < 29:
            return {'type': 'ERROR', 'reason': f'ORDER_EXECUTED too short ({len(data)} < 29)'}
        fields = ITCHMessage._FMT_ORDER_EXECUTED.unpack_from(data)
        return {
            'type': 'ORDER_EXECUTED',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'shares': fields[3],
            'match_number': fields[4]
        }

    @staticmethod
    def parse_order_cancelled(data: bytes) -> Dict[str, Any]:
        """Parse Order Cancelled (C) message — 21 bytes.
        Parsuje wiadomość Order Cancelled (C) — 21 bajtów."""
        if len(data) < 21:
            return {'type': 'ERROR', 'reason': f'ORDER_CANCELLED too short ({len(data)} < 21)'}
        fields = ITCHMessage._FMT_ORDER_CANCELLED.unpack_from(data)
        return {
            'type': 'ORDER_CANCELLED',
            'timestamp_ns': fields[1],
            'order_ref': fields[2],
            'cancelled_shares': fields[3]
        }

    @staticmethod
    def parse_trade(data: bytes) -> Dict[str, Any]:
        """Parse Trade (P) message — 42 bytes.
        Parsuje wiadomość Trade (P) — 42 bajty."""
        if len(data) < 42:
            return {'type': 'ERROR', 'reason': f'TRADE too short ({len(data)} < 42)'}
        fields = ITCHMessage._FMT_TRADE.unpack_from(data)
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
        """Parse System Event (S) message — 10 bytes.
        Parsuje wiadomość System Event (S) — 10 bajtów."""
        if len(data) < 10:
            return {'type': 'ERROR', 'reason': f'SYSTEM_EVENT too short ({len(data)} < 10)'}
        # Lookup table: maps event code bytes to human-readable names
        # Tablica odniesień: mapuje bajty kodów zdarzeń na czytelne nazwy
        EVENTS = {b'O': 'START_OF_MESSAGES', b'S': 'START_OF_SYSTEM_HOURS',
                  b'Q': 'START_OF_MARKET_HOURS', b'M': 'END_OF_MARKET_HOURS',
                  b'E': 'END_OF_SYSTEM_HOURS', b'C': 'END_OF_MESSAGES'}
        fields = ITCHMessage._FMT_SYSTEM_EVENT.unpack_from(data)
        return {
            'type': 'SYSTEM_EVENT',
            'timestamp_ns': fields[1],
            'event_code': EVENTS.get(fields[2], f'UNKNOWN({fields[2]})')
        }

    @staticmethod
    def parse_stock_directory(data: bytes) -> Dict[str, Any]:
        """Parse Stock Directory (R) message — 18 bytes.
        Parsuje wiadomość Stock Directory (R) — 18 bajtów."""
        if len(data) < 18:
            return {'type': 'ERROR', 'reason': f'STOCK_DIRECTORY too short ({len(data)} < 18)'}
        fields = ITCHMessage._FMT_STOCK_DIRECTORY.unpack_from(data)
        return {
            'type': 'STOCK_DIRECTORY',
            'timestamp_ns': fields[1],
            'stock': fields[2].decode('ascii', errors='replace').strip(),
            'market_category': fields[3].decode('ascii', errors='replace')
        }

    # Dispatch table: maps message type byte → parser function
    # Tablica dyspozycji: mapuje bajt typu wiadomości → funkcję parsera
    # Defined once at class level (not recreated on every parse call — faster)
    # Zdefiniowana raz na poziomie klasy (nie tworzona przy każdym wywołaniu — szybciej)
    _PARSERS: Dict[bytes, Any] = {
        b'A': parse_add_order,
        b'F': parse_add_order_mpid,
        b'D': parse_delete_order,
        b'U': parse_replace_order,
        b'E': parse_order_executed,
        b'C': parse_order_cancelled,
        b'P': parse_trade,
        b'S': parse_system_event,
        b'R': parse_stock_directory,
    }

    def parse(self, data: bytes) -> Dict[str, Any]:
        """Parse any ITCH message by dispatching on the first byte (message type).
        Parsuje dowolną wiadomość ITCH na podstawie pierwszego bajtu (typu wiadomości)."""
        start = time.time_ns()
        msg_type = data[0:1]

        parser = self._PARSERS.get(msg_type)
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

    # Add Order with MPID: BUY 200 TSLA @ 250.0000 by participant "GSCO" (Goldman Sachs)
    messages.append(struct.pack('!c q q c I 8s I 4s',
        b'F', 1000009000, 1004, b'B', 200, b'TSLA    ', 2500000, b'GSCO'))

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
