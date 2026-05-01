#pragma once
/**
 * Multicast Market Data Feed — C++ header-only implementation.
 * Kanał danych rynkowych Multicast — implementacja C++ header-only.
 *
 * Binary message serialization for UDP multicast market data feeds.
 * Binarna serializacja wiadomości dla kanałów danych rynkowych UDP multicast.
 *
 * Features / Cechy:
 * - Binary wire format (network byte order / big-endian)
 *   Binarny format sieciowy (network byte order / big-endian)
 * - Cache-line aligned message struct (alignas(64))
 *   Struktura wiadomości wyrównana do linii cache
 * - Zero-copy deserialization where possible
 *   Deserializacja zero-copy gdzie to możliwe
 * - Nanosecond timestamps for latency measurement
 *   Znaczniki czasu w nanosekundach do pomiaru opóźnień
 * - UDP multicast sender/receiver wrappers
 *   Nakładki na nadajnik/odbiornik UDP multicast
 *
 * Compile / Kompilacja:
 *   g++ -O2 -std=c++17 -o multicast/multicast_demo multicast/multicast_demo.cpp
 *
 * Run / Uruchomienie:
 *   ./multicast/multicast_demo 100000
 */

#include <cstdint>
#include <cstring>
#include <chrono>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ============================================================
// Constants / Stałe
// ============================================================

static constexpr int64_t  MC_PRICE_SCALE  = 10000;
static constexpr uint16_t MC_DEFAULT_PORT = 5001;
static constexpr size_t   MC_MSG_SIZE     = 40;  // Wire format size / Rozmiar formatu sieciowego

// ============================================================
// Big-endian helpers / Helpery big-endian
// ============================================================

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

// ============================================================
// MarketDataMessage — binary message struct
// Struktura binarnej wiadomości danych rynkowych
// ============================================================

// Message types / Typy wiadomości
enum class MsgType : char {
    ADD    = 'A',  // New order / Nowe zlecenie
    DELETE = 'D',  // Cancel order / Anuluj zlecenie
    TRADE  = 'T',  // Matched trade / Dopasowana transakcja
    UPDATE = 'U',  // Order update / Aktualizacja zlecenia
    QUOTE  = 'Q',  // Best bid/ask update / Aktualizacja najlepszej oferty
    SYSTEM = 'S'   // System event / Zdarzenie systemowe
};

struct alignas(64) MarketDataMessage {
    uint64_t sequence;       // Message sequence number / Numer sekwencji wiadomości
    uint64_t timestamp_ns;   // Send timestamp in nanoseconds / Znacznik czasu wysłania (ns)
    char     symbol[8];      // Ticker symbol, space-padded / Symbol akcji, dopełniony spacjami
    int64_t  price;          // Fixed-point price (x10000) / Cena stałoprzecinkowa (x10000)
    uint32_t quantity;       // Order quantity / Ilość zlecenia
    char     side;           // 'B' buy, 'S' sell / 'B' kupno, 'S' sprzedaż
    char     msg_type;       // MsgType enum value / Wartość enum MsgType
    char     padding[2];     // Alignment padding / Dopełnienie wyrównania
    // Total wire size: 40 bytes / Łączny rozmiar sieciowy: 40 bajtów
};

// ============================================================
// Serialization / Serializacja
// Wire format (40 bytes, network byte order):
//   [0..7]   sequence     (uint64 BE)
//   [8..15]  timestamp_ns (uint64 BE)
//   [16..23] symbol       (8 bytes, space-padded)
//   [24..31] price        (int64 BE)
//   [32..35] quantity     (uint32 BE)
//   [36]     side         (char)
//   [37]     msg_type     (char)
//   [38..39] padding      (zeroed)
// ============================================================

namespace multicast {

/**
 * Serialize message to network byte order buffer.
 * Serializuje wiadomość do bufora w network byte order.
 * Returns wire size (always MC_MSG_SIZE = 40).
 */
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

/**
 * Deserialize message from network byte order buffer.
 * Deserializuje wiadomość z bufora w network byte order.
 * Returns true if buffer is large enough.
 */
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

/**
 * Get current timestamp in nanoseconds.
 * Pobierz aktualny znacznik czasu w nanosekundach.
 */
inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

/**
 * Build a market data message with current timestamp.
 * Buduje wiadomość danych rynkowych z aktualnym znacznikiem czasu.
 */
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
    // Copy symbol, space-pad / Kopiuj symbol, dopełnij spacjami
    std::memset(msg.symbol, ' ', 8);
    std::memcpy(msg.symbol, sym, std::strnlen(sym, 8));
    return msg;
}

