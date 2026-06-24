/*
 * Common trading types — used by every module that operates on orders.
 */
#pragma once

#include <cstdint>


// Price — a price in TICKS (fixed-point integer). 1 tick = $0.0001.
// Convention for the whole lab: never float/double for prices on the hot path.
// (PRICE_SCALE is defined locally in oms.hpp; here we keep only the alias.)
using Price = std::int64_t;


// Side — BUY or SELL. One canonical type so OMS/Risk/Strategy/Logger
// don't invent their own encodings (const char* / int / enum).
enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1
};

inline const char* side_str(Side s) noexcept {
    return s == Side::BUY ? "BUY" : "SELL";
}

// side_from_str: tolerates "BUY"/"SELL", "B"/"S", lowercase. Anything that does not
// start with 'B'/'b' is treated as SELL.
inline Side side_from_str(const char* s) noexcept {
    return (s && (s[0] == 'B' || s[0] == 'b')) ? Side::BUY : Side::SELL;
}
