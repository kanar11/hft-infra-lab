/*
 * WsClient — minimalny klient WebSocket (RFC 6455).
 *
 * Czysto edukacyjna implementacja:
 *   - tylko plain ws://  (production wss:// wymaga TLS / OpenSSL)
 *   - brak maskingu po stronie klienta (większość produkcyjnych serwerów
 *     wymusza masking; ten klient pracuje z mock_ws_server poniżej)
 *   - tylko text frames + auto-odpowiedź na ping (pong) + obsługa close
 *   - brak fragmentacji (FIN zawsze 1)
 *
 * Po co własna impl skoro istnieje libwebsocketspp / Boost.Beast?
 * Bo to **lab edukacyjny** — pokazuje strukturę protokołu (HTTP upgrade,
 * frame header, opcode, length encoding). Production HFT i tak nie używa
 * WebSocket'ów do market data (zbyt wolne), za to crypto exchange'y tak —
 * więc warto wiedzieć jak to działa.
 *
 * Format ramki (RFC 6455 §5.2):
 *
 *    0               1               2               3
 *    0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 *   +-+-+-+-+-------+-+-------------+-------------------------------+
 *   |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 *   |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 *   |N|V|V|V|       |S|             |   (if payload len == 126/127) |
 *   | |1|2|3|       |K|             |                               |
 *   +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 *
 * Opcody: 0x1=text, 0x2=binary, 0x8=close, 0x9=ping, 0xA=pong.
 * Payload len: 0-125 inline, 126 → next 2 bytes (uint16), 127 → next 8 (uint64).
 * Mask bit + 4-byte mask key: tylko gdy klient→server (zawsze 1) i opcjonalnie
 * server→klient (zwykle 0).
 */
#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>


namespace feed {

enum class WsOpcode : std::uint8_t {
    CONTINUATION = 0x0,
    TEXT         = 0x1,
    BINARY       = 0x2,
    CLOSE        = 0x8,
    PING         = 0x9,
    PONG         = 0xA,
};


class WsClient {
    int fd_ = -1;

public:
    WsClient() = default;
    ~WsClient() { close(); }

    WsClient(const WsClient&)            = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&&)                 = delete;
    WsClient& operator=(WsClient&&)      = delete;

