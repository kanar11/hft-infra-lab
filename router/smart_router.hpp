/*
 * SmartOrderRouter (SOR) — chooses which exchange to send an order to.
 *
 * The same stock trades in parallel on many exchanges (NYSE, NASDAQ,
 * BATS, IEX, ARCA...). Each has a different price, a different fee and a different latency.
 * The SOR decides where to send the order to achieve "best execution".
 *
 * Three strategies:
 *   BEST_PRICE      — the best EFFECTIVE price (all-in, with fee/rebate)
 *   LOWEST_LATENCY  — the fastest venue round-trip
 *   SPLIT           — split a large order across venues by available liquidity
 *
 * Effective price (all-in) — a key piece of realism:
 *   A real SOR does NOT optimize the raw quote price, but the price AFTER fees.
 *   A venue with asks $223.48 and a $0.005/share fee gives all-in $223.485 — worse
 *   than $223.49 with a -$0.002 rebate (all-in $223.488)... actually better, but
 *   with larger fees the ordering flips. Maker/taker model: a positive
 *   fee = you take liquidity (taker), negative = you provide liquidity (maker rebate).
 *
 *     BUY:  all_in = ask + fee   (you pay; a negative rebate → you pay less)
 *     SELL: all_in = bid - fee   (you receive; a negative rebate → you receive more)
 *
 *   BEST_PRICE for a BUY minimizes all_in, for a SELL maximizes all_in.
 *
 * Pipeline: Strategy → Risk → ROUTER → OMS → Exchange.
 *
 * Performance (lab): ~9.7M routes/sec, p50=70ns, p99=150ns.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>


// Max number of venues (fixed array, zero heap on the hot path).
static constexpr int MAX_VENUES = 16;


enum class RoutingStrategy : uint8_t {
    BEST_PRICE      = 0,
    LOWEST_LATENCY  = 1,
    SPLIT           = 2
};


// RouteReject — WHY route_order did not return a route (#125). The caller (OMS /
// strategy) distinguishes "nowhere to route" from "no liquidity right now".
enum class RouteReject : uint8_t {
    NONE          = 0,   // route found (valid=true)
    NO_VENUES     = 1,   // zero active venues at all
    NO_LIQUIDITY  = 2,   // venues exist, but no liquidity on the order's side
};

inline const char* route_reject_str(RouteReject r) noexcept {
    switch (r) {
        case RouteReject::NONE:         return "NONE";
        case RouteReject::NO_VENUES:    return "NO_VENUES";
        case RouteReject::NO_LIQUIDITY: return "NO_LIQUIDITY";
        default:                        return "UNKNOWN";
    }
}


// Venue — a single exchange with its top-of-book quote, fee and latency.
//
// latency_ns (mean): average round-trip latency. If latency_p99_ns > 0
// (opt-in), LOWEST_LATENCY selects by it instead of by the mean — this is the real
// behavior of production SORs, because the tail is long (p99/p50 usually 5-10×)
// and decides execution quality more than the average case.
struct Venue {
    char    name[16];
    int64_t latency_ns;          // static mean (seeded from config / venue SLA)
    int64_t latency_p99_ns;      // p99 — 0 = unset, then LOWEST_LATENCY uses the mean
    double  fee_per_share;       // positive = taker fee, negative = maker rebate
    double  best_bid;
    double  best_ask;
    int32_t bid_size;
    int32_t ask_size;
    bool    is_active;
    // EWMA of the measured round-trip latency (0 = no samples). Updated
    // by record_latency() from real executions — routing adapts to
    // current conditions instead of trusting static numbers from config.
    double  ewma_latency_ns;
    uint32_t latency_samples;
    uint32_t consecutive_failures;  // health (#86): streak of rejects/timeouts
    int64_t  routed_shares;         // TCA (#117): total routed volume
    uint64_t routes_count;          // how many times an order landed on this venue
    int64_t  last_quote_ns = 0;     // when the quote was last refreshed (#392; 0 = never)

    Venue() noexcept
        : latency_ns(0), latency_p99_ns(0), fee_per_share(0),
          best_bid(0), best_ask(0), bid_size(0), ask_size(0), is_active(true),
          ewma_latency_ns(0.0), latency_samples(0), consecutive_failures(0),
          routed_shares(0), routes_count(0) {
        name[0] = '\0';
    }

    Venue(const char* n, int64_t lat, double fee) noexcept
        : latency_ns(lat), latency_p99_ns(0), fee_per_share(fee),
          best_bid(0), best_ask(0), bid_size(0), ask_size(0), is_active(true),
          ewma_latency_ns(0.0), latency_samples(0), consecutive_failures(0),
          routed_shares(0), routes_count(0) {
        std::strncpy(name, n, 15);
        name[15] = '\0';
    }

    // selection_latency_ns: which latency LOWEST_LATENCY uses.
    // Priority: measured EWMA (when samples exist) > p99 (when set) > mean.
    // A real SOR prefers the OBSERVED latency over the declared one — a venue slows
    // under load and we react in subsequent decisions.
    int64_t selection_latency_ns() const noexcept {
        if (latency_samples > 0) return static_cast<int64_t>(ewma_latency_ns);
        return latency_p99_ns > 0 ? latency_p99_ns : latency_ns;
    }
};


// RouteDecision — what the router returns. Besides venue/price/qty it now exposes
// the effective price and the total fee, so the caller sees the REAL cost
// (not just the quote). num_venues > 1 for SPLIT.
struct RouteDecision {
    char    venue[16];          // for SPLIT: the venue with the largest allocation
    double  price;              // average execution price (quote only, no fees)
    double  effective_price;    // all-in per share (quote ± fee)
    double  total_fee;          // total fee for the whole order (can be negative = rebate)
    int32_t quantity;           // shares actually routed (filled)
    int32_t unfilled_qty;       // shortfall: no liquidity to cover the rest
    int32_t num_venues;         // how many venues used (SPLIT)
    int64_t latency_ns;         // router decision time (not the venue round-trip!)
    bool    valid;              // false = no route (like None in Python)
    RouteReject reject_reason;  // why there is no route (#125; NONE when valid)

    RouteDecision() noexcept
        : price(0), effective_price(0), total_fee(0), quantity(0),
          unfilled_qty(0), num_venues(0), latency_ns(0), valid(false),
          reject_reason(RouteReject::NONE) {
        venue[0] = '\0';
    }
};


class SmartOrderRouter {
    Venue           venues_[MAX_VENUES];
    int             venue_count_;
    RoutingStrategy default_strategy_;
    int32_t         split_threshold_;
    uint32_t        max_failures_ = 3;   // health (#86): auto-disable threshold for a venue

    uint64_t total_routes_;
    uint64_t total_rejected_;
    uint64_t total_latency_ns_;
    double   total_fees_paid_ = 0.0;   // sum of fee/rebate over routes (#138; <0 = net rebate)

    static int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    // Effective price (all-in) — the quote adjusted for fee/rebate.
    // BUY lower = better, SELL higher = better.
    static double effective_price(const Venue& v, bool is_buy) noexcept {
        return is_buy ? v.best_ask + v.fee_per_share
                      : v.best_bid - v.fee_per_share;
    }

public:
    SmartOrderRouter(RoutingStrategy strat = RoutingStrategy::BEST_PRICE,
                     int32_t split_thresh = 500) noexcept
        : venue_count_(0),
          default_strategy_(strat),
          split_threshold_(split_thresh),
          total_routes_(0),
          total_rejected_(0),
          total_latency_ns_(0) {}

    void add_venue(const Venue& v) noexcept {
        if (venue_count_ < MAX_VENUES) venues_[venue_count_++] = v;
    }

    // remove_venue: completely remove a venue from routing (#170) — decommission,
    // not just disable (set_venue_active). Shifts the array. Returns false
    // for an unknown venue.
    bool remove_venue(const char* venue_name) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                for (int j = i; j < venue_count_ - 1; ++j) venues_[j] = venues_[j + 1];
                --venue_count_;
                return true;
            }
        }
        return false;
    }

    // record_latency: feed the EWMA a measured round-trip latency (ns) from a real
    // execution. alpha=0.2 — fresh samples weigh 20%, smooths noise but reacts
    // to a trend. After the first sample EWMA = it; then exponential smoothing.
    void record_latency(const char* venue_name, int64_t observed_ns) noexcept {
        if (observed_ns <= 0) return;
        constexpr double alpha = 0.2;
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                Venue& v = venues_[i];
                v.ewma_latency_ns = (v.latency_samples == 0)
                    ? static_cast<double>(observed_ns)
                    : alpha * static_cast<double>(observed_ns) + (1.0 - alpha) * v.ewma_latency_ns;
                ++v.latency_samples;
                v.consecutive_failures = 0;   // a successful round-trip = a healthy venue
                v.is_active = true;
                return;
            }
        }
    }

    // === Venue health (#86) ===
    // A real SOR stops routing to a venue that rejects/times out, and returns
    // when it responds again. record_reject counts the failure streak (≥ threshold → disable);
    // record_success / record_latency reset the streak and reactivate.
    void set_failure_threshold(uint32_t n) noexcept { if (n > 0) max_failures_ = n; }

    void record_reject(const char* venue_name) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                if (++venues_[i].consecutive_failures >= max_failures_)
                    venues_[i].is_active = false;     // dropped from the candidate pool
                return;
            }
        }
    }
    void record_success(const char* venue_name) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].consecutive_failures = 0;
                venues_[i].is_active = true;
                return;
            }
        }
    }
    bool venue_active(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0) return venues_[i].is_active;
        return false;
    }
    // set_venue_active: manual enable/disable of a venue (#146) — admin control
    // (maintenance, regulatory halt) independent of auto-health. Enabling resets
    // the failure streak. Returns false for an unknown venue.
    bool set_venue_active(const char* venue_name, bool active) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].is_active = active;
                if (active) venues_[i].consecutive_failures = 0;
                return true;
            }
        }
        return false;
    }
    // venue_ewma_latency: the measured EWMA of a venue's latency in ns (#146; 0 = no samples).
    double venue_ewma_latency(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0) return venues_[i].ewma_latency_ns;
        return 0.0;
    }

    // venue_failure_streak: the venue's CURRENT consecutive-failure count
    // (#416) — the number record_reject grows and record_success zeroes,
    // and the distance to the auto-disable threshold (#86). Before #416 the
    // streak was invisible from outside: a venue either looked fine or was
    // suddenly gone. -1 for an unknown venue.
    int32_t venue_failure_streak(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0)
                return static_cast<int32_t>(venues_[i].consecutive_failures);
        return -1;
    }

    // least_healthy_venue: the ACTIVE venue closest to tripping the
    // auto-disable (#416) — the actionable WHICH of venue health: reroute
    // its flow or investigate BEFORE the breaker fires and the venue
    // vanishes from the candidate pool. Writes its streak into out_streak.
    // nullptr (out untouched) when no active venue has a failure streak.
    const char* least_healthy_venue(int32_t& out_streak) const noexcept {
        const char* who   = nullptr;
        uint32_t    worst = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (v.consecutive_failures > worst) {
                worst = v.consecutive_failures;
                who   = v.name;
            }
        }
        if (who != nullptr) out_streak = static_cast<int32_t>(worst);
        return who;
    }

    // update_quote: update a venue's top-of-book (on every market data tick).
    // quote_ts_ns (#392): when this refresh happened — 0 (the default, and
    // all pre-#392 call sites) stamps the real clock; tests pass synthetic
    // times so the staleness reads below stay deterministic.
    void update_quote(const char* venue_name, double bid, double ask,
                      int32_t bid_size, int32_t ask_size,
                      int64_t quote_ts_ns = 0) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].best_bid = bid;
                venues_[i].best_ask = ask;
                venues_[i].bid_size = bid_size;
                venues_[i].ask_size = ask_size;
                venues_[i].last_quote_ns = (quote_ts_ns != 0) ? quote_ts_ns : now_ns();
                return;
            }
        }
    }

    // fresh_venue_count: how many ACTIVE venues carry a quote at most
    // max_age_ns old at at_ns (#448) — the BREADTH of the fresh family.
    // fresh_nbbo_mid (#424), fresh_available_liquidity (#432) and
    // sweep_to_fill_fresh (#440) say what the fresh market looks like;
    // this says how MANY independent venues back that picture — a fresh
    // NBBO resting on one venue is one outage away from nothing (the
    // market-coverage analog of multicast channels_with_gaps #395).
    // Never-quoted venues are excluded; same clock injection as #392.
    int fresh_venue_count(int64_t max_age_ns, int64_t at_ns) const noexcept {
        int n = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active || v.last_quote_ns == 0) continue;
            if (at_ns - v.last_quote_ns > max_age_ns) continue;
            ++n;
        }
        return n;
    }

    // venue_quote_age_ns: ns since the venue's quote was refreshed, against
    // a caller-supplied clock (#392; same injection pattern as OMS #388).
    // -1 when the venue is unknown or has NEVER quoted — 0 would look
    // freshly quoted. Clamped at 0 when now precedes the stamp.
    int64_t venue_quote_age_ns(const char* venue_name, int64_t at_ns) const noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) != 0) continue;
            if (venues_[i].last_quote_ns == 0) return -1;
            const int64_t age = at_ns - venues_[i].last_quote_ns;
            return age > 0 ? age : 0;
        }
        return -1;
    }

    // stalest_quote_age_ns: the WORST quote age among ACTIVE venues that
    // have quoted at least once (#392) — the freshness of the weakest input
    // feeding every NBBO view (nbbo_mid/microprice/imbalance/...). An NBBO
    // leg resting on a quote nobody refreshed is toxic: it looks like
    // liquidity but the market has moved on. Venues that never quoted are
    // excluded (they feed the NBBO nothing). -1 when no active venue has
    // quoted yet.
    int64_t stalest_quote_age_ns(int64_t at_ns) const noexcept {
        int64_t worst = -1;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active || v.last_quote_ns == 0) continue;
            const int64_t age = at_ns - v.last_quote_ns;
            const int64_t clamped = age > 0 ? age : 0;
            if (clamped > worst) worst = clamped;
        }
        return worst;
    }

    // route_order: choose a venue for the order. side[0]=='B' → buy.
    RouteDecision route_order(const char* side, int32_t quantity,
                               RoutingStrategy strat) noexcept {
        const int64_t t0     = now_ns();
        const bool    is_buy = (side[0] == 'B');

        // Gather active candidates with actual liquidity on our side.
        Venue* candidates[MAX_VENUES];
        int    num_candidates = 0;
        for (int i = 0; i < venue_count_; ++i) {
            if (!venues_[i].is_active) continue;
            const bool has_liq = is_buy
                ? (venues_[i].best_ask > 0 && venues_[i].ask_size > 0)
                : (venues_[i].best_bid > 0 && venues_[i].bid_size > 0);
            if (has_liq) candidates[num_candidates++] = &venues_[i];
        }
        if (num_candidates == 0) {
            ++total_rejected_;
            RouteDecision d;
            d.reject_reason = (venue_count_ == 0) ? RouteReject::NO_VENUES
                                                  : RouteReject::NO_LIQUIDITY;
            return d;
        }

        if (strat == RoutingStrategy::SPLIT && quantity >= split_threshold_) {
            return split_order(candidates, num_candidates, is_buy, quantity, t0);
        }

        // Single-venue strategies: BEST_PRICE (effective price) or LOWEST_LATENCY.
        Venue* best = candidates[0];
        double best_eff;
        if (strat == RoutingStrategy::LOWEST_LATENCY) {
            // Select by selection_latency (p99 when set, otherwise mean).
            // Production SORs look at the tail of the distribution, not the average — p99
            // decides execution quality under stress/burst.
            for (int i = 1; i < num_candidates; ++i) {
                if (candidates[i]->selection_latency_ns() < best->selection_latency_ns())
                    best = candidates[i];
            }
            best_eff = effective_price(*best, is_buy);   // only after the final pick is known
        } else {
            // BEST_PRICE (and the fallback for SPLIT below the threshold) — effective price.
            best_eff = effective_price(*best, is_buy);
            for (int i = 1; i < num_candidates; ++i) {
                const double eff    = effective_price(*candidates[i], is_buy);
                const bool   better = is_buy ? (eff < best_eff) : (eff > best_eff);
                if (better) { best = candidates[i]; best_eff = eff; }
            }
        }
        // best_eff now holds effective_price(*best, is_buy) — reused below for
        // d.effective_price instead of recomputing it a second time.

        // Respect the available liquidity on the chosen venue — a large order can
        // exceed the top-of-book size. The rest is shortfall (the caller re-routes
        // or waits). A real SOR does not promise a fill beyond the visible size.
        const int32_t available = is_buy ? best->ask_size : best->bid_size;
        const int32_t filled    = std::min(quantity, available);

        RouteDecision d;
        d.valid           = true;
        std::strncpy(d.venue, best->name, 15);
        d.venue[15]       = '\0';
        d.price           = is_buy ? best->best_ask : best->best_bid;
        d.effective_price = best_eff;
        d.total_fee       = best->fee_per_share * filled;
        d.quantity        = filled;
        d.unfilled_qty    = quantity - filled;
        d.num_venues      = 1;
        d.latency_ns      = now_ns() - t0;

        best->routed_shares += filled;   // TCA per venue (#117)
        ++best->routes_count;
        ++total_routes_;
        total_latency_ns_ += d.latency_ns;
        total_fees_paid_  += d.total_fee;   // #138 cumulative cost
        return d;
    }

    // Overload: use the default strategy.
    RouteDecision route_order(const char* side, int32_t quantity) noexcept {
        return route_order(side, quantity, default_strategy_);
    }

    // === NBBO (National Best Bid/Offer) — #97 ===
    // The best AGGREGATE price across all active venues with liquidity.
    // A reference for trade-through / best-execution; the raw quote (no fees).
    // Returns 0 when there is no liquidity on a given side.
    double national_best_bid() const noexcept {
        double best = 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.bid_size > 0 && v.best_bid > best) best = v.best_bid;
        }
        return best;
    }
    double national_best_ask() const noexcept {
        double best = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.ask_size > 0 && (!found || v.best_ask < best)) {
                best = v.best_ask; found = true;
            }
        }
        return found ? best : 0.0;
    }
    // nbbo_bid_venue / nbbo_ask_venue: which active venue holds the NBB / NBO (#294)
    // — the route target for each side. Mirrors national_best_bid/ask but returns the
    // venue name instead of the price. nullptr when no venue shows liquidity on that
    // side. Distinct from cheapest_venue (fee-adjusted) — this is the raw best quote.
    const char* nbbo_bid_venue() const noexcept {
        const char* who = nullptr; double best = 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.bid_size > 0 && v.best_bid > best) { best = v.best_bid; who = v.name; }
        }
        return who;
    }
    const char* nbbo_ask_venue() const noexcept {
        const char* who = nullptr; double best = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.ask_size > 0 && (!found || v.best_ask < best)) {
                best = v.best_ask; found = true; who = v.name;
            }
        }
        return who;
    }
    double nbbo_mid() const noexcept {
        const double b = national_best_bid(), a = national_best_ask();
        return (b > 0.0 && a > 0.0) ? (b + a) / 2.0 : 0.0;
    }
    // fresh_nbbo_mid: the NBBO mid computed ONLY from venues whose quote is
    // at most max_age_ns old at at_ns (#424) — the staleness gate (#392)
    // applied to the NBBO itself. nbbo_mid() happily averages a bid nobody
    // has refreshed in seconds with a live ask, and that blended number is
    // where mispricing starts; here a stale venue's BOTH sides drop out
    // entirely. Returns 0 when no fresh two-sided market remains — which is
    // itself the signal: the consolidated picture is not tradeable. Same
    // clock injection as the rest of the #392 family.
    double fresh_nbbo_mid(int64_t max_age_ns, int64_t at_ns) const noexcept {
        double bb = 0.0, ba = 0.0;
        bool   has_bid = false, has_ask = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active || v.last_quote_ns == 0) continue;
            if (at_ns - v.last_quote_ns > max_age_ns) continue;   // stale venue: gone
            if (v.best_bid > 0.0 && v.bid_size > 0 && (!has_bid || v.best_bid > bb)) {
                bb = v.best_bid; has_bid = true;
            }
            if (v.best_ask > 0.0 && v.ask_size > 0 && (!has_ask || v.best_ask < ba)) {
                ba = v.best_ask; has_ask = true;
            }
        }
        return (has_bid && has_ask) ? (bb + ba) / 2.0 : 0.0;
    }
    // nbbo_microprice: size-weighted consolidated mid across venues (#318). The plain
    // nbbo_mid (#402) ignores depth; the microprice weights the National Best Bid by
    // the size resting at the National Best Ask and vice-versa (the standard
    // Q_ask/Q_bid weighting), so it leans toward the side that is likely to be hit
    // next — a sharper fair-value estimate and short-horizon move predictor than the
    // mid. Uses the displayed size at the NBB venue and the NBO venue. With balanced
    // sizes it equals nbbo_mid; heavier bid size pulls it toward the ask and vice
    // versa. 0 without two-sided liquidity.
    double nbbo_microprice() const noexcept {
        double  bid = 0.0; int32_t bid_sz = 0;
        double  ask = 0.0; int32_t ask_sz = 0; bool ask_found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (v.bid_size > 0 && v.best_bid > bid) { bid = v.best_bid; bid_sz = v.bid_size; }
            if (v.ask_size > 0 && v.best_ask > 0.0 && (!ask_found || v.best_ask < ask)) {
                ask = v.best_ask; ask_sz = v.ask_size; ask_found = true;
            }
        }
        if (bid <= 0.0 || !ask_found || ask <= 0.0) return 0.0;
        const int64_t denom = static_cast<int64_t>(bid_sz) + ask_sz;
        if (denom <= 0) return 0.0;
        return (bid * ask_sz + ask * bid_sz) / static_cast<double>(denom);
    }
    // nbbo_spread: the consolidated NBBO spread (#208) = NBO - NBB across all
    // active venues. Usually TIGHTER than on a single exchange (the best bid and
    // best ask can be on different venues). <=0 signals locked/crossed between
    // venues (cross-venue arbitrage). 0 when there is no two-sided liquidity.
    double nbbo_spread() const noexcept {
        const double b = national_best_bid(), a = national_best_ask();
        return (b > 0.0 && a > 0.0) ? (a - b) : 0.0;
    }
    // nbbo_spread_bps: nbbo_spread relative to nbbo_mid in basis points — a measure of
    // consolidated market quality independent of the price level.
    double nbbo_spread_bps() const noexcept {
        const double m = nbbo_mid();
        return m > 0.0 ? nbbo_spread() / m * 10000.0 : 0.0;
    }
    // nbbo_imbalance: consolidated top-of-book pressure across venues (#326). Sums the
    // quoted size AT the national best bid (every active venue posting that exact price)
    // against the size AT the national best ask, as (bidSz - askSz)/(bidSz + askSz) in
    // [-1, 1]. +1 = all displayed weight on the bid (buy pressure), -1 = the ask, 0 =
    // balanced or one-sided. Unlike nbbo_microprice (#318), which size-weights the
    // PRICE, this is a pure depth-pressure ratio at the touch — the cross-venue analog
    // of a single book's top-of-book imbalance. 0 without two-sided NBBO liquidity.
    double nbbo_imbalance() const noexcept {
        const double nbb = national_best_bid();
        const double nbo = national_best_ask();
        if (nbb <= 0.0 || nbo <= 0.0) return 0.0;
        int64_t bid_sz = 0, ask_sz = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (v.bid_size > 0 && v.best_bid == nbb) bid_sz += v.bid_size;
            if (v.ask_size > 0 && v.best_ask == nbo) ask_sz += v.ask_size;
        }
        const int64_t denom = bid_sz + ask_sz;
        return denom > 0 ? static_cast<double>(bid_sz - ask_sz) / static_cast<double>(denom) : 0.0;
    }
    // nbbo_locked / nbbo_crossed: consolidated market-integrity checks across venues
    // (#270). LOCKED = national best bid == national best ask (spread zero) — a
    // Reg NMS locked market, usually a stale quote or a maker about to be hit.
    // CROSSED = NBB > NBO — a cross-venue arbitrage (buy the ask cheaper than you
    // sell the bid), in practice a feed inconsistency / lagging venue. Both false
    // without two-sided liquidity.
    bool nbbo_locked() const noexcept {
        const double b = national_best_bid(), a = national_best_ask();
        return b > 0.0 && a > 0.0 && b == a;
    }
    bool nbbo_crossed() const noexcept {
        const double b = national_best_bid(), a = national_best_ask();
        return b > 0.0 && a > 0.0 && b > a;
    }
    // internally_crossed_count: active venues posting a crossed quote on their OWN
    // book — best_bid > best_ask on the same venue (#302). A single venue should
    // never be self-crossed, so this is a hard data-quality red flag (stale/garbled
    // feed for that venue), distinct from nbbo_crossed (#270) which is a cross-venue
    // NBBO condition. Only counts venues quoting both sides.
    int internally_crossed_count() const noexcept {
        int c = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (v.is_active && v.best_bid > 0.0 && v.best_ask > 0.0 && v.best_bid > v.best_ask) ++c;
        }
        return c;
    }

    // venues_at_nbbo: how many ACTIVE venues quote exactly AT the national best on
    // a side (#367) — bid_side=true counts venues whose best_bid == NBB, false
    // whose best_ask == NBO. Measures how contested the touch is: 1 venue at the
    // NBBO means a fragile top (that venue pulling or getting hit collapses the
    // national best), several means a robust, well-supported quote. Distinct from
    // liquidity_venue_count (#359, venues quoting the side at ANY price) — this
    // counts only those setting the best price. 0 when that side has no liquidity.
    int venues_at_nbbo(bool bid_side) const noexcept {
        const double best = bid_side ? national_best_bid() : national_best_ask();
        if (best <= 0.0) return 0;
        int c = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (bid_side) { if (v.bid_size > 0 && v.best_bid == best) ++c; }
            else          { if (v.ask_size > 0 && v.best_ask == best) ++c; }
        }
        return c;
    }

    // nbbo_depth (#480, MILESTONE 480): the total displayed size available
    // at EXACTLY the national best price on the side an order would take —
    // a BUY hits the asks, so this sums ask_size across active venues
    // quoting the National Best Offer; a SELL sums bid_size at the National
    // Best Bid. The SIZE companion to venues_at_nbbo (#367, the venue
    // count): how much you can take at the very best price before routing
    // spills to a worse venue. Distinct from available_liquidity (#109),
    // which sums EVERY venue's top-of-book regardless of price — in a
    // fragmented market that overstates what is really at the touch. 0
    // without liquidity on the side. (nbbo_depth(is_buy=true) is the ask
    // side = venues_at_nbbo(bid_side=false).)
    int64_t nbbo_depth(bool is_buy) const noexcept {
        const double best = is_buy ? national_best_ask() : national_best_bid();
        if (best <= 0.0) return 0;
        int64_t sz = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (is_buy) { if (v.ask_size > 0 && v.best_ask == best) sz += v.ask_size; }
            else        { if (v.bid_size > 0 && v.best_bid == best) sz += v.bid_size; }
        }
        return sz;
    }

    // touch_concentration (#488): the fraction of a side's total displayed
    // liquidity that sits AT the national best price = nbbo_depth (#480) /
    // available_liquidity (#109), in (0,1]. Near 1 the market is
    // consolidated — nearly every quoting venue is at the touch, so a
    // marketable order fills at one price; low means the venues are
    // scattered across prices and an order larger than the touch WALKS
    // through worse venues before it fills. The router analog of itch's
    // depth_concentration (#439, book shape as a mass fraction), measured
    // across venues instead of price levels. 0 when the side has no
    // liquidity.
    double touch_concentration(bool is_buy) const noexcept {
        const int32_t avail = available_liquidity(is_buy);
        return avail > 0
            ? static_cast<double>(nbbo_depth(is_buy)) / static_cast<double>(avail)
            : 0.0;
    }

    // effective_spread_bps: the REAL cost of crossing the spread WITH FEES (#240) =
    // best all-in ask (quote+fee) - best all-in bid (quote-fee), in bps. Larger than
    // nbbo_spread_bps (#208, quote only) by the round-trip fees: it shows how much it really
    // costs to enter+exit at taker prices. 0 without two-sided liquidity.
    double effective_spread_bps() const noexcept {
        const double eff_ask = best_effective_price(true);    // all-in buy
        const double eff_bid = best_effective_price(false);   // all-in sell
        if (eff_ask <= 0.0 || eff_bid <= 0.0) return 0.0;
        const double m = (eff_ask + eff_bid) / 2.0;
        return m > 0.0 ? (eff_ask - eff_bid) / m * 10000.0 : 0.0;
    }
    // active_venue_count: how many venues are active (after health/manual) (#154).
    int active_venue_count() const noexcept {
        int n = 0;
        for (int i = 0; i < venue_count_; ++i) if (venues_[i].is_active) ++n;
        return n;
    }
    // liquidity_venue_count: how many ACTIVE venues are actually QUOTING a
    // positive size on the given side right now (#359). Differs from
    // active_venue_count (#154, active regardless of whether they quote): the
    // gap between the two is how many active venues are dark / not quoting that
    // side. A breadth-of-execution measure — with only one quoting venue there
    // is no cross-venue competition and a route can't diversify market impact.
    int liquidity_venue_count(bool is_buy) const noexcept {
        int n = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (is_buy) { if (v.best_ask > 0 && v.ask_size > 0) ++n; }
            else        { if (v.best_bid > 0 && v.bid_size > 0) ++n; }
        }
        return n;
    }
    // best_effective_price: the best ALL-IN price (quote ± fee) available now on a
    // given side, without routing (#154). BUY: minimum; SELL: maximum.
    // 0 when there is no liquidity. Inspection of "what would I pay" before deciding.
    double best_effective_price(bool is_buy) const noexcept {
        double best = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const double eff = effective_price(v, is_buy);
            if (!found || (is_buy ? (eff < best) : (eff > best))) { best = eff; found = true; }
        }
        return found ? best : 0.0;
    }

    // effective_price_dispersion: the spread of ALL-IN prices (quote +/- fee)
    // across active venues quoting a side (#464) = max_eff - min_eff, always
    // >= 0. The value of smart routing in one number: zero means every venue
    // offers the same all-in price so routing is a coin flip, a wide spread
    // means picking the right venue saves real money per share (and the best
    // #155 vs worst gap is exactly this). Fees are in it, so a venue with a
    // great quote but a punishing taker fee widens the dispersion correctly.
    // 0 with fewer than two venues quoting the side.
    double effective_price_dispersion(bool is_buy) const noexcept {
        double lo = 0.0, hi = 0.0;
        int found = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const double eff = effective_price(v, is_buy);
            if (found == 0 || eff < lo) lo = eff;
            if (found == 0 || eff > hi) hi = eff;
            ++found;
        }
        return found >= 2 ? hi - lo : 0.0;
    }

    // is_marketable: could a limit order execute IMMEDIATELY (#184) — there is
    // an active venue whose all-in price (quote ± fee) is on the right
    // side of the limit. BUY: best all-in ask <= limit; SELL: best all-in
    // bid >= limit. false when there is no liquidity. A pre-route guard for limit orders:
    // non-marketable -> rest in the book instead of routing into the void.
    bool is_marketable(bool is_buy, double limit_price) const noexcept {
        const double best = best_effective_price(is_buy);
        if (best <= 0.0) return false;
        return is_buy ? (best <= limit_price) : (best >= limit_price);
    }

    // liquidity_at_limit: how many shares a limit order could take RIGHT NOW
    // (#376) — the sum of top-of-book size over active venues whose all-in
    // price (quote ± fee, the same convention as is_marketable #184)
    // satisfies the limit. BUY: all-in ask <= limit; SELL: all-in bid >=
    // limit. Quantifies is_marketable's yes/no into an immediate-fill cap:
    // marketable ⇔ liquidity_at_limit > 0, and anything beyond the returned
    // size must rest or walk to prices worse than the limit. Distinct from
    // available_liquidity (#109), which sums the same sizes but ignores
    // price entirely.
    int32_t liquidity_at_limit(bool is_buy, double limit_price) const noexcept {
        int32_t total = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const double eff = effective_price(v, is_buy);
            if (is_buy ? (eff <= limit_price) : (eff >= limit_price)) {
                total += is_buy ? v.ask_size : v.bid_size;
            }
        }
        return total;
    }

    // venue_effective_price: the all-in price (quote +/- fee) for a SPECIFIC venue by
    // name (#248). Inspection / pricing of a directed order to a
    // specified exchange, independent of best-price. 0 when unknown, inactive or
    // no liquidity on a given side. Complements best_effective_price (scan of all).
    double venue_effective_price(const char* venue_name, bool is_buy) const noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (std::strcmp(v.name, venue_name) != 0) continue;
            if (!v.is_active) return 0.0;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            return has_liq ? effective_price(v, is_buy) : 0.0;
        }
        return 0.0;
    }

    // cheapest_venue: the NAME of the venue with the best all-in price (quote +/- fee) on a
    // given side (#200). Complements best_effective_price (the price itself) — here you know
    // WHERE. BUY: min all-in ask; SELL: max all-in bid. nullptr when there is no liquidity.
    // Inspection/logging of a routing decision without executing route_order.
    const char* cheapest_venue(bool is_buy) const noexcept {
        const char* best_name = nullptr;
        double best = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const double eff = effective_price(v, is_buy);
            if (!found || (is_buy ? (eff < best) : (eff > best))) {
                best = eff; best_name = v.name; found = true;
            }
        }
        return found ? best_name : nullptr;
    }

    // rebate_venue_count: how many ACTIVE venues pay a maker rebate
    // (fee_per_share < 0) (#504) — the count of places that would PAY you
    // to add liquidity. Zero means an all-taker landscape where posting
    // never earns; a high count means the router has real rebate options
    // to steer passive flow to. The breadth companion to cheapest_fee_venue
    // (#496, the single best), same fee family (set_venue_fee #176,
    // avg_fee_per_share #232).
    int rebate_venue_count() const noexcept {
        int n = 0;
        for (int i = 0; i < venue_count_; ++i)
            if (venues_[i].is_active && venues_[i].fee_per_share < 0.0) ++n;
        return n;
    }

    // avg_venue_fee: the mean NOMINAL fee_per_share across active venues
    // (#504) — the current fee LANDSCAPE (positive = the average venue
    // charges to take, negative = the average pays to make), distinct from
    // avg_fee_per_share (#232, the REALIZED fee weighted by what actually
    // routed). A rising nominal average with steady routed fees means fees
    // are climbing on venues the router has learned to avoid. 0 when no
    // venue is active.
    double avg_venue_fee() const noexcept {
        double sum = 0.0; int n = 0;
        for (int i = 0; i < venue_count_; ++i)
            if (venues_[i].is_active) { sum += venues_[i].fee_per_share; ++n; }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    }

    // cheapest_fee_venue: the NAME of the active venue with the lowest
    // fee_per_share (#496) — the most favorable fee tier, a negative value
    // being the best maker rebate. Writes the fee into out_fee. Where
    // cheapest_venue (#200) picks by the all-in PRICE (quote + fee) for a
    // taker crossing now, this picks by the FEE ALONE — the venue to POST
    // passive liquidity on to minimize cost or collect the biggest rebate,
    // independent of the current quote. Pairs with the fee family
    // (set_venue_fee #176, avg_fee_per_share #232). nullptr (out_fee
    // untouched) when no venue is active.
    const char* cheapest_fee_venue(double& out_fee) const noexcept {
        const char* best_name = nullptr;
        double best = 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (best_name == nullptr || v.fee_per_share < best) {
                best = v.fee_per_share; best_name = v.name;
            }
        }
        if (best_name != nullptr) out_fee = best;
        return best_name;
    }

    // most_expensive_venue: the NAME of the venue with the WORST all-in
    // price on a side (#472) — the mirror of cheapest_venue (#200) and the
    // actionable WHICH behind effective_price_dispersion (#464): dispersion
    // is the magnitude of the cross-venue price gap, this names the venue
    // at the bad end of it (a punishing-fee venue a naive router might hit,
    // or a blacklist candidate). BUY: max all-in ask; SELL: min all-in bid.
    // nullptr when no venue quotes the side. Fees are in the comparison, so
    // a good raw quote wrecked by a taker fee is correctly named worst.
    const char* most_expensive_venue(bool is_buy) const noexcept {
        const char* worst_name = nullptr;
        double worst = 0.0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const double eff = effective_price(v, is_buy);
            if (!found || (is_buy ? (eff > worst) : (eff < worst))) {
                worst = eff; worst_name = v.name; found = true;
            }
        }
        return found ? worst_name : nullptr;
    }

    // deepest_venue: name of the venue with the LARGEST displayed size on a side
    // (#255). BUY looks at ask_size, SELL at bid_size; only active venues with a
    // positive quote. Complements cheapest_venue (#200, best price): a large order
    // that prioritizes fill size over price routes to the deepest book. nullptr
    // when no venue has liquidity.
    const char* deepest_venue(bool is_buy) const noexcept {
        const char* best_name = nullptr;
        int32_t best = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            if (!has_liq) continue;
            const int32_t size = is_buy ? v.ask_size : v.bid_size;
            if (size > best) { best = size; best_name = v.name; }
        }
        return best_name;
    }

    // fastest_venue: name of the active venue with the lowest selection latency
    // (#278) — EWMA of observed round-trips if available, else the configured p99 /
    // static latency (selection_latency_ns). For latency-sensitive routing it
    // completes the "best-by-X" family: cheapest_venue (price), deepest_venue
    // (size), fastest_venue (speed). nullptr when no venue is active.
    const char* fastest_venue() const noexcept {
        const char* best_name = nullptr;
        int64_t best = 0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            const int64_t lat = v.selection_latency_ns();
            if (!found || lat < best) { best = lat; best_name = v.name; found = true; }
        }
        return best_name;
    }

    // avg_venue_latency_ns: mean selection latency across all ACTIVE venues (#286).
    // Whereas fastest_venue gives the single best path, this is the market-wide
    // connectivity health — a rising average warns that the whole fabric (not just
    // one venue) is degrading. 0 when no venue is active.
    double avg_venue_latency_ns() const noexcept {
        int64_t sum = 0; int n = 0;
        for (int i = 0; i < venue_count_; ++i) {
            if (!venues_[i].is_active) continue;
            sum += venues_[i].selection_latency_ns();
            ++n;
        }
        return n > 0 ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    }

    // venue_latency_spread_ns: max - min selection latency across ACTIVE venues
    // (#351). Complements avg_venue_latency_ns (the mean): a wide spread means
    // some venues are far slower than others even while the average looks fine
    // — worth pinning latency-sensitive strategies away from the laggards
    // instead of averaging across the whole fabric. 0 when fewer than one
    // active venue (nothing to spread).
    int64_t venue_latency_spread_ns() const noexcept {
        int64_t lo = 0, hi = 0; bool found = false;
        for (int i = 0; i < venue_count_; ++i) {
            if (!venues_[i].is_active) continue;
            const int64_t lat = venues_[i].selection_latency_ns();
            if (!found) { lo = hi = lat; found = true; }
            else        { if (lat < lo) lo = lat; if (lat > hi) hi = lat; }
        }
        return found ? (hi - lo) : 0;
    }

    // available_liquidity: total displayed top-of-book size on the order's side
    // (is_buy → asks, sell → bids), only active venues with a positive quote.
    // Pre-route sizing: whether there is any liquidity to cover the order (#109).
    int32_t available_liquidity(bool is_buy) const noexcept {
        int32_t total = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (is_buy) { if (v.best_ask > 0 && v.ask_size > 0) total += v.ask_size; }
            else        { if (v.best_bid > 0 && v.bid_size > 0) total += v.bid_size; }
        }
        return total;
    }
    // fresh_available_liquidity: available_liquidity (#109) counting only
    // venues whose quote is at most max_age_ns old at at_ns (#432) — the
    // SIZE companion to fresh_nbbo_mid's (#424) price. Stale displayed size
    // is the worst kind of liquidity: it looks sweepable, the order routes
    // to it, and the market that posted it left long ago (a fade/reject in
    // practice). The gap between #109 and this number is the phantom-size
    // share of the touch. Never-quoted venues are excluded (they show no
    // size anyway); same clock injection as the #392 family.
    int32_t fresh_available_liquidity(bool is_buy, int64_t max_age_ns,
                                      int64_t at_ns) const noexcept {
        int32_t total = 0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active || v.last_quote_ns == 0) continue;
            if (at_ns - v.last_quote_ns > max_age_ns) continue;   // stale: phantom size
            if (is_buy) { if (v.best_ask > 0 && v.ask_size > 0) total += v.ask_size; }
            else        { if (v.best_bid > 0 && v.bid_size > 0) total += v.bid_size; }
        }
        return total;
    }
    // available_liquidity_notional: total $ value of top-of-book liquidity across
    // active venues on the order side (#310) — buy hits asks (sum best_ask*ask_size),
    // sell hits bids (sum best_bid*bid_size). The $ companion to available_liquidity
    // (#109, shares): how much capital sits at the touch you can sweep immediately,
    // a more comparable size gate across symbols of different prices.
    double available_liquidity_notional(bool is_buy) const noexcept {
        double total = 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (!v.is_active) continue;
            if (is_buy) { if (v.best_ask > 0 && v.ask_size > 0) total += v.best_ask * v.ask_size; }
            else        { if (v.best_bid > 0 && v.bid_size > 0) total += v.best_bid * v.bid_size; }
        }
        return total;
    }
    // fill_shortfall: how many shares of the order the displayed liquidity will NOT cover (#224) =
    // max(0, shares - available_liquidity). The rest would have to rest in the book /
    // wait for a new quote. Pre-route sizing.
    int32_t fill_shortfall(bool is_buy, int32_t shares) const noexcept {
        const int32_t avail = available_liquidity(is_buy);
        return shares > avail ? shares - avail : 0;
    }
    // fillable_ratio: what fraction of the order fills immediately per displayed (#224),
    // 0..1. min(avail, shares) / shares. 0 for shares <= 0.
    double fillable_ratio(bool is_buy, int32_t shares) const noexcept {
        if (shares <= 0) return 0.0;
        const int32_t avail = available_liquidity(is_buy);
        const int32_t f = avail < shares ? avail : shares;
        return static_cast<double>(f) / static_cast<double>(shares);
    }
    // sweep_to_fill: the realized fill of a price-prioritized multi-venue sweep —
    // the cross-venue analog of walking a single book. Consumes each active
    // venue's displayed top-of-book size from the best price outward (BUY takes
    // the cheapest asks first, SELL hits the highest bids first) up to `shares`.
    // Returns shares filled (<= shares; < shares when consolidated depth is thin)
    // and writes the volume-weighted average sweep price into out_vwap (0 on no
    // fill). Unlike best_effective_price (#155, the single best touch) this blends
    // the price across the MULTIPLE venues a large order must hit — the real
    // expected cost of a sweep and the basis for sweep slippage. Raw quotes (no
    // fees), matching the NBBO convention; effective_spread_bps covers the fee view.
    int32_t sweep_to_fill(bool is_buy, int32_t shares, double& out_vwap) const noexcept {
        out_vwap = 0.0;
        if (shares <= 0) return 0;
        bool    used[MAX_VENUES] = {};   // value-init: cppcheck-clean, no heap
        int32_t remaining = shares, filled = 0;
        double  notional = 0.0;
        // venue_count_ <= MAX_VENUES and small; pick the best remaining venue
        // each pass (price priority), at most venue_count_ passes.
        for (int pass = 0; pass < venue_count_ && remaining > 0; ++pass) {
            int    best_i  = -1;
            double best_px = 0.0;
            for (int i = 0; i < venue_count_; ++i) {
                if (used[i]) continue;
                const Venue& v = venues_[i];
                if (!v.is_active) continue;
                const double  px = is_buy ? v.best_ask : v.best_bid;
                const int32_t sz = is_buy ? v.ask_size : v.bid_size;
                if (px <= 0.0 || sz <= 0) continue;
                const bool better = (best_i < 0) ||
                                    (is_buy ? px < best_px : px > best_px);
                if (better) { best_i = i; best_px = px; }
            }
            if (best_i < 0) break;                       // no more liquidity
            used[best_i] = true;
            const Venue&  v    = venues_[best_i];
            const int32_t sz   = is_buy ? v.ask_size : v.bid_size;
            const int32_t take = remaining < sz ? remaining : sz;
            notional  += best_px * take;
            filled    += take;
            remaining -= take;
        }
        if (filled > 0) out_vwap = notional / static_cast<double>(filled);
        return filled;
    }
    // sweep_slippage_bps: the expected cost of a `shares` sweep vs the NBBO mid,
    // in basis points and always POSITIVE (a cost) — a BUY sweeps UP through the
    // asks (vwap > mid), a SELL DOWN through the bids (vwap < mid). Builds on
    // sweep_to_fill + nbbo_mid. 0 when nothing fills or there is no two-sided
    // NBBO. The cross-venue analog of itch slippage_bps (#199).
    double sweep_slippage_bps(bool is_buy, int32_t shares) const noexcept {
        double vwap = 0.0;
        const int32_t filled = sweep_to_fill(is_buy, shares, vwap);
        const double  m = nbbo_mid();
        if (filled <= 0 || vwap <= 0.0 || m <= 0.0) return 0.0;
        const double diff = is_buy ? (vwap - m) : (m - vwap);
        return diff / m * 10000.0;
    }
    // sweep_to_fill_fresh (#440, MILESTONE 440): sweep_to_fill (#335) with
    // the #392 staleness gate — only venues whose quote is at most
    // max_age_ns old participate. The planner-tier member of the fresh
    // family (fresh_nbbo_mid #424 = price, fresh_available_liquidity #432
    // = size, this = COST): a sweep planned over stale quotes books fills
    // that fade on arrival, so the plan itself must drop them. Same
    // raw-quote VWAP convention as #335 (NBBO terms; the at-limit family
    // #400 is the all-in view). Returns the fillable shares from FRESH
    // venues and writes their VWAP into out_vwap (0 on no fill); the gap
    // vs sweep_to_fill is the phantom part of the plan.
    int32_t sweep_to_fill_fresh(bool is_buy, int32_t shares,
                                int64_t max_age_ns, int64_t at_ns,
                                double& out_vwap) const noexcept {
        out_vwap = 0.0;
        if (shares <= 0) return 0;
        bool    used[MAX_VENUES] = {};   // value-init: cppcheck-clean, no heap
        int32_t remaining = shares, filled = 0;
        double  notional = 0.0;
        for (int pass = 0; pass < venue_count_ && remaining > 0; ++pass) {
            int    best_i  = -1;
            double best_px = 0.0;
            for (int i = 0; i < venue_count_; ++i) {
                if (used[i]) continue;
                const Venue& v = venues_[i];
                if (!v.is_active || v.last_quote_ns == 0) continue;
                if (at_ns - v.last_quote_ns > max_age_ns) continue;   // stale: not in the plan
                const double  px = is_buy ? v.best_ask : v.best_bid;
                const int32_t sz = is_buy ? v.ask_size : v.bid_size;
                if (px <= 0.0 || sz <= 0) continue;
                const bool better = (best_i < 0) ||
                                    (is_buy ? px < best_px : px > best_px);
                if (better) { best_i = i; best_px = px; }
            }
            if (best_i < 0) break;                       // no fresh liquidity left
            used[best_i] = true;
            const Venue&  v    = venues_[best_i];
            const int32_t sz   = is_buy ? v.ask_size : v.bid_size;
            const int32_t take = remaining < sz ? remaining : sz;
            notional  += best_px * take;
            filled    += take;
            remaining -= take;
        }
        if (filled > 0) out_vwap = notional / static_cast<double>(filled);
        return filled;
    }

    // sweep_to_fill_at_limit (#400, MILESTONE 400): the marketable-limit
    // sweep planner — how many of `shares` fill RIGHT NOW without paying
    // worse than `limit_price`, and at what blended cost. Walks venues in
    // price priority like sweep_to_fill (#335) but admits only venues whose
    // ALL-IN price (quote ± fee — the is_marketable #184 /
    // liquidity_at_limit #376 convention) satisfies the limit, and both the
    // gate and out_vwap use that all-in price: a limit is a cap on what you
    // are willing to PAY, fees included (sweep_to_fill's raw-quote VWAP
    // matches the NBBO convention instead). Returns the fillable shares and
    // writes the all-in VWAP into out_vwap (0 on no fill). Given enough
    // shares the fill equals liquidity_at_limit(is_buy, limit_price); the
    // remainder past the return value must rest at the limit.
    int32_t sweep_to_fill_at_limit(bool is_buy, int32_t shares, double limit_price,
                                   double& out_vwap) const noexcept {
        out_vwap = 0.0;
        if (shares <= 0) return 0;
        bool    used[MAX_VENUES] = {};   // value-init: cppcheck-clean, no heap
        int32_t remaining = shares, filled = 0;
        double  notional = 0.0;
        for (int pass = 0; pass < venue_count_ && remaining > 0; ++pass) {
            int    best_i   = -1;
            double best_eff = 0.0;
            for (int i = 0; i < venue_count_; ++i) {
                if (used[i]) continue;
                const Venue& v = venues_[i];
                if (!v.is_active) continue;
                const double  px = is_buy ? v.best_ask : v.best_bid;
                const int32_t sz = is_buy ? v.ask_size : v.bid_size;
                if (px <= 0.0 || sz <= 0) continue;
                const double eff = effective_price(v, is_buy);
                if (is_buy ? (eff > limit_price) : (eff < limit_price)) continue;  // past the limit
                const bool better = (best_i < 0) ||
                                    (is_buy ? eff < best_eff : eff > best_eff);
                if (better) { best_i = i; best_eff = eff; }
            }
            if (best_i < 0) break;                       // nothing left within the limit
            used[best_i] = true;
            const Venue&  v    = venues_[best_i];
            const int32_t sz   = is_buy ? v.ask_size : v.bid_size;
            const int32_t take = remaining < sz ? remaining : sz;
            notional  += best_eff * take;
            filled    += take;
            remaining -= take;
        }
        if (filled > 0) out_vwap = notional / static_cast<double>(filled);
        return filled;
    }
    // venues_to_fill_at_limit (#408): the NUMBER of distinct venues the
    // limit-capped sweep must touch to fill `shares` — the venue-count face
    // of sweep_to_fill_at_limit (#400), exactly as venues_to_fill (#343) is
    // to sweep_to_fill (#335). Each touched venue is an extra order, fee
    // schedule and information leak; a marketable-limit that needs 5 venues
    // is a different execution problem than one the touch absorbs. Same
    // all-in limit gate as #400/#376. Returns -1 when the liquidity within
    // the limit cannot cover `shares` (the remainder would have to rest),
    // 0 for a non-positive size.
    int32_t venues_to_fill_at_limit(bool is_buy, int32_t shares,
                                    double limit_price) const noexcept {
        if (shares <= 0) return 0;
        bool    used[MAX_VENUES] = {};   // value-init: cppcheck-clean, no heap
        int32_t remaining = shares;
        int32_t touched   = 0;
        for (int pass = 0; pass < venue_count_ && remaining > 0; ++pass) {
            int    best_i   = -1;
            double best_eff = 0.0;
            for (int i = 0; i < venue_count_; ++i) {
                if (used[i]) continue;
                const Venue& v = venues_[i];
                if (!v.is_active) continue;
                const double  px = is_buy ? v.best_ask : v.best_bid;
                const int32_t sz = is_buy ? v.ask_size : v.bid_size;
                if (px <= 0.0 || sz <= 0) continue;
                const double eff = effective_price(v, is_buy);
                if (is_buy ? (eff > limit_price) : (eff < limit_price)) continue;
                const bool better = (best_i < 0) ||
                                    (is_buy ? eff < best_eff : eff > best_eff);
                if (better) { best_i = i; best_eff = eff; }
            }
            if (best_i < 0) break;                       // nothing left within the limit
            used[best_i] = true;
            const Venue&  v  = venues_[best_i];
            const int32_t sz = is_buy ? v.ask_size : v.bid_size;
            remaining -= remaining < sz ? remaining : sz;
            ++touched;
        }
        return remaining > 0 ? -1 : touched;
    }

    // venues_to_fill: the NUMBER of distinct venues that must be swept (in
    // price-priority order) to fill `shares` (#343). Same walk as sweep_to_fill,
    // but counts venues touched instead of computing a VWAP — the cross-venue
    // analog of itch's levels_to_fill (#342). A large order needing many venues
    // telegraphs intent more than one filled at a single venue — useful for
    // gauging market-impact/signaling risk before routing. -1 when the combined
    // displayed liquidity across all active venues can't cover the requested size.
    int32_t venues_to_fill(bool is_buy, int32_t shares) const noexcept {
        if (shares <= 0) return 0;
        bool    used[MAX_VENUES] = {};   // value-init: cppcheck-clean, no heap
        int32_t remaining = shares;
        int32_t venues_touched = 0;
        for (int pass = 0; pass < venue_count_ && remaining > 0; ++pass) {
            int    best_i  = -1;
            double best_px = 0.0;
            for (int i = 0; i < venue_count_; ++i) {
                if (used[i]) continue;
                const Venue& v = venues_[i];
                if (!v.is_active) continue;
                const double  px = is_buy ? v.best_ask : v.best_bid;
                const int32_t sz = is_buy ? v.ask_size : v.bid_size;
                if (px <= 0.0 || sz <= 0) continue;
                const bool better = (best_i < 0) ||
                                    (is_buy ? px < best_px : px > best_px);
                if (better) { best_i = i; best_px = px; }
            }
            if (best_i < 0) break;                       // no more liquidity
            used[best_i] = true;
            ++venues_touched;
            const Venue&  v  = venues_[best_i];
            const int32_t sz = is_buy ? v.ask_size : v.bid_size;
            remaining -= (remaining < sz ? remaining : sz);
        }
        return remaining <= 0 ? venues_touched : -1;
    }
    // venue_liquidity_share: a named venue's share of the CURRENT displayed
    // liquidity on a side (#262), as a percent. Unlike venue_share_pct (#216,
    // historical routed volume) this is the live quote concentration right now —
    // a high share means the consolidated book leans on one venue's quote.
    // 0 for unknown / inactive / no liquidity.
    double venue_liquidity_share(const char* venue_name, bool is_buy) const noexcept {
        const int32_t total = available_liquidity(is_buy);
        if (total <= 0) return 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const Venue& v = venues_[i];
            if (std::strcmp(v.name, venue_name) != 0) continue;
            if (!v.is_active) return 0.0;
            const bool has_liq = is_buy ? (v.best_ask > 0 && v.ask_size > 0)
                                        : (v.best_bid > 0 && v.bid_size > 0);
            const int32_t size = has_liq ? (is_buy ? v.ask_size : v.bid_size) : 0;
            return static_cast<double>(size) / static_cast<double>(total) * 100.0;
        }
        return 0.0;
    }

    int venue_count() const noexcept { return venue_count_; }
    // venue_routed_shares: total routed volume on a given venue (TCA, #117).
    int64_t venue_routed_shares(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0) return venues_[i].routed_shares;
        return 0;
    }
    // venue_route_count: how many times an order LANDED on a venue (#456) —
    // the count face of venue_routed_shares' volume. A SPLIT bumps this on
    // every leg it touches, so a venue can carry a high count of small
    // clips or a low count of blocks. -1 for an unknown venue (0 is a real
    // "quoted but never chosen").
    int64_t venue_route_count(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0)
                return static_cast<int64_t>(venues_[i].routes_count);
        return -1;
    }
    // avg_route_size: mean shares per routing decision on a venue (#456) =
    // routed_shares / routes_count. venue_share_pct (#216) says how much
    // VOLUME a venue got and routing_concentration (#384) how concentrated
    // the volume is; this says the TYPICAL CLIP — a venue fed many odd lots
    // reads small here even at a large volume share, a block venue reads
    // large. The routing-side analog of OMS avg_fill_size (#274). 0 for an
    // unknown venue or one never routed to.
    double avg_route_size(const char* venue_name) const noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) != 0) continue;
            const Venue& v = venues_[i];
            return v.routes_count > 0
                ? static_cast<double>(v.routed_shares) / static_cast<double>(v.routes_count)
                : 0.0;
        }
        return 0.0;
    }
    // total_routed_shares: total volume routed across all venues (#130).
    int64_t total_routed_shares() const noexcept {
        int64_t s = 0;
        for (int i = 0; i < venue_count_; ++i) s += venues_[i].routed_shares;
        return s;
    }
    // venue_share_pct: what % of the entire routed volume landed on a given venue
    // (#216) — execution concentration for best-ex / TCA reporting. A high share of
    // one venue may require justification. 0 for an unknown venue or at zero
    // volume.
    double venue_share_pct(const char* venue_name) const noexcept {
        const int64_t total = total_routed_shares();
        if (total <= 0) return 0.0;
        return static_cast<double>(venue_routed_shares(venue_name))
             / static_cast<double>(total) * 100.0;
    }
    // routing_concentration: Herfindahl index of routed volume across venues
    // (#384) = Σ (venue_routed_shares / total)², in (0, 1]. 1.0 = every share
    // went to ONE venue (single point of failure, weak best-ex evidence);
    // 1/N = perfectly even across N venues. The single-number concentration
    // summary that venue_share_pct (#216) only gives one venue at a time —
    // same HHI construction as risk's exposure_concentration (#331), applied
    // to order flow instead of positions. 0.0 before anything is routed.
    double routing_concentration() const noexcept {
        const int64_t total = total_routed_shares();
        if (total <= 0) return 0.0;
        double hhi = 0.0;
        for (int i = 0; i < venue_count_; ++i) {
            const double share = static_cast<double>(venues_[i].routed_shares)
                               / static_cast<double>(total);
            hhi += share * share;
        }
        return hhi;
    }
    // reset_routing_stats: zero the per-venue TCA counters (new session/window).
    void reset_routing_stats() noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            venues_[i].routed_shares = 0;
            venues_[i].routes_count  = 0;
        }
    }

    // reset_session_stats: a FULL TCA reset for a new session (#192) — zeroes the
    // global counters (routes/rejects/latency/fees) that reset_routing_stats did NOT
    // touch, plus per-venue. Venues and their quotes stay (no need to add/quote them
    // again). Call once at the open of the session.
    void reset_session_stats() noexcept {
        total_routes_     = 0;
        total_rejected_   = 0;
        total_latency_ns_ = 0;
        total_fees_paid_  = 0.0;
        reset_routing_stats();
    }

    // total_fees_paid: sum of fees/rebates over all routes (#138). Negative =
    // net rebate (maker). The basis for execution-cost analysis.
    double   total_fees_paid()    const noexcept { return total_fees_paid_; }
    // avg_fee_per_share: average REALIZED fee per share (#232) = total_fees /
    // total_routed_shares. Positive = net taker (you pay for liquidity), negative =
    // net maker (you collect rebate). A key TCA measure of routing quality. 0 when
    // nothing was routed.
    double   avg_fee_per_share() const noexcept {
        const int64_t total = total_routed_shares();
        return total > 0 ? total_fees_paid_ / static_cast<double>(total) : 0.0;
    }

    // set_venue_fee: runtime swap of a venue's tariff (#176). Fee schedules
    // change (volume tier, maker/taker promotion); all-in routing
    // (quote ± fee) immediately reflects the new rate. false for an unknown venue.
    bool set_venue_fee(const char* venue_name, double fee_per_share) noexcept {
        for (int i = 0; i < venue_count_; ++i)
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].fee_per_share = fee_per_share;
                return true;
            }
        return false;
    }

    uint64_t get_total_routes()   const noexcept { return total_routes_; }
    uint64_t get_total_rejected() const noexcept { return total_rejected_; }
    // reject_rate: fraction of routing attempts that ended in rejection (#162) =
    // rejected / (routes + rejected). 0 when there are no attempts.
    double reject_rate() const noexcept {
        const uint64_t total = total_routes_ + total_rejected_;
        return total > 0 ? static_cast<double>(total_rejected_) / static_cast<double>(total) : 0.0;
    }
    // avg_routing_latency_ns: average router DECISION latency (not the venue
    // round-trip) per successful route (#162).
    double avg_routing_latency_ns() const noexcept {
        return total_routes_ > 0 ? static_cast<double>(total_latency_ns_) / static_cast<double>(total_routes_) : 0.0;
    }

    void print_stats() const {
        printf("\n=== Router Statistics ===\n");
        printf("  Total routes: %lu\n", (unsigned long)total_routes_);
        printf("  Rejected: %lu\n", (unsigned long)total_rejected_);
        const double avg = total_routes_ > 0
            ? static_cast<double>(total_latency_ns_) / total_routes_ : 0.0;
        printf("  Avg routing latency: %.0f ns\n", avg);
    }

private:
    // split_order: split a large order. Sorts venues by EFFECTIVE price
    // (not raw!) and fills from the cheapest until the order or
    // liquidity is exhausted. Reports the average price, total fee and number of venues used.
    RouteDecision split_order(Venue** candidates, int num, bool is_buy,
                               int32_t quantity, int64_t t0) noexcept {
        // Selection sort by effective price (small N → O(N²) doesn't matter).
        for (int i = 0; i < num - 1; ++i) {
            for (int j = i + 1; j < num; ++j) {
                const double a = effective_price(*candidates[i], is_buy);
                const double b = effective_price(*candidates[j], is_buy);
                const bool   swap = is_buy ? (b < a) : (b > a);  // best first
                if (swap) std::swap(candidates[i], candidates[j]);
            }
        }

        int32_t remaining   = quantity;
        int32_t filled      = 0;
        int32_t venues_used = 0;
        double  price_sum   = 0.0;   // Σ quote × alloc (for the average price)
        double  fee_sum     = 0.0;   // Σ fee × alloc   (total fee)

        for (int i = 0; i < num && remaining > 0; ++i) {
            const int32_t available = is_buy ? candidates[i]->ask_size
                                             : candidates[i]->bid_size;
            const int32_t alloc = std::min(remaining, available);
            if (alloc <= 0) continue;
            const double px = is_buy ? candidates[i]->best_ask : candidates[i]->best_bid;
            price_sum += px * alloc;
            fee_sum   += candidates[i]->fee_per_share * alloc;
            filled    += alloc;
            remaining -= alloc;
            candidates[i]->routed_shares += alloc;   // TCA per venue (#117)
            ++candidates[i]->routes_count;
            ++venues_used;
        }

        if (filled == 0) {
            ++total_rejected_;
            RouteDecision d;
            d.reject_reason = RouteReject::NO_LIQUIDITY;
            return d;
        }

        RouteDecision d;
        d.valid           = true;
        std::strncpy(d.venue, candidates[0]->name, 15);  // the best-price venue = primary
        d.venue[15]       = '\0';
        d.price           = price_sum / filled;
        // all-in average: for BUY the cost rises by fee, for SELL it falls.
        d.effective_price = is_buy ? (price_sum + fee_sum) / filled
                                   : (price_sum - fee_sum) / filled;
        d.total_fee       = fee_sum;
        d.quantity        = filled;
        d.unfilled_qty    = quantity - filled;   // shortfall when Σliquidity < order
        d.num_venues      = venues_used;
        d.latency_ns      = now_ns() - t0;

        ++total_routes_;
        total_latency_ns_ += d.latency_ns;
        total_fees_paid_  += d.total_fee;   // #138 cumulative cost
        return d;
    }
};
