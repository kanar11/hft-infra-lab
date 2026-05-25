/*
 * exec_algo.hpp — algorytmy execution: TWAP i VWAP.
 *
 * Czym to się różni od mean_reversion / market_maker?
 *
 *   - Mean reversion / market maker to **ALPHA strategies** — próbują
 *     przewidzieć dokąd idzie cena i zarobić na ruchu.
 *   - VWAP / TWAP to **EXECUTION algorithms** — *nie* przewidują ceny.
 *     Mają zlecenie "rodzic" (parent order: kup 100k AAPL do końca dnia)
 *     i muszą je tak rozkroić w czasie żeby:
 *       a) zrealizować całość przed deadline'em
 *       b) NIE ruszyć rynku własnym order flow'em (gdyby fundusz
 *          wrzucił 100k w jednym tickecie, cena by uciekła w górę i
 *          sami zapłaciliby drożej — slippage)
 *       c) osiągnąć cenę średnią blisko benchmark'u (VWAP rynku)
 *
 * Kto tego używa: każdy buy-side (fundusze emerytalne, ETF'y, asset
 * managerzy) który ma do egzekucji duże zlecenia. Sell-side bank
 * sprzedaje execution algos jako produkt. To codzienne pieczywo
 * institutional trading.
 *
 * TWAP (Time-Weighted Average Price):
 *   Najprostszy slicer. Dzieli parent order na N równych kawałków
 *   i wysyła co T sekund. Plus: zero overhead, deterministyczny.
 *   Minus: ignoruje rozkład wolumenu — wysyła tyle samo o 9:35 (mały
 *   wolumen, łatwo ruszyć rynek) co o 15:55 (dużo wolumenu, łatwo
 *   się ukryć).
 *
 * VWAP (Volume-Weighted Average Price):
 *   Slicer ze świadomością profilu wolumenu. Dostaje historyczny
 *   profil "ile % dziennego wolumenu zwykle handlowane jest w slocie X"
 *   (najczęściej U-shape: 30% rano, 40% środek, 30% pod koniec sesji)
 *   i wysyła proporcjonalnie do oczekiwanego wolumenu w tym slocie.
 *   Plus: cena średnia bliżej benchmark'u rynku. Minus: jeśli profil
 *   się myli (dziś inny dzień), płacisz za to slippage'em.
 *
 * Pomiar jakości: **slippage w basis points (bps)** vs benchmark VWAP rynku.
 *   1 bps = 0.01% = $0.01 na każde $100. Production HFT goni za <1 bps
 *   na dużych zleceniach; retail brokerzy bywają w okolicach 5-20 bps.
 */
#pragma once

#include "../common/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>


namespace exec {


// ParentOrder — zlecenie "rodzic" do egzekucji w określonym oknie czasowym.
//
//   total_qty       — łączna ilość shares do egzekucji
//   start_ts_sec    — kiedy zaczynamy (sekundy od jakiegoś punktu zerowego;
//                     typowo "sekundy od market open 09:30 ET")
//   duration_sec    — szerokość okna (np. 23400 = 6.5h, pełna sesja)
//   num_slices      — na ile child orders kroimy parent'a
//
// Przykład: BUY 10000 AAPL przez 60 sekund, 20 slotów po 3 sekundy:
//   {symbol="AAPL", side=BUY, total_qty=10000, start=0, duration=60, slices=20}
struct ParentOrder {
    char     symbol[9];
    Side     side;
    int32_t  total_qty;
    int32_t  start_ts_sec;
    int32_t  duration_sec;
    int      num_slices;

    ParentOrder() noexcept : side(Side::BUY), total_qty(0),
                              start_ts_sec(0), duration_sec(0), num_slices(0) {
        symbol[0] = '\0';
    }
};


// ChildOrder — pojedynczy "kawałek" do wysłania w tym tick'u.
//   valid=false → nic nie wysyłaj w tym tick'u (np. nie nadszedł jeszcze
//                  termin następnego slotu, albo parent już wykonany).
struct ChildOrder {
    int32_t qty;          // ile akcji w tym dziecku
    int32_t price_ticks;  // limit price w tickach (0 = market order)
    bool    valid;

