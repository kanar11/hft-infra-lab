/*
 * ITCHOrderBook — L3 book reconstruction from the NASDAQ ITCH stream
 * (expansion #82).
 *
 * itch_parser.hpp decodes SINGLE messages; nobody assembled a BOOK from them.
 * This is the missing bridge: a feed handler that, from Add/Execute/Cancel/
 * Delete/Replace, rebuilds the full picture of the market (best bid/ask, depth,
 * resting orders by order_ref).
 *
 * This is NOT a matching engine (orderbook_pro) — the reconstructor only REPLAYS
 * what the feed says; it never matches and never crosses. Real ITCH is an
 * already-matched book, so Adds on both sides do not cross — and
 * executions/cancels arrive directly as messages, not as a result of a match.
 *
 * Event mapping:
 *   ADD_ORDER       → resting order (ref → side/price/qty) + level aggregate
 *   ORDER_EXECUTED  → reduce resting qty by exec_shares (0 → remove)
 *   ORDER_CANCELLED → reduce qty by cancelled_shares (partial cancel)
 *   DELETE_ORDER    → remove the resting order entirely
 *   REPLACE_ORDER   → delete(orig) + add(new) with a new price/qty
 *   (TRADE/SYSTEM/STOCK_DIR — do not change the limit-order book)
 *
 * Prices in int64 ticks (× 100 = $0.01), per-level aggregates in two std::map.
 * best_bid = max key of bids_, best_ask = min key of asks_.
 */
#pragma once

#include "itch_parser.hpp"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace itch {

class ITCHOrderBook {
    struct Resting {
        char     side;          // 'B' / 'S'
        int64_t  price_ticks;
        uint32_t shares;
    };

    std::unordered_map<int64_t, Resting> orders_;  // order_ref → resting
    std::map<int64_t, int64_t> bids_;              // price_ticks → Σ qty (BUY)
    std::map<int64_t, int64_t> asks_;              // price_ticks → Σ qty (SELL)

    // Feed-handler statistics / diagnostics.
    uint64_t adds_ = 0, executes_ = 0, cancels_ = 0, deletes_ = 0, replaces_ = 0;
    uint64_t orphans_ = 0;   // event for an unknown ref (gap in the feed / desync)

    static int64_t to_ticks(double price) noexcept {
        return static_cast<int64_t>(price * 100.0 + (price >= 0 ? 0.5 : -0.5));
    }
    std::map<int64_t, int64_t>& side_book(char side) noexcept {
        return (side == 'B') ? bids_ : asks_;
    }

    // reduce_: remove qty from a resting order (execute/cancel). Cleans up the level
    // and the order when they drop to zero. An unknown ref = orphan (a gap-recovery signal).
    void reduce_(int64_t ref, uint32_t qty) noexcept {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++orphans_; return; }
        Resting& r = it->second;
        const uint32_t dec = (qty < r.shares) ? qty : r.shares;
        r.shares -= dec;
        auto& book = side_book(r.side);
        auto lvl = book.find(r.price_ticks);
        if (lvl != book.end()) {
            lvl->second -= static_cast<int64_t>(dec);
            if (lvl->second <= 0) book.erase(lvl);
        }
        if (r.shares == 0) orders_.erase(it);
    }

