# FIX 4.2 Protocol Parser

Parses Financial Information eXchange messages for order routing.

## Supported Message Types
- D = New Order
- G = Modify
- F = Cancel
- 8 = Execution Report
- 0 = Heartbeat

## Key Tags
- 35 = MsgType
- 55 = Symbol
- 54 = Side (1=Buy, 2=Sell)
- 44 = Price
- 38 = OrderQty

## Performance
- ~4-7μs parse time per message (Python)

## Run
```bash
