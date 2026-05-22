/*
 * TradeLogger — ścieżka audytu zleceń (SPSC ring + asynchroniczny flush).
 *
 * Każde zdarzenie (submit, fill, cancel, risk decision, kill switch,
 * system event) jest zapisywane jako stały rekord TradeEvent (128 bajtów,
 * 2 linie cache'a) z dwoma znacznikami czasu — CLOCK_MONOTONIC do
 * pomiaru latency i CLOCK_REALTIME dla regulatorów (MiFID II RTS 25
 * Article 2 wymaga UTC epoch).
 *
 * Architektura:
 *   - Wątek handlu pisze przez log()           — hot path, O(1), bez alokacji
 *   - Wątek flush_thread_ drenuje ring do plik  — off-CPU, batched fwrite
 *   - Trzeci wątek może czytać snapshot         — tylko między sesjami,
 *                                                  nie współbieżnie z log()
 *
 * Wydajność: ~15-25 M zdarzeń/sek log(), ~5-8 M zapisów/sek do pliku.
 *
 * Format pliku: FlushFileHeader (64 B) + N × TradeEvent (128 B). Plik
 * jest dopisywalny (open "ab") — każda sesja dorzuca własny header
 * + swoje rekordy, parser offline rozpoznaje po magic "HFTLOG\0\0".
 *
 * Trzy warianty loggera w repo (różne kompromisy):
 *   - TradeLogger (ten plik)    — ręcznie zwijany SPSC ring + flush thread
 *                                  + tryb ring_mode (nadpisuje najstarsze)
 *   - LockfreeTradeLogger       — używa lockfree::SPSCQueue jako backing
 *                                  store; bez ring_mode (fail-on-full)
 *   - MmapTradeLogger           — file mmap'd do RAMu, log() to memcpy,
 *                                  jądro flushuje asynchronicznie
 *
 * Pipeline: Strategy → Router → Risk → OMS → Logger → audit file.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <unordered_set>
#include <string>

#include "../common/time_utils.hpp"


// Maks. zdarzeń w buforze pierścieniowym. 1M × 128 B = 128 MB —
// alokowane raz w konstruktorze.
static constexpr int MAX_EVENTS = 1'000'000;

// Limit dla reserve() w unique_orders/symbols — żeby nie alokować
// dużej hash mapy gdy bufor jest częściowo zapełniony.
static constexpr int MAX_TRACKED_IDS = 4096;


// Co stało się ze zleceniem. uint8_t — 1 bajt vs 4, w TradeEvent
// (128 B exactly) liczy się każdy bajt.
enum class EventType : uint8_t {
    ORDER_SUBMIT  = 0,   // strategia wysłała zlecenie do OMS
    RISK_ACCEPT   = 1,   // RiskManager przepuścił
    RISK_REJECT   = 2,   // RiskManager odrzucił (kod powodu w `details`)
    ORDER_FILL    = 3,   // giełda zrealizowała w pełni
    ORDER_PARTIAL = 4,   // częściowa realizacja
    ORDER_CANCEL  = 5,   // anulowane
    KILL_SWITCH   = 6,   // tryb awaryjny — handel zatrzymany
    SYSTEM_START  = 7,   // start sesji
    SYSTEM_STOP   = 8,   // koniec sesji
    EVENT_COUNT   = 9    // sentinela do iteracji po liczniku
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


// Pojedynczy nieushuwalny rekord ścieżki audytu. Dokładnie 128 bajtów
// (2 linie cache'a) — pola ułożone tak żeby uniknąć paddingu.
// alignas(64): każdy rekord zaczyna się na granicy linii cache'a, więc
// wątek flush czytający slot K nie unieważnia linii w której wątek
// handlu pisze slot K+1.
struct alignas(64) TradeEvent {
    // 8-bajtowe pola najpierw — kompilator nie wstawia paddingu
    int64_t   mono_ns;       // CLOCK_MONOTONIC, ns — dla latency
    int64_t   wall_ns;       // CLOCK_REALTIME, ns  — UTC epoch dla regulatora
    uint64_t  sequence_no;   // monotonic counter — regulator wykrywa luki
    uint64_t  order_id;      // pasuje do OMS::Order::order_id
    double    price;         // float dla wyświetlania (OMS trzyma fixed-point)
    // 4-bajtowe
    int32_t   quantity;
    // 1-bajt + char[] — bez paddingu między nimi
    EventType event_type;
    char      symbol[9];     // "AAPL" + null
    char      side[5];       // "BUY"/"SELL" + null
    char      details[69];   // wolne pole na kontekst (venue, reject reason, ...)
    // Layout: 8+8+8+8+8 + 4 + 1+9+5+69 = 128 bajtów
};
static_assert(sizeof(TradeEvent) == 128, "TradeEvent must be exactly 128 bytes");


// Header pliku audytu. 64 bajty, raz na sesję, przed rekordami.
struct FlushFileHeader {
    char     magic[8];          // "HFTLOG\0\0" — identyfikacja formatu
    uint32_t version;           // = 1
    uint32_t record_size;       // sizeof(TradeEvent) = 128
    int64_t  created_wall_ns;   // UTC wall clock przy starcie sesji
    char     padding[40];       // rezerwa na przyszłość (8+4+4+8+40 = 64)
};
static_assert(sizeof(FlushFileHeader) == 64, "FlushFileHeader must be 64 bytes");


class TradeLogger {
    // Ring buffer — alokowany raz, nigdy nie realokowany
    std::unique_ptr<TradeEvent[]> events_;
    int max_events_;

    // SPSC atomics — wątek handlu pisze head_, wątek flush czyta head_
    // (acquire) i pisze flush_tail_ (relaxed — sam jest jedynym writerem).
    // Para release/acquire gwarantuje że flush_thread zobaczy kompletne
    // dane rekordu dopiero gdy zobaczy head_+1.
    alignas(64) std::atomic<int> head_{0};
    alignas(64) std::atomic<int> flush_tail_{0};

    // Counters — tylko wątek handlu (lub query offline). 64-bit, bo
    // przy 25 M evt/s int32 wraps w ~85 s — długa sesja zniszczyłaby
    // sequence numbers których regulator używa do detekcji luk.
    uint64_t sequence_{0};
    uint64_t total_logged_{0};
    bool     ring_mode_;        // true = nadpisuj najstarsze, false = stop gdy pełny

    // Per-type counters — szybki "ile było submitów / fillów / rejectów"
    int counters_[static_cast<int>(EventType::EVENT_COUNT)]{};

    // Stan flush thread
    std::thread       flush_thread_;
    std::atomic<bool> flush_running_{false};
    FILE*             flush_file_{nullptr};

    // drain_to_file: zapisuje ciągłe spany [t..h) do flush_file_.
    // Batched fwrite — jeden syscall na ciągły zakres zamiast N. Dla
    // ring_mode_ rozdziela na maks. 2 spany (przed wrap, po wrap).
    int drain_to_file(int t, int h) noexcept {
        while (t != h) {
            int slot   = ring_mode_ ? (t % max_events_) : t;
            int contig = ring_mode_ ? std::min(max_events_ - slot, h - t) : (h - t);
            std::fwrite(&events_[slot], sizeof(TradeEvent), contig, flush_file_);
            t += contig;
        }
        return t;
    }

    // flush_loop: pętla wątku flush. Co iterację: czyta head_ (acquire),
    // drainuje [flush_tail_..head_), potem 50 µs sleep żeby nie kręcić
    // CPU. Po flush_running_ = false jeszcze jeden drain (producent
    // mógł zdążyć dopisać) i fflush.
    //
    // W produkcji 50 µs sleep zastępuje się futexem / condvarem żeby
    // skrócić latency wakup'u. Tutaj prostsza wersja dla czytelności.
    void flush_loop() noexcept {
        while (true) {
            int h = head_.load(std::memory_order_acquire);
            int t = flush_tail_.load(std::memory_order_relaxed);
            if (t != h) {
                t = drain_to_file(t, h);
                flush_tail_.store(t, std::memory_order_relaxed);
            }
            if (!flush_running_.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        // Finalny drain po sygnale stop — producent mógł jeszcze coś
        // wepchnąć między test flush_running_ a break.
        int h = head_.load(std::memory_order_acquire);
        int t = drain_to_file(flush_tail_.load(std::memory_order_relaxed), h);
        flush_tail_.store(t, std::memory_order_relaxed);
        std::fflush(flush_file_);
    }

public:
    // ring_mode=false (default): bufor liniowy, log() zwraca 0 gdy pełny.
    // ring_mode=true:             bufor pierścieniowy, log() nadpisuje
    //                              najstarsze zdarzenia po przepełnieniu.
    explicit TradeLogger(bool ring_mode = false, int capacity = MAX_EVENTS)
        : events_(std::make_unique<TradeEvent[]>(capacity)),
          max_events_(capacity), ring_mode_(ring_mode) {
        std::memset(counters_, 0, sizeof(counters_));
    }

    ~TradeLogger() { stop_async_flush(); }

    TradeLogger(const TradeLogger&)            = delete;
    TradeLogger& operator=(const TradeLogger&) = delete;
    // Move byłby skomplikowany — flush_thread_/flush_file_/flush_running_
    // wymagałby transferu + atomic resync head_/flush_tail_. Logger jest
    // pinowany do wątku handlu, nie warto.
    TradeLogger(TradeLogger&&)                 = delete;
    TradeLogger& operator=(TradeLogger&&)      = delete;

    // start_async_flush: otwórz plik (append) i odpal flush thread.
    // Zwraca false jeśli fopen padł. Plik dopisuje header — można wielokrotnie
    // otwierać ten sam plik, każdy run zaczyna od własnego HFTLOG headera.
    bool start_async_flush(const char* filepath) noexcept {
        flush_file_ = std::fopen(filepath, "ab");
        if (!flush_file_) return false;

        FlushFileHeader hdr{};
        std::memcpy(hdr.magic, "HFTLOG\0\0", 8);
        hdr.version         = 1;
        hdr.record_size     = sizeof(TradeEvent);
        hdr.created_wall_ns = wall_ns();
        std::fwrite(&hdr, sizeof(hdr), 1, flush_file_);
        std::fflush(flush_file_);

        flush_running_.store(true, std::memory_order_relaxed);
        flush_thread_ = std::thread([this]() { flush_loop(); });
        return true;
    }

    // stop_async_flush: sygnalizuj flush thread'owi zakończenie, poczekaj
    // na join, zamknij plik. Blokuje dopóki wszystkie pending eventy
    // nie zostaną zapisane. Wywoływane przez destructor.
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

    // log: hot path. Buduje TradeEvent w wybranym slocie, inkrementuje
    // sequence_/total_logged_/counters_, release-store na head_ (czytany
    // przez flush thread acquire).
    //
    // Zwraca sequence_no (>0) przy sukcesie albo 0 jeśli linear bufor
    // jest pełny. Nie zwracamy pointera do slotu — w ring_mode slot
    // byłby nadpisany przez następne log()'i, caller mógłby czytać
    // stale data.
    uint64_t log(EventType type, uint64_t order_id = 0,
                 const char* symbol = "", const char* side = "",
                 int32_t quantity = 0, double price = 0.0,
                 const char* details = "") noexcept {
        int head = head_.load(std::memory_order_relaxed);
        if (!ring_mode_ && head >= max_events_) return 0;

        int slot = ring_mode_ ? (head % max_events_) : head;
        TradeEvent& e = events_[slot];
        e.mono_ns     = mono_ns();
        e.wall_ns     = wall_ns();
        e.sequence_no = ++sequence_;
        e.order_id    = order_id;
        e.price       = price;
        e.quantity    = quantity;
        e.event_type  = type;
        std::strncpy(e.symbol,  symbol,  8);  e.symbol[8]   = '\0';
        std::strncpy(e.side,    side,    4);  e.side[4]     = '\0';
        std::strncpy(e.details, details, 68); e.details[68] = '\0';

        const int idx = static_cast<int>(type);
        if (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
            counters_[idx]++;
        total_logged_++;

        // Release store: flush thread odczyta ten slot dopiero gdy zobaczy
        // head+1. Wszystkie zapisy do TradeEvent są przed nim w program
        // order, więc po release są widoczne dla acquire-readera.
        head_.store(head + 1, std::memory_order_release);
        return e.sequence_no;
    }

    // Akcessory (proste read-only).
    int      total_events()  const noexcept {
        int h = head_.load(std::memory_order_relaxed);
        return ring_mode_ ? std::min(h, max_events_) : h;
    }
    uint64_t total_logged()  const noexcept { return total_logged_; }
    uint64_t sequence()      const noexcept { return sequence_; }
    int      capacity()      const noexcept { return max_events_; }
    bool     is_ring_mode()  const noexcept { return ring_mode_; }
    bool     buffer_full()   const noexcept {
        return head_.load(std::memory_order_relaxed) >= max_events_;
    }
    int get_counter(EventType type) const noexcept {
        const int idx = static_cast<int>(type);
        return (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
               ? counters_[idx] : 0;
    }

    // Query — wszystkie bezpieczne tylko między sesjami (nie współbieżnie
    // z log()). Iterują po pełnym buforze, O(N).
    int get_events(const TradeEvent** out) const noexcept {
        *out = events_.get();
        return total_events();
    }

    // get_events_filtered: kopiuje do `out` rekordy spełniające filtry.
    // Filtry są opcjonalne (przekaż 0 / -1 / nullptr żeby pominąć).
    int get_events_filtered(TradeEvent* out, int max_out,
                            uint64_t filter_order_id = 0,
                            int filter_type = -1,
                            const char* filter_symbol = nullptr) const noexcept {
        int count = 0;
        const int n = total_events();
        for (int i = 0; i < n && count < max_out; ++i) {
            const TradeEvent& e = events_[i];
            if (filter_order_id > 0 && e.order_id != filter_order_id) continue;
            if (filter_type    >= 0 && static_cast<int>(e.event_type) != filter_type) continue;
            if (filter_symbol  && std::strcmp(e.symbol, filter_symbol) != 0) continue;
            out[count++] = e;
        }
        return count;
    }

    int get_order_trail(uint64_t order_id, TradeEvent* out, int max_out) const noexcept {
        return get_events_filtered(out, max_out, order_id);
    }

    // unique_orders/symbols: liczy ile *różnych* order_id / symboli pojawiło
    // się w buforze. Alokuje hash set — wywołuj offline.
    int unique_orders() const {
        std::unordered_set<uint64_t> ids;
        ids.reserve(std::min(total_events(), MAX_TRACKED_IDS));
        const int n = total_events();
        for (int i = 0; i < n; ++i)
            if (events_[i].order_id > 0) ids.insert(events_[i].order_id);
        return static_cast<int>(ids.size());
    }

    int unique_symbols() const {
        std::unordered_set<std::string> syms;
        syms.reserve(std::min(total_events(), MAX_TRACKED_IDS));
        const int n = total_events();
        for (int i = 0; i < n; ++i)
            if (events_[i].symbol[0] != '\0') syms.emplace(events_[i].symbol);
        return static_cast<int>(syms.size());
    }

    // time_span_ms: rozpiętość czasu między pierwszym a ostatnim zdarzeniem
    // (z mono_ns — odporne na korekcje NTP).
    double time_span_ms() const noexcept {
        const int n = total_events();
        if (n < 2) return 0.0;
        return static_cast<double>(events_[n - 1].mono_ns - events_[0].mono_ns) / 1'000'000.0;
    }

    // get_events_in_range: zapytanie po wall_ns (UTC), np. "wszystkie eventy
    // między 09:30:00.000 a 09:30:00.100" — przydatne dla regulatorów
    // którzy żądają replay konkretnego okna czasowego.
    int get_events_in_range(TradeEvent* out, int max_out,
                            int64_t start_wall_ns, int64_t end_wall_ns) const noexcept {
        int count = 0;
        const int n = total_events();
        for (int i = 0; i < n && count < max_out; ++i) {
            const TradeEvent& e = events_[i];
            if (e.wall_ns >= start_wall_ns && e.wall_ns <= end_wall_ns)
                out[count++] = e;
        }
        return count;
    }

    // Statystyki inter-event latency (gap mono_ns między kolejnymi).
    struct LatencyStats {
        int     count;
        int64_t min_ns, max_ns, mean_ns, p50_ns, p90_ns, p99_ns;
    };

    LatencyStats get_latency_stats() const noexcept {
        LatencyStats stats{};
        const int n = total_events();
        if (n < 2) return stats;

        const int cnt = n - 1;
        std::vector<int64_t> latencies(cnt);
        int64_t sum = 0;
        for (int i = 0; i < cnt; ++i) {
            latencies[i] = events_[i + 1].mono_ns - events_[i].mono_ns;
            sum += latencies[i];
        }
        std::sort(latencies.begin(), latencies.end());

        stats.count   = cnt;
        stats.min_ns  = latencies[0];
        stats.max_ns  = latencies[cnt - 1];
        stats.mean_ns = sum / cnt;
        stats.p50_ns  = latencies[cnt / 2];
        stats.p90_ns  = latencies[static_cast<int>(cnt * 0.90)];
        stats.p99_ns  = latencies[std::min(static_cast<int>(cnt * 0.99), cnt - 1)];
        return stats;
    }

    // Raporty — offline only (alokują, robią printf'y).
    void print_summary() const {
        printf("\n=== Logger Summary ===\n");
        printf("  Total events:   %d\n",          total_events());
        printf("  Total logged:   %lu\n",         (unsigned long)total_logged_);
        printf("  Unique orders:  %d\n",          unique_orders());
        printf("  Unique symbols: %d\n",          unique_symbols());
        printf("  Time span:      %.3f ms\n",     time_span_ms());
        printf("  Event counts:\n");
        for (int i = 0; i < static_cast<int>(EventType::EVENT_COUNT); ++i)
            if (counters_[i] > 0)
                printf("    %-20s %d\n",
                       event_type_str(static_cast<EventType>(i)), counters_[i]);
    }

    void print_order_trail(uint64_t order_id) const {
        printf("--- Order #%lu Audit Trail ---\n", (unsigned long)order_id);
        const int n = total_events();
        for (int i = 0; i < n; ++i) {
            if (events_[i].order_id != order_id) continue;
            const TradeEvent& e = events_[i];
            printf("  seq=%-8lu  %-15s  %s  %s  qty=%d  px=%.4f  %s\n",
                   (unsigned long)e.sequence_no,
                   event_type_str(e.event_type),
                   e.symbol, e.side, e.quantity, e.price, e.details);
        }
    }

    // export_binary: synchroniczny zrzut całego bufora do pliku.
    // Używaj gdy chcesz zarchiwizować bez wcześniejszego start_async_flush.
    bool export_binary(const char* filepath) const {
        FILE* f = std::fopen(filepath, "ab");
        if (!f) return false;
        FlushFileHeader hdr{};
        std::memcpy(hdr.magic, "HFTLOG\0\0", 8);
        hdr.version         = 1;
        hdr.record_size     = sizeof(TradeEvent);
        hdr.created_wall_ns = wall_ns();
        std::fwrite(&hdr, sizeof(hdr), 1, f);
        const int n = total_events();
        std::fwrite(events_.get(), sizeof(TradeEvent), n, f);
        std::fclose(f);
        return true;
    }

    // CSV / JSON do stdout lub pliku. Wszystkie 4 trasy używają
    // write_csv_row / write_json_obj jako wspólnych formatters.
    void export_csv()                              const { export_csv_to(stdout); }
    void export_json()                             const { export_json_to(stdout); }
    bool export_csv_to_file(const char* filepath)  const { return write_to_file(filepath, &TradeLogger::export_csv_to); }
    bool export_json_to_file(const char* filepath) const { return write_to_file(filepath, &TradeLogger::export_json_to); }

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
        const int n = total_events();
        for (int i = 0; i < n; ++i) write_csv_row(f, events_[i]);
    }
    void export_json_to(FILE* f) const {
        const int n = total_events();
        fprintf(f, "{\n  \"total_logged\": %lu,\n  \"events_in_buffer\": %d,\n  \"events\": [\n",
                (unsigned long)total_logged_, n);
        for (int i = 0; i < n; ++i) write_json_obj(f, events_[i], i == n - 1);
        fprintf(f, "  ]\n}\n");
    }

    // write_to_file: otwórz w "w", wywołaj member writer, zamknij.
    // Wspólny szkielet dla export_csv_to_file / export_json_to_file.
    bool write_to_file(const char* filepath,
                       void (TradeLogger::*writer)(FILE*) const) const {
        FILE* f = std::fopen(filepath, "w");
        if (!f) return false;
        (this->*writer)(f);
        std::fclose(f);
        return true;
    }
};
