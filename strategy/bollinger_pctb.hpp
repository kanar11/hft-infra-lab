/*
 * Bollinger %B (expansion #246).
 *
 * %B = (cena - dolne_pasmo) / (gorne_pasmo - dolne_pasmo), gdzie pasma =
 * SMA +/- k * odchylenie standardowe (populacyjne) po oknie N. Normalizuje
 * polozenie ceny WZGLEDEM pasm Bollingera:
 *   0.0 = na dolnym pasmie, 0.5 = na sredniej, 1.0 = na gornym;
 *   > 1.0 = powyzej gornego (wykupienie), < 0.0 = ponizej dolnego (wyprzedanie).
 *
 * W odroznieniu od strategii Bollinger (sygnaly wejscia) %B to ciagly WSKAZNIK
 * pozycji — wygodny do filtrow i ensemble. Plaskie okno (sd=0) -> 0.5. Header-only.
 */
#pragma once

#include <deque>
#include <cmath>


class BollingerPercentB {
    int    period_;
    double k_;
    std::deque<double> window_;

public:
    explicit BollingerPercentB(int period = 20, double k = 2.0) noexcept
        : period_(period < 1 ? 1 : period), k_(k) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    double value() const noexcept {
        if (window_.size() < 2) return 0.5;
        const double n = static_cast<double>(window_.size());
        double sum = 0.0;
        for (double p : window_) sum += p;
        const double mean = sum / n;
        double var = 0.0;
        for (double p : window_) { const double d = p - mean; var += d * d; }
        var /= n;                                   // wariancja populacyjna
        const double sd = std::sqrt(var);
        if (sd == 0.0) return 0.5;                  // plaskie okno -> srodek
        const double lower = mean - k_ * sd;
        const double width = 2.0 * k_ * sd;         // gorne - dolne
        return (window_.back() - lower) / width;
    }

    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_; }
    void reset() noexcept { window_.clear(); }
};
