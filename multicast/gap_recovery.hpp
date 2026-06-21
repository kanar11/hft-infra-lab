/*
 * GapRecovery — warstwa RECOVERY nad detekcją luk sekwencji (expansion #82).
 *
 * Wydzielona z multicast.hpp do osobnego, lekkiego nagłówka: multicast.hpp
 * ciągnie nagłówki gniazd (sys/socket) i globalny MsgType — a recovery to
 * czysta logika, którą chcemy testować/używać bez transportu. multicast.hpp
 * include'uje ten plik, więc multicast_demo nadal dostaje GapRecovery.
 *
 * SequenceTracker (multicast.hpp) tylko WYKRYWA luki. To pierwszy krok; realny
 * feed handler musi jeszcze ODZYSKAĆ braki: zapamiętać KTÓRE seq zgubił, wysłać
 * gap-fill request do serwera retransmisji (MoldUDP64 request / A-B arbitration)
 * i pogodzić retransmitowane pakiety z primary feedem.
 *
 * Model:
 *   observe(seq)        — primary feed; luka → wpis brakujących seq do `missing`
 *   on_retransmit(seq)  — pakiet z serwera recovery / linii B; usuwa z `missing`
 *   next_request(lo,hi) — zakres do gap-fill request (najniższy..najwyższy brak)
 *   has_gaps()/missing()— czy księga jest jeszcze niepewna
 */
#pragma once

#include <cstdint>
#include <set>
#include <map>
#include <vector>
#include <unordered_map>
#include <deque>
#include <iterator>
#include <utility>

namespace multicast {


// FeedRateMeter — przepustowosc feedu w oknie czasu (expansion #132).
//
// Feed handler musi widziec biezacy rate (msgs/sec) by wykryc burst (ryzyko
// przeladowania / drop) albo cisze. Sliding-window licznik znacznikow czasu:
// przy kazdym pomiarze wyrzuca starsze niz okno, rozmiar = liczba w oknie.
struct FeedRateMeter {
    std::int64_t      window_ns;
    std::deque<std::int64_t> ts;

    explicit FeedRateMeter(std::int64_t window_ns_ = 1'000'000'000) noexcept
        : window_ns(window_ns_ > 0 ? window_ns_ : 1) {}

    void on_message(std::int64_t now_ns) {
        ts.push_back(now_ns);
        evict(now_ns);
        if (ts.size() > peak_count_) peak_count_ = ts.size();   // #163 burst peak
    }
    std::size_t count(std::int64_t now_ns) {
        evict(now_ns);
        return ts.size();
    }
    double rate_per_sec(std::int64_t now_ns) {
        return static_cast<double>(count(now_ns)) * 1e9 / static_cast<double>(window_ns);
    }
    // peak_count / peak_rate_per_sec (#163): najwyzsza liczba/rate w oknie kiedy-
    // kolwiek zaobserwowana — wykrywa burst (ryzyko przeladowania) nawet jesli
    // chwilowy rate jest niski.
    std::size_t peak_count()        const noexcept { return peak_count_; }
    double      peak_rate_per_sec() const noexcept {
        return static_cast<double>(peak_count_) * 1e9 / static_cast<double>(window_ns);
    }
    void reset() noexcept { ts.clear(); peak_count_ = 0; }

private:
    std::size_t peak_count_ = 0;
public:

private:
    void evict(std::int64_t now_ns) {
        const std::int64_t cutoff = now_ns - window_ns;
        while (!ts.empty() && ts.front() <= cutoff) ts.pop_front();
    }
};

struct GapRecovery {
    std::set<uint64_t> missing;          // znane braki, czekają na retransmisję
    uint64_t expected    = 0;
    bool     initialized = false;
    uint64_t gap_events  = 0;            // ile osobnych zdarzeń luki
    uint64_t recovered   = 0;            // ile braków odzyskanych
    uint64_t duplicates  = 0;            // seq < expected i NIE był brakiem

    // observe: primary feed. W kolejności → OK; do przodu → wpisz lukę; do tyłu
    // → albo wypełnia znany brak (recovered), albo czysty duplikat.
    void observe(uint64_t seq) noexcept {
        if (!initialized) { initialized = true; expected = seq + 1; return; }
        if (seq == expected) { ++expected; return; }
        if (seq > expected) {
            for (uint64_t s = expected; s < seq; ++s) missing.insert(s);
            ++gap_events;
            expected = seq + 1;
            return;
        }
        if (missing.erase(seq)) ++recovered;   // spóźniony pakiet wypełnia lukę
        else                    ++duplicates;
    }

