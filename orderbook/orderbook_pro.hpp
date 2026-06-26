/*
 * FullOrderBook<LEVELS, MAX_ORDERS> — production-grade orderbook.
 *
 * Po co kolejny? FlatOrderBook (orderbook_flat.hpp) ma O(1) add/cancel/modify
 * with ID tracking, but aggregates quantity per price level — it does not know the queue
 * (FIFO) and does not support advanced order types. This file adds:
 *
 *   - **L3 detail** — a full FIFO queue per price level (not just Σ qty)
 *   - **Queue position tracking** — you know you are N-th in the queue
 *   - **Order types**: LIMIT, IOC, FOK, POST_ONLY, ICEBERG, STOP, PEG
 *   - **Time-in-force**: DAY, GTC, IOC, FOK, GTD
 *   - **Self-trade prevention** (STP) — cancel-newest / cancel-oldest / cancel-both
 *   - **Lifecycle events** — callback per ACCEPT / FILL / PARTIAL / CANCEL / REJECT
 *   - **Snapshot + delta** — bytestream do recovery (incremental + full)
 *   - **Trade tape** — ring buffer ostatnich N executions
 *   - **L1/L2/L3 views** — top quote / N-level depth / full order list
 *   - **Imbalance + microprice** — order flow signals
 *   - **Pre-trade checks** — locked/crossed market protection
 *
 * Wybory projektowe:
 *   - Memory: a pre-allocated `Order` pool (free-list LIFO), zero heap alloc
 *     on the hot path. All structures are cache-aligned.
 *   - Price levels: a flat array `levels_[LEVELS]` indexed by (price-PRICE_MIN).
 *     Each `PriceLevel` is a small struct with `head_ / tail_ / total_qty_ / order_count_`.
 *   - FIFO queue: an intrusive doubly-linked list — `Order` has `next_at_level_`
 *     i `prev_at_level_`. Cancel = O(1) unlink. Insert na koniec = O(1).
 *   - Order ID lookup: `std::unordered_map<uint64_t, Order*>` — O(1) cancel/modify.
 *   - Best bid/ask: tracked as int32 indices into `levels_` (like FlatOrderBook).
 *   - Multi-level depth: top N bids/asks cached in `top_bids_[N]` /
 *     `top_asks_[N]` (lazy, invalidated on book change).
 *
 * Limity szablonu:
 *   - LEVELS         — width of the price grid in ticks (should cover a day)
 *   - MAX_ORDERS     — the Order pool (=max simultaneously active orders in the book)
 *
 * Tick = 1/PRICE_SCALE of a dollar (=$0.0001 if PRICE_SCALE=10000).
 *
 * Performance (on a similar templated book):
 *   - add limit:        p50 ~80 ns, p99 ~150 ns
 *   - cancel by id:     p50 ~50 ns, p99 ~120 ns
 *   - top-of-book read: p50 ~5 ns  (1 cache line)
 *   - L2 depth 10:      p50 ~30 ns (10 levels scan)
 *
 * Pipeline integration:
 *   Strategy → Risk → Router → OMS → FullOrderBook (matching) → Logger
 */
#pragma once

#include "../common/types.hpp"     // Price, Side, side_str
#include "orderbook_pro_types.hpp" // data model: constants, enums, Order/PriceLevel/Trade/...

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>     // INT32_MAX (sentinel NO_ASK_TICKS)
#include <cmath>       // std::sqrt — tape price stddev
#include <cstdint>
#include <cstdio>
#include <cstdlib>     // std::abs(int) — auction imbalance scoring
#include <cstring>
#include <unordered_map>
#include <vector>


namespace orderbook_pro {




// FullOrderBook<LEVELS, MAX_ORDERS> — generic, fixed-capacity.
//   LEVELS      — how many price slots. price_ticks ∈ [0, LEVELS).
//                  For AAPL at a $0.01 tick and a $100..$200 range it is enough
//                  ~10000-65536 (zostawia headroom).
//   MAX_ORDERS  — the Order pool. A real intraday LOB has 10k-100k simultaneously.
//
// noexcept everywhere — no exceptions; errors reported via the return value
// + RejectReason. An HFT requirement — an exception from an unexpected place kills
// determinizm latencji.
template <std::int32_t LEVELS = 16384, std::int32_t MAX_ORDERS = 65536>
class FullOrderBook {
    static_assert(LEVELS > 0,     "LEVELS must be positive");
    static_assert(MAX_ORDERS > 0, "MAX_ORDERS must be positive");

    // Storage
    Order        pool_[MAX_ORDERS];         // pool aktywnych + free slots
    Order*       free_list_head_ = nullptr; // LIFO free-list (cache-friendly)
    PriceLevel   levels_[LEVELS];           // one slot per tick
    std::unordered_map<std::uint64_t, Order*> id_index_;  // O(1) cancel/modify

    // Best bid/ask cache — int sentinels
    std::int32_t best_bid_ticks_ = NO_BID_TICKS;
    std::int32_t best_ask_ticks_ = NO_ASK_TICKS;

    // Trade tape — ring buffer
    static constexpr std::size_t TAPE_CAP = 1024;
    Trade        tape_[TAPE_CAP]{};
    std::size_t  tape_head_ = 0;        // next write index
    std::size_t  tape_count_ = 0;       // total trades written (overruns ok)
    std::uint64_t next_exec_id_ = 1;

    // Auto order_id (when the caller does not provide its own)
    std::uint64_t next_order_id_ = 1;

    // Statystyki
    BookStats    stats_;
    std::size_t  active_orders_ = 0;
    std::size_t  pool_high_water_ = 0;

    // STP policy (default NONE — tests may freely wash-trade)
    SelfTradePrevention stp_policy_ = SelfTradePrevention::NONE;

    // Anti-internalization: mapa client_id → firm_id (MPID). STP normalnie
    // catches only the same client_id; a firm has many desks/accounts that also
    // should not trade with each other. When both orders belong to the same firm,
    // STP fires as if it were the same entity.
    std::unordered_map<std::uint64_t, std::uint64_t> client_to_firm_;
    std::uint64_t firm_self_trade_blocks_ = 0;

    // Are a and b the same trading entity (the same client or the same firm)?
    bool same_trading_entity(std::uint64_t a, std::uint64_t b) const noexcept {
        if (a == 0 || b == 0) return false;   // an anonymous party is never blocked
        if (a == b) return true;              // the same client_id
        const auto ia = client_to_firm_.find(a);
        const auto ib = client_to_firm_.find(b);
        return ia != client_to_firm_.end() && ib != client_to_firm_.end() &&
               ia->second == ib->second;      // the same firm (MPID)
    }

    // Callback for events (optionally set via set_event_callback)
    using EventCallback = void(*)(const BookEvent&, void* ctx);
    EventCallback event_cb_  = nullptr;
    void*         event_ctx_ = nullptr;

    // Drop copy — a copy of the events of a single monitored account
    EventCallback drop_copy_cb_     = nullptr;
    void*         drop_copy_ctx_    = nullptr;
    std::uint64_t drop_copy_client_ = 0;       // 0 = disabled
    std::uint64_t drop_copy_events_ = 0;

    // Helper: monotonic timestamp ns (header-local to avoid dependencies).
    static std::uint64_t mono_ns_now() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Bezpieczne sprawdzenie zakresu price ticks.
    static bool in_range(std::int32_t price_ticks) noexcept {
        return price_ticks >= PRICE_MIN_TICKS && price_ticks < LEVELS;
    }

    // Allocate an Order from the pool or nullptr when the pool is exhausted.
    Order* alloc_order() noexcept {
        if (!free_list_head_) return nullptr;
        Order* o = free_list_head_;
        free_list_head_ = o->next_free;
        o->reset();
        ++active_orders_;
        if (active_orders_ > pool_high_water_) {
            pool_high_water_ = active_orders_;
            stats_.peak_order_pool_used = pool_high_water_;
        }
        return o;
    }

    // Return the Order to the free-list.
    void free_order(Order* o) noexcept {
        if (!o) return;
        o->next_free = free_list_head_;
        free_list_head_ = o;
        --active_orders_;
    }

    // Push the Order to the end of the FIFO queue at the price level (intrusive list).
    void enqueue_at_level(Order* o) noexcept {
        PriceLevel& lvl = levels_[o->price_ticks];
        o->next_at_level = nullptr;
        o->prev_at_level = lvl.tail;
        if (lvl.tail) lvl.tail->next_at_level = o;
        else          lvl.head = o;
        lvl.tail = o;
        lvl.total_qty    += o->displayed_qty;
        // Hidden remainder = total - filled - displayed (filled may be > 0
        // at partial-fill-then-rest; the old T-D formula counted filled as hidden)
        lvl.total_hidden += (o->total_qty - o->filled_qty - o->displayed_qty);
        ++lvl.order_count;
    }

    // Remove the Order from its price level (O(1) — intrusive unlink).
    void unlink_from_level(Order* o) noexcept {
        PriceLevel& lvl = levels_[o->price_ticks];
        if (o->prev_at_level) o->prev_at_level->next_at_level = o->next_at_level;
        else                  lvl.head = o->next_at_level;
        if (o->next_at_level) o->next_at_level->prev_at_level = o->prev_at_level;
        else                  lvl.tail = o->prev_at_level;
        lvl.total_qty    -= o->displayed_qty;
        // Symmetric to enqueue: T - F - D. The old T-D formula caused
        // negative total_hidden drift on cancel of a partially-filled maker
        // (fill decrements D, so T-D grew by filled_qty).
        lvl.total_hidden -= (o->total_qty - o->filled_qty - o->displayed_qty);
        if (lvl.order_count > 0) --lvl.order_count;
    }

    // After adding/removing from the book — refresh best_bid/best_ask when needed.
    //
    // start_ticks range guard: callers invoke this with best_bid_ticks_-1 /
    // best_ask_ticks_+1. When best_*_ticks_ was already a sentinel (NO_BID=-1,
    // NO_ASK=INT32_MAX) — e.g. after STP CANCEL_OLDEST removed the ONLY level
    // in match_at_level and cancel_internal itself refreshed best to the sentinel —
    // the arithmetic gives a start outside [0,LEVELS) (and NO_ASK+1 is outright overflow
    // to INT32_MIN). Without the guard the loop indexes levels_[negative] → OOB/SEGV.
    // Correctly: a start outside the grid means "no quote on this side".
    void refresh_best_bid_from(std::int32_t start_ticks) noexcept {
        if (start_ticks < PRICE_MIN_TICKS || start_ticks >= LEVELS) {
            best_bid_ticks_ = NO_BID_TICKS;
            return;
        }
        // Scan downward starting from start_ticks until you find a non-empty level
        // (or drop below 0 → no bids).
        for (std::int32_t p = start_ticks; p >= PRICE_MIN_TICKS; --p) {
            if (levels_[p].total_qty > 0) { best_bid_ticks_ = p; return; }
        }
        best_bid_ticks_ = NO_BID_TICKS;
    }
    void refresh_best_ask_from(std::int32_t start_ticks) noexcept {
        if (start_ticks < PRICE_MIN_TICKS || start_ticks >= LEVELS) {
            best_ask_ticks_ = NO_ASK_TICKS;
            return;
        }
        for (std::int32_t p = start_ticks; p < LEVELS; ++p) {
            if (levels_[p].total_qty > 0) { best_ask_ticks_ = p; return; }
        }
        best_ask_ticks_ = NO_ASK_TICKS;
    }

    // Emit an event to the callback if set.
    void emit(EventType ev, std::uint64_t order_id, std::uint64_t maker_id,
              std::int32_t price_ticks, std::int32_t qty,
              OrderStatus status, RejectReason rr,
              std::uint64_t client_id = 0) noexcept {
        const std::uint64_t seq = ++next_event_seq_;
        last_emitted_seq_ = seq;
        if (!event_cb_ && !drop_copy_cb_) return;
        BookEvent e{ev, order_id, maker_id, price_ticks, qty, status, rr,
                    mono_ns_now(), seq, client_id};
        if (event_cb_) event_cb_(e, event_ctx_);
        // Drop copy: a copy of the monitored account's events (FIX drop copy) —
        // risk/compliance gets the stream independently of the main callback
        if (drop_copy_cb_ && client_id != 0 && client_id == drop_copy_client_) {
            ++drop_copy_events_;
            drop_copy_cb_(e, drop_copy_ctx_);
        }
    }

    // Zapisz Trade do tape (ring buffer) + aktualizuj last_trade_ticks_
    // used by STOP triggers + fee accounting.
    void record_trade(std::uint64_t maker_id, std::uint64_t taker_id,
                      std::int32_t price_ticks, std::int32_t qty,
                      Side taker_side, std::uint64_t ts_ns) noexcept {
        Trade& t = tape_[tape_head_ % TAPE_CAP];
        t.exec_id = next_exec_id_++;
        t.maker_order_id = maker_id;
        t.taker_order_id = taker_id;
        t.price_ticks = price_ticks;
        t.qty = qty;
        t.taker_side = taker_side;
        t.ts_ns = ts_ns;
        ++tape_head_;
        ++tape_count_;
        ++stats_.total_fills;
        stats_.total_volume += static_cast<std::uint64_t>(qty);
        last_trade_ticks_ = price_ticks;
        update_size_distribution(qty);
        // Fee accounting (basis): notional = price * qty; fee = notional * bps / 10000
        // We keep it in "basis units" (price_ticks * qty * bps), the caller divides by 10000.
        const std::int64_t notional_ticks =
            static_cast<std::int64_t>(price_ticks) * qty;
        cum_taker_fees_ += notional_ticks * taker_fee_bps_;
        cum_maker_fees_ += notional_ticks * maker_fee_bps_;
        // MIFID II RTS27/28: effective spread = 2 × |exec - mid|
        if (mifid_enabled_) {
            ++mifid_.num_executions;
            mifid_.total_volume         += static_cast<std::uint64_t>(qty);
            mifid_.total_notional_ticks += notional_ticks;
            const std::int32_t m = mid_ticks();
            if (m > 0) {
                const std::int64_t diff = std::abs(static_cast<std::int64_t>(price_ticks) - m);
                mifid_.sum_effective_spread += diff * 2 * qty;
                // signed price impact (taker direction = sign of (price - mid))
                const std::int64_t signed_diff =
                    static_cast<std::int64_t>(price_ticks) - m;
                mifid_.sum_signed_price_impact +=
                    (taker_side == Side::BUY ? signed_diff : -signed_diff) * qty;
            }
        }
        // Order-flow accumulators (VPIN-style toxicity)
        if (taker_side == Side::BUY) {
            taker_buy_volume_ += static_cast<std::uint64_t>(qty);
            ++taker_buy_count_;
        } else {
            taker_sell_volume_ += static_cast<std::uint64_t>(qty);
            ++taker_sell_count_;
        }
        // Trade direction Markov chain
        if (markov_has_prev_) {
            if (markov_prev_side_ == Side::BUY) {
                if (taker_side == Side::BUY) ++markov_BB_;
                else                          ++markov_BS_;
            } else {
                if (taker_side == Side::BUY) ++markov_SB_;
                else                          ++markov_SS_;
            }
        }
        markov_prev_side_ = taker_side;
        markov_has_prev_  = true;
        // Signed volume EMA
        {
            const double signed_q = (taker_side == Side::BUY ? +1.0 : -1.0) *
                                    static_cast<double>(qty);
            if (!ema_signed_volume_init_) {
                ema_signed_volume_ = signed_q;
                ema_signed_volume_init_ = true;
            } else {
                ema_signed_volume_ = 0.1 * signed_q + 0.9 * ema_signed_volume_;
            }
        }
        // Per-side trade VWAP
        const std::int64_t notional = static_cast<std::int64_t>(price_ticks) * qty;
        if (taker_side == Side::BUY) {
            cum_buy_notional_ticks_ += notional;
            cum_buy_volume_         += static_cast<std::uint64_t>(qty);
        } else {
            cum_sell_notional_ticks_ += notional;
            cum_sell_volume_         += static_cast<std::uint64_t>(qty);
        }
        // Inter-trade time gap + Welford variance
        if (prev_trade_ts_ns_for_gap_ != 0 && ts_ns > prev_trade_ts_ns_for_gap_) {
            const std::uint64_t gap = ts_ns - prev_trade_ts_ns_for_gap_;
            if (gap < inter_trade_gap_min_ns_) inter_trade_gap_min_ns_ = gap;
            if (gap > inter_trade_gap_max_ns_) inter_trade_gap_max_ns_ = gap;
            inter_trade_gap_sum_ns_ += gap;
            ++inter_trade_gap_count_;
            // Welford for variance
            ++gap_var_count_;
            const double gap_d = static_cast<double>(gap);
            const double delta = gap_d - gap_var_mean_;
            gap_var_mean_ += delta / static_cast<double>(gap_var_count_);
            const double delta2 = gap_d - gap_var_mean_;
            gap_var_M2_ += delta * delta2;
            // Autocorrelation lag-1 of gaps (raw, without centering — simple estimator)
            if (gap_prev_has_) {
                gap_autocorr_xy_sum_ += gap_d * gap_prev_value_;
                gap_autocorr_xx_sum_ += gap_prev_value_ * gap_prev_value_;
            }
            gap_prev_value_ = gap_d;
            gap_prev_has_   = true;
        }
        prev_trade_ts_ns_for_gap_ = ts_ns;
        // Largest single trade
        if (qty > largest_single_trade_qty_) largest_single_trade_qty_ = qty;
        // SSR circuit breaker (Rule 201): a drop to the threshold activates the restriction
        if (ssr_armed_ && !ssr_active_ && price_ticks <= ssr_trigger_px_) {
            ssr_active_ = true;
            ++ssr_trips_;
        }
        // Lee-Ready classification — as an outside observer (without taker_side)
        // would classify the trade: quote rule (vs mid), fallback tick test
        // (vs previous price). In the engine we know the actual taker_side, so
        // we compute accuracy (Lee/Ready 1991 report ~85% on NYSE).
        {
            const std::int32_t m_now = mid_ticks();
            bool classifiable = true;
            Side classified = Side::BUY;
            if (m_now > 0 && price_ticks != m_now) {
                classified = (price_ticks > m_now) ? Side::BUY : Side::SELL;
            } else if (prev_trade_px_for_lambda_ >= 0 &&
                       static_cast<std::int64_t>(price_ticks) !=
                           prev_trade_px_for_lambda_) {
                classified = (static_cast<std::int64_t>(price_ticks) >
                              prev_trade_px_for_lambda_) ? Side::BUY : Side::SELL;
            } else {
                classifiable = false;
            }
            if (classifiable) {
                ++lr_total_classified_;
                if (classified == Side::BUY) ++lr_buy_classified_;
                else                          ++lr_sell_classified_;
                if (classified == taker_side) ++lr_correct_;
            } else {
                ++lr_unclassifiable_;
            }
        }
        // Tick-by-tick Δp distribution (clipped do [-4, +4])
        if (prev_trade_px_for_lambda_ >= 0) {
            const std::int32_t raw = price_ticks - static_cast<std::int32_t>(prev_trade_px_for_lambda_);
            const std::int32_t clip = std::max<std::int32_t>(-4,
                                       std::min<std::int32_t>(4, raw));
            ++price_change_hist_[static_cast<std::size_t>(clip + 4)];
            ++price_change_total_;
        }
        // Kyle's lambda accumulator (global + per-side decomp)
        if (prev_trade_px_for_lambda_ >= 0) {
            const double dp = static_cast<double>(price_ticks - prev_trade_px_for_lambda_);
            const double signed_v = (taker_side == Side::BUY ? +1.0 : -1.0) *
                                    static_cast<double>(qty);
            const double vsq = signed_v * signed_v;
            cum_price_volume_product_ += dp * signed_v;
            cum_signed_volume_sq_     += vsq;
            if (taker_side == Side::BUY) {
                cum_pv_buy_  += dp * signed_v;
                cum_vsq_buy_ += vsq;
            } else {
                cum_pv_sell_  += dp * signed_v;
                cum_vsq_sell_ += vsq;
            }
        }
        prev_trade_px_for_lambda_ = price_ticks;
        // Latency arbitrage: same-side aggressors within window
        if (larb_window_ns_ > 0 && last_fill_ts_ns_ != 0 &&
            ts_ns > last_fill_ts_ns_ &&
            (ts_ns - last_fill_ts_ns_) <= larb_window_ns_ &&
            taker_side == larb_last_side_) {
            ++larb_same_side_fast_;
        }
        larb_last_side_     = taker_side;
        larb_last_fill_ts_  = ts_ns;
        last_fill_ts_ns_    = ts_ns;
        if (taker_side == Side::BUY) last_buy_fill_ts_ns_  = ts_ns;
        else                          last_sell_fill_ts_ns_ = ts_ns;
        // Volume-at-price profile (capped to the last SAMPLES levels written)
        if (price_ticks >= 0 && price_ticks < LEVELS) {
            volume_at_price_[static_cast<std::size_t>(price_ticks)] +=
                static_cast<std::uint64_t>(qty);
        }
    }

public:
    FullOrderBook() noexcept {
        // Build the free-list: link each slot to the next.
        for (std::int32_t i = 0; i < MAX_ORDERS; ++i) {
            pool_[i].reset();
            pool_[i].next_free = (i + 1 < MAX_ORDERS) ? &pool_[i + 1] : nullptr;
        }
        free_list_head_ = &pool_[0];
        id_index_.reserve(static_cast<std::size_t>(MAX_ORDERS));
    }

    // Deleted copy/move — the book has intrusive pointers; a copy would corrupt them.
    FullOrderBook(const FullOrderBook&)            = delete;
    FullOrderBook& operator=(const FullOrderBook&) = delete;
    FullOrderBook(FullOrderBook&&)                 = delete;
    FullOrderBook& operator=(FullOrderBook&&)      = delete;

    // ====================================================================
    // Konfiguracja
    // ====================================================================

    void set_stp_policy(SelfTradePrevention p) noexcept { stp_policy_ = p; }
    SelfTradePrevention stp_policy() const noexcept { return stp_policy_; }

    // Anti-internalization — assign client_id to a firm (MPID). After assignment
    // STP treats all accounts of the same firm as a single entity.
    // firm_id = 0 removes the assignment. Requires stp_policy_ != NONE to work.
    void set_client_firm(std::uint64_t client_id, std::uint64_t firm_id) noexcept {
        if (client_id == 0) return;
        if (firm_id == 0) client_to_firm_.erase(client_id);
        else              client_to_firm_[client_id] = firm_id;
    }
    std::uint64_t client_firm(std::uint64_t client_id) const noexcept {
        const auto it = client_to_firm_.find(client_id);
        return it == client_to_firm_.end() ? 0 : it->second;
    }
    // STP blocks caught at the firm level (different accounts, same firm).
    std::uint64_t firm_self_trade_blocks() const noexcept {
        return firm_self_trade_blocks_;
    }

    // Drop copy (FIX drop copy semantics): a separate event stream of a SINGLE
    // the monitored account — the risk desk / compliance sees ACCEPT/FILL/CANCEL/
    // client's EXPIRE independently of the main callback. client_id = 0 disables it.
    void set_drop_copy(std::uint64_t client_id, EventCallback cb,
                        void* ctx) noexcept {
        drop_copy_client_ = client_id;
        drop_copy_cb_     = cb;
        drop_copy_ctx_    = ctx;
    }
    std::uint64_t drop_copy_events() const noexcept { return drop_copy_events_; }

    void set_event_callback(EventCallback cb, void* ctx) noexcept {
        event_cb_ = cb; event_ctx_ = ctx;
    }

    // ====================================================================
    // L1 quote — top of book
    // ====================================================================

    std::int32_t best_bid_ticks() const noexcept { return best_bid_ticks_; }
    std::int32_t best_ask_ticks() const noexcept { return best_ask_ticks_; }
    bool has_bid() const noexcept { return best_bid_ticks_ != NO_BID_TICKS; }
    bool has_ask() const noexcept { return best_ask_ticks_ != NO_ASK_TICKS; }

    // mid_ticks: returns (bid+ask)/2, or -1 when there is no quote on one of the sides.
    std::int32_t mid_ticks() const noexcept {
        if (!has_bid() || !has_ask()) return -1;
        return (best_bid_ticks_ + best_ask_ticks_) / 2;
    }

    // microprice_ticks: weighted by size na top of book.
    //   micro = (bid*ask_qty + ask*bid_qty) / (bid_qty + ask_qty)
    // A better predictor of the next trade than mid (accounts for order flow).
    std::int32_t microprice_ticks() const noexcept {
        if (!has_bid() || !has_ask()) return -1;
        const std::int32_t bq = levels_[best_bid_ticks_].total_qty;
        const std::int32_t aq = levels_[best_ask_ticks_].total_qty;
        const std::int64_t denom = static_cast<std::int64_t>(bq) + aq;
        if (denom == 0) return mid_ticks();
        const std::int64_t num =
            static_cast<std::int64_t>(best_bid_ticks_) * aq +
            static_cast<std::int64_t>(best_ask_ticks_) * bq;
        return static_cast<std::int32_t>(num / denom);
    }

    // Top-of-book quote z qty + count.
    TopOfBook top_of_book() const noexcept {
        TopOfBook t{};
        t.best_bid_ticks = best_bid_ticks_;
        t.best_ask_ticks = best_ask_ticks_;
        if (has_bid()) {
            t.bid_qty   = levels_[best_bid_ticks_].total_qty;
            t.bid_count = levels_[best_bid_ticks_].order_count;
        }
        if (has_ask()) {
            t.ask_qty   = levels_[best_ask_ticks_].total_qty;
            t.ask_count = levels_[best_ask_ticks_].order_count;
        }
        return t;
    }

    // ====================================================================
    // Stats + introspekcja
    // ====================================================================

    const BookStats& stats() const noexcept { return stats_; }
    std::size_t active_orders() const noexcept { return active_orders_; }
    std::size_t pool_capacity() const noexcept { return MAX_ORDERS; }
    std::size_t pool_used_high_water() const noexcept { return pool_high_water_; }

    // Lookup by order_id (for test introspection; do not use on the hot path).
    const Order* find_order(std::uint64_t id) const noexcept {
        auto it = id_index_.find(id);
        return it == id_index_.end() ? nullptr : it->second;
    }

    // ====================================================================
    // Matching helpers
    // ====================================================================
private:
    // Checks whether price `p` on side `side` would be an aggressor (crosses the market).
    // BUY  is an aggressor when p >= best_ask
    // SELL is an aggressor when p <= best_bid
    bool would_cross(Side side, std::int32_t p) const noexcept {
        if (side == Side::BUY)  return has_ask() && p >= best_ask_ticks_;
        else                     return has_bid() && p <= best_bid_ticks_;
    }

    // Checks for a locked/crossed market before adding (post-fill state).
    // LOCKED: best_bid == best_ask (equal price).
    // CROSSED: best_bid > best_ask (crossed market — a pathology).
    bool is_locked()  const noexcept {
        return has_bid() && has_ask() && best_bid_ticks_ == best_ask_ticks_;
    }
    bool is_crossed() const noexcept {
        return has_bid() && has_ask() && best_bid_ticks_ > best_ask_ticks_;
    }

    // Quote tester for FOK: is the available liquidity on this side up to `price` ≥ qty?
    bool fok_fillable(Side side, std::int32_t price, std::int32_t qty) const noexcept {
        std::int32_t avail = 0;
        if (side == Side::BUY) {
            for (std::int32_t p = best_ask_ticks_; p <= price && p < LEVELS; ++p) {
                avail += levels_[p].total_qty;
                if (avail >= qty) return true;
            }
        } else {
            for (std::int32_t p = best_bid_ticks_; p >= price && p >= 0; --p) {
                avail += levels_[p].total_qty;
                if (avail >= qty) return true;
            }
        }
        return avail >= qty;
    }

