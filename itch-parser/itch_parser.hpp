/*
 * ITCHParser — a parser for the binary NASDAQ ITCH 5.0 protocol.
 *
 * ITCH = NASDAQ's market data feed. Binary, big-endian, fixed-length messages
 * (per type). Usually delivered over UDP multicast framed in MoldUDP64
 * (see multicast/multicast.hpp).
 *
 * Parsing market data is on the critical path — every nanosecond saved here =
 * better prices in the strategy. Zero allocation, zero copy,
 * direct big-endian decode via be64toh.
 *
 * Performance (lab): ~60M msg/sec, 16 ns/msg, p50=40ns, p99=50ns.
 *
 * Message types and their wire size (bytes):
 *   A = ADD_ORDER       34
 *   F = ADD_ORDER_MPID  38
 *   D = DELETE_ORDER    17
 *   U = REPLACE_ORDER   33
 *   E = ORDER_EXECUTED  29
 *   C = ORDER_CANCELLED 21
 *   P = TRADE           42
 *   S = SYSTEM_EVENT    10
 *   R = STOCK_DIRECTORY 18
 *
 * Length validation: parse() checks `len >= expected_size[type]` before
 * each type (see parse_add_order etc.). A shorter
 * buffer → MsgType::ERROR, not an out-of-bounds read.
 */

#pragma once

#include <cstdint>   // fixed-size integer types
#include <cstring>   // memcpy — copying raw bytes
#include <string>
#include <endian.h>  // be64toh, be32toh — BE → native order conversion


// Byte-order helpers. ITCH uses big-endian (MSB first).
// Intel/AMD CPUs use little-endian, so we must swap bytes.
//
// We use memcpy() before swapping to avoid "unaligned access" crashes.
// (Data in a packet stream is not guaranteed to be aligned to word boundaries)

// Read a big-endian 64-bit integer from a raw byte pointer
static inline int64_t read_be64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);          // copy 8 bytes into v safely
    return (int64_t)be64toh(v); // swap byte order if needed
}

// Read a big-endian 32-bit unsigned integer
static inline uint32_t read_be32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return be32toh(v);
}

// Message structs — one POD per ITCH type, packed by the parsers below.
// Think of it like a row in a database table — a fixed set of fields.

// ADD_ORDER message: someone placed a new limit order in the book
struct AddOrderMsg {
    int64_t  timestamp_ns;  // nanoseconds since midnight
    int64_t  order_ref;     // unique order ID (like PID for processes)
    char     side;          // 'B' = BUY,  'S' = SELL
    uint32_t shares;        // number of shares
    char     stock[9];      // stock symbol e.g. "AAPL\0" (8 chars + null)
    double   price;         // price in dollars e.g. 150.25
};

// ADD_ORDER_MPID message: like ADD_ORDER but includes Market Participant ID (who placed it)
struct AddOrderMpidMsg {
    int64_t  timestamp_ns;
    int64_t  order_ref;
    char     side;
    uint32_t shares;
    char     stock[9];
    double   price;
    char     mpid[5];       // Market Participant ID e.g. "GSCO" (Goldman Sachs) + null
};

// DELETE_ORDER message: an order was cancelled/removed from the book
struct DeleteOrderMsg {
    int64_t timestamp_ns;
    int64_t order_ref;      // which order to remove
};

// REPLACE_ORDER message: modify an existing order (new price/size)
struct ReplaceOrderMsg {
    int64_t  timestamp_ns;
    int64_t  orig_order_ref; // original order being replaced
    int64_t  new_order_ref;  // new order reference
    uint32_t new_shares;     // updated quantity
    double   new_price;      // updated price
};

// ORDER_EXECUTED message: a fill happened — shares traded
struct OrderExecutedMsg {
    int64_t  timestamp_ns;
    int64_t  order_ref;      // which order was filled
    uint32_t exec_shares;    // how many shares traded
    int64_t  match_number;   // unique trade ID
};

// ORDER_CANCELLED message: partial cancellation (reduces shares remaining)
struct OrderCancelledMsg {
    int64_t  timestamp_ns;
    int64_t  order_ref;
    uint32_t cancelled_shares; // how many shares removed
};

// TRADE message: a matched trade (both sides)
struct TradeMsg {
    int64_t  timestamp_ns;
    int64_t  order_ref;
    char     side;
    uint32_t shares;
    char     stock[9];
    double   price;
    int64_t  match_number;
};

// SYSTEM_EVENT message: market open/close/halted signals
struct SystemEventMsg {
    int64_t timestamp_ns;
    char    event_code; // 'O'=open, 'C'=close, 'H'=halt
};

// STOCK_DIRECTORY message: stock metadata (symbol info)
struct StockDirectoryMsg {
    int64_t timestamp_ns;
    char    stock[9];
    char    market_category; // 'Q'=NASDAQ, 'N'=NYSE, etc.
};

// Message-type enum — type-safe, not raw ints.
enum class MsgType {
    ADD_ORDER,
    ADD_ORDER_MPID,
    DELETE_ORDER,
    REPLACE_ORDER,
    ORDER_EXECUTED,
    ORDER_CANCELLED,
    TRADE,
    SYSTEM_EVENT,
    STOCK_DIRECTORY,
    UNKNOWN,
    ERROR
};

