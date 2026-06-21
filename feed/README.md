# Live WebSocket feed

A fourth data source for the lab pipeline — alongside the synthetic LCG feed
(`simulator/`), real-data LOBSTER CSV (`replay/`), and TCP FIX over epoll
(`network/`).

WebSocket is today's standard for crypto market data (Binance, Coinbase,
Kraken) and parts of retail equity feeds. Production HFT on regulated
exchanges tends to use UDP multicast / kernel bypass instead, but a WS client
is handy for prototypes, monitoring, and crypto strategies.

## What's here

- **`ws_client.hpp`** — a minimal RFC 6455 client (~210 lines):
  - TCP connect + HTTP/1.1 upgrade handshake
  - frame parsing: FIN, opcode, mask bit, payload length (inline / 16-bit / 64-bit)
  - auto pong on ping, clean close, clean text/binary receive
  - **plain ws:// only** (production wss:// would require OpenSSL — intentionally out of scope)

- **`feed_demo.cpp`** — a self-contained demo, both ends in one binary:
  - thread A: mock WebSocket server on `127.0.0.1:19998`
    - accepts the HTTP upgrade (without verifying `Sec-WebSocket-Accept` — this is a lab)
    - emits 50 trades in Binance format: `{"e":"trade","s":"BTCUSDT","p":"42000.50","q":"0.5","t":1000}`
  - thread B: `WsClient` connects via `feed::WsClient::connect()`,
    reads each frame, parses the price via `sscanf`, counts received messages
  - exit code 0 when received == 50

## Running

```bash
make build
./feed/feed_demo
```

Expected output:

```
=== feed_demo (WebSocket) ===
  sent by mock : 50
  received     : 50
  last price   : 42049.50
```

Wired into CI as `timeout 10 ./feed/feed_demo` — needs no external
network, fully deterministic.

## WS frame format (RFC 6455 §5.2)

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len == 126/127) |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
```

Opcodes used in the demo:

| Hex | Opcode | Direction         | Handling in WsClient                |
|-----|--------|-------------------|-------------------------------------|
| 0x1 | TEXT   | server → client   | returned by `recv_text`             |
| 0x2 | BINARY | server → client   | returned by `recv_text`             |
| 0x8 | CLOSE  | both ways         | echoes CLOSE, returns 0             |
| 0x9 | PING   | server → client   | auto-PONG, continues the read loop  |
| 0xA | PONG   | server → client   | ignored                             |

Payload length encoding:

| len byte | meaning                                            |
|----------|----------------------------------------------------|
| 0–125    | inline length                                      |
| 126      | next 2 bytes = uint16 BE                           |
| 127      | next 8 bytes = uint64 BE                           |

Mask bit + 4-byte mask_key: client → server *always* masks (RFC),
server → client usually does not. Our demo server does not mask, and the
client doesn't either (mock_server accepts that); production exchanges would
reject an unmasked client frame as a protocol violation.

## Why a custom impl when libwebsockets / Boost.Beast exist?

This is an educational lab — the goal is to show the *structure* of the protocol
(HTTP upgrade, frame header, opcode, length encoding), not production-grade
support for extensions, fragmentation, wss://, compression, etc.
The whole thing fits in ~210 lines and is readable in one sitting.

In a real crypto-HFT project you would swap `WsClient` for:
- **Boost.Beast** — if you stay in C++ and need wss:// + TLS
- **libwebsockets** — if you want event-loop-friendly + extensions
- **uWebSockets** — if every nanosecond counts (fastest WS lib in benchmarks)

## What's in `WsClient` NOW (`ws_client.hpp`)

After the latest iteration the client is **RFC 6455 compliant** for what you
actually need to talk to a production WS server:

| Feature                          | Status | Details                                          |
|----------------------------------|--------|--------------------------------------------------|
| Plain `ws://` (no TLS)           | ✅     | TCP connect + HTTP upgrade                       |
| HTTP/1.1 upgrade handshake       | ✅     | without verifying Sec-WebSocket-Accept (lab)     |
| Random Sec-WebSocket-Key         | ✅     | 16B from `/dev/urandom`, base64                  |
| Masking client→server (RFC §5.3) | ✅     | random 4-byte mask key per frame, XOR payload    |
| Parse FIN / opcode / mask bit    | ✅     | all 6 opcodes                                    |
| Payload length 7/16/64 bit       | ✅     | inline / 126 / 127                               |
| Ping → auto-pong                 | ✅     | echoes the payload                               |
| Clean close (opcode 0x8)         | ✅     | echoes CLOSE and returns 0                       |
| Fragmentation (FIN=0)            | ❌     | demo assumes FIN=1 always (Binance does too)     |
| WSS (TLS / `wss://`)             | ⚠️     | separate `WssClient` class — see below           |
| Compression (permessage-deflate) | ❌     | out of scope                                     |