    // connect: TCP connect + HTTP upgrade handshake. Zwraca false na każdy błąd.
    // host/port wskazują endpoint, path to ścieżka w URL'u (np. "/" lub "/ws/btcusdt@trade").
    bool connect(const char* host, int port, const char* path = "/") noexcept {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<std::uint16_t>(port));
        if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            // Spróbuj DNS dla nazw hostów (np. "localhost")
            hostent* he = ::gethostbyname(host);
            if (!he) { close(); return false; }
            std::memcpy(&addr.sin_addr, he->h_addr, he->h_length);
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }

        // HTTP upgrade. Sec-WebSocket-Key powinien być losowy 16B base64;
        // dla demo wystarczy stała wartość — i tak nie weryfikujemy odpowiedzi.
        char req[512];
        const int n = std::snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n",
            path, host, port);
        if (::send(fd_, req, static_cast<std::size_t>(n), MSG_NOSIGNAL) != n) {
            close();
            return false;
        }

        // Czekaj na "HTTP/1.1 101 Switching Protocols" + nagłówki + pusty wiersz.
        char resp[1024];
        std::size_t got = 0;
        while (got < sizeof(resp) - 1) {
            ssize_t r = ::recv(fd_, resp + got, sizeof(resp) - 1 - got, 0);
            if (r <= 0) { close(); return false; }
            got += static_cast<std::size_t>(r);
            resp[got] = '\0';
            if (std::strstr(resp, "\r\n\r\n")) break;  // nagłówki zakończone
        }
        if (std::strncmp(resp, "HTTP/1.1 101", 12) != 0) {
            close();
            return false;
        }
        return true;
    }

    // recv_text: odbierz jedną wiadomość text. Zwraca długość payloadu (>0)
    // albo 0 na close / EOF, -1 na błąd. Pomijaj ping (odeślij pong) i
    // przechwytuj close cleanly.
    int recv_text(char* out, std::size_t max_len) noexcept {
        for (;;) {
            std::uint8_t h[2];
            if (!recv_all(h, 2)) return -1;

            const bool         fin    = (h[0] & 0x80) != 0;
            const WsOpcode     opcode = static_cast<WsOpcode>(h[0] & 0x0F);
            const bool         masked = (h[1] & 0x80) != 0;
            std::uint64_t      len    = h[1] & 0x7F;
            (void)fin;  // demo nie obsługuje fragmentacji

            if (len == 126) {
                std::uint8_t ext[2];
                if (!recv_all(ext, 2)) return -1;
                len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
            } else if (len == 127) {
                std::uint8_t ext[8];
                if (!recv_all(ext, 8)) return -1;
                len = 0;
                for (int i = 0; i < 8; ++i)
                    len = (len << 8) | ext[i];
            }
            std::uint8_t mask_key[4]{};
            if (masked && !recv_all(mask_key, 4)) return -1;

            // Limit żeby nie czytać megabajtów do buforu klienta.
            if (len > max_len) return -1;

            if (!recv_all(reinterpret_cast<std::uint8_t*>(out), len)) return -1;
            if (masked) {
                for (std::uint64_t i = 0; i < len; ++i) out[i] ^= mask_key[i & 3];
            }

            switch (opcode) {
            case WsOpcode::TEXT:
            case WsOpcode::BINARY:
                return static_cast<int>(len);
            case WsOpcode::PING:
                // Odeślij pong z tym samym payloadem.
                send_frame(WsOpcode::PONG, out, static_cast<std::size_t>(len));
                continue;
            case WsOpcode::PONG:
                continue;  // ignore
            case WsOpcode::CLOSE:
                send_frame(WsOpcode::CLOSE, out, static_cast<std::size_t>(len));
                return 0;
            case WsOpcode::CONTINUATION:
                continue;
            }
        }
    }

    // send_text: wyślij text frame. Bez maskingu (mock_ws_server akceptuje).
    bool send_text(const char* msg, std::size_t len) noexcept {
        return send_frame(WsOpcode::TEXT, msg, len);
    }

    bool is_open() const noexcept { return fd_ >= 0; }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    int raw_fd() const noexcept { return fd_; }

private:
    bool recv_all(std::uint8_t* dst, std::uint64_t n) noexcept {
        std::uint64_t got = 0;
        while (got < n) {
            ssize_t r = ::recv(fd_, dst + got, n - got, 0);
            if (r <= 0) return false;
            got += static_cast<std::uint64_t>(r);
        }
        return true;
    }

    bool send_frame(WsOpcode opcode, const void* payload, std::size_t len) noexcept {
        std::uint8_t hdr[10];
        std::size_t  hdr_len = 2;
        hdr[0] = 0x80 | static_cast<std::uint8_t>(opcode);  // FIN=1 + opcode
        if (len < 126) {
            hdr[1] = static_cast<std::uint8_t>(len);
        } else if (len <= 0xFFFF) {
            hdr[1] = 126;
            hdr[2] = static_cast<std::uint8_t>((len >> 8) & 0xFF);
            hdr[3] = static_cast<std::uint8_t>(len & 0xFF);
            hdr_len = 4;
        } else {
            hdr[1] = 127;
            for (int i = 0; i < 8; ++i)
                hdr[2 + i] = static_cast<std::uint8_t>((len >> (8 * (7 - i))) & 0xFF);
            hdr_len = 10;
        }
        if (::send(fd_, hdr, hdr_len, MSG_NOSIGNAL) != static_cast<ssize_t>(hdr_len)) return false;
        if (len > 0 && ::send(fd_, payload, len, MSG_NOSIGNAL) != static_cast<ssize_t>(len))
            return false;
        return true;
    }
};

}  // namespace feed
