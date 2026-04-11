# Order Management System

Full order lifecycle management with pre-trade risk checks.

## Features
- Order states: NEW → SENT → FILLED/PARTIAL/CANCELLED/REJECTED
- Pre-trade risk: position limits, order value caps
- Position tracking with average price
- Realised P&L calculation

## Risk Checks
- Max order value (rejects if exceeded)
- Max position size (rejects if exceeded)

## Performance
- ~30K orders/sec with logging (Python)

## Run
```bash
python3 oms.py
```
