/*
 * orderbook_pro_cluster.hpp — BookCluster<N>: wielo-symbolowy kontener.
 *
 * Wydzielone z orderbook_pro.hpp. Wrapper na N niezależnych FullOrderBook'ów
 * z O(1) lookup po nazwie symbolu, cross-symbol arbitrage detection,
 * agregacjami klastra i własnym snapshotem ("OBCL"). Osobny moduł — zależy
 * od silnika (FullOrderBook), ale silnik nie zależy od niego.
 */
#pragma once

#include "orderbook_pro.hpp"       // FullOrderBook
#include <cstdint>
#include <cstring>

namespace orderbook_pro {

// ====================================================================
// BookCluster<N_SYMBOLS, ...> — wielo-symbolowa książka
// ====================================================================
//
// Wrapper na N osobnych FullOrderBook'ów z O(1) lookup po nazwie symbolu.
// Symbole są kodowane jako 8-char ASCII pakowane do uint64 (sym_to_key
// idea: każdy znak w 8 bitach low-byte first).
//
// Po co? Realne venues handlują tysiące symboli — separate book per symbol
// daje pełną izolację (one symbol's halt nie blokuje innych) + cache locality
// (każdy book ma własne levels[], orders[]).
//
// Konfiguracja: N_SYMBOLS = max liczba różnych symboli. LEVELS / MAX_ORDERS_PER_SYM
// per-book.
//
// Cross-symbol features:
//   - total_volume_across_all() — Σ wolumen wszystkich symboli
//   - avg_spread_ticks() — średni spread w klastrze (proxy dla market quality)
//   - busiest_symbol() — symbol z największą ilością aktywnych zleceń
template <std::size_t N_SYMBOLS = 16,
          std::int32_t LEVELS = 16384,
          std::int32_t MAX_ORDERS_PER_SYM = 8192>
class BookCluster {
    using BookT = FullOrderBook<LEVELS, MAX_ORDERS_PER_SYM>;

    BookT  books_[N_SYMBOLS];
    char   symbols_[N_SYMBOLS][9];    // 8 chars + null
    bool   slot_used_[N_SYMBOLS]      = {};
    std::size_t active_count_         = 0;

    // sym → uint64 packing (8 chars LSB first)
    static std::uint64_t pack(const char* sym) noexcept {
        // Najpierw zmierz długość przez memchr (cppcheck rozumie ten wzorzec
        // i nie zgłasza arrayIndexOutOfBoundsCond na short-circuit z `sym[i]`).
        std::uint64_t k = 0;
        const void* nul = std::memchr(sym, '\0', 8);
        const std::size_t n = nul
            ? static_cast<std::size_t>(static_cast<const char*>(nul) - sym)
            : 8;
        for (std::size_t i = 0; i < n; ++i) {
            k |= static_cast<std::uint64_t>(static_cast<unsigned char>(sym[i])) << (i * 8);
        }
        return k;
    }

