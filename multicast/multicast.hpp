#pragma once
/*
 * Multicast Market Data Feed — header-only C++.
 *
 * Binarna serializacja wiadomości dla kanałów danych rynkowych UDP multicast
 * + wykrywanie luk sekwencji (packet loss detection).
 *
 * Cechy:
 *   - binarny wire format (network byte order / big-endian), 40 bajtów/wiadomość
 *   - struktura wiadomości wyrównana do linii cache (alignas(64))
 *   - deserializacja zero-copy gdzie się da
 *   - znaczniki czasu w nanosekundach do pomiaru opóźnień
 *   - nakładki na gniazda UDP multicast (nadajnik/odbiornik)
 *   - SequenceTracker — wykrywanie packet loss / duplikatów / reorderingu
 *
 * Dlaczego sequence tracking jest KLUCZOWY:
 *   UDP jest UNRELIABLE — pakiety giną (przepełniony bufor NIC/kernela),
 *   przychodzą nie w kolejności, bywają zdublowane. Prawdziwa giełda numeruje
 *   każdą wiadomość monotonicznym sequence number WŁAŚNIE po to żeby odbiorca
 *   wykrył lukę. Po wykryciu luki feed handler robi recovery:
 *     - gap-fill request do serwera retransmisji (np. NASDAQ MoldUDP64), albo
 *     - A/B line arbitration — odbiera DWA identyczne feedy (linia A i B) i
 *       bierze pakiet który dotarł pierwszy, uzupełniając braki z drugiej linii.
 *   Bez sequence trackingu zgubiony Add Order = niewidzialne zlecenie w księdze
 *   = błędne ceny = realne straty. To nie jest opcjonalne w produkcji.
 *
 * Kompilacja:
 *   g++ -O2 -std=c++17 -o multicast/multicast_demo multicast/multicast_demo.cpp
 *
 * Uruchomienie:
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


static constexpr int64_t  MC_PRICE_SCALE  = 10000;
static constexpr uint16_t MC_DEFAULT_PORT = 5001;
static constexpr size_t   MC_MSG_SIZE     = 40;  // rozmiar wire format


// Helpery big-endian (network byte order). ITCH/multicast wymaga big-endian.
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


// Typy wiadomości.
enum class MsgType : char {
    ADD    = 'A',  // nowe zlecenie
    DELETE = 'D',  // anuluj zlecenie
    TRADE  = 'T',  // dopasowana transakcja
    UPDATE = 'U',  // aktualizacja zlecenia
    QUOTE  = 'Q',  // aktualizacja best bid/ask
    SYSTEM = 'S'   // zdarzenie systemowe
};

struct alignas(64) MarketDataMessage {
    uint64_t sequence;       // numer sekwencji (klucz do gap detection)
    uint64_t timestamp_ns;   // znacznik czasu wysłania (ns)
    char     symbol[8];      // ticker, dopełniony spacjami
    int64_t  price;          // cena stałoprzecinkowa (x10000)
    uint32_t quantity;       // ilość zlecenia
    char     side;           // 'B' kupno, 'S' sprzedaż
    char     msg_type;       // wartość enum MsgType
    char     padding[2];     // dopełnienie wyrównania
    // Łączny rozmiar wire: 40 bajtów
};


// Wire format (40 bajtów, network byte order):
//   [0..7]   sequence     (uint64 BE)
//   [8..15]  timestamp_ns (uint64 BE)
//   [16..23] symbol       (8 bajtów, dopełnione spacjami)
//   [24..31] price        (int64 BE)
//   [32..35] quantity     (uint32 BE)
//   [36]     side         (char)
//   [37]     msg_type     (char)
//   [38..39] padding      (wyzerowane)

namespace multicast {

// serialize: wiadomość → bufor w network byte order. Zwraca MC_MSG_SIZE (40).
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

// deserialize: bufor → wiadomość. Zwraca false gdy bufor za krótki.
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

// now_ns: aktualny znacznik czasu w nanosekundach.
inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// make_message: zbuduj wiadomość z aktualnym znacznikiem czasu.
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
    // Symbol dopełniony spacjami do 8 znaków.
    std::memset(msg.symbol, ' ', 8);
    const void* end = std::memchr(sym, '\0', 8);
    const size_t len = end ? static_cast<size_t>(static_cast<const char*>(end) - sym) : 8;
    std::memcpy(msg.symbol, sym, len);
    return msg;
}


// SequenceTracker — wykrywa luki (packet loss), duplikaty i reordering.
//
// Model: każda wiadomość ma monotoniczny sequence number. Tracker pamięta
// następny OCZEKIWANY numer i porównuje go z przychodzącym:
//   seq == expected  → OK, kolejny w kolejności
//   seq >  expected  → LUKA: zgubiliśmy (seq - expected) pakietów
//   seq <  expected  → DUPLIKAT / spóźniony (już przeszliśmy ten numer)
//
// Uwaga: prosta heurystyka "highest seq seen" — nie buforuje out-of-order
// pakietów (nie reasembluje). Po luce ustawiamy expected = seq+1 i jedziemy
// dalej; prawdziwy recovery (gap-fill / A-B arbitration) buforowałby i
// uzupełniał, ale to wykracza poza ten lab. Tu chodzi o DETEKCJĘ — która
// jest pierwszym krokiem każdego recovery.
struct SequenceTracker {
    enum class Status { OK, GAP, DUPLICATE };

    uint64_t expected_seq = 0;     // następny oczekiwany numer
    uint64_t received     = 0;     // ile pakietów zaakceptowanych (w kolejności + po luce)
    uint64_t gaps         = 0;     // ile osobnych zdarzeń luki
    uint64_t lost         = 0;     // łączna liczba zgubionych pakietów
    uint64_t duplicates   = 0;     // ile pakietów seq < expected
    bool     initialized  = false; // czy widzieliśmy pierwszy pakiet

    // observe: zgłoś sequence number przychodzącej wiadomości. Aktualizuje
    // statystyki i zwraca status tego pakietu.
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
        // seq < expected_seq → duplikat albo spóźniony (już go widzieliśmy).
        ++duplicates;
        return Status::DUPLICATE;
    }

    // loss_rate: ułamek zgubionych pakietów względem wszystkich które
    // POWINNY dotrzeć (received + lost). 0.0 = zero strat.
    double loss_rate() const noexcept {
        const uint64_t total = received + lost;
        return total > 0 ? static_cast<double>(lost) / static_cast<double>(total) : 0.0;
    }

    void reset() noexcept { *this = SequenceTracker{}; }
};


// MulticastSender — nakładka na gniazdo UDP multicast (nadajnik).
class MulticastSender {
    int fd_ = -1;
    struct sockaddr_in dest_{};

public:
    MulticastSender() = default;
    ~MulticastSender() { close(); }
    MulticastSender(const MulticastSender&) = delete;
    MulticastSender& operator=(const MulticastSender&) = delete;

    // init: gniazdo nadawcze dla danej grupy multicast i portu.
    bool init(const char* group, uint16_t port) noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        // TTL multicast = 2 (przejście przez jeden router).
        unsigned char ttl = 2;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        // Loopback włączony — nadajnik może odbierać własne wiadomości.
        unsigned char loop = 1;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        dest_.sin_family = AF_INET;
        dest_.sin_port   = htons(port);
        ::inet_aton(group, &dest_.sin_addr);
        return true;
    }

    // send: wyślij wiadomość przez multicast. Zwraca bajty lub -1.
    int send(const MarketDataMessage& msg) noexcept {
        uint8_t buf[MC_MSG_SIZE];
        serialize(msg, buf);
        return static_cast<int>(
            ::sendto(fd_, buf, MC_MSG_SIZE, 0,
                     reinterpret_cast<const struct sockaddr*>(&dest_),
                     sizeof(dest_)));
    }

    // send_raw: wyślij surowy zserializowany bufor.
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


// MulticastReceiver — nakładka na gniazdo UDP multicast (odbiornik)
// + wbudowany SequenceTracker auto-aktualizowany przy każdym receive().
class MulticastReceiver {
    int             fd_ = -1;
    SequenceTracker seq_;

public:
    MulticastReceiver() = default;
    ~MulticastReceiver() { close(); }
    MulticastReceiver(const MulticastReceiver&) = delete;
    MulticastReceiver& operator=(const MulticastReceiver&) = delete;

    // init: gniazdo odbiorcze + dołączenie do grupy multicast.
    bool init(const char* group, uint16_t port) noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        // Wielu odbiorców na tym samym porcie (niekrytyczne jeśli brak wsparcia).
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

        // Dołącz do grupy multicast.
        struct ip_mreq mreq{};
        if (::inet_aton(group, &mreq.imr_multiaddr) == 0) {  // 0 = zły adres
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

    // receive: odbierz wiadomość (blokuje do nadejścia danych). Aktualizuje
    // sequence tracker. Zwraca recv timestamp (ns) lub 0 na błąd.
    // Opcjonalny out_status zwraca status sekwencji tego pakietu (OK/GAP/DUP).
    uint64_t receive(MarketDataMessage& msg,
                     SequenceTracker::Status* out_status = nullptr) noexcept {
        uint8_t buf[MC_MSG_SIZE + 16];
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        uint64_t recv_ts = now_ns();
        if (n <= 0) return 0;  // błąd lub zamknięcie
        if (n < static_cast<ssize_t>(MC_MSG_SIZE)) return 0;
        if (!deserialize(buf, static_cast<size_t>(n), msg)) return 0;
        const SequenceTracker::Status st = seq_.observe(msg.sequence);
        if (out_status) *out_status = st;
        return recv_ts;
    }

    // set_timeout: timeout odbioru w ms. 0 = blokujący.
    bool set_timeout(int ms) noexcept {
        struct timeval tv{};
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    }

    // Dostęp do statystyk sekwencji (gaps, lost, duplicates, loss_rate).
    const SequenceTracker& sequence_tracker() const noexcept { return seq_; }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool is_open() const noexcept { return fd_ >= 0; }
};


// Statystyki opóźnień.
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
