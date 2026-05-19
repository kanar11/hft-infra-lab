/*
 * LobsterReader — streams Nasdaq events from LOBSTER message CSV files.
 *
 * LOBSTER (https://lobsterdata.com) publishes reconstructed Nasdaq ITCH
 * order events as a plain CSV with one row per event. Each file covers
 * one symbol for one trading day; rows are sorted by timestamp.
 *
 * Message CSV format (no header row, 6 columns):
 *
 *   timestamp,event_type,order_id,size,price,direction
 *   34200.123456789,1,11885113,21,2238100,1
 *   34200.124567890,1,11885114,15,2238500,-1
 *   34200.125678901,4,11885113,10,2238100,1
 *
 *   timestamp   — seconds (and nanoseconds) since 09:30:00 ET
 *   event_type  — 1=submit, 2=partial cancel, 3=delete, 4=visible execution,
 *                 5=hidden execution, 6=cross trade, 7=halt
 *   order_id    — exchange order ID
 *   size        — shares (always positive)
 *   price       — dollars × 10000 (e.g. 2238100 = $223.81)
 *   direction   — +1 BUY, -1 SELL
 *
 * Use
 * ---
 *   LobsterReader r("/path/to/AAPL_2012-06-21_messages.csv");
 *   if (!r.is_open()) ...
 *   LobsterMessage m;
 *   while (r.next(m)) { ... }
 *
 * The reader owns the FILE*; no allocation on the per-row hot path.
 */
#pragma once

#include <cstdint>
#include <cstdio>


namespace lobster {

enum class EventType : std::uint8_t {
    SUBMIT          = 1,   // new limit order placed
    CANCEL_PARTIAL  = 2,   // partial cancellation
    DELETE          = 3,   // full order deletion
    EXECUTE_VISIBLE = 4,   // visible (lit) execution
    EXECUTE_HIDDEN  = 5,   // hidden execution
    CROSS_TRADE     = 6,   // auction cross
    HALT            = 7,   // trading halt
};


struct LobsterMessage {
    double       timestamp_s;   // seconds since 09:30:00 ET
    EventType    event_type;
    std::int64_t order_id;
    std::int32_t size;          // shares
    std::int32_t price;         // dollars × 10000
    std::int8_t  direction;     // +1 buy, -1 sell
};


class LobsterReader {
    std::FILE* fp_;
    std::uint64_t rows_read_ = 0;
    std::uint64_t rows_bad_  = 0;

public:
    explicit LobsterReader(const char* path) noexcept
        : fp_(std::fopen(path, "r")) {}

    ~LobsterReader() noexcept {
        if (fp_) std::fclose(fp_);
    }

    LobsterReader(const LobsterReader&)            = delete;
    LobsterReader& operator=(const LobsterReader&) = delete;
    LobsterReader(LobsterReader&&)                 = delete;
    LobsterReader& operator=(LobsterReader&&)      = delete;

    bool is_open() const noexcept { return fp_ != nullptr; }

    // next: read the next row. Returns false on EOF or unrecoverable error.
    // Malformed individual rows are skipped (counted in rows_bad()) and the
    // reader continues.
    bool next(LobsterMessage& out) noexcept {
        if (!fp_) return false;

        char line[256];
        while (std::fgets(line, sizeof(line), fp_)) {
            // Skip blank lines and obvious comments.
            if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') continue;

            double timestamp;
            int    event_type_raw;
            long   order_id;
            int    size;
            int    price;
            int    direction;
            const int n = std::sscanf(line, "%lf,%d,%ld,%d,%d,%d",
                                      &timestamp, &event_type_raw, &order_id,
                                      &size, &price, &direction);
            if (n != 6) {
                rows_bad_++;
                continue;
            }
            out.timestamp_s = timestamp;
            out.event_type  = static_cast<EventType>(event_type_raw);
            out.order_id    = static_cast<std::int64_t>(order_id);
            out.size        = static_cast<std::int32_t>(size);
            out.price       = static_cast<std::int32_t>(price);
            out.direction   = static_cast<std::int8_t>(direction);
            rows_read_++;
            return true;
        }
        return false;
    }

    std::uint64_t rows_read() const noexcept { return rows_read_; }
    std::uint64_t rows_bad()  const noexcept { return rows_bad_; }
};

}  // namespace lobster
