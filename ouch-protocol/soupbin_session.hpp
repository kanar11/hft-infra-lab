/*
 * SoupBinOuch session glue (expansion #78) — ties the transport layer
 * (soupbin.hpp) to the application layer (ouch_protocol.hpp) into ONE stream:
 *
 *   Client → Server:  L (login) , U (Enter/Cancel/Replace Order, payload=OUCH)
 *   Server → Client:  A (login accepted) , S (sequenced data, payload=OUCH
 *                      Accepted/Executed/Cancelled) , H (heartbeat) , Z (end)
 *
 * Previously soupbin.hpp had primitives (pack/parse/seq), and ouch_protocol.hpp
 * encode/decode — but nothing demonstrated the full login→order→exec round-trip.
 * Here are the two sides:
 *   - OuchSessionClient — the client's stateful machine: consumes the TCP stream
 *     (many packets per recv), decodes OUCH from 'S' packets, counts reports.
 *   - mock_exchange_respond — a minimal exchange: on 'L' it replies 'A', on every 'U'
 *     (Enter Order) it replies 'S'(Accepted) + 'S'(Executed). Lets the whole
 *     loop run in-process (like the mock server in feed/feed_demo).
 */
#pragma once

#include "soupbin.hpp"        // framing + SequenceTracker
#include "ouch_protocol.hpp"  // OUCHMessage encode/decode

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdio>


