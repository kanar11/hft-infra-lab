# FIX 4.2 Protocol Parser

Parses Financial Information eXchange messages for order routing.

## Performance

| Metric | C++ |
|--------|-----|
| **Throughput** | **5.5M msg/sec** |
| **Latency (p50)** | **150 ns** |
| **Latency (p99)** | **250 ns** |

## Supported Message Types

- D = New Order
- G = Modify
- F = Cancel
- 8 = Execution Report
- 0 = Heartbeat

## Key Tags

- 8  = BeginString (ALWAYS first)
- 9  = BodyLength (ALWAYS second)
- 35 = MsgType
- 55 = Symbol
- 54 = Side (1=Buy, 2=Sell)
- 44 = Price
- 38 = OrderQty
- 10 = CheckSum (ALWAYS last)

## Session validation (industry standard)

Every compliant FIX engine MUST validate the mandatory fields — earlier the
parser did not check them. Now:

| What | Method | Description |
|------|--------|-------------|
| **SOH delimiter** | auto | Real wire FIX uses `\x01` (ASCII SOH), not `\|`. The parser auto-detects: SOH if present, otherwise `\|` (human-readable for logs/tests). |
| **CheckSum (tag 10)** | `checksum_valid()` | Modulo-256 sum of all bytes up to the delimiter before `10=`. Detects transmission corruption. |
| **BodyLength (tag 9)** | `body_length_valid()` | Number of body bytes (from the field after tag 9 to the delimiter before tag 10). |
| **Required header** | `has_required_header()` | Presence of tags 8, 9, 35, 10. |
| **Full validation** | `is_valid()` | header + checksum + bodylength. |

Building valid messages (with auto-computed tags 9 and 10):

```cpp
char msg[256];
FIXMessage::build_message(msg, sizeof(msg),
    "35=D\x01" "55=AAPL\x01" "54=1\x01" "44=150.25\x01" "38=100\x01");
// → "8=FIX.4.2<SOH>9=<len><SOH>35=D<SOH>...<SOH>10=<checksum><SOH>"

FIXMessage m;
m.parse(msg);
if (m.is_valid()) { /* accept the order */ }
```

## Files

| File | Description |
|------|-------------|
| `fix_parser.hpp` | C++ header-only — parser + CheckSum/BodyLength validation + builder |
| `fix_demo.cpp` | C++ demo with 31 unit tests (incl. checksum/bodylength/SOH) + throughput benchmark |

## Run

```bash
# C++ (build + run)
make build
./fix-protocol/fix_demo              # tests + benchmark (1M parses)
./fix-protocol/fix_demo 5000000      # 5M parses
```
