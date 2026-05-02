/*
 * Risk Manager — C++ Implementation
 * Menedżer Ryzyka — implementacja C++
 *
 * The risk manager sits between the strategy and the OMS — every order
 * MUST pass risk checks before it can be sent to the exchange.
 *
 * Menedżer ryzyka siedzi między strategią a OMS — każde zlecenie MUSI
 * przejść kontrole ryzyka zanim może być wysłane na giełdę.
 *
 * WHY C++ HERE:
 * Risk checks are on the critical path. If risk check takes 5μs in Python
 * but only 20ns in C++, that's 4,980ns saved on EVERY order — millions
 * of orders per day means real money saved.
 *
 * DLACZEGO C++ TUTAJ:
 * Kontrole ryzyka są na krytycznej ścieżce. Jeśli kontrola ryzyka trwa 5μs
 * w Pythonie, ale tylko 20ns w C++, to 4,980ns zaoszczędzone na KAŻDYM zleceniu.
 *
 * Performance comparison / Porównanie wydajności:
 *   Python Risk Manager:  ~200K checks/sec
 *   C++ Risk Manager:     ~30-50 million checks/sec  (200x faster)
 *
 * Features / Funkcje:
 *   - Order value limit          / Limit wartości zlecenia
 *   - Per-symbol position limit  / Limit pozycji na symbol
 *   - Portfolio exposure limit   / Limit ekspozycji portfela
 *   - Circuit breaker (daily loss) / Przełącznik obwodu (dzienna strata)
 *   - Kill switch                / Wyłącznik awaryjny
 *   - Drawdown tracking          / Śledzenie spadku
 *   - Rate limiting              / Ograniczanie szybkości
 *
 * Pipeline: Strategy → Router → **Risk Manager** → OMS → Exchange
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <deque>
#include <cstdio>
#include <cmath>

#include "../common/types.hpp"
#include "../common/symbol_key.hpp"
#include "../common/time_utils.hpp"


// === Enums ===

enum class RiskAction : uint8_t {
    ALLOW  = 0,
    REJECT = 1,
    KILL   = 2
};

inline const char* action_str(RiskAction a) noexcept {
    switch (a) {
        case RiskAction::ALLOW:  return "ALLOW";
        case RiskAction::REJECT: return "REJECT";
        case RiskAction::KILL:   return "KILL";
        default:                 return "UNKNOWN";
    }
}


// === RiskCheckResult — what the risk manager returns for each order ===
// === Wynik sprawdzenia ryzyka — co menedżer ryzyka zwraca dla każdego zlecenia ===

struct RiskCheckResult {
    RiskAction action;
    // reason: fixed-size buffer instead of std::string — no heap allocation
    // Like a fixed-length log line — we know risk reasons are short
    // reason: bufor o stałym rozmiarze zamiast std::string — bez alokacji na stercie
    char reason[128];
    int64_t latency_ns;

    RiskCheckResult() noexcept
        : action(RiskAction::ALLOW), latency_ns(0) {
        reason[0] = '\0';
    }

    RiskCheckResult(RiskAction a, const char* r, int64_t lat) noexcept
        : action(a), latency_ns(lat) {
        std::strncpy(reason, r, 127);
        reason[127] = '\0';
    }
};


// === RiskLimits — configurable thresholds ===
// === Limity ryzyka — konfigurowalne progi ===

struct RiskLimits {
    int32_t  max_position_per_symbol;   // max shares in one stock
    int32_t  max_portfolio_exposure;    // max total |shares| across all stocks
    int64_t  max_daily_loss;            // in dollars (triggers circuit breaker)
    int32_t  max_orders_per_second;     // rate limit
    int64_t  max_order_value;           // in dollars (single order cap)
    double   max_drawdown_pct;          // max % drop from peak P&L (0-100)

    RiskLimits() noexcept
        : max_position_per_symbol(5000),
          max_portfolio_exposure(50000),
          max_daily_loss(100000),
          max_orders_per_second(1000),
          max_order_value(500000),
          max_drawdown_pct(5.0) {}
};


// === RiskManager — the main class ===
// === Menedżer Ryzyka — główna klasa ===

class RiskManager {
    RiskLimits limits_;

    // Position tracking: realized (post-fill) and pending (in-flight, signed BUY+/SELL-).
    // Keyed by sym_to_key (packed uint64) — same scheme as OMS, no std::string allocation.
    // Śledzenie pozycji: zrealizowane i pending. Klucz = sym_to_key (packed uint64).
    std::unordered_map<uint64_t, int32_t> positions_;
    std::unordered_map<uint64_t, int32_t> pending_;

    // Denormalized portfolio exposure: sum_s |positions_[s] + pending_[s]|.
    // Maintained as an invariant by every mutator so check_order is O(1).
    int64_t total_abs_exposure_;

    // P&L state
    double daily_pnl_;
    double peak_pnl_;

    // Kill switch
    // Wyłącznik awaryjny
    bool kill_switch_active_;

    // Rate limiting: timestamps of recent orders (nanoseconds).
    // deque so the head-pruning while-loop is O(M) total amortised, not O(M²) like vector::erase.
    std::deque<int64_t> order_timestamps_;

    // Statistics
    uint64_t total_checks_;
    uint64_t total_rejects_;
    uint64_t total_latency_ns_;

public:
    // Constructor — equivalent to Python's __init__
    // Konstruktor — odpowiednik __init__ w Pythonie
    explicit RiskManager(const RiskLimits& limits = RiskLimits()) noexcept
        : limits_(limits),
          total_abs_exposure_(0),
          daily_pnl_(0.0),
          peak_pnl_(0.0),
          kill_switch_active_(false),
          total_checks_(0),
          total_rejects_(0),
          total_latency_ns_(0) {}

    // check_order: run all pre-trade risk checks
    // Returns RiskCheckResult with ALLOW, REJECT, or KILL
    // check_order: uruchom wszystkie przedhandlowe kontrole ryzyka
    // Zwraca RiskCheckResult z ALLOW, REJECT lub KILL
    RiskCheckResult check_order(const char* symbol, Side side,
                                 double price, int32_t quantity) noexcept {
        const int64_t t0 = mono_ns();
        total_checks_++;

        // 1. Kill switch — reject everything
        if (kill_switch_active_) return make_reject("Kill switch active", t0);

        // 2. Order value check
        int64_t order_value = static_cast<int64_t>(price * quantity);
        if (order_value > limits_.max_order_value)
            return make_reject("Order value exceeds limit", t0);

        // 3. Per-symbol position limit (realized + pending + new)
        const uint64_t key       = sym_to_key(symbol);
        const int32_t  signed_new = (side == Side::BUY) ? quantity : -quantity;
        const int32_t  cur_pos   = lookup(positions_, key);
        const int32_t  cur_pend  = lookup(pending_,   key);
        const int32_t  projected = cur_pos + cur_pend + signed_new;
        if (std::abs(projected) > limits_.max_position_per_symbol)
            return make_reject("Position limit exceeded", t0);

        // 4. Portfolio exposure check — O(1) via total_abs_exposure_ invariant.
        const int32_t old_contrib = std::abs(cur_pos + cur_pend);
        const int32_t new_contrib = std::abs(projected);
        if (total_abs_exposure_ - old_contrib + new_contrib > limits_.max_portfolio_exposure)
            return make_reject("Portfolio exposure exceeded", t0);

        // 5. Circuit breaker (daily loss limit)
        // 5. Przełącznik obwodu (dzienny limit strat)
        if (daily_pnl_ < -static_cast<double>(limits_.max_daily_loss)) {
            kill_switch_active_ = true;
            return make_reject("Circuit breaker: daily loss limit", t0);
        }

        // 6. Drawdown check
        // 6. Sprawdzenie spadku
        if (peak_pnl_ > 0.0) {
            double drawdown_pct = (peak_pnl_ - daily_pnl_) / peak_pnl_ * 100.0;
            if (drawdown_pct > limits_.max_drawdown_pct) {
                kill_switch_active_ = true;
                return make_reject("Drawdown limit exceeded", t0);
            }
        }

        // 7. Rate limiting — O(1) amortised: pop expired timestamps from the deque front.
        const int64_t now         = mono_ns();
        const int64_t one_sec_ago = now - 1'000'000'000;
        while (!order_timestamps_.empty() && order_timestamps_.front() <= one_sec_ago)
            order_timestamps_.pop_front();
        if (static_cast<int32_t>(order_timestamps_.size()) >= limits_.max_orders_per_second)
            return make_reject("Rate limit exceeded", t0);
        order_timestamps_.push_back(now);

        const int64_t elapsed = mono_ns() - t0;
        total_latency_ns_ += elapsed;
        return RiskCheckResult(RiskAction::ALLOW, "All checks passed", elapsed);
    }

    // on_order_sent: caller sent an accepted order to the venue — reserve pending capacity
    void on_order_sent(const char* symbol, Side side, int32_t quantity) noexcept {
        adjust_pending(sym_to_key(symbol), side == Side::BUY ? quantity : -quantity);
    }

    // on_order_cancelled: caller cancelled the unfilled remainder — release pending
    void on_order_cancelled(const char* symbol, Side side, int32_t remaining) noexcept {
        adjust_pending(sym_to_key(symbol), side == Side::BUY ? -remaining : remaining);
    }

    // update_position: called after a fill — flow filled qty from pending to realized.
    // pos+pend is unchanged, so total_abs_exposure_ is unchanged.
    void update_position(const char* symbol, Side side, int32_t quantity) noexcept {
        const int32_t signed_q = (side == Side::BUY) ? quantity : -quantity;
        const uint64_t key = sym_to_key(symbol);
        positions_[key] += signed_q;
        pending_[key]   -= signed_q;
    }

    // update_pnl: called after each fill to track daily P&L
    // update_pnl: wywoływane po każdej realizacji do śledzenia dziennego P&L
    void update_pnl(double pnl_change) noexcept {
        daily_pnl_ += pnl_change;
        if (daily_pnl_ > peak_pnl_) {
            peak_pnl_ = daily_pnl_;
        }
    }

    void activate_kill_switch() noexcept { kill_switch_active_ = true; }
    void deactivate_kill_switch() noexcept { kill_switch_active_ = false; }
    bool is_kill_switch_active() const noexcept { return kill_switch_active_; }

    void reset_daily() noexcept {
        daily_pnl_ = 0.0;
        peak_pnl_ = 0.0;
        order_timestamps_.clear();
        pending_.clear();
        kill_switch_active_ = false;
        // Pending is gone; recompute exposure from realized positions only.
        total_abs_exposure_ = 0;
        for (const auto& kv : positions_) total_abs_exposure_ += std::abs(kv.second);
    }

    // Accessors for testing
    int32_t get_position(const char* symbol) const noexcept { return lookup(positions_, sym_to_key(symbol)); }
    int32_t get_pending(const char* symbol)  const noexcept { return lookup(pending_,   sym_to_key(symbol)); }

    double get_daily_pnl() const noexcept { return daily_pnl_; }
    uint64_t get_total_checks() const noexcept { return total_checks_; }
    uint64_t get_total_rejects() const noexcept { return total_rejects_; }

    void print_stats() const {
        printf("\n=== Risk Manager Statistics ===\n");
        printf("  Total checks: %lu\n", (unsigned long)total_checks_);
        printf("  Allowed: %lu\n", (unsigned long)(total_checks_ - total_rejects_));
        printf("  Rejected: %lu\n", (unsigned long)total_rejects_);
        double avg = total_checks_ > 0
            ? static_cast<double>(total_latency_ns_) / total_checks_ : 0.0;
        printf("  Avg check latency: %.0f ns\n", avg);
        printf("  Kill switch: %s\n", kill_switch_active_ ? "ACTIVE" : "inactive");
        printf("  Daily P&L: $%.2f\n", daily_pnl_);
    }

private:
    RiskCheckResult make_reject(const char* reason, int64_t t0) noexcept {
        int64_t elapsed = mono_ns() - t0;
        total_rejects_++;
        total_latency_ns_ += elapsed;
        return RiskCheckResult(RiskAction::REJECT, reason, elapsed);
    }

    // lookup: return value for key or 0 if absent (no insert)
    static int32_t lookup(const std::unordered_map<uint64_t, int32_t>& m,
                          uint64_t key) noexcept {
        auto it = m.find(key);
        return (it != m.end()) ? it->second : 0;
    }

    // adjust_pending: shared mutator for on_order_sent / on_order_cancelled.
    // Keeps total_abs_exposure_ in sync with sum_s |pos[s] + pend[s]|.
    void adjust_pending(uint64_t key, int32_t delta) noexcept {
        const int32_t pos      = lookup(positions_, key);
        const int32_t old_pend = lookup(pending_,   key);
        const int32_t new_pend = old_pend + delta;
        pending_[key]         = new_pend;
        total_abs_exposure_  += std::abs(pos + new_pend) - std::abs(pos + old_pend);
    }
};
