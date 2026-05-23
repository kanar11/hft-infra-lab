/*
 * MeanReversionStrategy — strategia powrotu do średniej (mean reversion).
 *
 * Logika: gdy cena odbiega od krótkookresowej średniej ruchomej (SMA) o więcej
 * niż próg, stawiamy na powrót do średniej. To klasyczna strategia "reactive"
 * — czekamy na rynek, reagujemy na odchylenie. Market_maker.hpp pokazuje
 * wariant proactive (kwotujemy zawsze, zbieramy spread).
 *
 *   cena > SMA + threshold → SELL (przewartościowane, spodziewamy się spadku)
 *   cena < SMA - threshold → BUY  (niedowartościowane, spodziewamy się wzrostu)
 *   inaczej               → HOLD (brak akcji)
 *
 * Pipeline: ITCH feed → parser → STRATEGY (sygnały) → router → risk → OMS.
 *
 * Wydajność (lab): ~8M decyzji/sec, p50=100ns, p99=121ns na ticku.
 *
 * Decyzje projektowe:
 *   - PriceWindow trzyma running_sum → sma() jest O(1) zamiast O(window).
 *   - Stała tablica MAX_STOCKS×PriceWindow, zero heap na hot path.
 *   - find_or_create() to liniowy skan, ale N≤64 mieści się w 1 linii cache.
 *   - Symbol jako char[9] + std::strcmp — szybsze niż std::string dla N≤8 chr.
 *   - NaN/Inf guard żeby zepsuty feed nie propagował się w deviation_pct.
 */
#pragma once

#include "../common/time_utils.hpp"
#include "../common/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>


// Maks. liczba symboli śledzonych jednocześnie (stała tablica, no heap).
static constexpr int MAX_STOCKS = 64;

// Maks. rozmiar okna SMA (ile ostatnich cen trzymamy per symbol).
static constexpr int MAX_WINDOW = 128;


// Sygnał handlowy — generowany gdy cena wystarczająco odbiega od średniej.
struct Signal {
    int64_t timestamp_ns;      // czas wygenerowania sygnału
    char    stock[9];          // ticker
    Side    side;              // BUY/SELL (znaczące tylko gdy valid=true)
    double  price;             // bieżąca cena
    int32_t quantity;          // rozmiar zlecenia
    double  sma;               // SMA w momencie sygnału
    double  deviation_pct;     // jak daleko od średniej (%)
    bool    valid;             // false = HOLD (brak sygnału)

    Signal() noexcept
        : timestamp_ns(0), side(Side::BUY), price(0), quantity(0),
          sma(0), deviation_pct(0), valid(false) {
        stock[0] = '\0';
    }
};


// Liczniki strategii (do print_stats / benchmarków).
struct StrategyStats {
    uint64_t signals_generated;
    uint64_t buys;
    uint64_t sells;
    uint64_t holds;
    uint64_t total_latency_ns;

    StrategyStats() noexcept
        : signals_generated(0), buys(0), sells(0), holds(0), total_latency_ns(0) {}

    double avg_latency_ns() const noexcept {
        const uint64_t total = signals_generated + holds;
        return total == 0 ? 0.0 : static_cast<double>(total_latency_ns) / total;
    }
};


// PriceWindow — circular buffer cen jednego symbolu z incremental running_sum
// żeby sma() pozostała O(1) (zamiast O(window) sumowania per tick).
struct PriceWindow {
    char   symbol[9];
    double prices[MAX_WINDOW];
    double running_sum;        // suma cen w oknie — utrzymywana incrementally
    int    count;              // ile cen zapisanych (do window_size)
    int    head;               // pozycja zapisu w circular buf
    int    window_size;        // docelowy rozmiar okna (np. 20)
    bool   active;             // czy slot jest używany?

    PriceWindow() noexcept
        : prices{}, running_sum(0.0), count(0), head(0), window_size(20), active(false) {
        symbol[0] = '\0';
    }

    // add: dopisz cenę, utrzymuj running_sum (eviction najstarszej gdy okno pełne).
    void add(double price) noexcept {
        if (count == window_size) running_sum -= prices[head];  // eviction
        prices[head] = price;
        running_sum += price;
        head = (head + 1) % window_size;
        if (count < window_size) ++count;
    }

    double sma() const noexcept { return count == 0 ? 0.0 : running_sum / count; }
    bool   full() const noexcept { return count >= window_size; }
};


class MeanReversionStrategy {
    PriceWindow   windows_[MAX_STOCKS];
    int           stock_count_;
    int           window_size_;
    double        threshold_;        // próg odchylenia (np. 0.001 = 0.1%)
    int32_t       order_size_;
    StrategyStats stats_;
    bool          overflow_warned_ = false;  // ostrzeż raz na run o pełnej tablicy

