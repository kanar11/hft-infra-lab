#pragma once
/*
 * Multicast Market Data Feed — header-only C++.
 *
 * Binary message serialization for UDP multicast market data channels
 * + sequence gap detection (packet loss detection).
 *
 * Features:
 *   - binary wire format (network byte order / big-endian), 40 bytes/message
 *   - message struct aligned to the cache line (alignas(64))
 *   - zero-copy deserialization where possible
 *   - nanosecond timestamps for latency measurement
 *   - UDP multicast socket wrappers (sender/receiver)
 *   - SequenceTracker — packet loss / duplicate / reorder detection
 *
 * Why sequence tracking is CRUCIAL:
 *   UDP is UNRELIABLE — packets are lost (overflowed NIC/kernel buffer),
 *   arrive out of order, sometimes get duplicated. A real exchange numbers
 *   every message with a monotonic sequence number EXACTLY so the receiver
 *   detects a gap. After detecting a gap the feed handler does recovery:
 *     - a gap-fill request to a retransmission server (e.g. NASDAQ MoldUDP64), or
 *     - A/B line arbitration — receives TWO identical feeds (line A and B) and
 *       takes the packet that arrived first, filling missing ones from the other line.
 *   Without sequence tracking a lost Add Order = an invisible order in the book
 *   = wrong prices = real losses. This is not optional in production.
 *
 * Compile:
 *   g++ -O2 -std=c++17 -o multicast/multicast_demo multicast/multicast_demo.cpp
 *
 * Run:
 *   ./multicast/multicast_demo 100000
 */

#include <cstdint>
#include <cstring>
#include <chrono>
#include <algorithm>

#include "gap_recovery.hpp"   // multicast::GapRecovery (recovery on top of SequenceTracker)

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>


static constexpr int64_t  MC_PRICE_SCALE  = 10000;
static constexpr uint16_t MC_DEFAULT_PORT = 5001;
static constexpr size_t   MC_MSG_SIZE     = 40;  // wire-format size


// Big-endian helpers (network byte order). ITCH/multicast requires big-endian.
namespace mc_endian {

inline void write_u16_be(uint8_t* buf, uint16_t val) noexcept {
    buf[0] = static_cast<uint8_t>(val >> 8);
    buf[1] = static_cast<uint8_t>(val);
}

inline void write_u32_be(uint8_t* buf, uint32_t val) noexcept {
    buf[0] = static_cast<uint8_t>(val >> 24);
    buf[1] = static_cast<uint8_t>(val >> 16);
    buf[2] = static_cast<uint8_t>(val >> 8);
    buf[3] = static_cast<uint8_t>(val);
}

inline void write_u64_be(uint8_t* buf, uint64_t val) noexcept {
    buf[0] = static_cast<uint8_t>(val >> 56);
    buf[1] = static_cast<uint8_t>(val >> 48);
    buf[2] = static_cast<uint8_t>(val >> 40);
    buf[3] = static_cast<uint8_t>(val >> 32);
    buf[4] = static_cast<uint8_t>(val >> 24);
    buf[5] = static_cast<uint8_t>(val >> 16);
    buf[6] = static_cast<uint8_t>(val >> 8);
    buf[7] = static_cast<uint8_t>(val);
}

inline uint16_t read_u16_be(const uint8_t* buf) noexcept {
    return (static_cast<uint16_t>(buf[0]) << 8) |
            static_cast<uint16_t>(buf[1]);
}

inline uint32_t read_u32_be(const uint8_t* buf) noexcept {
    return (static_cast<uint32_t>(buf[0]) << 24) |
           (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8)  |
            static_cast<uint32_t>(buf[3]);
}

inline uint64_t read_u64_be(const uint8_t* buf) noexcept {
    return (static_cast<uint64_t>(buf[0]) << 56) |
           (static_cast<uint64_t>(buf[1]) << 48) |
           (static_cast<uint64_t>(buf[2]) << 40) |
           (static_cast<uint64_t>(buf[3]) << 32) |
           (static_cast<uint64_t>(buf[4]) << 24) |
           (static_cast<uint64_t>(buf[5]) << 16) |
           (static_cast<uint64_t>(buf[6]) << 8)  |
            static_cast<uint64_t>(buf[7]);
}

} // namespace mc_endian


