/*
 * SoupBinTCP — an industrial TCP transport for NASDAQ OUCH (and a few other
 *              order entry protocols).
 *
 * Why a separate layer if OUCH goes over TCP? Because TCP is a byte STREAM —
 * it does not know message boundaries. SoupBin adds:
 *   - packet framing (length prefix), so the receiver knows where
 *     one OUCH message ends and the next begins
 *   - per-data-packet sequence numbers — detects gaps if something is lost
 *     (TCP usually delivers, but on retransmissions / reconnect there can be a gap)
 *   - login/logout flow (Login Request → Login Accepted/Rejected)
 *   - heartbeats (server: H every second, client: R every second) — detects
 *     half-open connections (NAT/firewall cuts a session after idle)
 *   - end-of-session marker (Z)
 *
 * Packet wire format:
 *
 *   [0..1]  Packet Length   uint16 BE — the length of the REST of the packet (excluding these 2 bytes)
 *   [2]     Packet Type     1 ASCII char (see enum below)
 *   [3..]   Payload         variable length (Length - 1)
 *
 * Packet types (from/to the client):
 *   Server → Client:
 *     S = Sequenced Data    payload: OUCH execution report / etc.
 *     H = Server Heartbeat  payload: none
 *     A = Login Accepted    payload: session(10) + sequence(20)
 *     J = Login Rejected    payload: reason(1)
 *     Z = End of Session    payload: none
 *     + = Debug             payload: text
 *
 *   Client → Server:
 *     U = Unsequenced Data  payload: OUCH order entry (Enter/Cancel/Replace)
 *     R = Client Heartbeat  payload: none
 *     L = Login Request     payload: user(6) + pass(10) + session(10) + seq(20)
 *     O = Logout Request    payload: none
 *
 * Sequence tracking: only 'S' packets have an implicit sequence (starting from
 * the "Requested Sequence Number" in Login Accepted, +1 per S packet). 'U' (orders)
 * do NOT have a sequence on the client side — it is assumed the TCP stream preserves
 * order, and the exchange confirms each via 'S' with its own seq.
 *
 * Standard: NASDAQ TotalView-ITCH uses MoldUDP64 (UDP multicast, we have it in
 * multicast/), and OUCH uses SoupBinTCP (TCP point-to-point) — these are parallel
 * transports for the same application layers.
 */
#pragma once

#include "ouch_protocol.hpp"   // for write_u16_be/read_u32_be helpers + OUCHMessage

#include <cstdint>
#include <cstring>


