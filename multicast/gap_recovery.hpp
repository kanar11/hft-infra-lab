/*
 * GapRecovery — a RECOVERY layer on top of sequence gap detection (expansion #82).
 *
 * Split out of multicast.hpp into a separate, lightweight header: multicast.hpp
 * pulls in socket headers (sys/socket) and the global MsgType — but recovery is
 * pure logic that we want to test/use without transport. multicast.hpp
 * includes this file, so multicast_demo still gets GapRecovery.
 *
 * SequenceTracker (multicast.hpp) only DETECTS gaps. That is the first step; a real
 * feed handler must also RECOVER the missing ones: remember WHICH seq it lost, send
 * a gap-fill request to a retransmission server (MoldUDP64 request / A-B arbitration)
 * and reconcile the retransmitted packets with the primary feed.
 *
 * Model:
 *   observe(seq)        — primary feed; a gap → records the missing seq into `missing`
 *   on_retransmit(seq)  — a packet from the recovery server / line B; removes from `missing`
 *   next_request(lo,hi) — the range for a gap-fill request (lowest..highest missing)
 *   has_gaps()/missing()— whether the book is still uncertain
 */
#pragma once

#include <cstdint>
#include <set>
#include <map>
#include <vector>
#include <unordered_map>
#include <iterator>
#include <utility>

#include "../common/ring_counter.hpp"

namespace multicast {


// FeedRateMeter — feed throughput over a time window (expansion #132).
//
// The feed handler must see the current rate (msgs/sec) to detect a burst (risk
// of overload / drop) or a lull. A sliding-window counter of timestamps:
// at each measurement it evicts those older than the window, size = count in the window.
struct FeedRateMeter {
    // Sliding window backed by TimestampRing (common/ring_counter.hpp) — a
    // contiguous power-of-2 ring (no heap, no pointer chasing), L1-resident
    // (32 KB) so insert/evict touch only hot cache lines and wrap with a
    // bitmask (no idiv). Bounded: holds up to RING_SIZE-1 timestamps; on a
    // burst beyond that the oldest are dropped and count() saturates — for
    // burst DETECTION a floor of 4095 is already an unambiguous overload
    // signal, and the bound keeps latency deterministic. Same ring mechanics
    // as RiskManager's order-rate limiter (risk/risk_manager.hpp).
    static constexpr int RING_SIZE = 4096;

    std::int64_t window_ns;

    explicit FeedRateMeter(std::int64_t window_ns_ = 1'000'000'000) noexcept
        : window_ns(window_ns_ > 0 ? window_ns_ : 1) {}

    void on_message(std::int64_t now_ns) noexcept {
        ring_.push(now_ns);
        ring_.evict(now_ns - window_ns);
        const std::size_t c = ring_.count();
        if (c > peak_count_) peak_count_ = c;   // #163 burst peak
    }
    std::size_t count(std::int64_t now_ns) noexcept {
        ring_.evict(now_ns - window_ns);
        return ring_.count();
    }
    double rate_per_sec(std::int64_t now_ns) noexcept {
        return static_cast<double>(count(now_ns)) * 1e9 / static_cast<double>(window_ns);
    }
    // peak_count / peak_rate_per_sec (#163): the highest count/rate in the window ever
    // observed — detects a burst (risk of overload) even if
    // the instantaneous rate is low.
    std::size_t peak_count()        const noexcept { return peak_count_; }
    double      peak_rate_per_sec() const noexcept {
        return static_cast<double>(peak_count_) * 1e9 / static_cast<double>(window_ns);
    }
    void reset() noexcept { ring_.reset(); peak_count_ = 0; }

private:
    TimestampRing<RING_SIZE> ring_;
    std::size_t  peak_count_ = 0;
};

struct GapRecovery {
    std::set<uint64_t> missing;          // known missing, awaiting retransmission
    uint64_t expected        = 0;
    uint64_t primary_packets = 0;        // observe() call count (#321)
    bool     initialized = false;
    uint64_t gap_events  = 0;            // how many separate gap events
    uint64_t recovered   = 0;            // how many missing recovered
    uint64_t duplicates  = 0;            // seq < expected and NOT a missing one

    // observe: primary feed. In order → OK; ahead → record a gap; behind
    // → either fills a known gap (recovered) or a pure duplicate.
    void observe(uint64_t seq) noexcept {
        ++primary_packets;   // #321: count every feed call regardless of outcome
        if (!initialized) { initialized = true; expected = seq + 1; return; }
        if (seq == expected) { ++expected; return; }
        if (seq > expected) {
            for (uint64_t s = expected; s < seq; ++s) missing.insert(s);
            ++gap_events;
            expected = seq + 1;
            return;
        }
        if (missing.erase(seq)) ++recovered;   // a late packet fills a gap
        else                    ++duplicates;
    }

    // on_retransmit: a packet from the recovery server / line B. It counts only when
    // it actually fills a known gap. Returns true when something was recovered.
    bool on_retransmit(uint64_t seq) noexcept {
        if (missing.erase(seq)) { ++recovered; return true; }
        return false;
    }

    // next_request: the [lo,hi] range for a gap-fill request covering the current
    // missing. Returns false when there are no gaps (the book is certain again).
    bool next_request(uint64_t& lo, uint64_t& hi) const noexcept {
        if (missing.empty()) return false;
        lo = *missing.begin();
        hi = *missing.rbegin();
        return true;
    }

    bool   has_gaps()      const noexcept { return !missing.empty(); }
    size_t missing_count() const noexcept { return missing.size(); }
    // duplicate_rate: fraction of primary feed packets that were true duplicates (#321)
    // = duplicates / primary_packets. A non-zero rate indicates a sender retransmitting
    // without being asked, or a mis-configured redundant line leaking onto this channel.
    // 0.0 when no packets have arrived.
    double duplicate_rate() const noexcept {
        return primary_packets > 0
            ? static_cast<double>(duplicates) / static_cast<double>(primary_packets)
            : 0.0;
    }

    // gap_event_rate (#475): how often a gap OPENS per primary packet
    // observed = gap_events / primary_packets. Distinct from duplicate_rate
    // (#321, dups per packet) and from avg_gap_burst (#329, SIZE per event):
    // this is the FREQUENCY of gap events normalized by traffic — the SLA
    // number, e.g. gaps-per-million-packets. A rising rate at a steady
    // burst size means the path is dropping more OFTEN (line quality),
    // where a rising avg_gap_burst at a steady rate means the drops got
    // BIGGER (buffer overflow). 0 before any packet.
    double gap_event_rate() const noexcept {
        return primary_packets > 0
            ? static_cast<double>(gap_events) / static_cast<double>(primary_packets)
            : 0.0;
    }

    // recovery_completeness (#156): the fraction of detected gaps already recovered
    // = recovered / (recovered + still_missing). 1.0 = nothing outstanding (the book
    // is certain), <1.0 = some gaps still open. A recovery-health metric.
    double recovery_completeness() const noexcept {
        const std::uint64_t total = recovered + missing.size();
        return total > 0 ? static_cast<double>(recovered) / static_cast<double>(total) : 1.0;
    }

    // avg_gap_burst: average number of CONSECUTIVE missing sequences per gap event
    // (#329) = (recovered + still-missing) / gap_events. Every sequence ever marked
    // missing is either recovered or still outstanding, so the numerator is the total
    // ever lost; the denominator is how many distinct gap EVENTS opened. ~1.0 = isolated
    // single-packet drops (random loss); >> 1 = bursty loss (a NIC/kernel buffer
    // overflow dropping a run of packets at once) — which points at a different fix
    // (bigger buffers vs. line quality). 0 when no gap has occurred.
    double avg_gap_burst() const noexcept {
        return gap_events > 0
            ? static_cast<double>(recovered + missing.size()) / static_cast<double>(gap_events)
            : 0.0;
    }

    // outstanding_range_count (#338): how many DISTINCT contiguous runs the
    // currently-missing sequences form — the number of separate gap-fill requests
    // still needed RIGHT NOW. missing_ranges() (#149) returns the actual [lo,hi]
    // pairs but allocates a vector; this counts them in a single pass over the
    // sorted set with no heap. Distinct from gap_events (a cumulative lifetime
    // counter that never decreases): this is the live, point-in-time fragmentation.
    // 0 when nothing is outstanding.
    std::size_t outstanding_range_count() const noexcept {
        std::size_t runs = 0;
        bool have_prev = false;
        std::uint64_t prev = 0;
        for (std::uint64_t s : missing) {        // std::set iterates ascending
            if (!have_prev || s != prev + 1) ++runs;
            prev = s; have_prev = true;
        }
        return runs;
    }

