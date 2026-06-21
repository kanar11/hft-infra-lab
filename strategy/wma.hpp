/*
 * WMA — Weighted Moving Average (expansion #198).
 *
 * Liniowo wazona srednia: w oknie N najstarsza obserwacja ma wage 1, najnowsza N
 * (WMA = Σ w_i*x_i / Σ w_i). W przeciwienstwie do SMA (rowne wagi) i EMA
 * (wykladnicze, nieskonczony ogon) wazy liniowo i ma SKONCZONE okno — krotsze
 * opoznienie niz SMA przy mniejszym szumie niz EMA. Baza pod Hull MA.
 * Header-only, okno w std::deque.
 */
#pragma once

#include <deque>


class WMA {
    int period_;
    std::deque<double> window_;

public:
    explicit WMA(int period) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    // value: Σ (waga_i * x_i) / Σ waga_i, waga rosnie 1..k od najstarszej.
    double value() const noexcept {
        if (window_.empty()) return 0.0;
        double num = 0.0;
        long   wsum = 0;
        long   w = 1;
        for (double x : window_) { num += x * static_cast<double>(w); wsum += w; ++w; }
        return num / static_cast<double>(wsum);
    }

    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_; }
    void reset() noexcept { window_.clear(); }
};