// Message types.
enum class MsgType : char {
    ADD    = 'A',  // a new order
    DELETE = 'D',  // cancel an order
    TRADE  = 'T',  // a matched trade
    UPDATE = 'U',  // an order update
    QUOTE  = 'Q',  // a best bid/ask update
    SYSTEM = 'S'   // a system event
};

struct alignas(64) MarketDataMessage {
    uint64_t sequence;       // sequence number (the key to gap detection)
    uint64_t timestamp_ns;   // send timestamp (ns)
    char     symbol[8];      // ticker, space-padded
    int64_t  price;          // fixed-point price (x10000)
    uint32_t quantity;       // order quantity
    char     side;           // 'B' buy, 'S' sell
    char     msg_type;       // MsgType enum value
    char     padding[2];     // alignment padding
    // Total wire size: 40 bytes
};


// Wire format (40 bytes, network byte order):
//   [0..7]   sequence     (uint64 BE)
//   [8..15]  timestamp_ns (uint64 BE)
//   [16..23] symbol       (8 bytes, space-padded)
//   [24..31] price        (int64 BE)
//   [32..35] quantity     (uint32 BE)
//   [36]     side         (char)
//   [37]     msg_type     (char)
//   [38..39] padding      (zeroed)

namespace multicast {

// serialize: message → buffer in network byte order. Returns MC_MSG_SIZE (40).
inline size_t serialize(const MarketDataMessage& msg, uint8_t* buf) noexcept {
    mc_endian::write_u64_be(buf + 0,  msg.sequence);
    mc_endian::write_u64_be(buf + 8,  msg.timestamp_ns);
    std::memcpy(buf + 16, msg.symbol, 8);
    mc_endian::write_u64_be(buf + 24, static_cast<uint64_t>(msg.price));
    mc_endian::write_u32_be(buf + 32, msg.quantity);
    buf[36] = msg.side;
    buf[37] = msg.msg_type;
    buf[38] = 0;
    buf[39] = 0;
    return MC_MSG_SIZE;
}

// deserialize: buffer → message. Returns false when the buffer is too short.
inline bool deserialize(const uint8_t* buf, size_t len, MarketDataMessage& msg) noexcept {
    if (len < MC_MSG_SIZE) return false;
    msg.sequence     = mc_endian::read_u64_be(buf + 0);
    msg.timestamp_ns = mc_endian::read_u64_be(buf + 8);
    std::memcpy(msg.symbol, buf + 16, 8);
    msg.price        = static_cast<int64_t>(mc_endian::read_u64_be(buf + 24));
    msg.quantity     = mc_endian::read_u32_be(buf + 32);
    msg.side         = static_cast<char>(buf[36]);
    msg.msg_type     = static_cast<char>(buf[37]);
    msg.padding[0]   = 0;
    msg.padding[1]   = 0;
    return true;
}

// now_ns: the current timestamp in nanoseconds.
inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// make_message: build a message with the current timestamp.
inline MarketDataMessage make_message(
    uint64_t seq,
    const char* sym,
    int64_t price,
    uint32_t qty,
    char side,
    MsgType type
) noexcept {
    MarketDataMessage msg{};
    msg.sequence     = seq;
    msg.timestamp_ns = now_ns();
    msg.price        = price;
    msg.quantity     = qty;
    msg.side         = side;
    msg.msg_type     = static_cast<char>(type);
    // Symbol space-padded to 8 chars.
    std::memset(msg.symbol, ' ', 8);
    const void* end = std::memchr(sym, '\0', 8);
    const size_t len = end ? static_cast<size_t>(static_cast<const char*>(end) - sym) : 8;
    std::memcpy(msg.symbol, sym, len);
    return msg;
}


// SequenceTracker — detects gaps (packet loss), duplicates and reordering.
//
// Model: each message has a monotonic sequence number. The tracker remembers
// the next EXPECTED number and compares it with the incoming one:
//   seq == expected  → OK, the next one in order
//   seq >  expected  → GAP: we lost (seq - expected) packets
//   seq <  expected  → DUPLICATE / late (we already passed this number)
//
// Note: a simple "highest seq seen" heuristic — it does not buffer out-of-order
// packets (no reassembly). After a gap we set expected = seq+1 and carry on;
// real recovery (gap-fill / A-B arbitration) would buffer and fill, but that is
// beyond this lab. Here it is about DETECTION — which is the first step of every recovery.
struct SequenceTracker {
    enum class Status { OK, GAP, DUPLICATE };

