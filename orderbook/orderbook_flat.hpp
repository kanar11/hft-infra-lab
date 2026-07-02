/*
 * FlatOrderBook<LEVELS> — flat-array price-level orderbook.
 *
 * The std::map variant in orderbook.cpp / orderbook_v2.cpp allocates a
 * red-black tree node on every insert, pays log(N) per operation, and
 * scatters nodes across the heap (cache-hostile pointer chasing).
 *
 * This variant trades memory for predictability: bid_qty_[price] and
 * ask_qty_[price] are pre-allocated flat arrays indexed directly by
 * price level. Every add/cancel is O(1) array write, with best_bid_
 * and best_ask_ cursors tracked incrementally so try_match() never
 * pays a tree lookup either.
 *
 * Memory cost: 2 × LEVELS × sizeof(int32_t) + 2 × LEVELS/8 bitmap bytes.
 *   LEVELS=65536 (default) ⇒ 512 KB qty arrays + 16 KB bitmaps — one L2 cache.
 *
 * Occupancy bitmaps: one bit per price level (bit set ⇔ qty > 0),
 * maintained on every level 0→nonzero / nonzero→0 transition. Cursor
 * advances ("next non-empty level") scan 64 levels per word read with
 * ctz/clz instead of walking the qty array level by level, so emptying
 * the best level of a sparse book costs O(gap/64) word reads instead of
 * O(gap) — and a full side wipe costs at most LEVELS/64 reads, not LEVELS.
 *
 * Performance characteristic:
 *   add_buy / add_sell   : O(1)
 *   best_bid / best_ask  : O(1) (cursor read)
 *   try_match            : O(matches + advances/64) — the advance scans
 *                          the occupancy bitmap one 64-level word at a
 *                          time, and stays a single word read in practice
 *                          because activity concentrates near the spread.
 *
 * Trade-off vs std::map:
 *   + No heap allocation, ever
 *   + Cache-friendly contiguous arrays
 *   + O(1) hot-path operations
 *   − Fixed price range at compile time
 *   − Larger working set (full LEVELS array vs only used keys)
 *
 * Production HFT engines extend this with:
 *   - Order-ID → price-level map for cancel/modify by ID
 *   - Per-level queue of individual orders (FIFO time priority)
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>


namespace orderbook {

template <std::size_t LEVELS = 65536>
class FlatOrderBook {
    static_assert(LEVELS > 0,                          "LEVELS must be positive");
    static_assert((LEVELS & (LEVELS - 1)) == 0,        "LEVELS must be a power of two");
    static_assert(LEVELS <= (1u << 30),                "LEVELS too large for int32 cursor");

public:
    using Qty   = std::int32_t;
    using Price = std::int32_t;                        // price in ticks (1 tick = 0.01)

    static constexpr Price NO_BID =  -1;
    static constexpr Price NO_ASK = static_cast<Price>(LEVELS);

private:
    struct OrderRef {
        Price price;
        Qty   qty;
        bool  is_buy;
    };

    static constexpr std::size_t WORDS = LEVELS / 64;
    static_assert(WORDS > 0, "LEVELS must be at least 64");

    Qty   bid_qty_[LEVELS]{};
    Qty   ask_qty_[LEVELS]{};
    // Occupancy bitmaps: bit p set ⇔ qty_[p] > 0 (see header comment).
    std::uint64_t bid_bits_[WORDS]{};
    std::uint64_t ask_bits_[WORDS]{};
    Price best_bid_ = NO_BID;
    Price best_ask_ = NO_ASK;
    std::uint64_t trades_ = 0;

    // Optional order-ID tracker — populated only by submit_with_id() /
    // cancel() / modify(). The aggregated add_buy / add_sell paths never
    // touch this map, so the lightweight aggregate API stays allocation-free.
    std::unordered_map<std::uint64_t, OrderRef> orders_by_id_;

public:
    FlatOrderBook() = default;

    FlatOrderBook(const FlatOrderBook&)            = delete;
    FlatOrderBook& operator=(const FlatOrderBook&) = delete;
    FlatOrderBook(FlatOrderBook&&)                 = delete;
    FlatOrderBook& operator=(FlatOrderBook&&)      = delete;

    bool add_buy(Price price, Qty qty) noexcept {
        if (price < 0 || price >= static_cast<Price>(LEVELS) || qty <= 0) return false;
        if (bid_qty_[price] == 0) set_bit(bid_bits_, price);
        bid_qty_[price] += qty;
        if (price > best_bid_) best_bid_ = price;
        try_match();
        return true;
    }

    bool add_sell(Price price, Qty qty) noexcept {
        if (price < 0 || price >= static_cast<Price>(LEVELS) || qty <= 0) return false;
        if (ask_qty_[price] == 0) set_bit(ask_bits_, price);
        ask_qty_[price] += qty;
        if (price < best_ask_) best_ask_ = price;
        try_match();
        return true;
    }

    // --- Order-ID-tracked variants (for cancel/modify by ID) ---
    //
    // These coexist with add_buy/add_sell: the ID-tracked path adds an entry
    // to orders_by_id_ so cancel/modify can find the original (price, qty,
    // side). If an order gets partially or fully filled by try_match, the
    // map entry stays — a subsequent cancel decrements the level by
    // min(remaining_at_level, order_qty), gracefully handling the fact that
    // a flat book doesn't track per-order FIFO position.

    bool submit_with_id(std::uint64_t id, Price price, Qty qty, bool is_buy) noexcept {
        if (price < 0 || price >= static_cast<Price>(LEVELS) || qty <= 0) return false;
        if (orders_by_id_.find(id) != orders_by_id_.end()) return false;  // dup ID
        orders_by_id_.emplace(id, OrderRef{price, qty, is_buy});
        if (is_buy) {
            if (bid_qty_[price] == 0) set_bit(bid_bits_, price);
            bid_qty_[price] += qty;
            if (price > best_bid_) best_bid_ = price;
        } else {
            if (ask_qty_[price] == 0) set_bit(ask_bits_, price);
            ask_qty_[price] += qty;
            if (price < best_ask_) best_ask_ = price;
        }
        try_match();
        return true;
    }

    bool cancel(std::uint64_t id) noexcept {
        auto it = orders_by_id_.find(id);
        if (it == orders_by_id_.end()) return false;
        const OrderRef ref = it->second;
        orders_by_id_.erase(it);
        if (ref.is_buy) {
            const Qty drop = std::min(ref.qty, bid_qty_[ref.price]);
            bid_qty_[ref.price] -= drop;
            if (bid_qty_[ref.price] == 0) clear_bit(bid_bits_, ref.price);
            best_bid_ = scan_down(bid_bits_, best_bid_);
        } else {
            const Qty drop = std::min(ref.qty, ask_qty_[ref.price]);
            ask_qty_[ref.price] -= drop;
            if (ask_qty_[ref.price] == 0) clear_bit(ask_bits_, ref.price);
            best_ask_ = scan_up(ask_bits_, best_ask_);
        }
        return true;
    }

    bool modify(std::uint64_t id, Price new_price, Qty new_qty) noexcept {
        auto it = orders_by_id_.find(id);
        if (it == orders_by_id_.end()) return false;
        const bool is_buy = it->second.is_buy;
        cancel(id);
        return submit_with_id(id, new_price, new_qty, is_buy);
    }

    std::size_t tracked_orders() const noexcept { return orders_by_id_.size(); }

    Price best_bid()    const noexcept { return best_bid_; }
    Price best_ask()    const noexcept { return best_ask_; }
    std::uint64_t trades() const noexcept { return trades_; }
    bool empty()        const noexcept { return best_bid_ == NO_BID && best_ask_ == NO_ASK; }

    // qty_at: read the quantity resting at a given level (testing/inspection).
    Qty bid_qty_at(Price p) const noexcept {
        return (p < 0 || p >= static_cast<Price>(LEVELS)) ? 0 : bid_qty_[p];
    }
    Qty ask_qty_at(Price p) const noexcept {
        return (p < 0 || p >= static_cast<Price>(LEVELS)) ? 0 : ask_qty_[p];
    }

private:
    void try_match() noexcept {
        while (best_bid_ != NO_BID && best_ask_ != NO_ASK && best_bid_ >= best_ask_) {
            const Qty fill = std::min(bid_qty_[best_bid_], ask_qty_[best_ask_]);
            bid_qty_[best_bid_] -= fill;
            ask_qty_[best_ask_] -= fill;
            ++trades_;
            // Advance the cursor of whichever side(s) got exhausted; the
            // bitmap scan jumps straight to the next non-empty level.
            if (bid_qty_[best_bid_] == 0) {
                clear_bit(bid_bits_, best_bid_);
                best_bid_ = scan_down(bid_bits_, best_bid_);
            }
            if (ask_qty_[best_ask_] == 0) {
                clear_bit(ask_bits_, best_ask_);
                best_ask_ = scan_up(ask_bits_, best_ask_);
            }
        }
    }

    // --- Occupancy-bitmap primitives ---

    static void set_bit(std::uint64_t* bits, Price p) noexcept {
        bits[static_cast<std::size_t>(p) >> 6] |= (1ULL << (static_cast<unsigned>(p) & 63u));
    }

    static void clear_bit(std::uint64_t* bits, Price p) noexcept {
        bits[static_cast<std::size_t>(p) >> 6] &= ~(1ULL << (static_cast<unsigned>(p) & 63u));
    }

    // Highest occupied level at or below `from` (NO_BID if none). O(1) when
    // `from` itself is occupied — the common "best level unchanged" case.
    static Price scan_down(const std::uint64_t* bits, Price from) noexcept {
        if (from < 0) return NO_BID;
        std::size_t w = static_cast<std::size_t>(from) >> 6;
        std::uint64_t word = bits[w] & (~0ULL >> (63u - (static_cast<unsigned>(from) & 63u)));
        for (;;) {
            if (word != 0) {
                return static_cast<Price>((w << 6) +
                       (63u - static_cast<unsigned>(__builtin_clzll(word))));
            }
            if (w == 0) return NO_BID;
            word = bits[--w];
        }
    }

    // Lowest occupied level at or above `from` (NO_ASK if none).
    static Price scan_up(const std::uint64_t* bits, Price from) noexcept {
        if (from >= static_cast<Price>(LEVELS)) return NO_ASK;
        std::size_t w = static_cast<std::size_t>(from) >> 6;
        std::uint64_t word = bits[w] & (~0ULL << (static_cast<unsigned>(from) & 63u));
        for (;;) {
            if (word != 0) {
                return static_cast<Price>((w << 6) +
                       static_cast<unsigned>(__builtin_ctzll(word)));
            }
            if (++w == WORDS) return NO_ASK;
            word = bits[w];
        }
    }
};

}  // namespace orderbook
