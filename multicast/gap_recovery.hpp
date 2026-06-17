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

namespace multicast {

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

}  // namespace multicast