    ChildOrder() noexcept : qty(0), price_ticks(0), valid(false) {}
};


// ExecStats — wynik egzekucji. Kumulowane przez apply_fill().
struct ExecStats {
    int64_t  filled_qty;       // ile shares zrealizowanych
    int64_t  cash_ticks;       // suma qty*price_ticks dla wszystkich fills (signed)
    int      num_fills;        // liczba osobnych fills
    int      slices_emitted;   // liczba child orders wystawionych
    int      slices_skipped;   // ile slotów minęło bez emisji (np. po hour'ach)

    ExecStats() noexcept : filled_qty(0), cash_ticks(0), num_fills(0),
                            slices_emitted(0), slices_skipped(0) {}

    // Realized VWAP = sum(qty*price) / sum(qty), w ticks.
    int32_t realized_vwap_ticks() const noexcept {
        return filled_qty > 0 ? static_cast<int32_t>(cash_ticks / filled_qty) : 0;
    }
};


// Slippage w basis points vs benchmark.
//   BUY:  jeśli realized > benchmark, płacisz drożej → dodatnie bps to ZŁE
//   SELL: jeśli realized < benchmark, dostałeś mniej → dodatnie bps to ZŁE
// Konwencja: dodatnie bps = gorsza egzekucja niż benchmark.
inline double slippage_bps(Side side, int32_t realized_ticks, int32_t benchmark_ticks) noexcept {
    if (benchmark_ticks <= 0) return 0.0;
    const double diff = static_cast<double>(realized_ticks - benchmark_ticks);
    const double bps  = (diff / benchmark_ticks) * 10000.0;
    return side == Side::BUY ? bps : -bps;
}


// TWAPExecutor — równe kawałki, równe odstępy czasowe.
//
// Slot N rozpoczyna się o czasie start_ts + N * seconds_per_slice.
// Każdy slot wystawia identyczny child order (total_qty / num_slices).
// Resztki z dzielenia idą do ostatniego slotu (gwarancja że suma = total).
class TWAPExecutor {
    ParentOrder parent_;
    int         seconds_per_slice_;
    int32_t     slice_size_;     // baza (może być +reszta w ostatnim slocie)
    ExecStats   stats_;

public:
    explicit TWAPExecutor(const ParentOrder& parent) noexcept
        : parent_(parent),
          seconds_per_slice_(parent.num_slices > 0
              ? std::max(1, parent.duration_sec / parent.num_slices) : 1),
          slice_size_(parent.num_slices > 0
              ? parent.total_qty / parent.num_slices : 0) {}

    // on_tick: wywołane co tick przez market data feed (zwykle co sekundę).
    // Zwraca ChildOrder do wysłania (valid=true) lub valid=false jeśli nic
    // nie ma do zrobienia w tym tick'u. price_ticks=0 → market order
    // (wywołujący sam decyduje czy chce limit przy mid'cie etc.).
    ChildOrder on_tick(int32_t now_sec) noexcept {
        ChildOrder o;
        if (parent_.num_slices <= 0 || stats_.filled_qty >= parent_.total_qty) return o;
        if (now_sec < parent_.start_ts_sec)                                     return o;

        // Który slot powinien być teraz wystawiony?
        const int32_t elapsed = now_sec - parent_.start_ts_sec;
        const int     slot    = elapsed / seconds_per_slice_;
        if (slot < stats_.slices_emitted) return o;  // jeszcze nie czas
        if (slot >= parent_.num_slices)   return o;  // okno zamknięte

        // Rozmiar slotu: baza, ostatni slot dostaje resztę z dzielenia.
        int32_t qty = slice_size_;
        if (slot == parent_.num_slices - 1) {
            qty = parent_.total_qty - slice_size_ * (parent_.num_slices - 1);
        }
        // Nie wystawiaj więcej niż zostało do total.
        const int32_t remaining = parent_.total_qty - static_cast<int32_t>(stats_.filled_qty);
        if (qty > remaining) qty = remaining;
        if (qty <= 0) return o;

        o.qty         = qty;
        o.price_ticks = 0;   // market order — niech wywołujący zdecyduje
        o.valid       = true;
        ++stats_.slices_emitted;
        return o;
    }