    // largest_outstanding_run (#338): the size (in sequences) of the BIGGEST
    // contiguous block of currently-missing numbers — the worst single hole in the
    // book right now, and the span one retransmit request would cover. A large run
    // points at a burst drop; many size-1 runs (see outstanding_range_count) point
    // at scattered single-packet loss. The live, point-in-time companion to
    // avg_gap_burst (#329, a historical mean over all gap events). 0 when nothing
    // is outstanding.
    std::uint64_t largest_outstanding_run() const noexcept {
        std::uint64_t best = 0, cur = 0, prev = 0;
        bool have_prev = false;
        for (std::uint64_t s : missing) {        // ascending
            if (have_prev && s == prev + 1) ++cur;
            else                            cur = 1;
            if (cur > best) best = cur;
            prev = s; have_prev = true;
        }
        return best;
    }

    // oldest_missing (#379): the LOWEST sequence still awaiting retransmission
    // (0 when there are no gaps). This is the head-of-line blocker: an
    // in-order consumer can release nothing at or above this hole.
    std::uint64_t oldest_missing() const noexcept {
        return missing.empty() ? 0 : *missing.begin();
    }

    // head_of_line_lag (#379): how far the in-order delivery point sits behind
    // the live edge because of the OLDEST outstanding hole: expected -
    // oldest_missing, 0 when fully contiguous. Depth companion to
    // outstanding_range_count (#338), which counts holes but not how stale
    // they are: a large lag with one range = one long-stalled retransmit
    // request; a small lag = only fresh gaps.
    std::uint64_t head_of_line_lag() const noexcept {
        return missing.empty() ? 0 : expected - *missing.begin();
    }

    // missing_ranges (#149): gaps grouped into CONTIGUOUS intervals [begin,end].
    // next_request gives only min..max (may include already-received); this gives
    // exact ranges for a gap-fill request (more efficient retransmission).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> missing_ranges() const {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        if (missing.empty()) return out;
        std::uint64_t start = *missing.begin(), prev = start;
        for (auto it = std::next(missing.begin()); it != missing.end(); ++it) {
            if (*it == prev + 1) { prev = *it; }
            else { out.emplace_back(start, prev); start = *it; prev = *it; }
        }
        out.emplace_back(start, prev);
        return out;
    }

    // recommend_snapshot (#115): when the gaps exceed a threshold, per-packet retransmission
    // is not worthwhile — a real feed handler requests a full SNAPSHOT and resyncs.
    bool recommend_snapshot(std::size_t threshold) const noexcept {
        return threshold > 0 && missing.size() >= threshold;
    }

    // snapshot_resync: after receiving a snapshot at seq `snapshot_seq`, everything
    // up to and including it is known — clears gaps <= snapshot_seq and sets
    // expected. Gaps above the snapshot remain (they will arrive in the normal stream).
    void snapshot_resync(std::uint64_t snapshot_seq) noexcept {
        for (auto it = missing.begin(); it != missing.end(); ) {
            if (*it <= snapshot_seq) { it = missing.erase(it); ++recovered; }
            else break;   // the set is sorted ascending — the rest is > snapshot_seq
        }
        if (snapshot_seq + 1 > expected) expected = snapshot_seq + 1;
        initialized = true;
    }

    void reset() noexcept { *this = GapRecovery{}; }
};


// ABLineArbitrator — arbitration of two redundant feed lines (expansion #91).
//
// Exchanges send market data on TWO identical lines (A and B) over the same
// UDP multicast. The receiver takes the packet from WHICHEVER line arrived first
// for a given sequence, and drops the duplicate from the other. If line A loses
// a packet, line B usually delivers it — the gap "self-heals" without a gap-fill
// request. This is standard feed resilience (NASDAQ/CME etc.).
//
// on_packet(seq, from_line_a) → true when the packet is NEW (forwarded),
// false when a duplicate (the other line already delivered this seq). Underneath,
// GapRecovery gives a unified view of gaps AFTER arbitration.
struct ABLineArbitrator {
    GapRecovery rec;          // state after merging both lines
    uint64_t a_first = 0;     // how many times line A delivered the seq first
    uint64_t b_first = 0;     // how many times line B
    uint64_t dups    = 0;     // dropped duplicates (the other line)

    bool on_packet(uint64_t seq, bool from_line_a) noexcept {
        // New = not yet consumed: at/before expected or fills a gap.
        const bool is_new = !rec.initialized
                          || seq >= rec.expected
                          || rec.missing.count(seq) != 0;
        if (!is_new) { ++dups; return false; }
        rec.observe(seq);                 // advance / recover (shared logic)
        if (from_line_a) ++a_first; else ++b_first;
        return true;
    }

    bool   has_gaps()      const noexcept { return rec.has_gaps(); }
    size_t missing_count() const noexcept { return rec.missing_count(); }

    // a_win_rate (#387): fraction of races line A delivered first, in [0,1].
    // A healthy dual-line setup sits near 0.5 (the lines race evenly); a rate
    // pinned near 1.0 or 0.0 means the losing line is consistently slower
    // (degraded path, switch/queue trouble) and the feed is one failure away
    // from running unprotected. 0.5 (neutral) before any race.
    double a_win_rate() const noexcept {
        const uint64_t total = a_first + b_first;
        return total > 0
            ? static_cast<double>(a_first) / static_cast<double>(total) : 0.5;
    }

    // lagging_line (#387): 'A' or 'B' when that line wins at most `threshold`
    // of the races (default 0.1), '-' when both are healthy or no traffic
    // yet. The actionable alarm on top of a_win_rate's raw number.
    char lagging_line(double threshold = 0.1) const noexcept {
        const uint64_t total = a_first + b_first;
        if (total == 0) return '-';
        const double a = static_cast<double>(a_first) / static_cast<double>(total);
        if (a <= threshold)       return 'A';
        if (1.0 - a <= threshold) return 'B';
        return '-';
    }

    // dup_rate (#387): dropped duplicates / all packets seen. Nearly every
    // packet of a healthy dual-line feed arrives twice, so this sits near
    // 0.5; well below 0.5 means the other line is NOT delivering at all
    // (dead line = zero redundancy). The inverse alarm to lagging_line,
    // which only sees a line while it still wins some races.
    double dup_rate() const noexcept {
        const uint64_t seen = a_first + b_first + dups;
        return seen > 0 ? static_cast<double>(dups) / static_cast<double>(seen) : 0.0;
    }

    void   reset()         noexcept { *this = ABLineArbitrator{}; }
};

// MultiChannelRecovery — gap recovery across MANY feed channels (expansion #122).
//
// Real exchanges split market data into separate multicast channels (e.g. NASDAQ by
// symbol range), each with its OWN sequence numbering. A single GapRecovery
// tracks one channel; this aggregator keeps one per channel_id and gives a
// combined view: whether there is a gap anywhere, how much in total is missing/recovered.
struct MultiChannelRecovery {
    std::unordered_map<std::uint32_t, GapRecovery> channels;

    void observe(std::uint32_t channel_id, std::uint64_t seq) {
        channels[channel_id].observe(seq);
    }
    bool on_retransmit(std::uint32_t channel_id, std::uint64_t seq) {
        return channels[channel_id].on_retransmit(seq);
    }

    bool any_gaps() const {
        for (const auto& kv : channels) if (kv.second.has_gaps()) return true;
        return false;
    }
    std::size_t total_missing() const {
        std::size_t n = 0;
        for (const auto& kv : channels) n += kv.second.missing_count();
        return n;
    }
    std::uint64_t total_recovered() const {
        std::uint64_t n = 0;
        for (const auto& kv : channels) n += kv.second.recovered;
        return n;
    }
    std::size_t channel_count() const noexcept { return channels.size(); }

    // channels_with_gaps (#395): how many channels currently have holes —
    // the BREADTH of the outage. One gapped channel = an isolated loss on
    // that stream; many at once = a shared upstream problem (NIC/kernel
    // buffer, switch) that per-channel retransmits will not fix.
    std::size_t channels_with_gaps() const {
        std::size_t n = 0;
        for (const auto& kv : channels) if (kv.second.has_gaps()) ++n;
        return n;
    }

