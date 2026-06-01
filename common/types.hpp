/*
 * Wspólne typy handlowe — używane przez każdy moduł operujący na zleceniach.
 */
#pragma once

#include <cstdint>


// Price — cena w TICKACH (fixed-point integer). 1 tick = $0.0001.
// Konwencja całego labu: nigdy float/double dla cen na hot path.
// (PRICE_SCALE jest zdefiniowane lokalnie w oms.hpp; tu trzymamy tylko alias.)
using Price = std::int64_t;


// Side — BUY albo SELL. Jeden kanoniczny typ żeby OMS/Risk/Strategy/Logger
// nie wymyślały własnych kodowań (const char* / int / enum).
enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1
};

inline const char* side_str(Side s) noexcept {
    return s == Side::BUY ? "BUY" : "SELL";
}

// side_from_str: toleruje "BUY"/"SELL", "B"/"S", lowercase. Wszystko co nie
// zaczyna się od 'B'/'b' jest traktowane jako SELL.
inline Side side_from_str(const char* s) noexcept {
    return (s && (s[0] == 'B' || s[0] == 'b')) ? Side::BUY : Side::SELL;
}
