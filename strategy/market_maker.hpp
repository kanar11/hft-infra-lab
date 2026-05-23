/*
 * MarketMaker — symetryczny dwustronny quoter z inventory skew.
 *
 * Mean reversion (mean_reversion.hpp) jest REAKTYWNY — czeka aż cena
 * odbiegnie od średniej i wystawia 1 zlecenie. Market maker jest PROAKTYWNY:
 * kwotuje cały czas obie strony book'a, zarabia spread × wolumen który
 * przecina jego kwotowania i jednocześnie zarządza powstałym inventory.
 *
 * Algorytm per tick:
 *   mid  = (best_bid + best_ask) / 2
 *   skew = -position * risk_aversion_ticks         [w tickach]
 *
 *     long inventory  → ujemny skew → kwotowania w DÓŁ
 *       (zachęca egzekucje na ASK makera, zniechęca BID-y które
 *        rozszerzyłyby long jeszcze bardziej)
 *     short inventory → dodatni skew → kwotowania w GÓRĘ
 *
 *   target_bid = mid - half_spread + skew
 *   target_ask = mid + half_spread + skew
 *
 * Limity inventory:
 *   position >=  max_inventory → BID wyłączony (nie rośniemy w long)
 *   position <= -max_inventory → ASK wyłączony (nie rośniemy w short)
 *
 * Model P&L:
 *   cash_ kumuluje signed ticks × shares każdego fila. Mark-to-market
 *   P&L = (cash_ + position * mid) / 100 dolarów.
 *
 * Klasa integration-agnostic — udostępnia czyste quote() / apply_fill().
 * Wywołujący (mm_demo albo przyszły OMS-runner) jest odpowiedzialny za
 * faktyczne submit/cancel zleceń przez OMS i wpinanie fillsów z powrotem.
 */
#pragma once

#include "../common/types.hpp"

#include <cstdint>
#include <cstring>


namespace mm {

struct MMConfig {
    int32_t quote_size          = 10;     // shares per side
    int32_t half_spread_ticks   = 2;      // odległość od mid (1 tick = $0.01)
    int32_t max_inventory       = 1000;   // hard cap na net position
    double  risk_aversion_ticks = 0.05;   // skew (ticki) na share net inventory
};


struct Quote {
    int32_t bid_price;   // 0 = strona wyłączona (np. przy max_inventory)
    int32_t ask_price;
    int32_t bid_size;
    int32_t ask_size;
};


class MarketMaker {
    MMConfig cfg_;
    char     symbol_[9];

    int32_t  position_     = 0;
    int64_t  cash_ticks_   = 0;   // signed cumulative cash flow w ticks*shares
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

    // quote: oblicz docelowe kwotowania na ten tick przy danym best_bid/best_ask
    // rynku. Oba w tickach (1 tick = $0.01).
    Quote quote(int32_t best_bid, int32_t best_ask) noexcept {
        Quote q{0, 0, 0, 0};
        if (best_bid <= 0 || best_ask <= 0 || best_ask <= best_bid) return q;

        const int32_t mid  = (best_bid + best_ask) / 2;
        const int32_t skew = static_cast<int32_t>(-static_cast<double>(position_)
                                                    * cfg_.risk_aversion_ticks);

        const int32_t bid_target = mid - cfg_.half_spread_ticks + skew;
        const int32_t ask_target = mid + cfg_.half_spread_ticks + skew;

        // BID wyłączony gdy przy long-limicie; ASK wyłączony gdy przy short-limicie.
        if (position_ < cfg_.max_inventory) {
            q.bid_price = bid_target;
            q.bid_size  = cfg_.quote_size;
        }
        if (position_ > -cfg_.max_inventory) {
            q.ask_price = ask_target;
            q.ask_size  = cfg_.quote_size;
        }

        // Quote churn: każda zmiana którejkolwiek strony liczy się jako cancel+replace.
        if (q.bid_price != last_bid_ || q.ask_price != last_ask_) {
            if (last_bid_ != 0 || last_ask_ != 0) ++quotes_cancelled_;
            ++quotes_placed_;
        }
        last_bid_ = q.bid_price;
        last_ask_ = q.ask_price;
        return q;
    }

    // apply_fill: wywołujący mówi nam że jedno z naszych kwotowań zostało egzekutowane.
    //   side == BUY  → maker kupił   (inventory ↑, cash ↓)
    //   side == SELL → maker sprzedał (inventory ↓, cash ↑)
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

    // pnl: mark-to-market w dolarach przy danym mid (w tickach).
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
