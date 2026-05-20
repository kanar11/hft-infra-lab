/*
 * MarketMaker — symmetric two-sided quoter with inventory skew.
 *
 * The mean-reversion strategy in mean_reversion.hpp is *reactive* — it
 * waits for prices to deviate from a moving average and submits a single
 * order. A market maker is *proactive*: it continuously quotes both
 * sides of the book and earns the spread × volume that crosses its
 * quotes, while managing the resulting inventory.
 *
 * Per-tick algorithm
 * ------------------
 *   mid  = (best_bid + best_ask) / 2
 *   skew = -position * risk_aversion_ticks      [in ticks]
 *
 *     long inventory  → negative skew → quotes shift DOWN
 *       (encourages crosses on the maker's ASK, discourages BIDs that
 *        would push the position further long)
 *     short inventory → positive skew → quotes shift UP
 *
 *   target_bid = mid - half_spread + skew
 *   target_ask = mid + half_spread + skew
 *
 * Inventory limits
 * ----------------
 * If position >=  max_inventory, suppress the BID side (don't grow long).
 * If position <= -max_inventory, suppress the ASK side (don't grow short).
 *
 * P&L model
 * ---------
 * cash_ accumulates signed ticks × shares of every fill. mark-to-market
 * P&L = (cash_ + position * mid) / 100  dollars.
 *
 * This class is integration-agnostic — it exposes pure quote() / apply_fill()
 * functions. The caller (mm_demo or a future OMS-integrated runner) is
 * responsible for actually submitting/cancelling orders via OMS and feeding
 * fills back in.
 */
#pragma once

#include "../common/types.hpp"

#include <cstdint>
#include <cstring>


namespace mm {

struct MMConfig {
    int32_t quote_size          = 10;     // shares per side
    int32_t half_spread_ticks   = 2;      // distance from mid (1 tick = $0.01)
    int32_t max_inventory       = 1000;   // hard cap on net position
    double  risk_aversion_ticks = 0.05;   // skew (ticks) per share of net inventory
};


struct Quote {
    int32_t bid_price;   // 0 = side suppressed (e.g. at max inventory)
    int32_t ask_price;
    int32_t bid_size;
    int32_t ask_size;
};


class MarketMaker {
    MMConfig cfg_;
    char     symbol_[9];

    int32_t  position_     = 0;
    int64_t  cash_ticks_   = 0;   // signed cumulative cash flow, in ticks*shares
    int32_t  last_bid_     = 0;
    int32_t  last_ask_     = 0;

    std::uint64_t quotes_placed_    = 0;
    std::uint64_t quotes_cancelled_ = 0;
    std::uint64_t fills_received_   = 0;

public:
    MarketMaker(const MMConfig& cfg, const char* symbol) noexcept : cfg_(cfg) {
        std::strncpy(symbol_, symbol, 8);
        symbol_[8] = '\0';
    }

    MarketMaker(const MarketMaker&)            = delete;
    MarketMaker& operator=(const MarketMaker&) = delete;
    MarketMaker(MarketMaker&&)                 = delete;
    MarketMaker& operator=(MarketMaker&&)      = delete;

    // quote: compute target quotes for this tick given the prevailing
    // best bid / best ask in the market. Both in ticks.
    Quote quote(int32_t best_bid, int32_t best_ask) noexcept {
        Quote q{0, 0, 0, 0};
        if (best_bid <= 0 || best_ask <= 0 || best_ask <= best_bid) return q;

        const int32_t mid  = (best_bid + best_ask) / 2;
        const int32_t skew = static_cast<int32_t>(-static_cast<double>(position_)
                                                    * cfg_.risk_aversion_ticks);

        const int32_t bid_target = mid - cfg_.half_spread_ticks + skew;
        const int32_t ask_target = mid + cfg_.half_spread_ticks + skew;

        // Bid suppressed when at long limit; ask suppressed when at short limit.
        if (position_ < cfg_.max_inventory) {
            q.bid_price = bid_target;
            q.bid_size  = cfg_.quote_size;
        }
        if (position_ > -cfg_.max_inventory) {
            q.ask_price = ask_target;
            q.ask_size  = cfg_.quote_size;
        }

        // Track quote churn: any change to either side counts as cancel+replace.
        if (q.bid_price != last_bid_ || q.ask_price != last_ask_) {
            if (last_bid_ != 0 || last_ask_ != 0) ++quotes_cancelled_;
            ++quotes_placed_;
        }
        last_bid_ = q.bid_price;
        last_ask_ = q.ask_price;
        return q;
    }

    // apply_fill: caller tells us one of our quotes got hit.
    //   side == BUY  → maker bought  (inventory ↑, cash ↓)
    //   side == SELL → maker sold    (inventory ↓, cash ↑)
    void apply_fill(Side side, int32_t qty, int32_t price_ticks) noexcept {
        if (qty <= 0) return;
        ++fills_received_;
        if (side == Side::BUY) {
            position_   += qty;
            cash_ticks_ -= static_cast<int64_t>(qty) * price_ticks;
        } else {
            position_   -= qty;
            cash_ticks_ += static_cast<int64_t>(qty) * price_ticks;
        }
    }

    // pnl: mark-to-market in dollars at the given mid price.
    double pnl(int32_t mid_ticks) const noexcept {
        const int64_t inv_value_ticks = static_cast<int64_t>(position_) * mid_ticks;
        return static_cast<double>(cash_ticks_ + inv_value_ticks) / 100.0;
    }

    int32_t       position()          const noexcept { return position_; }
    std::uint64_t quotes_placed()     const noexcept { return quotes_placed_; }
    std::uint64_t quotes_cancelled()  const noexcept { return quotes_cancelled_; }
    std::uint64_t fills_received()    const noexcept { return fills_received_; }
    const char*   symbol()            const noexcept { return symbol_; }
};

}  // namespace mm