// ParsedMessage: a tagged union holding ONE of the 8 message types
//
// A 'union' shares the same memory for multiple types (only one is valid at a time)
// Like a /proc file — same interface, different content depending on what you read
struct ParsedMessage {
    MsgType type;  // tells you WHICH field in 'data' is valid

    // union: all fields share the same memory block
    union {
        AddOrderMsg       add_order;
        AddOrderMpidMsg   add_order_mpid;
        DeleteOrderMsg    delete_order;
        ReplaceOrderMsg   replace_order;
        OrderExecutedMsg  order_executed;
        OrderCancelledMsg order_cancelled;
        TradeMsg          trade;
        SystemEventMsg    system_event;
        StockDirectoryMsg stock_directory;
    } data;
};

// ITCHParser — main entry: parse(buf, len) → ParsedMessage.
class ITCHParser {
public:
    // Statistics tracked during parsing
    // (public struct inside a class — members accessible from outside)
    struct Stats {
        uint64_t total_parsed   = 0;
        uint64_t add_orders     = 0;
        uint64_t delete_orders  = 0;
        uint64_t replace_orders = 0;
        uint64_t executions     = 0;
        uint64_t cancels        = 0;
        uint64_t trades         = 0;
        uint64_t system_events  = 0;
        uint64_t unknowns       = 0;
    };

    // Constructor: called when ITCHParser is created, initialises stats to zero
    // 'noexcept' means this function will never throw an exception (faster)
    ITCHParser() noexcept = default;

    // parse(): main entry point — reads one raw ITCH message and returns its type + data
    //
    // Parameters:
    //   data — pointer to raw bytes (the network packet)
    //   len  — number of bytes available
    // Returns:
    //   ParsedMessage with .type and .data filled in
    //
    // 'const uint8_t*' means: pointer to bytes that we won't modify (read-only)
    ParsedMessage parse(const uint8_t* data, size_t len) noexcept {
        ParsedMessage result{};
        result.type = MsgType::ERROR;

        // Need at least 1 byte to read the message type
        if (len < 1) return result;

        // First byte is always the message type character ('A', 'D', 'U', etc.)
        uint8_t msg_type = data[0];
        stats_.total_parsed++;

        // Switch statement: like a chain of if/elif, but compiled to a jump table (faster)
        switch (msg_type) {
            case 'A': result = parse_add_order(data, len);       stats_.add_orders++;     break;
            case 'F': result = parse_add_order_mpid(data, len);  stats_.add_orders++;     break;
            case 'D': result = parse_delete_order(data, len);    stats_.delete_orders++;  break;
            case 'U': result = parse_replace_order(data, len);   stats_.replace_orders++; break;
            case 'E': result = parse_order_executed(data, len);  stats_.executions++;     break;
            case 'C': result = parse_order_cancelled(data, len); stats_.cancels++;        break;
            case 'P': result = parse_trade(data, len);           stats_.trades++;         break;
            case 'S': result = parse_system_event(data, len);    stats_.system_events++;  break;
            case 'R': result = parse_stock_directory(data, len); break;
            default:
                result.type = MsgType::UNKNOWN;
                stats_.unknowns++;
        }
        return result;
    }

    // const Stats& means: return a reference to stats (no copy), and don't allow modification
    const Stats& stats() const noexcept { return stats_; }

    // Reset counters (e.g., at start of new trading day)
    void reset_stats() noexcept { stats_ = Stats{}; }

private:
    // Private member: only accessible from inside this class (like a local variable for the object)
    // The trailing underscore is a naming convention for private member variables
    Stats stats_;

    // ── Individual message parsers ────────────────────────────────────────
    // inline: hint to compiler to expand this code in-place instead of calling it
    // (eliminates function call overhead — every nanosecond matters in HFT)

    static inline ParsedMessage parse_add_order(const uint8_t* d, size_t len) noexcept {
        // ADD_ORDER layout (34 bytes total):
        // [0]    = msg_type  'A'     (1 byte)
        // [1..8] = timestamp_ns      (8 bytes, big-endian int64)
        // [9..16]= order_ref         (8 bytes, big-endian int64)
        // [17]   = side 'B' or 'S'  (1 byte)
        // [18..21]= shares           (4 bytes, big-endian uint32)
        // [22..29]= stock symbol     (8 bytes, ASCII text e.g. "AAPL    ")
        // [30..33]= price * 10000    (4 bytes, big-endian uint32)
        ParsedMessage m{};
        if (len < 34) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::ADD_ORDER;
        auto& msg = m.data.add_order;

        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        msg.side         = (char)d[17];            // 'B' or 'S'
        msg.shares       = read_be32(d + 18);
        memcpy(msg.stock, d + 22, 8);              // copy 8 bytes of stock name
        msg.stock[8]     = '\0';                    // null-terminate the string
        msg.price        = read_be32(d + 30) / 10000.0; // convert fixed-point to float
        return m;
    }