public:
    // --- Single events (when called directly) ---
    void on_add(int64_t ref, char side, double price, uint32_t shares) noexcept {
        if (shares == 0) return;
        const int64_t px = to_ticks(price);
        orders_[ref] = Resting{side, px, shares};
        side_book(side)[px] += static_cast<int64_t>(shares);
        ++adds_;
    }
    void on_execute(int64_t ref, uint32_t exec_shares) noexcept { reduce_(ref, exec_shares); ++executes_; }
    void on_cancel(int64_t ref, uint32_t cancelled_shares) noexcept { reduce_(ref, cancelled_shares); ++cancels_; }
    void on_delete(int64_t ref) noexcept {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++orphans_; ++deletes_; return; }
        reduce_(ref, it->second.shares);   // remove the whole thing
        ++deletes_;
    }
    void on_replace(int64_t orig_ref, int64_t new_ref, double new_price, uint32_t new_shares) noexcept {
        auto it = orders_.find(orig_ref);
        if (it == orders_.end()) { ++orphans_; ++replaces_; return; }
        const char side = it->second.side;   // replace keeps the side
        reduce_(orig_ref, it->second.shares);
        on_add(new_ref, side, new_price, new_shares);
        --adds_;                              // on_add counted an add; a replace is not an add
        ++replaces_;
    }

    // apply: dispatch from a ParsedMessage (the main entry — feed it the parser's stream).
    void apply(const ParsedMessage& pm) noexcept {
        switch (pm.type) {
            case MsgType::ADD_ORDER:
                on_add(pm.data.add_order.order_ref, pm.data.add_order.side,
                       pm.data.add_order.price, pm.data.add_order.shares);
                break;
            case MsgType::ADD_ORDER_MPID:
                on_add(pm.data.add_order_mpid.order_ref, pm.data.add_order_mpid.side,
                       pm.data.add_order_mpid.price, pm.data.add_order_mpid.shares);
                break;
            case MsgType::ORDER_EXECUTED:
                on_execute(pm.data.order_executed.order_ref, pm.data.order_executed.exec_shares);
                break;
            case MsgType::ORDER_CANCELLED:
                on_cancel(pm.data.order_cancelled.order_ref, pm.data.order_cancelled.cancelled_shares);
                break;
            case MsgType::DELETE_ORDER:
                on_delete(pm.data.delete_order.order_ref);
                break;
            case MsgType::REPLACE_ORDER:
                on_replace(pm.data.replace_order.orig_order_ref, pm.data.replace_order.new_order_ref,
                           pm.data.replace_order.new_price, pm.data.replace_order.new_shares);
                break;
            default: break;   // TRADE / SYSTEM_EVENT / STOCK_DIRECTORY — do not touch the book
        }
    }

    // --- Book-state queries ---
    double  best_bid()  const noexcept { return bids_.empty() ? 0.0 : bids_.rbegin()->first / 100.0; }
    double  best_ask()  const noexcept { return asks_.empty() ? 0.0 : asks_.begin()->first  / 100.0; }
    double  spread()    const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        return best_ask() - best_bid();
    }
    // spread_bps: spread relative to mid in basis points (#131) — a measure of
    // market quality/liquidity independent of the price level.
    double  spread_bps() const noexcept {
        const double m = mid_price();
        return m > 0.0 ? spread() / m * 10000.0 : 0.0;
    }
    // spread_ticks: spread in a WHOLE number of ticks ($0.01) (#207). For strategies
    // tick-size aware: 1 = the tightest book (touch, MMs fight for priority), larger =
    // there is room to improve the quote. 0 when one-sided.
    int64_t spread_ticks() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0;
        return asks_.begin()->first - bids_.rbegin()->first;   // prices in ticks x100
    }
    // is_marketable: would a limit order at limit_price cross immediately against
    // the resting book (#261)? BUY crosses when limit >= best_ask, SELL crosses
    // when limit <= best_bid. Pre-submit check: marketable -> takes liquidity now,
    // otherwise it would rest. false when that side has no resting liquidity.
    bool is_marketable(char side, double limit_price) const noexcept {
        if (side == 'B') { const double a = best_ask(); return a > 0.0 && limit_price >= a; }
        else             { const double b = best_bid(); return b > 0.0 && limit_price <= b; }
    }
    // clear: reset the reconstructor to empty (e.g. start of a new session / re-sync
    // after snapshot recovery). The feed-handler statistics are zeroed too.
    void clear() noexcept {
        orders_.clear(); bids_.clear(); asks_.clear();
        adds_ = executes_ = cancels_ = deletes_ = replaces_ = orphans_ = 0;
    }
    // mid_price: average of best bid/ask; 0 when the book is one-sided.
    double  mid_price() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        return (best_bid() + best_ask()) / 2.0;
    }
    int64_t best_bid_qty() const noexcept { return bids_.empty() ? 0 : bids_.rbegin()->second; }
    int64_t best_ask_qty() const noexcept { return asks_.empty() ? 0 : asks_.begin()->second; }
    // imbalance: top-of-book order-book imbalance = (bidQ-askQ)/(bidQ+askQ),
    // range [-1,1]. >0 = buy-side dominance (upward pressure), <0 = sell-side.
    // A classic short-term direction predictor in microstructure.
    double  imbalance() const noexcept {
        const int64_t b = best_bid_qty(), a = best_ask_qty();
        const int64_t tot = b + a;
        return tot > 0 ? static_cast<double>(b - a) / static_cast<double>(tot) : 0.0;
    }
    // microprice: size-weighted fair value (Stoikov). When the bid dominates
    // (Q_bid > Q_ask) it leans toward the ASK (an expected up-move) and vice versa —
    // a better estimator of the "true" price than a simple mid. 0 when the book is one-sided.
    //   microprice = (P_ask*Q_bid + P_bid*Q_ask) / (Q_bid + Q_ask)
    // liquidity_within: total qty within N TICKS of best on a given side (#164).
    // A measure of liquidity density at the top (how much fills near the touch)
    // independent of the number of levels. BUY: bids >= best_bid - ticks; SELL: asks
    // <= best_ask + ticks.
    int64_t liquidity_within(char side, int ticks) const noexcept {
        if (ticks < 0) return 0;
        int64_t sum = 0;
        if (side == 'B') {
            if (bids_.empty()) return 0;
            const int64_t floor = bids_.rbegin()->first - ticks;
            for (auto it = bids_.rbegin(); it != bids_.rend() && it->first >= floor; ++it) sum += it->second;
        } else {
            if (asks_.empty()) return 0;
            const int64_t ceil = asks_.begin()->first + ticks;
            for (auto it = asks_.begin(); it != asks_.end() && it->first <= ceil; ++it) sum += it->second;
        }
        return sum;
    }

    // fillable_shares: how many shares can be executed up to a LIMIT PRICE (#223). BUY:
    // sum of ask qty at price <= limit; SELL: sum of bid qty at price >= limit. Unlike
    // liquidity_within (ticks from best) — here the threshold is a specific order price.
    // Sizing a limit order: how much fills immediately without crossing past the limit.
    int64_t fillable_shares(char side, double limit_price) const noexcept {
        const int64_t lim = to_ticks(limit_price);
        int64_t sum = 0;
        if (side == 'B') {                       // buy: asks ascending, take <= limit
            for (const auto& [px, qty] : asks_) { if (px > lim) break; sum += qty; }
        } else {                                 // sell: bids descending from best, take >= limit
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                if (it->first < lim) break;
                sum += it->second;
            }
        }
        return sum;
    }

    // price_to_fill: the WORST price (level) that must be touched to execute
    // `shares` by walking the book (#247). BUY walks asks ascending, SELL walks
    // bids descending; returns the price of the level at which cumulative size covers
    // the order. This is the LIMIT for a sweep (unlike expected_fill=VWAP). 0 when there
    // is too little liquidity.
    double  price_to_fill(char side, int64_t shares) const noexcept {
        if (shares <= 0) return 0.0;
        int64_t remaining = shares;
        if (side == 'B') {
            for (const auto& [px, qty] : asks_) {
                remaining -= qty;
                if (remaining <= 0) return static_cast<double>(px) / 100.0;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                remaining -= it->second;
                if (remaining <= 0) return static_cast<double>(it->first) / 100.0;
            }
        }
        return 0.0;   // insufficient liquidity
    }

    // levels_to_fill: the NUMBER of price levels that must be touched to fill
    // `shares` by walking the book (#342). Same side convention as price_to_fill/
    // expected_fill: BUY walks asks ascending, SELL walks bids descending.
    // Complements price_to_fill (the worst price touched) and expected_fill (the
    // VWAP): this tells you HOW DEEP the sweep needs to go — useful for choosing
    // between an IOC at a single level and a multi-level sweep. -1 when there is
    // too little liquidity across the whole book to fill the requested size.
    int32_t levels_to_fill(char side, int64_t shares) const noexcept {
        if (shares <= 0) return 0;
        int64_t remaining = shares;
        int32_t levels = 0;
        if (side == 'B') {
            for (const auto& [px, qty] : asks_) {
                (void)px;
                ++levels;
                remaining -= qty;
                if (remaining <= 0) return levels;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                ++levels;
                remaining -= it->second;
                if (remaining <= 0) return levels;
            }
        }
        return -1;   // insufficient liquidity across the whole book
    }

    // total_shares: total resting number of shares on a given side (#174).
    // The whole size of the book on one side — a raw measure of available liquidity
    // (unlike liquidity_within, with no restriction to the touch area).
    int64_t total_shares(char side) const noexcept {
        const auto& book = (side == 'B') ? bids_ : asks_;
        int64_t sum = 0;
        for (const auto& [px, qty] : book) sum += qty;
        return sum;
    }

    // total_notional: total VALUE ($) of resting liquidity on a side (#191).
    // Sum of price*qty over all levels — complements total_shares (shares):
    // two books with the same number of shares can hold different capital at different
    // prices. A measure of depth in dollars.
    double total_notional(char side) const noexcept {
        const auto& book = (side == 'B') ? bids_ : asks_;
        double sum = 0.0;
        for (const auto& [px, qty] : book)
            sum += (static_cast<double>(px) / 100.0) * static_cast<double>(qty);
        return sum;
    }

    // level_count: number of distinct price levels on a given side (#174).
    // The thickness of the book — how many distinct prices have liquidity (sparse vs dense book).
    std::size_t level_count(char side) const noexcept {
        return (side == 'B') ? bids_.size() : asks_.size();
    }

    // is_locked: best_bid == best_ask (#183). A "locked" book — zero spread.
    // Signals a stale/inconsistent picture (a lagging feed) or a moment before
    // matching; a market maker usually pauses quoting.
    bool is_locked() const noexcept {
        return !bids_.empty() && !asks_.empty()
            && bids_.rbegin()->first == asks_.begin()->first;
    }

    // is_crossed: best_bid > best_ask (#183). A "crossed" book — theoretical
    // arbitrage (buy at ask < sell at bid). Realistically: feed inconsistency /
    // a missing Delete; a consumer should reject or take a snapshot.
    bool is_crossed() const noexcept {
        return !bids_.empty() && !asks_.empty()
            && bids_.rbegin()->first > asks_.begin()->first;
    }

    // vwap_depth: volume-weighted price of the top-N levels on a given side (#155).
    // Fair value that accounts for DEPTH (not just the touch); the deeper, the more
    // it reflects the execution price of a larger order.
    double  vwap_depth(char side, int n) const noexcept {
        if (n <= 0) return 0.0;
        double notional = 0.0; int64_t qty = 0; int c = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) {
                notional += (it->first / 100.0) * static_cast<double>(it->second);
                qty += it->second;
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) {
                notional += (it->first / 100.0) * static_cast<double>(it->second);
                qty += it->second;
            }
        }
        return qty > 0 ? notional / static_cast<double>(qty) : 0.0;
    }

    // depth_weighted_mid: fair value that averages the depth-VWAP of both sides over
    // the top N levels (#285) = (vwap_depth('B', n) + vwap_depth('S', n)) / 2. Unlike
    // mid_price (touch only) or microprice (size-weighted touch) this folds in N
    // levels of depth on EACH side, so a large resting wall a few levels deep pulls
    // the fair value toward it. 0 when either side lacks liquidity.
    double  depth_weighted_mid(int n) const noexcept {
        const double bv = vwap_depth('B', n);
        const double av = vwap_depth('S', n);
        return (bv > 0.0 && av > 0.0) ? (bv + av) / 2.0 : 0.0;
    }

    // depth_imbalance: order-book imbalance over the TOP-N levels (#148), a generalization
    // of top-of-book imbalance (#87, n=1). (Σbid - Σask)/(Σbid+Σask) over the n best
    // levels of each side. A deeper picture of pressure than the touch alone.
    double  depth_imbalance(int n) const noexcept {
        if (n <= 0) return 0.0;
        int64_t b = 0, a = 0; int c = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) b += it->second;
        c = 0;
        for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) a += it->second;
        const int64_t tot = b + a;
        return tot > 0 ? static_cast<double>(b - a) / static_cast<double>(tot) : 0.0;
    }
    // notional_imbalance: imbalance weighted by VALUE ($) of the top-N levels (#215).
    // Like depth_imbalance, but weight = price*qty instead of qty alone. Large
    // orders look at notional: 100 shares at $300 weigh 30x more than 100 at $10.
    // (Sb_$ - Sa_$)/(Sb_$ + Sa_$), range [-1,1]. 0 when empty.
    double  notional_imbalance(int n) const noexcept {
        if (n <= 0) return 0.0;
        double b = 0.0, a = 0.0; int c = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c)
            b += (static_cast<double>(it->first) / 100.0) * static_cast<double>(it->second);
        c = 0;
        for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c)
            a += (static_cast<double>(it->first) / 100.0) * static_cast<double>(it->second);
        const double tot = b + a;
        return tot > 0.0 ? (b - a) / tot : 0.0;
    }
    // liquidity_imbalance_within: order-book imbalance of the liquidity within N
    // ticks of each touch (#269) — (bid_liq - ask_liq)/(bid_liq + ask_liq) using
    // liquidity_within (#164) on both sides. Unlike depth_imbalance (top-N price
    // LEVELS) this is anchored to a price DISTANCE from the touch, so it's
    // unaffected by how levels are split. [-1, 1]; 0 when both sides empty.
    double  liquidity_imbalance_within(int ticks) const noexcept {
        const int64_t b = liquidity_within('B', ticks);
        const int64_t a = liquidity_within('S', ticks);
        const int64_t tot = b + a;
        return tot > 0 ? static_cast<double>(b - a) / static_cast<double>(tot) : 0.0;
    }
    double  microprice() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        const int64_t qb = best_bid_qty(), qa = best_ask_qty();
        const int64_t tot = qb + qa;
        if (tot <= 0) return 0.0;
        return (best_ask() * static_cast<double>(qb) + best_bid() * static_cast<double>(qa))
               / static_cast<double>(tot);
    }
    // microprice_skew: microprice - mid (#239). The sign and magnitude of directional
    // pressure in price units: >0 = more liquidity on the BID (upward pressure,
    // microprice pulls toward the ask), <0 = on the ASK (downward). 0 for a balanced
    // or one-sided market. A short-horizon alpha signal.
    double  microprice_skew() const noexcept {
        const double mp = microprice();
        const double m  = mid_price();
        return (mp > 0.0 && m > 0.0) ? mp - m : 0.0;
    }
    // imbalance_signal: discrete directional signal from top-of-book imbalance
    // crossing a threshold (#254). imbalance() = (qbid - qask)/(qbid + qask) in
    // [-1, 1]; returns +1 when bid-heavy beyond +threshold (upward pressure),
    // -1 when ask-heavy beyond -threshold, 0 in between. Ready-to-use entry filter.
    int imbalance_signal(double threshold) const noexcept {
        const double imb = imbalance();
        if (imb >  threshold) return 1;
        if (imb < -threshold) return -1;
        return 0;
    }
    int64_t qty_at(char side, double price) const noexcept {
        const auto& book = (side == 'B') ? bids_ : asks_;
        const auto it = book.find(to_ticks(price));
        return (it != book.end()) ? it->second : 0;
    }
    // expected_fill: pre-trade impact — walk the reconstructed book and compute the VWAP
    // a market order of `shares` on a given side would achieve (BUY takes asks
    // ascending, SELL bids descending). Returns how many shares are executable (≤ shares
    // when liquidity is insufficient); out_vwap = the average price (0 when 0 fill).
    int64_t expected_fill(char side, int64_t shares, double& out_vwap) const noexcept {
        out_vwap = 0.0;
        if (shares <= 0) return 0;
        int64_t remaining = shares, filled = 0;
        double  notional_ticks = 0.0;
        if (side == 'B') {
            for (auto it = asks_.begin(); it != asks_.end() && remaining > 0; ++it) {
                const int64_t take = std::min<int64_t>(remaining, it->second);
                notional_ticks += static_cast<double>(it->first) * take;
                filled += take; remaining -= take;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend() && remaining > 0; ++it) {
                const int64_t take = std::min<int64_t>(remaining, it->second);
                notional_ticks += static_cast<double>(it->first) * take;
                filled += take; remaining -= take;
            }
        }
        if (filled > 0) out_vwap = notional_ticks / static_cast<double>(filled) / 100.0;
        return filled;
    }

    // slippage_bps: expected execution cost in basis points (#199) — how many
    // bps worse than mid a market order of `shares` on a given side would execute at.
    // BUY pays above mid, SELL gets below — in both cases the result is POSITIVE (a cost).
    // Builds on expected_fill (VWAP after walking the book). 0 when there is no fill/mid.
    // Pre-trade sizing: whether the order eats too much spread/depth.
    double slippage_bps(char side, int64_t shares) const noexcept {
        double vwap = 0.0;
        const int64_t filled = expected_fill(side, shares, vwap);
        const double m = mid_price();
        if (filled <= 0 || m <= 0.0 || vwap <= 0.0) return 0.0;
        const double diff = (side == 'B') ? (vwap - m) : (m - vwap);
        return diff / m * 10000.0;
    }

    // round_trip_cost_bps: the full round-trip transaction cost for `shares` (#231)
    // = buy slippage + sell slippage (bps vs mid). How much depth eats when you enter
    // and exit a position of this size — the real break-even threshold for a
    // strategy. 0 when the book is one-sided (no mid). Builds on slippage_bps.
    double round_trip_cost_bps(int64_t shares) const noexcept {
        return slippage_bps('B', shares) + slippage_bps('S', shares);
    }

    // fill_shortfall: how many of `shares` a marketable order would leave UNFILLED
    // after sweeping all available depth on that side (#301) = shares - filled, via
    // expected_fill (#199). 0 means the book fully covers the order; a positive
    // value is the remainder that would rest or go unfilled. Unlike fillable_shares
    // (limit-price bounded) this is bounded only by total displayed depth.
    int64_t fill_shortfall(char side, int64_t shares) const noexcept {
        if (shares <= 0) return 0;
        double vwap = 0.0;
        const int64_t filled = expected_fill(side, shares, vwap);
        return shares - filled;
    }

    // top_levels: copy up to n BEST levels on a given side (BUY: bids descending
    // from best; SELL: asks ascending) — price + aggregated qty. Returns how many
    // levels were actually filled (<= n). L2 depth for strategies/display.
    int top_levels(char side, int n, double* out_px, int64_t* out_qty) const noexcept {
        int c = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) {
                out_px[c]  = it->first / 100.0;
                out_qty[c] = it->second;
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) {
                out_px[c]  = it->first / 100.0;
                out_qty[c] = it->second;
            }
        }
        return c;
    }

    // nth_level_price / nth_level_qty: random access to the n-th best price level on
    // a side (#277, 0-based; BUY counts bids down from best, SELL counts asks up).
    // Single-level lookup without copying the whole top-N (unlike top_levels). 0
    // when n is out of range. Handy for L2 strategies that reference one level.
    double  nth_level_price(char side, int n) const noexcept {
        if (n < 0) return 0.0;
        int i = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it, ++i)
                if (i == n) return static_cast<double>(it->first) / 100.0;
        } else {
            for (auto it = asks_.begin(); it != asks_.end(); ++it, ++i)
                if (i == n) return static_cast<double>(it->first) / 100.0;
        }
        return 0.0;
    }
    int64_t nth_level_qty(char side, int n) const noexcept {
        if (n < 0) return 0;
        int i = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it, ++i)
                if (i == n) return it->second;
        } else {
            for (auto it = asks_.begin(); it != asks_.end(); ++it, ++i)
                if (i == n) return it->second;
        }
        return 0;
    }
    // cumulative_qty: total displayed size across the top N price levels on a side
    // (#293), summing nth_level_qty (#277). How many shares a marketable order would
    // see before walking past depth N — the sizing input for a sweep. Naturally
    // stops at the end of book (out-of-range levels contribute 0).
    int64_t cumulative_qty(char side, int n) const noexcept {
        if (n <= 0) return 0;
        // Single linear walk of the top n levels. Calling nth_level_qty(i) per level
        // re-walked the map from the touch each time → O(n^2); this is O(n) with the
        // same level quantities summed in the same order.
        int64_t sum = 0; int c = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) sum += it->second;
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) sum += it->second;
        }
        return sum;
    }
    // depth_notional: total $ notional resting across the top N price levels on a
    // side (#309) = sum(price * qty) over levels 0..N-1 — the capital a sweep would
    // consume, the $ companion to cumulative_qty (#293, shares). Stops at the end of
    // book (an out-of-range level returns price 0). Uses nth_level_price/qty (#277).
    double  depth_notional(char side, int n) const noexcept {
        if (n <= 0) return 0.0;
        // Single linear walk: the old version called nth_level_price(i) AND
        // nth_level_qty(i) per level, each re-walking the map from the touch → O(n^2)
        // (doubled). O(n) here, same per-level price*qty summed in the same order.
        double sum = 0.0; int c = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c)
                sum += (static_cast<double>(it->first) / 100.0) * static_cast<double>(it->second);
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c)
                sum += (static_cast<double>(it->first) / 100.0) * static_cast<double>(it->second);
        }
        return sum;
    }
    // queue_ahead: total displayed shares resting AT a given price level (#317) —
    // the FIFO queue a new limit order at `price` would sit behind. 0 if no level at
    // that price. Queue-position estimation for passive orders: fill probability and
    // expected time-to-fill scale with how deep in the queue you join.
    int64_t queue_ahead(char side, double price) const noexcept {
        const int64_t px = to_ticks(price);
        if (side == 'B') {
            const auto it = bids_.find(px);
            return it != bids_.end() ? it->second : 0;
        }
        const auto it = asks_.find(px);
        return it != asks_.end() ? it->second : 0;
    }
    // largest_level_gap: the widest tick ($0.01) gap between ADJACENT occupied price
    // levels among the top N on a side (#325). The book is not contiguous — prices can
    // skip ticks — so once a sweep exhausts one level it jumps to the next OCCUPIED
    // price; this returns the worst such single-step dislocation within depth N. The
    // intra-side companion to spread_ticks (#207, the gap BETWEEN sides): a 1-tick
    // result is a dense, contiguous book, a large result an "air pocket" that makes the
    // touch fragile. 0 when fewer than two levels are in range (no gap to measure).
    int64_t largest_level_gap(char side, int n) const noexcept {
        if (n <= 1) return 0;
        int64_t maxgap = 0, prev = 0;
        int c = 0; bool have_prev = false;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) {
                const int64_t px = it->first;
                if (have_prev && prev - px > maxgap) maxgap = prev - px;
                prev = px; have_prev = true;
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) {
                const int64_t px = it->first;
                if (have_prev && px - prev > maxgap) maxgap = px - prev;
                prev = px; have_prev = true;
            }
        }
        return maxgap;
    }

    // book_slope: the order-book liquidity gradient on a side across the top n
    // occupied levels (Næs–Skjeltorp 2006) — cumulative displayed depth divided
    // by the price span it covers, in SHARES PER TICK ($0.01). High slope = depth
    // packed tightly near the touch (a resilient book that absorbs size with
    // little price travel); low slope = depth sparse and far from the touch
    // (fragile). Unlike depth_imbalance/notional_imbalance (pure qty), this
    // couples size to the price distance you must pay to reach it. 0 when fewer
    // than two levels are in range, or all collected depth sits at one price
    // (zero span — slope undefined).
    double book_slope(char side, int n) const noexcept {
        if (n <= 1) return 0.0;
        int64_t cum = 0, best_px = 0, last_px = 0;
        int c = 0; bool have_first = false;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) {
                if (!have_first) { best_px = it->first; have_first = true; }
                last_px = it->first;
                cum += it->second;
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) {
                if (!have_first) { best_px = it->first; have_first = true; }
                last_px = it->first;
                cum += it->second;
            }
        }
        if (c < 2) return 0.0;
        const int64_t span = (side == 'B') ? (best_px - last_px) : (last_px - best_px);
        if (span <= 0) return 0.0;
        return static_cast<double>(cum) / static_cast<double>(span);
    }

    // book_slope_imbalance: which side's near-touch liquidity builds up faster,
    // in [-1, 1] = (bid_slope - ask_slope)/(bid_slope + ask_slope) over the top n
    // levels. >0 = the bid side is steeper (denser support stacking below the
    // touch), <0 = the ask side is steeper. A structural companion to imbalance()
    // (which weighs only top-of-book qty): this weighs depth against price
    // distance. 0 when neither side has a measurable slope.
    double book_slope_imbalance(int n) const noexcept {
        const double bs = book_slope('B', n);
        const double as = book_slope('S', n);
        const double tot = bs + as;
        return tot > 0.0 ? (bs - as) / tot : 0.0;
    }

    size_t  bid_levels()     const noexcept { return bids_.size(); }
    size_t  ask_levels()     const noexcept { return asks_.size(); }
    size_t  resting_orders() const noexcept { return orders_.size(); }
    int64_t total_bid_qty()  const noexcept { int64_t s = 0; for (auto& l : bids_) s += l.second; return s; }
    int64_t total_ask_qty()  const noexcept { int64_t s = 0; for (auto& l : asks_) s += l.second; return s; }

    // --- Feed-handler statistics ---
    uint64_t adds()     const noexcept { return adds_; }
    uint64_t executes() const noexcept { return executes_; }
    uint64_t cancels()  const noexcept { return cancels_; }
    uint64_t deletes()  const noexcept { return deletes_; }
    uint64_t replaces() const noexcept { return replaces_; }
    uint64_t orphans()  const noexcept { return orphans_; }   // desync signal / feed gaps
};

}  // namespace itch