    // worst_channel (#395): the channel with the MOST outstanding missing
    // sequences — the one to gap-fill or snapshot-resync FIRST. The
    // actionable WHICH behind total_missing's aggregate. Returns false and
    // leaves the out params untouched when no channel has gaps.
    bool worst_channel(std::uint32_t& out_channel, std::size_t& out_missing) const {
        std::size_t   worst = 0;
        std::uint32_t who   = 0;
        for (const auto& kv : channels) {
            const std::size_t m = kv.second.missing_count();
            if (m > worst) { worst = m; who = kv.first; }
        }
        if (worst == 0) return false;
        out_channel = who;
        out_missing = worst;
        return true;
    }
};


// InterArrivalMeter — statistics of gaps between messages (expansion #142).
//
// Rate alone (FeedRateMeter, #132) does not show JITTER — a feed may average
// 1M/s, but with bursts and holes. This meter tracks min/max/avg gap between
// consecutive messages: a large max with a small min = an uneven feed (risk
// of queueing / drop), important for a latency-sensitive consumer.
struct InterArrivalMeter {
    std::int64_t  last_ns = 0;
    bool          started = false;
    std::int64_t  min_gap = INT64_MAX;
    std::int64_t  max_gap = 0;
    std::int64_t  sum_gap = 0;
    std::uint64_t gaps    = 0;

    void on_message(std::int64_t now_ns) noexcept {
        if (started) {
            const std::int64_t g = now_ns - last_ns;
            // #555 (audit): a non-monotonic timestamp (reorder / clock skew)
            // produced a NEGATIVE gap that was accepted wholesale — min_gap
            // went negative, sum_gap shrank, and every mean/jitter/burst read
            // built on them (#142/#523) was corrupted. A backwards stamp is
            // not an arrival cadence; skip it AND keep the baseline where it
            // was (rolling last_ns backwards would double-count the span on
            // the next packet — the InterArrivalStats #305 mistake).
            if (g < 0) return;
            if (g < min_gap) min_gap = g;
            if (g > max_gap) max_gap = g;
            sum_gap += g;
            ++gaps;
        }
        last_ns = now_ns;
        started = true;
    }

    double       avg_gap_ns() const noexcept { return gaps ? static_cast<double>(sum_gap) / gaps : 0.0; }
    std::int64_t min_gap_ns() const noexcept { return gaps ? min_gap : 0; }
    std::int64_t max_gap_ns() const noexcept { return max_gap; }
    std::int64_t jitter_ns()  const noexcept { return gaps ? (max_gap - min_gap) : 0; }  // span
    // burst_ratio (#523): the worst inter-arrival gap as a MULTIPLE of the
    // average = max_gap / avg_gap. Where jitter_ns (#142) is the ABSOLUTE span
    // (max - min, in ns) — which reads large on a slow feed and small on a fast
    // one for the same relative burstiness — this is SCALE-FREE: 1.0 is a
    // perfectly even feed (every gap equals the mean), and a large value flags
    // a bursty stall (one long pause among otherwise tight arrivals) regardless
    // of the feed's base rate. It is >= 1 whenever any variation exists (max is
    // never below the mean). 0 before any gap, or when every gap is 0
    // (simultaneous arrivals — no timescale to normalize against).
    double burst_ratio() const noexcept {
        const double avg = avg_gap_ns();
        return avg > 0.0 ? static_cast<double>(max_gap) / avg : 0.0;
    }
    void reset() noexcept { *this = InterArrivalMeter{}; }
};


// DedupWindow — sequence deduplication (expansion #171).
//
// UDP can DUPLICATE packets (retransmissions, A/B line, multipath). The consumer
// must process each seq EXACTLY ONCE (at-most-once). DedupWindow remembers
// recently seen numbers in a sliding window and rejects duplicates. It differs from
// ReorderBuffer (ordering) and GapRecovery (gaps) — here it is about duplicates.
//
//   accept(seq) -> true when NEW (forward on), false when a duplicate.
struct DedupWindow {
    std::uint64_t window;
    std::uint64_t high = 0;
    bool          init = false;
    std::set<std::uint64_t> seen;
    std::uint64_t duplicates = 0;
    std::uint64_t accepted   = 0;   // packets passed through (#403)

    explicit DedupWindow(std::uint64_t window_ = 1024) noexcept
        : window(window_ ? window_ : 1) {}

    bool accept(std::uint64_t seq) {
        if (!init) { init = true; high = seq; seen.insert(seq); ++accepted; return true; }
        if (seq + window <= high) { ++duplicates; return false; }   // outside the window -> treat as a dup
        if (seen.count(seq))      { ++duplicates; return false; }
        seen.insert(seq);
        if (seq > high) high = seq;
        // Prune: forget numbers that fell out of the window (bounds memory).
        while (!seen.empty() && *seen.begin() + window <= high) seen.erase(seen.begin());
        ++accepted;
        return true;
    }

    // total_seen (#403): everything ever offered = passed + rejected.
    std::uint64_t total_seen() const noexcept { return accepted + duplicates; }

    // dup_rate (#403): rejected / offered, in [0,1] — the hygiene ratio
    // behind the raw duplicates counter. On a single clean line this sits
    // near 0; a step up means an upstream component started replaying
    // (a misconfigured retransmitter, two lines bridged onto one group —
    // the same pathology GapRecovery::duplicate_rate #321 flags AFTER
    // sequencing, caught here at the dedup stage instead). 0 before any
    // packet.
    double dup_rate() const noexcept {
        const std::uint64_t total = total_seen();
        return total > 0
            ? static_cast<double>(duplicates) / static_cast<double>(total)
            : 0.0;
    }

    void reset() noexcept { *this = DedupWindow{window}; }
};


// BackpressureMonitor — whether the consumer keeps up with the feed (expansion #179).
//
// The multicast feed runs at the market's steady rate; if the book-builder / consumer
// does not keep up, the queue grows and you trade on STALE data. The monitor counts
// enqueued vs processed, tracks depth and its peak, and flags
// overload past a threshold. Lightweight: two counters.
//
//   on_enqueue() when a packet is received, on_dequeue() after processing;
//   depth() = backlog, overloaded(threshold) -> time to drop/snapshot.
struct BackpressureMonitor {
    std::uint64_t enqueued = 0;
    std::uint64_t dequeued = 0;
    std::uint64_t peak_depth = 0;

    void on_enqueue(std::uint64_t n = 1) noexcept {
        enqueued += n;
        const std::uint64_t d = depth();
        if (d > peak_depth) peak_depth = d;
    }
    void on_dequeue(std::uint64_t n = 1) noexcept {
        dequeued += (n > depth() ? depth() : n);   // depth does not go below 0
    }
    std::uint64_t depth() const noexcept { return enqueued - dequeued; }
    bool overloaded(std::uint64_t threshold) const noexcept { return depth() >= threshold; }

    // drain_ratio (#491): the fraction of everything ever enqueued that the
    // consumer has processed = dequeued / enqueued, in [0,1]. depth() is the
    // instantaneous backlog; this is the cumulative keep-up — near 1 the
    // consumer is draining as fast as the feed fills, persistently below 1
    // means a chronic backlog that only grows. 1.0 before anything is
    // enqueued (nothing outstanding).
    double drain_ratio() const noexcept {
        return enqueued > 0
            ? static_cast<double>(dequeued) / static_cast<double>(enqueued)
            : 1.0;
    }

    // ever_overloaded (#491): whether the backlog EVER reached the threshold
    // = peak_depth >= threshold. overloaded() only sees the depth RIGHT NOW,
    // so a queue that spiked and drained looks healthy there while this
    // remembers the breach — the difference between a live alarm and a
    // post-incident review. threshold 0 is always true once anything queued.
    bool ever_overloaded(std::uint64_t threshold) const noexcept {
        return peak_depth >= threshold;
    }

    void reset() noexcept { enqueued = dequeued = peak_depth = 0; }
};


// LossRateMeter — aggregate packet-loss rate (expansion #187).
//
// GapRecovery tracks the SPECIFIC missing ranges (for retransmission); LossRateMeter
// gives one SLA number: what % of expected packets did not arrive in the whole session.
// Expected = the sequence span (highest - first + 1); received = a counter.
// Assumes no duplicates (DedupWindow first); duplicates would understate the loss.
//
//   on_packet(seq) for each received; loss_rate() for the dashboard/alert.
struct LossRateMeter {
    std::uint64_t first = 0;
    std::uint64_t highest = 0;
    std::uint64_t received = 0;
    bool          init = false;

