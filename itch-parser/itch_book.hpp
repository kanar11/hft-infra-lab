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
    int64_t exec_shares_sum_     = 0;  // tape: total executed shares (#407)
    int64_t exec_notional_ticks_ = 0;  // tape: Σ shares × resting price_ticks (#407)
    int64_t exec_against_bid_    = 0;  // tape: shares hit on resting BIDS = seller-initiated (#415)
    int64_t exec_against_ask_    = 0;  // tape: shares lifted from ASKS = buyer-initiated (#415)
    int64_t max_cum_delta_       = 0;  // session peak of cumulative_delta (#535)
    int64_t min_cum_delta_       = 0;  // session trough of cumulative_delta (#535)
    int64_t  reprice_ticks_sum_  = 0;  // Σ |new - old| price ticks over applied replaces (#431)
    uint64_t repriced_           = 0;  // replaces that actually found their order (#431)
    int64_t  max_reprice_ticks_  = 0;  // largest single |new - old| reprice move (#519)
    uint64_t exec_prints_        = 0;  // executions that hit a resting order (#463)
    uint32_t largest_print_      = 0;  // biggest single trade print, shares (#503)
    int64_t  last_trade_ticks_   = 0;  // price of the most recent print (#471; 0 = none)
    bool     last_trade_buy_     = false; // aggressor of the last print (#471)
    int      last_tick_dir_      = 0;  // uptick/downtick carry (#479; SSR zero-plus rule)
    int      aggressor_run_      = 0;  // signed run of same-aggressor prints (#487)
    int      max_buy_run_        = 0;  // longest buyer-initiated sweep this session (#511)
    int      max_sell_run_       = 0;  // longest seller-initiated sweep this session (#511)

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
    void on_execute(int64_t ref, uint32_t exec_shares) noexcept {
        // Tape accounting (#407): an ITCH execution happens at the RESTING
        // order's price, which only orders_ knows — record it before
        // reduce_ (mirroring its clamp so an over-execute counts only the
        // truly resting part; an unknown ref leaves the tape untouched).
        auto it = orders_.find(ref);
        if (it != orders_.end()) {
            const uint32_t dec = (exec_shares < it->second.shares)
                                     ? exec_shares : it->second.shares;
            exec_shares_sum_     += dec;
            exec_notional_ticks_ += static_cast<int64_t>(dec) * it->second.price_ticks;
            // #415: the resting side names the aggressor exactly — a hit
            // resting BID was sold into, a lifted ASK was bought from.
            if (it->second.side == 'B') exec_against_bid_ += dec;
            else                        exec_against_ask_ += dec;
            // #535: track the running CVD high/low water marks.
            const int64_t cd = exec_against_ask_ - exec_against_bid_;
            if (cd > max_cum_delta_) max_cum_delta_ = cd;
            if (cd < min_cum_delta_) min_cum_delta_ = cd;
            ++exec_prints_;   // #463: a real trade print (orphans excluded)
            if (dec > largest_print_) largest_print_ = dec;   // #503: block detector
            // #471: the tape's last print — a hit resting BID was SOLD into
            // (aggressor sell), a lifted ASK was BOUGHT from (aggressor buy).
            const int64_t new_px = it->second.price_ticks;
            // #479: the price tick test with the SSR zero-plus/zero-minus
            // carry — a print above the last DIFFERENT price is +1, below is
            // -1, and a same-price print keeps the prior direction (the
            // uptick rule treats a zero-plus-tick as an uptick). Only from
            // the second print on (needs a prior price to compare).
            if (last_trade_ticks_ != 0) {
                if      (new_px > last_trade_ticks_) last_tick_dir_ = 1;
                else if (new_px < last_trade_ticks_) last_tick_dir_ = -1;
                // equal price -> carry the previous direction
            }
            last_trade_ticks_ = new_px;
            last_trade_buy_   = (it->second.side == 'S');
            // #487: the aggressor run — consecutive prints on the same side.
            // A hit resting BID is seller-initiated, a lifted ASK is buyer-
            // initiated; same side extends the run, a flip restarts it at +-1.
            if      (aggressor_run_ > 0 &&  last_trade_buy_) ++aggressor_run_;
            else if (aggressor_run_ < 0 && !last_trade_buy_) --aggressor_run_;
            else                                             aggressor_run_ = last_trade_buy_ ? 1 : -1;
            // #511: high-water marks of the sweep length, per side.
            if      (aggressor_run_ >  max_buy_run_)  max_buy_run_  =  aggressor_run_;
            else if (-aggressor_run_ > max_sell_run_) max_sell_run_ = -aggressor_run_;
        }
        reduce_(ref, exec_shares);
        ++executes_;
    }
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
        // #431: reprice distance — how far the order chased. Captured here
        // because only this moment has BOTH prices side by side.
        const int64_t old_px = it->second.price_ticks;
        const int64_t new_px = to_ticks(new_price);
        const int64_t move = (new_px > old_px) ? new_px - old_px : old_px - new_px;
        reprice_ticks_sum_ += move;
        if (move > max_reprice_ticks_) max_reprice_ticks_ = move;   // #519
        ++repriced_;
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
        exec_shares_sum_ = exec_notional_ticks_ = 0;   // #407
        exec_against_bid_ = exec_against_ask_ = 0;     // #415
        max_cum_delta_ = min_cum_delta_ = 0;           // #535
        reprice_ticks_sum_ = 0; repriced_ = 0;         // #431
        max_reprice_ticks_ = 0;                        // #519
        exec_prints_ = 0;                              // #463
        largest_print_ = 0;                            // #503
        last_trade_ticks_ = 0; last_trade_buy_ = false; // #471
        last_tick_dir_ = 0;                             // #479
        aggressor_run_ = 0;                             // #487
        max_buy_run_ = 0; max_sell_run_ = 0;            // #511
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

    // notional_within (#527): the total $ NOTIONAL resting within `ticks` of the
    // touch on a side = sum(price * qty) over levels no more than `ticks` away
    // from the best. The dollar companion to liquidity_within (#164, which sums
    // SHARES over the same tick window): capital defends a price, not share
    // count, so a thin band of a high-priced name can outweigh a fat band of a
    // penny name. Distinct from depth_notional (#309), which walks a fixed
    // LEVEL COUNT rather than a price DISTANCE — within a tick band the two
    // diverge when levels are unevenly spaced. 0 when the side is empty or
    // ticks < 0.
    double notional_within(char side, int ticks) const noexcept {
        if (ticks < 0) return 0.0;
        double sum = 0.0;
        if (side == 'B') {
            if (bids_.empty()) return 0.0;
            const int64_t floor = bids_.rbegin()->first - ticks;
            for (auto it = bids_.rbegin(); it != bids_.rend() && it->first >= floor; ++it)
                sum += (static_cast<double>(it->first) / 100.0) * static_cast<double>(it->second);
        } else {
            if (asks_.empty()) return 0.0;
            const int64_t ceil = asks_.begin()->first + ticks;
            for (auto it = asks_.begin(); it != asks_.end() && it->first <= ceil; ++it)
                sum += (static_cast<double>(it->first) / 100.0) * static_cast<double>(it->second);
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

    // spread_at_size: the round-trip spread ($) actually paid to sweep `shares`
    // on BOTH sides (#366) = price_to_fill('B', shares) - price_to_fill('S',
    // shares) — the worst ask level a buy of that size reaches minus the worst
    // bid level a sell of that size reaches. At small size (fits at the touch)
    // this equals the quoted spread; as size grows past the top levels it WIDENS,
    // capturing the depth cost the touch spread (#135) hides. The size-aware
    // companion to spread(). 0 when either side can't cover `shares`.
    double spread_at_size(int64_t shares) const noexcept {
        const double buy  = price_to_fill('B', shares);   // worst ask reached
        const double sell = price_to_fill('S', shares);   // worst bid reached
        return (buy > 0.0 && sell > 0.0) ? buy - sell : 0.0;
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
    // depth_concentration: the share of a side's TOTAL displayed depth
    // sitting in its top N levels (#439) = cumulative_qty(side, n) /
    // total_shares(side), in (0, 1]. The book's SHAPE in one number:
    // near 1 with many levels resting behind means top-heavy — the touch
    // carries everything and one sweep leaves the side hollow; low values
    // mean the touch is the tip of a deep book and refills are coming.
    // Complements book_slope (#334, the price-axis gradient) with a
    // mass-fraction view. 0 when the side is empty.
    double depth_concentration(char side, int n) const noexcept {
        const int64_t total = total_shares(side);
        if (total <= 0) return 0.0;
        return static_cast<double>(cumulative_qty(side, n))
             / static_cast<double>(total);
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

    // executed_shares (#407): total shares that actually TRADED against
    // resting orders — the tape volume, as opposed to executes() which
    // counts execution EVENTS. Cancels/deletes remove liquidity without
    // trading and never touch this.
    int64_t executed_shares() const noexcept { return exec_shares_sum_; }

    // executed_vwap (#407): the session tape VWAP in $, computed at the
    // RESTING orders' prices (ITCH executions print at the maker's price).
    // The realized benchmark to compare fills against — vwap_depth (#155)
    // prices the BOOK as it stands, this prices what actually TRADED.
    // 0 before any execution.
    double executed_vwap() const noexcept {
        return exec_shares_sum_ > 0
            ? (static_cast<double>(exec_notional_ticks_)
               / static_cast<double>(exec_shares_sum_)) / 100.0
            : 0.0;
    }

    // last_trade_price (#471): the price of the most recent execution — the
    // tape's last print, distinct from the book's mid/best (those are where
    // you COULD trade, this is where someone DID). 0 before any print.
    double last_trade_price() const noexcept {
        return static_cast<double>(last_trade_ticks_) / 100.0;
    }
    // last_trade_is_buy (#471): the aggressor of the last print — true when
    // an ask was lifted (buyer-initiated), false when a bid was hit
    // (seller-initiated). The single-print tick test: pairs with the last
    // price to classify the most recent trade's direction without a
    // rolling window (tape_imbalance #415 is the session aggregate).
    bool last_trade_is_buy() const noexcept { return last_trade_buy_; }

    // tick_direction (#479): the PRICE tick test with the SSR zero-plus/
    // zero-minus carry — +1 if the last print was above the previous
    // different price (uptick), -1 if below (downtick), and a same-price
    // print keeps the prior sign (a zero-plus-tick counts as an uptick,
    // the SEC Rule 201 short-sale convention). Distinct from
    // last_trade_is_buy (#471), which reads the AGGRESSOR from the resting
    // side: the tick test is a price-history fallback (Lee-Ready uses it
    // when the quote-based side is ambiguous) and can disagree with the
    // aggressor on a print that moves the price the "wrong" way. 0 before
    // the second print (no prior price to tick against).
    int tick_direction() const noexcept { return last_tick_dir_; }

    // aggressor_run (#487): the current streak of same-aggressor prints —
    // +N for N consecutive buyer-initiated trades (asks lifted), -N for N
    // seller-initiated (bids hit), restarting at +-1 when the aggressor
    // flips. A live order-flow read: a long positive run is sustained
    // buying pressure (someone working a large buy across the offers, an
    // iceberg being swept), where tape_imbalance (#415) is the session
    // aggregate. 0 before any print.
    int aggressor_run() const noexcept { return aggressor_run_; }

    // longest_buy_run / longest_sell_run (#511): the high-water marks of the
    // aggressor run (#487) — the longest sustained BUYER-initiated sweep (asks
    // lifted in a row) and SELLER-initiated sweep (bids hit in a row) seen this
    // session, both as positive magnitudes. aggressor_run() is the LIVE streak,
    // which resets on every flip; these remember the worst sweep even after it
    // ended — the sweep/iceberg severity a post-session review checks, the tape
    // parallel of risk's max_consec_wins / max_consec_losses (#405/#364). The
    // pair also localizes one-sided pressure: a long buy run with a tiny sell
    // run is a persistently bought name. 0 before any print / any run on a side.
    int longest_buy_run()  const noexcept { return max_buy_run_; }
    int longest_sell_run() const noexcept { return max_sell_run_; }

    // is_uptick (#479): true when the last price tick was up (or a carried
    // zero-plus-tick) — the SSR short-sale gate (a short at or below the
    // bid is only allowed on an uptick).
    bool is_uptick() const noexcept { return last_tick_dir_ > 0; }

    // trade_prints (#463): the number of executions that actually hit a
    // resting order — real trade prints, orphaned executes (unknown ref)
    // excluded, unlike executes() which counts every execute event.
    uint64_t trade_prints() const noexcept { return exec_prints_; }

    // largest_trade_size (#503): the biggest single trade print in shares —
    // a block detector on the tape, the MAX companion to avg_trade_size
    // (#463, the mean), the tape parallel of largest_resting_order (#391)
    // for the book. Their ratio (largest / avg) is block dominance: a huge
    // largest over a small average is one block crossing amid retail-size
    // slicing. An over-execute counts only its truly-resting (clamped)
    // part. 0 before any print.
    uint32_t largest_trade_size() const noexcept { return largest_print_; }

    // avg_trade_size (#463): mean shares per trade print = executed_shares
    // (#407) / trade_prints. The tape's typical clip: small means the tape
    // is dominated by algo slicing (many odd lots, information leakage),
    // large means block activity. The tape analog of the book-side clip
    // reads (order_count_at #447, largest_resting_order #391) and the
    // itch parallel of router avg_route_size (#456). 0 before any print.
    double avg_trade_size() const noexcept {
        return exec_prints_ > 0
            ? static_cast<double>(exec_shares_sum_) / static_cast<double>(exec_prints_)
            : 0.0;
    }

    // executed_against_bid / executed_against_ask (#415): the tape volume
    // split by the RESTING side — an exact aggressor classification, free
    // with L3 data (a hit bid was SOLD into, a lifted ask was BOUGHT from),
    // where trade-only feeds must infer it (Lee-Ready). The two sum to
    // executed_shares (#407).
    int64_t executed_against_bid() const noexcept { return exec_against_bid_; }
    int64_t executed_against_ask() const noexcept { return exec_against_ask_; }

    // cumulative_delta (#495): the running signed net aggressor volume =
    // executed_against_ask - executed_against_bid (buyer-initiated shares
    // minus seller-initiated), the classic CVD order-flow indicator. Where
    // tape_imbalance (#415) is the RATIO in [-1,1], this is the LEVEL in
    // shares: a positive and rising CVD is net buying accumulating over the
    // session regardless of how balanced any single burst was, and a CVD
    // that diverges from price (price up, CVD flat/down) is the classic
    // absorption/exhaustion tell. 0 before any execution and on a
    // perfectly balanced tape.
    int64_t cumulative_delta() const noexcept {
        return exec_against_ask_ - exec_against_bid_;
    }

    // max_cumulative_delta / min_cumulative_delta (#535): the session HIGH and
    // LOW water marks of cumulative_delta (#495) — the peak net buyer-initiated
    // accumulation and the peak net seller-initiated distribution reached this
    // session, both anchored at the session-start 0 (so max >= 0 >= min).
    // cumulative_delta() is the CVD right NOW; these remember its extremes, the
    // reference points order-flow DIVERGENCE is read against: price making a new
    // high while CVD sits below max_cumulative_delta is the classic absorption/
    // exhaustion tell (buyers no longer following price up), and a CVD that
    // reclaims its max confirms the move. The tape-flow parallel of the depth
    // high-water reads (largest_level #385, largest_trade_size #503). 0 before
    // any print; reset by clear().
    int64_t max_cumulative_delta() const noexcept { return max_cum_delta_; }
    int64_t min_cumulative_delta() const noexcept { return min_cum_delta_; }

    // tape_imbalance (#415): net aggressor pressure in [-1,1] =
    // (bought - sold) / total executed. +1 = every share was buyer-
    // initiated (asks lifted), -1 = pure selling into bids, 0 = balanced
    // two-way tape. The EXECUTED-flow counterpart of the RESTING-depth
    // imbalance (#148) — depth shows intent, the tape shows commitment.
    // 0 before any execution.
    double tape_imbalance() const noexcept {
        const int64_t total = exec_against_bid_ + exec_against_ask_;
        return total > 0
            ? static_cast<double>(exec_against_ask_ - exec_against_bid_)
                  / static_cast<double>(total)
            : 0.0;
    }

    // avg_reprice_ticks (#431): the mean |price move| of an applied replace,
    // in ticks — quote-chasing intensity. replaces() says HOW OFTEN quotes
    // move, this says HOW FAR: near-zero means size-only amendments and
    // gentle repricing, a rising average means the crowd is chasing a
    // running market (or repricing away from it in fear). Orphaned replaces
    // carry no old price and are excluded. 0 before any applied replace.
    double avg_reprice_ticks() const noexcept {
        return repriced_ > 0
            ? static_cast<double>(reprice_ticks_sum_) / static_cast<double>(repriced_)
            : 0.0;
    }

    // max_reprice_ticks (#519): the LARGEST single |new - old| price move of an
    // applied replace, in ticks — the tail companion to avg_reprice_ticks
    // (#431, the mean). A book that gently amends size all session hides one
    // violent reprice in a near-zero average; this surfaces it. The biggest
    // single quote jump is the actionable read: a maker yanking a quote far
    // from the market (a fast repricing away from a runaway print, or a stale
    // quote snapping to a gapped open), the reprice analog of largest_trade_
    // size (#503) on the tape and max_reorder_depth (#370) on the feed.
    // Orphaned replaces carry no old price and are excluded. 0 before any
    // applied replace.
    int64_t max_reprice_ticks() const noexcept { return max_reprice_ticks_; }

    // ref_event_count (#399): how many ref-based events (execute/cancel/
    // delete/replace) the book has processed — including the ones that
    // orphaned. Adds are keyed by a NEW ref, so they cannot orphan and are
    // excluded. The denominator for orphan_rate below.
    uint64_t ref_event_count() const noexcept {
        return executes_ + cancels_ + deletes_ + replaces_;
    }

    // orphan_rate (#399): the fraction of ref-based events that referenced
    // an UNKNOWN order = orphans / ref_event_count, in [0,1]. The feed-
    // health ratio behind the raw orphans() counter: a non-zero rate means
    // the book missed adds (multicast gap, late join) and its picture is
    // incomplete — pair it with audit_book (#383), which catches the
    // opposite failure (events that silently CORRUPTED a known order).
    // 0 before any ref-based event.
    double orphan_rate() const noexcept {
        const uint64_t total = ref_event_count();
        return total > 0
            ? static_cast<double>(orphans_) / static_cast<double>(total)
            : 0.0;
    }

    // participant_imbalance (#455): resting-ORDER-count imbalance in [-1,1]
    // = (bid orders - ask orders) / total resting orders. depth_imbalance
    // (#148) weighs SHARES, so one whale dominates it; this counts HEADS —
    // many small orders stacking one side is the crowd's opinion, and the
    // two diverging tells who is leaning: +imbalance here with -imbalance
    // in shares = a retail herd against one institutional block. Built on
    // resting_order_count (#374). 0 when the book is empty or balanced.
    double participant_imbalance() const noexcept {
        const std::size_t b = resting_order_count('B');
        const std::size_t a = resting_order_count('S');
        const std::size_t total = b + a;
        return total > 0
            ? (static_cast<double>(b) - static_cast<double>(a))
                  / static_cast<double>(total)
            : 0.0;
    }

    // order_count_at (#447): how many DISTINCT orders rest at a price — the
    // queue length in PARTICIPANTS. queue_ahead (#317) gives the SHARES in
    // front of a would-be joiner; this gives the head count: ten 100-lots
    // and one 1000-lot are the same shares but very different queues (each
    // order ahead is a separate fill event to wait through, and a separate
    // trader who may cancel). With qty_at (total size) and
    // largest_resting_order (#391, biggest clip) it fully shapes a level:
    // size / clip / head count. 0 when nothing rests there.
    std::size_t order_count_at(char side, double price) const noexcept {
        const int64_t px = to_ticks(price);
        std::size_t c = 0;
        for (const auto& kv : orders_) {
            const Resting& r = kv.second;
            if (r.side == side && r.price_ticks == px) ++c;
        }
        return c;
    }

    // largest_resting_order (#391): the single biggest order resting on a
    // side — the "wall". One institutional-size clip is a very different
    // book than many retail-size clips at the same total depth: it signals
    // committed interest and defines the level to lean on (or not to cross).
    // Returns its share count and writes the price it rests at into
    // out_price (left untouched when the side is empty). The MAX companion
    // to avg_resting_order_size (#374, the mean) — their ratio is block
    // dominance. 0 when the side has no resting orders.
    uint32_t largest_resting_order(char side, double* out_price = nullptr) const noexcept {
        uint32_t mx = 0;
        int64_t  px = 0;
        for (const auto& kv : orders_) {
            const Resting& r = kv.second;
            if (r.side != side) continue;
            if (r.shares > mx) { mx = r.shares; px = r.price_ticks; }
        }
        if (mx > 0 && out_price != nullptr) *out_price = static_cast<double>(px) / 100.0;
        return mx;
    }

    // largest_level (#423): the price level carrying the most AGGREGATE
    // shares on a side — the crowd's wall, as opposed to
    // largest_resting_order (#391), the single biggest clip: a level can be
    // the thickest in the book through a hundred small orders that no
    // per-order view flags. Returns the aggregate shares and writes the
    // level's price into out_price (untouched when the side is empty).
    // Comparing the two walls tells WHO built the level: #391 close to
    // #423 = one institution, far below = retail accumulation.
    int64_t largest_level(char side, double* out_price = nullptr) const noexcept {
        const auto& book = (side == 'B') ? bids_ : asks_;
        int64_t mx = 0, px = 0;
        for (const auto& kv : book) {
            if (kv.second > mx) { mx = kv.second; px = kv.first; }
        }
        if (mx > 0 && out_price != nullptr) *out_price = static_cast<double>(px) / 100.0;
        return mx;
    }

    // audit_book (#383): cross-checks the two parallel structures the feed
    // handler maintains — the per-order map (orders_) and the per-level
    // aggregates (bids_/asks_). Every event updates both in lockstep, so any
    // divergence means a handler bug or a corrupted feed (e.g. an ADD reusing
    // a LIVE order_ref silently overwrites the order while double-counting
    // its level). Returns 0 when consistent, else the first failed invariant:
    //   1/2 = a non-positive bid/ask level survived (should have been erased)
    //   3/4 = bid/ask levels do not equal the sum of resting orders
    //   5   = a zero-share resting order survived
    // O(n log n) and allocates — a diagnostic for gap-recovery/reconnect
    // validation, not a hot-path call. Same role as FullOrderBook's
    // audit_book_integrity (which caught 5 real accounting bugs).
    int audit_book() const {
        for (const auto& kv : bids_) if (kv.second <= 0) return 1;
        for (const auto& kv : asks_) if (kv.second <= 0) return 2;
        std::map<int64_t, int64_t> b, a;
        for (const auto& kv : orders_) {
            const Resting& r = kv.second;
            if (r.shares == 0) return 5;
            (r.side == 'B' ? b : a)[r.price_ticks] += static_cast<int64_t>(r.shares);
        }
        if (b != bids_) return 3;
        if (a != asks_) return 4;
        return 0;
    }

    // cancel_to_add_ratio: cancels / adds (#350) — a classic market-microstructure
    // flow-quality signal. High (>>1) means most resting interest is pulled before
    // it trades: fleeting liquidity / quote stuffing, common ahead of a toxic
    // print. Low (near 0) means orders mostly stick around to trade or expire
    // rather than getting cancelled. 0 when no adds have been seen yet.
    double cancel_to_add_ratio() const noexcept {
        return adds_ > 0 ? static_cast<double>(cancels_) / static_cast<double>(adds_) : 0.0;
    }
    // execute_to_add_ratio: executes / adds (#358) — the companion signal to
    // cancel_to_add_ratio (#350). Where that shows how much added liquidity gets
    // PULLED, this shows how much TRADES. Not literally "1 - cancel_to_add_ratio":
    // adds/cancels/executes are independent event streams (one add can generate
    // several partial-fill executes, or none if fully cancelled), so the two
    // ratios can both be high or both be low. 0 when no adds have been seen yet.
    double execute_to_add_ratio() const noexcept {
        return adds_ > 0 ? static_cast<double>(executes_) / static_cast<double>(adds_) : 0.0;
    }

    // resting_order_count / avg_resting_order_size: how many INDIVIDUAL resting
    // orders sit on a side and their mean size (#374) = total_shares(side) /
    // count. resting_orders() gives the both-sides total and total_shares (#174)
    // the per-side share sum; the per-side ORDER count and mean size distinguish
    // the same depth built from many small clips (fragmented / retail flow, thin
    // queue priority per order) vs a few large blocks (institutional resting
    // interest). O(orders) walk — a periodic diagnostic, not a hot-path read.
    size_t resting_order_count(char side) const noexcept {
        size_t c = 0;
        for (const auto& [ref, r] : orders_) if (r.side == side) ++c;
        return c;
    }
    double avg_resting_order_size(char side) const noexcept {
        int64_t shares = 0; size_t c = 0;
        for (const auto& [ref, r] : orders_)
            if (r.side == side) { shares += r.shares; ++c; }
        return c > 0 ? static_cast<double>(shares) / static_cast<double>(c) : 0.0;
    }
};

}  // namespace itch
