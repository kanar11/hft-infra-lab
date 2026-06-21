# Network — epoll TCP server

A minimal, dependency-free Linux `epoll(7)` TCP server scaffold plus a
self-contained FIX-ingestion demo. This is the **TCP source** of the lab's four
feed paths (alongside synthetic LCG in `simulator/`, real LOBSTER CSV in
`replay/`, and the WebSocket client in `feed/`).

## Files

| File | Description |
|------|-------------|
| `epoll_server.hpp` | C++ header-only `net::EpollServer` — non-blocking listen + epoll loop, accept-drain, per-connection read callback |
| `fix_server_demo.cpp` | Self-test: spins up the server, a client thread connects to localhost, sends N FIX-style messages, and verifies every ack returns |

## EpollServer API

```cpp
net::EpollServer server;
server.listen(19999);                 // bind 0.0.0.0:19999, non-blocking + SO_REUSEADDR

server.run([](int fd, const uint8_t* data, size_t n) {
    // called whenever a connection has bytes ready
    net::EpollServer::send_all(fd, "ACK\n", 4);
    return net::Continuation::KEEP;    // or CLOSE to drop this connection
}, /*timeout_ms=*/50);

server.stop();                         // ask the loop to exit (callback / other thread)
server.teardown();                     // close epoll + listen fds (also run by dtor)
```

- **Level-triggered** `EPOLLIN` (simpler to reason about than edge-triggered).
- **Accept drain**: on a listen-socket wakeup it `accept()`s in a loop until
  `EAGAIN`, so a burst of connections isn't queued until the next `epoll_wait`.
- **Move/copy disabled** (owns raw fds); RAII teardown in the destructor.

## Demo: `fix_server_demo`

```bash
make build
./network/fix_server_demo        # prints sent / parsed / acked, exit 0 on full round-trip
```

Both ends run in one process so CI needs no external client. Expected output:

```
fix_server_demo: listening on 127.0.0.1:19999
=== fix_server_demo ===
  messages sent : 50
  parsed by srv : 50
  acks received : 50  (client side)
```

Exit code is 0 only when the server parsed **every** message AND the client
received an ack for **every** one (a verified end-to-end round-trip).

### Framing

FIX on a real wire is variable-length with a `9=BodyLength` field and a
`10=CheckSum` trailer. To keep the demo tight it uses **newline-terminated**
FIX-style strings — one message per line. The server accumulates bytes across
arbitrary `recv()` chunk boundaries and cuts on `\n` before handing each line to
the lab's `FIXMessage` parser. A production engine would frame on tag 9 / tag 10.

## Production gaps (intentional, documented)

- **No outbound write buffering** — `send_all()` assumes `send()` completes in
  full. A real exchange gateway needs a per-fd outbound queue driven by `EPOLLOUT`.
- **Level-triggered, not `EPOLLET`** — edge-triggered is lower-overhead but forces
  callers to drain each fd fully; switch when latency outweighs complexity.
- **No `SO_REUSEPORT`** — single listener (fine for one accept thread / the demo).
- **No TLS** — plaintext TCP; production order entry runs over TLS or a private line.
