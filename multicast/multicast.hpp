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

    // observe_packet: wariant MoldUDP64. Pakiet niesie sequence PIERWSZEJ
    // wiadomości i ile ich jest (count). Następny oczekiwany = seq + count.
    // Heartbeat (count=0) ma seq == next-expected → wykrywa lukę nawet gdy
    // feed jest idle (żadnych danych, ale numer się zgadza lub nie).
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
        // packet_seq < expected_seq — pakiet zaczyna się przed oczekiwanym.
        const uint64_t packet_end = packet_seq + count;  // exclusive
        if (packet_end <= expected_seq) {
            ++duplicates;
            return Status::DUPLICATE;          // w całości już widziany
        }
        // Częściowy overlap — część wiadomości jest nowa (rzadkie, ale realne
        // przy A/B line arbitration gdy linie się minimalnie rozjeżdżają).
        received    += (packet_end - expected_seq);
        expected_seq = packet_end;
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


// MoldUDP64 — przemysłowy standard transportu market data over UDP (NASDAQ).
//
// Pojedynczy UDP datagram (downstream packet) niesie WIELE wiadomości:
//   [0..9]   Session         10 bajtów ASCII (identyfikator sesji handlowej)
//   [10..17] Sequence Number uint64 BE — numer PIERWSZEJ wiadomości w pakiecie
//   [18..19] Message Count   uint16 BE — ile wiadomości w pakiecie
//   potem MessageCount bloków, każdy:
//     [+0..1] Message Length uint16 BE
//     [+2..]  Message Data   <length> bajtów
//
// Pakiety specjalne (po Message Count):
//   0       → heartbeat (utrzymuje sesję, pozwala wykryć lukę gdy feed idle)
//   0xFFFF  → end of session
//
// Numery wiadomości w pakiecie są kolejne: seq, seq+1, ..., seq+count-1.
// Następny oczekiwany = seq + count. To pozwala odbiorcy wykryć lukę nawet
// gdy zgubił CAŁY datagram (next_expected nie zgadza się z kolejnym seq).
//
// Tak działa NASDAQ TotalView-ITCH, BX, PSX i wiele globalnych giełd —
// batchowanie wielu wiadomości w jeden datagram amortyzuje narzut UDP/IP
// (28 bajtów nagłówków na pakiet) przy zachowaniu sekwencji per-wiadomość.

static constexpr size_t   MOLD_HEADER_SIZE    = 20;
static constexpr size_t   MOLD_SESSION_LEN    = 10;
static constexpr uint16_t MOLD_HEARTBEAT      = 0;
static constexpr uint16_t MOLD_END_OF_SESSION = 0xFFFF;

struct MoldUDP64Header {
    char     session[MOLD_SESSION_LEN];
    uint64_t sequence;
    uint16_t message_count;
};

// mold_write_header: zapisz 20-bajtowy nagłówek MoldUDP64.
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

// mold_read_header: odczytaj nagłówek. Zwraca false gdy bufor za krótki.
inline bool mold_read_header(const uint8_t* buf, size_t len, MoldUDP64Header& h) noexcept {
    if (len < MOLD_HEADER_SIZE) return false;
    std::memcpy(h.session, buf, MOLD_SESSION_LEN);
    h.sequence      = mc_endian::read_u64_be(buf + 10);
    h.message_count = mc_endian::read_u16_be(buf + 18);
    return true;
}

// mold_serialize_packet: zbuduj kompletny pakiet MoldUDP64 z `count` wiadomości
// (count=0 → heartbeat). Zwraca rozmiar pakietu lub 0 gdy bufor za mały.
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

// mold_parse_packet: parsuje pakiet MoldUDP64. Aktualizuje tracker na poziomie
// pakietu (jeśli != nullptr) i woła on_msg() dla każdej wiadomości danych.
// Zwraca message_count (>=0) lub -1 na błąd formatu. End-of-session i heartbeat
// zwracają 0 (brak wiadomości danych).
template <typename OnMessage>
inline int mold_parse_packet(const uint8_t* buf, size_t len, MoldUDP64Header& h,
                             SequenceTracker* tracker, OnMessage&& on_msg) noexcept {
    if (!mold_read_header(buf, len, h)) return -1;

    // End-of-session i heartbeat nie niosą wiadomości danych → count efektywny 0
    // dla trackera (EoS-seq i heartbeat-seq oba == next-expected).
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


// ArbitratedReceiver — A/B line arbitration (standard branżowy redundancji feedów).
//
// Prawdziwa giełda publikuje DWA identyczne strumienie (linia A i linia B,
// zwykle na różnych grupach multicast / VLAN'ach / NIC'ach). Odbiorca słucha
// obu i bierze pakiet który dotarł PIERWSZY; duplikaty z drugiej linii są
// odrzucane. Jeśli jedna linia zgubi pakiet, druga go (zwykle) dostarczy.
// To eliminuje pojedyncze punkty awarii i daje 99.9999% completeness bez
// retransmisji.
//
// Algorytm tutaj uproszczony do logiki — nie ownuje gniazd ani threadów.
// Caller pollu'je dwa receivery (poll/epoll na obu fd) i wywołuje
// observe(seq, count) z każdego pakietu który dostarczył. Tracker mówi czy
// pakiet jest FRESH (pierwszy raz widziany — przekazujemy dalej) czy
// DUPLICATE (już widzieliśmy z drugiej linii — drop).
//
// To ten sam algorytm co observe_packet w SequenceTracker, ale wystawiony
// jako osobna klasa-wrapper żeby pokazać intent. Wewnętrznie używa pojedynczego
// SequenceTrackera: pierwsze przyjście wygrywa, drugie jest "<= expected" → dup.
class ArbitratedReceiver {
    SequenceTracker tracker_;
    uint64_t        from_a_ = 0;   // ile pakietów przyszło najpierw z linii A
    uint64_t        from_b_ = 0;   // ile najpierw z linii B
    uint64_t        deduped_ = 0;  // ile odrzuconych jako duplikaty drugiej linii

public:
    enum class Line { A, B };
    enum class Result { FRESH, DUPLICATE, GAP };

    // observe: rozważ pakiet z linii A albo B (seq = sequence pierwszej
    // wiadomości w pakiecie, count = liczba wiadomości; dla MoldUDP64).
    // Zwraca FRESH gdy to pierwszy raz widzimy ten zakres (przekaż dalej),
    // DUPLICATE gdy druga linia już dostarczyła (odrzuć), GAP gdy zostało
    // wykryte zgubienie pakietu (przekaż dalej + można odpalić recovery).
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

    // failover_ratio: ile % FRESH pakietów przyszło tylko dzięki linii B
    // (gdy A się zgubiła). >0 oznacza że linia B faktycznie ratuje sytuację.
    double failover_ratio() const noexcept {
        const uint64_t total = from_a_ + from_b_;
        return total > 0 ? static_cast<double>(from_b_) / static_cast<double>(total) : 0.0;
    }
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
