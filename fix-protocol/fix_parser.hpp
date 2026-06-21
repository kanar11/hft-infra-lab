/*
 * FIXMessage — parser protokołu FIX 4.2 z walidacją sesji.
 *
 * FIX (Financial Information eXchange) to standard komunikacji handlowej
 * broker↔giełda. Wiadomość to pary tag=value rozdzielone delimiterem:
 *
 *   8=FIX.4.2 | 9=65 | 35=D | 49=TRADER1 | 56=EXCH | 55=AAPL | ... | 10=123
 *
 * WAŻNE — prawdziwy wire FIX używa delimitera SOH (ASCII 0x01), nie '|'.
 * Pionowa kreska to konwencja human-readable (do logów/testów). Parser
 * auto-wykrywa: jeśli w wiadomości jest SOH → używa SOH, inaczej '|'.
 *
 * Kluczowe tagi:
 *   8  = BeginString  (wersja protokołu, ZAWSZE pierwszy)
 *   9  = BodyLength   (liczba bajtów body, ZAWSZE drugi)
 *   35 = MsgType      (D=NewOrder, G=Modify, F=Cancel, 8=Execution, 0=Heartbeat)
 *   34 = MsgSeqNum    (numer sekwencyjny sesji)
 *   49 = SenderCompID / 56 = TargetCompID
 *   55 = Symbol, 54 = Side (1=Buy 2=Sell), 44 = Price, 38 = OrderQty
 *   10 = CheckSum     (ZAWSZE ostatni, suma modulo-256, 3 cyfry)
 *
 * Walidacja na poziomie standardu (czego brakowało wcześniej):
 *   - CheckSum (tag 10): suma WSZYSTKICH bajtów aż do delimitera przed "10="
 *     modulo 256. Każdy zgodny silnik FIX MUSI to sprawdzać — wykrywa
 *     uszkodzenie transmisji. checksum_valid().
 *   - BodyLength (tag 9): liczba bajtów od pola PO tagu 9 do (włącznie)
 *     delimitera przed "10=". body_length_valid().
 *   - Obecność wymaganych pól nagłówka (8, 9, 35, 10). has_required_header().
 *
 * Wydajność (lab): ~5.5M msg/sec, p50=150ns, p99=250ns.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>


// Maks. tagów na wiadomość (większość ma <20). Stała tablica = zero heap.
static constexpr int MAX_FIX_TAGS  = 32;
static constexpr int MAX_FIX_VALUE = 32;   // maks. długość wartości per tag


// FIXField — jedna para tag=value.
struct FIXField {
    int  tag;
    char value[MAX_FIX_VALUE];

    FIXField() noexcept : tag(0) { value[0] = '\0'; }
};


class FIXMessage {
    FIXField fields_[MAX_FIX_TAGS];
    int      field_count_;

    // Stan walidacji (ustawiany w parse()).
    bool     cksum_present_   = false;
    bool     cksum_valid_     = false;
    bool     bodylen_present_ = false;
    bool     bodylen_valid_   = false;
    int      computed_cksum_  = -1;
    int      computed_bodylen_ = -1;

    // find_field: liniowy skan po tagu — O(N), ale N≤32 (1 linia cache).
    const FIXField* find_field(int tag) const noexcept {
        for (int i = 0; i < field_count_; ++i) {
            if (fields_[i].tag == tag) return &fields_[i];
        }
        return nullptr;
    }

    static int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

public:
    static constexpr char SOH = '\x01';   // standardowy delimiter wire FIX

    FIXMessage() noexcept : field_count_(0) {}

    // compute_checksum: suma modulo-256 bajtów [0, len). Algorytm CheckSum (tag 10).
    static uint8_t compute_checksum(const char* data, int len) noexcept {
        unsigned sum = 0;
        for (int i = 0; i < len; ++i)
            sum += static_cast<unsigned char>(data[i]);
        return static_cast<uint8_t>(sum & 0xFF);
    }

    // parse: rozbij wiadomość na pary tag=value + zwaliduj CheckSum/BodyLength.
    // Auto-wykrywa delimiter (SOH albo '|'). Zwraca czas parsowania (ns).
    int64_t parse(const char* raw_msg) noexcept {
        const int64_t t0 = now_ns();
        field_count_      = 0;
        cksum_present_    = false;
        cksum_valid_      = false;
        bodylen_present_  = false;
        bodylen_valid_    = false;
        computed_cksum_   = -1;
        computed_bodylen_ = -1;

        if (!raw_msg || raw_msg[0] == '\0') return now_ns() - t0;

        // Kopia robocza. memchr ogranicza scan do 1024 B — niezaufana wiadomość
        // bez null-terminatora nie zmusi nas do czytania poza buforem.
        char buf[1024];
        const void* nul = std::memchr(raw_msg, '\0', sizeof(buf));
        const int len = nul
                ? static_cast<int>(static_cast<const char*>(nul) - raw_msg)
                : static_cast<int>(sizeof(buf)) - 1;
        std::memcpy(buf, raw_msg, static_cast<size_t>(len));
        buf[len] = '\0';

        // Delimiter: SOH gdy obecny (prawdziwy wire FIX), inaczej '|' (human-readable).
        const char delim_char =
            std::memchr(raw_msg, SOH, static_cast<size_t>(len)) ? SOH : '|';

        // Offsety potrzebne do walidacji (względem buf == względem raw_msg).
        int  cksum_off = -1;     // gdzie zaczyna się "10="
        int  body_off  = -1;     // gdzie zaczyna się pole PO tagu 9
        bool pending_body = false;

        char* pos = buf;
        while (*pos && field_count_ < MAX_FIX_TAGS) {
            const int field_off = static_cast<int>(pos - buf);
            if (pending_body) { body_off = field_off; pending_body = false; }

            // Znajdź następny delimiter lub koniec.
            char* delim = pos;
            while (*delim && *delim != delim_char) ++delim;

            // Znajdź '=' w tym segmencie.
            char* eq = pos;
            while (eq < delim && *eq != '=') ++eq;

            if (eq < delim && eq > pos) {
                *eq = '\0';
                const int tag = std::atoi(pos);
                if (tag > 0) {
                    FIXField& f = fields_[field_count_];
                    f.tag = tag;
                    const char* val_start = eq + 1;
                    int val_len = static_cast<int>(delim - val_start);
                    if (val_len >= MAX_FIX_VALUE) val_len = MAX_FIX_VALUE - 1;
                    if (val_len > 0) std::memcpy(f.value, val_start, static_cast<size_t>(val_len));
                    f.value[val_len] = '\0';
                    ++field_count_;

                    if (tag == 9)  pending_body = true;       // body zaczyna się od następnego pola
                    if (tag == 10) cksum_off    = field_off;  // checksum liczona do tego miejsca
                }
            }

            if (*delim == delim_char) ++delim;
            pos = delim;
        }

        validate(raw_msg, cksum_off, body_off);
        return now_ns() - t0;
    }

    // get_msg_type: tag 35 — D=NewOrder, G=Modify, F=Cancel, 8=Execution.
    const char* get_msg_type() const noexcept {
        const FIXField* f = find_field(35);
        return f ? f->value : "UNKNOWN";
    }

    // is_admin_msg_type: czy typ (tag 35) to wiadomosc SESYJNA (admin) (#177).
    // Admin: 0=Heartbeat 1=TestRequest 2=ResendRequest 3=Reject 4=SequenceReset
    // 5=Logout A=Logon (wszystkie jednoznakowe). Reszta = aplikacyjna (biznesowa,
    // np. D/F/G/8). Silnik routuje admin do warstwy sesji, app do OMS; istotne
    // tez przy ResendRequest (admin -> gap-fill, app -> retransmisja).
    static bool is_admin_msg_type(const char* t) noexcept {
        if (!t || t[0] == '\0' || t[1] != '\0') return false;  // admin typy sa jednoznakowe
        switch (t[0]) {
            case '0': case '1': case '2': case '3': case '4': case '5': case 'A':
                return true;
            default:
                return false;
        }
    }

    // is_admin: czy TA wiadomosc jest sesyjna (admin) wg tag 35.
    bool is_admin() const noexcept {
        const FIXField* f = find_field(35);
        return f && is_admin_msg_type(f->value);
    }

    // === Repeating-group readers (#263) ===
    // FIX repeating groups (e.g. 35=W MarketDataSnapshot, 35=X incremental, mass
    // quotes) repeat the same tag once per entry. get_field() returns only the
    // FIRST occurrence; these let a consumer walk every entry. occurrence is 0-based.

    // count_field: how many times `tag` appears in the message.
    int count_field(int tag) const noexcept {
        int c = 0;
        for (int i = 0; i < field_count_; ++i) if (fields_[i].tag == tag) ++c;
        return c;
    }

    // get_field_nth: value of the n-th (0-based) occurrence of `tag` (nullptr if
    // fewer than n+1 occurrences).
    const char* get_field_nth(int tag, int occurrence) const noexcept {
        int c = 0;
        for (int i = 0; i < field_count_; ++i) {
            if (fields_[i].tag == tag) {
                if (c == occurrence) return fields_[i].value;
                ++c;
            }
        }
        return nullptr;
    }

    // get_int_nth / get_double_nth: typed n-th occurrence (0 if absent).
    int get_int_nth(int tag, int occurrence) const noexcept {
        const char* v = get_field_nth(tag, occurrence);
        return v ? std::atoi(v) : 0;
    }
    double get_double_nth(int tag, int occurrence) const noexcept {
        const char* v = get_field_nth(tag, occurrence);
        return v ? std::atof(v) : 0.0;
    }

    // get_symbol: tag 55 — ticker.
    const char* get_symbol() const noexcept {
        const FIXField* f = find_field(55);
        return f ? f->value : "UNKNOWN";
    }

    // get_side: tag 54 — 1=BUY, 2=SELL.
    const char* get_side() const noexcept {
        const FIXField* f = find_field(54);
        if (!f) return "UNKNOWN";
        return (f->value[0] == '1') ? "BUY" : "SELL";
    }

    // get_price: tag 44.
    double get_price() const noexcept {
        const FIXField* f = find_field(44);
        return f ? std::atof(f->value) : 0.0;
    }

    // get_quantity: tag 38.
    int32_t get_quantity() const noexcept {
        const FIXField* f = find_field(38);
        return f ? std::atoi(f->value) : 0;
    }

    // get_field: generyczny lookup po tagu (nullptr gdy brak).
    const char* get_field(int tag) const noexcept {
        const FIXField* f = find_field(tag);
        return f ? f->value : nullptr;
    }

    // get_int / get_double: typowane odczyty dowolnego taga (#168). 0 gdy brak
    // taga — wygodne dla pol numerycznych (qty, seq, ceny, kody) bez recznego atoi.
    int32_t get_int(int tag) const noexcept {
        const FIXField* f = find_field(tag);
        return f ? std::atoi(f->value) : 0;
    }
    double get_double(int tag) const noexcept {
        const FIXField* f = find_field(tag);
        return f ? std::atof(f->value) : 0.0;
    }

    int field_count() const noexcept { return field_count_; }

    // Walidacja sesji FIX.
    bool checksum_present()    const noexcept { return cksum_present_; }
    bool checksum_valid()      const noexcept { return cksum_valid_; }
    bool body_length_present() const noexcept { return bodylen_present_; }
    bool body_length_valid()   const noexcept { return bodylen_valid_; }
    int  computed_checksum()   const noexcept { return computed_cksum_; }
    int  computed_body_length() const noexcept { return computed_bodylen_; }

    // has_required_header: minimalny zgodny nagłówek FIX — 8, 9, 35, 10.
    bool has_required_header() const noexcept {
        return find_field(8) && find_field(9) && find_field(35) && find_field(10);
    }

    // is_valid: kompletna, dobrze uformowana wiadomość sesji FIX.
    bool is_valid() const noexcept {
        return has_required_header() && cksum_valid_ && bodylen_valid_;
    }

    // build_message: zbuduj POPRAWNĄ wiadomość FIX z body (pola od 35= dalej,
    // każde zakończone delimiterem). Dokleja 8=BeginString, 9=BodyLength i
    // 10=CheckSum z policzoną sumą. Zwraca długość lub 0 gdy bufor za mały.
    //
    // Przykład: body = "35=D\x0155=AAPL\x0154=1\x01" → pełna wiadomość z 8/9/10.
    static int build_message(char* out, int cap, const char* body,
                             const char* begin_string = "FIX.4.2",
                             char delim = SOH) noexcept {
        const int blen = static_cast<int>(std::strlen(body));
        char head[64];
        const int hlen = std::snprintf(head, sizeof(head), "8=%s%c9=%d%c",
                                       begin_string, delim, blen, delim);
        if (hlen < 0 || hlen + blen + 16 > cap) return 0;

        int off = 0;
        std::memcpy(out + off, head, static_cast<size_t>(hlen)); off += hlen;
        std::memcpy(out + off, body, static_cast<size_t>(blen)); off += blen;

        // CheckSum liczona po wszystkich bajtach do tej pory (włącznie z
        // delimiterem kończącym body — czyli bajtem przed "10=").
        const uint8_t ck = compute_checksum(out, off);
        const int clen = std::snprintf(out + off, static_cast<size_t>(cap - off),
                                       "10=%03u%c", static_cast<unsigned>(ck), delim);
        if (clen < 0) return 0;
        off += clen;
        return off;
    }

    // validate_new_order: walidacja NewOrderSingle (35=D) po stronie acceptora
    // (#116). Sprawdza obecnosc wymaganych tagow i sensownosc wartosci. Zwraca
    // nullptr gdy OK, albo string z powodem odrzucenia. Nie wymaga CheckSum —
    // to walidacja warstwy aplikacyjnej, nie transportu.
    const char* validate_new_order() const noexcept {
        const FIXField* mt = find_field(35);
        if (!mt || mt->value[0] != 'D')   return "not a NewOrderSingle (35!=D)";
        if (!find_field(11))              return "missing ClOrdID (11)";
        if (!find_field(55))              return "missing Symbol (55)";
        const FIXField* sd = find_field(54);
        if (!sd)                          return "missing Side (54)";
        if (sd->value[0] != '1' && sd->value[0] != '2') return "invalid Side (54)";
        const FIXField* q = find_field(38);
        if (!q)                           return "missing OrderQty (38)";
        if (std::atoi(q->value) <= 0)     return "non-positive OrderQty (38)";
        // Limit (40=2) musi miec dodatnia cene (44).
        const FIXField* ot = find_field(40);
        if (ot && ot->value[0] == '2') {
            const FIXField* px = find_field(44);
            if (!px || std::atof(px->value) <= 0.0) return "limit order requires positive Price (44)";
        }
        return nullptr;
    }

    // print: wyświetl wiadomość (debug).
    void print() const {
        const char* mt = get_msg_type();
        const char* name = "UNKNOWN";
        if      (mt[0] == 'D') name = "NEW ORDER";
        else if (mt[0] == 'G') name = "MODIFY";
        else if (mt[0] == 'F') name = "CANCEL";
        else if (mt[0] == '8') name = "EXECUTION";
        else if (mt[0] == '0') name = "HEARTBEAT";
        printf("%s | %s %d %s @ %.2f  [%s]\n",
               name, get_side(), get_quantity(), get_symbol(), get_price(),
               is_valid() ? "valid" : "unvalidated");
    }

private:
    // validate: policz CheckSum i BodyLength z offsetów zebranych w parse()
    // i porównaj z wartościami w wiadomości. Liczone na ORYGINALNYCH bajtach
    // (raw_msg), bo buf ma '\0' wstawione w miejsce '='.
    void validate(const char* raw_msg, int cksum_off, int body_off) noexcept {
        const FIXField* f10 = find_field(10);
        if (f10 && cksum_off >= 0) {
            cksum_present_  = true;
            computed_cksum_ = compute_checksum(raw_msg, cksum_off);
            cksum_valid_    = (std::atoi(f10->value) == computed_cksum_);
        }
        const FIXField* f9 = find_field(9);
        if (f9 && body_off >= 0 && cksum_off >= body_off) {
            bodylen_present_  = true;
            computed_bodylen_ = cksum_off - body_off;
            bodylen_valid_    = (std::atoi(f9->value) == computed_bodylen_);
        }
    }
};
