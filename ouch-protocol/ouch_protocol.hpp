/*
 * NASDAQ OUCH 4.2 Order Entry Protocol — C++ Implementation
 * Protokół wprowadzania zleceń NASDAQ OUCH 4.2 — implementacja C++
 *
 * Encodes and decodes OUCH messages for order entry and exchange responses.
 * Koduje i dekoduje wiadomości OUCH do wprowadzania zleceń i odpowiedzi giełdy.
 *
 * OUCH is a binary protocol — like sending raw bytes over a socket (sendto()).
 * OUCH to protokół binarny — jak wysyłanie surowych bajtów przez socket (sendto()).
 *
 * Message types / Typy wiadomości:
 *   Client → Exchange:
 *     O = Enter Order  (33 bytes)  — submit new order / złóż nowe zlecenie
 *     X = Cancel Order (19 bytes)  — cancel existing / anuluj istniejące
 *     U = Replace Order (37 bytes) — modify price/qty / zmień cenę/ilość
 *
 *   Exchange → Client:
 *     A = Accepted  (41 bytes) — order accepted / zlecenie przyjęte
 *     C = Cancelled (20 bytes) — order cancelled / zlecenie anulowane
 *     E = Executed  (31 bytes) — order filled / zlecenie zrealizowane
 *
 * Price encoding: fixed-point × 10000 ($150.25 → 1502500)
 * Kodowanie ceny: stałoprzecinkowe × 10000 ($150.25 → 1502500)
 *
 * Pipeline: Strategy → Router → Risk → OMS → **OUCH** → Exchange
 *
 * Performance / Wydajność:
 *   Python: ~1.7M msg/sec (encoding)
 *   C++:    ~30-50M msg/sec
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>

// === Helper: big-endian encoding (network byte order) ===
// OUCH uses network byte order (big-endian) — like htonl() in socket programming
// OUCH używa kolejności bajtów sieciowej (big-endian) — jak htonl() w programowaniu gniazd

inline void write_u32_be(uint8_t* dst, uint32_t val) noexcept {
    dst[0] = (val >> 24) & 0xFF;
    dst[1] = (val >> 16) & 0xFF;
    dst[2] = (val >>  8) & 0xFF;
    dst[3] =  val        & 0xFF;
}

inline uint32_t read_u32_be(const uint8_t* src) noexcept {
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) <<  8) |
            static_cast<uint32_t>(src[3]);
}

inline void write_u64_be(uint8_t* dst, uint64_t val) noexcept {
    for (int i = 7; i >= 0; --i) {
        dst[7 - i] = (val >> (i * 8)) & 0xFF;
    }
}

inline uint64_t read_u64_be(const uint8_t* src) noexcept {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | src[i];
    }
    return val;
}

// Copy string left-justified, padded with spaces (like OUCH spec requires)
// Kopiuj string wyrównany do lewej, dopełniony spacjami (jak wymaga specyfikacja OUCH)
inline void write_padded(uint8_t* dst, const char* src, int len) noexcept {
    std::memset(dst, ' ', len);
    int slen = std::strlen(src);
    if (slen > len) slen = len;
    std::memcpy(dst, src, slen);
}


// === OUCHResponse — parsed exchange response ===

struct OUCHResponse {
    char    type[12];       // "ACCEPTED", "CANCELLED", "EXECUTED", "ERROR", "UNKNOWN"
    char    token[15];      // order token (14 chars + null)
    char    side[5];        // "BUY" or "SELL"
    int32_t shares;
    char    stock[9];       // stock symbol (8 chars + null)
    double  price;
    int64_t order_ref;      // order reference number (Accepted only)
    int64_t match_number;   // match number (Executed only)
    char    reason[2];      // cancel reason (Cancelled only)
    char    error_msg[64];  // error description (Error only)

    OUCHResponse() noexcept
        : shares(0), price(0), order_ref(0), match_number(0) {
        type[0] = '\0';
        token[0] = '\0';
        side[0] = '\0';
        stock[0] = '\0';
        reason[0] = '\0';
        error_msg[0] = '\0';
    }
};


// === OUCHMessage — encoder/decoder ===

class OUCHMessage {
    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    // Strip trailing spaces from a padded field
    // Usuń końcowe spacje z dopełnionego pola
    static void strip_padding(char* dst, const uint8_t* src, int len) noexcept {
        std::memcpy(dst, src, len);
        // Find last non-space character
        int end = len;
        while (end > 0 && dst[end - 1] == ' ') end--;
        dst[end] = '\0';
    }

public:
    // === ENCODING (Client → Exchange) ===

    // enter_order: build Enter Order message (33 bytes)
    // Like 'echo -ne "\x4f..." | nc exchange 1234' — raw binary on the wire
    // Returns number of bytes written / Zwraca liczbę zapisanych bajtów
    static int enter_order(uint8_t* buf, const char* token, char side,
                           int32_t shares, const char* stock,
                           double price, char tif = 'D') noexcept {
        buf[0] = 'O';                              // msg type (1 byte)
        write_padded(buf + 1, token, 14);           // token (14 bytes)
        buf[15] = static_cast<uint8_t>(side);       // side (1 byte)
        write_u32_be(buf + 16, shares);             // shares (4 bytes)
        write_padded(buf + 20, stock, 8);           // stock (8 bytes)
        write_u32_be(buf + 28, static_cast<uint32_t>(price * 10000)); // price (4 bytes)
        buf[32] = static_cast<uint8_t>(tif);        // time-in-force (1 byte)
        return 33;
    }

    // cancel_order: build Cancel Order message (19 bytes)
    static int cancel_order(uint8_t* buf, const char* token,
                            int32_t shares = 0) noexcept {
        buf[0] = 'X';                              // msg type
        write_padded(buf + 1, token, 14);           // token
        write_u32_be(buf + 15, shares);             // shares to cancel (0 = all)
        return 19;
    }

    // replace_order: build Replace Order message (37 bytes)
    static int replace_order(uint8_t* buf, const char* existing_token,
                             const char* new_token, int32_t shares,
                             double price) noexcept {
        buf[0] = 'U';                              // msg type
        write_padded(buf + 1, existing_token, 14);  // existing token
        write_padded(buf + 15, new_token, 14);      // new token
        write_u32_be(buf + 29, shares);             // new shares
        write_u32_be(buf + 33, static_cast<uint32_t>(price * 10000)); // new price
        return 37;
    }

    // === DECODING (Exchange → Client) ===

    // parse_response: decode exchange response from raw bytes
    // Like reading binary data from recv() — need to know the wire format
    // Jak czytanie binarnych danych z recv() — trzeba znać format na kablu
    static OUCHResponse parse_response(const uint8_t* data, int len) noexcept {
        OUCHResponse resp;

        if (!data || len < 1) {
            std::strcpy(resp.type, "ERROR");
            std::strcpy(resp.error_msg, "empty response");
            return resp;
        }

        char msg_type = static_cast<char>(data[0]);

        if (msg_type == 'A') {  // Accepted
            if (len < 41) {
                std::strcpy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, 63, "ACCEPTED too short: %d < 41 bytes", len);
                return resp;
            }
            std::strcpy(resp.type, "ACCEPTED");
            strip_padding(resp.token, data + 1, 14);
            resp.side[0] = (data[15] == 'B') ? 'B' : 'S';
            std::strcpy(resp.side, (data[15] == 'B') ? "BUY" : "SELL");
            resp.shares = read_u32_be(data + 16);
            strip_padding(resp.stock, data + 20, 8);
            resp.price = read_u32_be(data + 28) / 10000.0;
            // skip TIF at byte 32
            resp.order_ref = read_u64_be(data + 33);

        } else if (msg_type == 'C') {  // Cancelled
            if (len < 20) {
                std::strcpy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, 63, "CANCELLED too short: %d < 20 bytes", len);
                return resp;
            }
            std::strcpy(resp.type, "CANCELLED");
            strip_padding(resp.token, data + 1, 14);
            resp.shares = read_u32_be(data + 15);
            resp.reason[0] = static_cast<char>(data[19]);
            resp.reason[1] = '\0';

        } else if (msg_type == 'E') {  // Executed
            if (len < 31) {
                std::strcpy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, 63, "EXECUTED too short: %d < 31 bytes", len);
                return resp;
            }
            std::strcpy(resp.type, "EXECUTED");
            strip_padding(resp.token, data + 1, 14);
            resp.shares = read_u32_be(data + 15);
            resp.price = read_u32_be(data + 19) / 10000.0;
            resp.match_number = read_u64_be(data + 23);

        } else {
            std::strcpy(resp.type, "UNKNOWN");
        }

        return resp;
    }
};
