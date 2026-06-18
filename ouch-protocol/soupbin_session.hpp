/*
 * SoupBinOuch session glue (expansion #78) — spina warstwę transportu
 * (soupbin.hpp) z warstwą aplikacyjną (ouch_protocol.hpp) w JEDEN strumień:
 *
 *   Client → Server:  L (login) , U (Enter/Cancel/Replace Order, payload=OUCH)
 *   Server → Client:  A (login accepted) , S (sequenced data, payload=OUCH
 *                      Accepted/Executed/Cancelled) , H (heartbeat) , Z (end)
 *
 * Wcześniej soupbin.hpp miał prymitywy (pack/parse/seq), a ouch_protocol.hpp
 * encode/decode — ale nic nie demonstrowało pełnego roundtripu login→order→exec.
 * Tu są dwie strony:
 *   - OuchSessionClient — stanowa maszyna klienta: konsumuje strumień TCP
 *     (wiele pakietów per recv), dekoduje OUCH z pakietów 'S', liczy raporty.
 *   - mock_exchange_respond — minimalna giełda: na 'L' odsyła 'A', na każde 'U'
 *     (Enter Order) odsyła 'S'(Accepted) + 'S'(Executed). Pozwala przejść całą
 *     pętlę in-process (jak mock server w feed/feed_demo).
 */
#pragma once

#include "soupbin.hpp"        // framing + SequenceTracker
#include "ouch_protocol.hpp"  // OUCHMessage encode/decode

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdio>


namespace soupbin {

// parse_seqnum: ASCII numeric (spacja-padded) → uint64. Ignoruje spacje.
inline uint64_t parse_seqnum(const uint8_t* p, std::size_t n) noexcept {
    uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = static_cast<char>(p[i]);
        if (c >= '0' && c <= '9') v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

// strip_field: usuń końcowe spacje z dopełnionego pola OUCH (OUCHMessage::
// strip_padding jest prywatne, więc lokalny odpowiednik dla mock giełdy).
inline void strip_field(char* dst, const uint8_t* src, int n) noexcept {
    std::memcpy(dst, src, static_cast<std::size_t>(n));
    int end = n;
    while (end > 0 && dst[end - 1] == ' ') --end;
    dst[end] = '\0';
}


// HeartbeatTimer — timer heartbeatow SoupBinTCP (expansion #118).
//
// SoupBin wymaga heartbeatow w OBIE strony: server wysyla 'H', klient 'R' co ~1s
// gdy idle — tak wykrywa sie half-open connection (NAT/firewall ucina idle TCP).
// Brak heartbeatu/danych od drugiej strony przez ~15s => zerwij i reconnect.
//   on_tx(now) — wywolaj po KAZDEJ wyslanej ramce (reset naszego idle)
//   on_rx(now) — wywolaj po KAZDEJ odebranej ramce (reset zegara peera)
//   need_send(now, interval) — czy czas wyslac nasz heartbeat
//   peer_timed_out(now, timeout) — czy druga strona milczy za dlugo
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


// OuchSessionClient — stan sesji po stronie klienta. Karmiony strumieniem
// bajtów z TCP; rozbija na pakiety SoupBin i dekoduje OUCH z 'S'.
class OuchSessionClient {
    bool            logged_in_     = false;
    bool            session_ended_ = false;
    SequenceTracker seq_;
    std::uint64_t   accepts_    = 0;
    std::uint64_t   executes_   = 0;
    std::uint64_t   cancels_    = 0;
    std::uint64_t   heartbeats_ = 0;
    std::uint64_t   errors_     = 0;

public:
    // on_packet: zaaplikuj jeden sparsowany pakiet server→client.
    void on_packet(const ParsedPacket& p) noexcept {
        if (!p.valid) { ++errors_; return; }
        switch (p.type) {
            case PacketType::LOGIN_ACCEPTED:
                logged_in_ = true;
                // payload: session(10) + sequence(20). Ustaw oczekiwany seq.
                if (p.payload_len >= SESSION_LEN + SEQNUM_LEN) {
                    const uint64_t s = parse_seqnum(p.payload + SESSION_LEN, SEQNUM_LEN);
                    seq_.reset_to(s == 0 ? 1 : s);
                }
                break;
            case PacketType::LOGIN_REJECTED:
                logged_in_ = false; ++errors_;
                break;
            case PacketType::SEQUENCED_DATA: {
                seq_.observe_sequenced();
                const OUCHResponse r = OUCHMessage::parse_response(
                    p.payload, static_cast<int>(p.payload_len));
                if      (std::strcmp(r.type, "ACCEPTED")  == 0) ++accepts_;
                else if (std::strcmp(r.type, "EXECUTED")  == 0) ++executes_;
                else if (std::strcmp(r.type, "CANCELLED") == 0) ++cancels_;
                else                                            ++errors_;
                break;
            }
            case PacketType::SERVER_HEARTBEAT: ++heartbeats_; break;
            case PacketType::END_OF_SESSION:   session_ended_ = true; break;
            case PacketType::DEBUG:            break;   // ignorujemy debug
            default:                           ++errors_; break;
        }
    }

    // consume: przetwórz strumień TCP (może zawierać wiele pakietów). Zwraca
    // liczbę w pełni przetworzonych bajtów; reszta (niepełny pakiet) zostaje
    // u wołającego do doklejenia kolejnego recv.
    std::size_t consume(const uint8_t* buf, std::size_t len) noexcept {
        std::size_t off = 0;
        while (off < len) {
            std::size_t used = 0;
            const ParsedPacket p = parse_packet(buf + off, len - off, &used);
            if (!p.valid) break;   // niepełny pakiet — czekaj na więcej
            on_packet(p);
            off += used;
        }
        return off;
    }

    bool          logged_in()     const noexcept { return logged_in_; }
    bool          session_ended() const noexcept { return session_ended_; }
    std::uint64_t accepts()       const noexcept { return accepts_; }
    std::uint64_t executes()      const noexcept { return executes_; }
    std::uint64_t cancels()       const noexcept { return cancels_; }
    std::uint64_t heartbeats()    const noexcept { return heartbeats_; }
    std::uint64_t errors()        const noexcept { return errors_; }
    std::uint64_t expected_seq()  const noexcept { return seq_.expected; }
};


// mock_exchange_respond — minimalna giełda zamykająca pętlę. Parsuje strumień
// klienta (L/U/O/R) i pisze odpowiedzi server→client do out. Na każde Enter
// Order ('U' z OUCH 'O') odsyła Accepted + Executed (pełny fill). Zwraca liczbę
// zapisanych bajtów. start_seq = "Requested Sequence Number" do Login Accepted.
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
            // Login Accepted: session(10) + sequence(20), spacja-padded.
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
            // Enter Order — odczytaj pola, odeślij Accepted + Executed.
            // Layout 'O' (z enter_order): token(14)@1, side@15, shares@16,
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
            // Executed (pełny fill) → 'S'
            mlen = OUCHMessage::encode_executed(msg, token, shares, price, match_no++);
            if (!room(HEADER_SIZE + static_cast<std::size_t>(mlen))) break;
            out_off += pack_data(out + out_off, cap - out_off, msg,
                                 static_cast<std::size_t>(mlen), /*client_side=*/false);

        } else if (p.type == PacketType::LOGOUT_REQUEST) {
            if (!room(HEADER_SIZE)) break;
            pack_header(out + out_off, PacketType::END_OF_SESSION, 0);
            out_off += HEADER_SIZE;
        }
        // 'R' (client heartbeat) — milcząco ignorujemy (mógłby odbić 'H').
    }
    return out_off;
}

}  // namespace soupbin
