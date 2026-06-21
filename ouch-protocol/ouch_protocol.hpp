/*
 * OUCHMessage — NASDAQ OUCH 4.2 order entry protocol (encoder/decoder).
 *
 * OUCH to binarny protokół — surowe bajty przez TCP. Na produkcji opakowane
 * w SoupBinTCP (patrz soupbin.hpp dla framingu, sequence, login/heartbeat).
 *
 * Typy wiadomości:
 *   Client → Exchange:
 *     O = Enter Order   (33 B)  — nowe zlecenie
 *     X = Cancel Order  (19 B)  — anuluj istniejące
 *     U = Replace Order (37 B)  — zmień cenę/ilość
 *
 *   Exchange → Client:
 *     A = Accepted  (41 B)  — zlecenie przyjęte
 *     C = Cancelled (20 B)  — zlecenie anulowane
 *     E = Executed  (31 B)  — zlecenie zrealizowane
 *
 * Kodowanie ceny: fixed-point × 10000 ($150.25 → 1502500).
 *
 * Pipeline: Strategy → Router → Risk → OMS → OUCH → Exchange.
 *
 * Wydajność (lab): ~19.9M msg/sec encoding, p50=30ns, p99=40ns.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>

// Helpery big-endian (network byte order). OUCH używa BE — jak htonl() w gniazdach.

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


// OUCHResponse — sparsowana odpowiedź giełdy.

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
    char    prev_token[15]; // poprzedni token (Replaced only) — token, ktory zostal zastapiony

    OUCHResponse() noexcept
        : shares(0), price(0), order_ref(0), match_number(0) {
        type[0] = '\0';
        token[0] = '\0';
        side[0] = '\0';
        stock[0] = '\0';
        reason[0] = '\0';
        error_msg[0] = '\0';
        prev_token[0] = '\0';
    }
};


// OUCHOrder — zdekodowane zlecenie KLIENTA (strona giełdy parsuje O/X/U). (#152)
struct OUCHOrder {
    char    type;           // 'O'=Enter, 'X'=Cancel, 'U'=Replace, 'E'=error
    char    token[15];      // Enter/Cancel: token; Replace: EXISTING token
    char    new_token[15];  // Replace: replacement token
    char    side;           // Enter: 'B'/'S'
    int32_t shares;
    char    stock[9];       // Enter: symbol
    double  price;          // Enter/Replace
    char    tif;            // Enter: time-in-force
    bool    valid;

    OUCHOrder() noexcept : type('E'), side(' '), shares(0), price(0), tif(' '), valid(false) {
        token[0] = '\0'; new_token[0] = '\0'; stock[0] = '\0';
    }
};


// OUCHMessage — encoder klienta + decoder odpowiedzi giełdy + decoder zleceń.

class OUCHMessage {
    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    // Bounds-checked string copy — prevents buffer overflow on malformed input
    template<int N>
    static void safe_copy(char (&dst)[N], const char* src) noexcept {
        std::strncpy(dst, src, N - 1);
        dst[N - 1] = '\0';
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

    // === ENCODING (Exchange → Client) ===
    // Symetria do enter_order/cancel_order: strona giełdy buduje raporty, które
    // parse_response() po stronie klienta dekoduje. Domyka OUCH (encode obu
    // stron + decode) i pozwala na pełny roundtrip przez SoupBinTCP.

    // encode_accepted: Accepted (41 B) — giełda potwierdza przyjęcie zlecenia.
    static int encode_accepted(uint8_t* buf, const char* token, char side,
                               int32_t shares, const char* stock, double price,
                               int64_t order_ref, char tif = 'D') noexcept {
        buf[0] = 'A';
        write_padded(buf + 1, token, 14);
        buf[15] = static_cast<uint8_t>(side);
        write_u32_be(buf + 16, shares);
        write_padded(buf + 20, stock, 8);
        write_u32_be(buf + 28, static_cast<uint32_t>(price * 10000));
        buf[32] = static_cast<uint8_t>(tif);
        write_u64_be(buf + 33, static_cast<uint64_t>(order_ref));
        return 41;
    }

    // encode_executed: Executed (31 B) — giełda raportuje (częściowe) wykonanie.
    static int encode_executed(uint8_t* buf, const char* token, int32_t shares,
                               double price, int64_t match_number) noexcept {
        buf[0] = 'E';
        write_padded(buf + 1, token, 14);
        write_u32_be(buf + 15, shares);
        write_u32_be(buf + 19, static_cast<uint32_t>(price * 10000));
        write_u64_be(buf + 23, static_cast<uint64_t>(match_number));
        return 31;
    }

    // encode_cancelled: Cancelled (20 B) — giełda potwierdza anulowanie.
    static int encode_cancelled(uint8_t* buf, const char* token, int32_t shares,
                                char reason = 'U') noexcept {
        buf[0] = 'C';
        write_padded(buf + 1, token, 14);
        write_u32_be(buf + 15, shares);
        buf[19] = static_cast<uint8_t>(reason);
        return 20;
    }

    // encode_rejected: Order Rejected (16 B) — giełda odrzuca zlecenie (np. zly
    // symbol, zamkniety rynek). reason: kod 1-znakowy.
    static int encode_rejected(uint8_t* buf, const char* token, char reason) noexcept {
        buf[0] = 'J';
        write_padded(buf + 1, token, 14);
        buf[15] = static_cast<uint8_t>(reason);
        return 16;
    }

    // encode_cancel_reject: Cancel Reject (16 B) — gielda ODRZUCA probe anulowania
    // ('X'), bo zlecenia nie da sie anulowac (juz w pelni wykonane, nieznany token,
    // albo cancel-pending). Rozni sie od 'C' (Cancelled = sukces): tu nic sie nie
    // zmienilo, klient nie moze zalozyc ze pozycja zmalala. reason: kod 1-znakowy
    // (#178).
    static int encode_cancel_reject(uint8_t* buf, const char* token, char reason) noexcept {
        buf[0] = 'I';
        write_padded(buf + 1, token, 14);
        buf[15] = static_cast<uint8_t>(reason);
        return 16;
    }

    // encode_cancel_pending: Cancel Pending (15 B) — gielda PRZYJELA prosbe o
    // anulowanie, ale jeszcze nie weszla w zycie (zlecenie w stanie blokujacym:
    // aukcja, cross, halt). Stan POSREDNI: jeszcze nie 'C' (gotowe) ani 'I'
    // (odrzucone). Klient czeka na finalne C/I; nie redukuje pozycji (#186).
    static int encode_cancel_pending(uint8_t* buf, const char* token) noexcept {
        buf[0] = 'P';
        write_padded(buf + 1, token, 14);
        return 15;
    }

    // encode_aiq_canceled: AIQ Canceled (20 B) — gielda zdjela CZESC zlecenia, bo
    // matchowalaby sie z WLASNYM spoczywajacym zleceniem firmy (Anti-Internalization
    // Quantity / self-match prevention). Nie blad: zlecenie zyje dalej z mniejsza
    // iloscia. decrement_shares = ile zdjeto; reason kod 1-znakowy (#194).
    static int encode_aiq_canceled(uint8_t* buf, const char* token, int32_t decrement_shares,
                                   char reason = 'Q') noexcept {
        buf[0] = 'D';
        write_padded(buf + 1, token, 14);
        write_u32_be(buf + 15, decrement_shares);
        buf[19] = static_cast<uint8_t>(reason);
        return 20;
    }

    // encode_system_event: System Event (10 B) — zdarzenie sesji gieldy (#202):
    // 'S'=start dnia, 'E'=koniec dnia, 'O'=otwarcie rynku, 'C'=zamkniecie, 'A'=
    // halt awaryjny, 'R'=wznowienie. Klient BRAMKUJE handel wg tych zdarzen
    // (np. nie wysyla zlecen przed 'O' / po 'C'). timestamp = nanosekundy.
    static int encode_system_event(uint8_t* buf, int64_t timestamp, char event_code) noexcept {
        buf[0] = 'S';
        write_u64_be(buf + 1, static_cast<uint64_t>(timestamp));
        buf[9] = static_cast<uint8_t>(event_code);
        return 10;
    }

    // encode_restated: Restated (24 B) — gielda zmienila parametry zlecenia BEZ
    // prosby klienta (#210): reprice zgodnosciowy (locked/crossed market), redukcja
    // display, korekta. Zlecenie zyje dalej z NOWYMI shares/price. reason: kod
    // 1-znakowy (np. 'P'=reprice, 'D'=display). Klient aktualizuje swoj obraz.
    static int encode_restated(uint8_t* buf, const char* token, int32_t shares,
                               double price, char reason = 'P') noexcept {
        buf[0] = 'R';
        write_padded(buf + 1, token, 14);
        write_u32_be(buf + 15, shares);
        write_u32_be(buf + 19, static_cast<uint32_t>(price * 10000));
        buf[23] = static_cast<uint8_t>(reason);
        return 24;
    }

    // encode_replaced: Order Replaced (45 B) — giełda potwierdza Replace ('U').
    // Niesie NOWY token (replacement) + POPRZEDNI (zastapiony) + nowe parametry.
    static int encode_replaced(uint8_t* buf, const char* repl_token, const char* prev_token,
                               int32_t shares, double price, int64_t order_ref) noexcept {
        buf[0] = 'U';
        write_padded(buf + 1,  repl_token, 14);
        write_padded(buf + 15, prev_token, 14);
        write_u32_be(buf + 29, shares);
        write_u32_be(buf + 33, static_cast<uint32_t>(price * 10000));
        write_u64_be(buf + 37, static_cast<uint64_t>(order_ref));
        return 45;
    }

    // encode_broken_trade: Broken Trade (28 B) — gielda UNIEWAZNIA wczesniej
    // wykonany trade (bust/DK). Klient musi odwrocic fill po swojej stronie.
    static int encode_broken_trade(uint8_t* buf, const char* token, int32_t shares,
                                   int64_t match_number, char reason = 'E') noexcept {
        buf[0] = 'B';
        write_padded(buf + 1, token, 14);
        write_u32_be(buf + 15, shares);
        write_u64_be(buf + 19, static_cast<uint64_t>(match_number));
        buf[27] = static_cast<uint8_t>(reason);
        return 28;
    }

    // === DECODING (Exchange → Client) ===

    // parse_response: decode exchange response from raw bytes
    // Like reading binary data from recv() — need to know the wire format
    // Jak czytanie binarnych danych z recv() — trzeba znać format na kablu
    static OUCHResponse parse_response(const uint8_t* data, int len) noexcept {
        OUCHResponse resp;

        if (!data || len < 1) {
            safe_copy(resp.type, "ERROR");
            safe_copy(resp.error_msg, "empty response");
            return resp;
        }

        char msg_type = static_cast<char>(data[0]);

        if (msg_type == 'A') {  // Accepted
            if (len < 41) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "ACCEPTED too short: %d < 41 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "ACCEPTED");
            strip_padding(resp.token, data + 1, 14);
            safe_copy(resp.side, (data[15] == 'B') ? "BUY" : "SELL");
            resp.shares = read_u32_be(data + 16);
            strip_padding(resp.stock, data + 20, 8);
            resp.price = read_u32_be(data + 28) / 10000.0;
            // skip TIF at byte 32
            resp.order_ref = read_u64_be(data + 33);

        } else if (msg_type == 'C') {  // Cancelled
            if (len < 20) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "CANCELLED too short: %d < 20 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "CANCELLED");
            strip_padding(resp.token, data + 1, 14);
            resp.shares = read_u32_be(data + 15);
            resp.reason[0] = static_cast<char>(data[19]);
            resp.reason[1] = '\0';

        } else if (msg_type == 'E') {  // Executed
            if (len < 31) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "EXECUTED too short: %d < 31 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "EXECUTED");
            strip_padding(resp.token, data + 1, 14);
            resp.shares = read_u32_be(data + 15);
            resp.price = read_u32_be(data + 19) / 10000.0;
            resp.match_number = read_u64_be(data + 23);

        } else if (msg_type == 'B') {  // Broken Trade
            if (len < 28) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "BROKEN too short: %d < 28 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "BROKEN");
            strip_padding(resp.token, data + 1, 14);
            resp.shares       = read_u32_be(data + 15);
            resp.match_number = read_u64_be(data + 19);
            resp.reason[0]    = static_cast<char>(data[27]);
            resp.reason[1]    = '\0';

        } else if (msg_type == 'J') {  // Order Rejected
            if (len < 16) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "REJECTED too short: %d < 16 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "REJECTED");
            strip_padding(resp.token, data + 1, 14);
            resp.reason[0] = static_cast<char>(data[15]);
            resp.reason[1] = '\0';

        } else if (msg_type == 'I') {  // Cancel Reject (#178)
            if (len < 16) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "CANCEL_REJECT too short: %d < 16 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "CXL_REJECT");
            strip_padding(resp.token, data + 1, 14);
            resp.reason[0] = static_cast<char>(data[15]);
            resp.reason[1] = '\0';

        } else if (msg_type == 'P') {  // Cancel Pending (#186)
            if (len < 15) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "CANCEL_PENDING too short: %d < 15 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "CXL_PEND");   // <=10 znakow: unik stringop-truncation (sanitizer -O1)
            strip_padding(resp.token, data + 1, 14);

        } else if (msg_type == 'D') {  // AIQ Canceled (#194)
            if (len < 20) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "AIQ_CANCEL too short: %d < 20 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "AIQ_CXL");
            strip_padding(resp.token, data + 1, 14);
            resp.shares    = read_u32_be(data + 15);    // ile zdjeto
            resp.reason[0] = static_cast<char>(data[19]);
            resp.reason[1] = '\0';

        } else if (msg_type == 'S') {  // System Event (#202)
            if (len < 10) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "SYS_EVENT too short: %d < 10 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "SYS_EVENT");
            resp.match_number = static_cast<int64_t>(read_u64_be(data + 1));  // timestamp
            resp.reason[0]    = static_cast<char>(data[9]);                   // kod zdarzenia
            resp.reason[1]    = '\0';

        } else if (msg_type == 'R') {  // Restated (#210)
            if (len < 24) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "RESTATED too short: %d < 24 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "RESTATED");
            strip_padding(resp.token, data + 1, 14);
            resp.shares    = read_u32_be(data + 15);
            resp.price     = read_u32_be(data + 19) / 10000.0;
            resp.reason[0] = static_cast<char>(data[23]);
            resp.reason[1] = '\0';

        } else if (msg_type == 'U') {  // Order Replaced
            if (len < 45) {
                safe_copy(resp.type, "ERROR");
                std::snprintf(resp.error_msg, sizeof(resp.error_msg) - 1,
                              "REPLACED too short: %d < 45 bytes", len);
                return resp;
            }
            safe_copy(resp.type, "REPLACED");
            strip_padding(resp.token,      data + 1,  14);  // nowy (replacement)
            strip_padding(resp.prev_token, data + 15, 14);  // zastapiony
            resp.shares    = read_u32_be(data + 29);
            resp.price     = read_u32_be(data + 33) / 10000.0;
            resp.order_ref = read_u64_be(data + 37);

        } else {
            safe_copy(resp.type, "UNKNOWN");
        }

        return resp;
    }

    // === DECODING zlecen KLIENTA (Client → Exchange) — strona gieldy (#152) ===
    // Symetria do enter_order/cancel_order/replace_order: gateway gieldy parsuje
    // surowe bajty 'O'/'X'/'U' z recv() na strukture. valid=false przy za krotkim
    // buforze / nieznanym typie.
    static OUCHOrder parse_order(const uint8_t* data, int len) noexcept {
        OUCHOrder o;
        if (!data || len < 1) return o;
        const char t = static_cast<char>(data[0]);
        if (t == 'O') {                         // Enter Order (33 B)
            if (len < 33) return o;
            o.type = 'O';
            strip_padding(o.token, data + 1, 14);
            o.side   = static_cast<char>(data[15]);
            o.shares = static_cast<int32_t>(read_u32_be(data + 16));
            strip_padding(o.stock, data + 20, 8);
            o.price  = read_u32_be(data + 28) / 10000.0;
            o.tif    = static_cast<char>(data[32]);
            o.valid  = true;
        } else if (t == 'X') {                  // Cancel Order (19 B)
            if (len < 19) return o;
            o.type = 'X';
            strip_padding(o.token, data + 1, 14);
            o.shares = static_cast<int32_t>(read_u32_be(data + 15));
            o.valid  = true;
        } else if (t == 'U') {                  // Replace Order (37 B)
            if (len < 37) return o;
            o.type = 'U';
            strip_padding(o.token,     data + 1,  14);   // existing
            strip_padding(o.new_token, data + 15, 14);   // replacement
            o.shares = static_cast<int32_t>(read_u32_be(data + 29));
            o.price  = read_u32_be(data + 33) / 10000.0;
            o.valid  = true;
        }
        return o;
    }

    // validate_order: walidacja zlecenia KLIENTA po stronie gieldy (#169). Zwraca
    // nullptr gdy OK, albo powod odrzucenia. Pairs z parse_order — gateway
    // sprawdza zanim przekaze do matchingu.
    static const char* validate_order(const OUCHOrder& o) noexcept {
        if (!o.valid)        return "malformed";
        if (o.token[0] == '\0') return "empty token";
        if (o.type == 'O') {
            if (o.side != 'B' && o.side != 'S') return "invalid side";
            if (o.shares <= 0)   return "non-positive shares";
            if (o.price  <= 0.0) return "non-positive price";
            if (o.stock[0] == '\0') return "empty symbol";
        } else if (o.type == 'U') {
            if (o.new_token[0] == '\0') return "empty replacement token";
            if (o.shares <= 0)   return "non-positive shares";
            if (o.price  <= 0.0) return "non-positive price";
        } else if (o.type == 'X') {
            if (o.shares < 0)    return "negative shares";
        }
        return nullptr;
    }
};