    uint64_t expected_seq = 0;     // the next expected number
    uint64_t received     = 0;     // how many packets accepted (in order + after a gap)
    uint64_t gaps         = 0;     // how many separate gap events
    uint64_t lost         = 0;     // total number of lost packets
    uint64_t duplicates   = 0;     // how many packets with seq < expected
    bool     initialized  = false; // whether we have seen the first packet

    // observe: report the sequence number of an incoming message. Updates
    // statistics and returns this packet's status.
    Status observe(uint64_t seq) noexcept {
        if (!initialized) {
            initialized  = true;
            expected_seq = seq + 1;
            ++received;
            return Status::OK;
        }
        if (seq == expected_seq) {
            ++expected_seq;
            ++received;
            return Status::OK;
        }
        if (seq > expected_seq) {
            ++gaps;
            lost        += (seq - expected_seq);
            expected_seq = seq + 1;
            ++received;
            return Status::GAP;
        }
        // seq < expected_seq → duplicate or late (we already saw it).
        ++duplicates;
        return Status::DUPLICATE;
    }

    // observe_packet: the MoldUDP64 variant. A packet carries the sequence of the FIRST
    // message and how many there are (count). Next expected = seq + count.
    // A heartbeat (count=0) has seq == next-expected → detects a gap even when
    // the feed is idle (no data, but the number matches or not).
    Status observe_packet(uint64_t packet_seq, uint16_t count) noexcept {
        if (!initialized) {
            initialized  = true;
            expected_seq = packet_seq + count;
            received    += count;
            return Status::OK;
        }
        if (packet_seq == expected_seq) {
            expected_seq += count;
            received     += count;
            return Status::OK;
        }
        if (packet_seq > expected_seq) {
            ++gaps;
            lost        += (packet_seq - expected_seq);
            expected_seq = packet_seq + count;
            received    += count;
            return Status::GAP;
        }
        // packet_seq < expected_seq — the packet starts before the expected one.
        const uint64_t packet_end = packet_seq + count;  // exclusive
        if (packet_end <= expected_seq) {
            ++duplicates;
            return Status::DUPLICATE;          // already seen in full
        }
        // Partial overlap — part of the message is new (rare, but real
        // with A/B line arbitration when the lines diverge slightly).
        received    += (packet_end - expected_seq);
        expected_seq = packet_end;
        ++duplicates;
        return Status::DUPLICATE;
    }

    // loss_rate: the fraction of lost packets relative to all that
    // SHOULD have arrived (received + lost). 0.0 = zero loss.
    double loss_rate() const noexcept {
        const uint64_t total = received + lost;
        return total > 0 ? static_cast<double>(lost) / static_cast<double>(total) : 0.0;
    }

