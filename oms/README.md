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

## Performance (Red Hat EL10, VirtualBox 2-core VM)
- 35K orders/sec (submit + fill cycle, Python 3.12)
- 28µs per order with risk checks and P&L tracking

## Run
```bash
python3 oms.py
```