    // apply_fill: wywołane przez handler fills'ów po execution na giełdzie.
    void apply_fill(int32_t qty, int32_t price_ticks) noexcept {
        if (qty <= 0 || price_ticks <= 0) return;
        stats_.filled_qty += qty;
        stats_.cash_ticks += static_cast<int64_t>(qty) * price_ticks;
        ++stats_.num_fills;
    }

    const ParentOrder& parent() const noexcept { return parent_; }
    const ExecStats&   stats()  const noexcept { return stats_; }
    int32_t realized_vwap_ticks() const noexcept { return stats_.realized_vwap_ticks(); }
    bool    done() const noexcept { return stats_.filled_qty >= parent_.total_qty; }
};


// VWAPExecutor — slicer ze świadomością profilu wolumenu.
//
// Bierze ParentOrder + vector<double> volume_profile (musi mieć
// parent.num_slices elementów, suma ~= 1.0). Dla każdego slotu N
// wyemituje child = total_qty * volume_profile[N] (zaokrąglony).
// Resztki kumulowane są w bieżącym slocie żeby ostatecznie zrealizować
// dokładnie total_qty.
//
// Jeśli profile_ jest pusty, fallback do równych kawałków = TWAP.
class VWAPExecutor {
    ParentOrder         parent_;
    std::vector<double> profile_;             // suma = 1.0, parent.num_slices elementów
    std::vector<int32_t> target_cumulative_;  // ile shares do końca slotu N
    int                 seconds_per_slice_;
    int                 next_slot_to_process_;  // następny slot do potraktowania (anti-double-process)
    int32_t             total_emitted_qty_;   // suma qty wszystkich wyemitowanych child'ów
    ExecStats           stats_;

public:
    VWAPExecutor(const ParentOrder& parent, std::vector<double> volume_profile)
        : parent_(parent),
          profile_(std::move(volume_profile)),
          seconds_per_slice_(parent.num_slices > 0
              ? std::max(1, parent.duration_sec / parent.num_slices) : 1),
          next_slot_to_process_(0),
          total_emitted_qty_(0) {

        // Normalizacja: jeśli suma != 1.0, przeskaluj.
        double sum = 0.0;
        for (const double v : profile_) sum += v;
        if (sum > 0.0) for (double& v : profile_) v /= sum;

        // Cumulative targets — zaokrąglone integery. Ostatni slot = total_qty
        // zawsze, żeby zniwelować błędy zaokrągleń.
        target_cumulative_.reserve(static_cast<std::size_t>(parent.num_slices));
        double cum = 0.0;
        for (int i = 0; i < parent.num_slices; ++i) {
            const double frac = (i < static_cast<int>(profile_.size())) ? profile_[i] : 0.0;
            cum += frac;
            int32_t target = static_cast<int32_t>(cum * parent.total_qty + 0.5);
            if (i == parent.num_slices - 1) target = parent.total_qty;  // exact
            target_cumulative_.push_back(target);
        }
    }

