/*
 * Trade Logger / Audit Trail — C++ Implementation
 * Logger transakcji / Ścieżka audytu — implementacja C++
 *
 * Regulatory compliance: SEC Rule 17a-4, MiFID II RTS 25 require a complete,
 * timestamped, durable audit trail for every order event.
 * Zgodność regulacyjna: SEC Rule 17a-4, MiFID II RTS 25 wymagają pełnej,
 * znacznikowanej czasowo, trwałej ścieżki audytu dla każdego zdarzenia zlecenia.
 *
 * Design: SPSC ring buffer in shared memory.
 *   - Trading thread  → writes events via log() [hot path, O(1), no allocation]
 *   - Flush thread    → drains ring to binary file [off hot path]
 *   - Query thread    → reads snapshot for monitoring / regulatory export
 *
 * Two clocks per event (required by MiFID II RTS 25 Article 2):
 *   mono_ns  — CLOCK_MONOTONIC, for latency measurement (never jumps)
 *   wall_ns  — CLOCK_REALTIME,  for regulatory reporting (UTC epoch ns)
 *
 * Binary file format: FlushFileHeader (64 bytes) + N × TradeEvent (128 bytes each)
 * This is seekable, appendable, and parseable offline without the process running.
 *
 * Pipeline: Strategy → Router → Risk → OMS → **Logger** (records everything)
 *
 * Performance / Wydajność:
 *   log() hot path:  ~15-25M events/sec  (~40-60 ns/event)
 *   flush thread:    ~5-8M records/sec   to binary file
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <unordered_set>
#include <string>

// Max events in the in-memory ring buffer.
// At 128 bytes/event: 1M events = 128 MB — allocated once at startup.
// Maks. zdarzeń w buforze pierścieniowym: 1M × 128 bajtów = 128 MB
static constexpr int MAX_EVENTS = 1'000'000;

// Max unique orders/symbols tracked in summary stats (for unordered_set reserve)
static constexpr int MAX_TRACKED_IDS = 4096;


// ============================================================
// EventType — what happened to an order
// ============================================================

enum class EventType : uint8_t {
    ORDER_SUBMIT  = 0,   // strategy submits order to OMS
    RISK_ACCEPT   = 1,   // risk manager approved
    RISK_REJECT   = 2,   // risk manager blocked
    ORDER_FILL    = 3,   // exchange fully filled the order
    ORDER_PARTIAL = 4,   // partial fill received
    ORDER_CANCEL  = 5,   // order cancelled
    KILL_SWITCH   = 6,   // emergency halt — all trading stopped
    SYSTEM_START  = 7,   // trading session started
    SYSTEM_STOP   = 8,   // trading session ended
    EVENT_COUNT   = 9    // sentinel — number of event types
};

inline const char* event_type_str(EventType t) noexcept {
    static const char* names[] = {
        "ORDER_SUBMIT", "RISK_ACCEPT", "RISK_REJECT",
        "ORDER_FILL", "ORDER_PARTIAL", "ORDER_CANCEL",
        "KILL_SWITCH", "SYSTEM_START", "SYSTEM_STOP"
    };
    int idx = static_cast<int>(t);
    if (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
        return names[idx];
    return "UNKNOWN";
}


// ============================================================
// TradeEvent — one immutable record in the audit trail
//
// Exactly 128 bytes (2 cache lines). Fields ordered to avoid padding.
// alignas(64): each event starts on a cache-line boundary so the
// flush thread can safely read events_ entries while the trading
// thread writes the next slot (no false sharing between adjacent events).
// ============================================================

struct alignas(64) TradeEvent {
    // --- 8-byte fields first (no internal padding) ---
    int64_t  mono_ns;       // CLOCK_MONOTONIC ns — for latency measurement
    int64_t  wall_ns;       // CLOCK_REALTIME ns  — UTC epoch, for regulators (MiFID II)
    uint64_t sequence_no;   // global monotonic counter — regulators use this to detect gaps
    uint64_t order_id;      // matches OMS::Order::order_id (uint64_t, not int32_t!)
    double   price;         // price per share as float for display (OMS stores fixed-point)

    // --- 4-byte ---
    int32_t  quantity;

    // --- 1-byte + char arrays (packed, no padding between them) ---
    EventType event_type;   // 1 byte
    char      symbol[9];    // "AAPL" + null (NASDAQ max 8 chars)
    char      side[5];      // "BUY"/"SELL" + null
    char      details[69];  // free-form context (venue, reject reason, etc.)
    // Layout: 8+8+8+8+8 + 4 + 1+9+5+69 = 128 bytes exactly — 2 cache lines
};

static_assert(sizeof(TradeEvent) == 128, "TradeEvent must be exactly 128 bytes");


// ============================================================
// Binary file format (flush file)
//
// FlushFileHeader (64 bytes) followed by N × TradeEvent (128 bytes each).
// The file is append-only: each run opens with O_APPEND and writes
// a new header + records. Offline tools can parse by scanning for headers.
// ============================================================

struct FlushFileHeader {
    char     magic[8];       // "HFTLOG\0\0" — identifies the file type
    uint32_t version;        // format version = 1
    uint32_t record_size;    // sizeof(TradeEvent) = 128
    int64_t  created_wall_ns;// UTC wall clock when this session opened
    char     padding[40];    // reserved for future use — 8+4+4+8+40 = 64 bytes
};

static_assert(sizeof(FlushFileHeader) == 64, "FlushFileHeader must be 64 bytes");


// ============================================================
// TradeLogger — SPSC ring buffer with async binary flush
//
// Single writer (trading thread) + single reader (flush thread).
// Query methods (unique_orders, print_summary, etc.) are safe to call
// from a third thread only if log() is not concurrently running,
// i.e. between trading sessions or with external synchronization.
// ============================================================

class TradeLogger {
    // Ring buffer — allocated once, never reallocated
    std::unique_ptr<TradeEvent[]> events_;
    int max_events_;

    // SPSC atomics — written by trading thread, read by flush thread.
    // head_: next slot to write (trading thread increments after writing).
    // Use release/acquire pair so flush thread sees complete event data.
    alignas(64) std::atomic<int> head_{0};       // trading thread writes
    alignas(64) std::atomic<int> flush_tail_{0}; // flush thread reads (its own counter)

    // Non-atomic counters — only accessed by trading thread (or single-threaded queries)
    // 64-bit so they survive long sessions: at 25M evt/s, int32 wraps in ~85s.
    uint64_t sequence_{0};       // monotonic counter embedded into each event
    uint64_t total_logged_{0};   // total events ever passed to log() (doesn't decrease on wrap)
    bool ring_mode_;

    // Per-type counters (updated by trading thread only)
    int counters_[static_cast<int>(EventType::EVENT_COUNT)]{};

    // Async flush state
    std::thread       flush_thread_;
    std::atomic<bool> flush_running_{false};
    FILE*             flush_file_{nullptr};

    // Two-clock timestamping: monotonic for latency, wall for regulators
    static int64_t mono_ns() noexcept {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
    }

    static int64_t wall_ns_now() noexcept {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
    }

    // drain_to_file: write contiguous spans from t..h to flush_file_, returns final t.
    // Batches into one fwrite per contiguous range — one syscall per drain instead of N.
    int drain_to_file(int t, int h) noexcept {
        while (t != h) {
            int slot   = ring_mode_ ? (t % max_events_) : t;
            int contig = ring_mode_ ? std::min(max_events_ - slot, h - t) : (h - t);
            std::fwrite(&events_[slot], sizeof(TradeEvent), contig, flush_file_);
            t += contig;
        }
        return t;
    }

    // flush_loop: background thread — drains ring buffer to binary file
    // Reads up to head_ (acquire), writes raw TradeEvent records to disk.
    void flush_loop() noexcept {
        while (true) {
            int h = head_.load(std::memory_order_acquire);
            int t = flush_tail_.load(std::memory_order_relaxed);
            if (t != h) {
                t = drain_to_file(t, h);
                flush_tail_.store(t, std::memory_order_relaxed);
            }

            if (!flush_running_.load(std::memory_order_relaxed)) break;

            // Brief sleep to avoid busy-spinning when no events are arriving.
            // In production: use a futex or condition variable here instead.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        // Final drain after flush_running_ = false (publisher may still race in)
        int h = head_.load(std::memory_order_acquire);
        int t = drain_to_file(flush_tail_.load(std::memory_order_relaxed), h);
        flush_tail_.store(t, std::memory_order_relaxed);
        std::fflush(flush_file_);
    }

public:
    explicit TradeLogger(bool ring_mode = false, int capacity = MAX_EVENTS)
        : events_(std::make_unique<TradeEvent[]>(capacity)),
          max_events_(capacity), ring_mode_(ring_mode) {
        std::memset(counters_, 0, sizeof(counters_));
    }

    ~TradeLogger() {
        stop_async_flush();
    }

    TradeLogger(const TradeLogger&)            = delete;
    TradeLogger& operator=(const TradeLogger&) = delete;
    // Move would have to transfer flush_thread_, flush_file_, flush_running_
    // and atomically resync head_/flush_tail_. Not worth the complexity
    // for a trading-thread-pinned object — disallow.
    TradeLogger(TradeLogger&&)                 = delete;
    TradeLogger& operator=(TradeLogger&&)      = delete;

    // --------------------------------------------------------
    // start_async_flush: open binary file and start flush thread.
    // Call once at session start. File is appended, not truncated —
    // each session adds a header followed by its records.
    // --------------------------------------------------------
    bool start_async_flush(const char* filepath) noexcept {
        flush_file_ = std::fopen(filepath, "ab");
        if (!flush_file_) return false;

        // Write session header so offline parsers know where this session starts
        FlushFileHeader hdr{};
        std::memcpy(hdr.magic, "HFTLOG\0\0", 8);
        hdr.version      = 1;
        hdr.record_size  = sizeof(TradeEvent);
        hdr.created_wall_ns = wall_ns_now();
        std::fwrite(&hdr, sizeof(hdr), 1, flush_file_);
        std::fflush(flush_file_);

        flush_running_.store(true, std::memory_order_relaxed);
        flush_thread_ = std::thread([this]() { flush_loop(); });
        return true;
    }

    // stop_async_flush: signal flush thread to drain and exit, then join.
    // Blocks until all pending events are written to disk.
    void stop_async_flush() noexcept {
        if (flush_thread_.joinable()) {
            flush_running_.store(false, std::memory_order_relaxed);
            flush_thread_.join();
        }
        if (flush_file_) {
            std::fclose(flush_file_);
            flush_file_ = nullptr;
        }
    }

    // --------------------------------------------------------
    // log: record a trade event — the HOT PATH (O(1), no allocation)
    //
    // Captures both CLOCK_MONOTONIC and CLOCK_REALTIME at call time.
    // Returns the embedded sequence_no (>0) on success, or 0 if the
    // non-ring buffer is full. We don't return a pointer to the slot
    // because in ring mode the slot would be overwritten by later
    // log() calls — callers can only safely use the sequence number.
    // --------------------------------------------------------
    uint64_t log(EventType type, uint64_t order_id = 0,
                 const char* symbol = "", const char* side = "",
                 int32_t quantity = 0, double price = 0.0,
                 const char* details = "") noexcept {
        int head = head_.load(std::memory_order_relaxed);

        if (!ring_mode_ && head >= max_events_) return 0;

        int slot = ring_mode_ ? (head % max_events_) : head;

        TradeEvent& e = events_[slot];
        e.mono_ns     = mono_ns();
        e.wall_ns     = wall_ns_now();
        e.sequence_no = ++sequence_;
        e.order_id    = order_id;
        e.price       = price;
        e.quantity    = quantity;
        e.event_type  = type;

        std::strncpy(e.symbol,  symbol,  8);  e.symbol[8]   = '\0';
        std::strncpy(e.side,    side,    4);  e.side[4]     = '\0';
        std::strncpy(e.details, details, 68); e.details[68] = '\0';

        int idx = static_cast<int>(type);
        if (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
            counters_[idx]++;

        total_logged_++;

        // Release store: flush thread won't read this slot until it sees head_+1
        head_.store(head + 1, std::memory_order_release);
        return e.sequence_no;
    }

    // --------------------------------------------------------
    // Accessors
    // --------------------------------------------------------
    int total_events()  const noexcept {
        int h = head_.load(std::memory_order_relaxed);
        return ring_mode_ ? std::min(h, max_events_) : h;
    }
    uint64_t total_logged() const noexcept { return total_logged_; }
    uint64_t sequence()     const noexcept { return sequence_; }
    int capacity()      const noexcept { return max_events_; }
    bool is_ring_mode() const noexcept { return ring_mode_; }
    bool buffer_full()  const noexcept {
        return head_.load(std::memory_order_relaxed) >= max_events_;
    }

    int get_counter(EventType type) const noexcept {
        int idx = static_cast<int>(type);
        return (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
               ? counters_[idx] : 0;
    }

    // --------------------------------------------------------
    // Query methods — safe to call between sessions (not concurrently with log())
    // --------------------------------------------------------
    int get_events(const TradeEvent** out) const noexcept {
        *out = events_.get();
        return total_events();
    }

    int get_events_filtered(TradeEvent* out, int max_out,
                            uint64_t filter_order_id = 0,
                            int filter_type = -1,
                            const char* filter_symbol = nullptr) const noexcept {
        int count = 0;
        int n = total_events();
        for (int i = 0; i < n && count < max_out; ++i) {
            const TradeEvent& e = events_[i];
            if (filter_order_id > 0 && e.order_id != filter_order_id) continue;
            if (filter_type >= 0 && static_cast<int>(e.event_type) != filter_type) continue;
            if (filter_symbol && std::strcmp(e.symbol, filter_symbol) != 0) continue;
            out[count++] = e;
        }
        return count;
    }

    int get_order_trail(uint64_t order_id, TradeEvent* out, int max_out) const noexcept {
        return get_events_filtered(out, max_out, order_id);
    }

    // unique_orders / unique_symbols: O(N) — call offline, not on hot path
    int unique_orders() const {
        std::unordered_set<uint64_t> ids;
        ids.reserve(std::min(total_events(), MAX_TRACKED_IDS));
        int n = total_events();
        for (int i = 0; i < n; ++i)
            if (events_[i].order_id > 0) ids.insert(events_[i].order_id);
        return static_cast<int>(ids.size());
    }

    int unique_symbols() const {
        std::unordered_set<std::string> syms;
        syms.reserve(std::min(total_events(), MAX_TRACKED_IDS));
        int n = total_events();
        for (int i = 0; i < n; ++i)
            if (events_[i].symbol[0] != '\0') syms.emplace(events_[i].symbol);
        return static_cast<int>(syms.size());
    }

    // time_span_ms: uses mono_ns (monotonic) — not affected by NTP clock adjustments
    double time_span_ms() const noexcept {
        int n = total_events();
        if (n < 2) return 0.0;
        int64_t span = events_[n - 1].mono_ns - events_[0].mono_ns;
        return static_cast<double>(span) / 1'000'000.0;
    }

    // --------------------------------------------------------
    // Time-range query using wall_ns (UTC) — for regulatory replay
    // e.g. "show me all events between 09:30:00.000 and 09:30:00.100 UTC"
    // --------------------------------------------------------
    int get_events_in_range(TradeEvent* out, int max_out,
                            int64_t start_wall_ns, int64_t end_wall_ns) const noexcept {
        int count = 0;
        int n = total_events();
        for (int i = 0; i < n && count < max_out; ++i) {
            const TradeEvent& e = events_[i];
            if (e.wall_ns >= start_wall_ns && e.wall_ns <= end_wall_ns)
                out[count++] = e;
        }
        return count;
    }

    // --------------------------------------------------------
    // Latency statistics — uses mono_ns (correct for HW timestamping)
    // --------------------------------------------------------
    struct LatencyStats {
        int     count;
        int64_t min_ns, max_ns, mean_ns, p50_ns, p90_ns, p99_ns;
    };

    LatencyStats get_latency_stats() const noexcept {
        LatencyStats stats{};
        int n = total_events();
        if (n < 2) return stats;

        int cnt = n - 1;
        std::vector<int64_t> latencies(cnt);
        int64_t sum = 0;
        for (int i = 0; i < cnt; ++i) {
            latencies[i] = events_[i + 1].mono_ns - events_[i].mono_ns;
            sum += latencies[i];
        }
        std::sort(latencies.begin(), latencies.end());

        stats.count  = cnt;
        stats.min_ns = latencies[0];
        stats.max_ns = latencies[cnt - 1];
        stats.mean_ns = sum / cnt;
        stats.p50_ns = latencies[cnt / 2];
        stats.p90_ns = latencies[(int)(cnt * 0.90)];
        stats.p99_ns = latencies[std::min((int)(cnt * 0.99), cnt - 1)];
        return stats;
    }

    // --------------------------------------------------------
    // Reporting — call offline, not on hot path
    // --------------------------------------------------------
    void print_summary() const {
        printf("\n=== Logger Summary ===\n");
        printf("  Total events:   %d\n", total_events());
        printf("  Total logged:   %lu\n", (unsigned long)total_logged_);
        printf("  Unique orders:  %d\n", unique_orders());
        printf("  Unique symbols: %d\n", unique_symbols());
        printf("  Time span:      %.3f ms\n", time_span_ms());
        printf("  Event counts:\n");
        for (int i = 0; i < static_cast<int>(EventType::EVENT_COUNT); ++i)
            if (counters_[i] > 0)
                printf("    %-20s %d\n", event_type_str(static_cast<EventType>(i)), counters_[i]);
    }

    void print_order_trail(uint64_t order_id) const {
        printf("--- Order #%lu Audit Trail ---\n", (unsigned long)order_id);
        int n = total_events();
        for (int i = 0; i < n; ++i) {
            if (events_[i].order_id != order_id) continue;
            const TradeEvent& e = events_[i];
            printf("  seq=%-8lu  %-15s  %s  %s  qty=%d  px=%.4f  %s\n",
                   (unsigned long)e.sequence_no,
                   event_type_str(e.event_type),
                   e.symbol, e.side, e.quantity, e.price, e.details);
        }
    }

    // export_binary: synchronous bulk write of all in-memory events to a binary file.
    // Use this for end-of-session archival when async flush is not running.
    bool export_binary(const char* filepath) const {
        FILE* f = std::fopen(filepath, "ab");
        if (!f) return false;
        FlushFileHeader hdr{};
        std::memcpy(hdr.magic, "HFTLOG\0\0", 8);
        hdr.version         = 1;
        hdr.record_size     = sizeof(TradeEvent);
        hdr.created_wall_ns = wall_ns_now();
        std::fwrite(&hdr, sizeof(hdr), 1, f);
        int n = total_events();
        std::fwrite(events_.get(), sizeof(TradeEvent), n, f);
        std::fclose(f);
        return true;
    }

    // export_csv / export_json: offline reporting, includes both timestamps
    void export_csv()                                const { export_csv_to(stdout); }
    void export_json()                               const { export_json_to(stdout); }
    bool export_csv_to_file(const char* filepath)    const { return write_to_file(filepath, &TradeLogger::export_csv_to); }
    bool export_json_to_file(const char* filepath)   const { return write_to_file(filepath, &TradeLogger::export_json_to); }

private:
    static void write_csv_row(FILE* f, const TradeEvent& e) noexcept {
        fprintf(f, "%lu,%ld,%ld,%s,%lu,%s,%s,%d,%.4f,%s\n",
                (unsigned long)e.sequence_no, e.mono_ns, e.wall_ns,
                event_type_str(e.event_type),
                (unsigned long)e.order_id, e.symbol, e.side,
                e.quantity, e.price, e.details);
    }
    static void write_json_obj(FILE* f, const TradeEvent& e, bool last) noexcept {
        fprintf(f, "    {\"seq\": %lu, \"mono_ns\": %ld, \"wall_ns\": %ld, "
                "\"event_type\": \"%s\", \"order_id\": %lu, "
                "\"symbol\": \"%s\", \"side\": \"%s\", "
                "\"quantity\": %d, \"price\": %.4f, \"details\": \"%s\"}%s\n",
                (unsigned long)e.sequence_no, e.mono_ns, e.wall_ns,
                event_type_str(e.event_type), (unsigned long)e.order_id,
                e.symbol, e.side, e.quantity, e.price, e.details,
                last ? "" : ",");
    }

    void export_csv_to(FILE* f) const {
        fprintf(f, "seq,mono_ns,wall_ns,event_type,order_id,symbol,side,quantity,price,details\n");
        int n = total_events();
        for (int i = 0; i < n; ++i) write_csv_row(f, events_[i]);
    }
    void export_json_to(FILE* f) const {
        int n = total_events();
        fprintf(f, "{\n  \"total_logged\": %lu,\n  \"events_in_buffer\": %d,\n  \"events\": [\n",
                (unsigned long)total_logged_, n);
        for (int i = 0; i < n; ++i) write_json_obj(f, events_[i], i == n - 1);
        fprintf(f, "  ]\n}\n");
    }

    // write_to_file: open in "w", invoke a member writer, close. Returns false on fopen failure.
    bool write_to_file(const char* filepath, void (TradeLogger::*writer)(FILE*) const) const {
        FILE* f = std::fopen(filepath, "w");
        if (!f) return false;
        (this->*writer)(f);
        std::fclose(f);
        return true;
    }
};