    void reset() noexcept { *this = SequenceTracker{}; }
};

// GapRecovery (recovery on top of detection) is split into a separate header so it is
// usable/testable without socket headers and the global MsgType from this file.
// (Included at the end of the file — see the bottom of the namespace.)


// MoldUDP64 — the industrial standard for transporting market data over UDP (NASDAQ).
//
// A single UDP datagram (downstream packet) carries MANY messages:
//   [0..9]   Session         10 bytes ASCII (trading session identifier)
//   [10..17] Sequence Number uint64 BE — the number of the FIRST message in the packet
//   [18..19] Message Count   uint16 BE — how many messages in the packet
//   then MessageCount blocks, each:
//     [+0..1] Message Length uint16 BE
//     [+2..]  Message Data   <length> bytes
//
// Special packets (by Message Count):
//   0       → heartbeat (keeps the session alive, lets a gap be detected when the feed is idle)
//   0xFFFF  → end of session
//
// Message numbers within a packet are consecutive: seq, seq+1, ..., seq+count-1.
// Next expected = seq + count. This lets the receiver detect a gap even
// when it lost a WHOLE datagram (next_expected does not match the next seq).
//
// This is how NASDAQ TotalView-ITCH, BX, PSX and many global exchanges work —
// batching many messages into one datagram amortizes the UDP/IP overhead
// (28 bytes of headers per packet) while keeping per-message sequencing.

static constexpr size_t   MOLD_HEADER_SIZE    = 20;
static constexpr size_t   MOLD_SESSION_LEN    = 10;
static constexpr uint16_t MOLD_HEARTBEAT      = 0;
static constexpr uint16_t MOLD_END_OF_SESSION = 0xFFFF;

struct MoldUDP64Header {
    char     session[MOLD_SESSION_LEN];
    uint64_t sequence;
    uint16_t message_count;
};

// mold_write_header: write the 20-byte MoldUDP64 header.
inline size_t mold_write_header(uint8_t* buf, const char* session,
                                 uint64_t seq, uint16_t count) noexcept {
    std::memset(buf, ' ', MOLD_SESSION_LEN);
    const void* end = std::memchr(session, '\0', MOLD_SESSION_LEN);
    const size_t slen = end
        ? static_cast<size_t>(static_cast<const char*>(end) - session)
        : MOLD_SESSION_LEN;
    std::memcpy(buf, session, slen);
    mc_endian::write_u64_be(buf + 10, seq);
    mc_endian::write_u16_be(buf + 18, count);
    return MOLD_HEADER_SIZE;
}

// mold_read_header: read the header. Returns false when the buffer is too short.
inline bool mold_read_header(const uint8_t* buf, size_t len, MoldUDP64Header& h) noexcept {
    if (len < MOLD_HEADER_SIZE) return false;
    std::memcpy(h.session, buf, MOLD_SESSION_LEN);
    h.sequence      = mc_endian::read_u64_be(buf + 10);
    h.message_count = mc_endian::read_u16_be(buf + 18);
    return true;
}

// mold_serialize_packet: build a complete MoldUDP64 packet with `count` messages
// (count=0 → heartbeat). Returns the packet size or 0 if the buffer is too small.
inline size_t mold_serialize_packet(uint8_t* buf, size_t cap,
                                     const char* session, uint64_t first_seq,
                                     const MarketDataMessage* msgs, uint16_t count) noexcept {
    const size_t needed = MOLD_HEADER_SIZE
                        + static_cast<size_t>(count) * (2 + MC_MSG_SIZE);
    if (cap < needed) return 0;
    size_t off = mold_write_header(buf, session, first_seq, count);
    for (uint16_t i = 0; i < count; ++i) {
        mc_endian::write_u16_be(buf + off, static_cast<uint16_t>(MC_MSG_SIZE));
        off += 2;
        serialize(msgs[i], buf + off);
        off += MC_MSG_SIZE;
    }
    return off;
}

// mold_parse_packet: parses a MoldUDP64 packet. Updates the packet-level tracker
// (if != nullptr) and calls on_msg() for each data message.
// Returns message_count (>=0) or -1 on a format error. End-of-session and heartbeat
// return 0 (no data messages).
template <typename OnMessage>
inline int mold_parse_packet(const uint8_t* buf, size_t len, MoldUDP64Header& h,
                             SequenceTracker* tracker, OnMessage&& on_msg) noexcept {
    if (!mold_read_header(buf, len, h)) return -1;

    // End-of-session and heartbeat carry no data messages → effective count 0
    // for the tracker (EoS-seq and heartbeat-seq are both == next-expected).
    const bool is_control = (h.message_count == MOLD_HEARTBEAT ||
                             h.message_count == MOLD_END_OF_SESSION);
    if (tracker) tracker->observe_packet(h.sequence, is_control ? 0 : h.message_count);
    if (is_control) return 0;

    size_t off = MOLD_HEADER_SIZE;
    for (uint16_t i = 0; i < h.message_count; ++i) {
        if (off + 2 > len) return -1;
        const uint16_t mlen = mc_endian::read_u16_be(buf + off);
        off += 2;
        if (off + mlen > len) return -1;
        MarketDataMessage m{};
        if (mlen >= MC_MSG_SIZE && deserialize(buf + off, mlen, m)) on_msg(m);
        off += mlen;
    }
    return static_cast<int>(h.message_count);
}


// MulticastSender — a wrapper over a UDP multicast socket (sender).
class MulticastSender {
    int fd_ = -1;
    struct sockaddr_in dest_{};

public:
    MulticastSender() = default;
    ~MulticastSender() { close(); }
    MulticastSender(const MulticastSender&) = delete;
    MulticastSender& operator=(const MulticastSender&) = delete;

