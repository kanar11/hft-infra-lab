/*
 * FIX 4.2 Protocol Parser — C++ Implementation
 * Parser protokołu FIX 4.2 — implementacja C++
 *
 * Parses Financial Information eXchange (FIX) 4.2 messages used for
 * electronic trading communication between brokers and exchanges.
 * Analizuje wiadomości FIX 4.2 używane do elektronicznej komunikacji
 * handlowej pomiędzy brokerami i giełdami.
 *
 * FIX messages are pipe-delimited key=value pairs:
 *   "8=FIX.4.2|35=D|55=AAPL|54=1|44=150.25|38=100"
 * Like /etc/passwd fields separated by ':' — each field has a meaning.
 * Jak pola /etc/passwd rozdzielone ':' — każde pole ma znaczenie.
 *
 * Key FIX tags / Kluczowe tagi FIX:
 *   8  = BeginString (protocol version / wersja protokołu)
 *   35 = MsgType: D=NewOrder, G=Modify, F=Cancel, 8=Execution
 *   49 = SenderCompID (who sent it / kto wysłał)
 *   56 = TargetCompID (who receives / kto odbiera)
 *   55 = Symbol (stock ticker / symbol akcji)
 *   54 = Side: 1=Buy, 2=Sell
 *   44 = Price
 *   38 = OrderQty (quantity / ilość)
 *
 * Performance / Wydajność:
 *   Python: ~400K msg/sec (~2300ns/msg)
 *   C++:    ~20-40M msg/sec
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>

// Max FIX tags we store per message (most messages have <20 tags)
// Like a fixed-size hash table — avoids heap allocation
// Maks. tagów FIX na wiadomość (większość ma <20 tagów)
static constexpr int MAX_FIX_TAGS = 32;

// Max value length per tag
static constexpr int MAX_FIX_VALUE = 32;


// === FIXField — one tag=value pair ===

struct FIXField {
    int  tag;                       // FIX tag number (e.g., 35, 55, 44)
    char value[MAX_FIX_VALUE];      // tag value as string

    FIXField() noexcept : tag(0) { value[0] = '\0'; }
};


// === FIXMessage — parsed FIX message ===

class FIXMessage {
    FIXField fields_[MAX_FIX_TAGS];
    int      field_count_;

    // find_field: linear scan for a tag — O(N) but N is small (≤32)
    // Like 'grep "^tag=" message' — scan each field looking for matching tag
    // Jak 'grep "^tag=" message' — skanuj każde pole szukając pasującego tagu
    const FIXField* find_field(int tag) const noexcept {
        for (int i = 0; i < field_count_; ++i) {
            if (fields_[i].tag == tag) return &fields_[i];
        }
        return nullptr;
    }

    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

public:
    FIXMessage() noexcept : field_count_(0) {}

    // parse: split pipe-delimited message into tag=value pairs
    // Like 'IFS="|" read -ra fields <<< "$msg"' in bash, then split each by '='
    // Jak 'IFS="|" read -ra fields <<< "$msg"' w bashu, potem podziel każde przez '='
    // Returns parse time in nanoseconds / Zwraca czas parsowania w nanosekundach
    int64_t parse(const char* raw_msg) noexcept {
        int64_t t0 = now_ns();
        field_count_ = 0;

        if (!raw_msg || raw_msg[0] == '\0') {
            return now_ns() - t0;
        }

        // Work on a copy (we modify it with strtok-like scanning)
        // Pracuj na kopii (modyfikujemy ją skanowaniem w stylu strtok)
        char buf[1024];
        int len = std::strlen(raw_msg);
        if (len >= 1024) len = 1023;
        std::memcpy(buf, raw_msg, len);
        buf[len] = '\0';

        // Split by '|' delimiter — like awk -F'|'
        // Podziel po '|' — jak awk -F'|'
        char* pos = buf;
        while (*pos && field_count_ < MAX_FIX_TAGS) {
            // Find next '|' or end of string
            char* delim = pos;
            while (*delim && *delim != '|') delim++;

            // Find '=' within this segment
            char* eq = pos;
            while (eq < delim && *eq != '=') eq++;

            if (eq < delim && eq > pos) {
                // Null-terminate tag part
                *eq = '\0';

                // Parse tag number (atoi skips leading whitespace)
                int tag = std::atoi(pos);
                if (tag > 0) {
                    FIXField& f = fields_[field_count_];
                    f.tag = tag;

                    // Copy value (everything after '=')
                    const char* val_start = eq + 1;
                    int val_len = (int)(delim - val_start);
                    if (val_len >= MAX_FIX_VALUE) val_len = MAX_FIX_VALUE - 1;
                    if (val_len > 0) {
                        std::memcpy(f.value, val_start, val_len);
                    }
                    f.value[val_len] = '\0';
                    field_count_++;
                }
            }

            // Move past delimiter
            if (*delim == '|') delim++;
            pos = delim;
        }

        return now_ns() - t0;
    }

    // === Accessors for common FIX tags ===

    // get_msg_type: tag 35 — D=NewOrder, G=Modify, F=Cancel, 8=Execution
    const char* get_msg_type() const noexcept {
        const FIXField* f = find_field(35);
        return f ? f->value : "UNKNOWN";
    }

    // get_symbol: tag 55 — stock ticker (e.g., "AAPL")
    const char* get_symbol() const noexcept {
        const FIXField* f = find_field(55);
        return f ? f->value : "UNKNOWN";
    }

    // get_side: tag 54 — 1=BUY, 2=SELL
    const char* get_side() const noexcept {
        const FIXField* f = find_field(54);
        if (!f) return "UNKNOWN";
        return (f->value[0] == '1') ? "BUY" : "SELL";
    }

    // get_price: tag 44 — order price
    double get_price() const noexcept {
        const FIXField* f = find_field(44);
        return f ? std::atof(f->value) : 0.0;
    }

    // get_quantity: tag 38 — order quantity
    int32_t get_quantity() const noexcept {
        const FIXField* f = find_field(38);
        return f ? std::atoi(f->value) : 0;
    }

    // get_field: generic tag lookup (returns nullptr if not found)
    const char* get_field(int tag) const noexcept {
        const FIXField* f = find_field(tag);
        return f ? f->value : nullptr;
    }

    int field_count() const noexcept { return field_count_; }

    // print: display parsed message (for debugging)
    void print() const {
        const char* msg_type = get_msg_type();
        const char* type_name = "UNKNOWN";
        if (msg_type[0] == 'D') type_name = "NEW ORDER";
        else if (msg_type[0] == 'G') type_name = "MODIFY";
        else if (msg_type[0] == 'F') type_name = "CANCEL";
        else if (msg_type[0] == '8') type_name = "EXECUTION";
        else if (msg_type[0] == '0') type_name = "HEARTBEAT";

        printf("%s | %s %d %s @ %.2f\n",
               type_name, get_side(), get_quantity(), get_symbol(), get_price());
    }
};