namespace soupbin {

// Wire-format constants.
inline constexpr std::size_t HEADER_SIZE = 3;    // length(2) + type(1)
inline constexpr std::size_t USERNAME_LEN = 6;
inline constexpr std::size_t PASSWORD_LEN = 10;
inline constexpr std::size_t SESSION_LEN  = 10;
inline constexpr std::size_t SEQNUM_LEN   = 20;  // ASCII numeric, space-padded


// Packet types (ASCII).
enum class PacketType : char {
    // Server → Client
    SEQUENCED_DATA    = 'S',
    SERVER_HEARTBEAT  = 'H',
    LOGIN_ACCEPTED    = 'A',
    LOGIN_REJECTED    = 'J',
    END_OF_SESSION    = 'Z',
    DEBUG             = '+',
    // Client → Server
    UNSEQUENCED_DATA  = 'U',
    CLIENT_HEARTBEAT  = 'R',
    LOGIN_REQUEST     = 'L',
    LOGOUT_REQUEST    = 'O',
};


// write_u16_be: a 2-byte big-endian write (OUCH already has 32/64 but not 16).
inline void write_u16_be(uint8_t* dst, uint16_t val) noexcept {
    dst[0] = static_cast<uint8_t>(val >> 8);
    dst[1] = static_cast<uint8_t>(val);
}

inline uint16_t read_u16_be(const uint8_t* src) noexcept {
    return (static_cast<uint16_t>(src[0]) << 8) | static_cast<uint16_t>(src[1]);
}


// pack_header: write length + type. Returns HEADER_SIZE.
// length is the length of the REST of the packet (i.e. 1 byte type + payload_len).
inline std::size_t pack_header(uint8_t* dst, PacketType type, std::size_t payload_len) noexcept {
    const uint16_t rest_len = static_cast<uint16_t>(1 + payload_len);
    write_u16_be(dst, rest_len);
    dst[2] = static_cast<uint8_t>(type);
    return HEADER_SIZE;
}


// pack_heartbeat: a short path for H/R (no payload).
inline std::size_t pack_heartbeat(uint8_t* dst, bool client_side) noexcept {
    return pack_header(dst, client_side ? PacketType::CLIENT_HEARTBEAT
                                        : PacketType::SERVER_HEARTBEAT, 0);
}


// pack_data: wrap a raw payload (e.g. an encoded OUCH message) in a
// SEQUENCED_DATA ('S' server→client) or UNSEQUENCED_DATA ('U' client→server) packet.
// Returns the total packet size or 0 if the buffer is too small.
inline std::size_t pack_data(uint8_t* dst, std::size_t cap, const uint8_t* payload,
                              std::size_t payload_len, bool client_side) noexcept {
    if (cap < HEADER_SIZE + payload_len) return 0;
    pack_header(dst, client_side ? PacketType::UNSEQUENCED_DATA
                                  : PacketType::SEQUENCED_DATA, payload_len);
    if (payload_len > 0) std::memcpy(dst + HEADER_SIZE, payload, payload_len);
    return HEADER_SIZE + payload_len;
}


// pack_login_request: build an 'L' packet with user/pass/session/sequence.
// empty session → the server picks the current one; sequence "0" → start from the beginning,
// "1" → all messages, ">=N" → only from seq N (recovery).
inline std::size_t pack_login_request(uint8_t* dst, std::size_t cap,
                                       const char* username, const char* password,
                                       const char* session = "",
                                       const char* sequence = "0") noexcept {
    const std::size_t payload = USERNAME_LEN + PASSWORD_LEN + SESSION_LEN + SEQNUM_LEN;
    if (cap < HEADER_SIZE + payload) return 0;

    pack_header(dst, PacketType::LOGIN_REQUEST, payload);
    auto* p = dst + HEADER_SIZE;

    // Each field is space-padded left to its length.
    auto write_padded_field = [](uint8_t* out, const char* src, std::size_t n) {
        std::memset(out, ' ', n);
        const std::size_t slen = src ? std::strlen(src) : 0;
        std::memcpy(out, src, slen > n ? n : slen);
    };
    write_padded_field(p,                                              username, USERNAME_LEN);
    write_padded_field(p + USERNAME_LEN,                               password, PASSWORD_LEN);
    write_padded_field(p + USERNAME_LEN + PASSWORD_LEN,                session,  SESSION_LEN);
    write_padded_field(p + USERNAME_LEN + PASSWORD_LEN + SESSION_LEN,  sequence, SEQNUM_LEN);
    return HEADER_SIZE + payload;
}


// pack_logout_request: 'O' with no payload.
inline std::size_t pack_logout_request(uint8_t* dst) noexcept {
    return pack_header(dst, PacketType::LOGOUT_REQUEST, 0);
}


// ParsedPacket — what parse_packet returns.
struct ParsedPacket {
    PacketType   type;
    const uint8_t* payload;       // a pointer INTO the input buffer (zero-copy)
    std::size_t  payload_len;
    bool         valid;

    ParsedPacket() noexcept : type(PacketType::DEBUG), payload(nullptr),
                               payload_len(0), valid(false) {}
};


// parse_packet: parse one packet from the buffer. Returns a ParsedPacket with a
// pointer to the payload (zero-copy). valid=false when the buffer is too short
// or the declared length exceeds the available bytes.
//
// out_consumed (if != nullptr) gets the number of bytes consumed —
// the caller can advance its cursor and call again for the next packet
// in the same buffer (TCP may deliver several packets in one recv).
inline ParsedPacket parse_packet(const uint8_t* buf, std::size_t len,
                                  std::size_t* out_consumed = nullptr) noexcept {
    ParsedPacket p;
    if (len < HEADER_SIZE) return p;
    const uint16_t rest_len = read_u16_be(buf);
    if (rest_len < 1) return p;                       // must fit at least 1 byte of type
    const std::size_t total = 2 + rest_len;            // 2 (length field) + rest
    if (len < total) return p;                        // incomplete packet — wait for more from TCP

    p.type        = static_cast<PacketType>(buf[2]);
    p.payload     = buf + HEADER_SIZE;
    p.payload_len = rest_len - 1;
    p.valid       = true;
    if (out_consumed) *out_consumed = total;
    return p;
}


// SequenceTracker for SoupBinTCP: only 'S' packets (sequenced data) have an
// implicit number. The stream is "one per packet" — increment per
// 'S' received. A gap = TCP delivered a packet out of order or a
// session restart without resync — a signal to reconnect with the right
// "Requested Sequence Number" in Login.
struct SequenceTracker {
    uint64_t expected = 1;   // SoupBin numbers from 1
    uint64_t received = 0;
    uint64_t gaps     = 0;

    // Call on every RECEIVED 'S' packet. Returns true when in order.
    bool observe_sequenced() noexcept {
        ++expected;
        ++received;
        return true;  // for SoupBin, gap detection is done on the reconnect side
                      // (the server resends packets from Requested Sequence) — here just stats
    }

    // reset_to: use after Login Accepted when the server returns the current sequence.
    void reset_to(uint64_t starting_seq) noexcept {
        expected = starting_seq;
        received = 0;
        gaps     = 0;
    }
};


}  // namespace soupbin
