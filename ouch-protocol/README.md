# NASDAQ OUCH 4.2 Protocol

Binary order entry protocol for sending orders TO the exchange.

*Binarny protokół wprowadzania zamówień do wysyłania zamówień NA giełdę.*

## Message Types / Typy wiadomości
- O = Enter Order (buy/sell with price, qty, TIF)
- X = Cancel Order
- U = Replace Order (modify price/qty)

## Response Types / Typy odpowiedzi
- A = Accepted
- C = Cancelled
- E = Executed

## Performance / Wydajność
- ~575 ns/msg encoding
- ~1.7M msg/sec throughput

*~575 ns/msg kodowanie*
*~1,7M msg/s przepustowość*

## ITCH vs OUCH vs FIX
- ITCH: binary, receive market data FROM exchange
- OUCH: binary, send orders TO exchange
- FIX: text-based, both directions (slower)

*ITCH: binarne, odbieranie danych rynkowych Z giełdy*
*OUCH: binarne, wysyłanie zamówień NA giełdę*
*FIX: tekstowe, oba kierunki (wolniej)*

## Run / Uruchomienie
```bash
python3 ouch_sender.py
```
