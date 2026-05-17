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
 * Memory cost: 2 × LEVELS × sizeof(int32_t).
 *   LEVELS=65536 (default) ⇒ 512 KB per book — one L2 cache.
 *
 * Performance characteristic:
 *   add_buy / add_sell   : O(1)
 *   best_bid / best_ask  : O(1) (cursor read)
 *   try_match            : O(matches + advances) — the advance can
 *                          scan past empty levels but stays local in
 *                          practice because activity concentrates near
 *                          the spread.
 *
 * Trade-off vs std::map:
 *   + No heap allocation, ever
 *   + Cache-friendly contiguous arrays
 *   + O(1) hot-path operations
 *   − Fixed price range at compile time
 *   − Larger working set (full LEVELS array vs only used keys)
 *
 * Production HFT engines extend this with:
 *   - A bitmap of non-empty levels for O(1) "next non-empty" scan
 *   - Order-ID → price-level map for cancel/modify by ID
 *   - Per-level queue of individual orders (FIFO time priority)
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>


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
    Qty   bid_qty_[LEVELS]{};
    Qty   ask_qty_[LEVELS]{};
    Price best_bid_ = NO_BID;
    Price best_ask_ = NO_ASK;
    std::uint64_t trades_ = 0;

public:
    FlatOrderBook() = default;

    FlatOrderBook(const FlatOrderBook&)            = delete;
    FlatOrderBook& operator=(const FlatOrderBook&) = delete;
    FlatOrderBook(FlatOrderBook&&)                 = delete;
    FlatOrderBook& operator=(FlatOrderBook&&)      = delete;

    bool add_buy(Price price, Qty qty) noexcept {
        if (price < 0 || price >= static_cast<Price>(LEVELS) || qty <= 0) return false;
        bid_qty_[price] += qty;
        if (price > best_bid_) best_bid_ = price;
        try_match();
        return true;
    }

    bool add_sell(Price price, Qty qty) noexcept {
        if (price < 0 || price >= static_cast<Price>(LEVELS) || qty <= 0) return false;
        ask_qty_[price] += qty;
        if (price < best_ask_) best_ask_ = price;
        try_match();
        return true;
    }

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
            // Advance bid cursor down past exhausted levels.
            while (best_bid_ >= 0 && bid_qty_[best_bid_] == 0) --best_bid_;
            // Advance ask cursor up past exhausted levels.
            while (best_ask_ < static_cast<Price>(LEVELS) && ask_qty_[best_ask_] == 0) ++best_ask_;
        }
    }
};

}  // namespace orderbook
