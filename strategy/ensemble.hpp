/*
 * combine_signals — signal ensemble/voting (expansion #140).
 *
 * A single strategy can be noisy; combining several (mean-reversion, momentum,
 * RSI, donchian...) and requiring AGREEMENT reduces false signals. combine_signals
 * counts valid BUY vs SELL signals and emits a direction only when:
 *   - at least `min_agree` signals point that way, AND
 *   - that side has the majority (more than the opposite)
 * Otherwise -> HOLD (invalid). Quantity = sum of qty of the agreeing side; price/stock
 * from the first signal of that side.
 *
 * Header-only, a pure function — the caller gathers signals from N strategies for the
 * same symbol/tick and calls it once.
 */
#pragma once

#include "mean_reversion.hpp"   // Signal

#include <cstring>


inline Signal combine_signals(const Signal* sigs, int n, int min_agree) noexcept {
    Signal out;   // invalid by default (HOLD)
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
