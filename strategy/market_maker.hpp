/*
 * MarketMaker — a symmetric two-sided quoter with inventory skew.
 *
 * Mean reversion (mean_reversion.hpp) is REACTIVE — it waits until price
 * deviates from the average and submits 1 order. A market maker is PROACTIVE:
 * it quotes both sides of the book all the time, earns the spread × the volume that
 * crosses its quotes, and at the same time manages the resulting inventory.
 *
 * Algorithm per tick:
 *   mid  = (best_bid + best_ask) / 2
 *   skew = -position * risk_aversion_ticks         [in ticks]
 *
 *     long inventory  → negative skew → quotes DOWN
 *       (encourages execution on the maker's ASK, discourages BIDs that
 *        would extend the long even further)
 *     short inventory → positive skew → quotes UP
 *
 *   target_bid = mid - half_spread + skew
 *   target_ask = mid + half_spread + skew
 *
 * Inventory limits:
 *   position >=  max_inventory → BID disabled (we do not grow the long)
 *   position <= -max_inventory → ASK disabled (we do not grow the short)
 *
 * P&L model:
 *   cash_ accumulates signed ticks × shares of each fill. Mark-to-market
 *   P&L = (cash_ + position * mid) / 100 dollars.
 *
 * The class is integration-agnostic — it exposes clean quote() / apply_fill().
 * The caller (mm_demo or a future OMS runner) is responsible for the
 * actual submit/cancel of orders through the OMS and for feeding fills back.
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
    int32_t bid_price;   // 0 = side disabled (e.g. at max_inventory)
    int32_t ask_price;
    int32_t bid_size;
    int32_t ask_size;
};


class MarketMaker {
    MMConfig cfg_;
    char     symbol_[9];

    int32_t  position_     = 0;
    int64_t  cash_ticks_   = 0;   // signed cumulative cash flow in ticks*shares
    int32_t  last_bid_     = 0;
    int32_t  last_ask_     = 0;

    std::uint64_t quotes_placed_    = 0;
    std::uint64_t quotes_cancelled_ = 0;
    std::uint64_t fills_received_   = 0;

    // Adverse selection tracking — see mark_post_fill_move().
    Side          last_fill_side_       = Side::BUY;
    int32_t       last_fill_price_ticks_ = 0;  // 0 = no "pending" fill
    int64_t       adverse_total_ticks_   = 0;  // signed cumulative

public:
    MarketMaker(const MMConfig& cfg, const char* symbol) noexcept : cfg_(cfg) {
        std::strncpy(symbol_, symbol, 8);
        symbol_[8] = '\0';
    }

    MarketMaker(const MarketMaker&)            = delete;
    MarketMaker& operator=(const MarketMaker&) = delete;
    MarketMaker(MarketMaker&&)                 = delete;
    MarketMaker& operator=(MarketMaker&&)      = delete;

    // quote: compute the target quotes for this tick given the market's best_bid/best_ask.
    // Both in ticks (1 tick = $0.01).
    Quote quote(int32_t best_bid, int32_t best_ask) noexcept {
        Quote q{0, 0, 0, 0};
        if (best_bid <= 0 || best_ask <= 0 || best_ask <= best_bid) return q;

        const int32_t mid  = (best_bid + best_ask) / 2;
        const int32_t skew = static_cast<int32_t>(-static_cast<double>(position_)
                                                    * cfg_.risk_aversion_ticks);

        const int32_t bid_target = mid - cfg_.half_spread_ticks + skew;
        const int32_t ask_target = mid + cfg_.half_spread_ticks + skew;

        // BID disabled when at the long limit; ASK disabled when at the short limit.
        if (position_ < cfg_.max_inventory) {
            q.bid_price = bid_target;
            q.bid_size  = cfg_.quote_size;
        }
        if (position_ > -cfg_.max_inventory) {
            q.ask_price = ask_target;
            q.ask_size  = cfg_.quote_size;
        }

        // Quote churn: any change of either side counts as a cancel+replace.
        if (q.bid_price != last_bid_ || q.ask_price != last_ask_) {
            if (last_bid_ != 0 || last_ask_ != 0) ++quotes_cancelled_;
            ++quotes_placed_;
        }
        last_bid_ = q.bid_price;
        last_ask_ = q.ask_price;
        return q;
    }

    // apply_fill: the caller tells us that one of our quotes was executed.
    //   side == BUY  → the maker bought (inventory ↑, cash ↓)
    //   side == SELL → the maker sold   (inventory ↓, cash ↑)
    //
    // After execution the caller SHOULD later call mark_post_fill_move()
    // when the next mid is known — this lets us track adverse selection
    // (informed traders hit the MM just before price moves their way).
    void apply_fill(Side side, int32_t qty, int32_t price_ticks) noexcept {
        if (qty <= 0) return;
        ++fills_received_;
        last_fill_side_       = side;
        last_fill_price_ticks_ = price_ticks;
        if (side == Side::BUY) {
            position_   += qty;
            cash_ticks_ -= static_cast<int64_t>(qty) * price_ticks;
        } else {
            position_   -= qty;
            cash_ticks_ += static_cast<int64_t>(qty) * price_ticks;
        }
    }

    // mark_post_fill_move: a model of **adverse selection**. In reality when an MM
    // gets a fill, most counterparties are *informed traders* (they have
    // better info than the MM) → price MORE OFTEN moves further their way right after
    // execution. The classic MM problem "when I get hit it's more often bad for
    // me".
    //
    // The caller passes the current mid after a fill. We compute the "adverse move":
    //   - if the MM bought (BUY) and price fell → adverse (mid < fill_price)
    //   - if the MM sold (SELL) and price rose → adverse (mid > fill_price)
    // adverse_total_ticks_ accumulates signed adverse ticks per fill.
    void mark_post_fill_move(int32_t new_mid_ticks) noexcept {
        if (last_fill_price_ticks_ <= 0) return;   // no last fill
        const int32_t delta = new_mid_ticks - last_fill_price_ticks_;
        const int32_t adverse = (last_fill_side_ == Side::BUY) ? -delta : delta;
        adverse_total_ticks_ += adverse;
        last_fill_price_ticks_ = 0;   // consumed
    }

    // avg_adverse_ticks_per_fill: the average number of ticks of "move against us"
    // after a fill. A positive number = informed flow is hitting us, negative = lucky
    // (counterparties lost on us).
    double avg_adverse_ticks_per_fill() const noexcept {
        return fills_received_ > 0
            ? static_cast<double>(adverse_total_ticks_) / fills_received_ : 0.0;
    }
    int64_t adverse_total_ticks() const noexcept { return adverse_total_ticks_; }

    // pnl: mark-to-market in dollars at the given mid (in ticks).
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
