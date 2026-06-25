/*
 * WsClient — a minimal WebSocket client (RFC 6455), plain ws://.
 *
 * RFC-compliant functionality:
 *   - HTTP/1.1 upgrade handshake with a random Sec-WebSocket-Key (16B from /dev/urandom)
 *   - masking of client→server frames (a 4-byte mask key per frame, XOR of the payload)
 *   - parsing of all opcodes (TEXT/BINARY/CLOSE/PING/PONG/CONTINUATION)
 *   - payload length 7/16/64 bit
 *   - auto-PONG on PING, clean CLOSE handshake
 *
 * What it does NOT have (out of the lab's scope):
 *   - TLS (wss://) — a separate WssClient in feed/wss_client.hpp, requires OpenSSL
 *   - fragmentation (FIN=0) — production exchanges always send FIN=1
 *   - permessage-deflate compression — optional in the RFC
 *   - verification of the server's Sec-WebSocket-Accept response (we don't protect
 *     against MITM without TLS anyway, so it would be pointless)
 *
 * Why a custom impl when Boost.Beast / libwebsockets exist?
 * Because this is an **educational lab** — it shows the protocol structure (HTTP upgrade,
 * frame header, opcode, length encoding) in ~270 lines. Boost.Beast has
 * tens of thousands of lines and a dependency chain on 200 MB of headers.
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
 * Opcodes: 0x1=text, 0x2=binary, 0x8=close, 0x9=ping, 0xA=pong.
 * Payload len: 0-125 inline, 126 → next 2 bytes (uint16), 127 → next 8 (uint64).
 * Mask bit + 4-byte mask key: only when client→server (always 1) and optionally
 * server→client (usually 0).
 */
#pragma once

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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


// detail — utilities shared by WsClient and WssClient.
// Free functions (not static class members) so they are available from wss_client.hpp
// without exposing them on the class's public API.
namespace detail {

// random_bytes: kernel CSPRNG (/dev/urandom) + a fallback to rand().
// The RFC doesn't require cryptographic strength for mask_key, but urandom is
// available on every Linux/BSD/macOS with no deps, so why not.
inline void random_bytes(std::uint8_t* out, std::size_t n) noexcept {
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        std::size_t got = 0;
        while (got < n) {
            const ssize_t r = ::read(fd, out + got, n - got);
            if (r <= 0) break;
            got += static_cast<std::size_t>(r);
        }
        ::close(fd);
        if (got == n) return;
    }
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<std::uint8_t>(std::rand() & 0xFF);
}

// gen_sec_websocket_key: 16 random bytes → a 24-char base64 (RFC 6455 §4.1).
inline void gen_sec_websocket_key(char out[25]) noexcept {
    std::uint8_t raw[16];
    random_bytes(raw, 16);
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0;
    for (int i = 0; i < 15; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(raw[i]) << 16)
                               | (static_cast<std::uint32_t>(raw[i + 1]) << 8)
                               |  static_cast<std::uint32_t>(raw[i + 2]);
        out[j++] = alphabet[(v >> 18) & 0x3F];
        out[j++] = alphabet[(v >> 12) & 0x3F];
        out[j++] = alphabet[(v >>  6) & 0x3F];
        out[j++] = alphabet[ v        & 0x3F];
    }
    // 16th byte → 2 data chars + 2 '=' padding (16 = 5*3 + 1).
    const std::uint32_t v = static_cast<std::uint32_t>(raw[15]) << 16;
    out[j++] = alphabet[(v >> 18) & 0x3F];
    out[j++] = alphabet[(v >> 12) & 0x3F];
    out[j++] = '=';
    out[j++] = '=';
    out[j] = '\0';
}

}  // namespace detail


class WsClient {
    int fd_ = -1;

public:
    WsClient() = default;
    ~WsClient() { close(); }

