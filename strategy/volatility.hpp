/*
 * VolatilityEstimator — kroczaca zmiennosc zwrotow (expansion #165).
 *
 * Rolling odchylenie standardowe zwrotow ((p - p_prev)/p_prev) z okna N. Podstawa
 * vol-targetingu i position sizingu: wieksza zmiennosc -> mniejsza pozycja (staly
 * budzet ryzyka). Reuzywalny modul niezalezny od konkretnej strategii.
 *
 * O(1) per cena dzieki running sum / sum-of-squares. Header-only.
 */
#pragma once

#include <cmath>
#include <cstdint>


class VolatilityEstimator {
    static constexpr int CAP = 256;
    int    window_;
    double rets_[CAP];
    double sum_, sum_sq_;
    int    count_, head_;
    double prev_;
    bool   has_prev_;

public:
    explicit VolatilityEstimator(int window = 20) noexcept
        : window_(window < 1 ? 1 : (window > CAP ? CAP : window)),
          rets_{}, sum_(0.0), sum_sq_(0.0), count_(0), head_(0), prev_(0.0), has_prev_(false) {}

    // on_price: dolicz nowa cene. Pierwsza = baseline (brak zwrotu).
    void on_price(double price) noexcept {
        if (price <= 0.0) return;
        if (!has_prev_) { prev_ = price; has_prev_ = true; return; }
        const double r = (price - prev_) / prev_;
        prev_ = price;
        if (count_ == window_) {                      // eviction najstarszego zwrotu
            const double old = rets_[head_];
            sum_ -= old; sum_sq_ -= old * old;
        }
        rets_[head_] = r; sum_ += r; sum_sq_ += r * r;
        head_ = (head_ + 1) % window_;
        if (count_ < window_) ++count_;
    }

    // volatility: odchylenie standardowe zwrotow w oknie (0 gdy <2 probek).
    double volatility() const noexcept {
        if (count_ < 2) return 0.0;
        const double mean = sum_ / count_;
        const double var  = sum_sq_ / count_ - mean * mean;
        return var > 0.0 ? std::sqrt(var) : 0.0;
    }

    // target_size: rozmiar pozycji przy danym budzecie ryzyka (vol-targeting):
    // base_size * (target_vol / current_vol), gdy vol>0; inaczej base_size.
    int32_t target_size(int32_t base_size, double target_vol) const noexcept {
        const double v = volatility();
        if (v <= 0.0 || target_vol <= 0.0) return base_size;
        const double scaled = base_size * (target_vol / v);
        return scaled < 1.0 ? 1 : static_cast<int32_t>(scaled);
    }

    int    samples() const noexcept { return count_; }
};
