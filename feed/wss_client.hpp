/*
 * WssClient — WsClient + TLS (wss://).
 *
 * A twin WS client wrapped in OpenSSL. This file is OPTIONAL —
 * it compiles only when the project is built with the HFT_USE_OPENSSL flag
 * and linked against -lssl -lcrypto (see the Makefile target `make feed/wss_demo`).
 *
 * Why a separate file rather than an #ifdef in ws_client.hpp?
 *   - the main CI doesn't require libssl-dev (smaller image, faster build)
 *   - readability: ws_client.hpp stays clean (~270 lines); everything
 *     TLS-specific lives here.
 *
 * What do you need to use it?
 *   sudo apt install libssl-dev          # Ubuntu / Debian
 *   sudo dnf install openssl-devel       # RHEL / Fedora
 *
 *   make feed/wss_demo HFT_USE_OPENSSL=1
 *   ./feed/wss_demo stream.binance.com 9443 /ws/btcusdt@trade
 *
 * Security: we verify the server certificate via OpenSSL's default store
 * (/etc/ssl/certs/). Without this verification MITM would be trivial.
 * We set SNI so virtual-host servers (Cloudflare etc.) know
 * which cert to return.
 */
#pragma once

#ifndef HFT_USE_OPENSSL
#  error "wss_client.hpp requires -DHFT_USE_OPENSSL and linking -lssl -lcrypto"
#endif

#include "ws_client.hpp"   // for the WsOpcode enum + random_bytes/gen_key helpers

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>


namespace feed {


class WssClient {
    int      fd_  = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL*     ssl_ = nullptr;

public:
    WssClient() = default;
    ~WssClient() { close(); }

    WssClient(const WssClient&)            = delete;
    WssClient& operator=(const WssClient&) = delete;
    WssClient(WssClient&&)                 = delete;
    WssClient& operator=(WssClient&&)      = delete;

    // connect: TCP → TLS handshake → HTTP upgrade.
    // host  — e.g. "stream.binance.com" (SNI + hostname verification)
    // port  — e.g. 443 (Binance uses 9443)
    // path  — e.g. "/ws/btcusdt@trade"
    bool connect(const char* host, int port, const char* path = "/") noexcept {
        // Init OpenSSL — idempotent, can be called multiple times.
        SSL_library_init();
        SSL_load_error_strings();

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) return false;

        // Verify the server certificate + a minimum of TLS 1.2 (1.0/1.1 deprecated).
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

        // TCP connect (as in ws_client.hpp — resolve, socket, connect).
        if (!tcp_connect(host, port)) { close(); return false; }

        ssl_ = SSL_new(ctx_);
        if (!ssl_) { close(); return false; }
        SSL_set_fd(ssl_, fd_);

        // SNI — critical for virtual-host servers (Cloudflare, AWS LB).
        // Without it you get the default cert and the hostname check fails.
        SSL_set_tlsext_host_name(ssl_, host);

        // Hostname verification — checks the CN/SAN in the certificate.
        if (SSL_set1_host(ssl_, host) != 1) { close(); return false; }

        if (SSL_connect(ssl_) != 1) {
            print_ssl_error("SSL_connect");
            close();
            return false;
        }

        // HTTP upgrade over TLS — identical to ws_client.hpp.
        char ws_key[25] = {};
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
        if (SSL_write(ssl_, req, n) != n) { close(); return false; }

        // Read the response byte-by-byte until CRLF CRLF. Same as in
        // ws_client.hpp: a greedy SSL_read would pull WS frames sent right after the
        // handshake into the buffer and lose them → recv_text() desync. Reading
        // one byte at a time stops us exactly at the header boundary.
        char resp[2048];
        int got = 0;
        while (got < static_cast<int>(sizeof(resp)) - 1) {
            char c;
            const int r = SSL_read(ssl_, &c, 1);
            if (r <= 0) { close(); return false; }
            resp[got++] = c;
            if (got >= 4 && resp[got - 4] == '\r' && resp[got - 3] == '\n' &&
                            resp[got - 2] == '\r' && resp[got - 1] == '\n') {
                break;
            }
        }
        resp[got] = '\0';
        if (std::strncmp(resp, "HTTP/1.1 101", 12) != 0) {
            std::fprintf(stderr, "wss: server replied non-101: %.*s\n", 64, resp);
            close();
            return false;
        }
        return true;
    }