    WsClient(const WsClient&)            = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&&)                 = delete;
    WsClient& operator=(WsClient&&)      = delete;

    // connect: TCP connect + HTTP upgrade handshake. Returns false on any error.
    // host/port point at the endpoint, path is the path in the URL (e.g. "/" or "/ws/btcusdt@trade").
    bool connect(const char* host, int port, const char* path = "/") noexcept {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<std::uint16_t>(port));
        if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            // Try DNS for hostnames (e.g. "localhost")
            hostent* he = ::gethostbyname(host);
            if (!he) { close(); return false; }
            std::memcpy(&addr.sin_addr, he->h_addr, he->h_length);
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }

        // HTTP upgrade. The Sec-WebSocket-Key must be fresh on every connection
        // (the RFC requires uniqueness — production exchanges reject a constant value
        // as a replay-attack attempt). We generate 16 random bytes from urandom
        // and encode them as base64.
        char ws_key[25] = {};  // 24 chars + null
        detail::gen_sec_websocket_key(ws_key);

        char req[512];
        const int n = std::snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n",
            path, host, port, ws_key);
        if (::send(fd_, req, static_cast<std::size_t>(n), MSG_NOSIGNAL) != n) {
            close();
            return false;
        }

        // Wait for "HTTP/1.1 101 Switching Protocols" + headers + an empty line.
        //
        // CRITICAL: we read byte-by-byte until CRLF CRLF. TCP is a stream —
        // the server sends the 101 response and IMMEDIATELY after may blast WS frames
        // that land in the same segment/recv buffer. If we read
        // greedily (recv in large chunks), we'd pull part of the WS frames into the
        // handshake buffer and LOSE them — recv_text() would start in the middle of
        // a frame → desync. Byte-by-byte guarantees we stop exactly
        // at \r\n\r\n and not a byte further. The cost (≈150 syscalls) is negligible
        // because the handshake is once per connection.
        char resp[1024];
        std::size_t got = 0;
        while (got < sizeof(resp) - 1) {
            std::uint8_t c;
            ssize_t r = ::recv(fd_, &c, 1, 0);
            if (r <= 0) { close(); return false; }
            resp[got++] = static_cast<char>(c);
            if (got >= 4 && resp[got - 4] == '\r' && resp[got - 3] == '\n' &&
                            resp[got - 2] == '\r' && resp[got - 1] == '\n') {
                break;  // end of headers — the rest of the stream is WS frames
            }
        }
        resp[got] = '\0';
        if (std::strncmp(resp, "HTTP/1.1 101", 12) != 0) {
            close();
            return false;
        }
        return true;
    }

    // recv_text: receive one text message. Returns the payload length (>0)
    // or 0 on close / EOF, -1 on error. Skips ping (sends back a pong) and
    // intercepts close cleanly.
    int recv_text(char* out, std::size_t max_len) noexcept {
        for (;;) {
            std::uint8_t h[2];
            if (!recv_all(h, 2)) return -1;

            const bool         fin    = (h[0] & 0x80) != 0;
            const WsOpcode     opcode = static_cast<WsOpcode>(h[0] & 0x0F);
            const bool         masked = (h[1] & 0x80) != 0;
            std::uint64_t      len    = h[1] & 0x7F;
            (void)fin;  // the demo does not handle fragmentation

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

            // Limit so we don't read megabytes into the client's buffer.
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
                // Send back a pong with the same payload.
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

    // send_text: send a text frame with a mask (RFC 6455 requirement for client→server).
    bool send_text(const char* msg, std::size_t len) noexcept {
        return send_frame(WsOpcode::TEXT, msg, len);
    }

    // send_ping: send a PING frame (client → server). Payload optional;
    // the standard uses a few timestamp/cookie bytes to verify the PONG
    // matches the PING. An empty payload is also legal.
    //
    // Why? Binance/Coinbase CLOSE an idle wss:// after ~3 min without a warning —
    // without periodic PING/PONG the strategy "goes silent" unnoticed.
    // Call it from a timer every 30s (or shorter than the server timeout).
    bool send_ping(const void* payload = nullptr, std::size_t len = 0) noexcept {
        return send_frame(WsOpcode::PING, payload, len);
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

    // send_frame: sends a WS frame with a mask (RFC 6455 §5.3 — the client MUST mask
    // all frames sent to the server). The mask key is 4 random bytes from urandom,
    // XORed with the payload before sending. mock_ws_server accepts both
    // masked and unmasked (it simply doesn't read our frames), so the demo
    // still works; but a real Binance/Coinbase will reject an unmasked frame.
    bool send_frame(WsOpcode opcode, const void* payload, std::size_t len) noexcept {
        std::uint8_t hdr[14];  // 2 base + 8 ext_len + 4 mask = max 14
        std::size_t  hdr_len = 2;
        hdr[0] = 0x80 | static_cast<std::uint8_t>(opcode);  // FIN=1 + opcode
        if (len < 126) {
            hdr[1] = 0x80 | static_cast<std::uint8_t>(len);  // 0x80 = mask bit
        } else if (len <= 0xFFFF) {
            hdr[1] = 0x80 | 126;
            hdr[2] = static_cast<std::uint8_t>((len >> 8) & 0xFF);
            hdr[3] = static_cast<std::uint8_t>(len & 0xFF);
            hdr_len = 4;
        } else {
            hdr[1] = 0x80 | 127;
            for (int i = 0; i < 8; ++i)
                hdr[2 + i] = static_cast<std::uint8_t>((len >> (8 * (7 - i))) & 0xFF);
            hdr_len = 10;
        }

        // Generate a 4-byte mask_key and insert it directly after the length.
        std::uint8_t mask_key[4];
        detail::random_bytes(mask_key, 4);
        std::memcpy(hdr + hdr_len, mask_key, 4);
        hdr_len += 4;

        if (::send(fd_, hdr, hdr_len, MSG_NOSIGNAL) != static_cast<ssize_t>(hdr_len))
            return false;

        if (len > 0) {
            // XOR the payload with mask_key (cyclic mod 4). We send in chunks
            // so as not to allocate the whole buffer on the heap.
            constexpr std::size_t CHUNK = 1024;
            std::uint8_t buf[CHUNK];
            const auto* src = static_cast<const std::uint8_t*>(payload);
            std::size_t sent = 0;
            while (sent < len) {
                const std::size_t n = std::min(len - sent, CHUNK);
                for (std::size_t i = 0; i < n; ++i) {
                    buf[i] = src[sent + i] ^ mask_key[(sent + i) & 3];
                }
                if (::send(fd_, buf, n, MSG_NOSIGNAL) != static_cast<ssize_t>(n))
                    return false;
                sent += n;
            }
        }
        return true;
    }

};

}  // namespace feed