    // init: a sending socket for a given multicast group and port.
    bool init(const char* group, uint16_t port) noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        // multicast TTL = 2 (crosses one router).
        unsigned char ttl = 2;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        // Loopback enabled — the sender can receive its own messages.
        unsigned char loop = 1;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        dest_.sin_family = AF_INET;
        dest_.sin_port   = htons(port);
        ::inet_aton(group, &dest_.sin_addr);
        return true;
    }

    // send: send a message over multicast. Returns bytes or -1.
    int send(const MarketDataMessage& msg) noexcept {
        uint8_t buf[MC_MSG_SIZE];
        serialize(msg, buf);
        return static_cast<int>(
            ::sendto(fd_, buf, MC_MSG_SIZE, 0,
                     reinterpret_cast<const struct sockaddr*>(&dest_),
                     sizeof(dest_)));
    }

    // send_raw: send a raw serialized buffer.
    int send_raw(const uint8_t* buf, size_t len) noexcept {
        return static_cast<int>(
            ::sendto(fd_, buf, len, 0,
                     reinterpret_cast<const struct sockaddr*>(&dest_),
                     sizeof(dest_)));
    }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool is_open() const noexcept { return fd_ >= 0; }
};


// MulticastReceiver — a wrapper over a UDP multicast socket (receiver)
// + a built-in SequenceTracker auto-updated on every receive().
class MulticastReceiver {
    int             fd_ = -1;
    SequenceTracker seq_;

public:
    MulticastReceiver() = default;
    ~MulticastReceiver() { close(); }
    MulticastReceiver(const MulticastReceiver&) = delete;
    MulticastReceiver& operator=(const MulticastReceiver&) = delete;

    // init: a receiving socket + joining the multicast group.
    bool init(const char* group, uint16_t port) noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        // Multiple receivers on the same port (non-critical if unsupported).
        int reuse = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(fd_, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_); fd_ = -1;
            return false;
        }

        // Join the multicast group.
        struct ip_mreq mreq{};
        if (::inet_aton(group, &mreq.imr_multiaddr) == 0) {  // 0 = bad address
            ::close(fd_); fd_ = -1;
            return false;
        }
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            ::close(fd_); fd_ = -1;
            return false;
        }

        return true;
    }

    // receive: receive a message (blocks until data arrives). Updates the
    // sequence tracker. Returns the recv timestamp (ns) or 0 on error.
    // The optional out_status returns this packet's sequence status (OK/GAP/DUP).
    uint64_t receive(MarketDataMessage& msg,
                     SequenceTracker::Status* out_status = nullptr) noexcept {
        uint8_t buf[MC_MSG_SIZE + 16];
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        uint64_t recv_ts = now_ns();
        if (n <= 0) return 0;  // error or close
        if (n < static_cast<ssize_t>(MC_MSG_SIZE)) return 0;
        if (!deserialize(buf, static_cast<size_t>(n), msg)) return 0;
        const SequenceTracker::Status st = seq_.observe(msg.sequence);
        if (out_status) *out_status = st;
        return recv_ts;
    }

    // set_timeout: receive timeout in ms. 0 = blocking.
    bool set_timeout(int ms) noexcept {
        struct timeval tv{};
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    }

    // Access to sequence statistics (gaps, lost, duplicates, loss_rate).
    const SequenceTracker& sequence_tracker() const noexcept { return seq_; }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool is_open() const noexcept { return fd_ >= 0; }
};


