# Order Management System / System zarządzania zamówieniami

Full order lifecycle management with pre-trade risk checks.

*Pełne zarządzanie cyklem życia zamówienia z kontrolami ryzyka przed transakcją.*

## Features / Funkcje
- Order states: NEW → SENT → FILLED/PARTIAL/CANCELLED/REJECTED
- Pre-trade risk: position limits, order value caps
- Position tracking with average price
- Realised P&L calculation

## Risk Checks / Kontrole ryzyka
- Max order value (rejects if exceeded)
- Max position size (rejects if exceeded)

## Performance / Wydajność (Red Hat EL10, VirtualBox 2-core VM)
- 35K orders/sec (submit + fill cycle, Python 3.12)
- 28µs per order with risk checks and P&L tracking

*35K zamówień/s (cykl przesłania + wypełnienia, Python 3.12)*
*28µs na zamówienie z kontrolami ryzyka i śledzeniem P&L*

## Run / Uruchomienie
```bash
python3 oms.py
```
