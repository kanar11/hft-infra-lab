/*
 * Symbol packing: NASDAQ-style 8-char ASCII ticker → uint64_t.
 * Pakowanie symbolu: 8-znakowy ticker ASCII → uint64_t.
 *
 * Used as a hash-map key to avoid std::string allocation on every lookup.
 * Caller contract: sym is null-terminated within 8 bytes (e.g. char[9]).
 */
#pragma once

#include <cstdint>
#include <cstring>


inline uint64_t sym_to_key(const char* sym) noexcept {
    uint64_t key = 0;
    const void* end = std::memchr(sym, '\0', 8);
    const std::size_t len = end
            ? static_cast<std::size_t>(static_cast<const char*>(end) - sym)
            : 8;
    for (std::size_t i = 0; i < len; ++i)
        key |= (static_cast<uint64_t>(static_cast<unsigned char>(sym[i])) << (i * 8));
    return key;
}
