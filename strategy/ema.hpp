/*
 * EMA — wykladnicza srednia kroczaca (expansion #173).
 *
 * EMA_t = alpha * x_t + (1 - alpha) * EMA_{t-1}. W przeciwienstwie do SMA wazy
 * mocniej swieze dane (krotsze opoznienie), bez okna w pamieci (O(1), jedna
 * liczba stanu). Reuzywalny prymityw dla strategii (trend, sygnaly EMA-crossover,
 * wygladzanie metryk).
 *
 * alpha bezposrednio albo z okresu: alpha = 2/(period+1) (konwencja "N-okresowa
 * EMA"). Header-only.
 */
#pragma once

#include <cmath>


class EMA {
    double alpha_;
    double value_;
    bool   init_;

public:
    explicit EMA(double alpha) noexcept
        : alpha_(alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha)),
          value_(0.0), init_(false) {}

    // from_period: N-okresowa EMA -> alpha = 2/(N+1).
    static EMA from_period(int period) noexcept {
        const int p = period < 1 ? 1 : period;
        return EMA(2.0 / (static_cast<double>(p) + 1.0));
    }

    // update: dolicz obserwacje, zwroc nowa wartosc EMA. Pierwsza = seed.
    double update(double x) noexcept {
        if (!init_) { value_ = x; init_ = true; }
        else        value_ = alpha_ * x + (1.0 - alpha_) * value_;
        return value_;
    }

    double value() const noexcept { return value_; }
    double alpha() const noexcept { return alpha_; }
    bool   ready() const noexcept { return init_; }
    void   reset() noexcept { value_ = 0.0; init_ = false; }
};
