# Live WebSocket feed

Czwarte źródło danych dla pipeline'u labu — obok syntetycznego LCG
(`simulator/`), real-data LOBSTER CSV (`replay/`) i TCP FIX over epoll
(`network/`).

WebSocket to dziś standard dla crypto market data (Binance, Coinbase,
Kraken) i części retail equity feedów. Production HFT na regulowanych
giełdach używa raczej UDP multicast / kernel bypass, ale klient WS bywa
przydatny do prototypów, monitoringu i strategii crypto.

## Co tu jest

- **`ws_client.hpp`** — minimalny klient RFC 6455 (~210 linii):
  - TCP connect + HTTP/1.1 upgrade handshake
  - parsing ramki: FIN, opcode, mask bit, payload length (inline / 16-bit / 64-bit)
  - auto pong na ping, clean close, czyste odbieranie text/binary
  - **plain ws:// only** (production wss:// wymagałby OpenSSL — celowo poza scope)

- **`feed_demo.cpp`** — self-contained demo, oba końce w jednym binarce:
  - thread A: mock WebSocket server na `127.0.0.1:19998`
    - akceptuje HTTP upgrade (bez weryfikacji `Sec-WebSocket-Accept` — to lab)
    - emituje 50 trade'ów w formacie Binance: `{"e":"trade","s":"BTCUSDT","p":"42000.50","q":"0.5","t":1000}`
  - thread B: `WsClient` łączy się przez `feed::WsClient::connect()`,
    czyta każdą ramkę, parsuje cenę przez `sscanf`, liczy received
  - exit code 0 gdy received == 50

## Uruchomienie

```bash
make build
./feed/feed_demo
```

Spodziewany output:

```
=== feed_demo (WebSocket) ===
  sent by mock : 50
  received     : 50
  last price   : 42049.50
```

W CI wpięte jako `timeout 10 ./feed/feed_demo` — nie wymaga zewnętrznej
sieci, deterministyczne.

## Format ramki WS (RFC 6455 §5.2)

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

Opcody używane w demo:

| Hex | Opcode | Kierunek          | Obsługa w WsClient                  |
|-----|--------|-------------------|-------------------------------------|
| 0x1 | TEXT   | server → client   | zwracany przez `recv_text`         |
| 0x2 | BINARY | server → client   | zwracany przez `recv_text`         |
| 0x8 | CLOSE  | obustronnie       | odsyła CLOSE, zwraca 0              |
| 0x9 | PING   | server → client   | auto-PONG, kontynuuje pętlę read    |
| 0xA | PONG   | server → client   | ignorowane                          |

Payload length encoding:

| len byte | znaczenie                                          |
|----------|----------------------------------------------------|
| 0–125    | długość inline                                     |
| 126      | następne 2 bajty = uint16 BE                       |
| 127      | następne 8 bajtów = uint64 BE                      |

Mask bit + 4-bajtowy mask_key: klient → server *zawsze* maskuje (RFC),
server → klient zwykle nie. Nasz demo serwer nie maskuje, klient też
nie (mock_server akceptuje); produkcyjne giełdy odrzuciłyby unmasked
client frame jako protocol violation.

## Po co własna impl skoro istnieje libwebsockets / Boost.Beast?

To lab edukacyjny — celem jest pokazanie *struktury* protokołu (HTTP
upgrade, frame header, opcode, length encoding), a nie produkcyjne
wsparcie dla extensions, fragmentation, wss://, compression itd.
Cały kod mieści się w ~210 linijach i jest czytelny w jednym posiedzeniu.

W realnym projekcie crypto-HFT podmieniłbym `WsClient` na:
- **Boost.Beast** — jeśli zostajesz na C++ i potrzebujesz wss:// + TLS
- **libwebsockets** — jeśli chcesz event-loop friendly + extensions
- **uWebSockets** — jeśli liczy się każda nanosekunda (najszybsza lib WS w benchmarkach)

## Połączenie do prawdziwej giełdy

Klient działa z każdym serwerem ws:// który nie wymaga maskingu od klienta.
Dla prawdziwego Binance (`stream.binance.com:9443` — wss://!) potrzebny
byłby TLS handshake przed upgrade'm i masking ramek client→server.
Wystarczy zamienić `connect()` na wariant SSL_*, dodać masking w
`send_frame()` (random 4-byte key + XOR payloadu) — oba zmieszczą się
w ~50 dodatkowych liniach.
