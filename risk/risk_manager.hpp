/*
 * ============================================================================
 *  Risk Manager — Menedżer Ryzyka (C++)
 * ============================================================================
 *
 *  STRESZCZENIE
 *  ------------
 *  Menedżer ryzyka jest "bramkarzem" na hot-pathie zleceń: każde zlecenie
 *  z `strategy` lub `router` MUSI przejść przez `check_order()` zanim trafi
 *  do `OMS`. Jeśli kontrola odmówi, zlecenie nigdy nie idzie na giełdę.
 *
 *  Miejsce w potoku:
 *      Strategy → Router → **Risk Manager** → OMS → Exchange
 *
 *  ARCHITEKTURA — TRZY STANY
 *  -------------------------
 *  Klasa trzyma trzy ortogonalne grupy stanu, każda ma własny cel:
 *
 *  1. Pozycje (positions_ + pending_)
 *     Co aktualnie mamy + co właśnie wysłaliśmy na giełdę ale jeszcze
 *     niezrealizowane. positions_[s] aktualizuje się TYLKO przy fillu;
 *     pending_[s] rośnie przy submit, maleje przy fill lub cancel.
 *
 *  2. P&L (daily_pnl_ + peak_pnl_)
 *     daily_pnl_ to suma realizowanych zysków/strat od początku sesji.
 *     peak_pnl_ to maksimum osiągnięte — punkt odniesienia dla drawdown.
 *
 *  3. Rate limiter (order_timestamps_)
 *     Kołowa kolejka znaczników czasu ostatnich zleceń. Stare wpisy
 *     (> 1s) są wyrzucane przy każdym check, więc rozmiar tej kolejki
 *     to liczba zleceń w ostatniej sekundzie.
 *
 *  Plus jedna **denormalizowana** wartość dla wydajności:
 *      total_abs_exposure_ = sum_s |positions_[s] + pending_[s]|
 *  Utrzymywana jako niezmiennik przez `adjust_pending()`. Dzięki temu
 *  sprawdzenie ekspozycji portfela w `check_order()` jest O(1) zamiast
 *  O(liczba_symboli).
 *
 *  KILL SWITCH — MASZYNA STANÓW
 *  ----------------------------
 *  kill_switch_active_ ma tylko dwa stany: false (handel włączony) i
 *  true (wszystko odrzucone). Aktywuje się na 3 sposoby:
 *      - circuit breaker: daily_pnl_ < -max_daily_loss
 *      - drawdown breach: (peak - daily) / peak > max_drawdown_pct
 *      - manualnie: activate_kill_switch()
 *  Wyłącza się na 2 sposoby:
 *      - reset_daily() (np. na początku nowej sesji)
 *      - deactivate_kill_switch() (manualnie, np. po naprawie problemu)
 *
 *  WYDAJNOŚĆ
 *  ---------
 *  Każdy `check_order()` wykonuje 7 kontroli w O(1):
 *      1. kill switch              — atomic bool read
 *      2. wartość zlecenia          — int * int compare
 *      3. limit pozycji per-symbol  — 2× hash lookup + abs
 *      4. ekspozycja portfela       — 1× hash lookup + 1× int subtract/add
 *      5. circuit breaker           — 1× double compare
 *      6. drawdown                  — 1× float divide
 *      7. rate limit                — pop_front pętla + size compare
 *
 *  Python equivalent osiągał ~200K checks/sec; ta wersja C++ osiąga
 *  ~30-50M checks/sec.
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <cstdio>
#include <cstdlib>     // std::remove
#include <cmath>
#include <string>      // persist_path_
#include <atomic>      // kill switch — czytany/pisany z wielu wątków (threaded pipeline)
#include <unistd.h>    // ::fsync, ::fileno

#include "../common/types.hpp"
#include "../common/symbol_key.hpp"
#include "../common/time_utils.hpp"


// ============================================================================
// RiskAction — co menedżer ryzyka decyduje dla zlecenia
// ============================================================================
//
// ALLOW  : zlecenie może iść dalej do OMS
// REJECT : pojedyncze zlecenie odrzucone (nie zmienia stanu menedżera)
// KILL   : krytyczne — kill switch został aktywowany, kolejne zlecenia
//          też będą REJECT dopóki nie wywołasz deactivate_kill_switch()
//          lub reset_daily()
//
// Reprezentacja jako uint8_t (1 bajt) — w packed struct RiskCheckResult
// nie marnujemy pamięci.
// ============================================================================

enum class RiskAction : uint8_t {
    ALLOW  = 0,
    REJECT = 1,
    KILL   = 2
};

// action_str: konwersja enum → string dla printf/log
inline const char* action_str(RiskAction a) noexcept {
    switch (a) {
        case RiskAction::ALLOW:  return "ALLOW";
        case RiskAction::REJECT: return "REJECT";
        case RiskAction::KILL:   return "KILL";
        default:                 return "UNKNOWN";
    }
}


// ============================================================================
// RiskCheckResult — co `check_order()` zwraca
// ============================================================================
//
// Pola:
//   action     : decyzja (ALLOW / REJECT / KILL)
//   reason     : krótki opis dlaczego (np. "Position limit exceeded").
//                Bufor o stałym rozmiarze 128 bajtów — bez alokacji
//                na stercie, kopiowane przez strncpy. Krótkie powody
//                (do ~50 znaków) mieszczą się z zapasem.
//   latency_ns : ile nanosekund zajęła sama kontrola (mono_ns z common/).
//                Używane do agregacji statystyk wydajności.
//
// Konstruktor domyślny → ALLOW z pustym powodem (sensowny default jeśli
// ktoś po prostu deklaruje `RiskCheckResult r;`).
//
// Konstruktor z 3 argumentami → przyjmuje wszystko, bezpiecznie kopiuje
// powód (strncpy + null terminator).
// ============================================================================

struct RiskCheckResult {
    RiskAction action;
    char       reason[128];
    int64_t    latency_ns;

    RiskCheckResult() noexcept
        : action(RiskAction::ALLOW), latency_ns(0) {
        reason[0] = '\0';
    }

    RiskCheckResult(RiskAction a, const char* r, int64_t lat) noexcept
        : action(a), latency_ns(lat) {
        std::strncpy(reason, r, 127);
        reason[127] = '\0';
    }
};


// ============================================================================
// RiskLimits — konfigurowalne progi
// ============================================================================
//
// Wszystkie te pola czyta tylko `RiskManager`. Wartości domyślne pasują
// do "rozsądnej średniej firmy" — produkcyjnie ładujesz z config.yaml
// przez config_loader.hpp.
//
// max_position_per_symbol : ile akcji jednego tickera maksymalnie
//                            możesz mieć long lub short (abs(net) ≤ limit)
// max_portfolio_exposure  : suma absolutnych pozycji po wszystkich tickerach
// max_daily_loss          : dzienny limit straty w dolarach — przekroczenie
//                            włącza kill switch (circuit breaker)
// max_orders_per_second   : rate limit, deque pruning okno 1-sek
// max_order_value         : maksymalna wartość JEDNEGO zlecenia (cena × qty)
// max_drawdown_pct        : maksymalny % spadek od peak_pnl_, np. 5.0 = 5%
// ============================================================================

struct RiskLimits {
    int32_t  max_position_per_symbol;
    int32_t  max_portfolio_exposure;
    int64_t  max_daily_loss;
    int32_t  max_orders_per_second;
    int64_t  max_order_value;
    double   max_drawdown_pct;
    // Fat-finger / price-band: maks. % odchylenia ceny zlecenia od ceny
    // referencyjnej symbolu (last/mid z market data). ≤ 0 = wyłączony.
    // 20% = standardowy luźny band (NMS LULD jest ciaśniejszy per-tier, ale
    // ten check łapie głównie grube pomyłki: 1500 zamiast 150).
    double   max_price_band_pct;

    RiskLimits() noexcept
        : max_position_per_symbol(5000),
          max_portfolio_exposure(50000),
          max_daily_loss(100000),
          max_orders_per_second(1000),
          max_order_value(500000),
          max_drawdown_pct(5.0),
          max_price_band_pct(20.0) {}
};


// ============================================================================
// RiskManager — główna klasa
// ============================================================================
//
// API podzielone na trzy grupy:
//
//   Hot path (wywoływane przez strategy / router / OMS na każde zlecenie):
//     check_order(symbol, side, price, qty) → RiskCheckResult
//
//   Stan mutators (wywoływane po pozytywnej kontroli + akcji giełdowej):
//     on_order_sent(symbol, side, qty)        — rezerwacja pending
//     on_order_cancelled(symbol, side, rem)   — zwolnienie pending
//     update_position(symbol, side, qty)      — przepływ pending→realized
//     update_pnl(pnl_change)                  — wpisanie zysku/straty
//
//   Sterowanie (admin / nowa sesja):
//     activate_kill_switch()       — wymuś stan KILL
//     deactivate_kill_switch()     — pozwól wrócić do handlu
//     reset_daily()                — nowy dzień: zeruj P&L, pending, rate limit
//
//   Accessory (debug / monitoring):
//     get_position(symbol), get_pending(symbol), get_daily_pnl(),
//     get_total_checks(), get_total_rejects(), is_kill_switch_active(),
//     print_stats()
// ============================================================================

class RiskManager {

    // --------------------------------------------------------------------
    // Stan: progi (czytane na każdy check_order)
    // --------------------------------------------------------------------
    RiskLimits limits_;

    // --------------------------------------------------------------------
    // Stan: pozycje
    //
    // positions_[key]  = zrealizowana ilość (signed) — aktualizowana
    //                    tylko w update_position() po fillu
    // pending_[key]    = ilość w locie (signed: BUY = +qty, SELL = -qty)
    //                    rośnie przy on_order_sent, maleje przy
    //                    on_order_cancelled lub update_position
    //
    // Klucz = sym_to_key(const char*) z common/symbol_key.hpp.
    // Packed uint64_t (do 8 znaków ASCII jako bity) — bez alokacji
    // std::string przy każdym lookup. Ten sam schemat co OMS.
    // --------------------------------------------------------------------
    std::unordered_map<uint64_t, int32_t> positions_;
    std::unordered_map<uint64_t, int32_t> pending_;

    // --------------------------------------------------------------------
    // Stan: denormalizowana ekspozycja portfela
    //
    // total_abs_exposure_ = sum_s |positions_[s] + pending_[s]|
    //
    // Utrzymywane jako niezmiennik przez `adjust_pending()`. Bez tego
    // sprawdzenie #4 w check_order musiałoby iterować po wszystkich
    // symbolach — O(N). Z niezmiennikiem to O(1).
    // --------------------------------------------------------------------
    int64_t total_abs_exposure_;

    // --------------------------------------------------------------------
    // Stan: P&L
    //
    // daily_pnl_ : skumulowany realizowany P&L od początku dnia
    // peak_pnl_  : najwyższe daily_pnl_ widziane do tej pory (do drawdown)
    //
    // Aktualizowane przez update_pnl(). Resetowane przez reset_daily().
    // --------------------------------------------------------------------
    double daily_pnl_;
    double peak_pnl_;

    // --------------------------------------------------------------------
    // Stan: kill switch + opcjonalna persistencja
    // --------------------------------------------------------------------
    // atomic — w run_pipeline_threaded check_order (wątek konsumenta) czyta
    // równolegle z zapisem przez breaker/manual. Plain bool = data race (UB);
    // komentarz w check_order od początku zakładał "atomic bool read".
    std::atomic<bool> kill_switch_active_;
    std::string persist_path_;     // pusty = persistencja wyłączona

    // Ceny referencyjne per symbol (last/mid z market data) dla price-bandu.
    // Aktualizowane przez update_reference_price(); puste = check pominięty
    // dla symboli bez znanej ceny.
    std::unordered_map<uint64_t, double> ref_price_;

    // Lista zakazanych symboli (#84) — halt giełdowy, Reg SHO restriction,
    // brak locate na short, compliance freeze. Każde zlecenie na symbol z tej
    // listy jest odrzucane przed pozostałymi checkami pozycji.
    std::unordered_set<uint64_t> restricted_;

    // --------------------------------------------------------------------
    // Stan: rate limit
    //
    // Kolejka FIFO znaczników czasu (CLOCK_MONOTONIC). Przy każdym
    // check_order ze przodu wyrzucamy wpisy starsze niż 1 sekunda; jeśli
    // pozostały rozmiar ≥ max_orders_per_second → REJECT.
    //
    // std::deque (nie vector) bo pop_front jest O(1). Z vectorem trzeba
    // by było erase(begin, begin+N) = O(M) za każdym razem.
    // --------------------------------------------------------------------
    std::deque<int64_t> order_timestamps_;

    // --------------------------------------------------------------------
    // Stan: statystyki (do print_stats / monitoring)
    // --------------------------------------------------------------------
    uint64_t total_checks_;
    uint64_t total_rejects_;
    uint64_t total_latency_ns_;

public:

    // ====================================================================
    // Konstruktor
    // ====================================================================
    //
    // Domyślnie używa RiskLimits() (sensowne firmowe progi). Można podać
    // własne limity ładowane z config.yaml.
    // ====================================================================

    explicit RiskManager(const RiskLimits& limits = RiskLimits()) noexcept
        : limits_(limits),
          total_abs_exposure_(0),
          daily_pnl_(0.0),
          peak_pnl_(0.0),
          kill_switch_active_(false),
          total_checks_(0),
          total_rejects_(0),
          total_latency_ns_(0) {}


    // ====================================================================
    // check_order — siedem kontroli na hot-pathie
    // ====================================================================
    //
    // Kolejność checków jest świadoma — najtańsze najpierw, żeby przy
    // odrzuceniu nie marnować pracy:
    //
    //   1. Kill switch        : pojedynczy bool, ~1 ns
    //   2. Order value        : multiplication + compare, ~5 ns
    //   3. Per-symbol limit   : 2× hash lookup, ~30 ns
    //   4. Portfolio exposure : odjęcie/dodanie z niezmiennika, ~5 ns
    //   5. Circuit breaker    : double compare, ~2 ns
    //   6. Drawdown           : divide + compare, ~5 ns
    //   7. Rate limit         : deque pop pętla + compare, ~10-50 ns
    //
    // Łącznie ~60-100 ns dla allow path (gorący przypadek).
    //
    // Zwraca RiskCheckResult z action + reason + latency_ns. NIE rzuca
    // wyjątków (noexcept) — produkcyjny hot path nie może niespodziewanie
    // się rozwijać stos.
    // ====================================================================

    RiskCheckResult check_order(const char* symbol, Side side,
                                 double price, int32_t quantity) noexcept {
        const int64_t t0 = mono_ns();
        total_checks_++;

        // 1. Kill switch — odrzuć wszystko jeśli aktywny
        if (kill_switch_active_) return make_reject("Kill switch active", t0);

        // 1b. Restricted symbol — halt / Reg SHO / brak locate / freeze.
        if (!restricted_.empty() && restricted_.count(sym_to_key(symbol)))
            return make_reject("Symbol restricted", t0);

        // 2. Wartość pojedynczego zlecenia
        const int64_t order_value = static_cast<int64_t>(price * quantity);
        if (order_value > limits_.max_order_value)
            return make_reject("Order value exceeds limit", t0);

        // 2b. Price band (fat-finger) — cena zbyt daleko od referencyjnej.
        //     Łapie grube pomyłki (np. 1500.00 zamiast 150.00) zanim trafią na
        //     rynek. Pomijany gdy band wyłączony albo brak ceny ref dla symbolu.
        if (limits_.max_price_band_pct > 0.0) {
            const auto rp = ref_price_.find(sym_to_key(symbol));
            if (rp != ref_price_.end() && rp->second > 0.0) {
                const double dev_pct = std::fabs(price - rp->second) / rp->second * 100.0;
                if (dev_pct > limits_.max_price_band_pct)
                    return make_reject("Price band breach (fat-finger)", t0);
            }
        }

        // 3 + 4. Limity pozycji (per-symbol i portfolio) — wspólny lookup
        const uint64_t key       = sym_to_key(symbol);
        const int32_t  signed_n  = signed_qty(side, quantity);
        const int32_t  cur_pos   = lookup(positions_, key);
        const int32_t  cur_pend  = lookup(pending_,   key);
        const int32_t  projected = cur_pos + cur_pend + signed_n;
        if (std::abs(projected) > limits_.max_position_per_symbol)
            return make_reject("Position limit exceeded", t0);

        // Ekspozycja portfela: O(1) dzięki niezmiennikowi total_abs_exposure_
        const int32_t old_contrib = std::abs(cur_pos + cur_pend);
        const int32_t new_contrib = std::abs(projected);
        if (total_abs_exposure_ - old_contrib + new_contrib > limits_.max_portfolio_exposure)
            return make_reject("Portfolio exposure exceeded", t0);

        // 5 + 6. Kontrole P&L (circuit breaker + drawdown). Obie mogą
        //        włączyć kill switch — wtedy następne check'i też dostaną REJECT.
        if (const char* fail = check_pnl_breakers())
            return make_reject(fail, t0);

        // 7. Rate limit (oddzielny helper — czystszy main flow)
        const int64_t now = mono_ns();
        if (!check_rate_limit(now))
            return make_reject("Rate limit exceeded", t0);

        // Wszystkie 7 kontroli przeszły
        const int64_t elapsed = mono_ns() - t0;
        total_latency_ns_ += elapsed;
        return RiskCheckResult(RiskAction::ALLOW, "All checks passed", elapsed);
    }


    // ====================================================================
    // Mutator: on_order_sent
    // ====================================================================
    //
    // Wywoływany ZA `check_order()` jeśli ALLOW i zlecenie zostało
    // wysłane na giełdę. Rezerwuje miejsce w pending_, tak żeby kolejne
    // check_order dla tego samego symbolu znał już "in-flight" exposure.
    //
    // BUY: pending += qty, SELL: pending -= qty.
    // ====================================================================
    void on_order_sent(const char* symbol, Side side, int32_t quantity) noexcept {
        adjust_pending(sym_to_key(symbol), signed_qty(side, quantity));
    }


    // ====================================================================
    // Mutator: on_order_cancelled
    // ====================================================================
    //
    // Wywoływany gdy zlecenie zostało anulowane PRZED fillem (lub
    // częściowo wypełnione i reszta anulowana). Zwalnia tyle pending,
    // ile nie zostało wypełnione (parametr `remaining`).
    //
    // Algebra: odwrotny znak do on_order_sent (BUY cancel = -, SELL cancel = +).
    // ====================================================================
    void on_order_cancelled(const char* symbol, Side side, int32_t remaining) noexcept {
        adjust_pending(sym_to_key(symbol), -signed_qty(side, remaining));
    }


    // ====================================================================
    // Mutator: update_position
    // ====================================================================
    //
    // Wywoływany po fillu. Przepływ ilości z pending do positions:
    //   positions_[s] += signed_q
    //   pending_[s]   -= signed_q
    //
    // Suma pos+pend pozostaje BEZ ZMIAN, więc total_abs_exposure_
    // nie wymaga aktualizacji. Po to istnieje rozdzielenie pending
    // od positions — żeby fill nie wymagał recompute'u niezmiennika.
    // ====================================================================
    void update_position(const char* symbol, Side side, int32_t quantity) noexcept {
        const int32_t  signed_q = signed_qty(side, quantity);
        const uint64_t key      = sym_to_key(symbol);
        positions_[key] += signed_q;
        pending_[key]   -= signed_q;
    }


    // ====================================================================
    // Mutator: update_pnl
    // ====================================================================
    //
    // Wywoływany po każdym fillu z realizowanym P&L (delta — nie suma).
    // Aktualizuje:
    //   daily_pnl_ += pnl_change
    //   peak_pnl_  = max(peak_pnl_, daily_pnl_)
    //
    // peak_pnl_ to high-water mark do liczenia drawdown w check #6.
    // ====================================================================
    void update_pnl(double pnl_change) noexcept {
        daily_pnl_ += pnl_change;
        if (daily_pnl_ > peak_pnl_) peak_pnl_ = daily_pnl_;
    }


    // ====================================================================
    // Mutator: update_reference_price
    // ====================================================================
    //
    // Wywoływany z market data (last trade / mid) dla price-bandu. Bez znanej
    // ceny referencyjnej fat-finger check dla danego symbolu jest pomijany —
    // pierwsze zlecenie na świeży symbol przechodzi, kolejne są walidowane
    // względem ostatniej ceny.
    // ====================================================================
    void update_reference_price(const char* symbol, double price) noexcept {
        if (price > 0.0) ref_price_[sym_to_key(symbol)] = price;
    }


    // ====================================================================
    // Restricted-symbol list (#84)
    // ====================================================================
    //
    // restrict_symbol  — wpisz symbol na listę zakazanych (halt/Reg SHO/freeze)
    // allow_symbol     — zdejmij z listy (np. wznowienie po halt'cie)
    // is_restricted    — czy symbol jest aktualnie zakazany
    // clear_restricted — wyczyść całą listę (np. nowa sesja)
    // ====================================================================
    void restrict_symbol(const char* symbol) noexcept { restricted_.insert(sym_to_key(symbol)); }
    void allow_symbol(const char* symbol)    noexcept { restricted_.erase(sym_to_key(symbol)); }
    bool is_restricted(const char* symbol) const noexcept {
        return restricted_.count(sym_to_key(symbol)) != 0;
    }
    void clear_restricted() noexcept { restricted_.clear(); }


    // ====================================================================
    // Kill switch — manualne sterowanie + persistencja
    // ====================================================================
    //
    // Po trip'ie (manualnym ALBO automatycznym przez update_pnl) zapisujemy
    // stan na dysk. Restart procesu w trakcie dnia handlowego musi WIDZIEĆ
    // że limit już dziś przekroczony — inaczej trader właśnie obszedł halt
    // resetując proces. load_persisted_state() przy startupie wczytuje.
    //
    // Format pliku (text dla audytu): "active=1\nlast_pnl=-12345.67\n"
    // Po manualnym deactivate trzeba dodatkowo wywołać clear_persisted_state().
    void activate_kill_switch() noexcept {
        kill_switch_active_ = true;
        persist_state();
    }
    void deactivate_kill_switch() noexcept {
        kill_switch_active_ = false;
        persist_state();   // persistuj też "wyłączony" żeby restart widział aktualny stan
    }
    bool is_kill_switch_active() const noexcept { return kill_switch_active_; }

    // set_persist_path: opcjonalny plik do persistencji stanu kill switcha.
    // Wywołaj raz po konstrukcji. Bez tego persistencja jest no-op (zachowanie
    // wstecz kompatybilne).
    void set_persist_path(const char* path) noexcept {
        if (path && *path) persist_path_ = path;
        else                persist_path_.clear();
    }

    // load_persisted_state: wczytaj stan zapisany w persist_path_. Wywołaj
    // PO konstrukcji + set_persist_path, PRZED przyjmowaniem zleceń. Zwraca
    // true gdy plik wczytany (active+pnl zaktualizowane), false gdy brak.
    bool load_persisted_state() noexcept {
        if (persist_path_.empty()) return false;
        FILE* f = std::fopen(persist_path_.c_str(), "r");
        if (!f) return false;
        int    active = 0;
        double pnl    = 0.0;
        const int n = std::fscanf(f, "active=%d\nlast_pnl=%lf\n", &active, &pnl);
        std::fclose(f);
        if (n < 1) return false;
        kill_switch_active_ = (active != 0);
        if (n >= 2) daily_pnl_ = pnl;  // przywróć też P&L żeby drawdown nie wystartował od zera
        return true;
    }

    // clear_persisted_state: usuń plik (np. po reset_daily na nowy dzień).
    void clear_persisted_state() noexcept {
        if (!persist_path_.empty()) std::remove(persist_path_.c_str());
    }


    // ====================================================================
    // reset_daily — start nowego dnia handlowego
    // ====================================================================
    //
    // Zeruje wszystkie stan'y "dzienne":
    //   - daily_pnl_ i peak_pnl_   → 0
    //   - rate limiter             → pusty (nowa minuta od zera)
    //   - pending_                 → pusty (otwarte zlecenia z poprzedniego
    //                                dnia są anulowane przez giełdę o północy)
    //   - kill_switch_active_      → false (na czysto)
    //
    // Pozycje (positions_) ZACHOWANE — overnight holdings zostają.
    // Po reset trzeba przeliczyć total_abs_exposure_ z samych
    // positions_, bo pending_ jest teraz pusty.
    // ====================================================================
    void reset_daily() noexcept {
        daily_pnl_ = 0.0;
        peak_pnl_  = 0.0;
        order_timestamps_.clear();
        pending_.clear();
        kill_switch_active_ = false;
        total_abs_exposure_ = 0;
        for (const auto& kv : positions_) total_abs_exposure_ += std::abs(kv.second);
        clear_persisted_state();   // nowy dzień = czyste konto, plik nieaktualny
    }


    // ====================================================================
    // Accessory (read-only)
    // ====================================================================
    int32_t  get_position(const char* symbol) const noexcept { return lookup(positions_, sym_to_key(symbol)); }
    int32_t  get_pending(const char* symbol)  const noexcept { return lookup(pending_,   sym_to_key(symbol)); }
    double   get_daily_pnl()                  const noexcept { return daily_pnl_; }
    uint64_t get_total_checks()               const noexcept { return total_checks_; }
    uint64_t get_total_rejects()              const noexcept { return total_rejects_; }


    // ====================================================================
    // print_stats — dump dla debug / monitoring
    // ====================================================================
    void print_stats() const {
        printf("\n=== Risk Manager Statistics ===\n");
        printf("  Total checks: %lu\n", (unsigned long)total_checks_);
        printf("  Allowed:      %lu\n", (unsigned long)(total_checks_ - total_rejects_));
        printf("  Rejected:     %lu\n", (unsigned long)total_rejects_);
        const double avg = total_checks_ > 0
            ? static_cast<double>(total_latency_ns_) / total_checks_ : 0.0;
        printf("  Avg latency:  %.0f ns/check\n", avg);
        printf("  Kill switch:  %s\n", kill_switch_active_ ? "ACTIVE" : "inactive");
        printf("  Daily P&L:    $%.2f\n", daily_pnl_);
    }


// ============================================================================
// Część prywatna — helpery wewnętrzne
// ============================================================================

private:

    // signed_qty: zamiana (side, qty) na sygnowaną liczbę.
    // BUY  → +qty
    // SELL → -qty
    // Używane 4× w klasie — wyciągnięte żeby nie powtarzać tego samego
    // ternary'a.
    static int32_t signed_qty(Side side, int32_t qty) noexcept {
        return (side == Side::BUY) ? qty : -qty;
    }


    // lookup: read-only dostęp do mapy bez wstawiania default-value.
    // Zwraca wartość lub 0, jeśli klucza nie ma. Używane przy obu mapach
    // (positions_, pending_).
    static int32_t lookup(const std::unordered_map<uint64_t, int32_t>& m,
                          uint64_t key) noexcept {
        auto it = m.find(key);
        return (it != m.end()) ? it->second : 0;
    }


    // adjust_pending: wspólny mutator dla on_order_sent / on_order_cancelled.
    // Trzyma niezmiennik total_abs_exposure_ = sum_s |pos[s] + pend[s]|.
    // Czysty O(1).
    void adjust_pending(uint64_t key, int32_t delta) noexcept {
        const int32_t pos      = lookup(positions_, key);
        const int32_t old_pend = lookup(pending_,   key);
        const int32_t new_pend = old_pend + delta;
        pending_[key]         = new_pend;
        total_abs_exposure_  += std::abs(pos + new_pend) - std::abs(pos + old_pend);
    }


    // check_pnl_breakers: kontrole #5 i #6 z check_order.
    //
    // Zwraca:
    //   nullptr           — wszystko OK, pozwól handlować
    //   const char* str   — powód odrzucenia (kill switch też włącza
    //                        się jako side-effect tej funkcji)
    //
    // Po breach'u kill_switch_active_ = true. Każdy następny check_order
    // odrzuci na #1 zanim w ogóle dotrze tu.
    const char* check_pnl_breakers() noexcept {
        // 5. Circuit breaker — przekroczona dzienna strata
        if (daily_pnl_ < -static_cast<double>(limits_.max_daily_loss)) {
            kill_switch_active_ = true;
            persist_state();   // restart procesu nie może obejść trip'a
            return "Circuit breaker: daily loss limit";
        }
        // 6. Drawdown — % spadek od peak_pnl_ przekroczył próg
        if (peak_pnl_ > 0.0) {
            const double drawdown_pct = (peak_pnl_ - daily_pnl_) / peak_pnl_ * 100.0;
            if (drawdown_pct > limits_.max_drawdown_pct) {
                kill_switch_active_ = true;
                persist_state();
                return "Drawdown limit exceeded";
            }
        }
        return nullptr;
    }

    // persist_state: atomowo zapisz active+daily_pnl do persist_path_.
    // Atomic write = tmpfile + rename (rename na tym samym fs jest atomowy).
    // Bez tego pad procesu w trakcie write zostawiłby uszkodzony plik.
    void persist_state() noexcept {
        if (persist_path_.empty()) return;
        std::string tmp = persist_path_ + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "active=%d\nlast_pnl=%.6f\n",
                     kill_switch_active_ ? 1 : 0, daily_pnl_);
        std::fflush(f);
        ::fsync(::fileno(f));       // durable do fizycznego dysku (POSIX, nie std::)
        std::fclose(f);
        std::rename(tmp.c_str(), persist_path_.c_str());
    }


    // check_rate_limit: kontrola #7 z check_order.
    //
    // Strategia: trzymaj FIFO znaczników czasu zleceń. Przed sprawdzeniem
    // wyrzuć wszystkie starsze niż 1s (deque.pop_front). Następnie:
    //   jeśli rozmiar ≥ limit → REJECT
    //   inaczej               → push_back(now), ALLOW
    //
    // Amortyzowane O(1) per check (każdy timestamp jest wstawiany raz
    // i wyrzucany raz w ciągu swojego życia ≤ 1s).
    //
    // Zwraca true (ALLOW), false (REJECT).
    bool check_rate_limit(int64_t now) noexcept {
        const int64_t one_sec_ago = now - 1'000'000'000;
        while (!order_timestamps_.empty() && order_timestamps_.front() <= one_sec_ago)
            order_timestamps_.pop_front();
        if (static_cast<int32_t>(order_timestamps_.size()) >= limits_.max_orders_per_second)
            return false;
        order_timestamps_.push_back(now);
        return true;
    }


    // make_reject: skraca powtarzalny pattern w check_order.
    // Liczy latency, inkrementuje total_rejects_, zbiera total_latency_ns_,
    // zwraca RiskCheckResult{REJECT, reason, elapsed}.
    RiskCheckResult make_reject(const char* reason, int64_t t0) noexcept {
        const int64_t elapsed = mono_ns() - t0;
        total_rejects_++;
        total_latency_ns_ += elapsed;
        return RiskCheckResult(RiskAction::REJECT, reason, elapsed);
    }
};