    void on_packet(std::uint64_t seq) noexcept {
        if (!init) { first = highest = seq; init = true; }
        else if (seq > highest) highest = seq;
        ++received;
    }
    std::uint64_t expected() const noexcept { return init ? (highest - first + 1) : 0; }
    std::uint64_t lost() const noexcept {
        const std::uint64_t e = expected();
        return e > received ? e - received : 0;
    }
    double loss_rate() const noexcept {
        const std::uint64_t e = expected();
        return e ? static_cast<double>(lost()) / static_cast<double>(e) : 0.0;
    }
    void reset() noexcept { first = highest = received = 0; init = false; }
};


// OutOfOrderMeter — the fraction of out-of-order packets (expansion #195).
//
// Measures how many packets arrived with a LOWER seq than the running max (i.e. AFTER
// a packet with a higher number) — a symptom of path reordering (multipath, ECMP,
// queueing). It differs from ReorderBuffer (repairs ordering) and LossRateMeter
// (loss): this is pure network-quality DIAGNOSTICS. A high ooo_rate suggests
// a feed-routing problem, not the source.
struct OutOfOrderMeter {
    std::uint64_t highest = 0;
    std::uint64_t total = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t max_depth = 0;   // deepest backward jump seen (#370)
    std::uint64_t depth_sum = 0;   // Σ backward-jump depth over ooo packets (#467)
    bool          init = false;

    void on_packet(std::uint64_t seq) noexcept {
        ++total;
        if (!init) { highest = seq; init = true; return; }
        if (seq < highest) {
            ++out_of_order;   // arrived after a higher number
            const std::uint64_t depth = highest - seq;   // #370: how far behind
            depth_sum += depth;                          // #467
            if (depth > max_depth) max_depth = depth;
        } else {
            highest = seq;
        }
    }
    double ooo_rate() const noexcept {
        return total ? static_cast<double>(out_of_order) / static_cast<double>(total) : 0.0;
    }
    // max_reorder_depth: the DEEPEST backward jump (highest_seq - seq) among the
    // out-of-order packets (#370) — how far behind the running max a late packet
    // ever arrived. Where ooo_rate measures HOW OFTEN reordering happens, this
    // measures HOW SEVERE it gets: a reorder buffer must hold at least this many
    // sequence positions to repair the worst case. The backward-jump counterpart
    // of ContiguousTracker::max_lookahead (#354, the forward gap ahead of the
    // watermark). 0 when no packet has arrived out of order.
    std::uint64_t max_reorder_depth() const noexcept { return max_depth; }
    // avg_reorder_depth (#467): the MEAN backward-jump depth over the
    // out-of-order packets = depth_sum / out_of_order. max_reorder_depth
    // (#370) sizes the buffer for the worst case; this says whether
    // reordering is usually shallow (adjacent swaps, avg near 1 — benign
    // NIC/driver batching) or consistently deep (avg approaching the max —
    // a genuinely disordered path). A max far above a near-1 average is one
    // freak reorder; a max close to the average is systemic. 0 when no
    // packet arrived out of order.
    double avg_reorder_depth() const noexcept {
        return out_of_order
            ? static_cast<double>(depth_sum) / static_cast<double>(out_of_order)
            : 0.0;
    }
    void reset() noexcept {
        highest = total = out_of_order = max_depth = depth_sum = 0;
        init = false;
    }
};


// SequenceResetDetector — detects a feed sequence RESET (expansion #203).
//
// At the start of day / after a publisher restart the sequence number rolls back to a low
// value. The consumer MUST detect this, otherwise it treats the new low numbers
// as a gigantic gap (gap-recovery goes crazy) — instead it must CLEAR
// the book and re-snapshot from scratch. The threshold distinguishes a reset (a big drop) from ordinary
// reordering (a small drop): a reset when seq + threshold < last.
struct SequenceResetDetector {
    std::uint64_t threshold;
    std::uint64_t last = 0;
    bool          init = false;
    std::uint64_t resets = 0;

    explicit SequenceResetDetector(std::uint64_t threshold_ = 1000) noexcept
        : threshold(threshold_) {}

    // on_seq: returns true when a reset is detected (time to clear the book).
    bool on_seq(std::uint64_t seq) noexcept {
        if (!init) { last = seq; init = true; return false; }
        const bool reset = (seq + threshold < last);   // a big drop = reset, not reorder
        if (reset) { ++resets; last = seq; }            // accept the new base
        else if (seq > last) last = seq;                // normal progress; a reorder does not roll back
        return reset;
    }
    void reset_state() noexcept { last = 0; init = false; resets = 0; }
};


// SnapshotRequestThrottle — throttles snapshot requests (expansion #211).
//
// When the feed flickers, gap-recovery may fire again and again and flood the snapshot
// server with requests ("snapshot storm"), which makes things worse. The throttle enforces
// a MINIMUM interval between requests: allow() returns true only when enough
// time has passed since the last one, otherwise false (counted as suppressed). Independent of
// the gap-detection logic — pure rate control.
struct SnapshotRequestThrottle {
    std::int64_t min_interval_ns;
    std::int64_t last_request_ns = 0;
    bool          requested = false;
    std::uint64_t suppressed = 0;
    std::uint64_t allowed    = 0;   // requests that went through (#563)

    explicit SnapshotRequestThrottle(std::int64_t min_interval_ns_ = 1'000'000) noexcept
        : min_interval_ns(min_interval_ns_) {}

    // allow: whether a snapshot request may be sent now.
    bool allow(std::int64_t now_ns) noexcept {
        if (!requested || now_ns - last_request_ns >= min_interval_ns) {
            last_request_ns = now_ns;
            requested = true;
            ++allowed;   // #563
            return true;
        }
        ++suppressed;
        return false;
    }

    // suppression_rate (#563): the fraction of snapshot attempts the throttle
    // swallowed = suppressed / (allowed + suppressed), in [0,1]. The FREQUENCY
    // face of the raw suppressed counter, same shape as TokenBucket's
    // denial_rate (#539): near 0 the recovery logic asks politely and the
    // throttle is a no-op; a high rate is the snapshot-storm signature the
    // throttle exists to contain — gap detection re-firing far faster than the
    // interval, which points upstream (a flapping line, an aggressive retry
    // loop), not at the throttle. 0 before any attempt.
    double suppression_rate() const noexcept {
        const std::uint64_t attempts = allowed + suppressed;
        return attempts > 0
            ? static_cast<double>(suppressed) / static_cast<double>(attempts)
            : 0.0;
    }
    void reset() noexcept { last_request_ns = 0; requested = false; suppressed = 0; allowed = 0; }
};


// TokenBucket — a rate limiter with bursts (expansion #219).
//
// A classic token bucket for PACING outgoing requests (retransmit-request,
// order rate, snapshot-request). At any moment you have up to `capacity` tokens;
// they refill at the rate `refill_per_sec`. try_consume() takes a token if
// one is available. It differs from SnapshotRequestThrottle (a fixed min-interval): the bucket ALLOWS
// a short burst up to capacity, and only throttles under a sustained too-high rate.
struct TokenBucket {
    double        capacity;
    double        tokens;
    double        refill_per_ns;
    std::int64_t  last_ns = 0;
    bool          init = false;
    std::uint64_t denied = 0;      // refused consumptions (#451)
    std::uint64_t granted = 0;     // successful consumptions (#539)

    TokenBucket(double capacity_, double refill_per_sec) noexcept
        : capacity(capacity_), tokens(capacity_),
          refill_per_ns(refill_per_sec / 1e9) {}

    // try_consume: whether n tokens can be taken now. First it tops up by elapsed
    // time (cap to capacity), then consumes if there is enough.
    bool try_consume(std::int64_t now_ns, double n = 1.0) noexcept {
        top_up(now_ns);
        if (tokens >= n) { tokens -= n; ++granted; return true; }   // #539
        ++denied;                                     // #451
        return false;
    }

    // denial_rate (#539): the fraction of consumption attempts the bucket
    // refused = denied / (granted + denied), in [0,1]. The FREQUENCY face of
    // the raw denied counter (#451): near 0 the producer runs inside its
    // budget and the bucket is a no-op; a rising rate means the demand
    // SUSTAINEDLY exceeds the refill rate (bursts alone are absorbed by
    // capacity, so persistent denials are a sizing problem — raise the rate or
    // slow the producer). The same shape as OMS submit_reject_rate (#212) and
    // router reject_rate (#162), applied to pacing. 0 before any attempt.
    double denial_rate() const noexcept {
        const std::uint64_t attempts = granted + denied;
        return attempts > 0
            ? static_cast<double>(denied) / static_cast<double>(attempts)
            : 0.0;
    }

