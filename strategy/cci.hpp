/*
 * CCI — Commodity Channel Index (expansion #238).
 *
 * CCI = (cena - SMA) / (0.015 * srednie_odchylenie_bezwzgledne), po oknie N.
 * Mierzy jak DALEKO biezaca cena odchyla sie od swojej sredniej, w jednostkach
 * typowego odchylenia. Stala 0.015 sprawia, ze ~70-80% wartosci miesci sie w
 * [-100, +100]; |CCI| > 100 = ruch nietypowo silny: > +100 wykupienie / poczatek
 * trendu wzrostowego, < -100 wyprzedanie. W odroznieniu od oscylatorow opartych na
 * min/max (Stochastic) bazuje na sredniej i srednim odchyleniu. Header-only.
 */
#pragma once

#include <deque>
#include <cmath>


class CCI {
    int period_;
    std::deque<double> window_;

public:
    explicit CCI(int period = 20) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    double value() const noexcept {
        if (window_.empty()) return 0.0;
        const double n = static_cast<double>(window_.size());
        double sum = 0.0;
        for (double p : window_) sum += p;
        const double mean = sum / n;
        double mad = 0.0;
        for (double p : window_) mad += std::fabs(p - mean);
        mad /= n;
        if (mad == 0.0) return 0.0;                 // plaskie okno — brak sygnalu
        return (window_.back() - mean) / (0.015 * mad);
    }

    bool ready()      const noexcept { return static_cast<int>(window_.size()) >= period_; }
    bool overbought() const noexcept { return value() > 100.0; }
    bool oversold()   const noexcept { return value() < -100.0; }
    void reset()      noexcept { window_.clear(); }
};
