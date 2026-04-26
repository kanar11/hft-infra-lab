/*
 * Risk Manager — pre-trade checks on the critical path.
 *
 * Pipeline position: Strategy -> Router -> [Risk] -> OMS -> Exchange
 *
 * Checks (in order):
 *   1. Kill switch
 *   2. Order notional value
 *   3. Per-symbol position limit
 *   4. Portfolio gross exposure
 *   5. Daily loss circuit breaker
 *   6. Drawdown from peak
 *   7. Orders/sec rate limit
 *
 * Hot-path discipline:
 *   - Symbols hashed as packed uint64 — no std::string allocation per check.
 *   - Rate limiter is an O(1) ring buffer of timestamps.
 *   - Single now_ns() per check; latency derived from one delta.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cmath>


// === Symbol key (8-char ticker packed into uint64) ===

struct RMSymbolKey {
    uint64_t v;

    RMSymbolKey() noexcept : v(0) {}

    explicit RMSymbolKey(const char* sym) noexcept : v(0) {
        char buf[8] = {0};
        for (int i = 0; i < 8 && sym[i]; ++i) buf[i] = sym[i];
        std::memcpy(&v, buf, 8);
    }

    bool operator==(const RMSymbolKey& o) const noexcept { return v == o.v; }
};

struct RMSymbolKeyHash {
    size_t operator()(const RMSymbolKey& k) const noexcept {
        uint64_t x = k.v;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }
};


// === Public API types ===

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

struct RiskCheckResult {
    RiskAction action;
    char       reason[128];   // fixed buffer — no heap alloc
    int64_t    latency_ns;

    RiskCheckResult() noexcept
        : action(RiskAction::ALLOW), latency_ns(0) { reason[0] = '\0'; }

    RiskCheckResult(RiskAction a, const char* r, int64_t lat) noexcept
        : action(a), latency_ns(lat) {
        std::strncpy(reason, r, 127);
        reason[127] = '\0';
    }
};

struct RiskLimits {
    int32_t max_position_per_symbol = 5000;
    int32_t max_portfolio_exposure  = 50000;
    int64_t max_daily_loss          = 100000;
    int32_t max_orders_per_second   = 1000;
    int64_t max_order_value         = 500000;
    double  max_drawdown_pct        = 5.0;
};


// === RiskManager ===

class RiskManager {
    RiskLimits limits_;

    std::unordered_map<RMSymbolKey, int32_t, RMSymbolKeyHash> positions_;
    int64_t gross_exposure_;   // sum of |positions| — maintained incrementally

    double  daily_pnl_;
    double  peak_pnl_;
    bool    kill_switch_active_;

    // Rate limiter: ring buffer of recent order timestamps.
    // Capacity = max_orders_per_second; once full, oldest is overwritten.
    std::vector<int64_t> rl_buf_;
    size_t  rl_head_;          // next write position
    size_t  rl_count_;         // entries currently stored

    uint64_t total_checks_;
    uint64_t total_rejects_;
    uint64_t total_latency_ns_;

    static int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

public:
    explicit RiskManager(const RiskLimits& limits = RiskLimits()) noexcept
        : limits_(limits),
          gross_exposure_(0),
          daily_pnl_(0.0),
          peak_pnl_(0.0),
          kill_switch_active_(false),
          rl_head_(0),
          rl_count_(0),
          total_checks_(0),
          total_rejects_(0),
          total_latency_ns_(0) {
        rl_buf_.assign(static_cast<size_t>(limits.max_orders_per_second), 0);
    }

    // check_order: run all pre-trade checks. Single now_ns() at entry, single delta at exit.
    RiskCheckResult check_order(const char* symbol, const char* side,
                                double price, int32_t quantity) noexcept {
        const int64_t t0 = now_ns();
        total_checks_++;

        // 1. Kill switch.
        if (kill_switch_active_) {
            return finalize(RiskAction::REJECT, "Kill switch active", t0);
        }

        // 2. Order notional value.
        const int64_t order_value = static_cast<int64_t>(price * quantity);
        if (order_value > limits_.max_order_value) {
            return finalize(RiskAction::REJECT, "Order value exceeds limit", t0);
        }

        // 3. Per-symbol position limit.
        const bool is_buy = (side[0] == 'B');
        const RMSymbolKey key(symbol);
        auto pos_it = positions_.find(key);
        const int32_t current_pos = (pos_it != positions_.end()) ? pos_it->second : 0;
        const int32_t projected   = current_pos + (is_buy ? quantity : -quantity);
        if (std::abs(projected) > limits_.max_position_per_symbol) {
            return finalize(RiskAction::REJECT, "Position limit exceeded", t0);
        }

        // 4. Portfolio gross exposure (incremental: replace this symbol's contribution).
        const int64_t projected_exposure =
            gross_exposure_ - std::abs(current_pos) + std::abs(projected);
        if (projected_exposure > limits_.max_portfolio_exposure) {
            return finalize(RiskAction::REJECT, "Portfolio exposure exceeded", t0);
        }

        // 5. Daily loss circuit breaker.
        if (daily_pnl_ < -static_cast<double>(limits_.max_daily_loss)) {
            kill_switch_active_ = true;
            return finalize(RiskAction::REJECT, "Circuit breaker: daily loss limit", t0);
        }

        // 6. Drawdown from peak.
        if (peak_pnl_ > 0.0) {
            const double dd_pct = (peak_pnl_ - daily_pnl_) / peak_pnl_ * 100.0;
            if (dd_pct > limits_.max_drawdown_pct) {
                kill_switch_active_ = true;
                return finalize(RiskAction::REJECT, "Drawdown limit exceeded", t0);
            }
        }

        // 7. Rate limit (orders/sec) — O(1) ring buffer scan-free check.
        if (rl_count_ == rl_buf_.size()) {
            // Buffer full: oldest entry sits at rl_head_ (next slot to overwrite).
            const int64_t oldest = rl_buf_[rl_head_];
            if (t0 - oldest < 1'000'000'000) {
                return finalize(RiskAction::REJECT, "Rate limit exceeded", t0);
            }
        }
        rl_buf_[rl_head_] = t0;
        rl_head_ = (rl_head_ + 1) % rl_buf_.size();
        if (rl_count_ < rl_buf_.size()) ++rl_count_;

        return finalize(RiskAction::ALLOW, "All checks passed", t0);
    }

    // update_position: called after a fill. Maintains gross_exposure_ incrementally.
    void update_position(const char* symbol, const char* side, int32_t quantity) noexcept {
        const RMSymbolKey key(symbol);
        const bool is_buy = (side[0] == 'B');
        int32_t& pos = positions_[key];
        const int32_t old_abs = std::abs(pos);
        pos += (is_buy ? quantity : -quantity);
        gross_exposure_ += std::abs(pos) - old_abs;
    }

    void update_pnl(double pnl_change) noexcept {
        daily_pnl_ += pnl_change;
        if (daily_pnl_ > peak_pnl_) peak_pnl_ = daily_pnl_;
    }

    void activate_kill_switch() noexcept   { kill_switch_active_ = true; }
    void deactivate_kill_switch() noexcept { kill_switch_active_ = false; }
    bool is_kill_switch_active() const noexcept { return kill_switch_active_; }

    void reset_daily() noexcept {
        daily_pnl_ = 0.0;
        peak_pnl_  = 0.0;
        rl_head_   = 0;
        rl_count_  = 0;
        kill_switch_active_ = false;
    }

    int32_t get_position(const char* symbol) const noexcept {
        auto it = positions_.find(RMSymbolKey(symbol));
        return (it != positions_.end()) ? it->second : 0;
    }

    double   get_daily_pnl()     const noexcept { return daily_pnl_; }
    uint64_t get_total_checks()  const noexcept { return total_checks_; }
    uint64_t get_total_rejects() const noexcept { return total_rejects_; }

    void print_stats() const {
        printf("\n=== Risk Manager Statistics ===\n");
        printf("  Total checks: %lu\n", (unsigned long)total_checks_);
        printf("  Allowed: %lu\n", (unsigned long)(total_checks_ - total_rejects_));
        printf("  Rejected: %lu\n", (unsigned long)total_rejects_);
        const double avg = total_checks_ > 0
            ? static_cast<double>(total_latency_ns_) / total_checks_ : 0.0;
        printf("  Avg check latency: %.0f ns\n", avg);
        printf("  Kill switch: %s\n", kill_switch_active_ ? "ACTIVE" : "inactive");
        printf("  Daily P&L: $%.2f\n", daily_pnl_);
    }

private:
    RiskCheckResult finalize(RiskAction action, const char* reason, int64_t t0) noexcept {
        const int64_t elapsed = now_ns() - t0;
        total_latency_ns_ += elapsed;
        if (action != RiskAction::ALLOW) total_rejects_++;
        return RiskCheckResult(action, reason, elapsed);
    }
};
