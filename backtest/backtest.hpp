/*
 * Backtester — atrybucja wyników strategii (expansion #80).
 *
 * Lab mierzyl dotad TYLKO throughput (msg/sec). Throughput nie mowi czy alpha
 * dziala — do tego trzeba metryk per strategia: czy zarabia, jak ryzykownie,
 * jak czesto trafia. Ten header to reuzywalny, deterministyczny RDZEN tych
 * metryk; backtest_demo karmi go realnym przebiegiem strategii przez OMS.
 *
 * Wejscie (akumulatory, O(1) per zdarzenie, zero alokacji):
 *   on_order(filled)  — kazde zlecenie: licznik fill-rate
 *   on_trade(pnl)     — kazda ZREALIZOWANA noga (delta realized P&L): equity,
 *                        win/loss, wariancja do Sharpe, drawdown
 *
 * Wyjscie (Report):
 *   total_pnl, trades, wins/losses, hit_rate, avg_win/avg_loss, profit_factor,
 *   sharpe (per-trade, skalowany sqrt(n)), max_drawdown (na krzywej equity),
 *   fill_rate.
 *
 * Sharpe per-trade = mean(pnl)/stddev(pnl) * sqrt(trades). To "information
 * ratio" na sekwencji transakcji (nie annualizowany kalendarzowo — w labie
 * nie mamy realnych interwalow czasowych miedzy fillami). Wartosc > ~2 sugeruje
 * istotnosc; < 0 = strategia traci.
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace backtest {

struct Report {
    std::uint64_t trades;
    std::uint64_t wins;
    std::uint64_t losses;
    std::uint64_t orders_submitted;
    std::uint64_t orders_filled;
    double total_pnl;
    double avg_win;
    double avg_loss;          // <= 0
    double hit_rate;          // wins / trades  [0..1]
    double profit_factor;     // sum(wins) / |sum(losses)|
    double sharpe;            // per-trade, * sqrt(trades)
    double max_drawdown;      // peak-to-trough na skumulowanym P&L (>= 0)
    double fill_rate;         // filled / submitted [0..1]
    double expectancy;        // sredni P&L na transakcje (total_pnl / trades)
    double largest_win;       // najwieksza pojedyncza wygrana
    double largest_loss;      // najwieksza pojedyncza strata (<= 0)
    std::uint64_t max_consecutive_wins;
    std::uint64_t max_consecutive_losses;
};

class Backtester {
    std::uint64_t trades_    = 0;
    std::uint64_t wins_      = 0;
    std::uint64_t losses_    = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t filled_    = 0;

    double sum_pnl_  = 0.0;
    double sum_sq_   = 0.0;   // Σ pnl² — do wariancji jednym przebiegiem
    double sum_win_  = 0.0;   // Σ dodatnich
    double sum_loss_ = 0.0;   // Σ ujemnych (≤ 0)

    double equity_   = 0.0;   // skumulowany realized P&L (krzywa equity)
    double peak_     = 0.0;   // high-water mark
    double max_dd_   = 0.0;   // najwiekszy spadek od peak

    double largest_win_  = 0.0;
    double largest_loss_ = 0.0;
    std::uint64_t cur_win_streak_  = 0, max_win_streak_  = 0;
    std::uint64_t cur_loss_streak_ = 0, max_loss_streak_ = 0;

    // Atrybucja P&L per-tag (#102) — np. per strategia/symbol/venue.
    std::unordered_map<std::string, double> pnl_by_tag_;

public:
    // on_order: zarejestruj zlecenie do fill-rate. filled=true gdy doszlo do
    // jakiegokolwiek wykonania.
    void on_order(bool filled) noexcept {
        ++submitted_;
        if (filled) ++filled_;
    }

    // on_trade: zarejestruj zrealizowana noge (delta realized P&L, w dolarach).
    // pnl == 0 liczony jako "nie-strata" (win) — flat round-trip nie psuje
    // hit-rate w dol; rzadki przypadek przy fixed-point.
    void on_trade(double pnl) noexcept {
        ++trades_;
        sum_pnl_ += pnl;
        sum_sq_  += pnl * pnl;
        if (pnl >= 0.0) {
            ++wins_; sum_win_ += pnl;
            if (pnl > largest_win_) largest_win_ = pnl;
            cur_loss_streak_ = 0;
            if (++cur_win_streak_ > max_win_streak_) max_win_streak_ = cur_win_streak_;
        } else {
            ++losses_; sum_loss_ += pnl;
            if (pnl < largest_loss_) largest_loss_ = pnl;
            cur_win_streak_ = 0;
            if (++cur_loss_streak_ > max_loss_streak_) max_loss_streak_ = cur_loss_streak_;
        }

        equity_ += pnl;
        if (equity_ > peak_) peak_ = equity_;
        const double dd = peak_ - equity_;
        if (dd > max_dd_) max_dd_ = dd;
    }

    // on_trade z tagiem (#102): jak on_trade, ale dodatkowo przypisuje P&L do
    // kubełka `tag` (strategia/symbol/venue). Globalne metryki bez zmian.
    void on_trade(double pnl, const char* tag) {
        on_trade(pnl);
        if (tag && *tag) pnl_by_tag_[tag] += pnl;
    }

    // pnl_for_tag: skumulowany P&L danego tagu (0 gdy nieznany).
    double pnl_for_tag(const char* tag) const {
        const auto it = pnl_by_tag_.find(tag);
        return (it != pnl_by_tag_.end()) ? it->second : 0.0;
    }
    size_t tag_count() const noexcept { return pnl_by_tag_.size(); }

    Report compute() const noexcept {
        Report r{};
        r.trades           = trades_;
        r.wins             = wins_;
        r.losses           = losses_;
        r.orders_submitted = submitted_;
        r.orders_filled    = filled_;
        r.total_pnl        = sum_pnl_;
        r.avg_win          = wins_   ? sum_win_  / static_cast<double>(wins_)   : 0.0;
        r.avg_loss         = losses_ ? sum_loss_ / static_cast<double>(losses_) : 0.0;
        r.hit_rate         = trades_ ? static_cast<double>(wins_) / static_cast<double>(trades_) : 0.0;
        r.profit_factor    = (sum_loss_ < 0.0) ? sum_win_ / (-sum_loss_)
                                               : (sum_win_ > 0.0 ? INFINITY : 0.0);
        if (trades_ > 1) {
            const double mean = sum_pnl_ / static_cast<double>(trades_);
            const double var  = sum_sq_ / static_cast<double>(trades_) - mean * mean;
            const double sd   = var > 0.0 ? std::sqrt(var) : 0.0;
            r.sharpe = sd > 0.0 ? mean / sd * std::sqrt(static_cast<double>(trades_)) : 0.0;
        } else {
            r.sharpe = 0.0;
        }
        r.max_drawdown = max_dd_;
        r.fill_rate    = submitted_ ? static_cast<double>(filled_) / static_cast<double>(submitted_) : 0.0;
        r.expectancy   = trades_ ? sum_pnl_ / static_cast<double>(trades_) : 0.0;
        r.largest_win  = largest_win_;
        r.largest_loss = largest_loss_;
        r.max_consecutive_wins   = max_win_streak_;
        r.max_consecutive_losses = max_loss_streak_;
        return r;
    }

    void print_report(const char* label = "Strategy") const {
        const Report r = compute();
        std::printf("\n=== Backtest Report: %s ===\n", label);
        std::printf("  Orders submitted : %lu\n",      (unsigned long)r.orders_submitted);
        std::printf("  Orders filled    : %lu  (fill-rate %.1f%%)\n",
                    (unsigned long)r.orders_filled, r.fill_rate * 100.0);
        std::printf("  Closed trades    : %lu  (%lu win / %lu loss)\n",
                    (unsigned long)r.trades, (unsigned long)r.wins, (unsigned long)r.losses);
        std::printf("  Hit rate         : %.1f%%\n",   r.hit_rate * 100.0);
        std::printf("  Total P&L        : $%.2f\n",    r.total_pnl);
        std::printf("  Avg win / loss   : $%.2f / $%.2f\n", r.avg_win, r.avg_loss);
        std::printf("  Profit factor    : %.2f\n",     r.profit_factor);
        std::printf("  Expectancy/trade : $%.2f\n",    r.expectancy);
        std::printf("  Largest win/loss : $%.2f / $%.2f\n", r.largest_win, r.largest_loss);
        std::printf("  Max win/loss streak: %lu / %lu\n",
                    (unsigned long)r.max_consecutive_wins, (unsigned long)r.max_consecutive_losses);
        std::printf("  Sharpe (per-trade): %.2f\n",    r.sharpe);
        std::printf("  Max drawdown     : $%.2f\n",    r.max_drawdown);
    }
};

}  // namespace backtest