// ============================================================
// MulticastSender — UDP multicast socket wrapper
// Nakładka na gniazdo UDP multicast (nadajnik)
// ============================================================

class MulticastSender {
    int fd_ = -1;
    struct sockaddr_in dest_{};

public:
    MulticastSender() = default;
    ~MulticastSender() { close(); }
    MulticastSender(const MulticastSender&) = delete;
    MulticastSender& operator=(const MulticastSender&) = delete;

    /**
     * Initialize sender socket for given multicast group and port.
     * Inicjalizuje gniazdo nadawcze dla danej grupy multicast i portu.
     */
    bool init(const char* group, uint16_t port) noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        // Set multicast TTL to 2 (cross one router)
        // Ustaw TTL multicast na 2 (przez jeden router)
        unsigned char ttl = 2;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        // Enable loopback so sender can receive own messages
        // Włącz loopback aby nadajnik mógł odbierać własne wiadomości
        unsigned char loop = 1;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        dest_.sin_family = AF_INET;
        dest_.sin_port   = htons(port);
        ::inet_aton(group, &dest_.sin_addr);
        return true;
    }

    /**
     * Send a market data message over multicast.
     * Wysyła wiadomość danych rynkowych przez multicast.
     * Returns bytes sent or -1 on error.
     */
    int send(const MarketDataMessage& msg) noexcept {
        uint8_t buf[MC_MSG_SIZE];
        serialize(msg, buf);
        return static_cast<int>(
            ::sendto(fd_, buf, MC_MSG_SIZE, 0,
                     reinterpret_cast<const struct sockaddr*>(&dest_),
                     sizeof(dest_)));
    }

    /**
     * Send raw serialized buffer.
     * Wysyła surowy zserializowany bufor.
     */
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

// ============================================================
// MulticastReceiver — UDP multicast socket wrapper
// Nakładka na gniazdo UDP multicast (odbiornik)
// ============================================================

class MulticastReceiver {
    int fd_ = -1;

public:
    MulticastReceiver() = default;
    ~MulticastReceiver() { close(); }
    MulticastReceiver(const MulticastReceiver&) = delete;
    MulticastReceiver& operator=(const MulticastReceiver&) = delete;

    /**
     * Initialize receiver socket, join multicast group.
     * Inicjalizuje gniazdo odbiorcze, dołącza do grupy multicast.
     */
    bool init(const char* group, uint16_t port) noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        // Allow multiple receivers on same port (non-fatal if unsupported)
        // Pozwól na wielu odbiorców na tym samym porcie (niekrytyczne)
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

        // Join multicast group / Dołącz do grupy multicast
        struct ip_mreq mreq{};
        if (::inet_aton(group, &mreq.imr_multiaddr) == 0) {
            // inet_aton returns 0 on invalid address
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

    /**
     * Receive a market data message. Blocks until data arrives.
     * Odbiera wiadomość danych rynkowych. Blokuje do nadejścia danych.
     * Returns receive timestamp in nanoseconds, or 0 on error.
     */
    uint64_t receive(MarketDataMessage& msg) noexcept {
        uint8_t buf[MC_MSG_SIZE + 16];
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        uint64_t recv_ts = now_ns();
        if (n <= 0) return 0;  // error or connection closed
        if (n < static_cast<ssize_t>(MC_MSG_SIZE)) return 0;
        if (!deserialize(buf, static_cast<size_t>(n), msg)) return 0;
        return recv_ts;
    }

    /**
     * Set receive timeout in milliseconds. 0 = blocking.
     * Ustaw timeout odbioru w milisekundach. 0 = blokujący.
     */
    bool set_timeout(int ms) noexcept {
        struct timeval tv{};
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool is_open() const noexcept { return fd_ >= 0; }
};

// ============================================================
// Latency statistics / Statystyki opóźnień
// ============================================================

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