    ChildOrder on_tick(int32_t now_sec) noexcept {
        ChildOrder o;
        if (parent_.num_slices <= 0 || stats_.filled_qty >= parent_.total_qty) return o;
        if (now_sec < parent_.start_ts_sec)                                     return o;

        const int32_t elapsed = now_sec - parent_.start_ts_sec;
        const int     slot    = elapsed / seconds_per_slice_;
        if (slot < next_slot_to_process_) return o;  // ten slot już obsłużony
        if (slot >= parent_.num_slices)   return o;  // okno zamknięte

        // qty = cumulative target dla tego slotu MINUS co już wystawiliśmy.
        // Dzięki temu sloty z profile=0% automatycznie catch-up'ują wolumen
        // w następnym aktywnym slocie (a slices_skipped liczy się raz na slot).
        const int32_t target_to_emit = target_cumulative_[slot];
        int32_t qty = target_to_emit - total_emitted_qty_;
        next_slot_to_process_ = slot + 1;  // bez względu na qty — anti-double-process

        if (qty <= 0) { ++stats_.slices_skipped; return o; }

        o.qty         = qty;
        o.price_ticks = 0;
        o.valid       = true;
        total_emitted_qty_ += qty;
        ++stats_.slices_emitted;
        return o;
    }

    void apply_fill(int32_t qty, int32_t price_ticks) noexcept {
        if (qty <= 0 || price_ticks <= 0) return;
        stats_.filled_qty += qty;
        stats_.cash_ticks += static_cast<int64_t>(qty) * price_ticks;
        ++stats_.num_fills;
    }

    const ParentOrder& parent() const noexcept { return parent_; }
    const ExecStats&   stats()  const noexcept { return stats_; }
    int32_t realized_vwap_ticks() const noexcept { return stats_.realized_vwap_ticks(); }
    bool    done() const noexcept { return stats_.filled_qty >= parent_.total_qty; }
};


// MarketVWAPTracker — niezależny pomiar VWAP rynku z tego samego strumienia
// tradeów. Używany jako benchmark do obliczenia slippage'u execution algo.
//
// Wywoływany on_trade(price, qty) dla każdego prawdziwego trade'u (nie naszego!)
// na giełdzie. Realized VWAP rynku = sum(price*qty) / sum(qty).
class MarketVWAPTracker {
    int64_t cash_ticks_ = 0;
    int64_t volume_     = 0;

public:
    void on_trade(int32_t price_ticks, int32_t qty) noexcept {
        if (price_ticks <= 0 || qty <= 0) return;
        cash_ticks_ += static_cast<int64_t>(price_ticks) * qty;
        volume_     += qty;
    }

    int32_t vwap_ticks() const noexcept {
        return volume_ > 0 ? static_cast<int32_t>(cash_ticks_ / volume_) : 0;
    }

    int64_t volume() const noexcept { return volume_; }
};


// Standardowy U-shape profil wolumenu dla US equity. Najwięcej handlu rano
// (open auction + reakcje na overnight news) i pod koniec sesji (closing
// auction, rebalans funduszy). Środek dnia spokojny.
//
// Podział na n slotów: 30% w pierwszych 20%, 40% w środku 60%, 30% w
// ostatnich 20%. Proporcjonalne wewnątrz każdej z 3 stref.
inline std::vector<double> u_shape_profile(int num_slices) {
    std::vector<double> p;
    if (num_slices <= 0) return p;
    p.reserve(static_cast<std::size_t>(num_slices));

    const int open_slots   = std::max(1, num_slices / 5);  // 20%
    const int close_slots  = std::max(1, num_slices / 5);  // 20%
    const int middle_slots = std::max(1, num_slices - open_slots - close_slots);

    // 30% / 40% / 30% wolumenu rozłożone proporcjonalnie wewnątrz stref.
    const double open_weight   = 0.30 / open_slots;
    const double middle_weight = 0.40 / middle_slots;
    const double close_weight  = 0.30 / close_slots;

    for (int i = 0; i < open_slots; ++i)    p.push_back(open_weight);
    for (int i = 0; i < middle_slots; ++i)  p.push_back(middle_weight);
    for (int i = 0; i < close_slots; ++i)   p.push_back(close_weight);
    return p;
}


}  // namespace exec
