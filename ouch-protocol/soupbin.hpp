/*
 * SoupBinTCP — przemysłowy transport TCP dla NASDAQ OUCH (i kilku innych
 *              protokołów order entry).
 *
 * Po co osobna warstwa skoro OUCH leci po TCP? Bo TCP to STRUMIEŃ bajtów —
 * nie zna granic wiadomości. SoupBin dodaje:
 *   - obramowanie pakietów (length prefix), żeby odbiorca wiedział gdzie
 *     kończy się jedna wiadomość OUCH a zaczyna następna
 *   - sequence numbers per pakiet danych — wykrywa luki gdyby coś się zgubiło
 *     (TCP zwykle dostarcza, ale przy retransmisjach / reconnect bywa gap)
 *   - login/logout flow (Login Request → Login Accepted/Rejected)
 *   - heartbeaty (server: H co sekundę, client: R co sekundę) — wykrywa
 *     half-open connections (NAT/firewall ucina session po idle)
 *   - end-of-session marker (Z)
 *
 * Wire format pakietu:
 *
 *   [0..1]  Packet Length   uint16 BE — długość RESZTY pakietu (bez tych 2 bajtów)
 *   [2]     Packet Type     1 znak ASCII (patrz enum poniżej)
 *   [3..]   Payload         zmienna długość (Length - 1)
 *
 * Typy pakietów (od/do klienta):
 *   Server → Client:
 *     S = Sequenced Data    payload: OUCH execution report / itp.
 *     H = Server Heartbeat  payload: brak
 *     A = Login Accepted    payload: session(10) + sequence(20)
 *     J = Login Rejected    payload: reason(1)
 *     Z = End of Session    payload: brak
 *     + = Debug             payload: text
 *
 *   Client → Server:
 *     U = Unsequenced Data  payload: OUCH order entry (Enter/Cancel/Replace)
 *     R = Client Heartbeat  payload: brak
 *     L = Login Request     payload: user(6) + pass(10) + session(10) + seq(20)
 *     O = Logout Request    payload: brak
 *
 * Sequence tracking: tylko 'S' pakiety mają implicit sequence (start od
 * "Requested Sequence Number" z Login Accepted, +1 per pakiet S). 'U' (zlecenia)
 * NIE mają sequence po stronie klienta — uważa się że strumień TCP zachowuje
 * porządek, a giełda potwierdzi każde przez 'S' z własnym seq.
 *
 * Standard: NASDAQ TotalView-ITCH używa MoldUDP64 (UDP multicast, my mamy w
 * multicast/), a OUCH używa SoupBinTCP (TCP point-to-point) — to równoległe
 * transporty dla tych samych warstw aplikacyjnych.
 */
#pragma once

#include "ouch_protocol.hpp"   // dla write_u16_be/read_u32_be helperów + OUCHMessage

#include <cstdint>
#include <cstring>