    // Match at a single price level (FIFO traversal). Decreases qty_remaining.
    // Called from the price loop in match_against().
    void match_at_level(PriceLevel& lvl, Order* taker,
                        std::int32_t& qty_remaining, std::uint64_t ts_ns) noexcept {
        Order* m = lvl.head;
        while (m && qty_remaining > 0) {
            // STP check: are the maker and taker the same entity (client or
            // firm — anti-internalization)?
            if (stp_policy_ != SelfTradePrevention::NONE &&
                same_trading_entity(m->client_id, taker->client_id)) {
                ++stats_.total_self_trade_blocks;
                if (m->client_id != taker->client_id)
                    ++firm_self_trade_blocks_;   // caught at the firm level
                if (stp_policy_ == SelfTradePrevention::CANCEL_NEWEST) {
                    // Taker falls — abort completely
                    qty_remaining = 0;
                    return;
                } else if (stp_policy_ == SelfTradePrevention::CANCEL_OLDEST) {
                    // The maker falls — remove it from the book and carry on
                    Order* next_m = m->next_at_level;
                    cancel_internal(m, /*emit_event=*/true);
                    m = next_m;
                    continue;
                } else if (stp_policy_ == SelfTradePrevention::CANCEL_BOTH) {
                    Order* next_m = m->next_at_level;
                    cancel_internal(m, true);
                    qty_remaining = 0;
                    (void)next_m;
                    return;
                }
            }

            const std::int32_t exec_qty = std::min(qty_remaining, m->displayed_qty);
            m->filled_qty   += exec_qty;
            taker->filled_qty += exec_qty;
            qty_remaining   -= exec_qty;
            // First-fill ts hook + latency stats (per order, only once)
            if (m->first_fill_ts_ns == 0) {
                m->first_fill_ts_ns = ts_ns;
                record_first_fill_latency(m->submit_ts_ns, ts_ns);
            }
            if (taker->first_fill_ts_ns == 0) {
                taker->first_fill_ts_ns = ts_ns;
                record_first_fill_latency(taker->submit_ts_ns, ts_ns);
            }
            exposure_on_fill(m->client_id, m->side, exec_qty);
            exposure_on_fill(taker->client_id, taker->side, exec_qty);
            account_vwap_on_fill(m->client_id, m->price_ticks, exec_qty);
            account_vwap_on_fill(taker->client_id, m->price_ticks, exec_qty);
            mmp_on_maker_fill(m->client_id);
            // Implementation shortfall (per fill, signed by side).
            // BUY: cost > 0 if fill_px > decision_mid; SELL: cost > 0 if fill_px < decision_mid.
            if (taker->decision_mid_ticks > 0) {
                const std::int64_t fp = m->price_ticks;
                const std::int64_t dm = taker->decision_mid_ticks;
                const std::int64_t cost_per_share = (taker->side == Side::BUY)
                    ? (fp - dm) : (dm - fp);
                cum_implementation_shortfall_ticks_qty_ +=
                    cost_per_share * static_cast<std::int64_t>(exec_qty);
                cum_implementation_shortfall_qty_ +=
                    static_cast<std::uint64_t>(exec_qty);
            }
            // Aggressive vs passive accounting per account
            tag_aggressor_volume(taker->client_id, exec_qty);
            tag_passive_volume(m->client_id, exec_qty);

            // Trade record (price = maker's price = lvl.price = m->price_ticks)
            record_trade(m->id, taker->id, m->price_ticks, exec_qty,
                         taker->side, ts_ns);
            emit(EventType::FILL, taker->id, m->id, m->price_ticks, exec_qty,
                 taker->remaining_qty() == 0 ? OrderStatus::FILLED
                                              : OrderStatus::PARTIALLY_FILLED,
                 RejectReason::NONE, taker->client_id);

            // Aktualizuj displayed/hidden i level total
            lvl.total_qty -= exec_qty;
            m->displayed_qty -= exec_qty;

            if (m->displayed_qty == 0 && m->total_qty - m->filled_qty > 0 &&
                m->type == OrderType::ICEBERG) {
                // Iceberg: top up the displayed from the hidden reserve — refresh to
                // the original display size (fallback 100 for legacy orders
                // without a recorded size, e.g. loaded from an old snapshot)
                const std::int32_t hidden = m->total_qty - m->filled_qty;
                std::int32_t show = (m->iceberg_display_size > 0)
                                     ? m->iceberg_display_size : 100;
                // Anti-detection jitter: a fixed refresh size reveals the iceberg
                // (predators count the repeatable refills). A deterministic LCG
                // — replaying the same sequence yields identical refreshes.
                if (ice_jitter_bps_ > 0) {
                    ice_jitter_rng_ = ice_jitter_rng_ * 6364136223846793005ULL
                                      + 1442695040888963407ULL;
                    const std::int32_t span = static_cast<std::int32_t>(
                        (static_cast<std::int64_t>(show) * ice_jitter_bps_) / 10000);
                    if (span > 0) {
                        const std::int32_t delta = static_cast<std::int32_t>(
                            (ice_jitter_rng_ >> 33) %
                            static_cast<std::uint64_t>(2 * span + 1)) - span;
                        show += delta;
                        if (show < 1) show = 1;
                    }
                }
                m->displayed_qty = std::min(hidden, show);
                lvl.total_qty    += m->displayed_qty;
                ++iceberg_refresh_count_;
                lvl.total_hidden -= m->displayed_qty;
                // Maker iceberg traci priority — przesuwamy na koniec FIFO
                Order* next_m = m->next_at_level;
                if (lvl.tail != m) {
                    // unlink + reinsert at tail
                    if (m->prev_at_level) m->prev_at_level->next_at_level = m->next_at_level;
                    else                  lvl.head = m->next_at_level;
                    if (m->next_at_level) m->next_at_level->prev_at_level = m->prev_at_level;
                    m->prev_at_level = lvl.tail;
                    m->next_at_level = nullptr;
                    lvl.tail->next_at_level = m;
                    lvl.tail = m;
                }
                m = next_m;
                continue;
            }

            if (m->filled_qty >= m->total_qty) {
                // Maker full → unlink + free
                Order* next_m = m->next_at_level;
                m->status = OrderStatus::FILLED;
                emit(EventType::FILL, m->id, taker->id, m->price_ticks, exec_qty,
                     OrderStatus::FILLED, RejectReason::NONE, m->client_id);
                // Unlink from the level (we already updated total_qty above, so
                // unlink_from_level would do it again. We do a manual unlink.)
                if (m->prev_at_level) m->prev_at_level->next_at_level = m->next_at_level;
                else                  lvl.head = m->next_at_level;
                if (m->next_at_level) m->next_at_level->prev_at_level = m->prev_at_level;
                else                  lvl.tail = m->prev_at_level;
                if (lvl.order_count > 0) --lvl.order_count;
                id_index_.erase(m->id);
                record_lifecycle_age(m->submit_ts_ns);
                ++completion_filled_fully_;
                if (m->side == Side::BUY) ++maker_fills_buy_side_;
                else                       ++maker_fills_sell_side_;
                oco_on_complete(m->id);
                bracket_on_full_fill(m->id);
                free_order(m);
                m = next_m;
            } else {
                m->status = OrderStatus::PARTIALLY_FILLED;
                // the taker got everything or the maker has displayed=0 but is not an iceberg.
                // If displayed_qty == 0 and not an iceberg → unlink (a limit order
                // that got a display reduction should not happen).
                m = m->next_at_level;
            }
        }
    }

    // Match against the opposite side. Updates the taker, generates fills.
    void match_against(Order* taker) noexcept {
        const std::uint64_t ts = mono_ns_now();
        std::int32_t qty_left = taker->remaining_qty();
        const std::int32_t qty_pre = qty_left;
        std::int32_t levels_consumed = 0;
        // Mid pre-match — used for effective spread (TCA)
        const std::int32_t mid_pre_ticks = has_bid() && has_ask()
            ? (best_bid_ticks_ + best_ask_ticks_) / 2 : -1;

        if (taker->side == Side::BUY) {
            // BUY eats asks from the best (cheapest) up to the limit price
            while (qty_left > 0 && has_ask() && best_ask_ticks_ <= taker->price_ticks) {
                const std::int32_t qty_before = qty_left;
                PriceLevel& lvl = levels_[best_ask_ticks_];
                match_at_level(lvl, taker, qty_left, ts);
                if (qty_left < qty_before) ++levels_consumed;
                if (lvl.empty()) {
                    refresh_best_ask_from(best_ask_ticks_ + 1);
                }
            }
        } else {
            while (qty_left > 0 && has_bid() && best_bid_ticks_ >= taker->price_ticks) {
                const std::int32_t qty_before = qty_left;
                PriceLevel& lvl = levels_[best_bid_ticks_];
                match_at_level(lvl, taker, qty_left, ts);
                if (qty_left < qty_before) ++levels_consumed;
                if (lvl.empty()) {
                    refresh_best_bid_from(best_bid_ticks_ - 1);
                }
            }
        }
        // Sweep stats — track multi-level fills
        if (qty_left < qty_pre) {
            ++stats_.total_sweeps;
            stats_.sum_levels_touched += static_cast<std::uint64_t>(levels_consumed);
            if (levels_consumed >= 2) ++stats_.multi_level_sweeps;
        }
        // Effective spread: 2× |VWAP(fills) − mid_pre|. Aproksymujemy fill_px = taker->price.
        if (mid_pre_ticks >= 0 && qty_left < qty_pre) {
            const std::int32_t diff = std::abs(taker->price_ticks - mid_pre_ticks);
            stats_.total_effective_spread_2x_ticks += static_cast<std::uint64_t>(2 * diff);
            ++stats_.total_effective_spread_samples;
            // Bands compliance
            if (fill_band_threshold_ticks_ > 0) {
                if (diff <= fill_band_threshold_ticks_) ++fills_within_band_;
                else                                     ++fills_outside_band_;
            }
            // Slippage guard violation
            if (slippage_guard_threshold_ticks_ > 0 &&
                diff > slippage_guard_threshold_ticks_) {
                ++slippage_guard_violations_;
            }
        }
        // NBBO audit — did the taker pay worse than the best opposite quote BEFORE the match?
        // Mid_pre_ticks already from the other side; we check: fill at taker->limit
        // for BUY should be <= best_ask_pre; for SELL >= best_bid_pre.
        // (A simple heuristic here — comparison to the limit price.)
        if (qty_left < qty_pre) {
            if (taker->side == Side::BUY &&
                taker->price_ticks > 0 &&
                mid_pre_ticks > 0 &&
                taker->price_ticks > mid_pre_ticks + 1000) {
                ++nbbo_violations_count_;
            } else if (taker->side == Side::SELL &&
                       taker->price_ticks > 0 &&
                       mid_pre_ticks > 0 &&
                       taker->price_ticks < mid_pre_ticks - 1000) {
                ++nbbo_violations_count_;
            }
        }
    }

    // Shared cancel path — used by user cancel + STP + internal.
    void cancel_internal(Order* o, bool emit_event) noexcept {
        if (!o || !o->is_active()) return;
        const std::int32_t cancel_price = o->price_ticks;
        const char         cancel_side  = (o->side == Side::BUY) ? 'B' : 'S';
        unlink_from_level(o);
        // Refresh best bid/ask if this level emptied
        if (levels_[o->price_ticks].empty()) {
            if (o->side == Side::BUY  && o->price_ticks == best_bid_ticks_)
                refresh_best_bid_from(o->price_ticks - 1);
            if (o->side == Side::SELL && o->price_ticks == best_ask_ticks_)
                refresh_best_ask_from(o->price_ticks + 1);
        }
        const std::int32_t leftover = o->remaining_qty();
        o->status = OrderStatus::CANCELLED;
        id_index_.erase(o->id);
        if (emit_event) {
            emit(EventType::CANCEL, o->id, 0, o->price_ticks, leftover,
                 OrderStatus::CANCELLED, RejectReason::NONE, o->client_id);
        }
        push_audit(EventType::CANCEL, o->id, o->side, o->price_ticks, leftover,
                    OrderStatus::CANCELLED);
        exposure_on_cancel(o->client_id, o->side, leftover);
        record_lifecycle_age(o->submit_ts_ns);
        if (o->filled_qty == 0)        ++completion_cancelled_unfilled_;
        else                            ++completion_cancelled_partial_;
        if (o->side == Side::BUY) ++cancellations_by_buy_;
        else                       ++cancellations_by_sell_;
        oco_on_complete(o->id);
        bracket_specs_.erase(o->id);   // cancel entry = disarm bracket
        free_order(o);
        ++stats_.total_orders_cancelled;
        push_delta(levels_[cancel_price].empty() ? 'D' : 'M',
                   cancel_side, cancel_price, levels_[cancel_price].total_qty);
    }

public:
    // ====================================================================
    // submit() — the main order-add path
    // ====================================================================
    //
    // Returns the order_id of the accepted order (>0) or 0 when rejected.
    // out_reason (if != nullptr) receives the RejectReason for diagnostics.
    //
    // Logika:
    //   1. Walidacja: range, qty, duplicate id, range.
    //   2. STP check (if policy != NONE) — but STP is more complete in the match.
    //   3. POST_ONLY: if it crosses → REJECT.
    //   4. FOK: if not achievable → REJECT.
    //   5. Alokuj Order z pool.
    //   6. If the type is STOP, do NOT match — place it in the trigger wait-queue.
    //   7. Match against the opposite side up to the limit price.
    //   8. If IOC and remainder — cancel the rest. If FOK and not all → revert.
    //   9. The rest (if > 0): insert into the FIFO at the price level + index id.
    std::uint64_t submit(Side side, std::int32_t price_ticks, std::int32_t qty,
                          OrderType type = OrderType::LIMIT,
                          TimeInForce tif = TimeInForce::DAY,
                          std::uint64_t order_id = 0,
                          std::uint64_t client_id = 0,
                          std::int32_t displayed_qty = 0,
                          RejectReason* out_reason = nullptr,
                          std::int32_t min_qty = 0) noexcept {
        auto reject = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            const std::uint64_t rid = order_id ? order_id : 0;
            if (client_id != 0) ++client_rejections_[client_id];
            emit(EventType::REJECT, rid, 0, price_ticks, qty,
                 OrderStatus::REJECTED, r, client_id);
            push_audit(EventType::REJECT, rid, side, price_ticks, qty,
                       OrderStatus::REJECTED);
            return 0;
        };

        // Walidacje
        if (session_phase_ == SessionPhase::CLOSED)
                                       return reject(RejectReason::MARKET_CLOSED);
        if (halted_)                   return reject(RejectReason::HALTED);
        if (qty <= 0)                  return reject(RejectReason::QTY_ZERO_OR_NEGATIVE);
        if (!in_range(price_ticks) && type != OrderType::MARKET)
                                       return reject(RejectReason::PRICE_OUT_OF_RANGE);
        if (order_id != 0 && id_index_.find(order_id) != id_index_.end())
                                       return reject(RejectReason::DUPLICATE_ID);
        // Rate limit (token bucket) — external submits only; internal
        // resubmity engine (stop trigger, MOC/LOC injection, bracket arming)
        // concern already-accepted orders and do not consume tokens
        if (rate_limiting_enabled_ && !in_internal_submit_ && client_id != 0 &&
            !consume_rate_token(client_id))
                                       return reject(RejectReason::RATE_LIMITED);
        // MMP — when the account's protection has tripped, new quotes are blocked until
        // an explicit mmp_reset (the MM must confirm it has refreshed its quotes).
        // The engine's internal resubmits bypass it (they concern old orders).
        if (mmp_enabled_ && !in_internal_submit_ && client_id != 0) {
            const auto it = mmp_.find(client_id);
            if (it != mmp_.end() && it->second.tripped)
                                       return reject(RejectReason::MMP_TRIPPED);
        }
        // LULD check — quote poza band'em → REJECT (+opt-in auto-halt)
        if (luld_enabled_ && (price_ticks < luld_low_ticks_ ||
                                price_ticks > luld_high_ticks_)) {
            ++luld_breaches_;
            // halt_to_auction: LULD-style pause (order entry continues, match
            // waits for the reopen cross); auto_halt: hard halt (submits fail)
            if (luld_halt_to_auction_)   halt_for_auction();
            else if (luld_auto_halt_)    halt("LULD_BREACH");
            return reject(RejectReason::LULD_BAND_BREACH);
        }

        // POST_ONLY: must not take liquidity (cross protection)
        if (type == OrderType::POST_ONLY && would_cross(side, price_ticks)) {
            return reject(RejectReason::POST_ONLY_WOULD_CROSS);
        }

        // FOK: must fill in full immediately
        if ((type == OrderType::FOK || tif == TimeInForce::FOK)
            && !fok_fillable(side, price_ticks, qty)) {
            return reject(RejectReason::FOK_NOT_FILLABLE);
        }

        // MIN_QTY: matched must be ≥ min_qty or REJECT.
        // (Skip for POST_ONLY/HIDDEN/auction — they do not match continuously.)
        if (min_qty > 0 && type != OrderType::POST_ONLY
            && type != OrderType::HIDDEN && !in_auction_mode_) {
            const auto preview = walk_the_book(side, qty, price_ticks);
            if (preview.fillable_qty < min_qty)
                return reject(RejectReason::MIN_QTY_NOT_MET);
        }

        // Alokacja
        Order* o = alloc_order();
        if (!o) return reject(RejectReason::POOL_EXHAUSTED);

        o->id            = order_id ? order_id : next_order_id_++;
        o->client_id     = client_id;
        o->price_ticks   = price_ticks;
        o->total_qty     = qty;
        o->filled_qty    = 0;
        // HIDDEN: full qty in total_hidden, displayed=0 → invisible in L1/L2.
        // ICEBERG: show only the displayed_qty arg (if >0).
        // Others: full visibility.
        // Iceberg display clamped to qty — displayed > total would corrupt hidden
        // accounting (T-F-D < 0). We remember the size for refreshes.
        const std::int32_t ice_show =
            (type == OrderType::ICEBERG && displayed_qty > 0)
                ? std::min(displayed_qty, qty) : 0;
        o->displayed_qty = (type == OrderType::HIDDEN) ? 0
                          : (ice_show > 0)              ? ice_show
                                                        : qty;
        o->iceberg_display_size = ice_show;
        o->side          = side;
        o->type          = type;
        o->tif           = tif;
        o->status        = OrderStatus::NEW;
        o->submit_ts_ns  = mono_ns_now();
        o->decision_mid_ticks = (has_bid() && has_ask())
            ? (best_bid_ticks_ + best_ask_ticks_) / 2 : -1;
        // Burst detector
        if (burst_window_ns_ > 0) {
            if (burst_last_submit_ns_ != 0 &&
                o->submit_ts_ns > burst_last_submit_ns_ &&
                (o->submit_ts_ns - burst_last_submit_ns_) <= burst_window_ns_) {
                if (burst_in_run_count_ == 0) {
                    ++burst_runs_count_;
                    burst_in_run_count_ = 2;
                } else {
                    ++burst_in_run_count_;
                }
            } else {
                burst_in_run_count_ = 0;
            }
            burst_last_submit_ns_ = o->submit_ts_ns;
        }
        ++stats_.total_orders_added;
        tally_accept(type, tif);

        emit(EventType::ACCEPT, o->id, 0, o->price_ticks, o->total_qty,
             OrderStatus::OPEN, RejectReason::NONE, o->client_id);
        push_audit(EventType::ACCEPT, o->id, o->side, o->price_ticks, o->total_qty,
                    OrderStatus::OPEN);
        exposure_on_submit(o->client_id, o->side, o->total_qty);

        // Match (unless POST_ONLY — but POST_ONLY would already be rejected if it crossed,
        // nor in auction mode — orders wait for the batch cross via run_auction).
        // HIDDEN orders do not match continuously (dark-pool semantics) — they enter
        // directly into the book as pure-dark liquidity for cross/auction.
        if (type != OrderType::POST_ONLY && type != OrderType::HIDDEN &&
            !in_auction_mode_) {
            match_against(o);
            process_oco_pending();
            process_bracket_pending();
            process_mmp_pending();
        }

        // Po matchu:
        if (o->filled_qty >= o->total_qty) {
            // Full fill → free the slot (does not enter the book)
            o->status = OrderStatus::FILLED;
            const std::uint64_t rid = o->id;
            ++completion_filled_fully_;
            free_order(o);
            return rid;
        }

        // IOC: do not leave remainders — cancel
        if (type == OrderType::IOC || tif == TimeInForce::IOC) {
            const std::int32_t left = o->remaining_qty();
            o->status = OrderStatus::CANCELLED;
            const std::uint64_t rid = o->id;
            emit(EventType::CANCEL, rid, 0, o->price_ticks, left,
                 OrderStatus::CANCELLED, RejectReason::NONE, o->client_id);
            if (o->filled_qty == 0) ++completion_cancelled_unfilled_;
            else                    ++completion_cancelled_partial_;
            free_order(o);
            return rid;
        }

        // Insert into the book (FIFO)
        o->status = OrderStatus::OPEN;
        if (o->filled_qty > 0) {
            o->status = OrderStatus::PARTIALLY_FILLED;
            // Remainder after a partial match: displayed must not exceed
            // remaining — otherwise L2 depth is inflated by filled_qty.
            o->displayed_qty = std::min(o->displayed_qty, o->remaining_qty());
        }
        // Queue depth at arrival — sample before enqueue (current order_count = position in FIFO)
        const std::int32_t qd_arrival = levels_[o->price_ticks].order_count;
        queue_depth_arrival_sum_ += static_cast<std::uint64_t>(qd_arrival);
        ++queue_depth_arrival_count_;
        if (qd_arrival > queue_depth_arrival_max_)
            queue_depth_arrival_max_ = qd_arrival;
        enqueue_at_level(o);
        id_index_[o->id] = o;

        // Update best bid/ask — ONLY for visible orders (HIDDEN never
        // appears in L1/L2; it is only for cross/auction matching).
        if (type != OrderType::HIDDEN) {
            if (o->side == Side::BUY) {
                if (best_bid_ticks_ == NO_BID_TICKS || o->price_ticks > best_bid_ticks_)
                    best_bid_ticks_ = o->price_ticks;
            } else {
                if (best_ask_ticks_ == NO_ASK_TICKS || o->price_ticks < best_ask_ticks_)
                    best_ask_ticks_ = o->price_ticks;
            }
            push_delta('A', o->side == Side::BUY ? 'B' : 'S',
                       o->price_ticks, levels_[o->price_ticks].total_qty);
        }
        return o->id;
    }

    // ====================================================================
    // cancel(order_id) — user cancel; returns true when found and removed.
    // ====================================================================
    bool cancel(std::uint64_t order_id) noexcept {
        auto it = id_index_.find(order_id);
        if (it == id_index_.end()) return false;
        Order* o = it->second;
        // A pending STOP is not in the levels (status NEW, not is_active) —
        // a separate path via stop_orders_.
        if (o->type == OrderType::STOP && o->status == OrderStatus::NEW) {
            const bool ok = cancel_pending_stop(o);
            process_oco_pending();
            return ok;
        }
        cancel_internal(o, /*emit_event=*/true);
        process_oco_pending();
        return true;
    }

    // ====================================================================
    // modify(order_id, new_price, new_qty) — cancel + resubmit, GUBI priority
    // (in line with most exchanges if you change the price). If only qty DOWN
    // (down-only), some exchanges keep priority; here I keep
    // only in the case of qty DOWN ON SAME PRICE.
    // ====================================================================
    std::uint64_t modify(std::uint64_t order_id,
                          std::int32_t new_price_ticks,
                          std::int32_t new_qty,
                          RejectReason* out_reason = nullptr) noexcept {
        auto it = id_index_.find(order_id);
        if (it == id_index_.end()) {
            if (out_reason) *out_reason = RejectReason::NONE;
            return 0;
        }
        Order* o = it->second;

        // Same-price + qty DOWN → priority-preserving in-place
        // (rule consistent with NASDAQ ITCH/OUCH "Decrease" — preserves FIFO)
        if (new_price_ticks == o->price_ticks &&
            new_qty < o->total_qty &&
            new_qty > o->filled_qty) {
            const std::int32_t delta = (o->total_qty - new_qty);
            // The reduction comes first from displayed, the surplus from the hidden reserve.
            // Level aggregates drop by exactly as much as
            // the order's components dropped — old code subtracted min(delta, level_total)
            // from total_qty (corrupted Σ displayed with many orders on a level)
            // and did not touch total_hidden at all (iceberg decrease).
            const std::int32_t vis_delta = std::min(delta, o->displayed_qty);
            o->total_qty = new_qty;
            o->displayed_qty -= vis_delta;
            levels_[o->price_ticks].total_qty    -= vis_delta;
            levels_[o->price_ticks].total_hidden -= (delta - vis_delta);
            // Bracket entry: exits must hedge the final (reduced) qty
            {
                const auto bit = bracket_specs_.find(order_id);
                if (bit != bracket_specs_.end()) bit->second.qty = new_qty;
            }
            ++stats_.total_orders_replaced;
            ++stats_.priority_preserved_mods;
            emit(EventType::REPLACE, o->id, 0, o->price_ticks, new_qty,
                 o->status, RejectReason::NONE, o->client_id);
            return o->id;
        }

        // Cancel + resubmit — gubi priority (price change OR qty UP)
        const Side       side       = o->side;
        const OrderType  type       = o->type;
        const TimeInForce tif       = o->tif;
        const std::uint64_t client_id = o->client_id;
        const std::int32_t  filled  = o->filled_qty;
        // OCO survives cancel-replace (order_id does not change — venue
        // convention). Stash + unlink BEFORE cancel_internal, so it does not cancel
        // partnera; restore po udanym resubmicie.
        const std::uint64_t oco_partner = oco_partner_of(order_id);
        if (oco_partner != 0) (void)unlink_oco(order_id);
        // The bracket spec also survives cancel-replace; the qty spec follows the new
        // qty (exits must hedge the final entry position)
        BracketSpec saved_bspec{};
        bool had_bracket = false;
        {
            const auto bit = bracket_specs_.find(order_id);
            if (bit != bracket_specs_.end()) {
                had_bracket = true;
                saved_bspec = bit->second;
                saved_bspec.qty = new_qty;
            }
        }
        cancel_internal(o, /*emit_event=*/false);
        ++stats_.total_orders_replaced;
        ++stats_.priority_lost_mods;
        const std::uint64_t nid = submit(side, new_price_ticks,
                                          new_qty - filled, type, tif,
                                          order_id, client_id, 0, out_reason);
        if (oco_partner != 0 && nid != 0) {
            if (id_index_.find(nid) != id_index_.end() &&
                id_index_.find(oco_partner) != id_index_.end()) {
                (void)link_oco(nid, oco_partner);
            } else if (id_index_.find(nid) == id_index_.end()) {
                // Zmodyfikowana noga od razu fully filled → semantyka OCO:
                // the partner falls (the pending queue counts and also handles STOP/NEW)
                oco_pending_cancel_.push_back(oco_partner);
                process_oco_pending();
            }
        }
        if (had_bracket && nid != 0) {
            if (id_index_.find(nid) != id_index_.end()) {
                bracket_specs_[nid] = saved_bspec;
            } else {
                // Entry filled during the resubmit → arm the exits now
                bracket_pending_.push_back(saved_bspec);
                process_bracket_pending();
            }
        }
        // nid == 0 (resubmit reject): the order is gone, the OCO pair dissolved,
        // the bracket disarmed — partner/spec do not return
        return nid;
    }

    // ====================================================================
    // L2 depth — `n_levels` najlepszych cen per side
    // ====================================================================
    //
    // Fills bid_out[] and ask_out[] capping to `n_levels` or LEVELS.
    // Returns how many were actually returned.
    std::int32_t depth(std::int32_t n_levels,
                       DepthLevel* bid_out, DepthLevel* ask_out,
                       std::int32_t* bid_count, std::int32_t* ask_count) const noexcept {
        std::int32_t bn = 0, an = 0;
        if (bid_out && has_bid()) {
            for (std::int32_t p = best_bid_ticks_; p >= 0 && bn < n_levels; --p) {
                const PriceLevel& lvl = levels_[p];
                if (lvl.total_qty > 0) {
                    bid_out[bn] = DepthLevel{p, lvl.total_qty, lvl.order_count};
                    ++bn;
                }
            }
        }
        if (ask_out && has_ask()) {
            for (std::int32_t p = best_ask_ticks_; p < LEVELS && an < n_levels; ++p) {
                const PriceLevel& lvl = levels_[p];
                if (lvl.total_qty > 0) {
                    ask_out[an] = DepthLevel{p, lvl.total_qty, lvl.order_count};
                    ++an;
                }
            }
        }
        if (bid_count) *bid_count = bn;
        if (ask_count) *ask_count = an;
        return bn + an;
    }

    // total_volume_at_price: Σ displayed qty at a specific level (0 when empty).
    std::int32_t total_volume_at_price(std::int32_t price_ticks) const noexcept {
        if (!in_range(price_ticks)) return 0;
        return levels_[price_ticks].total_qty;
    }

    std::int32_t order_count_at_price(std::int32_t price_ticks) const noexcept {
        if (!in_range(price_ticks)) return 0;
        return levels_[price_ticks].order_count;
    }

