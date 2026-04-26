/*
 * Smart Order Router (SOR).
 *
 * Pipeline position: Strategy -> [Router] -> Risk -> OMS -> Exchange
 *
 * Strategies:
 *   BEST_PRICE      lowest ask (buy) / highest bid (sell), tie-break on fee
 *   LOWEST_LATENCY  fastest venue round-trip
 *   SPLIT           distribute across venues by available liquidity
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cstdio>

static constexpr int MAX_VENUES = 16;


enum class RoutingStrategy : uint8_t {
    BEST_PRICE     = 0,
    LOWEST_LATENCY = 1,
    SPLIT          = 2
};


struct Venue {
    char    name[16];
    int64_t latency_ns;
    double  fee_per_share;   // negative = rebate
    double  best_bid;
    double  best_ask;
    int32_t bid_size;
    int32_t ask_size;
    bool    is_active;

    Venue() noexcept
        : latency_ns(0), fee_per_share(0), best_bid(0), best_ask(0),
          bid_size(0), ask_size(0), is_active(true) { name[0] = '\0'; }

    Venue(const char* n, int64_t lat, double fee) noexcept
        : latency_ns(lat), fee_per_share(fee), best_bid(0), best_ask(0),
          bid_size(0), ask_size(0), is_active(true) {
        std::strncpy(name, n, 15);
        name[15] = '\0';
    }
};


struct RouteDecision {
    char    venue[16];
    double  price;
    int32_t quantity;
    int64_t latency_ns;
    bool    valid;          // false = no route found

    RouteDecision() noexcept
        : price(0), quantity(0), latency_ns(0), valid(false) { venue[0] = '\0'; }
};


class SmartOrderRouter {
    Venue           venues_[MAX_VENUES];
    int             venue_count_;
    RoutingStrategy default_strategy_;
    int32_t         split_threshold_;

    uint64_t total_routes_;
    uint64_t total_rejected_;
    uint64_t total_latency_ns_;

    static int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
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

    void update_quote(const char* venue_name, double bid, double ask,
                      int32_t bid_size, int32_t ask_size) noexcept {
        for (int i = 0; i < venue_count_; ++i) {
            if (std::strcmp(venues_[i].name, venue_name) == 0) {
                venues_[i].best_bid = bid;
                venues_[i].best_ask = ask;
                venues_[i].bid_size = bid_size;
                venues_[i].ask_size = ask_size;
                return;
            }
        }
    }

    RouteDecision route_order(const char* side, int32_t quantity,
                              RoutingStrategy strat) noexcept {
        const int64_t t0 = now_ns();
        const bool is_buy = (side[0] == 'B');

        Venue* candidates[MAX_VENUES];
        int    num_candidates = 0;

        for (int i = 0; i < venue_count_; ++i) {
            if (!venues_[i].is_active) continue;
            if (is_buy && venues_[i].best_ask > 0 && venues_[i].ask_size > 0) {
                candidates[num_candidates++] = &venues_[i];
            } else if (!is_buy && venues_[i].best_bid > 0 && venues_[i].bid_size > 0) {
                candidates[num_candidates++] = &venues_[i];
            }
        }

        if (num_candidates == 0) {
            total_rejected_++;
            return RouteDecision();
        }

        if (strat == RoutingStrategy::SPLIT && quantity >= split_threshold_) {
            return split_order(candidates, num_candidates, is_buy, quantity, t0);
        }

        Venue* best = candidates[0];
        for (int i = 1; i < num_candidates; ++i) {
            if (strat == RoutingStrategy::LOWEST_LATENCY) {
                if (candidates[i]->latency_ns < best->latency_ns) best = candidates[i];
            } else {
                // BEST_PRICE (and fallback): tie-break on fee.
                if (is_buy) {
                    if (candidates[i]->best_ask < best->best_ask ||
                        (candidates[i]->best_ask == best->best_ask &&
                         candidates[i]->fee_per_share < best->fee_per_share)) {
                        best = candidates[i];
                    }
                } else {
                    if (candidates[i]->best_bid > best->best_bid ||
                        (candidates[i]->best_bid == best->best_bid &&
                         candidates[i]->fee_per_share < best->fee_per_share)) {
                        best = candidates[i];
                    }
                }
            }
        }

        RouteDecision decision;
        decision.valid = true;
        std::strncpy(decision.venue, best->name, 15);
        decision.venue[15] = '\0';
        decision.price      = is_buy ? best->best_ask : best->best_bid;
        decision.quantity   = quantity;
        decision.latency_ns = now_ns() - t0;

        total_routes_++;
        total_latency_ns_ += decision.latency_ns;
        return decision;
    }

    RouteDecision route_order(const char* side, int32_t quantity) noexcept {
        return route_order(side, quantity, default_strategy_);
    }

    uint64_t get_total_routes()   const noexcept { return total_routes_; }
    uint64_t get_total_rejected() const noexcept { return total_rejected_; }

    void print_stats() const {
        printf("\n=== Router Statistics ===\n");
        printf("  Total routes: %lu\n", (unsigned long)total_routes_);
        printf("  Rejected: %lu\n", (unsigned long)total_rejected_);
        const double avg = total_routes_ > 0
            ? static_cast<double>(total_latency_ns_) / total_routes_ : 0.0;
        printf("  Avg routing latency: %.0f ns\n", avg);
    }

private:
    RouteDecision split_order(Venue** candidates, int num, bool is_buy,
                              int32_t quantity, int64_t t0) noexcept {
        // Selection sort by price (N is tiny, branch-predictable).
        for (int i = 0; i < num - 1; ++i) {
            for (int j = i + 1; j < num; ++j) {
                const bool swap = is_buy
                    ? candidates[j]->best_ask < candidates[i]->best_ask
                    : candidates[j]->best_bid > candidates[i]->best_bid;
                if (swap) std::swap(candidates[i], candidates[j]);
            }
        }

        int32_t  remaining = quantity;
        int32_t  filled    = 0;
        double   price_sum = 0.0;
        const char* primary_venue = candidates[0]->name;

        for (int i = 0; i < num && remaining > 0; ++i) {
            const int32_t available = is_buy ? candidates[i]->ask_size : candidates[i]->bid_size;
            const int32_t alloc     = std::min(remaining, available);
            if (alloc > 0) {
                const double px = is_buy ? candidates[i]->best_ask : candidates[i]->best_bid;
                price_sum += px * alloc;
                filled    += alloc;
                remaining -= alloc;
            }
        }

        if (filled == 0) {
            total_rejected_++;
            return RouteDecision();
        }

        RouteDecision decision;
        decision.valid = true;
        std::strncpy(decision.venue, primary_venue, 15);
        decision.venue[15] = '\0';
        decision.price      = price_sum / filled;
        decision.quantity   = filled;
        decision.latency_ns = now_ns() - t0;

        total_routes_++;
        total_latency_ns_ += decision.latency_ns;
        return decision;
    }
};