    // recv_text — same logic as WsClient, only ::recv → SSL_read.
    int recv_text(char* out, std::size_t max_len) noexcept {
        for (;;) {
            std::uint8_t h[2];
            if (!ssl_read_all(h, 2)) return -1;

            const WsOpcode    opcode = static_cast<WsOpcode>(h[0] & 0x0F);
            const bool        masked = (h[1] & 0x80) != 0;
            std::uint64_t     len    = h[1] & 0x7F;

            if (len == 126) {
                std::uint8_t ext[2];
                if (!ssl_read_all(ext, 2)) return -1;
                len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
            } else if (len == 127) {
                std::uint8_t ext[8];
                if (!ssl_read_all(ext, 8)) return -1;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
            }
            std::uint8_t mask_key[4]{};
            if (masked && !ssl_read_all(mask_key, 4)) return -1;
            if (len > max_len) return -1;

            if (!ssl_read_all(reinterpret_cast<std::uint8_t*>(out), len)) return -1;
            if (masked) {
                for (std::uint64_t i = 0; i < len; ++i) out[i] ^= mask_key[i & 3];
            }

            switch (opcode) {
            case WsOpcode::TEXT:
            case WsOpcode::BINARY:    return static_cast<int>(len);
            case WsOpcode::PING:
                send_frame(WsOpcode::PONG, out, static_cast<std::size_t>(len));
                continue;
            case WsOpcode::PONG:
            case WsOpcode::CONTINUATION: continue;
            case WsOpcode::CLOSE:
                send_frame(WsOpcode::CLOSE, out, static_cast<std::size_t>(len));
                return 0;
            }
        }
    }

    // send_text — with a mask, identical to WsClient.
    bool send_text(const char* msg, std::size_t len) noexcept {
        return send_frame(WsOpcode::TEXT, msg, len);
    }

    // send_ping: a PING from the client — call it every 30s so Binance/Coinbase
    // doesn't close an idle wss:// after 3 min.
    bool send_ping(const void* payload = nullptr, std::size_t len = 0) noexcept {
        return send_frame(WsOpcode::PING, payload, len);
    }

    bool is_open() const noexcept { return ssl_ != nullptr && fd_ >= 0; }

    void close() noexcept {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
    }

private:
    bool tcp_connect(const char* host, int port) noexcept {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char port_str[16];
        std::snprintf(port_str, sizeof(port_str), "%d", port);
        if (::getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return false;

        fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ < 0) { ::freeaddrinfo(res); return false; }
        if (::connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
            ::freeaddrinfo(res);
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        ::freeaddrinfo(res);
        return true;
    }

    bool ssl_read_all(std::uint8_t* dst, std::uint64_t n) noexcept {
        std::uint64_t got = 0;
        while (got < n) {
            const int r = SSL_read(ssl_, dst + got, static_cast<int>(n - got));
            if (r <= 0) return false;
            got += static_cast<std::uint64_t>(r);
        }
        return true;
    }

    bool send_frame(WsOpcode opcode, const void* payload, std::size_t len) noexcept {
        std::uint8_t hdr[14];
        std::size_t  hdr_len = 2;
        hdr[0] = 0x80 | static_cast<std::uint8_t>(opcode);
        if (len < 126) {
            hdr[1] = 0x80 | static_cast<std::uint8_t>(len);
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
        std::uint8_t mask_key[4];
        detail::random_bytes(mask_key, 4);
        std::memcpy(hdr + hdr_len, mask_key, 4);
        hdr_len += 4;

        if (SSL_write(ssl_, hdr, static_cast<int>(hdr_len)) != static_cast<int>(hdr_len))
            return false;

        if (len > 0) {
            constexpr std::size_t CHUNK = 1024;
            std::uint8_t buf[CHUNK];
            const auto* src = static_cast<const std::uint8_t*>(payload);
            std::size_t sent = 0;
            while (sent < len) {
                const std::size_t n = std::min(len - sent, CHUNK);
                for (std::size_t i = 0; i < n; ++i)
                    buf[i] = src[sent + i] ^ mask_key[(sent + i) & 3];
                if (SSL_write(ssl_, buf, static_cast<int>(n)) != static_cast<int>(n))
                    return false;
                sent += n;
            }
        }
        return true;
    }

    static void print_ssl_error(const char* ctx) noexcept {
        char buf[256];
        const unsigned long e = ERR_get_error();
        ERR_error_string_n(e, buf, sizeof(buf));
        std::fprintf(stderr, "wss: %s failed: %s\n", ctx, buf);
    }
};


}  // namespace feed