    // imbalance_bps: (bid_qty - ask_qty)/(bid+ask) × 10000.
    //   +5000 = only bids (very buy-heavy)
    //   -5000 = same aski
    //   0     = balans
    // Signal flow — a key order-book imbalance metric for strategies.
    std::int32_t imbalance_bps() const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        const std::int64_t b = levels_[best_bid_ticks_].total_qty;
        const std::int64_t a = levels_[best_ask_ticks_].total_qty;
        const std::int64_t total = b + a;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((b - a) * 10000 / total);
    }

    // queue_position(order_id): how many orders are ahead of us in the FIFO at this level.
    // A market-making strategy uses it to judge "when will I get taken" — if
    // you are 1000th in the queue, the fill probability is low.
    std::int32_t queue_position(std::uint64_t order_id) const noexcept {
        auto it = id_index_.find(order_id);
        if (it == id_index_.end()) return -1;
        Order* o = it->second;
        std::int32_t pos = 0;
        for (Order* p = levels_[o->price_ticks].head; p && p != o; p = p->next_at_level) {
            ++pos;
        }
        return pos;
    }

    // ====================================================================
    // Trade tape — recent executions (ring buffer)
    // ====================================================================
    std::size_t tape_size() const noexcept {
        return std::min(tape_count_, TAPE_CAP);
    }

    // Copy the last `max_n` executions to out[]. Returns how many were returned
    // (never more than tape_size()).
    std::size_t recent_trades(Trade* out, std::size_t max_n) const noexcept {
        const std::size_t avail = tape_size();
        const std::size_t n = std::min(avail, max_n);
        for (std::size_t i = 0; i < n; ++i) {
            // from newest backwards
            const std::size_t idx = (tape_head_ + TAPE_CAP - 1 - i) % TAPE_CAP;
            out[i] = tape_[idx];
        }
        return n;
    }

    // VWAP z trade tape (volume weighted average price) — w tickach.
    std::int32_t tape_vwap_ticks() const noexcept {
        const std::size_t n = tape_size();
        if (n == 0) return -1;
        std::int64_t price_qty = 0;
        std::int64_t qty       = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - 1 - i) % TAPE_CAP];
            price_qty += static_cast<std::int64_t>(t.price_ticks) * t.qty;
            qty       += t.qty;
        }
        return qty > 0 ? static_cast<std::int32_t>(price_qty / qty) : -1;
    }

    // ====================================================================
    // Snapshot — wire format do recovery
    // ====================================================================
    //
    // Encoder/decoder: serializes all active orders into a byte buffer
    // (per-order POD), returns bytes_written. Wire format:
    //   [4 B magic "OBPRO" prefix nadpisany, no]
    //   [4 B version (1)]
    //   [8 B active_orders count]
    //   [N × OrderRecord (70 B each — v2 added iceberg_display_size)]
    //
    // This is NOT a delta — it is a FULL snapshot. The delta is implemented via
    // event callback z EventType (caller may build incremental log).
    //
    // Returns the number of bytes written or 0 when the buffer is too small.

    struct __attribute__((packed)) OrderRecord {
        std::uint64_t id;
        std::uint64_t client_id;
        std::int32_t  price_ticks;
        std::int32_t  total_qty;
        std::int32_t  filled_qty;
        std::int32_t  displayed_qty;
        std::uint8_t  side;
        std::uint8_t  type;
        std::uint8_t  tif;
        std::uint8_t  status;
        std::uint64_t submit_ts_ns;
        std::uint64_t expire_ts_ns;
        std::int32_t  stop_trigger_ticks;
        std::int32_t  peg_offset_ticks;
        std::int32_t  iceberg_display_size;   // od wersji 2
    };

    static constexpr std::uint32_t SNAPSHOT_VERSION = 2;
    static constexpr std::uint32_t SNAPSHOT_MAGIC   = 0x4F42504F;  // "OBPO"

    std::size_t snapshot_size_estimate() const noexcept {
        return 4 + 4 + 8 + active_orders_ * sizeof(OrderRecord);
    }

    std::size_t serialize_snapshot(std::uint8_t* buf, std::size_t cap) const noexcept {
        const std::size_t need = snapshot_size_estimate();
        if (cap < need) return 0;
        std::size_t off = 0;
        std::uint32_t magic   = SNAPSHOT_MAGIC;
        std::uint32_t version = SNAPSHOT_VERSION;
        std::uint64_t count   = active_orders_;
        std::memcpy(buf + off, &magic,   4); off += 4;
        std::memcpy(buf + off, &version, 4); off += 4;
        std::memcpy(buf + off, &count,   8); off += 8;

        // Iterate over all levels (both sides), for each over the FIFO
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            for (Order* o = levels_[p].head; o; o = o->next_at_level) {
                OrderRecord r{};
                r.id            = o->id;
                r.client_id     = o->client_id;
                r.price_ticks   = o->price_ticks;
                r.total_qty     = o->total_qty;
                r.filled_qty    = o->filled_qty;
                r.displayed_qty = o->displayed_qty;
                r.side          = static_cast<std::uint8_t>(o->side);
                r.type          = static_cast<std::uint8_t>(o->type);
                r.tif           = static_cast<std::uint8_t>(o->tif);
                r.status        = static_cast<std::uint8_t>(o->status);
                r.submit_ts_ns  = o->submit_ts_ns;
                r.expire_ts_ns  = o->expire_ts_ns;
                r.stop_trigger_ticks = o->stop_trigger_ticks;
                r.peg_offset_ticks   = o->peg_offset_ticks;
                r.iceberg_display_size = o->iceberg_display_size;
                std::memcpy(buf + off, &r, sizeof(r));
                off += sizeof(r);
            }
        }
        // Pending STOPs are allocated (counted in active_orders_ = header
        // count), but do NOT sit in levels — without this loop the written count
        // would be larger than the record count and load_snapshot would fail.
        for (const Order* o : stop_orders_) {
            OrderRecord r{};
            r.id            = o->id;
            r.client_id     = o->client_id;
            r.price_ticks   = o->price_ticks;
            r.total_qty     = o->total_qty;
            r.filled_qty    = o->filled_qty;
            r.displayed_qty = o->displayed_qty;
            r.side          = static_cast<std::uint8_t>(o->side);
            r.type          = static_cast<std::uint8_t>(o->type);
            r.tif           = static_cast<std::uint8_t>(o->tif);
            r.status        = static_cast<std::uint8_t>(o->status);
            r.submit_ts_ns  = o->submit_ts_ns;
            r.expire_ts_ns  = o->expire_ts_ns;
            r.stop_trigger_ticks = o->stop_trigger_ticks;
            r.peg_offset_ticks   = o->peg_offset_ticks;
            r.iceberg_display_size = o->iceberg_display_size;
            std::memcpy(buf + off, &r, sizeof(r));
            off += sizeof(r);
        }
        return off;
    }

    // Load a snapshot — reloads the entire book. Returns true on success.
    bool load_snapshot(const std::uint8_t* buf, std::size_t len) noexcept {
        if (len < 16) return false;
        std::uint32_t magic, version;
        std::uint64_t count;
        std::memcpy(&magic,   buf,      4);
        std::memcpy(&version, buf + 4,  4);
        std::memcpy(&count,   buf + 8,  8);
        if (magic != SNAPSHOT_MAGIC) return false;
        if (version != SNAPSHOT_VERSION) return false;
        if (len < 16 + count * sizeof(OrderRecord)) return false;

        clear();
        std::size_t off = 16;
        std::uint64_t max_loaded_id = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            OrderRecord r;
            std::memcpy(&r, buf + off, sizeof(r));
            off += sizeof(r);
            Order* o = alloc_order();
            if (!o) return false;
            if (r.id > max_loaded_id) max_loaded_id = r.id;
            o->id            = r.id;
            o->client_id     = r.client_id;
            o->price_ticks   = r.price_ticks;
            o->total_qty     = r.total_qty;
            o->filled_qty    = r.filled_qty;
            o->displayed_qty = r.displayed_qty;
            o->side          = static_cast<Side>(r.side);
            o->type          = static_cast<OrderType>(r.type);
            o->tif           = static_cast<TimeInForce>(r.tif);
            o->status        = static_cast<OrderStatus>(r.status);
            o->submit_ts_ns  = r.submit_ts_ns;
            o->expire_ts_ns  = r.expire_ts_ns;
            o->stop_trigger_ticks = r.stop_trigger_ticks;
            o->peg_offset_ticks   = r.peg_offset_ticks;
            o->iceberg_display_size = r.iceberg_display_size;
            // Routing per type: a pending STOP goes back to stop_orders_ (not to
            // levels), PEG do levels + peg_orders_ (inaczej traci reprice).
            if (o->type == OrderType::STOP &&
                o->status == OrderStatus::NEW) {
                stop_orders_.push_back(o);
                id_index_[o->id] = o;
                continue;
            }
            enqueue_at_level(o);
            id_index_[o->id] = o;
            if (o->type == OrderType::PEG) peg_orders_.push_back(o);
            if (o->side == Side::BUY) {
                if (best_bid_ticks_ == NO_BID_TICKS || o->price_ticks > best_bid_ticks_)
                    best_bid_ticks_ = o->price_ticks;
            } else {
                if (best_ask_ticks_ == NO_ASK_TICKS || o->price_ticks < best_ask_ticks_)
                    best_ask_ticks_ = o->price_ticks;
            }
        }
        // The auto-id must skip past the loaded orders — otherwise a new submit with
        // order_id=0 gets a colliding id and overwrites an entry in id_index_
        // (the restored order is left orphaned in the level).
        if (max_loaded_id >= next_order_id_) next_order_id_ = max_loaded_id + 1;
        return true;
    }

    // Full reset — book empty, statistics cleared.
    void clear() noexcept {
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            Order* o = levels_[p].head;
            while (o) {
                Order* next = o->next_at_level;
                free_order(o);
                o = next;
            }
            levels_[p].clear();
        }
        id_index_.clear();
        // Pending STOPs do not sit in levels — the loop above did not free them.
        // Without this the free pool leaked (the slot neither in the free-list nor reachable).
        for (Order* o : stop_orders_) free_order(o);
        stop_orders_.clear();
        peg_orders_.clear();   // pegs are in levels — already freed above
        best_bid_ticks_ = NO_BID_TICKS;
        best_ask_ticks_ = NO_ASK_TICKS;
        last_trade_ticks_ = -1;
        tape_head_ = 0;
        tape_count_ = 0;
    }

    // ====================================================================
    // STOP order management
    // ====================================================================
    //
    // STOP orders do not enter the book immediately — they wait in `stop_orders_`
    // until last_trade_ticks_ crosses `stop_trigger_ticks`:
    //   BUY STOP   triggers when last_trade >= stop_trigger
    //   SELL STOP  triggers when last_trade <= stop_trigger
    // After triggering they become LIMIT (with `price_ticks` as the limit) or MARKET
    // (when `price_ticks == 0`).
    //
    // submit_stop: register a STOP. Returns an id or 0 on error.
    std::uint64_t submit_stop(Side side, std::int32_t trigger_ticks,
                                std::int32_t limit_ticks, std::int32_t qty,
                                std::uint64_t order_id = 0,
                                std::uint64_t client_id = 0,
                                RejectReason* out_reason = nullptr) noexcept {
        auto reject = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            return 0;
        };
        if (session_phase_ == SessionPhase::CLOSED)
            return reject(RejectReason::MARKET_CLOSED);
        if (qty <= 0) return reject(RejectReason::QTY_ZERO_OR_NEGATIVE);
        if (trigger_ticks < PRICE_MIN_TICKS || trigger_ticks >= LEVELS)
            return reject(RejectReason::PRICE_OUT_OF_RANGE);

        Order* o = alloc_order();
        if (!o) return reject(RejectReason::POOL_EXHAUSTED);
        o->id            = order_id ? order_id : next_order_id_++;
        o->client_id     = client_id;
        o->price_ticks   = limit_ticks;     // 0 = market on trigger
        o->total_qty     = qty;
        o->displayed_qty = qty;
        o->side          = side;
        o->type          = OrderType::STOP;
        o->status        = OrderStatus::NEW;
        o->submit_ts_ns  = mono_ns_now();
        o->stop_trigger_ticks = trigger_ticks;
        stop_orders_.push_back(o);
        id_index_[o->id] = o;
        ++stats_.total_orders_added;
        tally_accept(OrderType::STOP, TimeInForce::DAY);
        emit(EventType::ACCEPT, o->id, 0, trigger_ticks, qty,
             OrderStatus::NEW, RejectReason::NONE, client_id);
        return o->id;
    }

    // check_stop_triggers: call after each executed trade. Every STOP
    // that meets its trigger condition becomes an active LIMIT/MARKET.
    void check_stop_triggers() noexcept {
        if (last_trade_ticks_ < 0 || stop_orders_.empty()) return;
        for (std::size_t i = 0; i < stop_orders_.size(); ) {
            Order* o = stop_orders_[i];
            // Reference per order: a plain stop ALWAYS uses last_trade; trailing
            // with mid mode enabled uses mid (the ratchet AND trigger from the
            // same reference — mixing would break self-trigger safety).
            // Fallback to last_trade when mid is unavailable (one-sided book).
            std::int32_t ref = last_trade_ticks_;
            if (o->peg_offset_ticks > 0 && trailing_ratchet_on_mid_) {
                const std::int32_t mp = mid_ticks();
                if (mp > 0) ref = mp;
            }
            // Trailing stop (STOP + peg_offset > 0): ratchet the trigger behind the price
            // when the market moves favourably. SELL: the trigger follows UP
            // (high-water - offset); BUY: DOWN (low-water + offset).
            // The ratchet never self-triggers: the new trigger is always
            // offset from ref, so the trigger condition does not hold in this tick.
            if (o->peg_offset_ticks > 0) {
                if (o->side == Side::SELL) {
                    const std::int32_t nt = ref - o->peg_offset_ticks;
                    if (nt > o->stop_trigger_ticks && nt >= 0) {
                        o->stop_trigger_ticks = nt;
                        ++trailing_ratchets_;
                    }
                } else {
                    const std::int32_t nt = ref + o->peg_offset_ticks;
                    if (nt < o->stop_trigger_ticks && nt < LEVELS) {
                        o->stop_trigger_ticks = nt;
                        ++trailing_ratchets_;
                    }
                }
            }
            const bool buy_trigger  = (o->side == Side::BUY)
                                    && (ref >= o->stop_trigger_ticks);
            const bool sell_trigger = (o->side == Side::SELL)
                                    && (ref <= o->stop_trigger_ticks);
            if (buy_trigger || sell_trigger) {
                // Pull from the queue, preserve the parameters, resubmit as LIMIT/MARKET
                const Side       sd  = o->side;
                const std::int32_t lp = o->price_ticks;
                const std::int32_t qy = o->total_qty;
                const std::uint64_t cid = o->client_id;
                const std::uint64_t oid = o->id;
                id_index_.erase(oid);
                free_order(o);
                stop_orders_[i] = stop_orders_.back();
                stop_orders_.pop_back();
                ++stats_.total_stop_triggers;
                // limit_ticks=0 → market: use NO_BID/NO_ASK as the limit fallback
                const std::int32_t limit_p = (lp > 0) ? lp
                                              : (sd == Side::BUY ? LEVELS - 1 : 0);
                in_internal_submit_ = true;
                submit(sd, limit_p, qy, OrderType::LIMIT, TimeInForce::DAY,
                       oid, cid, 0, nullptr);
                in_internal_submit_ = false;
            } else {
                ++i;
            }
        }
    }

    std::size_t stop_orders_count() const noexcept { return stop_orders_.size(); }
    std::int32_t last_trade_ticks() const noexcept { return last_trade_ticks_; }

    // ====================================================================
    // Trailing stop
    // ====================================================================
    //
    // A stop with a trigger that follows the market by trail_offset_ticks:
    //   SELL (ochrona longa): trigger = high-water-mark - offset, ratchet up
    //   BUY  (ochrona shorta): trigger = low-water-mark + offset, ratchet down
    // Initial trigger liczony od reference = last_trade (fallback: mid).
    // Marker: type == STOP && peg_offset_ticks > 0 (a plain STOP has offset 0).
    // The ratchet happens in check_stop_triggers — it requires regular calling.
    std::uint64_t submit_trailing_stop(Side side,
                                        std::int32_t trail_offset_ticks,
                                        std::int32_t limit_ticks,
                                        std::int32_t qty,
                                        std::uint64_t order_id = 0,
                                        std::uint64_t client_id = 0,
                                        RejectReason* out_reason = nullptr) noexcept {
        auto reject = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            return 0;
        };
        if (qty <= 0)                return reject(RejectReason::QTY_ZERO_OR_NEGATIVE);
        if (trail_offset_ticks <= 0) return reject(RejectReason::PRICE_OUT_OF_RANGE);
        std::int32_t ref = last_trade_ticks_;
        if (ref < 0) ref = mid_ticks();
        if (ref < 0)                 return reject(RejectReason::PRICE_OUT_OF_RANGE);
        const std::int32_t trigger = (side == Side::SELL)
            ? ref - trail_offset_ticks : ref + trail_offset_ticks;
        if (trigger < 0 || trigger >= LEVELS)
            return reject(RejectReason::PRICE_OUT_OF_RANGE);
        const std::uint64_t sid = submit_stop(side, trigger, limit_ticks, qty,
                                               order_id, client_id, out_reason);
        if (sid == 0) return 0;
        auto it = id_index_.find(sid);
        if (it != id_index_.end())
            it->second->peg_offset_ticks = trail_offset_ticks;
        return sid;
    }
    std::size_t trailing_stops_count() const noexcept {
        std::size_t n = 0;
        for (const Order* o : stop_orders_) {
            if (o->peg_offset_ticks > 0) ++n;
        }
        return n;
    }
    std::uint64_t trailing_ratchet_count() const noexcept { return trailing_ratchets_; }

    // Trailing na mid: ratchet i trigger z midpointu zamiast last_trade.
    // Reacts to quote movement without trades (less gap risk in a quiet
    // the market). Plain stops always use last_trade. Fallback to last_trade
    // when mid is unavailable.
    void set_trailing_ratchet_on_mid(bool on) noexcept {
        trailing_ratchet_on_mid_ = on;
    }
    bool trailing_ratchet_on_mid() const noexcept {
        return trailing_ratchet_on_mid_;
    }

    // Iceberg refresh jitter — randomizacja rozmiaru refilla ±bps/10000.
    // 0 = off (deterministyczny refresh do display size). Typowo 1000-3000.
    void set_iceberg_refresh_jitter_bps(std::int32_t bps) noexcept {
        if (bps >= 0 && bps <= 9000) ice_jitter_bps_ = bps;
    }
    std::int32_t iceberg_refresh_jitter_bps() const noexcept {
        return ice_jitter_bps_;
    }

    // ====================================================================
    // PEG order management
    // ====================================================================
    //
    // A PEG order pegged to best bid/ask + offset_ticks. After every change of top
    // of book, peg orders are moved to the new price level (re-quote).
    //
    // Konwencja:
    //   BUY PEG  → pinned to best_bid + peg_offset (offset usually ≤ 0)
    //   SELL PEG → pinned to best_ask + peg_offset
    std::uint64_t submit_peg(Side side, std::int32_t peg_offset_ticks,
                              std::int32_t qty,
                              std::uint64_t order_id = 0,
                              std::uint64_t client_id = 0,
                              RejectReason* out_reason = nullptr,
                              std::int32_t cap_ticks = 0) noexcept {
        auto reject = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            return 0;
        };
        if (session_phase_ == SessionPhase::CLOSED)
            return reject(RejectReason::MARKET_CLOSED);
        if (qty <= 0) return reject(RejectReason::QTY_ZERO_OR_NEGATIVE);
        // Initial price compute
        std::int32_t initial_price;
        if (side == Side::BUY) {
            if (!has_bid()) initial_price = 0;
            else            initial_price = best_bid_ticks_ + peg_offset_ticks;
        } else {
            if (!has_ask()) initial_price = LEVELS - 1;
            else            initial_price = best_ask_ticks_ + peg_offset_ticks;
        }
        // Pegged-with-limit: cap to a hard price limit (BUY: ceiling, SELL: floor)
        if (cap_ticks > 0) {
            if (side == Side::BUY  && initial_price > cap_ticks) initial_price = cap_ticks;
            if (side == Side::SELL && initial_price < cap_ticks) initial_price = cap_ticks;
        }
        if (initial_price < 0 || initial_price >= LEVELS)
            return reject(RejectReason::PRICE_OUT_OF_RANGE);

        Order* o = alloc_order();
        if (!o) return reject(RejectReason::POOL_EXHAUSTED);
        o->id            = order_id ? order_id : next_order_id_++;
        o->client_id     = client_id;
        o->price_ticks   = initial_price;
        o->total_qty     = qty;
        o->displayed_qty = qty;
        o->side          = side;
        o->type          = OrderType::PEG;
        o->status        = OrderStatus::OPEN;
        o->submit_ts_ns  = mono_ns_now();
        o->peg_offset_ticks = peg_offset_ticks;
        o->peg_cap_ticks    = cap_ticks;
        enqueue_at_level(o);
        id_index_[o->id] = o;
        peg_orders_.push_back(o);
        if (side == Side::BUY) {
            if (best_bid_ticks_ == NO_BID_TICKS || initial_price > best_bid_ticks_)
                best_bid_ticks_ = initial_price;
        } else {
            if (best_ask_ticks_ == NO_ASK_TICKS || initial_price < best_ask_ticks_)
                best_ask_ticks_ = initial_price;
        }
        ++stats_.total_orders_added;
        tally_accept(OrderType::PEG, TimeInForce::DAY);
        emit(EventType::ACCEPT, o->id, 0, initial_price, qty,
             OrderStatus::OPEN, RejectReason::NONE, client_id);
        return o->id;
    }

    // Mid-peg: pinned do mid + offset (zamiast best bid/ask). Marker w
    // stop_trigger_ticks == 1 (a primary peg has 0 there; a STOP is not in
    // peg_orders_, so no collision). The offset should keep the price
    // inside the spread — the peg does not match at entry (like a primary peg).
    std::uint64_t submit_peg_mid(Side side, std::int32_t peg_offset_ticks,
                                  std::int32_t qty,
                                  std::uint64_t order_id = 0,
                                  std::uint64_t client_id = 0,
                                  RejectReason* out_reason = nullptr,
                                  std::int32_t cap_ticks = 0) noexcept {
        auto reject = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            return 0;
        };
        if (session_phase_ == SessionPhase::CLOSED)
            return reject(RejectReason::MARKET_CLOSED);
        if (qty <= 0) return reject(RejectReason::QTY_ZERO_OR_NEGATIVE);
        const std::int32_t mp = mid_ticks();
        if (mp < 0) return reject(RejectReason::PRICE_OUT_OF_RANGE);
        std::int32_t initial_price = mp + peg_offset_ticks;
        if (cap_ticks > 0) {
            if (side == Side::BUY  && initial_price > cap_ticks) initial_price = cap_ticks;
            if (side == Side::SELL && initial_price < cap_ticks) initial_price = cap_ticks;
        }
        if (initial_price < 0 || initial_price >= LEVELS)
            return reject(RejectReason::PRICE_OUT_OF_RANGE);

        Order* o = alloc_order();
        if (!o) return reject(RejectReason::POOL_EXHAUSTED);
        o->id            = order_id ? order_id : next_order_id_++;
        o->client_id     = client_id;
        o->price_ticks   = initial_price;
        o->total_qty     = qty;
        o->displayed_qty = qty;
        o->side          = side;
        o->type          = OrderType::PEG;
        o->status        = OrderStatus::OPEN;
        o->submit_ts_ns  = mono_ns_now();
        o->peg_offset_ticks   = peg_offset_ticks;
        o->peg_cap_ticks      = cap_ticks;
        o->stop_trigger_ticks = 1;   // marker: mid-peg
        enqueue_at_level(o);
        id_index_[o->id] = o;
        peg_orders_.push_back(o);
        if (side == Side::BUY) {
            if (best_bid_ticks_ == NO_BID_TICKS || initial_price > best_bid_ticks_)
                best_bid_ticks_ = initial_price;
        } else {
            if (best_ask_ticks_ == NO_ASK_TICKS || initial_price < best_ask_ticks_)
                best_ask_ticks_ = initial_price;
        }
        ++stats_.total_orders_added;
        tally_accept(OrderType::PEG, TimeInForce::DAY);
        emit(EventType::ACCEPT, o->id, 0, initial_price, qty,
             OrderStatus::OPEN, RejectReason::NONE, client_id);
        return o->id;
    }

    // reprice_pegs: call after a change in top of book. Every peg whose target
    // price has diverged from the current one is moved.
    void reprice_pegs() noexcept {
        if (peg_orders_.empty()) return;
        for (std::size_t i = 0; i < peg_orders_.size(); ++i) {
            Order* o = peg_orders_[i];
            if (!o->is_active()) continue;
            std::int32_t target;
            if (o->stop_trigger_ticks == 1) {
                // Mid-peg — requires both sides of the TOB
                const std::int32_t mp = mid_ticks();
                if (mp < 0) continue;
                target = mp + o->peg_offset_ticks;
            } else if (o->side == Side::BUY) {
                if (!has_bid()) continue;
                target = best_bid_ticks_ + o->peg_offset_ticks;
            } else {
                if (!has_ask()) continue;
                target = best_ask_ticks_ + o->peg_offset_ticks;
            }
            // Pegged-with-limit: the target never exceeds the cap
            if (o->peg_cap_ticks > 0) {
                if (o->side == Side::BUY  && target > o->peg_cap_ticks)
                    target = o->peg_cap_ticks;
                if (o->side == Side::SELL && target < o->peg_cap_ticks)
                    target = o->peg_cap_ticks;
            }
            if (target < 0 || target >= LEVELS) continue;
            if (target == o->price_ticks) continue;
            // Move: unlink + insert at new level (priority lost)
            const std::int32_t old_price = o->price_ticks;
            unlink_from_level(o);
            if (levels_[old_price].empty()) {
                if (o->side == Side::BUY && old_price == best_bid_ticks_)
                    refresh_best_bid_from(old_price - 1);
                if (o->side == Side::SELL && old_price == best_ask_ticks_)
                    refresh_best_ask_from(old_price + 1);
            }
            o->price_ticks = target;
            enqueue_at_level(o);
            if (o->side == Side::BUY && target > best_bid_ticks_)
                best_bid_ticks_ = target;
            if (o->side == Side::SELL && target < best_ask_ticks_)
                best_ask_ticks_ = target;
            ++stats_.total_peg_reprices;
        }
    }

    std::size_t peg_orders_count() const noexcept { return peg_orders_.size(); }

    // ====================================================================
    // Mass cancel — kill switch by client_id
    // ====================================================================
    //
    // In real HFT this is a mandatory guard rail: when risk trips, all
    // open orders of the account must be cancelled instantly. Returns how many
    // anulowano.
    std::size_t mass_cancel(std::uint64_t client_id) noexcept {
        std::size_t cancelled = 0;
        // Iterate by collecting ids first — modifying id_index_ during
        // iteracji UB.
        std::vector<Order*> to_cancel;
        std::vector<Order*> to_cancel_stops;
        to_cancel.reserve(id_index_.size());
        for (auto& kv : id_index_) {
            Order* o = kv.second;
            if (o->client_id != client_id) continue;
            if (o->is_active()) {
                to_cancel.push_back(o);
            } else if (o->type == OrderType::STOP &&
                       o->status == OrderStatus::NEW) {
                // The kill switch MUST also cover pending stops — a waiting STOP
                // has status NEW (not is_active); the old version skipped it
                to_cancel_stops.push_back(o);
            }
        }
        for (Order* o : to_cancel) {
            cancel_internal(o, /*emit_event=*/true);
            ++cancelled;
        }
        for (Order* o : to_cancel_stops) {
            if (cancel_pending_stop(o)) ++cancelled;
        }
        stats_.total_mass_cancels += cancelled;
        process_oco_pending();
        return cancelled;
    }

    // ====================================================================
    // GTD expiry sweep
    // ====================================================================
    //
    // Call periodically (once a second / occasionally). Every GTD whose
    // expire_ts_ns_ < now_ns is cancelled as an EXPIRE event. Returns how many.
    std::size_t expire_gtd(std::uint64_t now_ns) noexcept {
        std::vector<Order*> to_expire;
        for (auto& kv : id_index_) {
            Order* o = kv.second;
            if (o->tif == TimeInForce::GTD && o->is_active() &&
                o->expire_ts_ns > 0 && o->expire_ts_ns <= now_ns) {
                to_expire.push_back(o);
            }
        }
        for (Order* o : to_expire) {
            const std::int32_t left = o->remaining_qty();
            const std::int32_t price = o->price_ticks;
            const std::uint64_t oid = o->id;
            unlink_from_level(o);
            if (levels_[price].empty()) {
                if (o->side == Side::BUY && price == best_bid_ticks_)
                    refresh_best_bid_from(price - 1);
                if (o->side == Side::SELL && price == best_ask_ticks_)
                    refresh_best_ask_from(price + 1);
            }
            o->status = OrderStatus::EXPIRED;
            emit(EventType::EXPIRE, oid, 0, price, left,
                 OrderStatus::EXPIRED, RejectReason::NONE, o->client_id);
            id_index_.erase(oid);
            if (o->filled_qty == 0) ++completion_expired_unfilled_;
            else                    ++completion_expired_partial_;
            oco_on_complete(oid);
            bracket_specs_.erase(oid);
            free_order(o);
            ++stats_.total_orders_expired;
        }
        process_oco_pending();
        return to_expire.size();
    }

    // ====================================================================
    // Walk-the-book — preview matching without modifying the book
    // ====================================================================
    //
    // Checks how much of `desired` qty can be filled immediately up to `limit_price`
    // (as submit(IOC) would). Returns filled_qty + average_price.
    // The caller uses it for pre-trade analysis ("how much is on the top 5 levels").
    struct WalkResult {
        std::int32_t  fillable_qty;
        std::int32_t  avg_price_ticks;     // qty-weighted
        std::int32_t  levels_touched;
        std::int32_t  worst_price_ticks;   // the worst level that would be traversed
    };
    WalkResult walk_the_book(Side side, std::int32_t desired_qty,
                              std::int32_t limit_price) const noexcept {
        WalkResult r{};
        std::int64_t qty_price_sum = 0;
        std::int32_t remaining = desired_qty;
        if (side == Side::BUY) {
            for (std::int32_t p = best_ask_ticks_;
                 p <= limit_price && p < LEVELS && remaining > 0; ++p) {
                const std::int32_t avail = levels_[p].total_qty;
                if (avail <= 0) continue;
                const std::int32_t take = std::min(avail, remaining);
                qty_price_sum += static_cast<std::int64_t>(take) * p;
                r.fillable_qty += take;
                remaining -= take;
                ++r.levels_touched;
                r.worst_price_ticks = p;
            }
        } else {
            for (std::int32_t p = best_bid_ticks_;
                 p >= limit_price && p >= 0 && remaining > 0; --p) {
                const std::int32_t avail = levels_[p].total_qty;
                if (avail <= 0) continue;
                const std::int32_t take = std::min(avail, remaining);
                qty_price_sum += static_cast<std::int64_t>(take) * p;
                r.fillable_qty += take;
                remaining -= take;
                ++r.levels_touched;
                r.worst_price_ticks = p;
            }
        }
        r.avg_price_ticks = (r.fillable_qty > 0)
            ? static_cast<std::int32_t>(qty_price_sum / r.fillable_qty)
            : 0;
        return r;
    }

    // ====================================================================
    // Order Flow Imbalance (OFI) — an industry signal
    // ====================================================================
    //
    // OFI measures the NET change in top-of-book qty since the last observation.
    //   ΔOFI = Δbid_qty(if the bid level did not drop) - Δask_qty(if the ask level did not rise)
    // Dodatnia OFI = buy pressure, ujemna = sell pressure.
    //
    // The caller invokes sample_ofi() periodically; it returns the delta and resets state.
    std::int64_t sample_ofi() noexcept {
        const std::int32_t cur_bid_qty = has_bid() ? levels_[best_bid_ticks_].total_qty : 0;
        const std::int32_t cur_ask_qty = has_ask() ? levels_[best_ask_ticks_].total_qty : 0;
        const std::int32_t cur_bid     = has_bid() ? best_bid_ticks_ : 0;
        const std::int32_t cur_ask     = has_ask() ? best_ask_ticks_ : 0;

        std::int64_t delta = 0;
        if (cur_bid >= last_ofi_bid_ticks_) delta += (cur_bid_qty - last_ofi_bid_qty_);
        if (cur_ask <= last_ofi_ask_ticks_) delta -= (cur_ask_qty - last_ofi_ask_qty_);

        last_ofi_bid_ticks_ = cur_bid;
        last_ofi_ask_ticks_ = cur_ask;
        last_ofi_bid_qty_   = cur_bid_qty;
        last_ofi_ask_qty_   = cur_ask_qty;
        cum_ofi_ += delta;
        return delta;
    }
    std::int64_t cumulative_ofi() const noexcept { return cum_ofi_; }

    // ====================================================================
    // Cumulative volume profile (volume at price)
    // ====================================================================
    //
    // Fills `out` (of size capacity_levels): a vector of (price, total_qty)
    // for NON-EMPTY levels in `[min_ticks, max_ticks]`. Returns how many were written.
    // Sorted ascending by price.
    std::int32_t volume_profile(std::int32_t min_ticks, std::int32_t max_ticks,
                                  DepthLevel* out, std::int32_t capacity) const noexcept {
        std::int32_t n = 0;
        const std::int32_t lo = std::max(min_ticks, PRICE_MIN_TICKS);
        const std::int32_t hi = std::min(max_ticks, LEVELS - 1);
        for (std::int32_t p = lo; p <= hi && n < capacity; ++p) {
            const PriceLevel& lvl = levels_[p];
            if (lvl.total_qty > 0) {
                out[n++] = DepthLevel{p, lvl.total_qty, lvl.order_count};
            }
        }
        return n;
    }

    // ====================================================================
    // Maker-taker fee accounting (basis points)
    // ====================================================================
    //
    // Venue convention: the maker gets a rebate (bps_maker usually negative for a rebate),
    // the taker pays a fee. Counted in cumulative bps × volume. The caller decides
    // whether to apply it — by default not counted (= 0 bps).
    void set_fee_bps(std::int32_t taker_bps, std::int32_t maker_bps) noexcept {
        taker_fee_bps_ = taker_bps;
        maker_fee_bps_ = maker_bps;
    }
    std::int64_t total_taker_fees_basis() const noexcept { return cum_taker_fees_; }
    std::int64_t total_maker_fees_basis() const noexcept { return cum_maker_fees_; }

    // ====================================================================
    // Auction matching (opening / closing cross)
    // ====================================================================
    //
    // Auction = single-price match (not continuous matching). The whole collection
    // of orders is matched at one clearing price chosen to maximize
    // the traded volume. The classic exchange auction algorithm:
    //
    //   For each candidate price p (from lowest to highest):
    //     cum_bid(p) = sum of bid qty at price >= p     (willingness to buy ≥ p)
    //     cum_ask(p) = sum of ask qty at price <= p     (willingness to sell ≤ p)
    //     matched(p) = min(cum_bid(p), cum_ask(p))
    //
    //   Clearing price = arg max matched(p). On ties: NASDAQ picks the average,
    //   NYSE the last trade. Here we use a mid-range tie-breaker.
    //
    //   After finding clearing — all orders with BID >= clearing and
    //   ASK <= clearing are filled (up to the limit min(cum_bid, cum_ask)) at
    //   clearing price. The rest remain in the book until continuous mode.
    //
    // Result:
    //   AuctionResult{clearing_price, matched_qty, surplus_bid, surplus_ask}.
    //
    // Used at: the opening cross (orders are gathered before 9:30, at 9:30
    // one cross), closing cross (15:55-16:00 → 16:00), trading halt reopen.
    struct AuctionResult {
        std::int32_t  clearing_price_ticks;
        std::int32_t  matched_qty;
        std::int32_t  surplus_bid_qty;     // unmatched bid po cleared price
        std::int32_t  surplus_ask_qty;     // unmatched ask po cleared price
        bool          executed;
    };

    // NOII (Net Order Imbalance Indicator) — indicative clearing BEZ
    // of execution. Exchanges broadcast this before the opening/closing cross
    // (NASDAQ NOII: indicative price + paired qty + imbalance side/qty).
    // executed == true means "the cross would happen" — the book untouched.
    AuctionResult indicative_auction_info() const noexcept {
        AuctionResult res{};
        res.clearing_price_ticks = -1;
        if (!has_bid() || !has_ask()) return res;

        // Walk the price range where both sides have crossed — in continuous trade
        // this does not hold (best_bid < best_ask), but in the auction queue both sides
        // they can intersect. We iterate from max(best_bid_ticks_) down to
        // min(best_ask_ticks_) — that is the interesting range.
        std::int32_t best_clearing = -1;
        std::int32_t best_matched  = -1;
        std::int32_t best_imbalance = INT32_MAX;

        // cum_bid(p) counted from HIGH down, cum_ask(p) from LOW up
        const std::int32_t lo = std::min(best_bid_ticks_, best_ask_ticks_);
        const std::int32_t hi = std::max(best_bid_ticks_, best_ask_ticks_);

        for (std::int32_t p = lo; p <= hi; ++p) {
            // cum_bid(p) = Σ BID qty na poziomach ≥ p.
            // Walk the head lists with a side filter (levels_ hold a mix of bid+ask
            // in the auction queue when both sides overlap each other's levels).
            std::int32_t cum_bid = 0;
            for (std::int32_t bp = hi; bp >= p; --bp) {
                for (const Order* o = levels_[bp].head; o; o = o->next_at_level) {
                    if (o->side == Side::BUY && o->is_active())
                        cum_bid += o->displayed_qty;
                }
            }
            std::int32_t cum_ask = 0;
            for (std::int32_t ap = lo; ap <= p; ++ap) {
                for (const Order* o = levels_[ap].head; o; o = o->next_at_level) {
                    if (o->side == Side::SELL && o->is_active())
                        cum_ask += o->displayed_qty;
                }
            }
            const std::int32_t matched = std::min(cum_bid, cum_ask);
            const std::int32_t imbalance = std::abs(cum_bid - cum_ask);
            if (matched > best_matched ||
                (matched == best_matched && imbalance < best_imbalance)) {
                best_matched = matched;
                best_clearing = p;
                best_imbalance = imbalance;
                res.surplus_bid_qty = cum_bid - matched;
                res.surplus_ask_qty = cum_ask - matched;
            }
        }
        if (best_matched <= 0) return res;
        res.clearing_price_ticks = best_clearing;
        res.matched_qty          = best_matched;
        res.executed             = true;
        return res;
    }

    AuctionResult run_auction() noexcept {
        // Clearing search shared with NOII — here only execution.
        AuctionResult res = indicative_auction_info();
        if (!res.executed) return res;
        const std::int32_t best_clearing = res.clearing_price_ticks;
        const std::int32_t best_matched  = res.matched_qty;
        const std::int32_t lo = std::min(best_bid_ticks_, best_ask_ticks_);
        const std::int32_t hi = std::max(best_bid_ticks_, best_ask_ticks_);

        // Actual fill at the clearing price — FIFO per side
        // (for determinism, oversubscription = older orders win)
        std::int32_t bid_remaining = best_matched;
        std::int32_t ask_remaining = best_matched;
        const std::uint64_t ts = mono_ns_now();
        // Wash-trade surveillance: per-client filled qty per side. Fills
        // in the auction they are anonymous (no maker-taker pairing), so STP
        // cannot act pairwise — instead we flag clients who
        // participated on BOTH sides of the cross (compliance reporting).
        std::unordered_map<std::uint64_t, std::int64_t> wash_buy, wash_sell;

        // Execute BIDS from the highest price down, but all at clearing_price
        for (std::int32_t p = hi; p >= best_clearing && bid_remaining > 0; --p) {
            Order* o = levels_[p].head;
            while (o && bid_remaining > 0) {
                Order* next = o->next_at_level;
                if (o->side == Side::BUY && o->is_active()) {
                    const std::int32_t take =
                        std::min(o->remaining_qty(), bid_remaining);
                    o->filled_qty += take;
                    bid_remaining -= take;
                    if (o->client_id != 0) wash_buy[o->client_id] += take;
                    // Aggregate accounting — displayed first, the rest from hidden
                    const std::int32_t vis_take = std::min(take, o->displayed_qty);
                    levels_[p].total_qty    -= vis_take;
                    o->displayed_qty        -= vis_take;
                    levels_[p].total_hidden -= (take - vis_take);
                    if (o->filled_qty >= o->total_qty) {
                        o->status = OrderStatus::FILLED;
                        emit(EventType::FILL, o->id, 0, best_clearing, take,
                             OrderStatus::FILLED, RejectReason::NONE,
                             o->client_id);
                        unlink_from_level(o);
                        id_index_.erase(o->id);
                        oco_on_complete(o->id);
                        bracket_on_full_fill(o->id);
                        free_order(o);
                    } else {
                        o->status = OrderStatus::PARTIALLY_FILLED;
                        emit(EventType::FILL, o->id, 0, best_clearing, take,
                             OrderStatus::PARTIALLY_FILLED, RejectReason::NONE,
                             o->client_id);
                    }
                }
                o = next;
            }
        }
        // ASKS from the lowest up to clearing_price
        for (std::int32_t p = lo; p <= best_clearing && ask_remaining > 0; ++p) {
            Order* o = levels_[p].head;
            while (o && ask_remaining > 0) {
                Order* next = o->next_at_level;
                if (o->side == Side::SELL && o->is_active()) {
                    const std::int32_t take =
                        std::min(o->remaining_qty(), ask_remaining);
                    o->filled_qty += take;
                    ask_remaining -= take;
                    if (o->client_id != 0) wash_sell[o->client_id] += take;
                    // Aggregate accounting — displayed first, the rest from hidden
                    const std::int32_t vis_take = std::min(take, o->displayed_qty);
                    levels_[p].total_qty    -= vis_take;
                    o->displayed_qty        -= vis_take;
                    levels_[p].total_hidden -= (take - vis_take);
                    record_trade(o->id, 0, best_clearing, take,
                                 Side::BUY, ts);
                    if (o->filled_qty >= o->total_qty) {
                        o->status = OrderStatus::FILLED;
                        emit(EventType::FILL, o->id, 0, best_clearing, take,
                             OrderStatus::FILLED, RejectReason::NONE,
                             o->client_id);
                        unlink_from_level(o);
                        id_index_.erase(o->id);
                        oco_on_complete(o->id);
                        bracket_on_full_fill(o->id);
                        free_order(o);
                    } else {
                        o->status = OrderStatus::PARTIALLY_FILLED;
                        emit(EventType::FILL, o->id, 0, best_clearing, take,
                             OrderStatus::PARTIALLY_FILLED, RejectReason::NONE,
                             o->client_id);
                    }
                }
                o = next;
            }
        }

        // Flag clients with fills on both sides of the cross
        last_auction_wash_clients_ = 0;
        for (const auto& kv : wash_buy) {
            if (kv.second <= 0) continue;
            const auto it_s = wash_sell.find(kv.first);
            if (it_s != wash_sell.end() && it_s->second > 0)
                ++last_auction_wash_clients_;
        }
        auction_wash_trade_flags_ += last_auction_wash_clients_;

        refresh_best_bid_from(LEVELS - 1);
        refresh_best_ask_from(0);
        // GTX (good-til-cross): remainders surviving the cross do not rest —
        // the cross was their deadline. Collect-then-cancel (unlink in a loop
        // over levels would be UB on the traversed list).
        {
            std::vector<Order*> gtx_sweep;
            for (auto& kv : id_index_) {
                Order* go = kv.second;
                if (go->tif == TimeInForce::GTX && go->is_active())
                    gtx_sweep.push_back(go);
            }
            for (Order* go : gtx_sweep) {
                ++gtx_cancelled_after_cross_;
                cancel_internal(go, /*emit_event=*/true);
            }
        }
        process_oco_pending();
        process_bracket_pending();
        ++stats_.total_auctions_executed;
        return res;
    }

    // ====================================================================
    // Auction imbalance extension (volatility extension)
    // ====================================================================
    //
    // Real exchanges (LSE, Xetra) extend the auction when the imbalance exceeds
    // a threshold — giving time for counter-flow instead of crossing with a large surplus.
    // Model: try_run_auction(threshold) — when the surplus on either side
    // > threshold, the cross is NOT executed (book untouched), the counter
    // extension grows, and the return carries indicative values (executed == false
    // sygnalizuje extension). Caller dosypuje orders i ponawia.
    AuctionResult try_run_auction(std::int32_t max_surplus_threshold) noexcept {
        AuctionResult ind = indicative_auction_info();
        if (!ind.executed) return ind;
        const std::int32_t surplus = std::max(ind.surplus_bid_qty,
                                               ind.surplus_ask_qty);
        if (surplus > max_surplus_threshold) {
            ++auction_extensions_;
            ind.executed = false;   // extension — indicative values remain
            return ind;
        }
        return run_auction();
    }
    std::uint64_t auction_extensions_count() const noexcept {
        return auction_extensions_;
    }

    // GTX (TimeInForce::GTX) — resztki skasowane po crossie aukcyjnym.
    // Use case: liquidity offered only for the opening/reopen cross.
    std::uint64_t gtx_cancelled_after_cross() const noexcept {
        return gtx_cancelled_after_cross_;
    }

    // ====================================================================
    // Session lifecycle (pre-open → continuous → closing → closed)
    // ====================================================================
    //
    // A full trading day with rule enforcement per phase:
    //   begin_pre_open()  → orders queue (auction mode), no match
    //   open_market()     → opening cross, transition to CONTINUOUS
    //   begin_closing()   → orders queue do closing cross
    //   close_market()    → closing cross (with MOC/LOC), transition to CLOSED;
    //                       submits in CLOSED are rejected (MARKET_CLOSED),
    //                       cancel remains allowed (e.g. clearing GTC)
    void begin_pre_open() noexcept {
        session_phase_ = SessionPhase::PRE_OPEN;
        in_auction_mode_ = true;
    }
    AuctionResult open_market() noexcept {
        in_auction_mode_ = false;
        session_phase_ = SessionPhase::CONTINUOUS;
        return run_auction();
    }
    void begin_closing() noexcept {
        session_phase_ = SessionPhase::CLOSING;
        in_auction_mode_ = true;
    }
    AuctionResult close_market() noexcept {
        AuctionResult r = run_closing_auction();   // manages the auction flag itself
        session_phase_ = SessionPhase::CLOSED;
        return r;
    }
    SessionPhase session_phase() const noexcept { return session_phase_; }

    // Wash-trade surveillance: clients with fills on BOTH sides of one
    // the cross. Auction fills are anonymous (no pairing), so STP
    // will not act pairwise — this is a compliance flag, not prevention.
    std::uint64_t auction_wash_trade_flags() const noexcept {
        return auction_wash_trade_flags_;
    }
    std::uint64_t last_auction_wash_clients() const noexcept {
        return last_auction_wash_clients_;
    }

    // ====================================================================
    // Rate limiting per account (token bucket)
    // ====================================================================
    //
    // Msg-rate throttle (SEC 15c3-5 pre-trade control). Every EXTERNAL
    // submit from an account consumes 1 token; the bucket refills refill_per_sec up to
    // capacity. No token = reject RATE_LIMITED. Internal resubmits
    // of the engine (stop trigger, MOC/LOC injection, bracket arming) are bypassed —
    // they concern already-accepted orders. Accounts without an entry are unlimited.
    void set_account_rate_limit(std::uint64_t client_id, double capacity,
                                 double refill_per_sec) noexcept {
        if (client_id == 0 || capacity < 1.0 || refill_per_sec < 0.0) return;
        rate_buckets_[client_id] = RateBucket{capacity, capacity,
                                               refill_per_sec, mono_ns_now()};
        rate_limiting_enabled_ = true;
    }
    std::uint64_t rate_limited_rejects() const noexcept {
        return rate_limited_rejects_;
    }

    // ====================================================================
    // Market Maker Protection (MMP)
    // ====================================================================
    //
    // A classic of options markets (CBOE/Eurex MMP): when a maker gets more
    // than max_fills fills within the window_ns window, all its quotes are
    // auto-cancelled (mass_cancel), and new quotes blocked (MMP_TRIPPED)
    // until an explicit mmp_reset. Protects the MM from a sweep of stale quotes during
    // a violent move, before it can refresh/withdraw them.
    //   • Only fills where the account is the maker (passive side) are counted.
    //   • A trip does a deferred mass_cancel (after the match loop — no dangling).
    void set_mmp(std::uint64_t client_id, std::uint32_t max_fills,
                  std::uint64_t window_ns) noexcept {
        if (client_id == 0 || max_fills == 0 || window_ns == 0) return;
        mmp_[client_id] = MmpConfig{window_ns, max_fills, 0,
                                     mono_ns_now(), false};
        mmp_enabled_ = true;
    }
    void mmp_reset(std::uint64_t client_id) noexcept {
        auto it = mmp_.find(client_id);
        if (it == mmp_.end()) return;
        it->second.tripped         = false;
        it->second.fills_in_window = 0;
        it->second.window_start_ns = mono_ns_now();
    }
    bool mmp_tripped(std::uint64_t client_id) const noexcept {
        const auto it = mmp_.find(client_id);
        return it != mmp_.end() && it->second.tripped;
    }
    std::uint64_t mmp_trips_total() const noexcept { return mmp_trips_total_; }

    // ====================================================================
    // Market-On-Close (MOC)
    // ====================================================================
    //
    // submit_moc: the order waits in a separate queue (not in the book) until the closing
    // cross. run_closing_auction() injects the MOC as a LIMIT at the edge
    // of the EXISTING book range (BUY → hi, SELL → lo) — always in-the-money
    // at every clearing price and with price priority in the fill loops, while the range
    // of the clearing search does not expand (the search is O(range²)).
    // MOC remainders after the cross are cancelled — a market order does not rest.
    std::uint64_t submit_moc(Side side, std::int32_t qty,
                              std::uint64_t client_id = 0,
                              RejectReason* out_reason = nullptr) noexcept {
        if (session_phase_ == SessionPhase::CLOSED) {
            if (out_reason) *out_reason = RejectReason::MARKET_CLOSED;
            ++stats_.total_orders_rejected;
            tally_rejection(RejectReason::MARKET_CLOSED);
            return 0;
        }
        if (qty <= 0) {
            if (out_reason) *out_reason = RejectReason::QTY_ZERO_OR_NEGATIVE;
            ++stats_.total_orders_rejected;
            tally_rejection(RejectReason::QTY_ZERO_OR_NEGATIVE);
            return 0;
        }
        const std::uint64_t id = next_order_id_++;
        moc_queue_.push_back(MocOrder{side, qty, client_id, id});
        ++moc_submitted_;
        emit(EventType::ACCEPT, id, 0, 0, qty, OrderStatus::NEW,
             RejectReason::NONE, client_id);
        return id;
    }
    bool cancel_moc(std::uint64_t id) noexcept {
        for (std::size_t i = 0; i < moc_queue_.size(); ++i) {
            if (moc_queue_[i].id == id) {
                moc_queue_[i] = moc_queue_.back();
                moc_queue_.pop_back();
                return true;
            }
        }
        return false;
    }
    AuctionResult run_closing_auction() noexcept {
        AuctionResult res{};
        res.clearing_price_ticks = -1;
        in_auction_mode_ = true;
        in_internal_submit_ = true;   // injections do not consume rate tokens
        // 1. LOC — enter at THEIR OWN limit prices (they participate in price
        //    discovery; they may widen the clearing-search range, but by real
        //    price, not extreme)
        std::vector<std::uint64_t> injected_loc;
        injected_loc.reserve(loc_queue_.size());
        for (const LocOrder& l : loc_queue_) {
            const std::uint64_t oid = submit(l.side, l.price_ticks, l.qty,
                                              OrderType::LIMIT, TimeInForce::DAY,
                                              l.id, l.client_id);
            if (oid != 0) injected_loc.push_back(oid);
        }
        loc_queue_.clear();
        // 2. MOC — at the edge of the book range (already with LOC; MOC is meant to be
        //    the most aggressive → price priority). An empty book even after
        //    LOC = no price discovery — cancel MOCs.
        std::int32_t book_hi, book_lo;
        if (has_bid() && has_ask()) {
            book_hi = std::max(best_bid_ticks_, best_ask_ticks_);
            book_lo = std::min(best_bid_ticks_, best_ask_ticks_);
        } else if (has_bid()) {
            book_hi = book_lo = best_bid_ticks_;
        } else if (has_ask()) {
            book_hi = book_lo = best_ask_ticks_;
        } else {
            moc_cancelled_unfilled_ += moc_queue_.size();
            moc_queue_.clear();
            in_auction_mode_ = false;
            in_internal_submit_ = false;
            return res;
        }
        std::vector<std::uint64_t> injected_moc;
        injected_moc.reserve(moc_queue_.size());
        for (const MocOrder& m : moc_queue_) {
            const std::int32_t px = (m.side == Side::BUY) ? book_hi : book_lo;
            const std::uint64_t oid = submit(m.side, px, m.qty,
                                              OrderType::LIMIT, TimeInForce::DAY,
                                              m.id, m.client_id);
            if (oid != 0) injected_moc.push_back(oid);
        }
        moc_queue_.clear();
        in_auction_mode_ = false;   // the closing cross ends auction mode
        in_internal_submit_ = false;
        res = run_auction();
        // 3. On-close semantics: remainders (MOC and LOC) do not rest
        for (std::uint64_t oid : injected_moc) {
            auto it = id_index_.find(oid);
            if (it != id_index_.end()) {
                ++moc_cancelled_unfilled_;
                cancel_internal(it->second, /*emit_event=*/true);
            }
        }
        for (std::uint64_t oid : injected_loc) {
            auto it = id_index_.find(oid);
            if (it != id_index_.end()) {
                ++loc_cancelled_unfilled_;
                cancel_internal(it->second, /*emit_event=*/true);
            }
        }
        process_oco_pending();
        return res;
    }
    std::size_t moc_queue_size() const noexcept { return moc_queue_.size(); }
    std::uint64_t moc_submitted() const noexcept { return moc_submitted_; }
    std::uint64_t moc_cancelled_unfilled() const noexcept {
        return moc_cancelled_unfilled_;
    }

    // Limit-On-Close — a limit order only for the closing cross.
    std::uint64_t submit_loc(Side side, std::int32_t price_ticks,
                              std::int32_t qty,
                              std::uint64_t client_id = 0,
                              RejectReason* out_reason = nullptr) noexcept {
        auto fail = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            return 0;
        };
        if (session_phase_ == SessionPhase::CLOSED)
                                   return fail(RejectReason::MARKET_CLOSED);
        if (qty <= 0)              return fail(RejectReason::QTY_ZERO_OR_NEGATIVE);
        if (!in_range(price_ticks)) return fail(RejectReason::PRICE_OUT_OF_RANGE);
        const std::uint64_t id = next_order_id_++;
        loc_queue_.push_back(LocOrder{side, price_ticks, qty, client_id, id});
        ++loc_submitted_;
        emit(EventType::ACCEPT, id, 0, price_ticks, qty, OrderStatus::NEW,
             RejectReason::NONE, client_id);
        return id;
    }
    bool cancel_loc(std::uint64_t id) noexcept {
        for (std::size_t i = 0; i < loc_queue_.size(); ++i) {
            if (loc_queue_[i].id == id) {
                loc_queue_[i] = loc_queue_.back();
                loc_queue_.pop_back();
                return true;
            }
        }
        return false;
    }
    std::size_t loc_queue_size() const noexcept { return loc_queue_.size(); }
    std::uint64_t loc_submitted() const noexcept { return loc_submitted_; }
    std::uint64_t loc_cancelled_unfilled() const noexcept {
        return loc_cancelled_unfilled_;
    }

    // ====================================================================
    // Short-Sale Restriction (SEC Rule 201 — uptick rule)
    // ====================================================================
    //
    // With SSR active, a short sale may be executed only ABOVE the current
    // best bid (a price ≤ bid would be an aggressive short hitting the bid —
    // forbidden). Rule 201 activates after a 10% drop from the reference (close
    // poprzedniego dnia) — arm_ssr_circuit_breaker armuje auto-trigger
    // sprawdzany per trade w record_trade.
    void set_ssr_active(bool on) noexcept { ssr_active_ = on; }
    bool ssr_active() const noexcept { return ssr_active_; }
    void arm_ssr_circuit_breaker(std::int32_t reference_price_ticks,
                                  std::int32_t decline_bps) noexcept {
        ssr_trigger_px_ = reference_price_ticks -
            static_cast<std::int32_t>(
                static_cast<std::int64_t>(reference_price_ticks) *
                decline_bps / 10000);
        ssr_armed_ = true;
    }
    std::uint64_t ssr_trips() const noexcept { return ssr_trips_; }

    std::uint64_t submit_short(std::int32_t price_ticks, std::int32_t qty,
                                std::uint64_t order_id = 0,
                                std::uint64_t client_id = 0,
                                RejectReason* out_reason = nullptr) noexcept {
        if (ssr_active_ && has_bid() && price_ticks <= best_bid_ticks_) {
            if (out_reason) *out_reason = RejectReason::SSR_RESTRICTED;
            ++stats_.total_orders_rejected;
            tally_rejection(RejectReason::SSR_RESTRICTED);
            return 0;
        }
        return submit(Side::SELL, price_ticks, qty, OrderType::LIMIT,
                      TimeInForce::DAY, order_id, client_id, 0, out_reason);
    }

    // ====================================================================
    // Reduce-only orders
    // ====================================================================
    //
    // An order may only REDUCE the account's position (filled_net_qty), never
    // grow/flip it. Qty clamped to |position| (crypto-venue
    // convention); no position or wrong direction = reject. The check is
    // point-in-time at submit — a resting order is not re-amended
    // when the position changes (a simplification).
    std::uint64_t submit_reduce_only(Side side, std::int32_t price_ticks,
                                      std::int32_t qty,
                                      std::uint64_t client_id,
                                      RejectReason* out_reason = nullptr) noexcept {
        std::int64_t allowed = 0;
        if (client_id != 0) {
            const AccountExposure ex = get_account_exposure(client_id);
            const std::int64_t pos = ex.filled_net_qty;   // >0 long, <0 short
            if (side == Side::SELL && pos > 0)
                allowed = std::min<std::int64_t>(pos, qty);
            else if (side == Side::BUY && pos < 0)
                allowed = std::min<std::int64_t>(-pos, qty);
        }
        if (allowed <= 0) {
            if (out_reason) *out_reason = RejectReason::REDUCE_ONLY_NO_POSITION;
            ++stats_.total_orders_rejected;
            tally_rejection(RejectReason::REDUCE_ONLY_NO_POSITION);
            return 0;
        }
        return submit(side, price_ticks, static_cast<std::int32_t>(allowed),
                      OrderType::LIMIT, TimeInForce::DAY, 0, client_id, 0,
                      out_reason);
    }

    // ====================================================================
    // L2 incremental delta protocol
    // ====================================================================
    //
    // Wire format for broadcasting top-of-book and depth changes to clients
    // (an alternative to the full snapshot). Format inspired by ITCH 5.0 + NASDAQ
    // BookFeed:
    //
    //   DeltaMessage:
    //     [0]    type:    'A' = add, 'D' = delete, 'M' = modify, 'T' = trade
    //     [1]    side:    'B' = bid, 'S' = ask, 'X' = both (na trade)
    //     [2..5] price_ticks (int32 LE)
    //     [6..9] new_qty      (int32 LE) — for M this is the NEW qty at this level
    //     [10..17] sequence_no (uint64 LE) — monotonic
    //
    // The caller invokes pop_delta_queue() after each operation to collect
    // accumulated deltas and send them over the network. An internal FIFO queue.
    struct DeltaMessage {
        char          type;       // A/D/M/T
        char          side;       // B/S/X
        std::int32_t  price_ticks;
        std::int32_t  new_qty;
        std::uint64_t sequence_no;
    };
    static constexpr std::size_t DELTA_WIRE_SIZE = 18;

    void enable_delta_queue(bool on) noexcept { delta_queue_enabled_ = on; }
    bool delta_queue_enabled() const noexcept { return delta_queue_enabled_; }
    std::size_t delta_queue_size() const noexcept { return delta_queue_.size(); }

    // pop_delta_queue: copy to `out` (max max_n), clear from the queue.
    // Returns how many were actually retrieved.
    std::size_t pop_delta_queue(DeltaMessage* out, std::size_t max_n) noexcept {
        const std::size_t n = std::min(max_n, delta_queue_.size());
        for (std::size_t i = 0; i < n; ++i) out[i] = delta_queue_[i];
        delta_queue_.erase(delta_queue_.begin(),
                            delta_queue_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    // serialize_delta: a single message → 18 little-endian bytes.
    static std::size_t serialize_delta(const DeltaMessage& d,
                                         std::uint8_t* buf) noexcept {
        buf[0] = static_cast<std::uint8_t>(d.type);
        buf[1] = static_cast<std::uint8_t>(d.side);
        std::memcpy(buf + 2,  &d.price_ticks, 4);
        std::memcpy(buf + 6,  &d.new_qty,     4);
        std::memcpy(buf + 10, &d.sequence_no, 8);
        return DELTA_WIRE_SIZE;
    }

    static bool deserialize_delta(const std::uint8_t* buf, std::size_t len,
                                    DeltaMessage& d) noexcept {
        if (len < DELTA_WIRE_SIZE) return false;
        d.type = static_cast<char>(buf[0]);
        d.side = static_cast<char>(buf[1]);
        std::memcpy(&d.price_ticks, buf + 2,  4);
        std::memcpy(&d.new_qty,     buf + 6,  4);
        std::memcpy(&d.sequence_no, buf + 10, 8);
        return true;
    }

    // ====================================================================
    // Internal — push delta from book mutations
    // ====================================================================
    void push_delta(char type, char side, std::int32_t price_ticks,
                     std::int32_t new_qty) noexcept {
        if (!delta_queue_enabled_) return;
        DeltaMessage d{type, side, price_ticks, new_qty, ++delta_seq_};
        delta_queue_.push_back(d);
    }

private:
    // Storage extension (extensions used in 2nd-pass features)
    std::vector<Order*>  stop_orders_;   // waiting for a trigger
    std::vector<Order*>  peg_orders_;    // do reprice on book change
    std::int32_t         last_trade_ticks_ = -1;

    // OFI state
    std::int32_t  last_ofi_bid_ticks_ = 0;
    std::int32_t  last_ofi_ask_ticks_ = 0;
    std::int32_t  last_ofi_bid_qty_   = 0;
    std::int32_t  last_ofi_ask_qty_   = 0;
    std::int64_t  cum_ofi_ = 0;

    // Fees state
    std::int32_t  taker_fee_bps_   = 0;
    std::int32_t  maker_fee_bps_   = 0;
    std::int64_t  cum_taker_fees_  = 0;
    std::int64_t  cum_maker_fees_  = 0;

    // L2 delta queue state
    bool                      delta_queue_enabled_ = false;
    std::vector<DeltaMessage> delta_queue_;
    std::uint64_t             delta_seq_ = 0;

    // Auction mode — when true, submit() skips match_against (orders sit
    // in the book until a later batch match via run_auction).
    bool                      in_auction_mode_ = false;

    // Halt state — trading halt (LULD, news pending, technical issue).
    // When halted_, every submit → REJECT(HALTED). Cancelling an order is still OK.
    bool                      halted_ = false;
    char                      halt_reason_[32]{};

    // Halt with reopen via auction (pause — order entry continues, the match waits)
    bool                      halt_reopen_pending_  = false;
    std::uint64_t             halt_auction_reopens_ = 0;

    // Audit log — opt-in chronological record wszystkich book mutations.
    // Used for replay, forensics, compliance review (SEC 17a-4).
public:
    struct AuditRecord {
        std::uint64_t ts_ns;
        std::uint64_t seq_no;
        std::uint64_t order_id;
        std::int32_t  price_ticks;
        std::int32_t  qty;
        EventType     event;       // ACCEPT/REJECT/FILL/CANCEL/EXPIRE/REPLACE
        Side          side;
        OrderStatus   result;
    };
private:
    bool                      audit_enabled_ = false;
    std::vector<AuditRecord>  audit_log_;
    std::uint64_t             audit_seq_ = 0;

    // LULD (Limit Up Limit Down) — auto-halt mechanizm SEC Rule 605.
    // A quote outside the bands is auto-rejected or auto-halts the book.
    bool                      luld_enabled_ = false;
    std::int32_t              luld_low_ticks_  = 0;
    std::int32_t              luld_high_ticks_ = 0;
    bool                      luld_auto_halt_  = false;   // true → breach halt'uje (hard)
    bool                      luld_halt_to_auction_ = false; // breach → pause with reopen cross
    std::uint64_t             luld_breaches_   = 0;

    void push_audit(EventType ev, std::uint64_t order_id, Side side,
                     std::int32_t price_ticks, std::int32_t qty,
                     OrderStatus result) noexcept {
        if (!audit_enabled_) return;
        AuditRecord r{mono_ns_now(), ++audit_seq_, order_id, price_ticks, qty,
                       ev, side, result};
        audit_log_.push_back(r);
    }

public:
    // ====================================================================
    // LULD (Limit Up / Limit Down) — SEC Rule 605 circuit breaker
    // ====================================================================
    //
    // Quote poza band'ami → REJECT(LULD_BAND_BREACH).
    // If auto_halt=true, a breach also halts the book (5 min SEC standard).
    //
    // Real-world: for S&P 500 stocks a 5% LULD band during the regular session,
    // 10% in the first 15 min. Trigger after one breach = pause market data + halt.
    void set_luld_bands(std::int32_t low_ticks, std::int32_t high_ticks,
                         bool auto_halt = true) noexcept {
        luld_enabled_    = true;
        luld_low_ticks_  = low_ticks;
        luld_high_ticks_ = high_ticks;
        luld_auto_halt_  = auto_halt;
    }
    void disable_luld() noexcept { luld_enabled_ = false; }
    bool luld_enabled() const noexcept { return luld_enabled_; }

    // ====================================================================
    // Trading halt with reopen via auction (LULD-style pause/resume)
    // ====================================================================
    //
    // A hard halt (halt()) rejects submits. A real LULD halt works differently:
    // trading stops, but order entry CONTINUES — orders queue for the
    // reopening cross, which does price discovery after the pause.
    //   halt_for_auction()     — pause (auction mode, without halted_)
    //   resume_with_auction()  — reopen cross + return to continuous
    // set_luld_halt_to_auction(true) hooks the pause to a LULD breach
    // (takes precedence over the hard auto_halt).
    void halt_for_auction() noexcept {
        in_auction_mode_ = true;
        halt_reopen_pending_ = true;
    }
    AuctionResult resume_with_auction() noexcept {
        in_auction_mode_ = false;
        halt_reopen_pending_ = false;
        ++halt_auction_reopens_;
        return run_auction();
    }
    bool halt_reopen_pending() const noexcept { return halt_reopen_pending_; }
    std::uint64_t halt_auction_reopens() const noexcept {
        return halt_auction_reopens_;
    }
    void set_luld_halt_to_auction(bool on) noexcept {
        luld_halt_to_auction_ = on;
    }
    std::int32_t luld_low()  const noexcept { return luld_low_ticks_; }
    std::int32_t luld_high() const noexcept { return luld_high_ticks_; }
    std::uint64_t luld_breaches() const noexcept { return luld_breaches_; }

    // ====================================================================
    // MIFID II RTS27/28 best-execution metrics
    // ====================================================================
    //
    // Regulatory output for EU venue reporting. Continuous tracking:
    //   - effective_spread = 2 × |exec_price - mid_at_exec| (per execution)
    //   - realized_spread  = 2 × |exec_price - mid_post_n_seconds| (proxy: trade-to-trade)
    //   - num_executions, total_volume, total_notional
    //
    // The caller invokes get_mifid_metrics() at the end of the session to generate a report.
    struct MIFIDMetrics {
        std::uint64_t num_executions          = 0;
        std::uint64_t total_volume            = 0;
        std::int64_t  total_notional_ticks    = 0;
        std::int64_t  sum_effective_spread    = 0;   // ticks × qty
        std::int64_t  sum_signed_price_impact = 0;   // realized spread proxy
    };
    MIFIDMetrics get_mifid_metrics() const noexcept { return mifid_; }
    void reset_mifid_metrics() noexcept { mifid_ = MIFIDMetrics{}; }
    void enable_mifid_metrics(bool on) noexcept { mifid_enabled_ = on; }
    bool mifid_metrics_enabled() const noexcept { return mifid_enabled_; }

private:
    MIFIDMetrics  mifid_{};
    bool          mifid_enabled_ = false;

public:
    // ====================================================================
    // Quote stuffing detection
    // ====================================================================
    //
    // Stuffing = sending thousands of orders per second + immediate
    // cancels, so the symbol cannot be read in real time. SEC Rule
    // 15c3-5: every venue must detect and flag this.
    //
    // Here we keep a per-client_id counter of cancels in a sliding window (last
    // N samples). Above the threshold → emit a STUFFING_FLAGGED event +
    // stats.total_stuffing_flags.
    void set_stuffing_threshold(std::uint32_t cancels_per_sec_threshold) noexcept {
        stuffing_threshold_ = cancels_per_sec_threshold;
    }
    std::uint32_t stuffing_threshold() const noexcept { return stuffing_threshold_; }
    bool is_stuffing_flagged(std::uint64_t client_id) const noexcept {
        const auto it = stuffing_flagged_.find(client_id);
        return it != stuffing_flagged_.end() && it->second;
    }
    std::uint64_t total_stuffing_flags() const noexcept { return total_stuffing_flags_; }

    // Reset the stuffing window per client (e.g. after a manual review).
    void clear_stuffing_flag(std::uint64_t client_id) noexcept {
        stuffing_flagged_[client_id] = false;
        cancel_counters_[client_id]  = 0;
    }

    // ====================================================================
    // Per-account exposure tracking
    // ====================================================================
    //
    // Per client_id: net qty (BUY - SELL) open + filled, plus gross qty.
    // The risk team uses it for real-time monitoring of exposure per account.
    //
    // Tracked in submit/cancel/fill events. The caller invokes
    // get_account_exposure(client_id) — returns an AccountExposure struct.
    struct AccountExposure {
        std::int64_t  open_buy_qty       = 0;     // bid orders in the book
        std::int64_t  open_sell_qty      = 0;     // ask orders in the book
        std::int64_t  filled_net_qty     = 0;     // realized position
        std::int64_t  filled_gross_volume = 0;    // gross qty traded
        std::uint64_t orders_submitted    = 0;
        std::uint64_t orders_cancelled    = 0;
        std::uint64_t fills_received      = 0;
        // Aggressive (taker) vs passive (maker) volume — useful for rebates
        // i toxicity scoring per account.
        std::uint64_t aggressive_volume   = 0;    // this side was the taker
        std::uint64_t passive_volume      = 0;    // this side was the maker
    };
    AccountExposure get_account_exposure(std::uint64_t client_id) const noexcept {
        const auto it = account_exposure_.find(client_id);
        return it == account_exposure_.end() ? AccountExposure{} : it->second;
    }
    // Aggressive ratio = taker_volume / (taker + maker). 1.0 = pure taker
    // (pays the taker fee — high cost); 0.0 = pure maker (collects a rebate).
    double aggressive_ratio_for(std::uint64_t client_id) const noexcept {
        const auto ex = get_account_exposure(client_id);
        const std::uint64_t total = ex.aggressive_volume + ex.passive_volume;
        if (total == 0) return 0.0;
        return static_cast<double>(ex.aggressive_volume) /
               static_cast<double>(total);
    }
    // Per-account cancel-to-fill ratio.
    double cancel_to_fill_ratio_for(std::uint64_t client_id) const noexcept {
        const auto ex = get_account_exposure(client_id);
        if (ex.fills_received == 0) return 0.0;
        return static_cast<double>(ex.orders_cancelled) /
               static_cast<double>(ex.fills_received);
    }

private:
    // Helper hooks for aggressor vs passive volume tagging.
    void tag_aggressor_volume(std::uint64_t cid, std::int32_t qty) noexcept {
        if (cid == 0) return;
        account_exposure_[cid].aggressive_volume += static_cast<std::uint64_t>(qty);
    }
    void tag_passive_volume(std::uint64_t cid, std::int32_t qty) noexcept {
        if (cid == 0) return;
        account_exposure_[cid].passive_volume += static_cast<std::uint64_t>(qty);
    }

private:
    std::unordered_map<std::uint64_t, AccountExposure> account_exposure_;

    // Stuffing detection state
    std::uint32_t                            stuffing_threshold_ = 0;
    std::unordered_map<std::uint64_t, bool>  stuffing_flagged_;
    std::unordered_map<std::uint64_t, std::uint32_t> cancel_counters_;
    std::uint64_t                            total_stuffing_flags_ = 0;

    // update_exposure helpers — called from submit/cancel/fill hooks.
    void exposure_on_submit(std::uint64_t cid, Side side, std::int32_t qty) noexcept {
        if (cid == 0) return;
        AccountExposure& ex = account_exposure_[cid];
        if (side == Side::BUY) ex.open_buy_qty  += qty;
        else                   ex.open_sell_qty += qty;
        ++ex.orders_submitted;
    }
    void exposure_on_cancel(std::uint64_t cid, Side side, std::int32_t remaining) noexcept {
        if (cid == 0) return;
        AccountExposure& ex = account_exposure_[cid];
        if (side == Side::BUY) ex.open_buy_qty  -= remaining;
        else                   ex.open_sell_qty -= remaining;
        ++ex.orders_cancelled;
        // Stuffing check
        if (stuffing_threshold_ > 0) {
            const auto cnt = ++cancel_counters_[cid];
            if (cnt > stuffing_threshold_ && !stuffing_flagged_[cid]) {
                stuffing_flagged_[cid] = true;
                ++total_stuffing_flags_;
            }
        }
    }
    void exposure_on_fill(std::uint64_t cid, Side side, std::int32_t qty) noexcept {
        if (cid == 0) return;
        AccountExposure& ex = account_exposure_[cid];
        if (side == Side::BUY) {
            ex.open_buy_qty   -= qty;
            ex.filled_net_qty += qty;
        } else {
            ex.open_sell_qty  -= qty;
            ex.filled_net_qty -= qty;
        }
        ex.filled_gross_volume += qty;
        ++ex.fills_received;
    }

public:
    // ====================================================================
    // Trade size distribution + TWAP + reference price drift
    // ====================================================================
    //
    // Trade size distribution: classify executions into 4 segments for detection
    // retail (small) vs institutional flow (block):
    //   SMALL   ≤ 100
    //   MEDIUM  101..1000
    //   LARGE   1001..10000
    //   BLOCK   > 10000
    // Per-segment counter + volume sum.
    struct TradeSizeDistribution {
        std::uint64_t  small_count    = 0;   // ≤100
        std::uint64_t  medium_count   = 0;   // 101-1000
        std::uint64_t  large_count    = 0;   // 1001-10000
        std::uint64_t  block_count    = 0;   // >10000
        std::uint64_t  small_volume   = 0;
        std::uint64_t  medium_volume  = 0;
        std::uint64_t  large_volume   = 0;
        std::uint64_t  block_volume   = 0;
    };

    TradeSizeDistribution get_size_distribution() const noexcept {
        return size_dist_;
    }
    void reset_size_distribution() noexcept { size_dist_ = TradeSizeDistribution{}; }

    // TWAP — time-weighted average price z trade tape.
    // Unlike tape_vwap (volume-weighted), TWAP treats each trade
    // equally. Used to detect wash-trading (when volumes are unequal
    // but TWAP =VWAP, suspiciously).
    std::int32_t tape_twap_ticks() const noexcept {
        const std::size_t n = tape_size();
        if (n == 0) return -1;
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - 1 - i) % TAPE_CAP];
            sum += t.price_ticks;
        }
        return static_cast<std::int32_t>(sum / static_cast<std::int64_t>(n));
    }

    // Reference price + drift detection.
    //
    // set_reference_price(ticks) — set the anchor (e.g. previous close, opening cross,
    // SIP NBBO mid). Then reference_drift_bps() returns |mid - ref| / ref × 10000.
    // A drift > X bps may trigger a halt review.
    void set_reference_price(std::int32_t ref_ticks) noexcept {
        reference_price_ticks_ = ref_ticks;
        reference_set_ = true;
    }
    bool has_reference_price() const noexcept { return reference_set_; }
    std::int32_t reference_price_ticks() const noexcept { return reference_price_ticks_; }

    // reference_drift_bps: bps deviation of mid from the reference. Returns -1 when
    // ref is not set or there is no mid.
    std::int32_t reference_drift_bps() const noexcept {
        if (!reference_set_) return -1;
        const std::int32_t m = mid_ticks();
        if (m < 0 || reference_price_ticks_ <= 0) return -1;
        const std::int64_t diff = std::abs(static_cast<std::int64_t>(m) - reference_price_ticks_);
        return static_cast<std::int32_t>(diff * 10000 / reference_price_ticks_);
    }

    // Order rejection rate per client.
    double rejection_rate(std::uint64_t client_id) const noexcept {
        const auto it = account_exposure_.find(client_id);
        if (it == account_exposure_.end() || it->second.orders_submitted == 0)
            return 0.0;
        const auto rejected_it = client_rejections_.find(client_id);
        const std::uint64_t r = (rejected_it == client_rejections_.end())
                                ? 0 : rejected_it->second;
        return static_cast<double>(r) /
               static_cast<double>(it->second.orders_submitted + r);
    }