    // available (#451): the token balance RIGHT NOW — the probe a pacing
    // scheduler asks before committing (the multicast twin of risk's
    // rate_limit_headroom #429). Tops up by elapsed time exactly as the
    // next try_consume would, but takes nothing; probing is free. The
    // `denied` counter is the ops half: how often the bucket said no.
    double available(std::int64_t now_ns) noexcept {
        top_up(now_ns);
        return tokens;
    }

    void reset() noexcept { tokens = capacity; last_ns = 0; init = false; denied = 0; granted = 0; }

private:
    // top_up: shared refill math for try_consume and available (#451).
    void top_up(std::int64_t now_ns) noexcept {
        if (!init) { init = true; last_ns = now_ns; return; }
        tokens += static_cast<double>(now_ns - last_ns) * refill_per_ns;
        if (tokens > capacity) tokens = capacity;
        last_ns = now_ns;
    }
};


// ConflationBuffer — keeps only the LATEST value per key (expansion #227).
//
// When the consumer does not keep up, there is no point processing every intermediate
// price/book update — only the latest STATE matters. ConflationBuffer
// overwrites the value per key (e.g. symbol) and counts how many updates it THROTTLED
// (conflated). The consumer periodically drains the latest state. A classic technique
// under backpressure: instead of queueing indefinitely, jump to the freshest.
struct ConflationBuffer {
    std::map<std::uint64_t, double> latest;   // key -> latest value
    std::uint64_t conflated = 0;
    std::uint64_t received  = 0;              // every update offered (#427)
    std::size_t   peak_pending_ = 0;          // high-water of latest.size() (#515)

    void update(std::uint64_t key, double value) {
        ++received;
        auto [it, inserted] = latest.try_emplace(key, value);
        if (!inserted) { it->second = value; ++conflated; }   // overwrite = conflation
        if (latest.size() > peak_pending_) peak_pending_ = latest.size();   // #515
    }
    bool get(std::uint64_t key, double& out) const {
        const auto it = latest.find(key);
        if (it == latest.end()) return false;
        out = it->second;
        return true;
    }
    std::size_t pending() const noexcept { return latest.size(); }
    void drain() noexcept { latest.clear(); }   // the consumer took the latest state

    // peak_pending (#515): the high-water mark of pending() — the most DISTINCT
    // keys that ever piled up between two drains. pending() collapses to 0 on
    // drain(), so the burst that made the consumer fall furthest behind leaves
    // no trace there; this remembers it, deliberately SURVIVING drain() the way
    // SlidingWindowRate::peak_count (#507) survives its window aging out. The
    // sizing input for the drain buffer: conflation_ratio (#427) says what
    // FRACTION was absorbed, this says the worst absolute backlog of instruments
    // the consumer had to catch up on in one sweep. Counts distinct keys, so a
    // conflated overwrite (same key) never inflates it. 0 before any update.
    std::size_t peak_pending() const noexcept { return peak_pending_; }

    // conflation_ratio (#427): overwritten updates / all updates offered,
    // in [0,1) — how much of the feed the conflation absorbed before the
    // consumer drained. Near 0 the consumer sees (almost) every tick;
    // rising, it is reading an ever more SUMMARIZED market — each drained
    // value hides more intermediate prints. The keep-up gauge for a
    // conflated consumer, complementing BackpressureMonitor (#179), which
    // watches an UNCONFLATED queue's depth instead. 0 before any update.
    double conflation_ratio() const noexcept {
        return received > 0
            ? static_cast<double>(conflated) / static_cast<double>(received)
            : 0.0;
    }
};


// LatencyTracker — EWMA + peak processing latency (expansion #235).
//
// The feed handler wants to know how long HANDLING a packet takes (parse + book update).
// LatencyTracker combines two measures: a smoothed EWMA mean (trend, robust to
// single spikes) and max_ns (the worst case — that is what breaks the SLA). It differs
// from InterArrivalMeter (the gaps BETWEEN packets): here we measure the HANDLING cost.
struct LatencyTracker {
    double        alpha;
    double        ewma = 0.0;
    std::int64_t  max_ns = 0;
    std::int64_t  min_ns_val = 0;   // best case seen; valid once count > 0 (#483)
    std::uint64_t count = 0;

    explicit LatencyTracker(double alpha_ = 0.1) noexcept : alpha(alpha_) {}

    void sample(std::int64_t latency_ns) noexcept {
        if (count == 0) { ewma = static_cast<double>(latency_ns); min_ns_val = latency_ns; }
        else            ewma = alpha * static_cast<double>(latency_ns) + (1.0 - alpha) * ewma;
        if (latency_ns > max_ns) max_ns = latency_ns;
        if (latency_ns < min_ns_val) min_ns_val = latency_ns;   // #483
        ++count;
    }
    double       avg_ns()  const noexcept { return ewma; }     // smoothed mean
    std::int64_t peak_ns() const noexcept { return max_ns; }   // the worst case
    // min_ns (#483): the FASTEST handling seen — the floor set by the
    // parse+book-update fast path with warm caches. peak_ns is what breaks
    // the SLA; a min far below the EWMA mean means the average is dragged by
    // occasional slow paths (cache misses, GC-like stalls), where a min
    // close to the mean means the cost is uniform. 0 before any sample.
    std::int64_t min_ns()  const noexcept { return count ? min_ns_val : 0; }
    // jitter_ns (#483): peak - min, the full spread of handling cost. Tight
    // = predictable per-packet work; wide = the tail the reorder/latency
    // budget must absorb. Completes the min/mean/max envelope the way
    // InterArrivalStats (#305) has it for arrival cadence.
    std::int64_t jitter_ns() const noexcept { return count ? max_ns - min_ns_val : 0; }
    void reset() noexcept { ewma = 0.0; max_ns = 0; min_ns_val = 0; count = 0; }
};


// ContiguousTracker — the highest CONTIGUOUS seq (expansion #243).
//
// The consumer can safely act only on data below which there are NO gaps
// (e.g. publish the book, compute P&L). ContiguousTracker maintains a "cumulative
// ack": the highest number such that everything below has arrived. Out-of-order
// packets land in a buffer and are pulled in when the gap fills. Unlike
// GapRecovery (which tracks WHICH seq are missing) — here one number: "certain up to here".
struct ContiguousTracker {
    std::uint64_t next_expected;        // the first not-yet-delivered seq
    std::set<std::uint64_t> ahead;      // received prematurely (above the gap)

    explicit ContiguousTracker(std::uint64_t start = 1) noexcept : next_expected(start) {}

    void receive(std::uint64_t seq) {
        if (seq < next_expected) return;          // duplicate / old — ignore
        if (seq == next_expected) {
            ++next_expected;
            while (ahead.count(next_expected)) { ahead.erase(next_expected); ++next_expected; }
        } else {
            const std::uint64_t dist = seq - next_expected;   // #354
            if (dist > max_lookahead_) max_lookahead_ = dist;
            ahead.insert(seq);                    // gap below — set aside
        }
    }
    // contiguous_high: the last seq with no gap below (0 when nothing contiguous yet).
    std::uint64_t contiguous_high() const noexcept { return next_expected - 1; }
    std::size_t   buffered() const noexcept { return ahead.size(); }
    // max_lookahead: the largest distance, in sequence numbers, between the
    // contiguous_high watermark AT THE TIME and any out-of-order packet buffered
    // ahead of it (#354) — a high-water mark, unlike buffered() (how many are
    // CURRENTLY waiting). A wide sustained lookahead means the reorder buffer
    // needs deep capacity, not just patience. 0 when nothing was ever buffered
    // ahead of a gap.
    std::uint64_t max_lookahead() const noexcept { return max_lookahead_; }
    // current_lookahead (#499): how far ahead of the gap the buffer reaches
    // RIGHT NOW = furthest-buffered - next_expected (0 when nothing is
    // buffered ahead). The live companion to max_lookahead's high-water
    // mark, in the same units: it rises as out-of-order packets pile up
    // above an open gap and collapses to 0 the instant the gap fills and
    // the buffer drains. Where max_lookahead sizes the buffer for the worst
    // case ever, this is the pressure on it at this moment — and it never
    // exceeds max_lookahead. buffered() counts the packets waiting; this is
    // the SPAN they cover.
    std::uint64_t current_lookahead() const noexcept {
        return ahead.empty() ? 0 : (*ahead.rbegin() - next_expected);
    }
    void reset(std::uint64_t start = 1) noexcept {
        next_expected = start; ahead.clear(); max_lookahead_ = 0;
    }

private:
    std::uint64_t max_lookahead_ = 0;
};


// SlidingWindowRate — event count over a moving time window (expansion #257).
//
// FeedRateMeter gives an instantaneous (inter-arrival) and peak rate; this gives
// the exact COUNT of events within the last `window_ns`, pruning timestamps that
// fall out of the window. Useful for windowed burst detection and capacity
// checks: "how many messages did I receive in the last millisecond?".
struct SlidingWindowRate {
    // Same TimestampRing ring as FeedRateMeter (common/ring_counter.hpp; no
    // heap, mask wrap, L1-resident). Bounded at RING_SIZE-1 events in the
    // window; on overflow the oldest are dropped and count() saturates —
    // acceptable for windowed burst/capacity checks.
    static constexpr int RING_SIZE = 4096;

