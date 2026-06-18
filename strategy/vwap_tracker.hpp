/*
 * VWAPTracker — sledzenie rynkowego VWAP + slippage egzekucji (expansion #113).
 *
 * VWAP (volume-weighted average price) to standardowy benchmark jakosci
 * egzekucji: "czy kupilem taniej niz srednia wazona wolumenem rynku?". Tracker
 * akumuluje notional i wolumen z printow rynku (TradeMsg/Executed) i liczy:
 *   - vwap()                — biezacy VWAP
 *   - slippage_bps(px, buy) — o ile gorsza (>0) / lepsza (<0) byla nasza cena
 *                             wzgledem VWAP, w punktach bazowych
 *
 * Jeden tracker per symbol (caller trzyma mape). Header-only, O(1) per print.
 */
#pragma once

#include <cstdint>

class VWAPTracker {
    double  notional_ = 0.0;   // Sigma price*volume
    int64_t volume_   = 0;     // Sigma volume

public:
    // on_trade: dolicz print rynkowy (cena, wolumen).
    void on_trade(double price, int64_t vol) noexcept {
        if (price > 0.0 && vol > 0) {
            notional_ += price * static_cast<double>(vol);
            volume_   += vol;
        }
    }

    double  vwap()   const noexcept { return volume_ > 0 ? notional_ / static_cast<double>(volume_) : 0.0; }
    int64_t volume() const noexcept { return volume_; }

    // slippage_bps: jakosc egzekucji wzgledem VWAP, w bps. Dla BUY zaplacenie
    // POWYZEJ VWAP = dodatnie (gorzej); dla SELL sprzedaz PONIZEJ VWAP = dodatnie.
    // <0 = pobilismy VWAP (lepiej). 0 gdy brak danych VWAP.
    double slippage_bps(double exec_price, bool is_buy) const noexcept {
        const double v = vwap();
        if (v <= 0.0) return 0.0;
        const double diff = is_buy ? (exec_price - v) : (v - exec_price);
        return diff / v * 10000.0;
    }

    void reset() noexcept { notional_ = 0.0; volume_ = 0; }
};
