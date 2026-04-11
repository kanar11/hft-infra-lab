# NASDAQ ITCH 5.0 Binary Protocol Parser

Parses binary market data feed messages from NASDAQ.

## Supported Messages
- A = Add Order
- D = Delete Order
- U = Replace Order
- E = Order Executed
- P = Trade

## Performance
- ~258K messages/sec (Python)
- ~3-5μs parse time per message

## ITCH vs FIX
- ITCH: binary, receive market data FROM exchange
- FIX: text, send orders TO exchange

## Run
```bash
python3 itch_parser.py
```