    // Liniowy search po slotach. N_SYMBOLS ≤ 16 — szybsze niż unordered_map
    // (1-2 cache lines mieści się w cache L1, branch predictor radzi sobie).
    std::int32_t find_slot(const char* sym) const noexcept {
        const std::uint64_t key = pack(sym);
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            if (pack(symbols_[i]) == key) return static_cast<std::int32_t>(i);
        }
        return -1;
    }

public:
    BookCluster() noexcept {
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) symbols_[i][0] = '\0';
    }

    BookCluster(const BookCluster&)            = delete;
    BookCluster& operator=(const BookCluster&) = delete;
    BookCluster(BookCluster&&)                 = delete;
    BookCluster& operator=(BookCluster&&)      = delete;

    // Zarejestruj nowy symbol. Zwraca true on success, false gdy slot
    // wyczerpany albo symbol już istnieje.
    bool register_symbol(const char* sym) noexcept {
        if (find_slot(sym) >= 0) return false;  // already registered
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) {
                slot_used_[i] = true;
                // memcpy zamiast strncpy — gcc -Wstringop-truncation strzela
                // gdy src może mieć 8 znaków bez NUL (load_snapshot podaje
                // taki bufor). Długość przez memchr, ogon zerowany.
                std::memset(symbols_[i], 0, 9);
                const void* nul = std::memchr(sym, '\0', 8);
                const std::size_t n = nul
                    ? static_cast<std::size_t>(
                          static_cast<const char*>(nul) - sym)
                    : 8;
                std::memcpy(symbols_[i], sym, n);
                ++active_count_;
                return true;
            }
        }
        return false;
    }

    BookT* book(const char* sym) noexcept {
        const std::int32_t slot = find_slot(sym);
        return slot >= 0 ? &books_[slot] : nullptr;
    }
    const BookT* book(const char* sym) const noexcept {
        const std::int32_t slot = find_slot(sym);
        return slot >= 0 ? &books_[slot] : nullptr;
    }

    std::size_t active_symbol_count() const noexcept { return active_count_; }
    std::size_t capacity_symbols()    const noexcept { return N_SYMBOLS; }

    // Cross-symbol aggregations
    std::uint64_t total_volume_across_all() const noexcept {
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) total += books_[i].stats().total_volume;
        }
        return total;
    }

    // avg_spread_ticks: średni spread po symbolach które mają obie strony quote.
    // Zwraca -1 gdy żaden symbol nie ma TOB.
    std::int32_t avg_spread_ticks() const noexcept {
        std::int64_t sum = 0;
        std::int32_t cnt = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            const std::int32_t s = books_[i].spread_ticks();
            if (s >= 0) { sum += s; ++cnt; }
        }
        return cnt > 0 ? static_cast<std::int32_t>(sum / cnt) : -1;
    }

    // busiest_symbol: symbol z największym active_orders(). Zwraca nullptr gdy puste.
    const char* busiest_symbol() const noexcept {
        std::size_t best = N_SYMBOLS;
        std::size_t best_count = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            const std::size_t c = books_[i].active_orders();
            if (c > best_count) { best_count = c; best = i; }
        }
        return best < N_SYMBOLS ? symbols_[best] : nullptr;
    }

    // total_active_orders_across_all
    std::size_t total_active_orders() const noexcept {
        std::size_t t = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) t += books_[i].active_orders();
        }
        return t;
    }

    // Cluster-wide cumulative fills (Σ stats_.total_fills per symbol).
    std::uint64_t cluster_total_fills() const noexcept {
        std::uint64_t t = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) t += books_[i].stats().total_fills;
        }
        return t;
    }
    std::uint64_t cluster_total_volume() const noexcept {
        std::uint64_t t = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) t += books_[i].stats().total_volume;
        }
        return t;
    }
    std::uint64_t cluster_total_orders_added() const noexcept {
        std::uint64_t t = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) t += books_[i].stats().total_orders_added;
        }
        return t;
    }
    // Volume-weighted average spread across cluster (ważone total_volume per symbol).
    // Bardziej realistyczny niż simple avg — symbole z większym flow większy weight.
    double volume_weighted_avg_spread_ticks() const noexcept {
        std::int64_t weighted_spread = 0;
        std::uint64_t total_volume = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            const std::int32_t s = books_[i].spread_ticks();
            if (s < 0) continue;
            const std::uint64_t v = books_[i].stats().total_volume;
            if (v == 0) continue;
            weighted_spread += static_cast<std::int64_t>(s) * v;
            total_volume += v;
        }
        if (total_volume == 0) return 0.0;
        return static_cast<double>(weighted_spread) /
               static_cast<double>(total_volume);
    }

    // Cluster-wide cumulative flow imbalance ważony po volumie per symbol.
    // Σ buy_vol / Σ (buy + sell) vol × 10000 (bps).
    std::int32_t cluster_flow_imbalance_bps() const noexcept {
        std::int64_t buy = 0, sell = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            buy  += static_cast<std::int64_t>(books_[i].taker_buy_volume());
            sell += static_cast<std::int64_t>(books_[i].taker_sell_volume());
        }
        const std::int64_t total = buy + sell;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((buy - sell) * 10000 / total);
    }

    // ====================================================================
    // Cluster snapshot — multi-symbol recovery
    // ====================================================================
    //
    // Wire format: 4 B magic "OBCL" + 4 B version + 8 B count, potem per
    // symbol: 9 B nazwa + 8 B długość snapshotu księgi + bytes (format
    // księgi v2, włącznie z pending stopami i pegami).
    static constexpr std::uint32_t CLUSTER_SNAPSHOT_MAGIC   = 0x4C43424FU;  // "OBCL"
    static constexpr std::uint32_t CLUSTER_SNAPSHOT_VERSION = 1;

    std::size_t snapshot_size_estimate() const noexcept {
        std::size_t total = 4 + 4 + 8;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i])
                total += 9 + 8 + books_[i].snapshot_size_estimate();
        }
        return total;
    }

    std::size_t serialize_snapshot(std::uint8_t* buf, std::size_t cap) const noexcept {
        if (cap < snapshot_size_estimate()) return 0;
        std::size_t off = 0;
        const std::uint32_t magic   = CLUSTER_SNAPSHOT_MAGIC;
        const std::uint32_t version = CLUSTER_SNAPSHOT_VERSION;
        const std::uint64_t count   = active_count_;
        std::memcpy(buf + off, &magic,   4); off += 4;
        std::memcpy(buf + off, &version, 4); off += 4;
        std::memcpy(buf + off, &count,   8); off += 8;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            std::memcpy(buf + off, symbols_[i], 9); off += 9;
            // Długość przed snapshotem — serializuj za polem, wpisz potem
            const std::size_t blen = books_[i].serialize_snapshot(
                buf + off + 8, cap - off - 8);
            if (blen == 0) return 0;   // pusta księga pisze 16 B, 0 = błąd
            const std::uint64_t blen64 = blen;
            std::memcpy(buf + off, &blen64, 8);
            off += 8 + blen;
        }
        return off;
    }

    bool load_snapshot(const std::uint8_t* buf, std::size_t len) noexcept {
        if (len < 16) return false;
        std::uint32_t magic, version;
        std::uint64_t count;
        std::memcpy(&magic,   buf,     4);
        std::memcpy(&version, buf + 4, 4);
        std::memcpy(&count,   buf + 8, 8);
        if (magic != CLUSTER_SNAPSHOT_MAGIC)     return false;
        if (version != CLUSTER_SNAPSHOT_VERSION) return false;
        if (count > N_SYMBOLS)                   return false;
        // Pełny reset klastra — książki używanych slotów też
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) books_[i].clear();
            slot_used_[i] = false;
        }
        active_count_ = 0;
        std::size_t off = 16;
        for (std::uint64_t k = 0; k < count; ++k) {
            if (off + 9 + 8 > len) return false;
            char sym[9];
            std::memcpy(sym, buf + off, 9); off += 9;
            sym[8] = '\0';
            std::uint64_t blen;
            std::memcpy(&blen, buf + off, 8); off += 8;
            if (off + blen > len) return false;
            if (!register_symbol(sym)) return false;
            BookT* bk = book(sym);
            if (!bk || !bk->load_snapshot(buf + off,
                                           static_cast<std::size_t>(blen)))
                return false;
            off += blen;
        }
        return true;
    }

    // Symbol z najwyższym book-level flow_imbalance (informational flow magnet).
    const char* most_imbalanced_symbol() const noexcept {
        std::size_t best = N_SYMBOLS;
        std::int32_t best_abs = -1;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            const std::int32_t imb = std::abs(books_[i].flow_imbalance_bps());
            if (imb > best_abs) { best_abs = imb; best = i; }
        }
        return best < N_SYMBOLS ? symbols_[best] : nullptr;
    }

    // ====================================================================
    // Cross-symbol arbitrage detection
    // ====================================================================
    //
    // Skanuje pary symboli w klastrze i raportuje sytuacje gdzie:
    //   bid_X >= ask_Y  (możemy kupić na Y, sprzedać na X z profitem)
    //
    // Use case: arbitrage detection przy listing tej samej akcji na wielu
    // venues (NASDAQ vs NYSE) lub ETF arb (SPY vs IVV vs VOO). W praktyce
    // realny arb wymaga uwzględnienia opłat + slippage, ale ta funkcja daje
    // RAW signal.
    struct ArbOpportunity {
        char         long_symbol[9];   // gdzie ASK (kupimy)
        char         short_symbol[9];  // gdzie BID (sprzedamy)
        std::int32_t buy_price_ticks;  // ask na long_symbol
        std::int32_t sell_price_ticks; // bid na short_symbol
        std::int32_t spread_ticks;     // sell - buy (zysk pre-fees)
        std::int32_t max_qty;          // min(ask_qty_long, bid_qty_short)
    };

    // detect_cross_arb: wypełnia `out[]` (max max_n) lukami arb. Zwraca ile.
    std::size_t detect_cross_arb(ArbOpportunity* out, std::size_t max_n) const noexcept {
        std::size_t found = 0;
        for (std::size_t i = 0; i < N_SYMBOLS && found < max_n; ++i) {
            if (!slot_used_[i]) continue;
            if (!books_[i].has_bid()) continue;
            for (std::size_t j = 0; j < N_SYMBOLS && found < max_n; ++j) {
                if (i == j || !slot_used_[j]) continue;
                if (!books_[j].has_ask()) continue;
                const std::int32_t bid_i = books_[i].best_bid_ticks();
                const std::int32_t ask_j = books_[j].best_ask_ticks();
                if (bid_i > ask_j) {
                    // Arb: kupić na j @ ask_j, sprzedać na i @ bid_i
                    ArbOpportunity& o = out[found++];
                    // symbols_ jest zero-padded do 9 B (register_symbol) —
                    // memcpy 9 kopiuje też NUL, bez strncpy-truncation warninga
                    std::memcpy(o.long_symbol,  symbols_[j], 9);
                    std::memcpy(o.short_symbol, symbols_[i], 9);
                    o.buy_price_ticks  = ask_j;
                    o.sell_price_ticks = bid_i;
                    o.spread_ticks     = bid_i - ask_j;
                    const auto tob_i = books_[i].top_of_book();
                    const auto tob_j = books_[j].top_of_book();
                    o.max_qty = std::min(tob_j.ask_qty, tob_i.bid_qty);
                }
            }
        }
        return found;
    }

    // count_cross_arb: ile arb opportunities. Bez kopiowania do bufora.
    std::size_t count_cross_arb() const noexcept {
        std::size_t cnt = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i] || !books_[i].has_bid()) continue;
            for (std::size_t j = 0; j < N_SYMBOLS; ++j) {
                if (i == j || !slot_used_[j] || !books_[j].has_ask()) continue;
                if (books_[i].best_bid_ticks() > books_[j].best_ask_ticks()) ++cnt;
            }
        }
        return cnt;
    }
};



}  // namespace orderbook_pro