    // on_retransmit: pakiet z serwera recovery / linii B. Liczy się tylko gdy
    // realnie wypełnia znaną lukę. Zwraca true gdy coś odzyskano.
    bool on_retransmit(uint64_t seq) noexcept {
        if (missing.erase(seq)) { ++recovered; return true; }
        return false;
    }

    // next_request: zakres [lo,hi] do gap-fill request pokrywający aktualne
    // braki. Zwraca false gdy nie ma luk (księga znów pewna).
    bool next_request(uint64_t& lo, uint64_t& hi) const noexcept {
        if (missing.empty()) return false;
        lo = *missing.begin();
        hi = *missing.rbegin();
        return true;
    }

    bool   has_gaps()      const noexcept { return !missing.empty(); }
    size_t missing_count() const noexcept { return missing.size(); }

    // recovery_completeness (#156): ulamek wykrytych brakow ktore juz odzyskano
    // = recovered / (recovered + jeszcze_brakuje). 1.0 = nic nie zalega (ksiega
    // pewna), <1.0 = czesc luk wciaz otwarta. Metryka zdrowia recovery.
    double recovery_completeness() const noexcept {
        const std::uint64_t total = recovered + missing.size();
        return total > 0 ? static_cast<double>(recovered) / static_cast<double>(total) : 1.0;
    }

    // missing_ranges (#149): braki zgrupowane w CIAGLE przedzialy [begin,end].
    // next_request daje tylko min..max (moze obejmowac juz-odebrane); to daje
    // dokladne zakresy do gap-fill request (efektywniejsza retransmisja).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> missing_ranges() const {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        if (missing.empty()) return out;
        std::uint64_t start = *missing.begin(), prev = start;
        for (auto it = std::next(missing.begin()); it != missing.end(); ++it) {
            if (*it == prev + 1) { prev = *it; }
            else { out.emplace_back(start, prev); start = *it; prev = *it; }
        }
        out.emplace_back(start, prev);
        return out;
    }

    // recommend_snapshot (#115): gdy braki przekrocza prog, retransmisja per-pakiet
    // jest nieoplacalna — realny feed handler prosi o pelny SNAPSHOT i resynca.
    bool recommend_snapshot(std::size_t threshold) const noexcept {
        return threshold > 0 && missing.size() >= threshold;
    }

    // snapshot_resync: po odebraniu snapshotu na seq `snapshot_seq` wszystko
    // do niego wlacznie jest znane — czysci luki <= snapshot_seq i ustawia
    // expected. Braki powyzej snapshotu zostaja (przyjda normalnym strumieniem).
    void snapshot_resync(std::uint64_t snapshot_seq) noexcept {
        for (auto it = missing.begin(); it != missing.end(); ) {
            if (*it <= snapshot_seq) { it = missing.erase(it); ++recovered; }
            else break;   // set posortowany rosnaco — reszta jest > snapshot_seq
        }
        if (snapshot_seq + 1 > expected) expected = snapshot_seq + 1;
        initialized = true;
    }

    void reset() noexcept { *this = GapRecovery{}; }
};


// ABLineArbitrator — arbitraż dwóch redundantnych linii feedu (expansion #91).
//
// Giełdy wysyłają market data DWIEMA identycznymi liniami (A i B) tym samym
// UDP multicastem. Odbiorca bierze pakiet z TEJ linii, która dotarła pierwsza
// dla danego sequence, a duplikat z drugiej odrzuca. Jeśli linia A zgubi
// pakiet, linia B zwykle go dostarcza — luka się "samonaprawia" bez gap-fill
// requestu. To standardowa odporność feedu (NASDAQ/CME itp.).
//
// on_packet(seq, from_line_a) → true gdy pakiet NOWY (przekazany dalej),
// false gdy duplikat (druga linia już dostarczyła ten seq). Pod spodem
// GapRecovery daje zunifikowany obraz luk PO arbitrażu.
struct ABLineArbitrator {
    GapRecovery rec;          // stan po połączeniu obu linii
    uint64_t a_first = 0;     // ile razy linia A dostarczyła seq pierwsza
    uint64_t b_first = 0;     // ile razy linia B
    uint64_t dups    = 0;     // odrzucone duplikaty (druga linia)