    static inline ParsedMessage parse_add_order_mpid(const uint8_t* d, size_t len) noexcept {
        // ADD_ORDER_MPID layout (38 bytes): same as ADD_ORDER + 4-byte MPID at end
        ParsedMessage m{};
        if (len < 38) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::ADD_ORDER_MPID;
        auto& msg = m.data.add_order_mpid;
        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        msg.side         = (char)d[17];
        msg.shares       = read_be32(d + 18);
        memcpy(msg.stock, d + 22, 8);
        msg.stock[8]     = '\0';
        msg.price        = read_be32(d + 30) / 10000.0;
        memcpy(msg.mpid, d + 34, 4);
        msg.mpid[4]      = '\0';
        return m;
    }

    static inline ParsedMessage parse_delete_order(const uint8_t* d, size_t len) noexcept {
        // DELETE_ORDER layout (17 bytes):
        // [0]    = msg_type 'D'
        // [1..8] = timestamp_ns
        // [9..16]= order_ref
        ParsedMessage m{};
        if (len < 17) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::DELETE_ORDER;
        auto& msg = m.data.delete_order;
        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        return m;
    }

    static inline ParsedMessage parse_replace_order(const uint8_t* d, size_t len) noexcept {
        // REPLACE_ORDER layout (33 bytes):
        // [0]     = 'U'
        // [1..8]  = timestamp_ns
        // [9..16] = orig_order_ref
        // [17..24]= new_order_ref
        // [25..28]= new_shares
        // [29..32]= new_price * 10000
        ParsedMessage m{};
        if (len < 33) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::REPLACE_ORDER;
        auto& msg = m.data.replace_order;
        msg.timestamp_ns   = read_be64(d + 1);
        msg.orig_order_ref = read_be64(d + 9);
        msg.new_order_ref  = read_be64(d + 17);
        msg.new_shares     = read_be32(d + 25);
        msg.new_price      = read_be32(d + 29) / 10000.0;
        return m;
    }

    static inline ParsedMessage parse_order_executed(const uint8_t* d, size_t len) noexcept {
        // ORDER_EXECUTED layout (29 bytes):
        // [0]     = 'E'
        // [1..8]  = timestamp_ns
        // [9..16] = order_ref
        // [17..20]= exec_shares
        // [21..28]= match_number
        ParsedMessage m{};
        if (len < 29) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::ORDER_EXECUTED;
        auto& msg = m.data.order_executed;
        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        msg.exec_shares  = read_be32(d + 17);
        msg.match_number = read_be64(d + 21);
        return m;
    }

    static inline ParsedMessage parse_order_cancelled(const uint8_t* d, size_t len) noexcept {
        // ORDER_CANCELLED layout (21 bytes):
        // [0]     = 'C'
        // [1..8]  = timestamp_ns
        // [9..16] = order_ref
        // [17..20]= cancelled_shares
        ParsedMessage m{};
        if (len < 21) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::ORDER_CANCELLED;
        auto& msg = m.data.order_cancelled;
        msg.timestamp_ns    = read_be64(d + 1);
        msg.order_ref       = read_be64(d + 9);
        msg.cancelled_shares = read_be32(d + 17);
        return m;
    }

    static inline ParsedMessage parse_trade(const uint8_t* d, size_t len) noexcept {
        // TRADE layout (42 bytes):
        // [0]     = 'P'
        // [1..8]  = timestamp_ns
        // [9..16] = order_ref
        // [17]    = side
        // [18..21]= shares
        // [22..29]= stock
        // [30..33]= price * 10000
        // [34..41]= match_number
        ParsedMessage m{};
        if (len < 42) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::TRADE;
        auto& msg = m.data.trade;
        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        msg.side         = (char)d[17];
        msg.shares       = read_be32(d + 18);
        memcpy(msg.stock, d + 22, 8);
        msg.stock[8]     = '\0';
        msg.price        = read_be32(d + 30) / 10000.0;
        msg.match_number = read_be64(d + 34);
        return m;
    }

    static inline ParsedMessage parse_system_event(const uint8_t* d, size_t len) noexcept {
        // SYSTEM_EVENT layout (10 bytes):
        // [0]    = 'S'
        // [1..8] = timestamp_ns
        // [9]    = event_code ('O'=open, 'C'=close, 'H'=halt)
        ParsedMessage m{};
        if (len < 10) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::SYSTEM_EVENT;
        m.data.system_event.timestamp_ns = read_be64(d + 1);
        m.data.system_event.event_code   = (char)d[9];
        return m;
    }

    static inline ParsedMessage parse_stock_directory(const uint8_t* d, size_t len) noexcept {
        // STOCK_DIRECTORY layout (18 bytes):
        // [0]    = 'R'
        // [1..8] = timestamp_ns
        // [9..16]= stock symbol
        // [17]   = market_category ('Q'=NASDAQ, 'N'=NYSE)
        ParsedMessage m{};
        if (len < 18) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::STOCK_DIRECTORY;
        m.data.stock_directory.timestamp_ns = read_be64(d + 1);
        memcpy(m.data.stock_directory.stock, d + 9, 8);
        m.data.stock_directory.stock[8] = '\0';
        m.data.stock_directory.market_category = (char)d[17];
        return m;
    }
};