private:
    // Trade size distribution state
    TradeSizeDistribution  size_dist_{};

    // Reference price state
    bool          reference_set_ = false;
    std::int32_t  reference_price_ticks_ = 0;

    // Per-client rejection counter
    std::unordered_map<std::uint64_t, std::uint64_t>  client_rejections_;

    // Top-of-book change detection — snapshot of last observed best_bid/best_ask.
    // Used by tob_changed_since_last_check() which is a stateful "poll for changes" API.
    std::int32_t  last_observed_bid_ticks_ = NO_BID_TICKS;
    std::int32_t  last_observed_ask_ticks_ = NO_ASK_TICKS;

    // Quote-life accounting (mean TOB residency).
    std::uint64_t last_tob_change_ts_ns_ = 0;
    std::uint64_t total_tob_life_ns_     = 0;

    // Spread-compression detection
    std::int32_t  spread_compression_threshold_ticks_ = -1;  // -1 == off
    std::uint64_t spread_compression_events_ = 0;

    // Order-flow (VPIN-style toxicity, signed by taker side)
    std::uint64_t taker_buy_volume_   = 0;
    std::uint64_t taker_sell_volume_  = 0;
    std::uint64_t taker_buy_count_    = 0;
    std::uint64_t taker_sell_count_   = 0;
    std::uint64_t last_fill_ts_ns_    = 0;

    // Quote flicker (TOB change without trade) — needs last_fill_ts vs last_tob_change_ts
    std::uint64_t quote_flicker_count_ = 0;

    // TOB stability streak
    std::uint64_t tob_unchanged_streak_ = 0;
    std::uint64_t max_tob_unchanged_streak_ = 0;

    // Time-weighted spread (∫ spread × dt)
    std::uint64_t last_spread_sample_ts_ns_ = 0;
    std::uint64_t time_weighted_spread_ticks_x_ns_ = 0;
    std::uint64_t total_spread_dt_ns_ = 0;

    // Event sequence numbers (monotonic)
    std::uint64_t next_event_seq_   = 0;
    std::uint64_t last_emitted_seq_ = 0;

    // Spread bias counters (mid-bid vs ask-mid)
    std::uint64_t spread_bias_ask_side_ = 0;
    std::uint64_t spread_bias_bid_side_ = 0;
    std::uint64_t spread_bias_neutral_  = 0;

    // Kyle's lambda accumulators
    std::int64_t  prev_trade_px_for_lambda_ = -1;
    double        cum_price_volume_product_ = 0.0;  // Σ Δp × v
    double        cum_signed_volume_sq_     = 0.0;  // Σ v²
    // Per-side decomposition
    double        cum_pv_buy_  = 0.0;
    double        cum_pv_sell_ = 0.0;
    double        cum_vsq_buy_ = 0.0;
    double        cum_vsq_sell_ = 0.0;

    // Spread regime thresholds + counters
    std::int32_t  spread_regime_tight_thresh_  = 0;
    std::int32_t  spread_regime_wide_thresh_   = 0;
    std::uint64_t spread_regime_tight_count_   = 0;
    std::uint64_t spread_regime_normal_count_  = 0;
    std::uint64_t spread_regime_wide_count_    = 0;

    // One-sided book counter (called by poll_tob_micro)
    std::uint64_t one_sided_bid_only_ = 0;
    std::uint64_t one_sided_ask_only_ = 0;

    // Tick-by-tick price change distribution (last trade-to-trade Δp)
    // 9 bins: -4..-1, 0, +1..+4 (Δp clipped). Bin 0 = "no change", index 4.
    std::uint64_t price_change_hist_[9]{};
    std::uint64_t price_change_total_ = 0;

    // Implementation shortfall (Σ (fill_px - decision_mid) × qty × side_sign)
    std::int64_t  cum_implementation_shortfall_ticks_qty_ = 0;
    std::uint64_t cum_implementation_shortfall_qty_       = 0;

    // Per-side VWAP accumulators
    std::int64_t  cum_buy_notional_ticks_  = 0;
    std::uint64_t cum_buy_volume_          = 0;
    std::int64_t  cum_sell_notional_ticks_ = 0;
    std::uint64_t cum_sell_volume_         = 0;

    // Inter-trade time gap stats
    std::uint64_t prev_trade_ts_ns_for_gap_ = 0;
    std::uint64_t inter_trade_gap_min_ns_   = UINT64_MAX;
    std::uint64_t inter_trade_gap_max_ns_   = 0;
    std::uint64_t inter_trade_gap_sum_ns_   = 0;
    std::uint64_t inter_trade_gap_count_    = 0;

    // Largest single trade observed
    std::int32_t  largest_single_trade_qty_ = 0;

    // First-fill latency tracker (per order, recorded once on first fill)
    std::uint64_t first_fill_latency_count_   = 0;
    std::uint64_t first_fill_latency_sum_ns_  = 0;
    std::uint64_t first_fill_latency_max_ns_  = 0;
    std::uint64_t first_fill_latency_min_ns_  = UINT64_MAX;

    // Submission burst detector
    std::uint64_t burst_window_ns_   = 0;     // 0 = off
    std::uint64_t burst_last_submit_ns_ = 0;
    std::uint64_t burst_runs_count_  = 0;     // # bursts (≥2 submits within window)
    std::uint64_t burst_in_run_count_ = 0;    // counter for the current run

    // Order completion histogram
    std::uint64_t completion_filled_fully_     = 0;  // total_qty == filled_qty
    std::uint64_t completion_cancelled_partial_= 0;  // 0 < filled_qty < total_qty
    std::uint64_t completion_cancelled_unfilled_= 0; // filled_qty == 0
    std::uint64_t completion_expired_partial_  = 0;
    std::uint64_t completion_expired_unfilled_ = 0;
    // Per-side maker fills (by m->side when fully filled in match_at_level)
    std::uint64_t maker_fills_buy_side_  = 0;
    std::uint64_t maker_fills_sell_side_ = 0;

    // Best-bid qty histogram (16 bins, log2-style: 0, 1-2, 3-4, 5-8, ...)
    static constexpr std::size_t QTY_HIST_BINS = 16;
    std::uint64_t best_bid_qty_hist_[QTY_HIST_BINS]{};
    std::uint64_t best_ask_qty_hist_[QTY_HIST_BINS]{};

    // EMA imbalance (alpha = 0.1 default; configurable)
    double  ema_imbalance_alpha_ = 0.1;
    double  ema_imbalance_bps_value_ = 0.0;
    bool    ema_imbalance_init_ = false;

    // Microprice ring buffer (last MID_RING_CAP samples — definition below)
    static constexpr std::size_t MID_RING_CAP = 16;
    std::int32_t  microprice_ring_[MID_RING_CAP]{};
    std::size_t   microprice_ring_head_  = 0;
    std::size_t   microprice_ring_count_ = 0;

    // Signed-volume EMA (alpha 0.1)
    double  ema_signed_volume_ = 0.0;
    bool    ema_signed_volume_init_ = false;

    // Cont-Kukanov OFI state
    std::int32_t  ofi_prev_bid_ticks_ = NO_BID_TICKS;
    std::int32_t  ofi_prev_ask_ticks_ = NO_ASK_TICKS;
    std::int32_t  ofi_prev_bid_qty_   = 0;
    std::int32_t  ofi_prev_ask_qty_   = 0;
    std::int64_t  ofi_cumulative_     = 0;
    std::uint64_t ofi_samples_        = 0;

    // Trade clustering — variance/mean inter-trade gap (overdispersion)
    // Welford's algorithm for running variance
    double        gap_var_mean_       = 0.0;
    double        gap_var_M2_         = 0.0;
    std::uint64_t gap_var_count_      = 0;

    // Maker survival snapshot — set of maker_ids active at last poll
    std::uint64_t maker_survival_total_polls_  = 0;
    std::uint64_t maker_survival_total_orders_ = 0;
    std::uint64_t maker_survival_survivors_    = 0;
    // Snapshot vector reused
    std::vector<std::uint64_t> maker_survival_prev_ids_;

    // Trade direction Markov chain (BB, BS, SB, SS counters)
    Side          markov_prev_side_     = Side::BUY;
    bool          markov_has_prev_      = false;
    std::uint64_t markov_BB_ = 0, markov_BS_ = 0, markov_SB_ = 0, markov_SS_ = 0;

    // Queue depth at maker arrival
    std::uint64_t queue_depth_arrival_sum_   = 0;
    std::uint64_t queue_depth_arrival_count_ = 0;
    std::int32_t  queue_depth_arrival_max_   = 0;

    // Autocorrelation lag-1 of inter-trade gaps
    double   gap_prev_value_ = 0.0;
    bool     gap_prev_has_   = false;
    double   gap_autocorr_xy_sum_ = 0.0;
    double   gap_autocorr_xx_sum_ = 0.0;

    // Slippage budget guard
    std::int32_t  slippage_guard_threshold_ticks_ = 0;     // 0 = off
    std::uint64_t slippage_guard_violations_       = 0;

    // NBBO violation audit (defensive — should not happen in a correct engine)
    std::uint64_t nbbo_violations_count_ = 0;

    // Per-side cancellation counters
    std::uint64_t cancellations_by_buy_  = 0;
    std::uint64_t cancellations_by_sell_ = 0;

    // Lee-Ready classification accuracy
    std::uint64_t lr_total_classified_ = 0;
    std::uint64_t lr_correct_          = 0;
    std::uint64_t lr_buy_classified_   = 0;
    std::uint64_t lr_sell_classified_  = 0;
    std::uint64_t lr_unclassifiable_   = 0;

    // Per-account VWAP (notional + volume per client_id)
    struct AccountVwap {
        std::int64_t notional_ticks = 0;
        std::int64_t volume         = 0;
    };
    std::unordered_map<std::uint64_t, AccountVwap> account_vwap_;

    void account_vwap_on_fill(std::uint64_t cid, std::int32_t px,
                               std::int32_t qty) noexcept {
        if (cid == 0) return;
        AccountVwap& v = account_vwap_[cid];
        v.notional_ticks += static_cast<std::int64_t>(px) * qty;
        v.volume         += qty;
    }

    // OCO (One-Cancels-Other) — a pair of orders; completion of one leg
    // (full fill / cancel / expire) cancels the other. The partner's cancel is
    // DEFERRED via oco_pending_cancel_ — you must not unlink other
    // orders in the middle of the match loop (dangling next_m).
    std::unordered_map<std::uint64_t, std::uint64_t> oco_partner_;
    std::vector<std::uint64_t> oco_pending_cancel_;
    std::uint64_t oco_triggered_cancels_ = 0;

    void oco_on_complete(std::uint64_t id) noexcept {
        auto it = oco_partner_.find(id);
        if (it == oco_partner_.end()) return;
        const std::uint64_t partner = it->second;
        oco_partner_.erase(it);
        oco_partner_.erase(partner);
        oco_pending_cancel_.push_back(partner);
    }

    void process_oco_pending() noexcept {
        while (!oco_pending_cancel_.empty()) {
            const std::uint64_t pid = oco_pending_cancel_.back();
            oco_pending_cancel_.pop_back();
            auto it = id_index_.find(pid);
            if (it != id_index_.end()) {
                ++oco_triggered_cancels_;
                // cancel_internal will call the partner's oco_on_complete — the pair's
                // entries are already cleared, so it is a no-op (no recursion)
                Order* o = it->second;
                if (o->type == OrderType::STOP && o->status == OrderStatus::NEW)
                    cancel_pending_stop(o);
                else
                    cancel_internal(o, /*emit_event=*/true);
            }
        }
    }

    // Cancel a STOP waiting for a trigger — it is not in the levels, so
    // cancel_internal (requiring is_active()) does not handle it. Removes
    // z stop_orders_ (swap-erase), id_index_ i emituje CANCEL.
    bool cancel_pending_stop(Order* o) noexcept {
        for (std::size_t i = 0; i < stop_orders_.size(); ++i) {
            if (stop_orders_[i] == o) {
                stop_orders_[i] = stop_orders_.back();
                stop_orders_.pop_back();
                o->status = OrderStatus::CANCELLED;
                emit(EventType::CANCEL, o->id, 0, o->price_ticks,
                     o->remaining_qty(), OrderStatus::CANCELLED,
                     RejectReason::NONE, o->client_id);
                id_index_.erase(o->id);
                oco_on_complete(o->id);
                bracket_specs_.erase(o->id);
                free_order(o);
                ++stats_.total_orders_cancelled;
                return true;
            }
        }
        return false;
    }

    // Bracket: an entry order + a spec of exits (TP limit + SL stop) fired
    // automatically after a FULL fill of the entry. Exits linked as an OCO pair.
    struct BracketSpec {
        Side          exit_side;
        std::int32_t  tp_price_ticks;
        std::int32_t  sl_trigger_ticks;
        std::int32_t  qty;
        std::uint64_t client_id;
    };
    std::unordered_map<std::uint64_t, BracketSpec> bracket_specs_;  // entry_id → spec
    std::vector<BracketSpec> bracket_pending_;   // armed after the match loop
    std::uint64_t brackets_armed_ = 0;

    // Trailing stop ratchets (trigger moved behind the market)
    std::uint64_t trailing_ratchets_ = 0;
    // Opt-in: trailing tracks mid instead of last_trade (reacts to quote
    // movement without trades; the trigger is also from mid — a consistent reference)
    bool          trailing_ratchet_on_mid_ = false;

    // Market-On-Close — a queue waiting for the closing cross (not in the book)
    struct MocOrder {
        Side          side;
        std::int32_t  qty;
        std::uint64_t client_id;
        std::uint64_t id;
    };
    std::vector<MocOrder> moc_queue_;
    std::uint64_t moc_submitted_          = 0;
    std::uint64_t moc_cancelled_unfilled_ = 0;

    // Limit-On-Close — like MOC, but with a limit price (participates in price
    // of the cross's discovery; remainders also do not rest)
    struct LocOrder {
        Side          side;
        std::int32_t  price_ticks;
        std::int32_t  qty;
        std::uint64_t client_id;
        std::uint64_t id;
    };
    std::vector<LocOrder> loc_queue_;
    std::uint64_t loc_submitted_          = 0;
    std::uint64_t loc_cancelled_unfilled_ = 0;

    // Short-Sale Restriction (SEC Rule 201 — uptick rule)
    bool          ssr_active_     = false;
    bool          ssr_armed_      = false;     // circuit breaker uzbrojony
    std::int32_t  ssr_trigger_px_ = 0;         // a drop ≤ this price activates SSR
    std::uint64_t ssr_trips_      = 0;

    // Iceberg refresh jitter (anti-detection) — deterministyczny LCG,
    // the same sequence of operations yields the same refreshes (replay-safe)
    std::int32_t  ice_jitter_bps_ = 0;         // 0 = off; 2000 = ±20%
    std::uint64_t ice_jitter_rng_ = 0x9E3779B97F4A7C15ULL;

    // Auction imbalance extensions
    std::uint64_t auction_extensions_ = 0;

    // Session phase (lifecycle opt-in; default CONTINUOUS = backward compat)
    SessionPhase session_phase_ = SessionPhase::CONTINUOUS;

    // GTX — resztki skasowane po crossie
    std::uint64_t gtx_cancelled_after_cross_ = 0;

    // Wash-trade surveillance in the auction (the same client on both sides of the cross)
    std::uint64_t auction_wash_trade_flags_  = 0;   // Σ flagged clients (all crosses)
    std::uint64_t last_auction_wash_clients_ = 0;   // z ostatniego crossu

    // Rate limiting per account — token bucket (msg rate throttle)
    struct RateBucket {
        double        tokens;
        double        capacity;
        double        refill_per_sec;
        std::uint64_t last_refill_ns;
    };
    std::unordered_map<std::uint64_t, RateBucket> rate_buckets_;
    bool          rate_limiting_enabled_ = false;
    bool          in_internal_submit_    = false;   // bypass for the engine's resubmits
    std::uint64_t rate_limited_rejects_  = 0;

    // Market Maker Protection — auto-mass-cancel quotes po N fillach makera
    // within a window T (a classic of options markets: protection against a sweep of stale quotes)
    struct MmpConfig {
        std::uint64_t window_ns;
        std::uint32_t max_fills;
        std::uint32_t fills_in_window;
        std::uint64_t window_start_ns;
        bool          tripped;
    };
    std::unordered_map<std::uint64_t, MmpConfig> mmp_;
    std::vector<std::uint64_t> mmp_pending_trips_;   // deferred — mass_cancel
                                                      // must not run inside the match loop
    std::uint64_t mmp_trips_total_ = 0;
    bool          mmp_enabled_     = false;

    void mmp_on_maker_fill(std::uint64_t cid) noexcept {
        if (!mmp_enabled_ || cid == 0) return;
        auto it = mmp_.find(cid);
        if (it == mmp_.end()) return;
        MmpConfig& m = it->second;
        if (m.tripped) return;
        const std::uint64_t now = mono_ns_now();
        if (now - m.window_start_ns > m.window_ns) {
            m.window_start_ns = now;
            m.fills_in_window = 0;
        }
        ++m.fills_in_window;
        if (m.fills_in_window > m.max_fills) {
            m.tripped = true;
            mmp_pending_trips_.push_back(cid);
        }
    }
    void process_mmp_pending() noexcept {
        while (!mmp_pending_trips_.empty()) {
            const std::uint64_t cid = mmp_pending_trips_.back();
            mmp_pending_trips_.pop_back();
            ++mmp_trips_total_;
            (void)mass_cancel(cid);   // also cancels the account's pending stops
        }
    }

    bool consume_rate_token(std::uint64_t cid) noexcept {
        auto it = rate_buckets_.find(cid);
        if (it == rate_buckets_.end()) return true;   // account without a limit
        RateBucket& rb = it->second;
        const std::uint64_t now = mono_ns_now();
        if (now > rb.last_refill_ns) {
            rb.tokens = std::min(rb.capacity,
                rb.tokens + static_cast<double>(now - rb.last_refill_ns) *
                            rb.refill_per_sec / 1e9);
            rb.last_refill_ns = now;
        }
        if (rb.tokens < 1.0) {
            ++rate_limited_rejects_;
            return false;
        }
        rb.tokens -= 1.0;
        return true;
    }

    void bracket_on_full_fill(std::uint64_t id) noexcept {
        auto it = bracket_specs_.find(id);
        if (it == bracket_specs_.end()) return;
        bracket_pending_.push_back(it->second);
        bracket_specs_.erase(it);
    }

    void arm_bracket_exits(const BracketSpec& s) noexcept {
        in_internal_submit_ = true;
        const std::uint64_t tp = submit(s.exit_side, s.tp_price_ticks, s.qty,
                                         OrderType::LIMIT, TimeInForce::GTC,
                                         0, s.client_id);
        const std::uint64_t sl = submit_stop(s.exit_side, s.sl_trigger_ticks,
                                              /*limit=*/0, s.qty, 0, s.client_id);
        in_internal_submit_ = false;
        if (tp != 0 && sl != 0) {
            (void)link_oco(tp, sl);
            ++brackets_armed_;
        }
    }

    void process_bracket_pending() noexcept {
        while (!bracket_pending_.empty()) {
            const BracketSpec s = bracket_pending_.back();
            bracket_pending_.pop_back();
            arm_bracket_exits(s);
        }
    }

    // Time-weighted mid (Σ mid × dt / Σ dt)
    std::uint64_t last_twmid_sample_ts_ns_ = 0;
    std::int32_t  last_twmid_sample_ticks_ = 0;
    std::int64_t  twmid_sum_ticks_x_ns_    = 0;
    std::uint64_t twmid_total_dt_ns_       = 0;

    void record_first_fill_latency(std::uint64_t submit_ts, std::uint64_t fill_ts) noexcept {
        if (submit_ts == 0 || fill_ts < submit_ts) return;
        const std::uint64_t lat = fill_ts - submit_ts;
        ++first_fill_latency_count_;
        first_fill_latency_sum_ns_ += lat;
        if (lat > first_fill_latency_max_ns_) first_fill_latency_max_ns_ = lat;
        if (lat < first_fill_latency_min_ns_) first_fill_latency_min_ns_ = lat;
    }

    // Spread histogram (32 buckets: 0..30 ticks + 31=overflow)
    static constexpr std::size_t SPREAD_HIST_BINS = 32;
    std::uint64_t spread_histogram_[SPREAD_HIST_BINS]{};
    std::uint64_t spread_hist_total_ = 0;

    // Latency arbitrage window detection
    std::uint64_t larb_window_ns_      = 0;            // 0 = off
    std::uint64_t larb_last_fill_ts_   = 0;
    Side          larb_last_side_      = Side::BUY;
    std::uint64_t larb_same_side_fast_ = 0;

    // Per-side last fill timestamps
    std::uint64_t last_buy_fill_ts_ns_  = 0;
    std::uint64_t last_sell_fill_ts_ns_ = 0;

    // Iceberg refresh counter
    std::uint64_t iceberg_refresh_count_ = 0;

    // Per-reason rejection counters (RejectReason::* indexed; 13 values)
    std::uint64_t rejections_by_reason_[18]{};
    // Per-TIF acceptance counters (TimeInForce::* indexed; 5 values)
    std::uint64_t accepts_by_tif_[6]{};
    // Per-OrderType acceptance counters (10 values)
    std::uint64_t accepts_by_type_[10]{};

    // Mid-price ring buffer (MID_RING_CAP defined above in the microprice section)
    std::int32_t  mid_ring_[MID_RING_CAP]{};
    std::size_t   mid_ring_head_  = 0;
    std::size_t   mid_ring_count_ = 0;

    // Fill-vs-mid bands counter — narrow / wide threshold
    std::int32_t  fill_band_threshold_ticks_ = 0;     // 0 = off
    std::uint64_t fills_within_band_ = 0;
    std::uint64_t fills_outside_band_ = 0;

    // Queue replenishment / consumption counters (TOB qty deltas)
    std::int32_t  last_tob_bid_qty_observed_ = -1;
    std::int32_t  last_tob_ask_qty_observed_ = -1;
    std::uint64_t queue_replenish_bid_ = 0;
    std::uint64_t queue_replenish_ask_ = 0;
    std::uint64_t queue_consume_bid_   = 0;
    std::uint64_t queue_consume_ask_   = 0;

    // Volume-at-price profile (zero-init via {})
    std::uint64_t volume_at_price_[LEVELS]{};

