/*
 * Mean Reversion Strategy — C++ Implementation
 * Strategia Powrotu do Średniej — implementacja C++
 *
 * Logic: when price deviates from its short-term moving average (SMA) by more
 * than a threshold, bet on reversion to the mean.
 * Logika: gdy cena odbiega od krótkookresowej średniej ruchomej (SMA) o więcej
 * niż próg, zakładamy powrót do średniej.
 *
 *   Price > SMA + threshold → SELL (overpriced, expect drop)
 *   Price < SMA - threshold → BUY  (underpriced, expect rise)
 *   Otherwise               → HOLD (no action)
 *
 * Pipeline: ITCH Feed → Parser → **Strategy** (signals) → Router → Risk → OMS
 *
 * Performance / Wydajność:
 *   Python: ~200K decisions/sec (~2300ns per tick)
 *   C++:    ~20-40M decisions/sec
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <algorithm>

// Max stocks we track simultaneously (fixed array, no heap on hot path)
// Like /proc/sys/fs/file-max — a hard limit on open resources
// Maks. akcji śledzonych jednocześnie (stała tablica, bez sterty na gorącej ścieżce)
static constexpr int MAX_STOCKS = 64;

// Max price window size (how many recent prices we keep per stock)
// Like 'tail -n 20' keeping only the last 20 lines
// Maks. rozmiar okna cen (ile ostatnich cen trzymamy per akcja)
static constexpr int MAX_WINDOW = 128;


// === Signal — a trading decision ===
// Generated when price deviates enough from the mean
// Generowany gdy cena wystarczająco odbiega od średniej

struct Signal {
    int64_t timestamp_ns;      // when the signal was generated / kiedy sygnał został wygenerowany
    char    stock[9];          // stock ticker / symbol giełdowy
    char    side[5];           // "BUY" or "SELL" / "BUY" lub "SELL"
    double  price;             // current price / bieżąca cena
    int32_t quantity;          // order size / wielkość zlecenia
    double  sma;               // moving average at time of signal / średnia ruchoma w momencie sygnału
    double  deviation_pct;     // how far from mean (%) / jak daleko od średniej (%)
    bool    valid;             // false = HOLD (no signal) / false = HOLD (brak sygnału)

    Signal() noexcept
        : timestamp_ns(0), price(0), quantity(0), sma(0),
          deviation_pct(0), valid(false) {
        stock[0] = '\0';
        side[0] = '\0';
    }
};


// === StrategyStats — performance counters ===

struct StrategyStats {
    uint64_t signals_generated;
    uint64_t buys;
    uint64_t sells;
    uint64_t holds;
    uint64_t total_latency_ns;

    StrategyStats() noexcept
        : signals_generated(0), buys(0), sells(0), holds(0), total_latency_ns(0) {}

    double avg_latency_ns() const noexcept {
        if (signals_generated + holds == 0) return 0.0;
        return static_cast<double>(total_latency_ns) / (signals_generated + holds);
    }
};


// === PriceWindow — circular buffer of recent prices for one stock ===
// Like a ring buffer / bufor pierścieniowy — when full, overwrites oldest
// deque(maxlen=20) in Python, but without heap allocation

struct PriceWindow {
    char   symbol[9];
    double prices[MAX_WINDOW];
    int    count;              // how many prices stored (up to window size)
    int    head;               // write position in circular buffer
    int    window_size;        // target window (e.g., 20)
    bool   active;             // is this slot in use?

    PriceWindow() noexcept
        : prices{}, count(0), head(0), window_size(20), active(false) {
        symbol[0] = '\0';
    }

    // add: append price to circular buffer
    // Like appending to 'tail -n 20' — oldest price is automatically dropped
    // Jak dodawanie do 'tail -n 20' — najstarsza cena automatycznie znika
    void add(double price) noexcept {
        prices[head] = price;
        head = (head + 1) % window_size;
        if (count < window_size) count++;
    }

    // sma: calculate Simple Moving Average
    // Sum all prices and divide by count — O(window_size)
    // Zsumuj wszystkie ceny i podziel przez liczbę — O(window_size)
    double sma() const noexcept {
        if (count == 0) return 0.0;
        double sum = 0.0;
        // When buffer is full, we read from (head) to (head + count - 1) mod window_size
        // Gdy bufor jest pełny, czytamy od head w kółko
        for (int i = 0; i < count; ++i) {
            int idx = (head - count + i + window_size) % window_size;
            sum += prices[idx];
        }
        return sum / count;
    }

    bool full() const noexcept { return count >= window_size; }
};


// === MeanReversionStrategy ===

class MeanReversionStrategy {
    PriceWindow windows_[MAX_STOCKS];
    int         stock_count_;
    int         window_size_;
    double      threshold_;        // deviation threshold (e.g., 0.001 = 0.1%)
    int32_t     order_size_;
    StrategyStats stats_;

    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    // find_or_create: get price window for a stock symbol
    // Like hash table lookup — O(N) scan but N is small (≤64)
    // Jak wyszukiwanie w hash tablicy — O(N) ale N jest małe (≤64)
    PriceWindow* find_or_create(const char* stock) noexcept {
        // Look for existing
        for (int i = 0; i < stock_count_; ++i) {
            if (std::strcmp(windows_[i].symbol, stock) == 0)
                return &windows_[i];
        }
        // Create new
        if (stock_count_ >= MAX_STOCKS) return nullptr;
        PriceWindow& w = windows_[stock_count_++];
        w.active = true;
        w.window_size = window_size_;
        w.count = 0;
        w.head = 0;
        std::strncpy(w.symbol, stock, 8);
        w.symbol[8] = '\0';
        return &w;
    }

public:
    // window: number of prices for moving average (like 'tail -n window').
    //         Clamped to [1, MAX_WINDOW] — 0 or negative would divide-by-zero in add()/sma().
    // threshold_pct: deviation threshold in percent (0.1 = 0.1%)
    // order_size: default order quantity
    MeanReversionStrategy(int window = 20, double threshold_pct = 0.1,
                           int32_t order_size = 100) noexcept
        : stock_count_(0),
          window_size_(std::max(1, std::min(window, MAX_WINDOW))),
          threshold_(threshold_pct / 100.0),
          order_size_(order_size) {}

    // on_market_data: process a price tick, return signal (or invalid = HOLD)
    // This is the HOT PATH — called on every market data update
    // To jest GORĄCA ŚCIEŻKA — wywoływana przy każdej aktualizacji danych rynkowych
    Signal on_market_data(const char* stock, double price,
                          int64_t timestamp_ns = 0) noexcept {
        int64_t t0 = now_ns();
        Signal sig;

        // Reject NaN/Inf prices — they would propagate silently into deviation
        // Odrzuć NaN/Inf — propagowałyby się cicho do sygnału
        PriceWindow* w = (std::isfinite(price) && price > 0.0) ? find_or_create(stock) : nullptr;
        if (!w) {
            stats_.holds++;
            stats_.total_latency_ns += (now_ns() - t0);
            return sig;  // invalid = HOLD
        }

        w->add(price);

        // Need full window before generating signals
        // Potrzebujemy pełnego okna przed generowaniem sygnałów
        if (!w->full()) {
            stats_.holds++;
            stats_.total_latency_ns += (now_ns() - t0);
            return sig;  // invalid = HOLD
        }

        double sma = w->sma();
        // Guard divide-by-zero: SMA can't be ≤0 unless every price was ≤0
        // (filtered above), but keep the check defensive.
        if (sma <= 0.0) {
            stats_.holds++;
            stats_.total_latency_ns += (now_ns() - t0);
            return sig;
        }
        // deviation = (price - sma) / sma — fraction (0.01 = 1% above)
        double deviation = (price - sma) / sma;

        if (deviation > threshold_) {
            // Price above SMA + threshold → SELL (expect reversion down)
            // Cena powyżej SMA + próg → SPRZEDAJ
            sig.valid = true;
            sig.timestamp_ns = timestamp_ns;
            std::strncpy(sig.stock, stock, 8);
            sig.stock[8] = '\0';
            std::strncpy(sig.side, "SELL", 4);
            sig.side[4] = '\0';
            sig.price = price;
            sig.quantity = order_size_;
            sig.sma = sma;
            sig.deviation_pct = deviation * 100.0;

            stats_.sells++;
            stats_.signals_generated++;

        } else if (deviation < -threshold_) {
            // Price below SMA - threshold → BUY (expect reversion up)
            // Cena poniżej SMA - próg → KUP
            sig.valid = true;
            sig.timestamp_ns = timestamp_ns;
            std::strncpy(sig.stock, stock, 8);
            sig.stock[8] = '\0';
            std::strncpy(sig.side, "BUY", 4);
            sig.side[4] = '\0';
            sig.price = price;
            sig.quantity = order_size_;
            sig.sma = sma;
            sig.deviation_pct = deviation * 100.0;

            stats_.buys++;
            stats_.signals_generated++;

        } else {
            stats_.holds++;
        }

        stats_.total_latency_ns += (now_ns() - t0);
        return sig;
    }

    // Accessors
    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }

    void print_stats() const {
        printf("\n=== Strategy Statistics / Statystyki strategii ===\n");
        printf("  Signals: %lu (%lu buys, %lu sells)\n",
               (unsigned long)stats_.signals_generated,
               (unsigned long)stats_.buys, (unsigned long)stats_.sells);
        printf("  Holds: %lu\n", (unsigned long)stats_.holds);
        uint64_t total = stats_.signals_generated + stats_.holds;
        if (total > 0) {
            double signal_rate = stats_.signals_generated * 100.0 / total;
            printf("  Signal rate: %.1f%%\n", signal_rate);
        }
        printf("  Avg decision latency: %.0f ns\n", stats_.avg_latency_ns());
    }
};