That's enough to talk to any public crypto exchange that uses **`ws://`**.
Most production exchanges, however, require `wss://` (TLS) — see below.

## Connecting to a real exchange (Binance, Coinbase, Kraken)

Crypto exchanges publish WS streams **over TLS** (`wss://`). For that we add
`feed/wss_client.hpp` (WsClient over OpenSSL) and `feed/wss_demo.cpp` — an example
client connecting to `stream.binance.com:9443`.

### Step 1 — install OpenSSL development headers

```bash
# Ubuntu / Debian
sudo apt install libssl-dev ca-certificates

# RHEL / Fedora / Rocky Linux
sudo dnf install openssl-devel ca-certificates

# macOS (Homebrew)
brew install openssl@3
```

`ca-certificates` provides the system trust store — without it TLS cannot verify
Binance's certificate and will reject the connection.

### Step 2 — build `wss_demo`

```bash
make wss                                  # target in the Makefile
# or directly:
g++ -O2 -std=c++17 -DHFT_USE_OPENSSL -Wall -Wextra -pthread \
    -o feed/wss_demo feed/wss_demo.cpp -lssl -lcrypto
```

The `-DHFT_USE_OPENSSL` flag is required — without it `wss_client.hpp` raises
`#error "wss_client.hpp requires -DHFT_USE_OPENSSL"`.

### Step 3 — run

```bash
# default: 20 BTC/USDT trades from Binance
./feed/wss_demo

# different endpoint
./feed/wss_demo stream.binance.com 9443 /ws/ethusdt@trade

# more trades
./feed/wss_demo stream.binance.com 9443 /ws/btcusdt@trade 100
```

Expected output:

```
=== wss_demo — Binance WebSocket live trades ===
Endpoint: wss://stream.binance.com:9443/ws/btcusdt@trade
Limit:    20 trade events

[connected] TCP + TLS + upgrade: 187.4 ms

  [  1]  price=68234.50    qty=0.00123
  [  2]  price=68234.51    qty=0.04812
  ...
```

### Step 4 (optional) — wiring into a lab strategy

`WssClient` has an identical API to `WsClient` (`connect`, `recv_text`, `send_text`,
`close`). You can treat it as a drop-in replacement in any code that uses
`feed::WsClient`. Example: instead of the synthetic generator in
`simulator/market_sim.hpp`, feed the strategy real BTC trades:

```cpp
#include "feed/wss_client.hpp"
#include "strategy/mean_reversion.hpp"

feed::WssClient cli;
cli.connect("stream.binance.com", 9443, "/ws/btcusdt@trade");

MeanReversionStrategy strat(window=20, threshold_pct=0.05);

char buf[2048];
while (true) {
    int n = cli.recv_text(buf, sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = '\0';

    // extract price from the JSON (see wss_demo.cpp for how it does it)
    double price = parse_price(buf);
    Signal sig = strat.on_market_data("BTCUSDT", price);
    if (sig.valid) {
        // BUY or SELL — send to OMS, Risk Manager, etc.
    }
}
```

### What we verify in TLS (security)

- **Cert chain** — via the system trust store (`SSL_CTX_set_default_verify_paths`)
- **Hostname** — the SAN or CN in the certificate must match the host (`SSL_set1_host`)
- **SNI** — we send the hostname in the handshake (`SSL_set_tlsext_host_name`),
  otherwise Cloudflare would return a default cert and the hostname check would fail
- **TLS 1.2+** — enforced (`SSL_CTX_set_min_proto_version`); TLS 1.0/1.1 are deprecated

Without these checks a MITM would be trivial.

### Limits of this implementation (if you plan to go to production)

- **No permessage-deflate compression** — Binance supports it, but optionally.
  Without it you get full JSON payloads instead of ~30%-smaller compressed ones.
- **No client-side ping/pong heartbeats** — Binance requires the client to *respond*
  to a PING (we already do that), but for very long connections the client should
  also *send* PINGs. Trivial to add (`send_frame(PING, ...)`) but not implemented.
- **No reconnect with exponential backoff** — on disconnect it simply returns -1.
  Your code should handle that in a loop.
- **No rate-limiting respect** — Binance has a 5-messages/sec limit on
  subscriptions. The demo sends nothing beyond the handshake so it's fine, but with
  multi-stream subscriptions keep it in mind.

All of that fits in at most ~200 additional lines if you ever want to build a
"production WsClient".