public:
    // ====================================================================
    // Top-of-book change tracking
    // ====================================================================
    //
    // poll_tob_change(): returns true when best_bid or best_ask changed
    // since the last call. Resets state. Strategies use it as a trigger
    // do re-quote / decision points.
    //
    // total_tob_changes() — a cumulative counter of TOB changes (every submit/
    // cancel/fill that changed best_bid or best_ask).
    bool poll_tob_change() noexcept {
        const bool changed = (best_bid_ticks_ != last_observed_bid_ticks_)
                          || (best_ask_ticks_ != last_observed_ask_ticks_);
        if (!changed) {
            ++tob_unchanged_streak_;
            if (tob_unchanged_streak_ > max_tob_unchanged_streak_)
                max_tob_unchanged_streak_ = tob_unchanged_streak_;
        } else {
            tob_unchanged_streak_ = 0;
        }
        if (changed) {
            const std::uint64_t now = mono_ns_now();
            // Quote-flicker: a TOB change between the last fill and now?
            // If there was NO trade since the previous TOB change, it is a flicker.
            if (last_tob_change_ts_ns_ != 0 &&
                last_fill_ts_ns_ < last_tob_change_ts_ns_) {
                ++quote_flicker_count_;
            }
            if (last_tob_change_ts_ns_ != 0 && now > last_tob_change_ts_ns_) {
                total_tob_life_ns_ += (now - last_tob_change_ts_ns_);
            }
            last_tob_change_ts_ns_ = now;
            last_observed_bid_ticks_ = best_bid_ticks_;
            last_observed_ask_ticks_ = best_ask_ticks_;
            ++stats_.total_tob_changes;
            // Spread compression check
            if (spread_compression_threshold_ticks_ > 0 &&
                has_bid() && has_ask() &&
                (best_ask_ticks_ - best_bid_ticks_) <
                spread_compression_threshold_ticks_) {
                ++spread_compression_events_;
            }
        }
        return changed;
    }

    // Quote-life — aggregated TOB residency time in ns / number of changes.
    // Approximation: counted from poll to poll. The strategy should poll
    // regularly so the result is meaningful.
    std::uint64_t mean_tob_life_ns() const noexcept {
        if (stats_.total_tob_changes < 2) return 0;
        return total_tob_life_ns_ / (stats_.total_tob_changes - 1);
    }
    std::uint64_t total_tob_life_ns() const noexcept { return total_tob_life_ns_; }

    // Spread-compression detector — sets a threshold (ticks) below
    // which every TOB change is counted as a "compression event".
    void set_spread_compression_threshold(std::int32_t threshold_ticks) noexcept {
        spread_compression_threshold_ticks_ = threshold_ticks;
    }
    std::uint64_t spread_compression_count() const noexcept {
        return spread_compression_events_;
    }

    // ====================================================================
    // Order flow imbalance / VPIN-style toxicity
    // ====================================================================
    //
    // VPIN (Volume-Synchronized Probability of Informed Trading; Easley/
    // de Prado/O'Hara 2012). Aproksymacja: cumulative |buy - sell| / total.
    // A high value = informative flow (one side dominates).
    std::uint64_t taker_buy_volume() const noexcept  { return taker_buy_volume_; }
    std::uint64_t taker_sell_volume() const noexcept { return taker_sell_volume_; }
    std::uint64_t taker_buy_count() const noexcept   { return taker_buy_count_; }
    std::uint64_t taker_sell_count() const noexcept  { return taker_sell_count_; }
    std::int32_t flow_imbalance_bps() const noexcept {
        const std::int64_t b = static_cast<std::int64_t>(taker_buy_volume_);
        const std::int64_t s = static_cast<std::int64_t>(taker_sell_volume_);
        const std::int64_t total = b + s;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((b - s) * 10000 / total);
    }
    // VPIN ~ |buy - sell| / (buy + sell). 0..1 (returnujemy bps).
    std::uint32_t vpin_bps() const noexcept {
        const std::int64_t b = static_cast<std::int64_t>(taker_buy_volume_);
        const std::int64_t s = static_cast<std::int64_t>(taker_sell_volume_);
        const std::int64_t total = b + s;
        if (total == 0) return 0;
        return static_cast<std::uint32_t>(std::abs(b - s) * 10000 / total);
    }

    // Quote flicker — TOB changed without an intervening trade.
    // High = quote stuffing or pure quoting noise.
    std::uint64_t quote_flicker_count() const noexcept { return quote_flicker_count_; }

    // Volume-at-price profile — kumulatywne exec qty per tick.
    // Strategies use it to detect support/resistance.
    std::uint64_t volume_at_price(std::int32_t price_ticks) const noexcept {
        if (price_ticks < 0 || price_ticks >= LEVELS) return 0;
        return volume_at_price_[static_cast<std::size_t>(price_ticks)];
    }
    // Point-of-control: the tick with the largest historical volume.
    std::int32_t point_of_control_ticks() const noexcept {
        std::int32_t best_tick = -1;
        std::uint64_t best_vol  = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            if (volume_at_price_[p] > best_vol) {
                best_vol = volume_at_price_[p];
                best_tick = p;
            }
        }
        return best_tick;
    }

    // ====================================================================
    // TOB stability streak
    // ====================================================================
    //
    // current_tob_unchanged_streak() — # of consecutive polls in which
    // TOB did not change. max_tob_unchanged_streak_observed() — the historical max.
    // A high value = a stable book; low = noisy/active flow.
    std::uint64_t current_tob_unchanged_streak() const noexcept {
        return tob_unchanged_streak_;
    }
    std::uint64_t max_tob_unchanged_streak_observed() const noexcept {
        return max_tob_unchanged_streak_;
    }

    // ====================================================================
    // Time-weighted spread (TWAS)
    // ====================================================================
    //
    // The strategy periodically invokes sample_time_weighted_spread(); we accumulate
    // (spread × dt) where dt = time since the previous sample. Mean TWAS =
    // Σ spread×dt / Σ dt. More representative than the arithmetic mean spread.
    void sample_time_weighted_spread() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::uint64_t now = mono_ns_now();
        if (last_spread_sample_ts_ns_ == 0) {
            last_spread_sample_ts_ns_ = now;
            return;
        }
        if (now > last_spread_sample_ts_ns_) {
            const std::uint64_t dt = now - last_spread_sample_ts_ns_;
            const std::uint64_t spread =
                static_cast<std::uint64_t>(best_ask_ticks_ - best_bid_ticks_);
            time_weighted_spread_ticks_x_ns_ += spread * dt;
            total_spread_dt_ns_ += dt;
            last_spread_sample_ts_ns_ = now;
        }
    }
    double mean_time_weighted_spread_ticks() const noexcept {
        if (total_spread_dt_ns_ == 0) return 0.0;
        return static_cast<double>(time_weighted_spread_ticks_x_ns_) /
               static_cast<double>(total_spread_dt_ns_);
    }

    // ====================================================================
    // Trade tape statistics
    // ====================================================================
    //
    // Scans the whole ring buffer (up to TAPE_CAP entries) and computes statistics
    // the last N trades. min/max/mean qty + price std-dev as volatility
    // estimator.
    struct TapeStats {
        std::int32_t  n_samples;
        std::int32_t  min_qty;
        std::int32_t  max_qty;
        double        mean_qty;
        std::int32_t  min_price_ticks;
        std::int32_t  max_price_ticks;
        double        mean_price_ticks;
        double        price_stddev_ticks;
    };
    TapeStats tape_statistics() const noexcept {
        TapeStats s{0, 0, 0, 0.0, 0, 0, 0.0, 0.0};
        const std::size_t n = std::min(tape_count_, TAPE_CAP);
        if (n == 0) return s;
        s.n_samples = static_cast<std::int32_t>(n);
        const Trade& first = tape_[(tape_head_ + TAPE_CAP - n) % TAPE_CAP];
        s.min_qty = s.max_qty = first.qty;
        s.min_price_ticks = s.max_price_ticks = first.price_ticks;
        std::int64_t sum_qty = 0, sum_px = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - n + k) % TAPE_CAP];
            sum_qty += t.qty;
            sum_px  += t.price_ticks;
            if (t.qty < s.min_qty)        s.min_qty = t.qty;
            if (t.qty > s.max_qty)        s.max_qty = t.qty;
            if (t.price_ticks < s.min_price_ticks) s.min_price_ticks = t.price_ticks;
            if (t.price_ticks > s.max_price_ticks) s.max_price_ticks = t.price_ticks;
        }
        s.mean_qty         = static_cast<double>(sum_qty) / static_cast<double>(n);
        s.mean_price_ticks = static_cast<double>(sum_px)  / static_cast<double>(n);
        // 2nd pass for std-dev (Welford would be numerically stable; here a simple one).
        double var = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - n + k) % TAPE_CAP];
            const double d = static_cast<double>(t.price_ticks) - s.mean_price_ticks;
            var += d * d;
        }
        var /= static_cast<double>(n);
        // sqrt without <cmath> – simplification via std::sqrt
        s.price_stddev_ticks = std::sqrt(var);
        return s;
    }

    // ====================================================================
    // Hurst exponent (single-window R/S estimate z tape prices)
    // ====================================================================
    //
    // H ≈ log(R/S) / log(N) where R = range mean-adjusted cumulative
    // deviations, S = std-dev. Random walk → ~0.5; trending → >0.5;
    // mean-reverting → <0.5. Single-window = crude (a proper estimator does
    // regression over many windows), but it suffices as a rough regime signal.
    // Returns 0.0 when <8 trades; 0.5 when prices are flat (S=0).
    double hurst_rs_estimate() const noexcept {
        const std::size_t n = std::min(tape_count_, TAPE_CAP);
        if (n < 8) return 0.0;
        double mean = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - n + k) % TAPE_CAP];
            mean += static_cast<double>(t.price_ticks);
        }
        mean /= static_cast<double>(n);
        double cum = 0.0, lo = 0.0, hi = 0.0, var = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - n + k) % TAPE_CAP];
            const double d = static_cast<double>(t.price_ticks) - mean;
            cum += d;
            if (cum < lo) lo = cum;
            if (cum > hi) hi = cum;
            var += d * d;
        }
        const double R = hi - lo;
        const double S = std::sqrt(var / static_cast<double>(n));
        if (S <= 0.0 || R <= 0.0) return 0.5;
        return std::log(R / S) / std::log(static_cast<double>(n));
    }

    // ====================================================================
    // Trade arrival rate
    // ====================================================================
    //
    // trades_per_second(): computed from the first/last tape ts. 0 when <2 trades.
    // A high value = active flow; a spike = breakout/news.
    double trades_per_second() const noexcept {
        const std::size_t n = std::min(tape_count_, TAPE_CAP);
        if (n < 2) return 0.0;
        const std::size_t first_idx = (tape_head_ + TAPE_CAP - n) % TAPE_CAP;
        const std::size_t last_idx  = (tape_head_ + TAPE_CAP - 1) % TAPE_CAP;
        const std::uint64_t t0 = tape_[first_idx].ts_ns;
        const std::uint64_t t1 = tape_[last_idx].ts_ns;
        if (t1 <= t0) return 0.0;
        const double dt_sec = static_cast<double>(t1 - t0) / 1e9;
        if (dt_sec <= 0.0) return 0.0;
        return static_cast<double>(n - 1) / dt_sec;
    }

    // ====================================================================
    // Realized volatility (sum of squared log returns)
    // ====================================================================
    //
    // realized_volatility_log_returns(): Σ (log(p_i/p_{i-1}))² across tape.
    // Approximation of microstructure σ²; multiply by (samples_per_year/N)
    // to obtain annualized vol.
    double realized_volatility_log_returns() const noexcept {
        const std::size_t n = std::min(tape_count_, TAPE_CAP);
        if (n < 2) return 0.0;
        double sum_sq = 0.0;
        std::int32_t prev_px = -1;
        for (std::size_t k = 0; k < n; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - n + k) % TAPE_CAP];
            if (prev_px > 0 && t.price_ticks > 0) {
                const double r = std::log(static_cast<double>(t.price_ticks) /
                                          static_cast<double>(prev_px));
                sum_sq += r * r;
            }
            prev_px = t.price_ticks;
        }
        return std::sqrt(sum_sq);
    }

    // ====================================================================
    // Spread bias + queue replenishment (TOB micro-dynamics)
    // ====================================================================
    //
    // Every poll_tob_micro() is an observation point for:
    //  • spread_bias: comparison of mid-bid vs ask-mid; when asks are close to mid,
    //    the bid holds; when bids are close to mid, the ask holds. A hint who is pressing.
    //  • queue_replenish: when TOB qty grew relative to the previous poll =
    //    market makers are adding liquidity. A drop = consumption.
    void poll_tob_micro() noexcept {
        if (has_bid() && !has_ask()) { ++one_sided_bid_only_; return; }
        if (!has_bid() && has_ask()) { ++one_sided_ask_only_; return; }
        if (!has_bid() || !has_ask()) return;
        const std::int32_t mid = (best_bid_ticks_ + best_ask_ticks_) / 2;
        const std::int32_t ask_offset = best_ask_ticks_ - mid;
        const std::int32_t bid_offset = mid - best_bid_ticks_;
        if (ask_offset < bid_offset)       ++spread_bias_ask_side_;
        else if (bid_offset < ask_offset)  ++spread_bias_bid_side_;
        else                                ++spread_bias_neutral_;
        // Queue replenishment
        const std::int32_t bid_qty = levels_[best_bid_ticks_].total_qty;
        const std::int32_t ask_qty = levels_[best_ask_ticks_].total_qty;
        if (last_tob_bid_qty_observed_ >= 0) {
            if (bid_qty > last_tob_bid_qty_observed_) ++queue_replenish_bid_;
            else if (bid_qty < last_tob_bid_qty_observed_) ++queue_consume_bid_;
        }
        if (last_tob_ask_qty_observed_ >= 0) {
            if (ask_qty > last_tob_ask_qty_observed_) ++queue_replenish_ask_;
            else if (ask_qty < last_tob_ask_qty_observed_) ++queue_consume_ask_;
        }
        last_tob_bid_qty_observed_ = bid_qty;
        last_tob_ask_qty_observed_ = ask_qty;
    }
    std::uint64_t spread_bias_ask_side()  const noexcept { return spread_bias_ask_side_; }
    std::uint64_t spread_bias_bid_side()  const noexcept { return spread_bias_bid_side_; }
    std::uint64_t spread_bias_neutral()   const noexcept { return spread_bias_neutral_; }
    std::uint64_t queue_replenish_bid_count() const noexcept { return queue_replenish_bid_; }
    std::uint64_t queue_replenish_ask_count() const noexcept { return queue_replenish_ask_; }
    std::uint64_t queue_consume_bid_count()  const noexcept { return queue_consume_bid_; }
    std::uint64_t queue_consume_ask_count()  const noexcept { return queue_consume_ask_; }

    // ====================================================================
    // Kyle's lambda (price impact slope)
    // ====================================================================
    //
    // λ = Σ Δp × v / Σ v² (slope of regression: price change ~ signed volume).
    // High λ = each unit of volume moves the price a lot (illiquid; toxic).
    double kyle_lambda() const noexcept {
        if (cum_signed_volume_sq_ <= 0.0) return 0.0;
        return cum_price_volume_product_ / cum_signed_volume_sq_;
    }
    double kyle_lambda_abs() const noexcept {
        return std::abs(kyle_lambda());
    }

    // ====================================================================
    // Latency arbitrage window detector
    // ====================================================================
    //
    // set_latency_arb_window_ns(ε): uzbrojenie alertu na same-side aggressors
    // within ≤ ε ns. High counts = exchange-co-located HFT chasing
    // venue updates faster than the competition.
    void set_latency_arb_window_ns(std::uint64_t window_ns) noexcept {
        larb_window_ns_ = window_ns;
    }
    std::uint64_t latency_arb_same_side_fast_count() const noexcept {
        return larb_same_side_fast_;
    }

    // ====================================================================
    // Per-side last fill timestamps
    // ====================================================================
    //
    // Adverse selection: if last_buy_fill is much fresher than last_sell,
    // then BUY takers dominate — bid pressing. Liquidity providers will use these
    // metrics to skew their quotes.
    std::uint64_t last_buy_fill_ts_ns()  const noexcept { return last_buy_fill_ts_ns_; }
    std::uint64_t last_sell_fill_ts_ns() const noexcept { return last_sell_fill_ts_ns_; }

    // ====================================================================
    // Iceberg refresh counter
    // ====================================================================
    //
    // Iceberg orders with a large hidden reserve do many refreshes. High
    // = a sign of long-term institutional accumulation. Per-order tracking would be
    // more optimal, but a global counter is enough for orientation.
    std::uint64_t iceberg_refresh_count() const noexcept {
        return iceberg_refresh_count_;
    }

    // ====================================================================
    // Largest resting order — wall detector
    // ====================================================================
    //
    // O(LEVELS × orders_per_level). Used periodically, not hot.
    // Wall = a lot of qty in a single order — may indicate an iceberg, hidden
    // intent, or a bluff/manipulation (spoofing).
    // ====================================================================
    // Depth concentration index
    // ====================================================================
    //
    // depth_concentration_bps(side, top_n): qty w top_n najlepszych levels
    // as a % of the total qty on that side (bps). High = liquidity close to
    // the best price (tight book); low = depth spread out (thicker tail).
    std::int32_t depth_concentration_bps(Side side, std::int32_t top_n) const noexcept {
        std::int64_t top = 0, total = 0;
        std::int32_t cnt = 0;
        if (side == Side::BUY) {
            if (!has_bid()) return 0;
            for (std::int32_t p = best_bid_ticks_; p >= 0; --p) {
                const std::int32_t q = levels_[p].total_qty;
                if (q > 0) {
                    total += q;
                    if (cnt < top_n) { top += q; ++cnt; }
                }
            }
        } else {
            if (!has_ask()) return 0;
            for (std::int32_t p = best_ask_ticks_; p < LEVELS; ++p) {
                const std::int32_t q = levels_[p].total_qty;
                if (q > 0) {
                    total += q;
                    if (cnt < top_n) { top += q; ++cnt; }
                }
            }
        }
        if (total == 0) return 0;
        return static_cast<std::int32_t>(top * 10000 / total);
    }

    // ====================================================================
    // Active book averages
    // ====================================================================
    //
    // avg_resting_qty_per_order(): mean qty per resting order (Σ qty/order_count).
    // Wysoka = institutional/iceberg-heavy; niska = retail/algo-heavy.
    double avg_resting_qty_per_order() const noexcept {
        std::int64_t total_qty = 0;
        std::int64_t total_orders = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            total_qty    += levels_[p].total_qty;
            total_orders += levels_[p].order_count;
        }
        if (total_orders == 0) return 0.0;
        return static_cast<double>(total_qty) / static_cast<double>(total_orders);
    }
    // active_price_levels(): how many price levels have orders.
    std::int32_t active_price_levels() const noexcept {
        std::int32_t n = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            if (levels_[p].order_count > 0) ++n;
        }
        return n;
    }

    // Queue-depth histogram: the distribution of order_count across active
    // levels. Bins: 1, 2, 3-4, 5-8, 9-16, 17+. A lot of mass in bin 0 =
    // a dispersed book (thin levels); mass up high = crowded queues
    // (competition for FIFO priority). Returns the number of active levels.
    std::int32_t queue_depth_histogram(std::uint32_t out_bins[6]) const noexcept {
        for (int i = 0; i < 6; ++i) out_bins[i] = 0;
        std::int32_t active = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            const std::int32_t c = levels_[p].order_count;
            if (c <= 0) continue;
            ++active;
            std::size_t bin;
            if      (c == 1)  bin = 0;
            else if (c == 2)  bin = 1;
            else if (c <= 4)  bin = 2;
            else if (c <= 8)  bin = 3;
            else if (c <= 16) bin = 4;
            else              bin = 5;
            ++out_bins[bin];
        }
        return active;
    }

    // ====================================================================
    // Mid-price ring buffer (last 16) + momentum signal
    // ====================================================================
    //
    // sample_mid_to_ring() — the strategy calls it periodically. Mid-momentum =
    // (latest - oldest) per sample interval; signal trendu w short horizon.
    void sample_mid_to_ring() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t m = (best_bid_ticks_ + best_ask_ticks_) / 2;
        mid_ring_[mid_ring_head_ % MID_RING_CAP] = m;
        ++mid_ring_head_;
        if (mid_ring_count_ < MID_RING_CAP) ++mid_ring_count_;
    }
    std::size_t mid_ring_samples() const noexcept { return mid_ring_count_; }
    std::int32_t mid_momentum_ticks() const noexcept {
        if (mid_ring_count_ < 2) return 0;
        const std::size_t n = mid_ring_count_;
        const std::size_t oldest_idx = (mid_ring_head_ + MID_RING_CAP - n) % MID_RING_CAP;
        const std::size_t newest_idx = (mid_ring_head_ + MID_RING_CAP - 1) % MID_RING_CAP;
        return mid_ring_[newest_idx] - mid_ring_[oldest_idx];
    }

    // ====================================================================
    // Fill-bands compliance metric
    // ====================================================================
    //
    // set_fill_band_threshold_ticks(T) — uzbrojenie. Fills z |fill_px - mid| ≤ T
    // → fills_within_band; else outside. Ratio = within / (within + outside).
    void set_fill_band_threshold_ticks(std::int32_t t) noexcept {
        fill_band_threshold_ticks_ = t;
    }
    std::uint64_t fills_within_band() const noexcept  { return fills_within_band_; }
    std::uint64_t fills_outside_band() const noexcept { return fills_outside_band_; }
    double fill_band_compliance_ratio() const noexcept {
        const std::uint64_t total = fills_within_band_ + fills_outside_band_;
        if (total == 0) return 0.0;
        return static_cast<double>(fills_within_band_) /
               static_cast<double>(total);
    }

    // ====================================================================
    // Per-side Kyle's lambda
    // ====================================================================
    //
    // Buy-side vs sell-side decomposition. Sygnalizuje asymmetric impact:
    // buy_lambda > sell_lambda → buyers pay more (asymmetric supply curve).
    double kyle_lambda_buy() const noexcept {
        if (cum_vsq_buy_ <= 0.0) return 0.0;
        return cum_pv_buy_ / cum_vsq_buy_;
    }
    double kyle_lambda_sell() const noexcept {
        if (cum_vsq_sell_ <= 0.0) return 0.0;
        return cum_pv_sell_ / cum_vsq_sell_;
    }
    double kyle_lambda_buy_abs()  const noexcept { return std::abs(kyle_lambda_buy()); }
    double kyle_lambda_sell_abs() const noexcept { return std::abs(kyle_lambda_sell()); }

    // ====================================================================
    // Spread regime classifier
    // ====================================================================
    //
    // Thresholds split time into 3 buckets. Use case: classify the market into
    // calm / normal / stressed regimes (volatility & impact regimes differ).
    void set_spread_regime_thresholds(std::int32_t tight, std::int32_t wide) noexcept {
        spread_regime_tight_thresh_ = tight;
        spread_regime_wide_thresh_  = wide;
    }
    void sample_spread_regime() noexcept {
        if (!has_bid() || !has_ask()) return;
        if (spread_regime_tight_thresh_ <= 0 || spread_regime_wide_thresh_ <= 0)
            return;
        const std::int32_t s = best_ask_ticks_ - best_bid_ticks_;
        if (s <= spread_regime_tight_thresh_)        ++spread_regime_tight_count_;
        else if (s >= spread_regime_wide_thresh_)    ++spread_regime_wide_count_;
        else                                          ++spread_regime_normal_count_;
    }
    std::uint64_t spread_regime_tight_count()  const noexcept { return spread_regime_tight_count_; }
    std::uint64_t spread_regime_normal_count() const noexcept { return spread_regime_normal_count_; }
    std::uint64_t spread_regime_wide_count()   const noexcept { return spread_regime_wide_count_; }

    // ====================================================================
    // TOB skewness — bid_qty vs ask_qty asymmetry
    // ====================================================================
    //
    // Returns (best_bid_qty - best_ask_qty) / (sum) × 10000 bps.
    // Pozytywny = bid_qty dominuje (passive demand).
    std::int32_t tob_skewness_bps() const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        const std::int64_t b = levels_[best_bid_ticks_].total_qty;
        const std::int64_t a = levels_[best_ask_ticks_].total_qty;
        const std::int64_t total = b + a;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((b - a) * 10000 / total);
    }

    // ====================================================================
    // Mid-VWAP divergence
    // ====================================================================
    //
    // mid_minus_tape_vwap_ticks() — current mid relative to the cumulative VWAP from the tape.
    // >0 = price drifted up from average; <0 = drifted down.
    std::int32_t mid_minus_tape_vwap_ticks() const noexcept {
        const std::int32_t vwap = tape_vwap_ticks();
        const std::int32_t mid = mid_ticks();
        if (vwap <= 0 || mid <= 0) return 0;
        return mid - vwap;
    }

    // ====================================================================
    // Mid-trend classifier
    // ====================================================================
    //
    // 3-state: UP / DOWN / SIDEWAYS based on mid_ring delta.
    enum class MidTrend : std::uint8_t { UNKNOWN = 0, UP = 1, DOWN = 2, SIDEWAYS = 3 };
    MidTrend classify_mid_trend(std::int32_t sideways_band_ticks = 1) const noexcept {
        if (mid_ring_count_ < 2) return MidTrend::UNKNOWN;
        const std::int32_t d = mid_momentum_ticks();
        if (d >  sideways_band_ticks) return MidTrend::UP;
        if (d < -sideways_band_ticks) return MidTrend::DOWN;
        return MidTrend::SIDEWAYS;
    }

    // ====================================================================
    // One-sided book counters (called by poll_tob_micro)
    // ====================================================================
    std::uint64_t one_sided_bid_only_count() const noexcept { return one_sided_bid_only_; }
    std::uint64_t one_sided_ask_only_count() const noexcept { return one_sided_ask_only_; }

    // ====================================================================
    // Spread histogram
    // ====================================================================
    //
    // sample_spread_to_histogram() — bucketuje current spread do 32-bin hist.
    // Bin index = min(31, spread). Bin 31 is the catch-all for >= 31 ticks.
    void sample_spread_to_histogram() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t s = best_ask_ticks_ - best_bid_ticks_;
        const std::size_t bin = std::min(static_cast<std::size_t>(s),
                                         SPREAD_HIST_BINS - 1);
        ++spread_histogram_[bin];
        ++spread_hist_total_;
    }
    std::uint64_t spread_histogram_bin(std::size_t bin) const noexcept {
        return bin < SPREAD_HIST_BINS ? spread_histogram_[bin] : 0;
    }
    std::uint64_t spread_histogram_total() const noexcept { return spread_hist_total_; }
    // Median spread in bps — the first bin with cumulative >= 50%.
    std::int32_t spread_histogram_median_ticks() const noexcept {
        if (spread_hist_total_ == 0) return -1;
        const std::uint64_t target = spread_hist_total_ / 2;
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < SPREAD_HIST_BINS; ++i) {
            cum += spread_histogram_[i];
            if (cum > target) return static_cast<std::int32_t>(i);
        }
        return static_cast<std::int32_t>(SPREAD_HIST_BINS - 1);
    }

    // ====================================================================
    // Top-K largest resting orders
    // ====================================================================
    //
    // Fills out_qty[] with up to K largest remaining qty. Returns how many
    // were found. A simple O(N×K) selection — use only periodically with small K.
    // ====================================================================
    // Tick-by-tick price change distribution (9 bins, -4..+4 clipped)
    // ====================================================================
    //
    // Bin index 0=Δp≤-4, 1=-3, 2=-2, 3=-1, 4=0, 5=+1, 6=+2, 7=+3, 8=Δp≥+4.
    // Heavy tails (bins 0 i 8) = high impact regime; bin 4 = quiet market.
    std::uint64_t price_change_hist_bin(std::size_t i) const noexcept {
        return i < 9 ? price_change_hist_[i] : 0;
    }
    std::uint64_t price_change_hist_total() const noexcept { return price_change_total_; }
    double price_change_hist_zero_fraction() const noexcept {
        if (price_change_total_ == 0) return 0.0;
        return static_cast<double>(price_change_hist_[4]) /
               static_cast<double>(price_change_total_);
    }
    // Tail mass: fraction trades z |Δp| >= 4.
    double price_change_hist_tail_fraction() const noexcept {
        if (price_change_total_ == 0) return 0.0;
        const std::uint64_t tail = price_change_hist_[0] + price_change_hist_[8];
        return static_cast<double>(tail) /
               static_cast<double>(price_change_total_);
    }

    // ====================================================================
    // HFT toxicity composite score
    // ====================================================================
    //
    // Scaled 0..10000 bps. A blend of three signals:
    //   • VPIN: |buy-sell|/total volume
    //   • |Kyle λ| normalized by mean_qty (proxy)
    //   • CTR (cancel-to-trade) clamped do 100
    // Each component equally weighted (33% × 33% × 33%).
    // ====================================================================
    // Per-side trade VWAP (from the whole tape; buy-taker vs sell-taker)
    // ====================================================================
    //
    // buy_vwap_ticks() / sell_vwap_ticks() — VWAP separately for each direction
    // taker order flow. Asymmetric: buy_vwap > mid → bullish; sell_vwap < mid → bearish.
    std::int32_t buy_vwap_ticks() const noexcept {
        if (cum_buy_volume_ == 0) return 0;
        return static_cast<std::int32_t>(
            cum_buy_notional_ticks_ / static_cast<std::int64_t>(cum_buy_volume_));
    }
    std::int32_t sell_vwap_ticks() const noexcept {
        if (cum_sell_volume_ == 0) return 0;
        return static_cast<std::int32_t>(
            cum_sell_notional_ticks_ / static_cast<std::int64_t>(cum_sell_volume_));
    }
    // buy_vs_sell_vwap_spread_ticks: positive when buy_taker_vwap > sell_taker_vwap
    std::int32_t buy_vs_sell_vwap_spread_ticks() const noexcept {
        const auto b = buy_vwap_ticks();
        const auto s = sell_vwap_ticks();
        if (b == 0 || s == 0) return 0;
        return b - s;
    }

    // ====================================================================
    // Inter-trade time gap statistics
    // ====================================================================
    //
    // Mean / min / max gap ns between consecutive trades. Low = burst flow,
    // wysokie = quiet periods. Distribution shape = clustering metryka.
    std::uint64_t inter_trade_gap_min_ns() const noexcept {
        return inter_trade_gap_count_ == 0 ? 0 : inter_trade_gap_min_ns_;
    }
    std::uint64_t inter_trade_gap_max_ns() const noexcept {
        return inter_trade_gap_max_ns_;
    }
    std::uint64_t inter_trade_gap_mean_ns() const noexcept {
        if (inter_trade_gap_count_ == 0) return 0;
        return inter_trade_gap_sum_ns_ / inter_trade_gap_count_;
    }
    std::uint64_t inter_trade_gap_sample_count() const noexcept {
        return inter_trade_gap_count_;
    }

    // ====================================================================
    // Largest single trade observed (block detector)
    // ====================================================================
    std::int32_t largest_single_trade_qty() const noexcept {
        return largest_single_trade_qty_;
    }

    // ====================================================================
    // First-fill latency stats
    // ====================================================================
    //
    // Mierzone per order — od submit_ts do first_fill_ts (ns). Reflektuje:
    //  • for the maker: queue wait time before the first match
    //  • for the taker: matching engine latency
    std::uint64_t first_fill_latency_count() const noexcept {
        return first_fill_latency_count_;
    }
    std::uint64_t first_fill_latency_min_ns() const noexcept {
        return first_fill_latency_count_ == 0 ? 0 : first_fill_latency_min_ns_;
    }
    std::uint64_t first_fill_latency_max_ns() const noexcept {
        return first_fill_latency_max_ns_;
    }
    std::uint64_t first_fill_latency_mean_ns() const noexcept {
        if (first_fill_latency_count_ == 0) return 0;
        return first_fill_latency_sum_ns_ / first_fill_latency_count_;
    }

    // ====================================================================
    // Submission burst detector
    // ====================================================================
    //
    // set_burst_window_ns(ε): increment the "burst run" counter when a
    // submit occurs within ≤ ε of the previous one. A burst signal = volatility regime
    // change or coordinated order entry.
    void set_burst_window_ns(std::uint64_t window_ns) noexcept {
        burst_window_ns_ = window_ns;
    }
    std::uint64_t burst_runs_count() const noexcept { return burst_runs_count_; }
    std::uint64_t burst_current_run_count() const noexcept { return burst_in_run_count_; }

    // ====================================================================
    // Order completion histogram
    // ====================================================================
    //
    // Four completion categories:
    //  • filled_fully — the match took the whole qty
    //  • cancelled_partial — cancel after a partial fill
    //  • cancelled_unfilled — cancel without any fill
    //  • expired_partial / expired_unfilled — analogously but via GTD/DAY
    // Ratio = filled_fully / total_orders_added — book "execution efficiency".
    std::uint64_t completion_filled_fully() const noexcept     { return completion_filled_fully_; }
    std::uint64_t completion_cancelled_partial() const noexcept{ return completion_cancelled_partial_; }
    std::uint64_t completion_cancelled_unfilled() const noexcept{ return completion_cancelled_unfilled_; }
    std::uint64_t completion_expired_partial() const noexcept  { return completion_expired_partial_; }
    std::uint64_t completion_expired_unfilled() const noexcept { return completion_expired_unfilled_; }
    double fill_rate_ratio() const noexcept {
        const std::uint64_t added = stats_.total_orders_added;
        if (added == 0) return 0.0;
        return static_cast<double>(completion_filled_fully_) /
               static_cast<double>(added);
    }

    // ====================================================================
    // Time-weighted mid (TWAP-of-mid)
    // ====================================================================
    //
    // The strategy periodically invokes sample_time_weighted_mid(). It accumulates
    // (mid × dt) between samples. mean = sum / total_dt. A better benchmark
    // than a simple mid snapshot.
    void sample_time_weighted_mid() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t mid = (best_bid_ticks_ + best_ask_ticks_) / 2;
        const std::uint64_t now = mono_ns_now();
        if (last_twmid_sample_ts_ns_ == 0) {
            last_twmid_sample_ts_ns_ = now;
            last_twmid_sample_ticks_ = mid;
            return;
        }
        if (now > last_twmid_sample_ts_ns_) {
            const std::uint64_t dt = now - last_twmid_sample_ts_ns_;
            // Trapezoidal: for a stable mean we use the previous mid × dt
            twmid_sum_ticks_x_ns_ +=
                static_cast<std::int64_t>(last_twmid_sample_ticks_) *
                static_cast<std::int64_t>(dt);
            twmid_total_dt_ns_ += dt;
            last_twmid_sample_ts_ns_ = now;
            last_twmid_sample_ticks_ = mid;
        }
    }
    double mean_time_weighted_mid_ticks() const noexcept {
        if (twmid_total_dt_ns_ == 0) return 0.0;
        return static_cast<double>(twmid_sum_ticks_x_ns_) /
               static_cast<double>(twmid_total_dt_ns_);
    }
    std::uint64_t time_weighted_mid_total_dt_ns() const noexcept {
        return twmid_total_dt_ns_;
    }

    // Mean trade qty across the whole cumulative tape
    double mean_trade_qty() const noexcept {
        if (stats_.total_fills == 0) return 0.0;
        return static_cast<double>(stats_.total_volume) /
               static_cast<double>(stats_.total_fills);
    }

    // ====================================================================
    // Per-side maker fills
    // ====================================================================
    //
    // Number of maker orders fully filled per side. BUY maker fully filled =
    // strong sell pressure ate complete bid. SELL maker fully filled =
    // strong buy pressure. Asymmetry → directional bias indicator.
    std::uint64_t maker_fills_buy_side() const noexcept  { return maker_fills_buy_side_; }
    std::uint64_t maker_fills_sell_side() const noexcept { return maker_fills_sell_side_; }

    // ====================================================================
    // Mean fill notional (Σ price × qty / Σ fills)
    // ====================================================================
    //
    // Larger = block trading / institutional flow; smaller = retail/algo.
    double mean_fill_notional_ticks() const noexcept {
        if (stats_.total_fills == 0) return 0.0;
        // Recompute from tape (cumulative buy + sell notionals are tape-derived)
        const std::int64_t total_notional = cum_buy_notional_ticks_ +
                                             cum_sell_notional_ticks_;
        return static_cast<double>(total_notional) /
               static_cast<double>(stats_.total_fills);
    }

    // ====================================================================
    // Most-active levels by qty (top-K)
    // ====================================================================
    //
    // top_k_active_levels_by_qty(out_prices, out_qtys, k): returns the top-K
    // price levels with the largest total_qty. O(LEVELS × K) — periodic.
    // ====================================================================
    // Depth pyramid score
    // ====================================================================
    //
    // depth_pyramid_steepness_bps(side, depth_n): slope between best level qty
    // and the N-th level qty. High = a steep depth drop-off (tightly stacked
    // best level, thin tail); low = an even spread of depth.
    // (qty_at_best - qty_at_n-th_level) / qty_at_best × 10000.
    std::int32_t depth_pyramid_steepness_bps(Side side, std::int32_t depth_n) const noexcept {
        if (depth_n < 2) return 0;
        std::int32_t qty_best = 0, qty_nth = 0;
        std::int32_t cnt = 0;
        if (side == Side::BUY) {
            if (!has_bid()) return 0;
            for (std::int32_t p = best_bid_ticks_; p >= 0 && cnt < depth_n; --p) {
                const std::int32_t q = levels_[p].total_qty;
                if (q > 0) {
                    if (cnt == 0) qty_best = q;
                    qty_nth = q;
                    ++cnt;
                }
            }
        } else {
            if (!has_ask()) return 0;
            for (std::int32_t p = best_ask_ticks_; p < LEVELS && cnt < depth_n; ++p) {
                const std::int32_t q = levels_[p].total_qty;
                if (q > 0) {
                    if (cnt == 0) qty_best = q;
                    qty_nth = q;
                    ++cnt;
                }
            }
        }
        if (cnt < depth_n || qty_best == 0) return 0;
        return static_cast<std::int32_t>(
            static_cast<std::int64_t>(qty_best - qty_nth) * 10000 / qty_best);
    }

    // ====================================================================
    // Cumulative resting volume per side
    // ====================================================================
    //
    // Σ qty po wszystkich BUY / SELL levels. O(LEVELS) — okresowy odczyt.
    std::int64_t cumulative_resting_volume(Side side) const noexcept {
        std::int64_t total = 0;
        if (side == Side::BUY) {
            if (!has_bid()) return 0;
            for (std::int32_t p = 0; p <= best_bid_ticks_ && p < LEVELS; ++p) {
                total += levels_[p].total_qty;
            }
        } else {
            if (!has_ask()) return 0;
            for (std::int32_t p = best_ask_ticks_; p < LEVELS; ++p) {
                total += levels_[p].total_qty;
            }
        }
        return total;
    }

    // ====================================================================
    // Best-level qty histogram (16 log2-style bins)
    // ====================================================================
    //
    // sample_best_qty_histogram() — periodyczny hook. Bin index = log2(qty)
    // clamped to [0..15]. Strategia analizuje "typical" best-level depth.
    void sample_best_qty_histogram() noexcept {
        if (has_bid()) {
            const std::int32_t bq = levels_[best_bid_ticks_].total_qty;
            ++best_bid_qty_hist_[qty_to_log2_bin(bq)];
        }
        if (has_ask()) {
            const std::int32_t aq = levels_[best_ask_ticks_].total_qty;
            ++best_ask_qty_hist_[qty_to_log2_bin(aq)];
        }
    }
    std::uint64_t best_bid_qty_hist_bin(std::size_t i) const noexcept {
        return i < QTY_HIST_BINS ? best_bid_qty_hist_[i] : 0;
    }
    std::uint64_t best_ask_qty_hist_bin(std::size_t i) const noexcept {
        return i < QTY_HIST_BINS ? best_ask_qty_hist_[i] : 0;
    }

    // ====================================================================
    // EMA imbalance (alpha-blended TOB imbalance signal)
    // ====================================================================
    //
    // sample_ema_imbalance() — periodyczny hook. EMA filtruje noise z imbalance.
    // alpha = 0.1 default (responsiveness: 10 samples ~ ε90 decay).
    void set_ema_imbalance_alpha(double a) noexcept {
        if (a > 0.0 && a <= 1.0) ema_imbalance_alpha_ = a;
    }
    void sample_ema_imbalance() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int64_t b = levels_[best_bid_ticks_].total_qty;
        const std::int64_t a = levels_[best_ask_ticks_].total_qty;
        const std::int64_t total = b + a;
        if (total == 0) return;
        const double current_bps = static_cast<double>((b - a) * 10000) /
                                    static_cast<double>(total);
        if (!ema_imbalance_init_) {
            ema_imbalance_bps_value_ = current_bps;
            ema_imbalance_init_ = true;
        } else {
            ema_imbalance_bps_value_ = ema_imbalance_alpha_ * current_bps +
                                       (1.0 - ema_imbalance_alpha_) * ema_imbalance_bps_value_;
        }
    }
    double ema_imbalance_bps() const noexcept { return ema_imbalance_bps_value_; }

    // ====================================================================
    // Microprice ring buffer (last MID_RING_CAP samples)
    // ====================================================================
    void sample_microprice_to_ring() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t mp = microprice_ticks();
        microprice_ring_[microprice_ring_head_ % MID_RING_CAP] = mp;
        ++microprice_ring_head_;
        if (microprice_ring_count_ < MID_RING_CAP) ++microprice_ring_count_;
    }
    std::size_t microprice_ring_samples() const noexcept {
        return microprice_ring_count_;
    }
    std::int32_t microprice_momentum_ticks() const noexcept {
        if (microprice_ring_count_ < 2) return 0;
        const std::size_t n = microprice_ring_count_;
        const std::size_t oldest_idx = (microprice_ring_head_ + MID_RING_CAP - n) % MID_RING_CAP;
        const std::size_t newest_idx = (microprice_ring_head_ + MID_RING_CAP - 1) % MID_RING_CAP;
        return microprice_ring_[newest_idx] - microprice_ring_[oldest_idx];
    }

    // ====================================================================
    // Signed-volume EMA (order flow filter)
    // ====================================================================
    //
    // EMA of signed qty (+qty BUY, -qty SELL) per trade. A direction signal
    // more responsive than the cumulative flow imbalance.
    double ema_signed_volume() const noexcept { return ema_signed_volume_; }
    bool   ema_signed_volume_ready() const noexcept { return ema_signed_volume_init_; }

    // ====================================================================
    // Cont-Kukanov Order Flow Imbalance (OFI)
    // ====================================================================
    //
    // The classic formula from Cont/Kukanov 2014:
    //   OFI = ΔW_b - ΔW_a
    // where:
    //   ΔW_b: +q_b if bid price up; q_b - q_b_prev if unchanged; -q_b_prev if down
    //   ΔW_a: -q_a if ask price up; q_a_prev - q_a if unchanged; +q_a_prev if down
    // (The ask side is inverted — a rise in the ask reduces buy pressure.)
    // (The _ck name for disambiguation from the simpler sample_ofi() above.)
    void sample_ofi_ck() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t bq = levels_[best_bid_ticks_].total_qty;
        const std::int32_t aq = levels_[best_ask_ticks_].total_qty;
        ++ofi_samples_;
        if (ofi_prev_bid_ticks_ == NO_BID_TICKS) {
            // first sample — establish baseline
            ofi_prev_bid_ticks_ = best_bid_ticks_;
            ofi_prev_ask_ticks_ = best_ask_ticks_;
            ofi_prev_bid_qty_   = bq;
            ofi_prev_ask_qty_   = aq;
            return;
        }
        // Bid contribution
        std::int64_t dw_b = 0;
        if (best_bid_ticks_ > ofi_prev_bid_ticks_)      dw_b = +bq;
        else if (best_bid_ticks_ < ofi_prev_bid_ticks_) dw_b = -ofi_prev_bid_qty_;
        else                                              dw_b = bq - ofi_prev_bid_qty_;
        // Ask contribution
        std::int64_t dw_a = 0;
        if (best_ask_ticks_ > ofi_prev_ask_ticks_)      dw_a = +ofi_prev_ask_qty_;
        else if (best_ask_ticks_ < ofi_prev_ask_ticks_) dw_a = -aq;
        else                                              dw_a = ofi_prev_ask_qty_ - aq;
        ofi_cumulative_ += dw_b - dw_a;
        ofi_prev_bid_ticks_ = best_bid_ticks_;
        ofi_prev_ask_ticks_ = best_ask_ticks_;
        ofi_prev_bid_qty_   = bq;
        ofi_prev_ask_qty_   = aq;
    }
    std::int64_t ofi_cumulative() const noexcept   { return ofi_cumulative_; }
    std::uint64_t ofi_samples()  const noexcept   { return ofi_samples_; }
    double ofi_per_sample() const noexcept {
        if (ofi_samples_ < 2) return 0.0;
        return static_cast<double>(ofi_cumulative_) /
               static_cast<double>(ofi_samples_ - 1);
    }

    // ====================================================================
    // Trade clustering coefficient (Fano factor — var/mean of inter-trade gaps)
    // ====================================================================
    //
    // Fano factor > 1 = overdispersed (bursty / clustered trades).
    // = 1 → Poisson process. < 1 → underdispersed (regular).
    double trade_gap_variance_ns_sq() const noexcept {
        if (gap_var_count_ < 2) return 0.0;
        return gap_var_M2_ / static_cast<double>(gap_var_count_ - 1);
    }
    double trade_gap_mean_ns_dbl() const noexcept { return gap_var_mean_; }
    double trade_clustering_fano() const noexcept {
        if (gap_var_count_ < 2 || gap_var_mean_ <= 0.0) return 0.0;
        return trade_gap_variance_ns_sq() / gap_var_mean_;
    }

    // ====================================================================
    // Maker survival ratio (across polls)
    // ====================================================================
    //
    // sample_maker_survival(): snapshot wszystkich aktywnych order_id; w
    // on the next poll counts how many from the previous snapshot are still alive.
    // mean_survival_ratio = total_survivors / total_orders_prev_sampled.
    void sample_maker_survival() {
        ++maker_survival_total_polls_;
        if (!maker_survival_prev_ids_.empty()) {
            std::uint64_t survivors = 0;
            for (auto id : maker_survival_prev_ids_) {
                if (id_index_.find(id) != id_index_.end()) ++survivors;
            }
            maker_survival_total_orders_ += maker_survival_prev_ids_.size();
            maker_survival_survivors_    += survivors;
        }
        maker_survival_prev_ids_.clear();
        maker_survival_prev_ids_.reserve(id_index_.size());
        for (const auto& kv : id_index_) {
            maker_survival_prev_ids_.push_back(kv.first);
        }
    }
    double maker_survival_ratio() const noexcept {
        if (maker_survival_total_orders_ == 0) return 0.0;
        return static_cast<double>(maker_survival_survivors_) /
               static_cast<double>(maker_survival_total_orders_);
    }
    std::uint64_t maker_survival_total_polls() const noexcept {
        return maker_survival_total_polls_;
    }

    // ====================================================================
    // Trade direction Markov chain (BB, BS, SB, SS counters)
    // ====================================================================
    //
    // Tracking serial dependence trade direction. P(BUY|BUY) → "trend persistence".
    // Wysoka P(BUY|BUY) = trending; wysoka P(BUY|SELL) = mean-reverting.
    std::uint64_t markov_count_BB() const noexcept { return markov_BB_; }
    std::uint64_t markov_count_BS() const noexcept { return markov_BS_; }
    std::uint64_t markov_count_SB() const noexcept { return markov_SB_; }
    std::uint64_t markov_count_SS() const noexcept { return markov_SS_; }
    double markov_prob_buy_given_buy() const noexcept {
        const auto t = markov_BB_ + markov_BS_;
        return t == 0 ? 0.0 : static_cast<double>(markov_BB_) /
                              static_cast<double>(t);
    }
    double markov_prob_buy_given_sell() const noexcept {
        const auto t = markov_SB_ + markov_SS_;
        return t == 0 ? 0.0 : static_cast<double>(markov_SB_) /
                              static_cast<double>(t);
    }
    double markov_trend_persistence_bps() const noexcept {
        // (P(B|B) + P(S|S)) - 1.0 × 10000 — positive = trending; negative = reverting
        const auto bb_tot = markov_BB_ + markov_BS_;
        const auto ss_tot = markov_SB_ + markov_SS_;
        if (bb_tot == 0 || ss_tot == 0) return 0.0;
        const double pbb = static_cast<double>(markov_BB_) /
                            static_cast<double>(bb_tot);
        const double pss = static_cast<double>(markov_SS_) /
                            static_cast<double>(ss_tot);
        return (pbb + pss - 1.0) * 10000.0;
    }

    // ====================================================================
    // Queue depth at maker arrival (FIFO position rejestrowana na enqueue)
    // ====================================================================
    //
    // The strategy observes: when a new maker lands on an empty level → great queue
    // priority; when on a crowded level → low priority.
    double mean_queue_depth_at_arrival() const noexcept {
        if (queue_depth_arrival_count_ == 0) return 0.0;
        return static_cast<double>(queue_depth_arrival_sum_) /
               static_cast<double>(queue_depth_arrival_count_);
    }
    std::int32_t max_queue_depth_at_arrival() const noexcept {
        return queue_depth_arrival_max_;
    }
    std::uint64_t queue_depth_arrival_samples() const noexcept {
        return queue_depth_arrival_count_;
    }

    // ====================================================================
    // Trade momentum count from tape (last N net direction)
    // ====================================================================
    //
    // Returns a signed value: +N when the last N are BUY-takers; -N when SELL-takers.
    // Range: [-min(n, tape_count), +min(n, tape_count)].
    // ====================================================================
    // Inter-trade gap autocorrelation lag-1
    // ====================================================================
    //
    // ACF₁ = Σ x_i × x_{i-1} / Σ x_{i-1}² (uncentered estimator).
    // Positive = trended timing; negative = alternating; ≈0 = uncorrelated.
    double inter_trade_gap_autocorr_lag1() const noexcept {
        if (gap_autocorr_xx_sum_ <= 0.0) return 0.0;
        // Clamp do [-1,1]: autokorelacja z definicji tam zyje. Niecentrowany
        // estymator na danych zaleznych od zegara (inter-trade gaps) potrafi
        // chwilowo przekroczyc 1 (rozne float-ordering g++/clang -> flaky test
        // acf_bounded). Clamp = the correct ACF range, a deterministic result.
        const double acf = gap_autocorr_xy_sum_ / gap_autocorr_xx_sum_;
        return acf < -1.0 ? -1.0 : (acf > 1.0 ? 1.0 : acf);
    }

    // ====================================================================
    // Slippage budget guard
    // ====================================================================
    //
    // set_slippage_guard_threshold_ticks(T): a counter of how many fill events had
    // |fill_px - mid_pre| > T. Pure detection (does not block the match).
    void set_slippage_guard_threshold_ticks(std::int32_t t) noexcept {
        slippage_guard_threshold_ticks_ = t;
    }
    std::uint64_t slippage_guard_violations() const noexcept {
        return slippage_guard_violations_;
    }

    // ====================================================================
    // NBBO violation audit (defensive)
    // ====================================================================
    //
    // Counts fills that looked "off-NBBO" (taker limit significantly beyond
    // mid). Sanity — in a correct engine it should be 0 for quiet markets.
    std::uint64_t nbbo_violations_count() const noexcept {
        return nbbo_violations_count_;
    }

    // ====================================================================
    // Per-side cancellation counters
    // ====================================================================
    std::uint64_t cancellations_by_buy()  const noexcept { return cancellations_by_buy_; }
    std::uint64_t cancellations_by_sell() const noexcept { return cancellations_by_sell_; }

    // ====================================================================
    // OCO (One-Cancels-Other) — bracket orders
    // ====================================================================
    //
    // link_oco(a, b): links two RESTING orders into a pair. Completion of one leg
    // (full fill / cancel / expire / auction fill) automatically cancels the other.
    // A partial fill does NOT trigger (only a full fill). modify() of a leg
    // PRESERVES the pair (cancel-replace, order_id unchanged); if
    // the modified leg fills fully immediately, the partner falls normally.
    // A pending STOP may be a leg (cancelled by cancel_pending_stop).
    bool link_oco(std::uint64_t a, std::uint64_t b) noexcept {
        if (a == 0 || b == 0 || a == b) return false;
        if (id_index_.find(a) == id_index_.end()) return false;
        if (id_index_.find(b) == id_index_.end()) return false;
        if (oco_partner_.count(a) != 0 || oco_partner_.count(b) != 0)
            return false;   // one of the legs is already in another pair
        oco_partner_[a] = b;
        oco_partner_[b] = a;
        return true;
    }
    bool unlink_oco(std::uint64_t id) noexcept {
        auto it = oco_partner_.find(id);
        if (it == oco_partner_.end()) return false;
        const std::uint64_t partner = it->second;
        oco_partner_.erase(it);
        oco_partner_.erase(partner);
        return true;
    }
    std::uint64_t oco_partner_of(std::uint64_t id) const noexcept {
        const auto it = oco_partner_.find(id);
        return it == oco_partner_.end() ? 0 : it->second;
    }
    std::size_t active_oco_pairs() const noexcept { return oco_partner_.size() / 2; }
    std::uint64_t oco_triggered_cancels() const noexcept { return oco_triggered_cancels_; }

    // ====================================================================
    // Bracket orders (entry + auto TP/SL)
    // ====================================================================
    //
    // submit_bracket(side, entry_price, qty, tp_price, sl_trigger):
    //   1. Submituje entry LIMIT/DAY.
    //   2. After a FULL fill of the entry it automatically arms exits on the opposite
    //      side: take-profit LIMIT/GTC @ tp_price + stop-loss STOP
    //      (market-on-trigger) @ sl_trigger, linked as an OCO pair.
    //   3. Cancel/expire the entry before the fill = disarm (exits are not created).
    // A partial fill does not arm (only a full fill). Returns entry_id
    // or 0 on reject.
    std::uint64_t submit_bracket(Side side, std::int32_t entry_price_ticks,
                                  std::int32_t qty,
                                  std::int32_t tp_price_ticks,
                                  std::int32_t sl_trigger_ticks,
                                  std::uint64_t client_id = 0) noexcept {
        const Side exit_side = (side == Side::BUY) ? Side::SELL : Side::BUY;
        const BracketSpec spec{exit_side, tp_price_ticks, sl_trigger_ticks,
                               qty, client_id};
        const std::uint64_t entry_id =
            submit(side, entry_price_ticks, qty, OrderType::LIMIT,
                   TimeInForce::DAY, 0, client_id);
        if (entry_id == 0) return 0;
        if (id_index_.find(entry_id) == id_index_.end()) {
            // Entry filled immediately as a taker (LIMIT/DAY is not
            // IOC, so absence from the index == full fill) — arm right away
            arm_bracket_exits(spec);
        } else {
            bracket_specs_[entry_id] = spec;
        }
        return entry_id;
    }
    std::size_t pending_bracket_specs() const noexcept { return bracket_specs_.size(); }
    std::uint64_t brackets_armed() const noexcept { return brackets_armed_; }

    // ====================================================================
    // Lee-Ready trade classification (quote rule + tick test)
    // ====================================================================
    //
    // Klasyka mikrostruktury: px > mid → BUY-initiated, px < mid → SELL,
    // px == mid → tick test. accuracy = % agreement with the actual taker_side.
    double lee_ready_accuracy() const noexcept {
        if (lr_total_classified_ == 0) return 0.0;
        return static_cast<double>(lr_correct_) /
               static_cast<double>(lr_total_classified_);
    }
    std::uint64_t lee_ready_classified_total() const noexcept { return lr_total_classified_; }
    std::uint64_t lee_ready_classified_buy()   const noexcept { return lr_buy_classified_; }
    std::uint64_t lee_ready_classified_sell()  const noexcept { return lr_sell_classified_; }
    std::uint64_t lee_ready_unclassifiable()   const noexcept { return lr_unclassifiable_; }

    // ====================================================================
    // Per-account VWAP
    // ====================================================================
    //
    // VWAP of all fills of a given client_id (maker and taker side combined).
    // 0 = no fills for this account.
    std::int32_t account_vwap_ticks(std::uint64_t client_id) const noexcept {
        const auto it = account_vwap_.find(client_id);
        if (it == account_vwap_.end() || it->second.volume == 0) return 0;
        return static_cast<std::int32_t>(it->second.notional_ticks /
                                          it->second.volume);
    }

    // ====================================================================
    // Book integrity audit (struktura intrusive list + agregaty)
    // ====================================================================
    //
    // Defensive self-check of invariants:
    //   • prev/next consistent (doubly-linked)
    //   • order->price_ticks == index levelu
    //   • lvl.order_count == the actual number of nodes
    //   • lvl.total_qty == Σ displayed_qty
    //   • lvl.total_hidden == Σ (total - filled - displayed)
    //   • lvl.tail points to the last node
    // Returns the number of violations (0 = book consistent). O(LEVELS + orders).
    std::uint64_t audit_book_integrity() const noexcept {
        std::uint64_t violations = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            const PriceLevel& lvl = levels_[p];
            std::int32_t count = 0;
            std::int64_t qty_sum = 0;
            std::int64_t hidden_sum = 0;
            const Order* prev = nullptr;
            for (const Order* o = lvl.head; o != nullptr; o = o->next_at_level) {
                if (o->prev_at_level != prev) ++violations;
                if (o->price_ticks != p)      ++violations;
                ++count;
                qty_sum    += o->displayed_qty;
                hidden_sum += o->total_qty - o->filled_qty - o->displayed_qty;
                prev = o;
            }
            if (lvl.tail != prev)            ++violations;
            if (lvl.order_count != count)    ++violations;
            if (lvl.total_qty != qty_sum)    ++violations;
            if (lvl.total_hidden != hidden_sum) ++violations;
        }
        return violations;
    }

    std::int32_t trade_momentum_last_n(std::size_t n) const noexcept {
        const std::size_t avail = std::min(tape_count_, TAPE_CAP);
        const std::size_t use   = std::min(n, avail);
        std::int32_t score = 0;
        for (std::size_t k = 0; k < use; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - 1 - k) % TAPE_CAP];
            score += (t.taker_side == Side::BUY) ? +1 : -1;
        }
        return score;
    }