    std::int64_t window_ns;

    explicit SlidingWindowRate(std::int64_t window_ns_ = 1'000'000'000) noexcept
        : window_ns(window_ns_) {}

    void on_event(std::int64_t now_ns) noexcept {
        ring_.push(now_ns);
        ring_.evict(now_ns - window_ns);
        const std::size_t c = ring_.count();
        if (c > peak_count_) peak_count_ = c;   // #507: window high-water mark
    }
    std::size_t count() const noexcept { return ring_.count(); }   // events within the window
    double rate_per_sec() const noexcept {
        return window_ns > 0 ? static_cast<double>(count()) * 1e9 / static_cast<double>(window_ns) : 0.0;
    }
    // peak_count / peak_rate_per_sec (#507): the busiest the window ever got
    // — the high-water mark of count() over the session, the burst peak.
    // count() is the load RIGHT NOW (whatever is inside the sliding window);
    // this remembers the worst burst even after it aged out, exactly as
    // FeedRateMeter's peak (#163) does for its window. A sizing/alert input:
    // the transient spike that count() no longer shows is the one that
    // overran a buffer. 0 before any event.
    std::size_t peak_count() const noexcept { return peak_count_; }
    double peak_rate_per_sec() const noexcept {
        return window_ns > 0 ? static_cast<double>(peak_count_) * 1e9 / static_cast<double>(window_ns) : 0.0;
    }
    void reset() noexcept { ring_.reset(); peak_count_ = 0; }

private:
    std::size_t peak_count_ = 0;   // #507
    TimestampRing<RING_SIZE> ring_;
};


// RetransmitTracker — retransmit-request lifecycle (expansion #265).
//
// GapRecovery tells you WHICH sequence numbers are missing. This manages the
// REQUESTS for them: you ask the retransmission server (MoldUDP64 request / line
// B) to resend a gap; if the fill doesn't arrive within `timeout_ns` you retry,
// up to `max_attempts`, after which the gap is ESCALATED (give up on retransmit,
// fall back to a full snapshot). Without this a single lost request would leave a
// permanent hole. Backed by a std::map<seq, request-state>.
struct RetransmitTracker {
    std::int64_t timeout_ns;
    int          max_attempts;

    struct Req { std::int64_t last_sent_ns; int attempts; };
    std::map<std::uint64_t, Req> pending;
    std::uint64_t fulfilled = 0;   // retransmits that arrived
    std::uint64_t escalated = 0;   // gaps that exhausted retries -> snapshot

    explicit RetransmitTracker(std::int64_t timeout_ns_ = 1'000'000, int max_attempts_ = 3) noexcept
        : timeout_ns(timeout_ns_), max_attempts(max_attempts_) {}

    // request: register a gap-fill request for `seq` (no-op if already pending).
    void request(std::uint64_t seq, std::int64_t now_ns) {
        pending.try_emplace(seq, Req{now_ns, 1});
    }
    // on_received: a retransmitted packet for `seq` arrived — clear it.
    void on_received(std::uint64_t seq) {
        if (pending.erase(seq)) ++fulfilled;
    }
    // poll: process timeouts. For each pending request older than timeout_ns: if it
    // still has attempts left, bump the attempt + timestamp (caller RESENDS) and
    // count it; otherwise escalate (remove + ++escalated). Returns how many need a
    // resend right now.
    std::size_t poll(std::int64_t now_ns) {
        std::size_t retries = 0;
        for (auto it = pending.begin(); it != pending.end(); ) {
            if (now_ns - it->second.last_sent_ns >= timeout_ns) {
                if (it->second.attempts < max_attempts) {
                    ++it->second.attempts;
                    it->second.last_sent_ns = now_ns;
                    ++retries;
                    ++it;
                } else {
                    ++escalated;
                    it = pending.erase(it);
                }
            } else {
                ++it;
            }
        }
        return retries;
    }
    std::size_t outstanding() const noexcept { return pending.size(); }

    // escalation_rate (#419): the fraction of RESOLVED gap-fill requests
    // that exhausted their retries and escalated to a snapshot =
    // escalated / (fulfilled + escalated), in [0,1]. The health of the
    // recovery path itself: near 0 the retransmit server answers, a rising
    // rate means it is degraded or dead and every gap is taking the
    // expensive snapshot road instead. 0 before anything resolves.
    double escalation_rate() const noexcept {
        const std::uint64_t resolved = fulfilled + escalated;
        return resolved > 0
            ? static_cast<double>(escalated) / static_cast<double>(resolved)
            : 0.0;
    }

    // max_attempts_in_flight (#419): the highest attempt count among the
    // PENDING requests — how close the worst outstanding gap-fill is to
    // escalating (== max_attempts means one more timeout ends it). The
    // live early warning to escalation_rate's after-the-fact ratio.
    // 0 when nothing is pending.
    int max_attempts_in_flight() const noexcept {
        int mx = 0;
        for (const auto& kv : pending)
            if (kv.second.attempts > mx) mx = kv.second.attempts;
        return mx;
    }

    void reset() noexcept { pending.clear(); fulfilled = 0; escalated = 0; }
};


// DualFeedReconciler — A/B line first-seen dedup + line-quality stats (#273).
//
// Exchanges publish the SAME feed on two independent lines (A and B, often via
// different network paths / datacenters) for redundancy. A consumer takes
// whichever copy of each sequence arrives FIRST and drops the duplicate. On top of
// that, counting WHICH line won tells you which path is consistently faster — a
// real ops signal for choosing the primary line. (ABLineArbitrator does the
// packet-level take-first; this adds the win statistics.) `seen` would be windowed
// in production, like DedupWindow.
struct DualFeedReconciler {
    std::set<std::uint64_t> seen;       // sequence numbers already delivered
    std::uint64_t a_wins = 0;           // times line A delivered first
    std::uint64_t b_wins = 0;           // times line B delivered first
    std::uint64_t duplicates = 0;       // copies dropped (the other line won)

    // on_packet: line 0 = A, 1 = B. Returns true if this is the FIRST copy of `seq`
    // (deliver it downstream), false if the other line already delivered it (drop).
    bool on_packet(std::uint64_t seq, int line) {
        const auto res = seen.insert(seq);
        if (!res.second) { ++duplicates; return false; }   // already delivered
        if (line == 0) ++a_wins; else ++b_wins;
        return true;
    }
    // a_win_rate: fraction of unique packets that line A delivered first.
    double a_win_rate() const noexcept {
        const std::uint64_t tot = a_wins + b_wins;
        return tot ? static_cast<double>(a_wins) / static_cast<double>(tot) : 0.0;
    }
    void reset() noexcept { seen.clear(); a_wins = b_wins = duplicates = 0; }
};


// SnapshotSyncBuffer — snapshot + incremental join (expansion #281).
//
// The standard L2 recovery pattern (CME, most crypto venues): you request a book
// SNAPSHOT while the INCREMENTAL feed keeps streaming. Increments that arrive
// before the snapshot lands must be BUFFERED; once the snapshot (valid up to
// sequence S) is applied, you drop buffered increments with seq <= S (already
// reflected in the snapshot) and REPLAY the rest (S+1, S+2, ...) to reach a
// consistent live book. After that, increments apply directly.
struct SnapshotSyncBuffer {
    bool          applied = false;
    std::uint64_t snapshot_seq = 0;
    std::set<std::uint64_t> buffered;   // increments held while the snapshot loads
    std::uint64_t dropped = 0;          // increments already covered by the snapshot

