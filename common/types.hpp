/*
 * Shared trading types — used by every module that talks about orders.
 * Wspólne typy handlowe — używane przez każdy moduł operujący na zleceniach.
 */
#pragma once

#include <cstdint>


// === Side: BUY or SELL ===
// One canonical type so OMS, Risk, Strategy, Logger, Simulator etc. don't
// each invent their own const char* / int / enum encoding.
enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1
};

inline const char* side_str(Side s) noexcept {
    return s == Side::BUY ? "BUY" : "SELL";
}

// side_from_str: tolerates "BUY"/"SELL", "B"/"S", lowercase first letter.
// Anything that doesn't start with 'B'/'b' is treated as SELL.
inline Side side_from_str(const char* s) noexcept {
    return (s && (s[0] == 'B' || s[0] == 'b')) ? Side::BUY : Side::SELL;
}
