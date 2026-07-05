/*
 * KST — Know Sure Thing (Martin Pring's summed-momentum oscillator, expansion #510).
 *
 * KST = 1*RCMA1 + 2*RCMA2 + 3*RCMA3 + 4*RCMA4, where each RCMAi is a simple
 * moving average of a rate-of-change over a progressively longer horizon:
 *   RCMA1 = SMA(ROC(r1), s1)   RCMA2 = SMA(ROC(r2), s2)
 *   RCMA3 = SMA(ROC(r3), s3)   RCMA4 = SMA(ROC(r4), s4)
 * The four horizons are weighted 1..4 so the slowest, longest-horizon momentum
 * dominates — Pring's design intent: a smoothed, long-biased momentum sum whose
 * zero-line cross and signal-line cross flag major trend turns.
 *
 * Distinct from Coppock (#333, a WMA of the SUM of TWO ROCs) and from a lone
 * ROC: KST first SMA-smooths FOUR separate ROC horizons, then weights them, then
 * adds a signal SMA — a four-horizon consensus rather than a two-horizon blend.
 * Because every input is a ROC percentage, the whole oscillator is scale-
 * invariant. Built on the existing ROC primitive; the SMA smoothing is a small
 * internal rolling mean.
 *
 * Classic params (Pring, daily): ROC 10/15/20/30 smoothed by SMA 10/10/10/15,
 * signal SMA 9. Header-only.
 */
#pragma once

#include "roc.hpp"
#include <deque>


class KST {
    // Minimal rolling simple moving average over the last N inputs (running sum
    // so value() is O(1); the deque only bounds the window).
    struct Sma {
        int n_;
        double sum_ = 0.0;
        std::deque<double> w_;
        explicit Sma(int n = 1) noexcept : n_(n < 1 ? 1 : n) {}
        void update(double x) {
            w_.push_back(x); sum_ += x;
            while (static_cast<int>(w_.size()) > n_) { sum_ -= w_.front(); w_.pop_front(); }
        }
        double value() const noexcept {
            return w_.empty() ? 0.0 : sum_ / static_cast<double>(w_.size());
        }
        bool ready() const noexcept { return static_cast<int>(w_.size()) >= n_; }
        void reset() noexcept { w_.clear(); sum_ = 0.0; }
    };

    ROC roc1_, roc2_, roc3_, roc4_;
    Sma sma1_, sma2_, sma3_, sma4_;
    Sma signal_;

public:
    explicit KST(int r1 = 10, int r2 = 15, int r3 = 20, int r4 = 30,
                 int s1 = 10, int s2 = 10, int s3 = 10, int s4 = 15,
                 int sig = 9) noexcept
        : roc1_(r1), roc2_(r2), roc3_(r3), roc4_(r4),
          sma1_(s1), sma2_(s2), sma3_(s3), sma4_(s4), signal_(sig) {}

    void update(double price) {
        roc1_.update(price); roc2_.update(price);
        roc3_.update(price); roc4_.update(price);
        // Feed each ROC's SMA only once that ROC has warmed up, so the smoother
        // averages fully-formed momentum (no ROC=0 warmup placeholders biasing
        // the mean toward zero) — same discipline as Coppock (#333).
        if (roc1_.ready()) sma1_.update(roc1_.value());
        if (roc2_.ready()) sma2_.update(roc2_.value());
        if (roc3_.ready()) sma3_.update(roc3_.value());
        if (roc4_.ready()) sma4_.update(roc4_.value());
        if (ready_rcma()) signal_.update(raw_kst());
    }

    // value: the raw KST line = weighted sum of the four smoothed ROCs.
    double value()  const noexcept { return raw_kst(); }
    // signal: SMA(KST, sig); the classic trade trigger is KST crossing it.
    double signal() const noexcept { return signal_.value(); }
    // ready once all four RCMA smoothers are formed; the signal line lags further.
    bool ready()        const noexcept { return ready_rcma(); }
    bool signal_ready() const noexcept { return signal_.ready(); }
    // Bullish when KST leads its signal line (momentum accelerating).
    bool bullish() const noexcept {
        return ready() && signal_.ready() && value() > signal();
    }
    void reset() noexcept {
        roc1_.reset(); roc2_.reset(); roc3_.reset(); roc4_.reset();
        sma1_.reset(); sma2_.reset(); sma3_.reset(); sma4_.reset(); signal_.reset();
    }

private:
    bool ready_rcma() const noexcept {
        return sma1_.ready() && sma2_.ready() && sma3_.ready() && sma4_.ready();
    }
    double raw_kst() const noexcept {
        return 1.0 * sma1_.value() + 2.0 * sma2_.value()
             + 3.0 * sma3_.value() + 4.0 * sma4_.value();
    }
};
