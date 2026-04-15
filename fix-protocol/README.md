# FIX 4.2 Protocol Parser

Parses Financial Information eXchange messages for order routing.

*Analizuje komunikaty Financial Information eXchange do routingu zamówień.*

## Supported Message Types / Obsługiwane typy wiadomości
- D = New Order
- G = Modify
- F = Cancel
- 8 = Execution Report
- 0 = Heartbeat

## Key Tags / Kluczowe znaczniki
- 35 = MsgType
- 55 = Symbol
- 54 = Side (1=Buy, 2=Sell)
- 44 = Price
- 38 = OrderQty

## Performance / Wydajność
- ~4-7μs parse time per message (Python)

*~4-7μs czas parsowania na wiadomość (Python)*

## Run / Uruchomienie
```bash
