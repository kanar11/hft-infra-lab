# NASDAQ ITCH 5.0 Binary Protocol Parser

Parses binary market data feed messages from NASDAQ.

*Analizuje binarne komunikaty źródła danych rynkowych z giełdy NASDAQ.*

## Supported Messages / Obsługiwane wiadomości
- A = Add Order
- D = Delete Order
- U = Replace Order
- E = Order Executed
- P = Trade

## Performance / Wydajność
- ~258K messages/sec (Python)
- ~3-5μs parse time per message

*~258K wiadomości/s (Python)*
*~3-5μs czas parsowania na wiadomość*

## ITCH vs FIX
- ITCH: binary, receive market data FROM exchange
- FIX: text, send orders TO exchange

*ITCH: binarne, odbieranie danych rynkowych Z giełdy*
*FIX: tekstowe, wysyłanie zamówień DO giełdy*

## Run / Uruchomienie
```bash
python3 itch_parser.py
```