    // on_increment: an incremental update with sequence `seq` arrived. Returns true
    // if the caller should apply it NOW (snapshot already in sync), false if it was
    // buffered to be replayed after the snapshot.
    bool on_increment(std::uint64_t seq) {
        if (applied) return true;       // live: apply directly
        buffered.insert(seq);
        return false;                   // buffered until the snapshot lands
    }
    // apply_snapshot: the snapshot valid up to `snap_seq` landed. Drop buffered
    // increments <= snap_seq (already in the snapshot), keep the rest. Returns the
    // number of buffered increments left to REPLAY.
    std::size_t apply_snapshot(std::uint64_t snap_seq) {
        snapshot_seq = snap_seq;
        applied = true;
        for (auto it = buffered.begin(); it != buffered.end(); ) {
            if (*it <= snap_seq) { ++dropped; it = buffered.erase(it); }
            else ++it;
        }
        return buffered.size();
    }
    std::size_t pending_replay() const noexcept { return buffered.size(); }

    // total_buffered (#459): every increment held during recovery = the
    // ones the snapshot already covered (dropped) plus the ones kept to
    // replay. Before apply_snapshot this is just the live buffer; after,
    // it is the full recovery workload.
    std::size_t total_buffered() const noexcept { return dropped + buffered.size(); }

    // snapshot_coverage (#459): the fraction of buffered increments the
    // snapshot already reflected = dropped / total_buffered, in [0,1]. The
    // FRESHNESS of the recovery snapshot: near 1 means it landed current
    // and almost nothing needs replaying; low means the snapshot lagged the
    // incremental feed and most of the buffer must be replayed to catch up
    // (a slow snapshot service, or a long recovery). 0 before apply_snapshot
    // (nothing dropped yet) and when no increment was buffered.
    double snapshot_coverage() const noexcept {
        const std::size_t total = total_buffered();
        return total > 0
            ? static_cast<double>(dropped) / static_cast<double>(total)
            : 0.0;
    }

    void reset() noexcept { applied = false; snapshot_seq = 0; buffered.clear(); dropped = 0; }
};


// FeedHealth — composite feed-health score (expansion #289).
//
// The various meters each measure ONE impairment (LossRateMeter #187, OutOfOrder
// Meter #195, FeedStalenessMonitor #98). For a failover / alert decision an
// operator wants ONE number. FeedHealth combines them into a 0-100 score: start at
// 100 and deduct weighted penalties for loss, reordering, and staleness. Decoupled
// from the meter structs — feed it the rates so it stays a pure, configurable
// scoring policy. is_healthy() applies a threshold for a go/no-go.
struct FeedHealth {
    double loss_weight   = 200.0;   // penalty per unit loss_rate (1.0 == 100% loss)
    double ooo_weight    = 100.0;   // penalty per unit out-of-order rate
    double stale_penalty = 50.0;    // flat deduction when the feed is stale
    double min_healthy   = 70.0;    // is_healthy threshold

    // score: 0..100 composite from loss rate, out-of-order rate, staleness flag.
    double score(double loss_rate, double ooo_rate, bool stale) const noexcept {
        double s = 100.0 - loss_rate * loss_weight - ooo_rate * ooo_weight;
        if (stale) s -= stale_penalty;
        return s < 0.0 ? 0.0 : (s > 100.0 ? 100.0 : s);
    }
    bool is_healthy(double loss_rate, double ooo_rate, bool stale) const noexcept {
        return score(loss_rate, ooo_rate, stale) >= min_healthy;
    }
    // worst_impairment (#531): names WHICH of the three penalties took the most
    // off score() — the actionable companion to the single composite number.
    // score() says HOW degraded the feed is; this says WHY, so an operator sees
    // "loss" vs "reorder" vs "stale" and knows what to chase instead of reading
    // three meters back. It compares the SAME weighted deductions score()
    // applies (loss_rate*loss_weight, ooo_rate*ooo_weight, and the flat
    // stale_penalty when stale), so a feed that scores badly on raw loss_rate
    // but weights reordering heavier can still name "reorder" as the true
    // culprit. Ties break loss > reorder > stale, the usual severity order.
    // Returns "none" when nothing is deducting (a clean feed). The failover-
    // triage analog of MultiChannelRecovery::worst_channel (#395).
    const char* worst_impairment(double loss_rate, double ooo_rate, bool stale) const noexcept {
        const double dl  = loss_rate * loss_weight;
        const double doo = ooo_rate * ooo_weight;
        const double ds  = stale ? stale_penalty : 0.0;
        if (dl <= 0.0 && doo <= 0.0 && ds <= 0.0) return "none";
        if (dl >= doo && dl >= ds) return "loss";
        if (doo >= ds)             return "reorder";
        return "stale";
    }
};


// GapFillTimer — recovery-SLA monitor (expansion #297).
//
// Detecting a gap (#82) and recovering it (snapshot/retransmit) takes TIME, and how
// long is a quality-of-feed SLA: a slow recovery means the book is stale for that
// long. GapFillTimer times each gap from detection to fill and reports count, mean
// and worst-case recovery — exactly the numbers an ops dashboard alerts on. Negative
// durations (clock skew / out-of-order timestamps) are ignored.
struct GapFillTimer {
    uint64_t gaps = 0;
    int64_t  total_recovery_ms = 0;
    int64_t  max_recovery_ms = 0;
    int64_t  min_recovery_ms_ = 0;   // best case seen; valid once gaps > 0 (#435)

    // record: a gap detected at detect_ms was filled at fill_ms.
    void record(int64_t detect_ms, int64_t fill_ms) noexcept {
        const int64_t dur = fill_ms - detect_ms;
        if (dur < 0) return;
        if (gaps == 0 || dur < min_recovery_ms_) min_recovery_ms_ = dur;   // #435
        ++gaps;
        total_recovery_ms += dur;
        if (dur > max_recovery_ms) max_recovery_ms = dur;
    }
    double avg_recovery_ms() const noexcept {
        return gaps > 0 ? static_cast<double>(total_recovery_ms) / static_cast<double>(gaps) : 0.0;
    }
    // min_recovery_ms (#435): the FASTEST gap-fill seen — completing the
    // min/mean/max envelope. The floor is set by the retransmit server's
    // round trip: a min far above the network RTT means even the best case
    // pays a queueing penalty, and the avg (#297) can never beat it. 0
    // before any recovery.
    int64_t min_recovery_ms() const noexcept { return gaps > 0 ? min_recovery_ms_ : 0; }
    // recovery_jitter_ms (#435): max - min, the recovery-time spread. Tight
    // = a predictable recovery path; wide = the retransmit service degrades
    // under load exactly when it is needed most.
    int64_t recovery_jitter_ms() const noexcept {
        return gaps > 0 ? max_recovery_ms - min_recovery_ms_ : 0;
    }
    void reset() noexcept {
        gaps = 0; total_recovery_ms = 0; max_recovery_ms = 0;
        min_recovery_ms_ = 0;   // #435
    }
};


// InterArrivalStats — feed jitter characterization (expansion #305).
//
// A healthy multicast feed delivers at a steady cadence; bursty or jittery arrivals
// stress the consumer's buffers and point to upstream congestion. InterArrivalStats
// records the gap between consecutive packet timestamps and reports min / mean / max
// — the jitter envelope (max - min). Distinct from LatencyTracker (#235), which
// measures per-message PROCESSING time, not arrival cadence. Non-monotonic
// timestamps (clock skew / reorder) are ignored.
struct InterArrivalStats {
    int64_t  last_ts = -1;
    uint64_t count = 0;          // number of gaps measured
    int64_t  total_gap = 0;
    int64_t  min_gap = 0;
    int64_t  max_gap = 0;
    int64_t  last_gap_ = 0;      // most recent inter-arrival gap (#362)