    bool on_packet(uint64_t seq, bool from_line_a) noexcept {
        // Nowy = jeszcze nie skonsumowany: na/przed expected albo wypełnia lukę.
        const bool is_new = !rec.initialized
                          || seq >= rec.expected
                          || rec.missing.count(seq) != 0;
        if (!is_new) { ++dups; return false; }
        rec.observe(seq);                 // advance / recover (wspólna logika)
        if (from_line_a) ++a_first; else ++b_first;
        return true;
    }

    bool   has_gaps()      const noexcept { return rec.has_gaps(); }
    size_t missing_count() const noexcept { return rec.missing_count(); }
    void   reset()         noexcept { *this = ABLineArbitrator{}; }
};

// MultiChannelRecovery — gap-recovery po WIELU kanalach feedu (expansion #122).
//
// Realne giełdy dziela market data na osobne kanaly multicast (np. NASDAQ po
// zakresie symboli), kazdy z WLASNA numeracja sekwencji. Pojedynczy GapRecovery
// sledzi jeden kanal; ten agregator trzyma po jednym per channel_id i daje
// zbiorczy obraz: czy gdziekolwiek jest luka, ile lacznie brakuje/odzyskano.
struct MultiChannelRecovery {
    std::unordered_map<std::uint32_t, GapRecovery> channels;

    void observe(std::uint32_t channel_id, std::uint64_t seq) {
        channels[channel_id].observe(seq);
    }
    bool on_retransmit(std::uint32_t channel_id, std::uint64_t seq) {
        return channels[channel_id].on_retransmit(seq);
    }

    bool any_gaps() const {
        for (const auto& kv : channels) if (kv.second.has_gaps()) return true;
        return false;
    }
    std::size_t total_missing() const {
        std::size_t n = 0;
        for (const auto& kv : channels) n += kv.second.missing_count();
        return n;
    }
    std::uint64_t total_recovered() const {
        std::uint64_t n = 0;
        for (const auto& kv : channels) n += kv.second.recovered;
        return n;
    }
    std::size_t channel_count() const noexcept { return channels.size(); }
};


// InterArrivalMeter — statystyki odstepow miedzy wiadomosciami (expansion #142).
//
// Sam rate (FeedRateMeter, #132) nie pokazuje JITTERA — feed moze miec srednio
// 1M/s, ale z burstami i dziurami. Ten miernik sledzi min/max/avg gap miedzy
// kolejnymi wiadomosciami: duzy max przy malym min = nierowny feed (ryzyko
// kolejkowania / drop), istotne dla latency-sensitive konsumenta.
struct InterArrivalMeter {
    std::int64_t  last_ns = 0;
    bool          started = false;
    std::int64_t  min_gap = INT64_MAX;
    std::int64_t  max_gap = 0;
    std::int64_t  sum_gap = 0;
    std::uint64_t gaps    = 0;

    void on_message(std::int64_t now_ns) noexcept {
        if (started) {
            const std::int64_t g = now_ns - last_ns;
            if (g < min_gap) min_gap = g;
            if (g > max_gap) max_gap = g;
            sum_gap += g;
            ++gaps;
        }
        last_ns = now_ns;
        started = true;
    }

    double       avg_gap_ns() const noexcept { return gaps ? static_cast<double>(sum_gap) / gaps : 0.0; }
    std::int64_t min_gap_ns() const noexcept { return gaps ? min_gap : 0; }
    std::int64_t max_gap_ns() const noexcept { return max_gap; }
    std::int64_t jitter_ns()  const noexcept { return gaps ? (max_gap - min_gap) : 0; }  // span
    void reset() noexcept { *this = InterArrivalMeter{}; }
};


// DedupWindow — deduplikacja sekwencji (expansion #171).
//
// UDP potrafi DUPLIKOWAC pakiety (retransmisje, A/B line, multipath). Konsument
// musi przetworzyc kazdy seq DOKLADNIE RAZ (at-most-once). DedupWindow pamieta
// niedawno widziane numery w przesuwnym oknie i odrzuca powtorki. Rozni sie od
// ReorderBuffer (kolejnosc) i GapRecovery (luki) — tu chodzi o duplikaty.
//
//   accept(seq) -> true gdy NOWY (przekaz dalej), false gdy duplikat.
struct DedupWindow {
    std::uint64_t window;
    std::uint64_t high = 0;
    bool          init = false;
    std::set<std::uint64_t> seen;
    std::uint64_t duplicates = 0;

