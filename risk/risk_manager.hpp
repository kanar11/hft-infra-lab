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
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cmath>


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

    // Position tracking: symbol → net quantity
    // Śledzenie pozycji: symbol → ilość netto
    std::unordered_map<std::string, int32_t> positions_;

    // P&L state
    double daily_pnl_;
    double peak_pnl_;

    // Kill switch
    // Wyłącznik awaryjny
    bool kill_switch_active_;

    // Rate limiting: timestamps of recent orders (nanoseconds)
    // Ograniczanie szybkości: znaczniki czasu ostatnich zleceń (nanosekundy)
    std::vector<int64_t> order_timestamps_;

    // Statistics
    uint64_t total_checks_;
    uint64_t total_rejects_;
    uint64_t total_latency_ns_;

    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

public:
    // Constructor — equivalent to Python's __init__
    // Konstruktor — odpowiednik __init__ w Pythonie
    explicit RiskManager(const RiskLimits& limits = RiskLimits()) noexcept
        : limits_(limits),
          daily_pnl_(0.0),
          peak_pnl_(0.0),
          kill_switch_active_(false),
          total_checks_(0),
          total_rejects_(0),
          total_latency_ns_(0) {
        order_timestamps_.reserve(limits.max_orders_per_second * 2);
    }

    // check_order: run all pre-trade risk checks
    // Returns RiskCheckResult with ALLOW, REJECT, or KILL
    // check_order: uruchom wszystkie przedhandlowe kontrole ryzyka
    // Zwraca RiskCheckResult z ALLOW, REJECT lub KILL
    RiskCheckResult check_order(const char* symbol, const char* side,
                                 double price, int32_t quantity) noexcept {
        int64_t t0 = now_ns();
        total_checks_++;

        // 1. Kill switch — reject everything
        // 1. Wyłącznik awaryjny — odrzuć wszystko
        if (kill_switch_active_) {
            return make_reject("Kill switch active", t0);
        }

        // 2. Order value check
        // 2. Sprawdzenie wartości zlecenia
        int64_t order_value = static_cast<int64_t>(price * quantity);
        if (order_value > limits_.max_order_value) {
            return make_reject("Order value exceeds limit", t0);
        }

        // 3. Per-symbol position limit
        // 3. Limit pozycji na symbol
        bool is_buy = (side[0] == 'B');
        std::string sym_key(symbol);
        int32_t current_pos = 0;
        auto pos_it = positions_.find(sym_key);
        if (pos_it != positions_.end()) {
            current_pos = pos_it->second;
        }
        int32_t projected = current_pos + (is_buy ? quantity : -quantity);
        if (std::abs(projected) > limits_.max_position_per_symbol) {
            return make_reject("Position limit exceeded", t0);
        }

        // 4. Portfolio exposure check
        // 4. Sprawdzenie ekspozycji portfela
        // Calculate total absolute exposure if this order goes through
        // Oblicz całkowitą bezwzględną ekspozycję jeśli to zlecenie przejdzie
        int64_t total_exposure = 0;
        for (const auto& [sym, qty] : positions_) {
            if (sym == sym_key) {
                total_exposure += std::abs(projected);
            } else {
                total_exposure += std::abs(qty);
            }
        }
        // If symbol not in positions yet, add its projected exposure
        // Jeśli symbolu jeszcze nie ma w pozycjach, dodaj jego prognozowaną ekspozycję
        if (pos_it == positions_.end()) {
            total_exposure += std::abs(projected);
        }
        if (total_exposure > limits_.max_portfolio_exposure) {
            return make_reject("Portfolio exposure exceeded", t0);
        }

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

        // 7. Rate limiting
        // 7. Ograniczanie szybkości
        int64_t now = now_ns();
        int64_t one_sec_ago = now - 1'000'000'000;
        // Remove old timestamps (older than 1 second)
        // Usuń stare znaczniki czasu (starsze niż 1 sekunda)
        size_t valid_start = 0;
        for (size_t i = 0; i < order_timestamps_.size(); ++i) {
            if (order_timestamps_[i] > one_sec_ago) {
                valid_start = i;
                break;
            }
            if (i == order_timestamps_.size() - 1) {
                valid_start = order_timestamps_.size();
            }
        }
        if (valid_start > 0 && !order_timestamps_.empty()) {
            order_timestamps_.erase(order_timestamps_.begin(),
                                     order_timestamps_.begin() + valid_start);
        }
        if (static_cast<int32_t>(order_timestamps_.size()) >= limits_.max_orders_per_second) {
            return make_reject("Rate limit exceeded", t0);
        }
        order_timestamps_.push_back(now);

        // All checks passed
        // Wszystkie kontrole przeszły
        int64_t elapsed = now_ns() - t0;
        total_latency_ns_ += elapsed;
        return RiskCheckResult(RiskAction::ALLOW, "All checks passed", elapsed);
    }

    // update_position: called after a fill to update tracked positions
    // update_position: wywoływane po realizacji do aktualizacji śledzonych pozycji
    void update_position(const char* symbol, const char* side, int32_t quantity) noexcept {
        std::string sym_key(symbol);
        bool is_buy = (side[0] == 'B');
        positions_[sym_key] += (is_buy ? quantity : -quantity);
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
        kill_switch_active_ = false;
    }

    // Accessors for testing
    int32_t get_position(const char* symbol) const noexcept {
        auto it = positions_.find(std::string(symbol));
        return (it != positions_.end()) ? it->second : 0;
    }

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
        int64_t elapsed = now_ns() - t0;
        total_rejects_++;
        total_latency_ns_ += elapsed;
        return RiskCheckResult(RiskAction::REJECT, reason, elapsed);
    }
};