    void on_message(int64_t ts) noexcept {
        if (last_ts >= 0) {
            const int64_t gap = ts - last_ts;
            // #555 (audit): the negative gap was skipped, but last_ts was
            // STILL rolled backwards to the stale stamp — so the next packet
            // measured its gap from the past and double-counted the span
            // (on_message(100), (90) skipped, (110) read 20 where the feed
            // advanced 10). A non-monotonic stamp must not move the baseline.
            if (gap < 0) return;
            if (count == 0 || gap < min_gap) min_gap = gap;
            if (gap > max_gap) max_gap = gap;
            total_gap += gap;
            last_gap_ = gap;
            ++count;
        }
        last_ts = ts;
    }
    double mean_gap() const noexcept {
        return count > 0 ? static_cast<double>(total_gap) / static_cast<double>(count) : 0.0;
    }
    int64_t jitter() const noexcept { return count > 0 ? max_gap - min_gap : 0; }
    // last_gap: the MOST RECENT inter-arrival gap (#362) — the instantaneous
    // cadence, vs min/mean/max which summarize the whole session. Comparing
    // last_gap against mean_gap is a live burst/stall detector: last_gap >>
    // mean_gap right now signals the feed just went quiet (possible stall)
    // even though the aggregate looks healthy. 0 before the second message.
    int64_t last_gap() const noexcept { return last_gap_; }
    void reset() noexcept {
        last_ts = -1; count = 0; total_gap = 0; min_gap = 0; max_gap = 0; last_gap_ = 0;
    }
};


// PacketStats — wire-level accounting for a feed (expansion #313).
//
// The sequence-oriented structs (dedup, reorder, gap recovery) all reason about
// MESSAGE numbers; PacketStats is the orthogonal NETWORK view — how many packets and
// bytes arrived, the largest packet seen, and the mean size. These are the raw
// throughput numbers an ops dashboard charts (bytes/sec capacity, MTU pressure from
// max_bytes). No sequence logic — purely the wire.
struct PacketStats {
    std::uint64_t packets = 0;
    std::uint64_t total_bytes = 0;
    std::uint32_t max_bytes = 0;
    std::uint32_t min_bytes = 0;   // smallest packet seen this session (#346)

    void on_packet(std::uint32_t bytes) noexcept {
        ++packets;
        total_bytes += bytes;
        if (bytes > max_bytes) max_bytes = bytes;
        if (min_bytes == 0 || bytes < min_bytes) min_bytes = bytes;
    }
    double mean_bytes() const noexcept {
        return packets > 0 ? static_cast<double>(total_bytes) / static_cast<double>(packets) : 0.0;
    }
    // byte_range: max_bytes - min_bytes (#346). Complements mean_bytes with the
    // SPREAD of packet sizes on the wire — a wide range mixes heartbeats/acks
    // (tiny) with full snapshots (large) on one line; a narrow range around a
    // large mean suggests uniform, mostly-data traffic. 0 before any packet.
    std::uint32_t byte_range() const noexcept {
        return packets > 0 ? (max_bytes - min_bytes) : 0;
    }
    void reset() noexcept { packets = 0; total_bytes = 0; max_bytes = 0; min_bytes = 0; }
};


// FeedStalenessMonitor — detects a DEAD feed (expansion #98).
//
// Exchanges send heartbeats when there is no data exactly so the receiver can tell
// "a quiet market" from "the line is down" (NAT/firewall cuts idle UDP, a switch dies).
// No packet AT ALL (data OR heartbeat) for > timeout = stale →
// the feed handler should switch to the backup line / re-subscribe.
//
//   on_packet(now_ns)         — call on every received packet (resets the clock)
//   check(now_ns, timeout_ns) — true when the feed is stale; counts events (edge)
struct FeedStalenessMonitor {
    int64_t  last_ns      = 0;
    bool     seen         = false;   // whether we have seen the first packet
    bool     stale_       = false;
    uint64_t stale_events = 0;       // how many times the feed went stale (edges)
    int64_t  longest_outage_ns_ = 0; // worst completed silent window (#443)
    int64_t  total_outage_ns_   = 0; // Σ completed silent windows (#443)

    void on_packet(int64_t now_ns) noexcept {
        // #443: reviving from a DETECTED stale spell completes an outage.
        // The duration is measured from the LAST PACKET — the whole silent
        // window the consumer actually sat through — not from the moment
        // check() first noticed it.
        if (stale_ && seen) {
            const int64_t outage = now_ns - last_ns;
            if (outage > 0) {
                total_outage_ns_ += outage;
                if (outage > longest_outage_ns_) longest_outage_ns_ = outage;
            }
        }
        last_ns = now_ns;
        seen    = true;
        stale_  = false;             // a fresh packet revives the feed
    }

    bool check(int64_t now_ns, int64_t timeout_ns) noexcept {
        if (!seen) return false;     // not started yet — not "stale"
        const bool now_stale = (now_ns - last_ns) > timeout_ns;
        if (now_stale && !stale_) ++stale_events;   // entering the stale state
        stale_ = now_stale;
        return stale_;
    }

    bool is_stale() const noexcept { return stale_; }
    // longest_outage_ns / total_outage_ns (#443): the DURATION axis of feed
    // health — stale_events says how OFTEN the feed died, these say for how
    // LONG (worst single outage and the summed downtime an SLA reports).
    // Only COMPLETED outages count; a still-running one is visible live via
    // check(). 0 before any revival from a detected stale spell.
    int64_t longest_outage_ns() const noexcept { return longest_outage_ns_; }
    int64_t total_outage_ns()   const noexcept { return total_outage_ns_; }
    // completed_outages (#547): stale spells that ENDED in a revival — every
    // entering edge (stale_events) minus the spell still running, if any. The
    // correct denominator for the mean below: a spell that has not revived yet
    // has no duration in total_outage_ns_, so counting it would dilute the
    // average with a zero.
    uint64_t completed_outages() const noexcept {
        return stale_events - (stale_ ? 1 : 0);
    }
    // avg_outage_ns (#547): the MEAN completed silent window = total_outage_ns
    // / completed_outages — the typical-outage leg that completes the duration
    // family: stale_events is how OFTEN (#98), longest_outage_ns how BAD
    // (#443), total_outage_ns the summed downtime (#443), this the typical
    // case. A longest far above the average is one catastrophic drop amid
    // routine blips (chase the incident); longest ~ average means every outage
    // looks the same (chase the systemic cause — a periodic upstream stall).
    // 0 before any completed outage.
    double avg_outage_ns() const noexcept {
        const uint64_t done = completed_outages();
        return done > 0
            ? static_cast<double>(total_outage_ns_) / static_cast<double>(done)
            : 0.0;
    }
    void reset()    noexcept { *this = FeedStalenessMonitor{}; }
};

// ReorderBuffer — a packet reordering buffer (expansion #110).
//
// GapRecovery DETECTS and reconciles gaps, but does not RESTORE the ORDER of data — a packet
// that arrived too early (seq > expected) must be held until the
// missing ones before it arrive, otherwise the consumer (e.g. the book reconstructor) would see
// events out of order. ReorderBuffer buffers "future" packets and delivers
// them only when the gap closes — the outgoing stream is ALWAYS in order.
//
//   push(seq, val): seq==expected -> deliver + drain the next contiguous from the buffer
//                   seq> expected -> buffer it (waits for the missing ones)
//                   seq< expected -> duplicate, drop
// Delivered in order are loaded into `out` (the caller drains and clears).
template <typename T>
struct ReorderBuffer {
    std::uint64_t        expected = 0;
    bool                 initialized = false;
    std::map<std::uint64_t, T> pending;   // seq > expected, waiting
    std::vector<T>       out;             // delivered in order
    std::uint64_t        duplicates = 0;
    std::size_t          max_buffered = 0;   // deepest pending ever held (#411)

    void push(std::uint64_t seq, const T& val) {
        if (!initialized) { expected = seq; initialized = true; }
        if (seq < expected) { ++duplicates; return; }    // already delivered
        pending[seq] = val;
        // #411: record the high-water mark BEFORE the drain — the moment the
        // buffer is deepest is right after an out-of-order insert.
        if (pending.size() > max_buffered) max_buffered = pending.size();
        // Drain all contiguous from expected.
        auto it = pending.find(expected);
        while (it != pending.end()) {
            out.push_back(it->second);
            pending.erase(it);
            ++expected;
            it = pending.find(expected);
        }
    }

    bool   has_gap()       const noexcept { return !pending.empty(); }
    size_t buffered()      const noexcept { return pending.size(); }
    // max_buffered_depth (#411): the deepest the pending map ever got — the
    // number every PAYLOAD held while waiting on a gap. buffered() is the
    // depth right now (usually 0 after a drain); this remembers the worst
    // burst, which is the number that sizes the buffer memory and bounds
    // the drain latency spike when the missing packet finally lands. The
    // payload-map counterpart of ContiguousTracker::max_lookahead (#354,
    // sequence distance, no payloads).
    std::size_t max_buffered_depth() const noexcept { return max_buffered; }
    void   clear_out()     noexcept { out.clear(); }
    void   reset()         { *this = ReorderBuffer{}; }
};

}  // namespace multicast