    explicit DedupWindow(std::uint64_t window_ = 1024) noexcept
        : window(window_ ? window_ : 1) {}

    bool accept(std::uint64_t seq) {
        if (!init) { init = true; high = seq; seen.insert(seq); return true; }
        if (seq + window <= high) { ++duplicates; return false; }   // poza oknem -> traktuj jak dup
        if (seen.count(seq))      { ++duplicates; return false; }
        seen.insert(seq);
        if (seq > high) high = seq;
        // Prune: zapomnij numery ktore wypadly z okna (ogranicza pamiec).
        while (!seen.empty() && *seen.begin() + window <= high) seen.erase(seen.begin());
        return true;
    }
    void reset() noexcept { *this = DedupWindow{window}; }
};


// BackpressureMonitor — czy konsument nadaza za feedem (expansion #179).
//
// Feed multicast leci ze stala szybkoscia rynku; jesli book-builder / konsument
// nie nadaza, kolejka rosnie i handlujesz na NIEAKTUALNYCH danych. Monitor liczy
// zakolejkowane vs przetworzone, sledzi glebokosc i jej szczyt, i flaguje
// przeciazenie po progu. Lekki: dwa liczniki.
//
//   on_enqueue() przy odbiorze pakietu, on_dequeue() po przetworzeniu;
//   depth() = zaleglosc, overloaded(threshold) -> pora zrzucic/zsnapshotowac.
struct BackpressureMonitor {
    std::uint64_t enqueued = 0;
    std::uint64_t dequeued = 0;
    std::uint64_t peak_depth = 0;

    void on_enqueue(std::uint64_t n = 1) noexcept {
        enqueued += n;
        const std::uint64_t d = depth();
        if (d > peak_depth) peak_depth = d;
    }
    void on_dequeue(std::uint64_t n = 1) noexcept {
        dequeued += (n > depth() ? depth() : n);   // glebokosc nie schodzi ponizej 0
    }
    std::uint64_t depth() const noexcept { return enqueued - dequeued; }
    bool overloaded(std::uint64_t threshold) const noexcept { return depth() >= threshold; }
    void reset() noexcept { enqueued = dequeued = peak_depth = 0; }
};


// LossRateMeter — agregatowa stopa utraty pakietow (expansion #187).
//
// GapRecovery sledzi KONKRETNE brakujace zakresy (do retransmisji); LossRateMeter
// daje jedna liczbe SLA: ile % oczekiwanych pakietow nie dotarlo w calej sesji.
// Oczekiwane = rozpietosc sekwencji (highest - first + 1); odebrane = licznik.
// Zaklada brak duplikatow (najpierw DedupWindow); duplikaty zanizalyby strate.
//
//   on_packet(seq) na kazdy odebrany; loss_rate() do dashboardu/alertu.
struct LossRateMeter {
    std::uint64_t first = 0;
    std::uint64_t highest = 0;
    std::uint64_t received = 0;
    bool          init = false;

    void on_packet(std::uint64_t seq) noexcept {
        if (!init) { first = highest = seq; init = true; }
        else if (seq > highest) highest = seq;
        ++received;
    }
    std::uint64_t expected() const noexcept { return init ? (highest - first + 1) : 0; }
    std::uint64_t lost() const noexcept {
        const std::uint64_t e = expected();
        return e > received ? e - received : 0;
    }
    double loss_rate() const noexcept {
        const std::uint64_t e = expected();
        return e ? static_cast<double>(lost()) / static_cast<double>(e) : 0.0;
    }
    void reset() noexcept { first = highest = received = 0; init = false; }
};


// OutOfOrderMeter — odsetek pakietow poza kolejnoscia (expansion #195).
//
// Mierzy ile pakietow przyszlo Z NIZSZYM seq niz dotychczasowy max (czyli PO
// pakiecie o wyzszym numerze) — objaw reorderingu na sciezce (multipath, ECMP,
// kolejkowanie). Rozni sie od ReorderBuffer (naprawia kolejnosc) i LossRateMeter
// (gubienie): to czysta DIAGNOSTYKA jakosci sieci. Wysoki ooo_rate sugeruje
// problem z routingiem feedu, nie ze zrodlem.
struct OutOfOrderMeter {
    std::uint64_t highest = 0;
    std::uint64_t total = 0;
    std::uint64_t out_of_order = 0;
    bool          init = false;