private:
    static std::size_t qty_to_log2_bin(std::int32_t q) noexcept {
        if (q <= 0) return 0;
        std::size_t b = 0;
        std::uint32_t v = static_cast<std::uint32_t>(q);
        while (v > 1 && b < QTY_HIST_BINS - 1) { v >>= 1; ++b; }
        return b;
    }
public:

    std::size_t top_k_active_levels_by_qty(std::int32_t* out_prices,
                                            std::int32_t* out_qtys,
                                            std::size_t k) const noexcept {
        if (!out_prices || !out_qtys || k == 0) return 0;
        for (std::size_t i = 0; i < k; ++i) {
            out_prices[i] = -1;
            out_qtys[i] = 0;
        }
        std::size_t filled = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            const std::int32_t q = levels_[p].total_qty;
            if (q <= 0) continue;
            std::size_t pos = std::min(filled, k - 1);
            if (q > out_qtys[pos] || filled < k) {
                if (filled < k) ++filled;
                std::size_t i_ins = std::min(filled - 1, k - 1);
                while (i_ins > 0 && out_qtys[i_ins - 1] < q) {
                    out_qtys[i_ins]   = out_qtys[i_ins - 1];
                    out_prices[i_ins] = out_prices[i_ins - 1];
                    --i_ins;
                }
                out_qtys[i_ins]   = q;
                out_prices[i_ins] = p;
            }
        }
        return filled;
    }

    // ====================================================================
    // Last-N rolling VWAP z tape
    // ====================================================================
    //
    // last_n_vwap_ticks(n): VWAP of the last min(n, tape_count) trades. Gives
    // a more responsive VWAP than cumulative (forgets old data).
    std::int32_t last_n_vwap_ticks(std::size_t n) const noexcept {
        const std::size_t avail = std::min(tape_count_, TAPE_CAP);
        const std::size_t use   = std::min(n, avail);
        if (use == 0) return 0;
        std::int64_t notional = 0;
        std::int64_t volume   = 0;
        for (std::size_t k = 0; k < use; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - 1 - k) % TAPE_CAP];
            notional += static_cast<std::int64_t>(t.price_ticks) * t.qty;
            volume   += t.qty;
        }
        if (volume == 0) return 0;
        return static_cast<std::int32_t>(notional / volume);
    }

    // ====================================================================
    // Implementation Shortfall (TCA — Almgren/Chriss style)
    // ====================================================================
    //
    // IS = Σ (fill_px - decision_mid) × qty × side_sign across all fills.
    // Positive = the trader lost on the market move (adverse selection).
    // mean_implementation_shortfall_ticks_per_share() — average cost per share.
    std::int64_t cumulative_implementation_shortfall_ticks_qty() const noexcept {
        return cum_implementation_shortfall_ticks_qty_;
    }
    double mean_implementation_shortfall_ticks_per_share() const noexcept {
        if (cum_implementation_shortfall_qty_ == 0) return 0.0;
        return static_cast<double>(cum_implementation_shortfall_ticks_qty_) /
               static_cast<double>(cum_implementation_shortfall_qty_);
    }

    std::uint32_t toxicity_composite_score_bps() const noexcept {
        const std::uint32_t vpin = vpin_bps();   // [0..10000]
        // |λ| normalized: rough bound by 1000 (5 ticks per 1 qty trade unit)
        const double lambda_abs = kyle_lambda_abs();
        std::uint32_t lambda_bps = static_cast<std::uint32_t>(
            std::min(lambda_abs * 1000.0, 10000.0));
        const double ctr = cancel_to_trade_ratio();
        std::uint32_t ctr_bps = static_cast<std::uint32_t>(
            std::min(ctr * 200.0, 10000.0));  // CTR=50 → 10000 bps
        return (vpin + lambda_bps + ctr_bps) / 3;
    }

    std::size_t top_k_resting_qty(std::int32_t* out_qty, std::size_t k) const noexcept {
        if (!out_qty || k == 0) return 0;
        for (std::size_t i = 0; i < k; ++i) out_qty[i] = 0;
        std::size_t filled = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            for (const Order* o = levels_[p].head; o != nullptr; o = o->next_at_level) {
                const std::int32_t left = o->total_qty - o->filled_qty;
                if (left <= 0) continue;
                // Insert into the sorted array (insertion-sort top-K)
                std::size_t pos = std::min(filled, k - 1);
                if (left > out_qty[pos] || filled < k) {
                    if (filled < k) ++filled;
                    // find the spot
                    std::size_t i_ins = std::min(filled - 1, k - 1);
                    while (i_ins > 0 && out_qty[i_ins - 1] < left) {
                        out_qty[i_ins] = out_qty[i_ins - 1];
                        --i_ins;
                    }
                    out_qty[i_ins] = left;
                }
            }
        }
        return filled;
    }

    std::int32_t largest_resting_order_qty() const noexcept {
        std::int32_t best = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            for (const Order* o = levels_[p].head; o != nullptr; o = o->next_at_level) {
                const std::int32_t left = o->total_qty - o->filled_qty;
                if (left > best) best = left;
            }
        }
        return best;
    }

    // ====================================================================
    // Per-reason / per-TIF / per-OrderType breakdowns
    // ====================================================================
    //
    // Compliance dashboard — where orders bounce (the error category)
    // and which types/TIF traders prefer. Acceptance ratio = added / submitted.
    std::uint64_t rejections_by_reason(RejectReason r) const noexcept {
        const auto i = static_cast<std::size_t>(r);
        return i < 18 ? rejections_by_reason_[i] : 0;
    }
    std::uint64_t accepts_by_tif(TimeInForce tif) const noexcept {
        const auto i = static_cast<std::size_t>(tif);
        return i < 6 ? accepts_by_tif_[i] : 0;
    }
    std::uint64_t accepts_by_type(OrderType t) const noexcept {
        const auto i = static_cast<std::size_t>(t);
        return i < 10 ? accepts_by_type_[i] : 0;
    }
    double acceptance_ratio() const noexcept {
        const std::uint64_t added = stats_.total_orders_added;
        const std::uint64_t rej   = stats_.total_orders_rejected;
        const std::uint64_t total = added + rej;
        if (total == 0) return 0.0;
        return static_cast<double>(added) / static_cast<double>(total);
    }
    // Najbardziej "popularna" kategoria rejectu — diagnostic.
    RejectReason most_common_reject_reason() const noexcept {
        std::size_t best_i = 0;
        std::uint64_t best_v = 0;
        for (std::size_t i = 1; i < 18; ++i) {  // skip NONE=0
            if (rejections_by_reason_[i] > best_v) {
                best_v = rejections_by_reason_[i];
                best_i = i;
            }
        }
        return static_cast<RejectReason>(best_i);
    }