namespace soupbin {

// Stałe wire format.
inline constexpr std::size_t HEADER_SIZE = 3;    // length(2) + type(1)
inline constexpr std::size_t USERNAME_LEN = 6;
inline constexpr std::size_t PASSWORD_LEN = 10;
inline constexpr std::size_t SESSION_LEN  = 10;
inline constexpr std::size_t SEQNUM_LEN   = 20;  // ASCII numeric, space-padded


// Typy pakietów (ASCII).
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


// write_u16_be: 2-bajtowy big-endian write (OUCH ma już 32/64 ale nie 16).
inline void write_u16_be(uint8_t* dst, uint16_t val) noexcept {
    dst[0] = static_cast<uint8_t>(val >> 8);
    dst[1] = static_cast<uint8_t>(val);
}

inline uint16_t read_u16_be(const uint8_t* src) noexcept {
    return (static_cast<uint16_t>(src[0]) << 8) | static_cast<uint16_t>(src[1]);
}


// pack_header: zapisz length + type. Zwraca HEADER_SIZE.
// length to długość RESZTY pakietu (czyli 1 byte type + payload_len).
inline std::size_t pack_header(uint8_t* dst, PacketType type, std::size_t payload_len) noexcept {
    const uint16_t rest_len = static_cast<uint16_t>(1 + payload_len);
    write_u16_be(dst, rest_len);
    dst[2] = static_cast<uint8_t>(type);
    return HEADER_SIZE;
}


// pack_heartbeat: krótka ścieżka dla H/R (brak payloadu).
inline std::size_t pack_heartbeat(uint8_t* dst, bool client_side) noexcept {
    return pack_header(dst, client_side ? PacketType::CLIENT_HEARTBEAT
                                        : PacketType::SERVER_HEARTBEAT, 0);
}


// pack_data: opakuj surowy payload (np. zakodowaną wiadomość OUCH) w pakiet
// SEQUENCED_DATA ('S' server→client) lub UNSEQUENCED_DATA ('U' client→server).
// Zwraca łączny rozmiar pakietu lub 0 gdy bufor za mały.
inline std::size_t pack_data(uint8_t* dst, std::size_t cap, const uint8_t* payload,
                              std::size_t payload_len, bool client_side) noexcept {
    if (cap < HEADER_SIZE + payload_len) return 0;
    pack_header(dst, client_side ? PacketType::UNSEQUENCED_DATA
                                  : PacketType::SEQUENCED_DATA, payload_len);
    if (payload_len > 0) std::memcpy(dst + HEADER_SIZE, payload, payload_len);
    return HEADER_SIZE + payload_len;
}


// pack_login_request: zbuduj pakiet 'L' z user/pass/session/sequence.
// session pusty → server wybiera bieżącą; sequence "0" → start od początku,
// "1" → wszystkie wiadomości, ">=N" → tylko od seq N (recovery).
inline std::size_t pack_login_request(uint8_t* dst, std::size_t cap,
                                       const char* username, const char* password,
                                       const char* session = "",
                                       const char* sequence = "0") noexcept {
    const std::size_t payload = USERNAME_LEN + PASSWORD_LEN + SESSION_LEN + SEQNUM_LEN;
    if (cap < HEADER_SIZE + payload) return 0;

    pack_header(dst, PacketType::LOGIN_REQUEST, payload);
    auto* p = dst + HEADER_SIZE;

    // Każde pole spacja-padded do lewej do swojej długości.
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


// pack_logout_request: 'O' bez payloadu.
inline std::size_t pack_logout_request(uint8_t* dst) noexcept {
    return pack_header(dst, PacketType::LOGOUT_REQUEST, 0);
}


// ParsedPacket — co zwraca parse_packet.
struct ParsedPacket {
    PacketType   type;
    const uint8_t* payload;       // wskaźnik W buforze wejściowym (zero-copy)
    std::size_t  payload_len;
    bool         valid;

    ParsedPacket() noexcept : type(PacketType::DEBUG), payload(nullptr),
                               payload_len(0), valid(false) {}
};


// parse_packet: rozparsuj jeden pakiet z bufora. Zwraca ParsedPacket z
// wskaźnikiem do payloadu (zero-copy). valid=false gdy bufor za krótki
// lub deklarowana długość przekracza dostępne bajty.
//
// out_consumed (jeśli != nullptr) dostaje liczbę zjedzonych bajtów —
// wywołujący może przesunąć swój kursor i wywołać znowu na kolejny pakiet
// w tym samym buforze (TCP może dostarczyć wiele pakietów w jednym recv).
inline ParsedPacket parse_packet(const uint8_t* buf, std::size_t len,
                                  std::size_t* out_consumed = nullptr) noexcept {
    ParsedPacket p;
    if (len < HEADER_SIZE) return p;
    const uint16_t rest_len = read_u16_be(buf);
    if (rest_len < 1) return p;                       // musi mieścić chociaż 1 bajt type
    const std::size_t total = 2 + rest_len;            // 2 (length field) + rest
    if (len < total) return p;                        // niepełny pakiet — czekaj na więcej z TCP

    p.type        = static_cast<PacketType>(buf[2]);
    p.payload     = buf + HEADER_SIZE;
    p.payload_len = rest_len - 1;
    p.valid       = true;
    if (out_consumed) *out_consumed = total;
    return p;
}


// SequenceTracker dla SoupBinTCP: tylko pakiety 'S' (sequenced data) mają
// implicit numer. Strumień jest "po jednym per pakiet" — inkrementuj per
// 'S' otrzymany. Luka = TCP dostarczył pakiet poza kolejnością albo
// session restart bez resyncu — sygnał do reconnect z odpowiednim
// "Requested Sequence Number" w Login.
struct SequenceTracker {
    uint64_t expected = 1;   // SoupBin numeruje od 1
    uint64_t received = 0;
    uint64_t gaps     = 0;

    // Wywołaj na każdy ODEBRANY pakiet 'S'. Zwraca true gdy w kolejności.
    bool observe_sequenced() noexcept {
        ++expected;
        ++received;
        return true;  // dla SoupBin gap detection robi się po stronie reconnect'u
                      // (server odsyła pakiety od Requested Sequence) — tu tylko stats
    }

    // reset_to: użyj po Login Accepted gdy server zwraca aktualny sequence.
    void reset_to(uint64_t starting_seq) noexcept {
        expected = starting_seq;
        received = 0;
        gaps     = 0;
    }
};


}  // namespace soupbin