    // find_or_create: znajdź lub utwórz okno cen dla symbolu.
    // Linear scan, ale N≤64 mieści się w 1-2 liniach cache → szybkie.
    PriceWindow* find_or_create(const char* stock) noexcept {
        for (int i = 0; i < stock_count_; ++i) {
            if (std::strcmp(windows_[i].symbol, stock) == 0) return &windows_[i];
        }
        // Wyczerpana pula slotów — ostrzeż raz, inaczej cicho gubimy sygnały.
        if (stock_count_ >= MAX_STOCKS) {
            if (!overflow_warned_) {
                printf("[Strategy] WARNING: MAX_STOCKS=%d reached; '%.*s' ignored\n",
                       MAX_STOCKS, 8, stock);
                overflow_warned_ = true;
            }
            return nullptr;
        }
        PriceWindow& w = windows_[stock_count_++];
        w.active      = true;
        w.window_size = window_size_;
        w.count       = 0;
        w.head        = 0;
        std::strncpy(w.symbol, stock, 8);
        w.symbol[8] = '\0';
        return &w;
    }

    // emit_signal: wspólne wypełnienie pól dla BUY i SELL (DRY).
    void emit_signal(Signal& sig, const char* stock, Side side, double price,
                     double sma, double deviation, int64_t ts) const noexcept {
        sig.valid         = true;
        sig.timestamp_ns  = ts;
        sig.side          = side;
        sig.price         = price;
        sig.quantity      = order_size_;
        sig.sma           = sma;
        sig.deviation_pct = deviation * 100.0;
        std::strncpy(sig.stock, stock, 8);
        sig.stock[8] = '\0';
    }

public:
    // window: okno SMA (clamped do [1, MAX_WINDOW] — 0 / ujemne dawało div-by-zero w add()/sma()).
    // threshold_pct: próg odchylenia w procentach (0.1 = 0.1%).
    // order_size: domyślna ilość per sygnał.
    MeanReversionStrategy(int window = 20, double threshold_pct = 0.1,
                           int32_t order_size = 100) noexcept
        : stock_count_(0),
          window_size_(std::max(1, std::min(window, MAX_WINDOW))),
          threshold_(threshold_pct / 100.0),
          order_size_(order_size) {}

    // on_market_data — HOT PATH, wywoływane na każdy tick.
    // Zwraca Signal (valid=true gdy BUY/SELL, valid=false gdy HOLD).
    Signal on_market_data(const char* stock, double price,
                          int64_t timestamp_ns = 0) noexcept {
        const int64_t t0 = mono_ns();
        Signal sig;

        // NaN/Inf guard — zepsuty feed nie powinien generować bzdurnych sygnałów.
        PriceWindow* w = (std::isfinite(price) && price > 0.0) ? find_or_create(stock) : nullptr;
        if (!w) {
            ++stats_.holds;
            stats_.total_latency_ns += (mono_ns() - t0);
            return sig;
        }

        w->add(price);

        // Bez pełnego okna nie generujemy sygnałów (SMA byłaby unreliable).
        if (!w->full()) {
            ++stats_.holds;
            stats_.total_latency_ns += (mono_ns() - t0);
            return sig;
        }

        const double sma = w->sma();
        if (sma <= 0.0) {  // defensive — wszystkie ceny w oknie były ≤0
            ++stats_.holds;
            stats_.total_latency_ns += (mono_ns() - t0);
            return sig;
        }
        const double deviation = (price - sma) / sma;  // np. 0.01 = 1% powyżej SMA

        if (deviation > threshold_) {
            emit_signal(sig, stock, Side::SELL, price, sma, deviation, timestamp_ns);
            ++stats_.sells;
            ++stats_.signals_generated;
        } else if (deviation < -threshold_) {
            emit_signal(sig, stock, Side::BUY, price, sma, deviation, timestamp_ns);
            ++stats_.buys;
            ++stats_.signals_generated;
        } else {
            ++stats_.holds;
        }

        stats_.total_latency_ns += (mono_ns() - t0);
        return sig;
    }

    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }

    void print_stats() const {
        printf("\n=== Strategy Statistics ===\n");
        printf("  Signals: %lu (%lu buys, %lu sells)\n",
               (unsigned long)stats_.signals_generated,
               (unsigned long)stats_.buys, (unsigned long)stats_.sells);
        printf("  Holds:   %lu\n", (unsigned long)stats_.holds);
        const uint64_t total = stats_.signals_generated + stats_.holds;
        if (total > 0) {
            printf("  Signal rate: %.1f%%\n", stats_.signals_generated * 100.0 / total);
        }
        printf("  Avg decision latency: %.0f ns\n", stats_.avg_latency_ns());
    }
};
