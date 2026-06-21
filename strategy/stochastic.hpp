/*
 * Stochastic Oscillator — %K (expansion #190).
 *
 * %K = (close - lowest_low) / (highest_high - lowest_low) * 100, po oknie N
 * obserwacji. Mierzy POLOZENIE biezacej ceny w zakresie okna: ~100 = przy szczycie
 * (wykupienie), ~0 = przy dnie (wyprzedanie). W odroznieniu od RSI (oparty na
 * sredniej zyskow/strat) patrzy na zakres min-max — szybciej reaguje na ekstrema.
 *
 * Konwencja: overbought > 80, oversold < 20; przy plaskim oknie (hi==lo) zwraca 50.
 * Header-only, okno w std::deque.
 */
#pragma once

#include <deque>


class Stochastic {
    int period_;
    std::deque<double> window_;

public:
    explicit Stochastic(int period = 14) noexcept
        : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    // percent_k: pozycja ostatniej ceny w zakresie [min,max] okna, 0..100.
    double percent_k() const noexcept {
        if (window_.empty()) return 50.0;
        double lo = window_.front();
        double hi = window_.front();
        for (double p : window_) { if (p < lo) lo = p; if (p > hi) hi = p; }
        if (hi <= lo) return 50.0;                 // plaskie okno — neutralnie
        return (window_.back() - lo) / (hi - lo) * 100.0;
    }

    bool   ready()      const noexcept { return static_cast<int>(window_.size()) >= period_; }
    bool   overbought() const noexcept { return percent_k() > 80.0; }
    bool   oversold()   const noexcept { return percent_k() < 20.0; }
    void   reset()      noexcept { window_.clear(); }
};