// ArbitratedReceiver — A/B line arbitration (the industry standard for feed redundancy).
//
// A real exchange publishes TWO identical streams (line A and line B,
// usually on different multicast groups / VLANs / NICs). The receiver listens
// to both and takes the packet that arrived FIRST; duplicates from the other line are
// dropped. If one line loses a packet, the other (usually) delivers it.
// This eliminates single points of failure and gives 99.9999% completeness without
// retransmission.
//
// The algorithm here is simplified to the logic — it does not own sockets or threads.
// The caller polls two receivers (poll/epoll on both fds) and calls
// observe(seq, count) for each packet it delivered. The tracker says whether a
// packet is FRESH (seen for the first time — we forward it) or
// DUPLICATE (already seen from the other line — drop).
//
// It is the same algorithm as observe_packet in SequenceTracker, but exposed
// as a separate wrapper class to show intent. Internally it uses a single
// SequenceTracker: the first arrival wins, the second is "<= expected" → dup.
class ArbitratedReceiver {
    SequenceTracker tracker_;
    uint64_t        from_a_ = 0;   // how many packets arrived first from line A
    uint64_t        from_b_ = 0;   // how many first from line B
    uint64_t        deduped_ = 0;  // how many dropped as duplicates of the other line

public:
    enum class Line { A, B };
    enum class Result { FRESH, DUPLICATE, GAP };

    // observe: consider a packet from line A or B (seq = sequence of the first
    // message in the packet, count = number of messages; for MoldUDP64).
    // Returns FRESH when we see this range for the first time (forward it),
    // DUPLICATE when the other line already delivered it (drop), GAP when a
    // packet loss was detected (forward it + recovery can be triggered).
    Result observe(Line line, uint64_t packet_seq, uint16_t count) noexcept {
        const auto st = tracker_.observe_packet(packet_seq, count);
        switch (st) {
        case SequenceTracker::Status::OK:
            if (line == Line::A) ++from_a_; else ++from_b_;
            return Result::FRESH;
        case SequenceTracker::Status::DUPLICATE:
            ++deduped_;
            return Result::DUPLICATE;
        case SequenceTracker::Status::GAP:
            if (line == Line::A) ++from_a_; else ++from_b_;
            return Result::GAP;
        }
        return Result::DUPLICATE;
    }

    const SequenceTracker& tracker()  const noexcept { return tracker_; }
    uint64_t fresh_from_a() const noexcept { return from_a_; }
    uint64_t fresh_from_b() const noexcept { return from_b_; }
    uint64_t deduped()      const noexcept { return deduped_; }

    // failover_ratio: what % of FRESH packets arrived only thanks to line B
    // (when A was lost). >0 means line B actually saves the day.
    double failover_ratio() const noexcept {
        const uint64_t total = from_a_ + from_b_;
        return total > 0 ? static_cast<double>(from_b_) / static_cast<double>(total) : 0.0;
    }
};


// Latency statistics.
struct LatencyStats {
    uint64_t count     = 0;
    uint64_t sum_ns    = 0;
    uint64_t min_ns    = UINT64_MAX;
    uint64_t max_ns    = 0;

    void record(uint64_t latency_ns) noexcept {
        ++count;
        sum_ns += latency_ns;
        if (latency_ns < min_ns) min_ns = latency_ns;
        if (latency_ns > max_ns) max_ns = latency_ns;
    }

    uint64_t avg_ns() const noexcept {
        return count > 0 ? sum_ns / count : 0;
    }
};

} // namespace multicast
