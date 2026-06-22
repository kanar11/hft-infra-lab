/*
 * SmartOrderRouter (SOR) — wybiera na którą giełdę wysłać zlecenie.
 *
 * Ta sama akcja handluje się równolegle na wielu giełdach (NYSE, NASDAQ,
 * BATS, IEX, ARCA...). Każda ma inną cenę, inną opłatę i inną latencję.
 * SOR decyduje gdzie posłać zlecenie żeby zrealizować "best execution".
 *
 * Trzy strategie:
 *   BEST_PRICE      — najlepsza cena EFEKTYWNA (all-in, z opłatą/rebate)
 *   LOWEST_LATENCY  — najszybsze venue round-trip
 *   SPLIT           — rozbij duże zlecenie między venue wg dostępnej płynności
 *
 * Cena efektywna (all-in) — kluczowy realizm:
 *   Prawdziwy SOR NIE optymalizuje surowej ceny quote, tylko cenę PO opłatach.
 *   Venue z asks $223.48 i opłatą $0.005/akcja daje all-in $223.485 — gorzej
 *   niż $223.49 z rebate -$0.002 (all-in $223.488)... a właściwie lepiej, ale
 *   przy większych opłatach kolejność się odwraca. Maker/taker model: dodatnia
 *   opłata = bierzesz płynność (taker), ujemna = dajesz płynność (maker rebate).
 *
 *     BUY:  all_in = ask + fee   (płacisz; rebate ujemny → płacisz mniej)
 *     SELL: all_in = bid - fee   (dostajesz; rebate ujemny → dostajesz więcej)
 *
 *   BEST_PRICE dla BUY minimalizuje all_in, dla SELL maksymalizuje all_in.
 *
 * Pipeline: Strategy → Risk → ROUTER → OMS → Exchange.
 *
 * Wydajność (lab): ~9.7M routes/sec, p50=70ns, p99=150ns.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>


// Maks. liczba venue (stała tablica, zero heap na hot path).
static constexpr int MAX_VENUES = 16;


enum class RoutingStrategy : uint8_t {
    BEST_PRICE      = 0,
    LOWEST_LATENCY  = 1,
    SPLIT           = 2
};


// RouteReject — DLACZEGO route_order nie zwrócił trasy (#125). Caller (OMS /
// strategia) odróżnia "nie ma gdzie routować" od "brak płynności teraz".
enum class RouteReject : uint8_t {
    NONE          = 0,   // trasa znaleziona (valid=true)
    NO_VENUES     = 1,   // zero aktywnych venue w ogóle
    NO_LIQUIDITY  = 2,   // venue są, ale brak płynności po stronie zlecenia
};

inline const char* route_reject_str(RouteReject r) noexcept {
    switch (r) {
        case RouteReject::NONE:         return "NONE";
        case RouteReject::NO_VENUES:    return "NO_VENUES";
        case RouteReject::NO_LIQUIDITY: return "NO_LIQUIDITY";
        default:                        return "UNKNOWN";
    }
}


// Venue — pojedyncza giełda z jej top-of-book quote, opłatą i latencją.
//
// latency_ns (mean): średnia latencja round-trip. Jeśli latency_p99_ns > 0
// (opt-in), LOWEST_LATENCY wybiera po nim zamiast po mean — to realne
// zachowanie production SOR'ów, bo ogon ma długi (p99/p50 zwykle 5-10×)
// i decyduje o jakości egzekucji ważniejszej niż average case.
struct Venue {
    char    name[16];
    int64_t latency_ns;          // statyczna średnia (seed z configu / SLA venue)
    int64_t latency_p99_ns;      // p99 — 0 = nie ustawione, wtedy LOWEST_LATENCY bierze mean
    double  fee_per_share;       // dodatnia = taker fee, ujemna = maker rebate
    double  best_bid;
    double  best_ask;
    int32_t bid_size;
    int32_t ask_size;
    bool    is_active;
    // EWMA zmierzonej latencji round-trip (0 = brak próbek). Aktualizowana
    // przez record_latency() z realnych egzekucji — routing adaptuje się do
    // bieżących warunków zamiast ufać statycznym liczbom z configu.
    double  ewma_latency_ns;
    uint32_t latency_samples;
    uint32_t consecutive_failures;  // health (#86): seria odrzuceń/timeoutów
    int64_t  routed_shares;         // TCA (#117): laczny zaroutowany wolumen
    uint64_t routes_count;          // ile razy zlecenie trafilo na to venue

    Venue() noexcept
        : latency_ns(0), latency_p99_ns(0), fee_per_share(0),
          best_bid(0), best_ask(0), bid_size(0), ask_size(0), is_active(true),
          ewma_latency_ns(0.0), latency_samples(0), consecutive_failures(0),
          routed_shares(0), routes_count(0) {
        name[0] = '\0';
    }

    Venue(const char* n, int64_t lat, double fee) noexcept
        : latency_ns(lat), latency_p99_ns(0), fee_per_share(fee),
          best_bid(0), best_ask(0), bid_size(0), ask_size(0), is_active(true),
          ewma_latency_ns(0.0), latency_samples(0), consecutive_failures(0),
          routed_shares(0), routes_count(0) {
        std::strncpy(name, n, 15);
        name[15] = '\0';
    }

    // selection_latency_ns: która latencja używana przez LOWEST_LATENCY.
    // Priorytet: zmierzona EWMA (gdy są próbki) > p99 (gdy ustawione) > mean.
    // Realny SOR woli OBSERWOWANĄ latencję nad deklarowaną — venue zwalnia pod
    // obciążeniem, a my reagujemy w kolejnych decyzjach.
    int64_t selection_latency_ns() const noexcept {
        if (latency_samples > 0) return static_cast<int64_t>(ewma_latency_ns);
        return latency_p99_ns > 0 ? latency_p99_ns : latency_ns;
    }
};


// RouteDecision — co router zwraca. Oprócz venue/price/qty wystawia teraz
// cenę efektywną i łączną opłatę, żeby wywołujący widział PRAWDZIWY koszt
// (a nie tylko quote). num_venues > 1 dla SPLIT.
struct RouteDecision {
    char    venue[16];          // dla SPLIT: venue z największą alokacją
    double  price;              // średnia cena egzekucji (sam quote, bez opłat)
    double  effective_price;    // all-in per share (quote ± opłata)
    double  total_fee;          // łączna opłata za całe zlecenie (może być ujemna = rebate)
    int32_t quantity;           // ile akcji faktycznie zaroutowano (filled)
    int32_t unfilled_qty;       // shortfall: brak płynności na pokrycie reszty
    int32_t num_venues;         // ile venue użyto (SPLIT)
    int64_t latency_ns;         // czas decyzji routera (nie venue round-trip!)
    bool    valid;              // false = brak trasy (jak None w Pythonie)
    RouteReject reject_reason;  // dlaczego brak trasy (#125; NONE gdy valid)

    RouteDecision() noexcept
        : price(0), effective_price(0), total_fee(0), quantity(0),
          unfilled_qty(0), num_venues(0), latency_ns(0), valid(false),
          reject_reason(RouteReject::NONE) {
        venue[0] = '\0';
    }
};


class SmartOrderRouter {
    Venue           venues_[MAX_VENUES];
    int             venue_count_;
    RoutingStrategy default_strategy_;
    int32_t         split_threshold_;
    uint32_t        max_failures_ = 3;   // health (#86): próg auto-wyłączenia venue

    uint64_t total_routes_;
    uint64_t total_rejected_;
    uint64_t total_latency_ns_;
    double   total_fees_paid_ = 0.0;   // suma fee/rebate po trasach (#138; <0 = net rebate)

    static int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    // Cena efektywna (all-in) — quote skorygowany o opłatę/rebate.
    // BUY niższa = lepsza, SELL wyższa = lepsza.
    static double effective_price(const Venue& v, bool is_buy) noexcept {
        return is_buy ? v.best_ask + v.fee_per_share
                      : v.best_bid - v.fee_per_share;
    }

public:
    SmartOrderRouter(RoutingStrategy strat = RoutingStrategy::BEST_PRICE,
                     int32_t split_thresh = 500) noexcept
        : venue_count_(0),
          default_strategy_(strat),
          split_threshold_(split_thresh),
          total_routes_(0),
          total_rejected_(0),
          total_latency_ns_(0) {}

    void add_venue(const Venue& v) noexcept {
        if (venue_count_ < MAX_VENUES) venues_[venue_count_++] = v;
    }

    // remove_venue: calkowicie usun venue z routingu (#170) — decommission,
    // nie tylko wylaczenie (set_venue_active). Przesuwa tablice. Zwraca false
    // gdy nieznane venue.
    bool remove_venue(const char* venue_name) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                for (int j = i; j < venue_count_ - 1; ++j) venues_[j] = venues_[j + 1];
                --venue_count_;
                return true;
            }
        }
        return false;
    }

    // record_latency: zasil EWMA zmierzoną latencją round-trip (ns) z realnej
    // egzekucji. alpha=0.2 — świeże próbki ważą 20%, wygładza szum ale reaguje
    // na trend. Po pierwszej próbce EWMA = ona; potem wykładnicze wygładzanie.
    void record_latency(const char* venue_name, int64_t observed_ns) noexcept {
        if (observed_ns <= 0) return;
        constexpr double alpha = 0.2;
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                Venue& v = venues_[i];
                v.ewma_latency_ns = (v.latency_samples == 0)
                    ? static_cast<double>(observed_ns)
                    : alpha * static_cast<double>(observed_ns) + (1.0 - alpha) * v.ewma_latency_ns;
                ++v.latency_samples;
                v.consecutive_failures = 0;   // udany round-trip = zdrowe venue
                v.is_active = true;
                return;
            }
        }
    }

    // === Venue health (#86) ===
    // Realny SOR przestaje routować na venue, które odrzuca/timeoutuje, i wraca
    // gdy znów odpowiada. record_reject zlicza serię porażek (≥ próg → wyłącz);
    // record_success / record_latency zerują serię i reaktywują.
    void set_failure_threshold(uint32_t n) noexcept { if (n > 0) max_failures_ = n; }

    void record_reject(const char* venue_name) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                if (++venues_[i].consecutive_failures >= max_failures_)
                    venues_[i].is_active = false;     // wypadł z puli kandydatów
                return;
            }
        }
    }
    void record_success(const char* venue_name) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].consecutive_failures = 0;
                venues_[i].is_active = true;
                return;
            }
        }
    }
    bool venue_active(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0) return venues_[i].is_active;
        return false;
    }
    // set_venue_active: manualne wlaczenie/wylaczenie venue (#146) — admin control
    // (maintenance, regulatory halt) niezalezny od auto-health. Wlaczenie zeruje
    // serie porazek. Zwraca false gdy nieznane venue.
    bool set_venue_active(const char* venue_name, bool active) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].is_active = active;
                if (active) venues_[i].consecutive_failures = 0;
                return true;
            }
        }
        return false;
    }
    // venue_ewma_latency: zmierzona EWMA latencji venue w ns (#146; 0 = brak probek).
    double venue_ewma_latency(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0) return venues_[i].ewma_latency_ns;
        return 0.0;
    }

    // update_quote: aktualizuj top-of-book venue (na każdy market data tick).
    void update_quote(const char* venue_name, double bid, double ask,
                      int32_t bid_size, int32_t ask_size) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].best_bid = bid;
                venues_[i].best_ask = ask;
                venues_[i].bid_size = bid_size;
                venues_[i].ask_size = ask_size;
                return;
            }
        }
    }

    // route_order: wybierz venue dla zlecenia. side[0]=='B' → buy.
    RouteDecision route_order(const char* side, int32_t quantity,
                               RoutingStrategy strat) noexcept {
        const int64_t t0     = now_ns();
        const bool    is_buy = (side[0] == 'B');

        // Zbierz aktywnych kandydatów z faktyczną płynnością po naszej stronie.
        Venue* candidates[MAX_VENUES];
        int    num_candidates = 0;
        for (int i = 0; i < venue_count_; ++i) {
            if (!venues_[i].is_active) continue;
            const bool has_liq = is_buy
                ? (venues_[i].best_ask > 0 && venues_[i].ask_size > 0)
                : (venues_[i].best_bid > 0 && venues_[i].bid_size > 0);
            if (has_liq) candidates[num_candidates++] = &venues_[i];
        }
        if (num_candidates == 0) {
            ++total_rejected_;
            RouteDecision d;
            d.reject_reason = (venue_count_ == 0) ? RouteReject::NO_VENUES
                                                  : RouteReject::NO_LIQUIDITY;
            return d;
        }

        if (strat == RoutingStrategy::SPLIT && quantity >= split_threshold_) {
            return split_order(candidates, num_candidates, is_buy, quantity, t0);
        }

        // Single-venue strategie: BEST_PRICE (cena efektywna) lub LOWEST_LATENCY.
        Venue* best = candidates[0];
        if (strat == RoutingStrategy::LOWEST_LATENCY) {
            // Wybieramy po selection_latency (p99 gdy ustawione, inaczej mean).
            // Production SOR'y patrzą na ogon rozkładu, nie na średnią — p99
            // decyduje o jakości egzekucji w warunkach stress'u/burst'u.
            for (int i = 1; i < num_candidates; ++i) {
                if (candidates[i]->selection_latency_ns() < best->selection_latency_ns())
                    best = candidates[i];
            }
        } else {
            // BEST_PRICE (i fallback dla SPLIT poniżej progu) — cena efektywna.
            double best_eff = effective_price(*best, is_buy);
            for (int i = 1; i < num_candidates; ++i) {
                const double eff    = effective_price(*candidates[i], is_buy);
                const bool   better = is_buy ? (eff < best_eff) : (eff > best_eff);
                if (better) { best = candidates[i]; best_eff = eff; }
            }
        }

        // Respektuj dostępną płynność na wybranym venue — duże zlecenie może
        // przekroczyć top-of-book size. Reszta to shortfall (caller re-routuje
        // albo czeka). Realny SOR nie obiecuje fillu ponad widoczny rozmiar.
        const int32_t available = is_buy ? best->ask_size : best->bid_size;
        const int32_t filled    = std::min(quantity, available);

        RouteDecision d;
        d.valid           = true;
        std::strncpy(d.venue, best->name, 15);
        d.venue[15]       = '\0';
        d.price           = is_buy ? best->best_ask : best->best_bid;
        d.effective_price = effective_price(*best, is_buy);
        d.total_fee       = best->fee_per_share * filled;
        d.quantity        = filled;
        d.unfilled_qty    = quantity - filled;
        d.num_venues      = 1;
        d.latency_ns      = now_ns() - t0;

        best->routed_shares += filled;   // TCA per venue (#117)
        ++best->routes_count;
        ++total_routes_;
        total_latency_ns_ += d.latency_ns;
        total_fees_paid_  += d.total_fee;   // #138 kumulatywny koszt
        return d;
    }

    // Overload: użyj domyślnej strategii.
    RouteDecision route_order(const char* side, int32_t quantity) noexcept {
        return route_order(side, quantity, default_strategy_);
    }

    // === NBBO (National Best Bid/Offer) — #97 ===
    // Najlepsza cena AGREGOWANA po wszystkich aktywnych venue z płynnością.
    // Referencja do trade-through / best-execution; surowy quote (bez opłat).
    // Zwraca 0 gdy brak płynności po danej stronie.
    double national_best_bid() const noexcept {
        double best = 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.bid_size > 0 && v.best_bid > best) best = v.best_bid;
        }
        return best;
    }
    double national_best_ask() const noexcept {
        double best = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.ask_size > 0 && (!found || v.best_ask < best)) {
                best = v.best_ask; found = true;
            }
        }
        return found ? best : 0.0;
    }
    // nbbo_bid_venue / nbbo_ask_venue: which active venue holds the NBB / NBO (#294)
    // — the route target for each side. Mirrors national_best_bid/ask but returns the
    // venue name instead of the price. nullptr when no venue shows liquidity on that
    // side. Distinct from cheapest_venue (fee-adjusted) — this is the raw best quote.
    const char* nbbo_bid_venue() const noexcept {
        const char* who = nullptr; double best = 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.bid_size > 0 && v.best_bid > best) { best = v.best_bid; who = v.name; }
        }
        return who;
    }
    const char* nbbo_ask_venue() const noexcept {
        const char* who = nullptr; double best = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.ask_size > 0 && (!found || v.best_ask < best)) {
                best = v.best_ask; found = true; who = v.name;
            }
        }
        return who;
    }
    double nbbo_mid() const noexcept {
        const double b = national_best_bid(), a = national_best_ask();
        return (b > 0.0 && a > 0.0) ? (b + a) / 2.0 : 0.0;
    }
    // nbbo_spread: skonsolidowany spread NBBO (#208) = NBO - NBB po wszystkich
    // aktywnych venue. Zwykle CIASNIEJSZY niz na pojedynczej gieldzie (best bid i
    // best ask moga byc na roznych venue). <=0 sygnalizuje locked/crossed miedzy
    // venue (cross-venue arbitraz). 0 gdy brak dwustronnej plynnosci.
    double nbbo_spread() const noexcept {
        const double b = national_best_bid(), a = national_best_ask();
        return (b > 0.0 && a > 0.0) ? (a - b) : 0.0;
    }
    // nbbo_spread_bps: nbbo_spread wzgledem nbbo_mid w punktach bazowych — miara
    // jakosci skonsolidowanego rynku niezalezna od poziomu ceny.
    double nbbo_spread_bps() const noexcept {
        const double m = nbbo_mid();
        return m > 0.0 ? nbbo_spread() / m * 10000.0 : 0.0;
    }
    // nbbo_locked / nbbo_crossed: consolidated market-integrity checks across venues
    // (#270). LOCKED = national best bid == national best ask (spread zero) — a
    // Reg NMS locked market, usually a stale quote or a maker about to be hit.
    // CROSSED = NBB > NBO — a cross-venue arbitrage (buy the ask cheaper than you
    // sell the bid), in practice a feed inconsistency / lagging venue. Both false
    // without two-sided liquidity.
    bool nbbo_locked() const noexcept {
        const double b = national_best_bid(), a = national_best_ask();
        return b > 0.0 && a > 0.0 && b == a;
    }
    bool nbbo_crossed() const noexcept {
        const double b = national_best_bid(), a = national_best_ask();
        return b > 0.0 && a > 0.0 && b > a;
    }
    // internally_crossed_count: active venues posting a crossed quote on their OWN
    // book — best_bid > best_ask on the same venue (#302). A single venue should
    // never be self-crossed, so this is a hard data-quality red flag (stale/garbled
    // feed for that venue), distinct from nbbo_crossed (#270) which is a cross-venue
    // NBBO condition. Only counts venues quoting both sides.
    int internally_crossed_count() const noexcept {
        int c = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.best_bid > 0.0 && v.best_ask > 0.0 && v.best_bid > v.best_ask) ++c;
        }
        return c;
    }

    // effective_spread_bps: RZECZYWISTY koszt przejscia spreadu Z OPLATAMI (#240) =
    // best all-in ask (quote+fee) - best all-in bid (quote-fee), w bps. Wiekszy niz
    // nbbo_spread_bps (#208, sam quote) o round-trip fees: pokazuje ile naprawde
    // kosztuje wejscie+wyjscie po cenach takera. 0 bez dwustronnej plynnosci.
    double effective_spread_bps() const noexcept {
        const double eff_ask = best_effective_price(true);    // all-in kupna
        const double eff_bid = best_effective_price(false);   // all-in sprzedazy
        if (eff_ask <= 0.0 || eff_bid <= 0.0) return 0.0;
        const double m = (eff_ask + eff_bid) / 2.0;
        return m > 0.0 ? (eff_ask - eff_bid) / m * 10000.0 : 0.0;
    }
    // active_venue_count: ile venue jest aktywnych (po health/manual) (#154).
    int active_venue_count() const noexcept {
        int n = 0;
        for (int i = 0; i < venue_count_; ++i) if (venues_[i].is_active) ++n;
        return n;
    }
    // best_effective_price: najlepsza cena ALL-IN (quote ± fee) dostepna teraz po
    // danej stronie, bez routowania (#154). BUY: minimalna; SELL: maksymalna.
    // 0 gdy brak plynnosci. Inspekcja "ile bym zaplacil" przed decyzja.
    double best_effective_price(bool is_buy) const noexcept {
        double best = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const double eff = effective_price(v, is_buy);
            if (!found || (is_buy ? (eff < best) : (eff > best))) { best = eff; found = true; }
        }
        return found ? best : 0.0;
    }

    // is_marketable: czy zlecenie z limitem moglo by sie wykonac OD RAZU (#184) —
    // istnieje aktywne venue, ktorego cena all-in (quote ± fee) jest po wlasciwej
    // stronie limitu. BUY: najlepszy all-in ask <= limit; SELL: najlepszy all-in
    // bid >= limit. false gdy brak plynnosci. Pre-route guard dla limit orderow:
    // niemarketowalne -> odloz do ksiazki zamiast routowac w prozne.
    bool is_marketable(bool is_buy, double limit_price) const noexcept {
        const double best = best_effective_price(is_buy);
        if (best <= 0.0) return false;
        return is_buy ? (best <= limit_price) : (best >= limit_price);
    }

    // venue_effective_price: cena all-in (quote +/- fee) dla KONKRETNEGO venue po
    // nazwie (#248). Inspekcja / wycena zlecenia kierowanego (directed order) na
    // wskazana gielde, niezaleznie od best-price. 0 gdy nieznane, nieaktywne lub
    // brak plynnosci po danej stronie. Uzupelnia best_effective_price (skan wszystkich).
    double venue_effective_price(const char* venue_name, bool is_buy) const noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (std::strcmp(v.name, venue_name) != 0) continue;
            if (!v.is_active) return 0.0;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            return has_liq ? effective_price(v, is_buy) : 0.0;
        }
        return 0.0;
    }

    // cheapest_venue: NAZWA venue z najlepsza cena all-in (quote +/- fee) po danej
    // stronie (#200). Uzupelnia best_effective_price (sama cena) — tu wiadomo
    // GDZIE. BUY: min all-in ask; SELL: max all-in bid. nullptr gdy brak plynnosci.
    // Inspekcja/logowanie decyzji routingu bez wykonywania route_order.
    const char* cheapest_venue(bool is_buy) const noexcept {
        const char* best_name = nullptr;
        double best = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const double eff = effective_price(v, is_buy);
            if (!found || (is_buy ? (eff < best) : (eff > best))) {
                best = eff; best_name = v.name; found = true;
            }
        }
        return found ? best_name : nullptr;
    }

    // deepest_venue: name of the venue with the LARGEST displayed size on a side
    // (#255). BUY looks at ask_size, SELL at bid_size; only active venues with a
    // positive quote. Complements cheapest_venue (#200, best price): a large order
    // that prioritizes fill size over price routes to the deepest book. nullptr
    // when no venue has liquidity.
    const char* deepest_venue(bool is_buy) const noexcept {
        const char* best_name = nullptr;
        int32_t best = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const int32_t size = is_buy ? v.ask_size : v.bid_size;
            if (size > best) { best = size; best_name = v.name; }
        }
        return best_name;
    }

    // fastest_venue: name of the active venue with the lowest selection latency
    // (#278) — EWMA of observed round-trips if available, else the configured p99 /
    // static latency (selection_latency_ns). For latency-sensitive routing it
    // completes the "best-by-X" family: cheapest_venue (price), deepest_venue
    // (size), fastest_venue (speed). nullptr when no venue is active.
    const char* fastest_venue() const noexcept {
        const char* best_name = nullptr;
        int64_t best = 0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const int64_t lat = v.selection_latency_ns();
            if (!found || lat < best) { best = lat; best_name = v.name; found = true; }
        }
        return best_name;
    }

    // avg_venue_latency_ns: mean selection latency across all ACTIVE venues (#286).
    // Whereas fastest_venue gives the single best path, this is the market-wide
    // connectivity health — a rising average warns that the whole fabric (not just
    // one venue) is degrading. 0 when no venue is active.
    double avg_venue_latency_ns() const noexcept {
        int64_t sum = 0; int n = 0;
        for (int i = 0; i < venue_count_; ++i) {
            if (!venues_[i].is_active) continue;
            sum += venues_[i].selection_latency_ns();
            ++n;
        }
        return n > 0 ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    }

    // available_liquidity: laczny displayed size top-of-book po stronie zlecenia
    // (is_buy → asks, sprzedaz → bids), tylko aktywne venue z dodatnim quote.
    // Pre-route sizing: czy w ogole jest plynnosc na pokrycie zlecenia (#109).
    int32_t available_liquidity(bool is_buy) const noexcept {
        int32_t total = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (is_buy) { if (v.best_ask > 0 && v.ask_size > 0) total += v.ask_size; }
            else        { if (v.best_bid > 0 && v.bid_size > 0) total += v.bid_size; }
        }
        return total;
    }
    // available_liquidity_notional: total $ value of top-of-book liquidity across
    // active venues on the order side (#310) — buy hits asks (sum best_ask*ask_size),
    // sell hits bids (sum best_bid*bid_size). The $ companion to available_liquidity
    // (#109, shares): how much capital sits at the touch you can sweep immediately,
    // a more comparable size gate across symbols of different prices.
    double available_liquidity_notional(bool is_buy) const noexcept {
        double total = 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (is_buy) { if (v.best_ask > 0 && v.ask_size > 0) total += v.best_ask * v.ask_size; }
            else        { if (v.best_bid > 0 && v.bid_size > 0) total += v.best_bid * v.bid_size; }
        }
        return total;
    }
    // fill_shortfall: ile akcji zlecenia NIE pokryje wyswietlona plynnosc (#224) =
    // max(0, shares - available_liquidity). Reszta musialaby odlezec w ksiazce /
    // poczekac na nowy quote. Pre-route sizing.
    int32_t fill_shortfall(bool is_buy, int32_t shares) const noexcept {
        const int32_t avail = available_liquidity(is_buy);
        return shares > avail ? shares - avail : 0;
    }
    // fillable_ratio: jaka czesc zlecenia wykona sie od reki wg displayed (#224),
    // 0..1. min(avail, shares) / shares. 0 dla shares <= 0.
    double fillable_ratio(bool is_buy, int32_t shares) const noexcept {
        if (shares <= 0) return 0.0;
        const int32_t avail = available_liquidity(is_buy);
        const int32_t f = avail < shares ? avail : shares;
        return static_cast<double>(f) / static_cast<double>(shares);
    }
    // venue_liquidity_share: a named venue's share of the CURRENT displayed
    // liquidity on a side (#262), as a percent. Unlike venue_share_pct (#216,
    // historical routed volume) this is the live quote concentration right now —
    // a high share means the consolidated book leans on one venue's quote.
    // 0 for unknown / inactive / no liquidity.
    double venue_liquidity_share(const char* venue_name, bool is_buy) const noexcept {
        const int32_t total = available_liquidity(is_buy);
        if (total <= 0) return 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (std::strcmp(v.name, venue_name) != 0) continue;
            if (!v.is_active) return 0.0;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            const int32_t size = has_liq ? (is_buy ? v.ask_size : v.bid_size) : 0;
            return static_cast<double>(size) / static_cast<double>(total) * 100.0;
        }
        return 0.0;
    }

    int venue_count() const noexcept { return venue_count_; }
    // venue_routed_shares: laczny zaroutowany wolumen na dane venue (TCA, #117).
    int64_t venue_routed_shares(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0) return venues_[i].routed_shares;
        return 0;
    }
    // total_routed_shares: laczny wolumen zaroutowany po wszystkich venue (#130).
    int64_t total_routed_shares() const noexcept {
        int64_t s = 0;
        for (int i = 0; i < venue_count_; ++i) s += venues_[i].routed_shares;
        return s;
    }
    // venue_share_pct: jaki % calego zaroutowanego wolumenu trafil na dane venue
    // (#216) — koncentracja egzekucji do raportowania best-ex / TCA. Wysoki udzial
    // jednego venue moze wymagac uzasadnienia. 0 dla nieznanego lub przy zerowym
    // wolumenie.
    double venue_share_pct(const char* venue_name) const noexcept {
        const int64_t total = total_routed_shares();
        if (total <= 0) return 0.0;
        return static_cast<double>(venue_routed_shares(venue_name))
             / static_cast<double>(total) * 100.0;
    }
    // reset_routing_stats: wyzeruj liczniki TCA per venue (nowa sesja/okno).
    void reset_routing_stats() noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            venues_[i].routed_shares = 0;
            venues_[i].routes_count  = 0;
        }
    }

    // reset_session_stats: PELNY reset TCA na nowa sesje (#192) — zeruje liczniki
    // globalne (routes/rejects/latency/fees), ktorych reset_routing_stats NIE
    // ruszal, oraz per-venue. Venue i ich quote'y zostaja (nie trzeba ich na nowo
    // dodawac/kwotowac). Wolaj raz na otwarcie sesji.
    void reset_session_stats() noexcept {
        total_routes_     = 0;
        total_rejected_   = 0;
        total_latency_ns_ = 0;
        total_fees_paid_  = 0.0;
        reset_routing_stats();
    }

    // total_fees_paid: suma oplat/rebate po wszystkich trasach (#138). Ujemne =
    // net rebate (maker). Podstawa analizy kosztow egzekucji.
    double   total_fees_paid()    const noexcept { return total_fees_paid_; }
    // avg_fee_per_share: srednia ZREALIZOWANA oplata na akcje (#232) = total_fees /
    // total_routed_shares. Dodatnia = netto taker (placisz za plynnosc), ujemna =
    // netto maker (zbierasz rebate). Kluczowa miara TCA jakosci routingu. 0 gdy
    // nic nie zaroutowano.
    double   avg_fee_per_share() const noexcept {
        const int64_t total = total_routed_shares();
        return total > 0 ? total_fees_paid_ / static_cast<double>(total) : 0.0;
    }

    // set_venue_fee: runtime podmiana taryfy venue (#176). Harmonogramy oplat
    // zmieniaja sie (tier po wolumenie, promocja maker/taker); routing all-in
    // (quote ± fee) natychmiast uwzglednia nowa stawke. false gdy nieznane venue.
    bool set_venue_fee(const char* venue_name, double fee_per_share) noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].fee_per_share = fee_per_share;
                return true;
            }
        return false;
    }

    uint64_t get_total_routes()   const noexcept { return total_routes_; }
    uint64_t get_total_rejected() const noexcept { return total_rejected_; }
    // reject_rate: odsetek prob routingu zakonczonych odrzuceniem (#162) =
    // rejected / (routes + rejected). 0 gdy brak prob.
    double reject_rate() const noexcept {
        const uint64_t total = total_routes_ + total_rejected_;
        return total > 0 ? static_cast<double>(total_rejected_) / static_cast<double>(total) : 0.0;
    }
    // avg_routing_latency_ns: srednia latencja DECYZJI routera (nie round-trip
    // venue) na udana trase (#162).
    double avg_routing_latency_ns() const noexcept {
        return total_routes_ > 0 ? static_cast<double>(total_latency_ns_) / static_cast<double>(total_routes_) : 0.0;
    }

    void print_stats() const {
        printf("\n=== Router Statistics ===\n");
        printf("  Total routes: %lu\n", (unsigned long)total_routes_);
        printf("  Rejected: %lu\n", (unsigned long)total_rejected_);
        const double avg = total_routes_ > 0
            ? static_cast<double>(total_latency_ns_) / total_routes_ : 0.0;
        printf("  Avg routing latency: %.0f ns\n", avg);
    }

private:
    // split_order: rozbij duże zlecenie. Sortuje venue po cenie EFEKTYWNEJ
    // (nie surowej!) i wypełnia od najtańszego, aż wyczerpiemy zlecenie lub
    // płynność. Raportuje średnią cenę, łączną opłatę i liczbę użytych venue.
    RouteDecision split_order(Venue** candidates, int num, bool is_buy,
                               int32_t quantity, int64_t t0) noexcept {
        // Selection sort po cenie efektywnej (małe N → O(N²) bez znaczenia).
        for (int i = 0; i < num - 1; ++i) {
            for (int j = i + 1; j < num; ++j) {
                const double a = effective_price(*candidates[i], is_buy);
                const double b = effective_price(*candidates[j], is_buy);
                const bool   swap = is_buy ? (b < a) : (b > a);  // najlepsze najpierw
                if (swap) std::swap(candidates[i], candidates[j]);
            }
        }

        int32_t remaining   = quantity;
        int32_t filled      = 0;
        int32_t venues_used = 0;
        double  price_sum   = 0.0;   // Σ quote × alloc (do średniej ceny)
        double  fee_sum     = 0.0;   // Σ fee × alloc   (łączna opłata)

        for (int i = 0; i < num && remaining > 0; ++i) {
            const int32_t available = is_buy ? candidates[i]->ask_size
                                             : candidates[i]->bid_size;
            const int32_t alloc = std::min(remaining, available);
            if (alloc <= 0) continue;
            const double px = is_buy ? candidates[i]->best_ask : candidates[i]->best_bid;
            price_sum += px * alloc;
            fee_sum   += candidates[i]->fee_per_share * alloc;
            filled    += alloc;
            remaining -= alloc;
            candidates[i]->routed_shares += alloc;   // TCA per venue (#117)
            ++candidates[i]->routes_count;
            ++venues_used;
        }

        if (filled == 0) {
            ++total_rejected_;
            RouteDecision d;
            d.reject_reason = RouteReject::NO_LIQUIDITY;
            return d;
        }

        RouteDecision d;
        d.valid           = true;
        std::strncpy(d.venue, candidates[0]->name, 15);  // venue z najlepszą ceną = primary
        d.venue[15]       = '\0';
        d.price           = price_sum / filled;
        // all-in średnia: dla BUY koszt rośnie o fee, dla SELL maleje.
        d.effective_price = is_buy ? (price_sum + fee_sum) / filled
                                   : (price_sum - fee_sum) / filled;
        d.total_fee       = fee_sum;
        d.quantity        = filled;
        d.unfilled_qty    = quantity - filled;   // shortfall gdy Σpłynność < zlecenie
        d.num_venues      = venues_used;
        d.latency_ns      = now_ns() - t0;

        ++total_routes_;
        total_latency_ns_ += d.latency_ns;
        total_fees_paid_  += d.total_fee;   // #138 kumulatywny koszt
        return d;
    }
};
