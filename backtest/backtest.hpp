/*
 * Backtester — strategy performance attribution (expansion #80, analytics #263).
 *
 * The lab used to measure ONLY throughput (msg/sec). Throughput says nothing about
 * whether alpha works — for that you need per-strategy metrics: does it make money,
 * how risky, how often it's right. This header is the reusable, deterministic CORE
 * of those metrics; backtest_demo feeds it a real strategy run through the OMS.
 *
 * Input (accumulators, O(1) per event, zero allocation except the equity curve):
 *   on_order(filled)  — every order: fill-rate counter
 *   on_trade(pnl)     — every CLOSED leg (realized-P&L delta): equity, win/loss,
 *                       variance (Sharpe/Sortino), drawdown, streaks, equity curve
 *
 * Output:
 *   Report (O(1) from accumulators): total_pnl, trades, wins/losses, hit_rate,
 *     avg_win/avg_loss, profit_factor, sharpe, sortino, max_drawdown,
 *     recovery_factor, payoff_ratio, kelly_fraction, expectancy, gross_profit/loss,
 *     largest_win/loss, streaks, fill_rate.
 *   On-demand curve analytics (O(n) over the equity curve): max_drawdown_duration,
 *     ulcer_index, value_at_risk, conditional_value_at_risk.
 *
 * Sharpe per-trade = mean(pnl)/stddev(pnl) * sqrt(trades). An "information ratio"
 * over the trade sequence (not calendar-annualized — the lab has no real time
 * intervals between fills). > ~2 suggests significance; < 0 = the strategy loses.
 * Sortino is the same but only penalizes DOWNSIDE deviation (negative legs),
 * which is fairer for asymmetric payoff profiles.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

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
    double sortino;           // like sharpe but downside-deviation only
    double max_drawdown;      // peak-to-trough on cumulative P&L (>= 0)
    double recovery_factor;   // total_pnl / max_drawdown (return per unit of DD risk)
    double payoff_ratio;      // avg_win / |avg_loss|
    double kelly_fraction;    // optimal bet fraction: p - (1-p)/payoff_ratio
    double fill_rate;         // filled / submitted [0..1]
    double expectancy;        // mean P&L per trade (total_pnl / trades)
    double gross_profit;      // sum of winning legs (>= 0)
    double gross_loss;        // sum of losing legs (<= 0)
    double largest_win;       // largest single win
    double largest_loss;      // largest single loss (<= 0)
    std::uint64_t max_consecutive_wins;
    std::uint64_t max_consecutive_losses;
};

class Backtester {
    std::uint64_t trades_    = 0;
    std::uint64_t wins_      = 0;
    std::uint64_t losses_    = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t filled_    = 0;

    double sum_pnl_     = 0.0;
    double sum_sq_      = 0.0;   // Σ pnl²       — variance in one pass
    double sum_sq_neg_  = 0.0;   // Σ pnl² for pnl<0 — downside variance (Sortino)
    double sum_win_     = 0.0;   // Σ positives
    double sum_loss_    = 0.0;   // Σ negatives (<= 0)

    double equity_   = 0.0;   // cumulative realized P&L (equity curve)
    double peak_     = 0.0;   // high-water mark
    double max_dd_   = 0.0;   // largest drop from peak

    double largest_win_  = 0.0;
    double largest_loss_ = 0.0;
    std::uint64_t cur_win_streak_  = 0, max_win_streak_  = 0;
    std::uint64_t cur_loss_streak_ = 0, max_loss_streak_ = 0;

    // Per-tag P&L attribution (#102) — e.g. per strategy/symbol/venue.
    std::unordered_map<std::string, double> pnl_by_tag_;

    // Equity curve (#108) — cumulative P&L after each trade (for shape/drawdown
    // analysis, not just its maximum, and for VaR/CVaR/ulcer reconstruction).
    std::vector<double> equity_curve_;

public:
    // on_order: register an order for fill-rate. filled=true when any execution
    // occurred.
    void on_order(bool filled) noexcept {
        ++submitted_;
        if (filled) ++filled_;
    }

    // on_trade: register a closed leg (realized-P&L delta, in dollars). pnl == 0
    // counts as a "non-loss" (win) — a flat round-trip shouldn't hurt the hit-rate;
    // a rare case with fixed-point pricing.
    void on_trade(double pnl) {   // not reserved: equity_curve_ may reallocate
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
            sum_sq_neg_ += pnl * pnl;
            if (pnl < largest_loss_) largest_loss_ = pnl;
            cur_win_streak_ = 0;
            if (++cur_loss_streak_ > max_loss_streak_) max_loss_streak_ = cur_loss_streak_;
        }

        equity_ += pnl;
        if (equity_ > peak_) peak_ = equity_;
        const double dd = peak_ - equity_;
        if (dd > max_dd_) max_dd_ = dd;
        equity_curve_.push_back(equity_);
    }

    // on_trade with a tag (#102): like on_trade, but also attributes P&L to the
    // `tag` bucket (strategy/symbol/venue). Global metrics unchanged.
    void on_trade(double pnl, const char* tag) {
        on_trade(pnl);
        if (tag && *tag) pnl_by_tag_[tag] += pnl;
    }

    // pnl_for_tag: cumulative P&L for a tag (0 if unknown).
    double pnl_for_tag(const char* tag) const {
        const auto it = pnl_by_tag_.find(tag);
        return (it != pnl_by_tag_.end()) ? it->second : 0.0;
    }
    size_t tag_count() const noexcept { return pnl_by_tag_.size(); }

    // equity_curve: cumulative P&L after each trade (element i = equity after i+1
    // trades). Last element == total_pnl.
    const std::vector<double>& equity_curve() const noexcept { return equity_curve_; }

    Report compute() const noexcept {
        Report r{};
        r.trades           = trades_;
        r.wins             = wins_;
        r.losses           = losses_;
        r.orders_submitted = submitted_;
        r.orders_filled    = filled_;
        r.total_pnl        = sum_pnl_;
        r.gross_profit     = sum_win_;
        r.gross_loss       = sum_loss_;
        r.avg_win          = wins_   ? sum_win_  / static_cast<double>(wins_)   : 0.0;
        r.avg_loss         = losses_ ? sum_loss_ / static_cast<double>(losses_) : 0.0;
        r.hit_rate         = trades_ ? static_cast<double>(wins_) / static_cast<double>(trades_) : 0.0;
        r.profit_factor    = (sum_loss_ < 0.0) ? sum_win_ / (-sum_loss_)
                                               : (sum_win_ > 0.0 ? INFINITY : 0.0);
        // Payoff (win/loss) ratio and the Kelly-optimal bet fraction.
        r.payoff_ratio = (r.avg_loss < 0.0) ? r.avg_win / (-r.avg_loss)
                                            : (r.avg_win > 0.0 ? INFINITY : 0.0);
        if (losses_ == 0) {
            r.kelly_fraction = (wins_ > 0) ? 1.0 : 0.0;   // no losses -> bet it all
        } else if (r.payoff_ratio > 0.0) {
            r.kelly_fraction = r.hit_rate - (1.0 - r.hit_rate) / r.payoff_ratio;
        } else {
            r.kelly_fraction = 0.0;
        }
        if (trades_ > 1) {
            const double n    = static_cast<double>(trades_);
            const double mean = sum_pnl_ / n;
            const double var  = sum_sq_ / n - mean * mean;
            const double sd   = var > 0.0 ? std::sqrt(var) : 0.0;
            r.sharpe = sd > 0.0 ? mean / sd * std::sqrt(n) : 0.0;
            const double dvar = sum_sq_neg_ / n;                 // downside variance
            const double dsd  = dvar > 0.0 ? std::sqrt(dvar) : 0.0;
            r.sortino = dsd > 0.0 ? mean / dsd * std::sqrt(n) : 0.0;
        }
        r.max_drawdown    = max_dd_;
        r.recovery_factor = (max_dd_ > 0.0) ? sum_pnl_ / max_dd_
                                            : (sum_pnl_ > 0.0 ? INFINITY : 0.0);
        r.fill_rate    = submitted_ ? static_cast<double>(filled_) / static_cast<double>(submitted_) : 0.0;
        r.expectancy   = trades_ ? sum_pnl_ / static_cast<double>(trades_) : 0.0;
        r.largest_win  = largest_win_;
        r.largest_loss = largest_loss_;
        r.max_consecutive_wins   = max_win_streak_;
        r.max_consecutive_losses = max_loss_streak_;
        return r;
    }

    // === On-demand curve analytics (O(n) over the equity curve) ===

    // max_drawdown_duration: the longest stretch (in trades) the equity stayed
    // BELOW a prior high-water mark before recovering — i.e. the worst "time
    // underwater". A strategy with a small max_drawdown but a long duration can
    // still be intolerable to run. Returns a trade count (not wall-clock).
    std::uint64_t max_drawdown_duration() const noexcept {
        std::uint64_t cur = 0, worst = 0;
        double peak = 0.0;
        for (double eq : equity_curve_) {
            if (eq >= peak) { peak = eq; cur = 0; }     // new high -> recovered
            else            { if (++cur > worst) worst = cur; }
        }
        return worst;
    }

    // ulcer_index: root-mean-square of the drawdown from the running peak across
    // the whole curve (in dollars). Unlike max_drawdown (a single worst point) it
    // penalizes the DEPTH and DURATION of all drawdowns — a smoother equity curve
    // scores lower. 0 for a monotonically rising curve.
    double ulcer_index() const noexcept {
        if (equity_curve_.empty()) return 0.0;
        double peak = 0.0, sum_sq = 0.0;
        for (double eq : equity_curve_) {
            if (eq > peak) peak = eq;
            const double dd = peak - eq;
            sum_sq += dd * dd;
        }
        return std::sqrt(sum_sq / static_cast<double>(equity_curve_.size()));
    }

    // value_at_risk: the per-trade P&L at the lower-tail (1 - confidence) quantile.
    // VaR(0.95) answers "the trade loss I won't exceed 95% of the time" and is
    // returned as a NON-NEGATIVE loss magnitude (0 if that quantile is a profit).
    // Reconstructs per-trade P&L from the equity curve, so no extra storage.
    double value_at_risk(double confidence) const {
        std::vector<double> pnls = trade_pnls();
        if (pnls.empty()) return 0.0;
        std::sort(pnls.begin(), pnls.end());
        const double q = quantile_value(pnls, 1.0 - confidence);
        return q < 0.0 ? -q : 0.0;
    }

    // conditional_value_at_risk (expected shortfall): the AVERAGE per-trade loss
    // in the worst (1 - confidence) tail — captures how bad the bad days are, not
    // just the threshold. Returned as a non-negative loss magnitude.
    double conditional_value_at_risk(double confidence) const {
        std::vector<double> pnls = trade_pnls();
        if (pnls.empty()) return 0.0;
        std::sort(pnls.begin(), pnls.end());
        size_t k = static_cast<size_t>((1.0 - confidence) * static_cast<double>(pnls.size()));
        if (k == 0) k = 1;                              // at least the single worst
        double sum = 0.0;
        for (size_t i = 0; i < k; ++i) sum += pnls[i];
        const double mean = sum / static_cast<double>(k);
        return mean < 0.0 ? -mean : 0.0;
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
        std::printf("  Gross profit/loss: $%.2f / $%.2f\n", r.gross_profit, r.gross_loss);
        std::printf("  Avg win / loss   : $%.2f / $%.2f\n", r.avg_win, r.avg_loss);
        std::printf("  Profit factor    : %.2f\n",     r.profit_factor);
        std::printf("  Payoff ratio     : %.2f\n",     r.payoff_ratio);
        std::printf("  Kelly fraction   : %.3f\n",     r.kelly_fraction);
        std::printf("  Expectancy/trade : $%.2f\n",    r.expectancy);
        std::printf("  Largest win/loss : $%.2f / $%.2f\n", r.largest_win, r.largest_loss);
        std::printf("  Max win/loss streak: %lu / %lu\n",
                    (unsigned long)r.max_consecutive_wins, (unsigned long)r.max_consecutive_losses);
        std::printf("  Sharpe (per-trade): %.2f\n",    r.sharpe);
        std::printf("  Sortino          : %.2f\n",     r.sortino);
        std::printf("  Max drawdown     : $%.2f  (%lu trades underwater)\n",
                    r.max_drawdown, (unsigned long)max_drawdown_duration());
        std::printf("  Recovery factor  : %.2f\n",     r.recovery_factor);
        std::printf("  Ulcer index      : $%.2f\n",    ulcer_index());
        std::printf("  VaR / CVaR (95%%) : $%.2f / $%.2f\n",
                    value_at_risk(0.95), conditional_value_at_risk(0.95));
    }

private:
    // trade_pnls: reconstruct per-trade P&L from the equity curve (diffs).
    std::vector<double> trade_pnls() const {
        std::vector<double> pnls;
        pnls.reserve(equity_curve_.size());
        double prev = 0.0;
        for (double eq : equity_curve_) { pnls.push_back(eq - prev); prev = eq; }
        return pnls;
    }

    // quantile_value: linear-ish lower-tail quantile of a SORTED vector. frac in
    // [0,1]; frac=0 -> minimum. Index = floor(frac * (n-1)).
    static double quantile_value(const std::vector<double>& sorted, double frac) {
        if (sorted.empty()) return 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        const size_t idx = static_cast<size_t>(frac * static_cast<double>(sorted.size() - 1));
        return sorted[idx];
    }
};

}  // namespace backtest