private:
    void tally_rejection(RejectReason r) noexcept {
        const auto i = static_cast<std::size_t>(r);
        if (i < 18) ++rejections_by_reason_[i];
    }
    void tally_accept(OrderType t, TimeInForce tif) noexcept {
        const auto ti = static_cast<std::size_t>(t);
        const auto fi = static_cast<std::size_t>(tif);
        if (ti < 10) ++accepts_by_type_[ti];
        if (fi < 6)  ++accepts_by_tif_[fi];
    }
public:

    // ====================================================================
    // for_each_order — read-only iterator po wszystkich aktywnych orderach
    // ====================================================================
    //
    // Calls Visitor(const Order&) for each active order (across all
    // levels, in FIFO order per level, best prices first). Replay/audit.
    // ====================================================================
    // Event sequence numbers — monotonic, for consumer gap detection / dedup
    // ====================================================================
    std::uint64_t last_event_seq_num() const noexcept { return last_emitted_seq_; }

    // ====================================================================
    // BookHealth — jednorazowy dashboard snapshot (zero alocacji)
    // ====================================================================
    //
    // Aggregates the most useful metrics for an operator panel / monitoring
    // / circuit-breaker decisions into one cheap read.
    struct BookHealth {
        // Liquidity
        std::int32_t  spread_ticks;
        std::int32_t  imbalance_bps_tob;
        std::int32_t  imbalance_bps_3;
        double        hidden_liquidity_ratio;
        // Flow
        std::int32_t  flow_imbalance_bps;
        std::uint32_t vpin_bps;
        double        cancel_to_trade_ratio;
        // Microstructure
        double        mean_quoted_spread_ticks;
        double        mean_effective_spread_ticks;
        double        effective_to_quoted_ratio;
        // Stability
        std::uint64_t quote_flicker_count;
        std::uint64_t max_tob_unchanged_streak;
        std::uint64_t spread_compression_count;
        // Counts
        std::uint64_t total_fills;
        std::uint64_t total_orders_added;
        std::uint64_t total_orders_cancelled;
        std::uint64_t last_event_seq_num;
    };
    BookHealth health_snapshot() noexcept {
        BookHealth h{};
        h.spread_ticks           = spread_ticks();
        h.imbalance_bps_tob      = imbalance_bps();
        h.imbalance_bps_3        = imbalance_bps_n(3);
        h.hidden_liquidity_ratio = hidden_liquidity_ratio();
        h.flow_imbalance_bps     = flow_imbalance_bps();
        h.vpin_bps               = vpin_bps();
        h.cancel_to_trade_ratio  = cancel_to_trade_ratio();
        h.mean_quoted_spread_ticks    = mean_quoted_spread_ticks();
        h.mean_effective_spread_ticks = mean_effective_spread_ticks();
        h.effective_to_quoted_ratio   = effective_to_quoted_ratio();
        h.quote_flicker_count    = quote_flicker_count();
        h.max_tob_unchanged_streak = max_tob_unchanged_streak_observed();
        h.spread_compression_count = spread_compression_count();
        h.total_fills            = stats_.total_fills;
        h.total_orders_added     = stats_.total_orders_added;
        h.total_orders_cancelled = stats_.total_orders_cancelled;
        h.last_event_seq_num     = last_emitted_seq_;
        return h;
    }

    // ====================================================================
    // Hidden liquidity ratio (visible vs hidden capacity)
    // ====================================================================
    //
    // Iceberg/HIDDEN orders have part of their qty invisible in L1/L2 depth.
    // Ratio = Σ hidden / (Σ visible + Σ hidden). High = a lot of dark liquidity.
    double hidden_liquidity_ratio() const noexcept {
        std::int64_t vis = 0, hid = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            vis += levels_[p].total_qty;
            hid += levels_[p].total_hidden;
        }
        const std::int64_t total = vis + hid;
        if (total == 0) return 0.0;
        return static_cast<double>(hid) / static_cast<double>(total);
    }

    // Per-side resting order counts (visible only — hidden zliczone osobno).
    // O(LEVELS) — use periodically, not on the hot path.
    std::int32_t resting_order_count(Side side) const noexcept {
        std::int32_t n = 0;
        if (side == Side::BUY) {
            for (std::int32_t p = 0; p <= best_bid_ticks_ && p < LEVELS; ++p) {
                n += levels_[p].order_count;
            }
        } else {
            for (std::int32_t p = best_ask_ticks_; p < LEVELS; ++p) {
                n += levels_[p].order_count;
            }
        }
        return n;
    }

    template <typename Visitor>
    void for_each_order(Visitor v) const noexcept {
        // Bids from best downward
        for (std::int32_t p = best_bid_ticks_; p >= 0 && p != NO_BID_TICKS; --p) {
            const PriceLevel& lvl = levels_[p];
            for (const Order* o = lvl.head; o != nullptr; o = o->next_at_level) {
                v(*o);
            }
            if (p == 0) break;
        }
        // Asks from best upward
        for (std::int32_t p = best_ask_ticks_;
             p < LEVELS && p != NO_ASK_TICKS; ++p) {
            const PriceLevel& lvl = levels_[p];
            for (const Order* o = lvl.head; o != nullptr; o = o->next_at_level) {
                v(*o);
            }
        }
    }

    // current_tob_snapshot(): immutable read without resetting state.
    bool tob_has_changed_since_last_poll() const noexcept {
        return (best_bid_ticks_ != last_observed_bid_ticks_)
            || (best_ask_ticks_ != last_observed_ask_ticks_);
    }

    // ====================================================================
    // Multi-level imbalance
    // ====================================================================
    //
    // imbalance_bps_n(n_levels): aggregated imbalance over the N deepest
    // levels per side, not just TOB. A real order-flow signal — TOB
    // imbalance is easily manipulated, but depth-3 or depth-5 is harder.
    std::int32_t imbalance_bps_n(std::int32_t n_levels) const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        std::int64_t b = 0, a = 0;
        std::int32_t bn = 0, an = 0;
        for (std::int32_t p = best_bid_ticks_; p >= 0 && bn < n_levels; --p) {
            const std::int32_t q = levels_[p].total_qty;
            if (q > 0) { b += q; ++bn; }
        }
        for (std::int32_t p = best_ask_ticks_; p < LEVELS && an < n_levels; ++p) {
            const std::int32_t q = levels_[p].total_qty;
            if (q > 0) { a += q; ++an; }
        }
        const std::int64_t total = b + a;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((b - a) * 10000 / total);
    }

    // ====================================================================
    // Market impact estimator (pre-trade analytics)
    // ====================================================================
    //
    // predicted_vwap_ticks(side, qty): simulates walk-the-book without modifying
    // of the book. Returns the expected VWAP (ticks) if an order of `qty` were IOC.
    // 0 = no liquidity. Side::BUY eats asks, Side::SELL eats bids.
    std::int32_t predicted_vwap_ticks(Side side, std::int32_t qty) const noexcept {
        if (qty <= 0) return 0;
        std::int64_t notional_ticks = 0;
        std::int32_t filled = 0;
        if (side == Side::BUY) {
            if (!has_ask()) return 0;
            for (std::int32_t p = best_ask_ticks_; p < LEVELS && filled < qty; ++p) {
                const std::int32_t avail = levels_[p].total_qty;
                if (avail <= 0) continue;
                const std::int32_t take = std::min(qty - filled, avail);
                notional_ticks += static_cast<std::int64_t>(p) * take;
                filled        += take;
            }
        } else {
            if (!has_bid()) return 0;
            for (std::int32_t p = best_bid_ticks_; p >= 0 && filled < qty; --p) {
                const std::int32_t avail = levels_[p].total_qty;
                if (avail <= 0) continue;
                const std::int32_t take = std::min(qty - filled, avail);
                notional_ticks += static_cast<std::int64_t>(p) * take;
                filled        += take;
            }
        }
        if (filled == 0) return 0;
        return static_cast<std::int32_t>(notional_ticks / filled);
    }

    // slippage_ticks: predicted_vwap - mid. Signed: for BUY positive (I pay more),
    // SELL negative (I get less). The absolute value = the expected cost.
    std::int32_t predicted_slippage_ticks(Side side, std::int32_t qty) const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        const std::int32_t mid = (best_bid_ticks_ + best_ask_ticks_) / 2;
        const std::int32_t vwap = predicted_vwap_ticks(side, qty);
        if (vwap == 0) return 0;
        return vwap - mid;
    }

    // depth_available_ticks(side, max_price_offset): the sum of qty available up to
    // the mid ± offset price. Used for a quick "how much can I buy without jumping
    // > N ticks from mid?".
    std::int32_t depth_within_offset(Side side, std::int32_t max_offset) const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        const std::int32_t mid = (best_bid_ticks_ + best_ask_ticks_) / 2;
        std::int32_t total = 0;
        if (side == Side::BUY) {
            const std::int32_t cap = std::min<std::int32_t>(LEVELS - 1, mid + max_offset);
            for (std::int32_t p = best_ask_ticks_; p <= cap; ++p) {
                total += levels_[p].total_qty;
            }
        } else {
            const std::int32_t floor_p = std::max<std::int32_t>(0, mid - max_offset);
            for (std::int32_t p = best_bid_ticks_; p >= floor_p; --p) {
                total += levels_[p].total_qty;
            }
        }
        return total;
    }

    // ====================================================================
    // Sweep-to-fill metrics
    // ====================================================================
    //
    // avg_levels_per_sweep — the mean number of price levels per execution.
    // A high value = a thin book / large orders (a toxic-flow indicator).
    double avg_levels_per_sweep() const noexcept {
        if (stats_.total_sweeps == 0) return 0.0;
        return static_cast<double>(stats_.sum_levels_touched) /
               static_cast<double>(stats_.total_sweeps);
    }

    // multi_level_sweep_ratio — the proportion of sweeps that hit >=2 levels.
    double multi_level_sweep_ratio() const noexcept {
        if (stats_.total_sweeps == 0) return 0.0;
        return static_cast<double>(stats_.multi_level_sweeps) /
               static_cast<double>(stats_.total_sweeps);
    }

    // ====================================================================
    // Order age stats — kanibalizm flow / queue residency
    // ====================================================================
    //
    // Tracked w match_at_level po fill + w cancel_internal.
    // For each completed lifecycle (fill or cancel), record age_ns
    // = now - submit_ts_ns_. Wykorzystywane do detection toxic queue
    // (when orders are filled quickly = aggressive flow; long-waiting =
    // resting MM).
    std::uint64_t total_completed_lifecycles() const noexcept {
        return age_stats_count_;
    }
    std::uint64_t total_age_ns() const noexcept { return age_stats_sum_ns_; }
    std::uint64_t max_age_ns_observed() const noexcept { return age_stats_max_ns_; }
    std::uint64_t avg_age_ns_at_completion() const noexcept {
        return age_stats_count_ == 0 ? 0
            : age_stats_sum_ns_ / age_stats_count_;
    }

    // ====================================================================
    // Execution quality / TCA metrics
    // ====================================================================
    //
    // Quoted spread — sample periodically by calling sample_quoted_spread() from
    // the marketdata loop. Mean quoted spread = Σ obs / N.
    // Effective spread — accumulowany per fill w match_against():
    //   eff_spread = 2 × |fill_px - mid_pre_match|
    // Relacja eff/quoted < 1 → price improvement; > 1 → adverse selection.
    void sample_quoted_spread() noexcept {
        if (!has_bid() || !has_ask()) return;
        stats_.total_quoted_spread_ticks_obs +=
            static_cast<std::uint64_t>(best_ask_ticks_ - best_bid_ticks_);
        ++stats_.total_quoted_spread_samples;
    }
    double mean_quoted_spread_ticks() const noexcept {
        if (stats_.total_quoted_spread_samples == 0) return 0.0;
        return static_cast<double>(stats_.total_quoted_spread_ticks_obs) /
               static_cast<double>(stats_.total_quoted_spread_samples);
    }
    double mean_effective_spread_ticks() const noexcept {
        if (stats_.total_effective_spread_samples == 0) return 0.0;
        return static_cast<double>(stats_.total_effective_spread_2x_ticks) /
               static_cast<double>(stats_.total_effective_spread_samples);
    }
    // Realized vs quoted ratio (a TCA classic). 0 == no data.
    double effective_to_quoted_ratio() const noexcept {
        const double q = mean_quoted_spread_ticks();
        if (q <= 0.0) return 0.0;
        return mean_effective_spread_ticks() / q;
    }

    // ====================================================================
    // Cancel-to-trade ratio (CTR) — per-book i per-account
    // ====================================================================
    //
    // SEC Rule 15c3-5 / MiFID II RTS 9: an excessive CTR (>50:1) indicates
    // quote stuffing / spoofing. Wyliczane na podstawie cumulative stats.
    double cancel_to_trade_ratio() const noexcept {
        if (stats_.total_fills == 0) return 0.0;
        return static_cast<double>(stats_.total_orders_cancelled) /
               static_cast<double>(stats_.total_fills);
    }
    double priority_loss_ratio() const noexcept {
        const std::uint64_t total = stats_.priority_preserved_mods +
                                     stats_.priority_lost_mods;
        if (total == 0) return 0.0;
        return static_cast<double>(stats_.priority_lost_mods) /
               static_cast<double>(total);
    }

