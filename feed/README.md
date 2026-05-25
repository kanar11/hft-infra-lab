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

## Co jest TERAZ w `WsClient` (`ws_client.hpp`)

Po ostatniej iteracji klient jest **RFC 6455 compliant** w zakresie tego co
naprawdę potrzeba żeby pogadać z produkcyjnym serwerem WS:

| Feature                          | Status | Szczegóły                                        |
|----------------------------------|--------|--------------------------------------------------|
| Plain `ws://` (no TLS)           | ✅     | TCP connect + HTTP upgrade                       |
| HTTP/1.1 upgrade handshake       | ✅     | bez weryfikacji Sec-WebSocket-Accept (lab)       |
| Sec-WebSocket-Key losowy         | ✅     | 16B z `/dev/urandom`, base64                     |
| Masking client→server (RFC §5.3) | ✅     | losowy 4-byte mask key per ramka, XOR payloadu   |
| Parsing FIN / opcode / mask bit  | ✅     | wszystkie 6 opcodów                              |
| Payload length 7/16/64 bit       | ✅     | inline / 126 / 127                               |
| Ping → auto-pong                 | ✅     | echo payloadu                                    |
| Clean close (opcode 0x8)         | ✅     | echo CLOSE i return 0                            |
| Fragmentacja (FIN=0)             | ❌     | demo zakłada zawsze FIN=1 (Binance też tak robi) |
| WSS (TLS / `wss://`)             | ⚠️     | osobna klasa `WssClient` — zobacz niżej          |
| Compression (permessage-deflate) | ❌     | poza scope                                       |

To wystarczy żeby gadać z każdą publiczną crypto giełdą która używa **`ws://`**.
Większość produkcyjnych giełd wymusza jednak `wss://` (TLS) — patrz dalej.

## Podłączenie do prawdziwej giełdy (Binance, Coinbase, Kraken)

Crypto giełdy publikują streamy WS **przez TLS** (`wss://`). Do tego dorzucamy
`feed/wss_client.hpp` (WsClient nad OpenSSL) i `feed/wss_demo.cpp` — przykładowy
klient łączący się z `stream.binance.com:9443`.

### Krok 1 — zainstaluj OpenSSL development headers

```bash
# Ubuntu / Debian
sudo apt install libssl-dev ca-certificates

# RHEL / Fedora / Rocky Linux
sudo dnf install openssl-devel ca-certificates

# macOS (Homebrew)
brew install openssl@3
```

`ca-certificates` daje systemowy trust store — bez tego TLS nie zweryfikuje
certyfikatu Binance i odrzuci połączenie.

### Krok 2 — zbuduj `wss_demo`

```bash
make wss                                  # target z Makefile
# albo bezpośrednio:
g++ -O2 -std=c++17 -DHFT_USE_OPENSSL -Wall -Wextra -pthread \
    -o feed/wss_demo feed/wss_demo.cpp -lssl -lcrypto
```

Flaga `-DHFT_USE_OPENSSL` jest wymagana — bez niej `wss_client.hpp` rzuca
`#error "wss_client.hpp wymaga -DHFT_USE_OPENSSL"`.

### Krok 3 — uruchom

```bash
# default: 20 trade'ów BTC/USDT z Binance
./feed/wss_demo

# inny endpoint
./feed/wss_demo stream.binance.com 9443 /ws/ethusdt@trade

# więcej trade'ów
./feed/wss_demo stream.binance.com 9443 /ws/btcusdt@trade 100
```

Spodziewany output:

```
=== wss_demo — Binance WebSocket live trades ===
Endpoint: wss://stream.binance.com:9443/ws/btcusdt@trade
Limit:    20 trade events

[connected] TCP + TLS + upgrade: 187.4 ms

  [  1]  price=68234.50    qty=0.00123
  [  2]  price=68234.51    qty=0.04812
  ...
```

### Krok 4 (opcjonalnie) — podpięcie do strategii w labie

`WssClient` ma identyczne API co `WsClient` (`connect`, `recv_text`, `send_text`,
`close`). Możesz go traktować jak drop-in replacement w każdym kodzie który
używa `feed::WsClient`. Przykład: zamiast generatora syntetycznego w
`simulator/market_sim.hpp`, karm strategię prawdziwymi tradesami BTC:

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

    // wyciągnij price z JSON (patrz wss_demo.cpp jak to robi)
    double price = parse_price(buf);
    Signal sig = strat.on_market_data("BTCUSDT", price);
    if (sig.valid) {
        // BUY albo SELL — wyślij do OMS, Risk Manager, etc.
    }
}
```

### Co weryfikujemy w TLS (bezpieczeństwo)

- **Cert chain** — przez systemowy trust store (`SSL_CTX_set_default_verify_paths`)
- **Hostname** — SAN albo CN w certyfikacie musi pasować do host'a (`SSL_set1_host`)
- **SNI** — wysyłamy hostname w handshake (`SSL_set_tlsext_host_name`),
  inaczej Cloudflare zwróciłby domyślny cert i hostname check by failnął
- **TLS 1.2+** — wymuszone (`SSL_CTX_set_min_proto_version`); TLS 1.0/1.1 są deprecated

Bez tych weryfikacji MITM byłby trywialny.

### Limity tej implementacji (jeśli planujesz produkcję)

- **Brak permessage-deflate compression** — Binance wspiera ale opcjonalnie.
  Bez tego dostajesz pełne JSON-y zamiast skompresowanych ~30% mniejszych.
- **Brak ping/pong heartbeats od klienta** — Binance wymaga że klient *odpowiada*
  na PING (już to robimy), ale dla bardzo długich połączeń też klient powinien
  *wysyłać* PING. Trywialne do dorobienia (`send_frame(PING, ...)`) ale nie
  zaimplementowane.
- **Brak reconnect z exponential backoff** — przy disconnect po prostu return -1.
  Twój kod powinien to obsłużyć w pętli.
- **Brak rate-limiting respect** — Binance ma 5 wiadomości/sek limit na
  subskrypcje. Demo nie wysyła nic poza handshake'iem więc OK, ale przy
  multi-stream subscription pamiętaj o tym.

To wszystko mieści się w max 200 dodatkowych liniach jeśli kiedyś chcesz
zrobić "production WsClient".
