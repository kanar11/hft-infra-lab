# NASDAQ OUCH 4.2 Protocol

Binary order entry protocol for sending orders TO the exchange.

## Message Types
- O = Enter Order (buy/sell with price, qty, TIF)
- X = Cancel Order
- U = Replace Order (modify price/qty)

## Response Types
- A = Accepted
- C = Cancelled
- E = Executed

## Performance
- ~575 ns/msg encoding
- ~1.7M msg/sec throughput

## ITCH vs OUCH vs FIX
- ITCH: binary, receive market data FROM exchange
- OUCH: binary, send orders TO exchange
- FIX: text-based, both directions (slower)

## Run
```bash
python3 ouch_sender.py
```