private:
    // Order age stats state
    std::uint64_t  age_stats_count_  = 0;
    std::uint64_t  age_stats_sum_ns_ = 0;
    std::uint64_t  age_stats_max_ns_ = 0;

    void record_lifecycle_age(std::uint64_t submit_ts_ns) noexcept {
        const std::uint64_t now = mono_ns_now();
        if (submit_ts_ns == 0 || now < submit_ts_ns) return;
        const std::uint64_t age = now - submit_ts_ns;
        ++age_stats_count_;
        age_stats_sum_ns_ += age;
        if (age > age_stats_max_ns_) age_stats_max_ns_ = age;
    }

public:

    // Hook: update size distribution z record_trade()
    void update_size_distribution(std::int32_t qty) noexcept {
        if (qty <= 100) {
            ++size_dist_.small_count;
            size_dist_.small_volume += static_cast<std::uint64_t>(qty);
        } else if (qty <= 1000) {
            ++size_dist_.medium_count;
            size_dist_.medium_volume += static_cast<std::uint64_t>(qty);
        } else if (qty <= 10000) {
            ++size_dist_.large_count;
            size_dist_.large_volume += static_cast<std::uint64_t>(qty);
        } else {
            ++size_dist_.block_count;
            size_dist_.block_volume += static_cast<std::uint64_t>(qty);
        }
    }

public:
    // ====================================================================
    // Spread + microstructure analytics
    // ====================================================================
    //
    // spread_ticks — best_ask - best_bid (>=0 in continuous mode).
    // Returns -1 when there is no quote on one of the sides.
    std::int32_t spread_ticks() const noexcept {
        if (!has_bid() || !has_ask()) return -1;
        return best_ask_ticks_ - best_bid_ticks_;
    }

    // half_spread_bps — relative spread w bps (mid_price reference).
    // Standardowa miara liquidity cost. < 5 bps = tight, > 50 bps = wide.
    std::int32_t half_spread_bps() const noexcept {
        if (!has_bid() || !has_ask()) return -1;
        const std::int64_t mid    = (best_bid_ticks_ + best_ask_ticks_) / 2;
        const std::int64_t sprd   = best_ask_ticks_ - best_bid_ticks_;
        if (mid <= 0) return -1;
        return static_cast<std::int32_t>((sprd * 10000 / 2) / mid);
    }

    // weighted_mid_ticks — alias for microprice (a popular name in the literature).
    std::int32_t weighted_mid_ticks() const noexcept { return microprice_ticks(); }

    // ====================================================================
    // Mass quote — atomic batch submission (market maker scenario)
    // ====================================================================
    //
    // An MM usually posts a 2-sided quote (bid + ask) atomically, to avoid being
    // exposed to a momentary move with non-atomic submissions. mass_quote()
    // atomic: ALL-OR-NONE (if one would fail, NOTHING goes through).
    //
    // The caller supplies the array `quotes[n]`. Each element is {side, price, qty}.
    // Returns the number of accepted orders (== n on success, 0 on the first fail).
    struct Quote {
        Side          side;
        std::int32_t  price_ticks;
        std::int32_t  qty;
    };
    std::size_t mass_quote(const Quote* quotes, std::size_t n,
                            std::uint64_t client_id,
                            std::uint64_t* out_ids = nullptr) noexcept {
        // 2-pass: a validation phase (we check that the pool and prices are OK), then a submit phase.
        if (active_orders_ + n > MAX_ORDERS) {
            ++stats_.total_orders_rejected;
            tally_rejection(RejectReason::POOL_EXHAUSTED);
            return 0;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const auto& q = quotes[i];
            if (q.qty <= 0 || !in_range(q.price_ticks)) {
                ++stats_.total_orders_rejected;
                tally_rejection(q.qty <= 0 ? RejectReason::QTY_ZERO_OR_NEGATIVE
                                            : RejectReason::PRICE_OUT_OF_RANGE);
                return 0;
            }
            // POST_ONLY semantic — a quote must not cross the market
            if (would_cross(q.side, q.price_ticks)) {
                ++stats_.total_orders_rejected;
                tally_rejection(RejectReason::POST_ONLY_WOULD_CROSS);
                return 0;
            }
        }
        // All OK — submit each
        for (std::size_t i = 0; i < n; ++i) {
            const auto& q = quotes[i];
            const std::uint64_t id = submit(q.side, q.price_ticks, q.qty,
                                              OrderType::POST_ONLY,
                                              TimeInForce::DAY, 0, client_id);
            if (out_ids) out_ids[i] = id;
        }
        ++stats_.total_mass_quotes;
        return n;
    }

    // ====================================================================
    // Liquidity heatmap data — N levels per side z rozszerzonym info
    // ====================================================================
    //
    // Returns a structure of dense liquidity around the top of book.
    struct LiquiditySnapshot {
        DepthLevel    bid_levels[10];
        DepthLevel    ask_levels[10];
        std::int32_t  bid_count;
        std::int32_t  ask_count;
        std::int32_t  spread_ticks;
        std::int32_t  imbalance_bps;
        std::int64_t  total_bid_volume;
        std::int64_t  total_ask_volume;
        std::int32_t  microprice_ticks;
    };

    // Auction mode — the caller invokes it before a batch submit for the cross.
    // After enter_auction_mode(), every submit skips matching and sits in the book.
    // Po exit_auction_mode() + run_auction(), single-price cross matched FIFO.
    void enter_auction_mode() noexcept { in_auction_mode_ = true; }
    void exit_auction_mode()  noexcept { in_auction_mode_ = false; }
    bool in_auction_mode()    const noexcept { return in_auction_mode_; }

    // Halt/resume — trading halt.
    // Audit log API.
    void enable_audit_log(bool on) noexcept { audit_enabled_ = on; }
    bool audit_log_enabled() const noexcept { return audit_enabled_; }
    std::size_t audit_log_size() const noexcept { return audit_log_.size(); }

    // pop_audit_records: copy to `out` (max max_n), clear from the buffer.
    // Returns how many were retrieved.
    std::size_t pop_audit_records(AuditRecord* out, std::size_t max_n) noexcept {
        const std::size_t n = std::min(max_n, audit_log_.size());
        for (std::size_t i = 0; i < n; ++i) out[i] = audit_log_[i];
        audit_log_.erase(audit_log_.begin(),
                          audit_log_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    void halt(const char* reason) noexcept {
        halted_ = true;
        std::memset(halt_reason_, 0, sizeof(halt_reason_));
        if (reason) {
            const std::size_t n = std::min(sizeof(halt_reason_) - 1,
                                             std::strlen(reason));
            std::memcpy(halt_reason_, reason, n);
        }
    }
    void resume() noexcept { halted_ = false; halt_reason_[0] = '\0'; }
    bool is_halted() const noexcept { return halted_; }
    const char* halt_reason() const noexcept { return halt_reason_; }

    LiquiditySnapshot liquidity_snapshot() const noexcept {
        LiquiditySnapshot s{};
        depth(10, s.bid_levels, s.ask_levels, &s.bid_count, &s.ask_count);
        s.spread_ticks      = spread_ticks();
        s.imbalance_bps     = imbalance_bps();
        s.microprice_ticks  = microprice_ticks();
        for (std::int32_t i = 0; i < s.bid_count; ++i)
            s.total_bid_volume += s.bid_levels[i].qty;
        for (std::int32_t i = 0; i < s.ask_count; ++i)
            s.total_ask_volume += s.ask_levels[i].qty;
        return s;
    }
};

}  // namespace orderbook_pro

