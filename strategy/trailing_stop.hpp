/*
 * TrailingStop — stop kroczacy (expansion #147).
 *
 * Stop, ktory PODAZA za korzystnym ruchem ceny (ratchet) i nigdy sie nie cofa:
 *   long  -> stop = max(stop, price - trail); wyjscie gdy price <= stop
 *   short -> stop = min(stop, price + trail); wyjscie gdy price >= stop
 * Chroni zysk: gdy cena idzie w nasza strone stop sie zaciska, a gdy odwroci o
 * `trail` od szczytu/dolka — zamykamy pozycje. Reuzywalny modul exitu dla
 * dowolnej strategii (mean-reversion/momentum/...).
 *
 * Header-only, bez zaleznosci. update(price) zwraca true gdy STOPPED OUT.
 */
#pragma once

class TrailingStop {
    bool   is_long_;
    double trail_;
    double stop_;
    bool   active_;

public:
    TrailingStop(bool is_long, double entry_price, double trail_amount) noexcept
        : is_long_(is_long),
          trail_(trail_amount > 0.0 ? trail_amount : 0.0),
          stop_(is_long ? entry_price - trail_amount : entry_price + trail_amount),
          active_(true) {}

    // update: podaj nowa cene. Zaciska stop w korzystna strone; zwraca true gdy
    // cena przebila stop (pozycja zamknieta). Po wyjsciu kolejne update -> false.
    bool update(double price) noexcept {
        if (!active_) return false;
        if (is_long_) {
            const double cand = price - trail_;
            if (cand > stop_) stop_ = cand;            // ratchet w gore
            if (price <= stop_) { active_ = false; return true; }
        } else {
            const double cand = price + trail_;
            if (cand < stop_) stop_ = cand;            // ratchet w dol
            if (price >= stop_) { active_ = false; return true; }
        }
        return false;
    }

    double stop()   const noexcept { return stop_; }
    bool   active() const noexcept { return active_; }
    bool   is_long() const noexcept { return is_long_; }
};