namespace soupbin {

// parse_seqnum: ASCII numeric (space-padded) → uint64. Ignores spaces.
inline uint64_t parse_seqnum(const uint8_t* p, std::size_t n) noexcept {
    uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = static_cast<char>(p[i]);
        if (c >= '0' && c <= '9') v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

// strip_field: strip trailing spaces from a padded OUCH field (OUCHMessage::
// strip_padding is private, so this is a local equivalent for the mock exchange).
inline void strip_field(char* dst, const uint8_t* src, int n) noexcept {
    std::memcpy(dst, src, static_cast<std::size_t>(n));
    int end = n;
    while (end > 0 && dst[end - 1] == ' ') --end;
    dst[end] = '\0';
}


// Client outgoing wrappers (expansion #145): encode OUCH + framing into a
// 'U' (Unsequenced Data) packet in one call. Without them the caller must manually assemble
// enter_order + pack_data (as in #78). Return the total packet size or 0.
inline std::size_t pack_enter_order(uint8_t* out, std::size_t cap, const char* token,
                                    char side, int32_t shares, const char* stock,
                                    double price, char tif = 'D') noexcept {
    uint8_t ouch[64];
    const int n = OUCHMessage::enter_order(ouch, token, side, shares, stock, price, tif);
    return pack_data(out, cap, ouch, static_cast<std::size_t>(n), /*client_side=*/true);
}
inline std::size_t pack_cancel_order(uint8_t* out, std::size_t cap, const char* token,
                                     int32_t shares = 0) noexcept {
    uint8_t ouch[32];
    const int n = OUCHMessage::cancel_order(ouch, token, shares);
    return pack_data(out, cap, ouch, static_cast<std::size_t>(n), /*client_side=*/true);
}
inline std::size_t pack_replace_order(uint8_t* out, std::size_t cap, const char* existing_token,
                                      const char* new_token, int32_t shares, double price) noexcept {
    uint8_t ouch[64];
    const int n = OUCHMessage::replace_order(ouch, existing_token, new_token, shares, price);
    return pack_data(out, cap, ouch, static_cast<std::size_t>(n), /*client_side=*/true);
}


// HeartbeatTimer — the SoupBinTCP heartbeat timer (expansion #118).
//
// SoupBin requires heartbeats in BOTH directions: the server sends 'H', the client 'R' every ~1s
// when idle — this is how a half-open connection is detected (NAT/firewall cuts idle TCP).
// No heartbeat/data from the other side for ~15s => drop and reconnect.
//   on_tx(now) — call after EVERY sent frame (resets our idle)
//   on_rx(now) — call after EVERY received frame (resets the peer's clock)
//   need_send(now, interval) — whether it's time to send our heartbeat
//   peer_timed_out(now, timeout) — whether the other side has been silent too long
struct HeartbeatTimer {
    int64_t last_tx = 0;
    int64_t last_rx = 0;
    bool    started = false;

    void on_tx(int64_t now) noexcept { last_tx = now; started = true; }
    void on_rx(int64_t now) noexcept { last_rx = now; started = true; }

    bool need_send(int64_t now, int64_t interval) const noexcept {
        return started && (now - last_tx) >= interval;
    }
    bool peer_timed_out(int64_t now, int64_t timeout) const noexcept {
        return started && (now - last_rx) > timeout;
    }
    void reset() noexcept { *this = HeartbeatTimer{}; }
};


// OuchSessionClient — the client-side session state. Fed a byte stream
// from TCP; splits it into SoupBin packets and decodes OUCH from 'S'.
class OuchSessionClient {
    bool            logged_in_     = false;
    bool            session_ended_ = false;
    char            login_reject_reason_ = '\0';   // #139: 'A'=not auth, 'S'=no session
    SequenceTracker seq_;
    std::uint64_t   accepts_    = 0;
    std::uint64_t   executes_   = 0;
    std::uint64_t   cancels_    = 0;
    std::uint64_t   rejects_    = 0;   // #234
    std::uint64_t   replaces_   = 0;   // #234
    std::uint64_t   heartbeats_ = 0;
    std::uint64_t   errors_     = 0;
    std::uint64_t   others_     = 0;   // legal, individually uncounted types (#434)

public:
    // on_packet: apply one parsed server→client packet.
    void on_packet(const ParsedPacket& p) noexcept {
        if (!p.valid) { ++errors_; return; }
        switch (p.type) {
            case PacketType::LOGIN_ACCEPTED:
                logged_in_ = true;
                // payload: session(10) + sequence(20). Set the expected seq.
                if (p.payload_len >= SESSION_LEN + SEQNUM_LEN) {
                    const uint64_t s = parse_seqnum(p.payload + SESSION_LEN, SEQNUM_LEN);
                    seq_.reset_to(s == 0 ? 1 : s);
                }
                break;
            case PacketType::LOGIN_REJECTED:
                logged_in_ = false; ++errors_;
                if (p.payload_len >= 1)              // #139: login rejection reason
                    login_reject_reason_ = static_cast<char>(p.payload[0]);
                break;
            case PacketType::SEQUENCED_DATA: {
                seq_.observe_sequenced();
                const OUCHResponse r = OUCHMessage::parse_response(
                    p.payload, static_cast<int>(p.payload_len));
                if      (std::strcmp(r.type, "ACCEPTED")  == 0) ++accepts_;
                else if (std::strcmp(r.type, "EXECUTED")  == 0) ++executes_;
                else if (std::strcmp(r.type, "CANCELLED") == 0) ++cancels_;
                else if (std::strcmp(r.type, "REJECTED")  == 0) ++rejects_;   // #234
                else if (std::strcmp(r.type, "REPLACED")  == 0) ++replaces_;  // #234
                // #434: only a genuinely unparseable payload is an ERROR.
                // Before this, every LEGAL type outside the five above —
                // BROKEN, CXL_PEND, CXL_REJECT, RESTATED, AIQ_CXL,
                // SYS_EVENT — inflated errors_, so a session digesting
                // routine busts and cancel acks looked corrupted.
                else if (std::strcmp(r.type, "ERROR")   == 0
                      || std::strcmp(r.type, "UNKNOWN") == 0)   ++errors_;
                else                                            ++others_;
                break;
            }
            case PacketType::SERVER_HEARTBEAT: ++heartbeats_; break;
            case PacketType::END_OF_SESSION:   session_ended_ = true; break;
            case PacketType::DEBUG:            break;   // ignore debug
            default:                           ++errors_; break;
        }
    }

    // consume: process a TCP stream (may contain several packets). Returns
    // the number of bytes fully processed; the rest (an incomplete packet) stays
    // with the caller to prepend to the next recv.
    std::size_t consume(const uint8_t* buf, std::size_t len) noexcept {
        std::size_t off = 0;
        while (off < len) {
            std::size_t used = 0;
            const ParsedPacket p = parse_packet(buf + off, len - off, &used);
            if (!p.valid) break;   // incomplete packet — wait for more
            on_packet(p);
            off += used;
        }
        return off;
    }

    bool          logged_in()     const noexcept { return logged_in_; }
    bool          session_ended() const noexcept { return session_ended_; }
    char          login_reject_reason() const noexcept { return login_reject_reason_; }  // #139
    std::uint64_t accepts()       const noexcept { return accepts_; }
    std::uint64_t executes()      const noexcept { return executes_; }
    std::uint64_t cancels()       const noexcept { return cancels_; }
    std::uint64_t rejects()       const noexcept { return rejects_; }     // #234
    std::uint64_t replaces()      const noexcept { return replaces_; }    // #234
    std::uint64_t heartbeats()    const noexcept { return heartbeats_; }
    std::uint64_t errors()        const noexcept { return errors_; }
    // others: legal sequenced messages outside the five individually
    // counted kinds (#434) — busts, cancel-lifecycle acks, restatements,
    // AIQ decrements, system events. Non-zero here is NORMAL traffic;
    // non-zero errors() is not.
    std::uint64_t others()        const noexcept { return others_; }
    std::uint64_t expected_seq()  const noexcept { return seq_.expected; }
};


// mock_exchange_respond — a minimal exchange closing the loop. Parses the client's
// stream (L/U/O/R) and writes server→client responses into out. On every Enter
// Order ('U' with OUCH 'O') it replies Accepted + Executed (a full fill). Returns the number
// of bytes written. start_seq = the "Requested Sequence Number" for Login Accepted.
inline std::size_t mock_exchange_respond(uint8_t* out, std::size_t cap,
                                         const uint8_t* client_stream, std::size_t len,
                                         uint64_t start_seq = 1) noexcept {
    std::size_t in_off = 0, out_off = 0;
    int64_t order_ref = 7000, match_no = 90000;

    auto room = [&](std::size_t need) { return out_off + need <= cap; };

    while (in_off < len) {
        std::size_t used = 0;
        const ParsedPacket p = parse_packet(client_stream + in_off, len - in_off, &used);
        if (!p.valid) break;
        in_off += used;

        if (p.type == PacketType::LOGIN_REQUEST) {
            // Login Accepted: session(10) + sequence(20), space-padded.
            const std::size_t payload = SESSION_LEN + SEQNUM_LEN;
            if (!room(HEADER_SIZE + payload)) break;
            pack_header(out + out_off, PacketType::LOGIN_ACCEPTED, payload);
            uint8_t* pl = out + out_off + HEADER_SIZE;
            std::memset(pl, ' ', payload);
            std::memcpy(pl, "SESSION01", 9);                  // session id
            char seqbuf[24];
            const int sn = std::snprintf(seqbuf, sizeof(seqbuf), "%llu",
                                         static_cast<unsigned long long>(start_seq));
            if (sn > 0) std::memcpy(pl + SESSION_LEN, seqbuf, static_cast<std::size_t>(sn));
            out_off += HEADER_SIZE + payload;

        } else if (p.type == PacketType::UNSEQUENCED_DATA &&
                   p.payload_len >= 1 && p.payload[0] == 'O') {
            // Enter Order — read the fields, send back Accepted + Executed.
            // Layout of 'O' (from enter_order): token(14)@1, side@15, shares@16,
            // stock(8)@20, price@28, tif@32.
            char token[15]; strip_field(token, p.payload + 1, 14);
            const char    side   = static_cast<char>(p.payload[15]);
            const int32_t shares = static_cast<int32_t>(read_u32_be(p.payload + 16));
            char stock[9];      strip_field(stock, p.payload + 20, 8);
            const double  price  = read_u32_be(p.payload + 28) / 10000.0;
            const char    tif    = static_cast<char>(p.payload[32]);

            uint8_t msg[64];
            // Accepted → 'S'
            int mlen = OUCHMessage::encode_accepted(msg, token, side, shares,
                                                    stock, price, order_ref++, tif);
            if (!room(HEADER_SIZE + static_cast<std::size_t>(mlen))) break;
            out_off += pack_data(out + out_off, cap - out_off, msg,
                                 static_cast<std::size_t>(mlen), /*client_side=*/false);
            // Executed (full fill) → 'S'
            mlen = OUCHMessage::encode_executed(msg, token, shares, price, match_no++);
            if (!room(HEADER_SIZE + static_cast<std::size_t>(mlen))) break;
            out_off += pack_data(out + out_off, cap - out_off, msg,
                                 static_cast<std::size_t>(mlen), /*client_side=*/false);

        } else if (p.type == PacketType::LOGOUT_REQUEST) {
            if (!room(HEADER_SIZE)) break;
            pack_header(out + out_off, PacketType::END_OF_SESSION, 0);
            out_off += HEADER_SIZE;
        }
        // 'R' (client heartbeat) — silently ignored (could bounce back an 'H').
    }
    return out_off;
}

}  // namespace soupbin