    void on_packet(std::uint64_t seq) noexcept {
        ++total;
        if (!init) { highest = seq; init = true; return; }
        if (seq < highest) ++out_of_order;   // przyszedl po wyzszym numerze
        else               highest = seq;
    }
    double ooo_rate() const noexcept {
        return total ? static_cast<double>(out_of_order) / static_cast<double>(total) : 0.0;
    }
    void reset() noexcept { highest = total = out_of_order = 0; init = false; }
};


// FeedStalenessMonitor — wykrywa MARTWY feed (expansion #98).
//
// Giełdy wysyłają heartbeaty gdy brak danych właśnie po to, by odbiorca odróżnił
// "spokojny rynek" od "linia padła" (NAT/firewall ucina idle UDP, switch pada).
// Brak JAKIEGOKOLWIEK pakietu (dane LUB heartbeat) przez > timeout = stale →
// feed handler powinien przełączyć na linię zapasową / re-subscribe.
//
//   on_packet(now_ns)         — wołaj na każdy odebrany pakiet (reset zegara)
//   check(now_ns, timeout_ns) — true gdy feed stale; liczy zdarzenia (zbocze)
struct FeedStalenessMonitor {
    int64_t  last_ns      = 0;
    bool     seen         = false;   // czy widzieliśmy pierwszy pakiet
    bool     stale_       = false;
    uint64_t stale_events = 0;       // ile razy feed wpadł w stan stale (zbocza)

    void on_packet(int64_t now_ns) noexcept {
        last_ns = now_ns;
        seen    = true;
        stale_  = false;             // świeży pakiet ożywia feed
    }

    bool check(int64_t now_ns, int64_t timeout_ns) noexcept {
        if (!seen) return false;     // jeszcze nie wystartował — nie "stale"
        const bool now_stale = (now_ns - last_ns) > timeout_ns;
        if (now_stale && !stale_) ++stale_events;   // wejście w stan stale
        stale_ = now_stale;
        return stale_;
    }

    bool is_stale() const noexcept { return stale_; }
    void reset()    noexcept { *this = FeedStalenessMonitor{}; }
};

// ReorderBuffer — bufor zmiany kolejnosci pakietow (expansion #110).
//
// GapRecovery WYKRYWA i godzi luki, ale nie ODTWARZA KOLEJNOSCI danych — pakiet,
// ktory przyszedl za wczesnie (seq > expected), trzeba przytrzymac az dotra
// brakujace przed nim, inaczej konsument (np. rekonstruktor ksiegi) zobaczyl by
// zdarzenia nie po kolei. ReorderBuffer buforuje "przyszle" pakiety i dostarcza
// je dopiero gdy luka sie zasklepi — strumien wychodzacy jest ZAWSZE w kolejnosci.
//
//   push(seq, val): seq==expected -> dostarcz + opróznij kolejne ciagle z bufora
//                   seq> expected -> zbuforuj (czeka na brakujace)
//                   seq< expected -> duplikat, odrzuc
// Dostarczone w kolejnosci ladowane do `out` (caller drainuje i czysci).
template <typename T>
struct ReorderBuffer {
    std::uint64_t        expected = 0;
    bool                 initialized = false;
    std::map<std::uint64_t, T> pending;   // seq > expected, czekaja
    std::vector<T>       out;             // dostarczone w kolejnosci
    std::uint64_t        duplicates = 0;

    void push(std::uint64_t seq, const T& val) {
        if (!initialized) { expected = seq; initialized = true; }
        if (seq < expected) { ++duplicates; return; }    // juz dostarczony
        pending[seq] = val;
        // Opróznij wszystkie ciagle od expected.
        auto it = pending.find(expected);
        while (it != pending.end()) {
            out.push_back(it->second);
            pending.erase(it);
            ++expected;
            it = pending.find(expected);
        }
    }

    bool   has_gap()       const noexcept { return !pending.empty(); }
    size_t buffered()      const noexcept { return pending.size(); }
    void   clear_out()     noexcept { out.clear(); }
    void   reset()         { *this = ReorderBuffer{}; }
};

}  // namespace multicast
