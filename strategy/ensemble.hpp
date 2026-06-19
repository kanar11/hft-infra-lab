/*
 * combine_signals — ensemble/glosowanie sygnalow (expansion #140).
 *
 * Pojedyncza strategia bywa zaszumiona; laczenie kilku (mean-reversion, momentum,
 * RSI, donchian...) i wymaganie ZGODY redukuje falszywe sygnaly. combine_signals
 * zlicza wazne sygnaly BUY vs SELL i emituje kierunek tylko gdy:
 *   - co najmniej `min_agree` sygnalow wskazuje te strone, ORAZ
 *   - ta strona ma przewage (wiecej niz przeciwna)
 * Inaczej -> HOLD (invalid). Ilosc = suma qty zgadzajacej sie strony; cena/stock
 * z pierwszego sygnalu tej strony.
 *
 * Header-only, czysta funkcja — caller zbiera sygnaly z N strategii dla tego
 * samego symbolu/ticku i woła raz.
 */
#pragma once

#include "mean_reversion.hpp"   // Signal

#include <cstring>


inline Signal combine_signals(const Signal* sigs, int n, int min_agree) noexcept {
    Signal out;   // domyslnie invalid (HOLD)
    if (!sigs || n <= 0 || min_agree <= 0) return out;

    int     buys = 0, sells = 0;
    int32_t buy_qty = 0, sell_qty = 0;
    const Signal* first_buy  = nullptr;
    const Signal* first_sell = nullptr;

    for (int i = 0; i < n; ++i) {
        if (!sigs[i].valid) continue;
        if (sigs[i].side == Side::BUY) {
            ++buys; buy_qty += sigs[i].quantity;
            if (!first_buy) first_buy = &sigs[i];
        } else {
            ++sells; sell_qty += sigs[i].quantity;
            if (!first_sell) first_sell = &sigs[i];
        }
    }

    const Signal* pick = nullptr;
    Side    side = Side::BUY;
    int32_t qty  = 0;
    if (buys >= min_agree && buys > sells)        { pick = first_buy;  side = Side::BUY;  qty = buy_qty; }
    else if (sells >= min_agree && sells > buys)  { pick = first_sell; side = Side::SELL; qty = sell_qty; }
    if (!pick) return out;

    out.valid         = true;
    out.side          = side;
    out.quantity      = qty;
    out.price         = pick->price;
    out.sma           = pick->sma;
    out.deviation_pct = pick->deviation_pct;
    out.timestamp_ns  = pick->timestamp_ns;
    std::memcpy(out.stock, pick->stock, sizeof(out.stock));
    return out;
}
