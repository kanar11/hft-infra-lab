/*
 * VolumeIndices — NVI + PVI (Negative / Positive Volume Index) (expansion #398).
 *
 * Fosbeck/Dysart cumulative indices, both starting at 1000:
 *   volume UP   vs previous print → PVI += PVI * %price_change   (NVI frozen)
 *   volume DOWN vs previous print → NVI += NVI * %price_change   (PVI frozen)
 *   volume UNCHANGED              → neither moves
 *
 * Where OBV (#341) and PVT (#357) WEIGHT price moves by volume, NVI/PVI
 * GATE on the volume direction instead: PVI accumulates price action that
 * happened on expanding activity (the crowd piling in), NVI accumulates
 * what price did on contracting activity — the classic "smart money"
 * reading, since informed flow does not need rising volume to move price.
 * Divergence between the two tells whether a move is crowd-driven or
 * quiet-accumulation-driven.
 *
 * Same on_trade(price, volume) convention as OBV/PVT/ForceIndex/MFI/VWMA.
 * Header-only, O(1) per print.
 */
#pragma once

#include <cstdint>

class VolumeIndices {
public:
    static constexpr double BASE = 1000.0;   // the conventional starting level

private:
    double       nvi_         = BASE;
    double       pvi_         = BASE;
    double       last_price_  = 0.0;
    std::int64_t last_volume_ = 0;
    bool         has_last_    = false;

public:
    // on_trade: add a market print. The first valid print only seeds the
    // baselines (no previous price/volume to compare against). Invalid
    // prints (non-positive price or volume) are ignored entirely.
    void on_trade(double price, std::int64_t volume) noexcept {
        if (!(price > 0.0) || volume <= 0) return;
        if (has_last_ && last_price_ > 0.0) {
            const double pct = (price - last_price_) / last_price_;
            if (volume > last_volume_)      pvi_ += pvi_ * pct;
            else if (volume < last_volume_) nvi_ += nvi_ * pct;
            // volume unchanged: no directional information — neither moves
        }
        last_price_  = price;
        last_volume_ = volume;
        has_last_    = true;
    }

    // nvi: price action accumulated on FALLING volume ("smart money").
    double nvi() const noexcept { return nvi_; }
    // pvi: price action accumulated on RISING volume (the crowd).
    double pvi() const noexcept { return pvi_; }
    bool   ready() const noexcept { return has_last_; }

    void reset() noexcept {
        nvi_ = BASE; pvi_ = BASE;
        last_price_ = 0.0; last_volume_ = 0; has_last_ = false;
    }
};
