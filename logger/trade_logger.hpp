/*
 * Trade Logger / Audit Trail — C++ Implementation
 * Logger transakcji / Ścieżka audytu — implementacja C++
 *
 * Logs every trade event with nanosecond timestamps for regulatory compliance.
 * Loguje każde zdarzenie handlowe z nanosekundowymi znacznikami czasu dla zgodności regulacyjnej.
 *
 * In real HFT, regulators (SEC, MiFID II) require a complete audit trail:
 * "Show me exactly what happened to order #1234, in order, with timestamps."
 * W prawdziwym HFT regulatorzy (SEC, MiFID II) wymagają pełnej ścieżki audytu:
 * "Pokaż dokładnie co się stało ze zleceniem #1234, w kolejności, ze znacznikami czasu."
 *
 * Pipeline: Strategy → Router → Risk → OMS → **Logger** (records everything)
 *
 * Performance / Wydajność:
 *   Python: ~200K events/sec
 *   C++:    ~15-25M events/sec
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>

// Max events we store in memory (fixed array, no heap allocation on hot path)
// Like a pre-allocated ring buffer — when full, oldest events are overwritten
// Maks. zdarzeń w pamięci (stała tablica, bez alokacji na stercie na gorącej ścieżce)
// Jak wstępnie zaalokowany bufor pierścieniowy — gdy pełny, najstarsze zdarzenia są nadpisywane
static constexpr int MAX_EVENTS = 1'000'000;

// Max unique orders/symbols we track in summary stats
// Maks. unikalnych zleceń/symboli w statystykach
static constexpr int MAX_TRACKED_IDS = 4096;


// === EventType — what happened ===
// Like syslog severity levels, but for trading events
// Jak poziomy syslog, ale dla zdarzeń handlowych

enum class EventType : uint8_t {
    ORDER_SUBMIT  = 0,   // strategy sends an order / strategia wysyła zlecenie
    RISK_ACCEPT   = 1,   // risk manager approved / menedżer ryzyka zatwierdził
    RISK_REJECT   = 2,   // risk manager blocked / menedżer ryzyka zablokował
    ORDER_FILL    = 3,   // exchange filled order / giełda zrealizowała zlecenie
    ORDER_PARTIAL = 4,   // partial fill / częściowa realizacja
    ORDER_CANCEL  = 5,   // order cancelled / zlecenie anulowane
    KILL_SWITCH   = 6,   // emergency stop / wyłącznik awaryjny
    SYSTEM_START  = 7,   // session started / sesja rozpoczęta
    SYSTEM_STOP   = 8,   // session ended / sesja zakończona
    EVENT_COUNT   = 9    // sentinel — total number of event types
};

// Convert EventType to string (for printing)
// Like a lookup table — O(1), no allocation
// Konwertuj EventType na string (do drukowania)
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


// === TradeEvent — one row in the audit trail ===
// Fixed-size struct: no std::string, no heap — fits in cache line
// Struktura o stałym rozmiarze: bez std::string, bez sterty — mieści się w linii cache

struct TradeEvent {
    int64_t   timestamp_ns;     // nanosecond timestamp / znacznik czasu w nanosekundach
    EventType event_type;       // what happened / co się stało
    int32_t   order_id;         // order identifier / identyfikator zlecenia
    char      symbol[9];        // stock ticker, e.g. "AAPL" / symbol giełdowy
    char      side[8];          // "BUY" or "SELL" / "BUY" lub "SELL"
    int32_t   quantity;         // number of shares / liczba akcji
    double    price;            // price per share / cena za akcję
    char      details[64];      // extra info / dodatkowe informacje

    TradeEvent() noexcept
        : timestamp_ns(0), event_type(EventType::ORDER_SUBMIT),
          order_id(0), quantity(0), price(0.0) {
        symbol[0] = '\0';
        side[0] = '\0';
        details[0] = '\0';
    }
};


// === TradeLogger — the main audit trail engine ===
// Stores events in a flat array — O(1) append, O(N) query
// In real HFT: lock-free ring buffer + separate flush thread
// Przechowuje zdarzenia w płaskiej tablicy — O(1) dopisanie, O(N) zapytanie
// W prawdziwym HFT: bezblokadowy bufor pierścieniowy + oddzielny wątek zrzutu

class TradeLogger {
    // Events stored on heap — too large for stack (1M × ~120 bytes ≈ 120MB)
    // Like mmap() in Linux — we allocate a big chunk once at startup, then fill it
    // Zdarzenia na stercie — za duże na stos (1M × ~120 bajtów ≈ 120MB)
    // Jak mmap() w Linuxie — alokujemy duży blok raz na starcie, potem wypełniamy
    TradeEvent* events_;
    int         max_events_;
    int         event_count_;
    int         head_;           // ring buffer write position / pozycja zapisu bufora pierścieniowego
    int         sequence_;
    int         total_logged_;   // total events ever logged (doesn't decrease on wrap)
    bool        ring_mode_;      // true = wrap around when full; false = stop when full

    // Per-type counters (like 'wc -l' per event type)
    // Liczniki per typ (jak 'wc -l' per typ zdarzenia)
    int counters_[static_cast<int>(EventType::EVENT_COUNT)];

    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

public:
    // ring_mode: if true, oldest events are overwritten when buffer is full
    //            if false (default), log() returns nullptr when full
    // ring_mode: jeśli true, najstarsze zdarzenia nadpisywane gdy bufor pełny
    //            jeśli false (domyślnie), log() zwraca nullptr gdy pełny
    explicit TradeLogger(bool ring_mode = false, int capacity = MAX_EVENTS)
        : max_events_(capacity), event_count_(0), head_(0),
          sequence_(0), total_logged_(0), ring_mode_(ring_mode) {
        events_ = new TradeEvent[max_events_];
        std::memset(counters_, 0, sizeof(counters_));
    }

    ~TradeLogger() {
        delete[] events_;
    }

    // No copy (large buffer) — like unique_ptr semantics
    // Bez kopiowania (duży bufor) — jak semantyka unique_ptr
    TradeLogger(const TradeLogger&) = delete;
    TradeLogger& operator=(const TradeLogger&) = delete;

    // Move constructor — transfer ownership of the buffer
    TradeLogger(TradeLogger&& other) noexcept
        : events_(other.events_), max_events_(other.max_events_),
          event_count_(other.event_count_), head_(other.head_),
          sequence_(other.sequence_), total_logged_(other.total_logged_),
          ring_mode_(other.ring_mode_) {
        std::memcpy(counters_, other.counters_, sizeof(counters_));
        other.events_ = nullptr;
        other.event_count_ = 0;
    }

    TradeLogger& operator=(TradeLogger&& other) noexcept {
        if (this != &other) {
            delete[] events_;
            events_ = other.events_;
            max_events_ = other.max_events_;
            event_count_ = other.event_count_;
            head_ = other.head_;
            sequence_ = other.sequence_;
            total_logged_ = other.total_logged_;
            ring_mode_ = other.ring_mode_;
            std::memcpy(counters_, other.counters_, sizeof(counters_));
            other.events_ = nullptr;
            other.event_count_ = 0;
        }
        return *this;
    }

    // log: record a trade event — the hot-path function
    // O(1) — just writes to the next slot in the array
    // In ring_mode, wraps around when full instead of stopping
    // log: zapisz zdarzenie handlowe — funkcja na gorącej ścieżce
    // O(1) — po prostu zapisuje do następnego slotu w tablicy
    // W ring_mode, zawija się gdy pełny zamiast się zatrzymywać
    TradeEvent* log(EventType type, int32_t order_id = 0,
                    const char* symbol = "", const char* side = "",
                    int32_t quantity = 0, double price = 0.0,
                    const char* details = "") noexcept {
        if (event_count_ >= max_events_ && !ring_mode_) return nullptr;  // buffer full, not ring mode

        int slot;
        if (ring_mode_) {
            slot = head_ % max_events_;
            head_ = (head_ + 1) % max_events_;
            if (event_count_ < max_events_) event_count_++;
        } else {
            slot = event_count_;
            event_count_++;
        }

        TradeEvent& e = events_[slot];
        e.timestamp_ns = now_ns();
        e.event_type = type;
        e.order_id = order_id;
        e.quantity = quantity;
        e.price = price;

        std::strncpy(e.symbol, symbol, 8);
        e.symbol[8] = '\0';
        std::strncpy(e.side, side, 7);
        e.side[7] = '\0';
        std::strncpy(e.details, details, 63);
        e.details[63] = '\0';

        // Update counter for this event type
        int idx = static_cast<int>(type);
        if (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
            counters_[idx]++;

        sequence_++;
        total_logged_++;
        return &e;
    }

    // get_events: return pointer to events array + count
    // Like reading /var/log/messages — gives you the raw log
    // Jak czytanie /var/log/messages — daje surowy log
    int get_events(const TradeEvent** out) const noexcept {
        *out = events_;
        return event_count_;
    }

    // get_events_filtered: copy matching events to output buffer
    // filter_order_id: -1 = all, filter_type: -1 = all, filter_symbol: nullptr = all
    // Like 'grep' with multiple conditions piped together
    // Jak 'grep' z wieloma warunkami połączonymi potokiem
    int get_events_filtered(TradeEvent* out, int max_out,
                            int32_t filter_order_id = -1,
                            int filter_type = -1,
                            const char* filter_symbol = nullptr) const noexcept {
        int count = 0;
        for (int i = 0; i < event_count_ && count < max_out; ++i) {
            const TradeEvent& e = events_[i];
            if (filter_order_id >= 0 && e.order_id != filter_order_id) continue;
            if (filter_type >= 0 && static_cast<int>(e.event_type) != filter_type) continue;
            if (filter_symbol && std::strcmp(e.symbol, filter_symbol) != 0) continue;
            out[count++] = e;
        }
        return count;
    }

    // get_order_trail: get all events for a specific order
    // This is what regulators ask for: "Show me order #1234's lifecycle"
    // To jest to, o co pytają regulatorzy: "Pokaż cykl życia zlecenia #1234"
    int get_order_trail(int32_t order_id, TradeEvent* out, int max_out) const noexcept {
        return get_events_filtered(out, max_out, order_id);
    }

    // get_counter: how many events of a given type
    // Ile zdarzeń danego typu
    int get_counter(EventType type) const noexcept {
        int idx = static_cast<int>(type);
        if (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
            return counters_[idx];
        return 0;
    }

    // Accessors
    int total_events() const noexcept { return event_count_; }
    int total_logged() const noexcept { return total_logged_; }
    int sequence() const noexcept { return sequence_; }
    int capacity() const noexcept { return max_events_; }
    bool is_ring_mode() const noexcept { return ring_mode_; }
    bool buffer_full() const noexcept { return event_count_ >= max_events_; }

    // unique_orders: count distinct order IDs (excluding 0)
    // Like 'sort -u' on the order_id column
    // Jak 'sort -u' na kolumnie order_id
    int unique_orders() const noexcept {
        int ids[MAX_TRACKED_IDS];
        int n = 0;
        for (int i = 0; i < event_count_; ++i) {
            int32_t oid = events_[i].order_id;
            if (oid <= 0) continue;
            bool found = false;
            for (int j = 0; j < n; ++j) {
                if (ids[j] == oid) { found = true; break; }
            }
            if (!found && n < MAX_TRACKED_IDS) ids[n++] = oid;
        }
        return n;
    }

    // unique_symbols: count distinct stock symbols (excluding empty)
    // Jak 'sort -u' na kolumnie symbol
    int unique_symbols() const noexcept {
        char syms[MAX_TRACKED_IDS][9];
        int n = 0;
        for (int i = 0; i < event_count_; ++i) {
            if (events_[i].symbol[0] == '\0') continue;
            bool found = false;
            for (int j = 0; j < n; ++j) {
                if (std::strcmp(syms[j], events_[i].symbol) == 0) { found = true; break; }
            }
            if (!found && n < MAX_TRACKED_IDS) {
                std::strncpy(syms[n], events_[i].symbol, 8);
                syms[n][8] = '\0';
                n++;
            }
        }
        return n;
    }

    // time_span_ms: milliseconds from first to last event
    // Milisekundy od pierwszego do ostatniego zdarzenia
    double time_span_ms() const noexcept {
        if (event_count_ < 2) return 0.0;
        int64_t span = events_[event_count_ - 1].timestamp_ns - events_[0].timestamp_ns;
        return static_cast<double>(span) / 1'000'000.0;
    }

    // print_summary: display session statistics
    // Wyświetl statystyki sesji
    void print_summary() const {
        printf("\n=== Logger Summary / Podsumowanie ===\n");
        printf("  Total events:   %d\n", event_count_);
        printf("  Unique orders:  %d\n", unique_orders());
        printf("  Unique symbols: %d\n", unique_symbols());
        printf("  Time span:      %.3f ms\n", time_span_ms());
        printf("  Event counts:\n");
        for (int i = 0; i < static_cast<int>(EventType::EVENT_COUNT); ++i) {
            if (counters_[i] > 0) {
                printf("    %-20s %d\n", event_type_str(static_cast<EventType>(i)), counters_[i]);
            }
        }
    }

    // print_order_trail: display full lifecycle of one order
    // Wyświetl pełny cykl życia jednego zlecenia
    void print_order_trail(int32_t order_id) const {
        printf("--- Order #%d Audit Trail ---\n", order_id);
        for (int i = 0; i < event_count_; ++i) {
            if (events_[i].order_id != order_id) continue;
            const TradeEvent& e = events_[i];
            printf("  %-15s  %s  %s  qty=%d  px=%.2f  %s\n",
                   event_type_str(e.event_type), e.symbol, e.side,
                   e.quantity, e.price, e.details);
        }
    }

    // --- Time-Range Queries ---
    // get_events_in_range: copy events within a timestamp window to output buffer
    // Returns how many events matched
    // Kopiuj zdarzenia w oknie czasowym do bufora wyjściowego
    int get_events_in_range(TradeEvent* out, int max_out,
                            int64_t start_ns, int64_t end_ns) const noexcept {
        int count = 0;
        for (int i = 0; i < event_count_ && count < max_out; ++i) {
            const TradeEvent& e = events_[i];
            if (e.timestamp_ns >= start_ns && e.timestamp_ns <= end_ns) {
                out[count++] = e;
            }
        }
        return count;
    }

    // --- Latency Statistics ---
    // Struct to hold percentile latency results
    struct LatencyStats {
        int     count;
        int64_t min_ns;
        int64_t max_ns;
        int64_t mean_ns;
        int64_t p50_ns;
        int64_t p90_ns;
        int64_t p99_ns;
    };

    // get_latency_stats: compute inter-event latency percentiles
    // Oblicz percentyle opóźnień między zdarzeniami
    LatencyStats get_latency_stats() const noexcept {
        LatencyStats stats = {0, 0, 0, 0, 0, 0, 0};
        if (event_count_ < 2) return stats;

        // Allocate latency array (up to 1M entries — stack is too small, use heap)
        int n = event_count_ - 1;
        int64_t* latencies = new int64_t[n];

        int64_t sum = 0;
        for (int i = 0; i < n; ++i) {
            latencies[i] = events_[i + 1].timestamp_ns - events_[i].timestamp_ns;
            sum += latencies[i];
        }

        std::sort(latencies, latencies + n);

        stats.count = n;
        stats.min_ns = latencies[0];
        stats.max_ns = latencies[n - 1];
        stats.mean_ns = sum / n;
        stats.p50_ns = latencies[n / 2];
        stats.p90_ns = latencies[(int)(n * 0.90)];
        stats.p99_ns = latencies[std::min((int)(n * 0.99), n - 1)];

        delete[] latencies;
        return stats;
    }

    // --- JSON Export ---
    // export_json: write all events to stdout in JSON format
    // Zapisz wszystkie zdarzenia na stdout w formacie JSON
    void export_json() const {
        printf("{\n  \"total_logged\": %d,\n  \"events_in_buffer\": %d,\n  \"events\": [\n",
               total_logged_, event_count_);
        for (int i = 0; i < event_count_; ++i) {
            const TradeEvent& e = events_[i];
            printf("    {\"timestamp_ns\": %ld, \"event_type\": \"%s\", "
                   "\"order_id\": %d, \"symbol\": \"%s\", \"side\": \"%s\", "
                   "\"quantity\": %d, \"price\": %.2f, \"details\": \"%s\"}%s\n",
                   e.timestamp_ns, event_type_str(e.event_type),
                   e.order_id, e.symbol, e.side,
                   e.quantity, e.price, e.details,
                   (i < event_count_ - 1) ? "," : "");
        }
        printf("  ]\n}\n");
    }

    // export_json_to_file: write JSON to a file path
    // Zapisz JSON do pliku
    bool export_json_to_file(const char* filepath) const {
        FILE* f = std::fopen(filepath, "w");
        if (!f) return false;
        fprintf(f, "{\n  \"total_logged\": %d,\n  \"events_in_buffer\": %d,\n  \"events\": [\n",
                total_logged_, event_count_);
        for (int i = 0; i < event_count_; ++i) {
            const TradeEvent& e = events_[i];
            fprintf(f, "    {\"timestamp_ns\": %ld, \"event_type\": \"%s\", "
                    "\"order_id\": %d, \"symbol\": \"%s\", \"side\": \"%s\", "
                    "\"quantity\": %d, \"price\": %.2f, \"details\": \"%s\"}%s\n",
                    e.timestamp_ns, event_type_str(e.event_type),
                    e.order_id, e.symbol, e.side,
                    e.quantity, e.price, e.details,
                    (i < event_count_ - 1) ? "," : "");
        }
        fprintf(f, "  ]\n}\n");
        std::fclose(f);
        return true;
    }

    // export_csv: write all events to stdout in CSV format
    // Zapisz wszystkie zdarzenia na stdout w formacie CSV
    void export_csv() const {
        printf("sequence,timestamp_ns,event_type,order_id,symbol,side,quantity,price,details\n");
        for (int i = 0; i < event_count_; ++i) {
            const TradeEvent& e = events_[i];
            printf("%d,%ld,%s,%d,%s,%s,%d,%.2f,%s\n",
                   i + 1, e.timestamp_ns, event_type_str(e.event_type),
                   e.order_id, e.symbol, e.side, e.quantity, e.price, e.details);
        }
    }
};
