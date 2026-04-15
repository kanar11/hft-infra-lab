/*
 * NASDAQ ITCH 5.0 Binary Protocol Parser — C++ Implementation
 * Parser protokołu binarnego NASDAQ ITCH 5.0 — implementacja C++
 *
 * This is the C++ counterpart of itch_parser.py.
 * To jest odpowiednik itch_parser.py w C++.
 *
 * WHY C++ HERE?
 * In real HFT systems, market data parsing is on the critical path:
 * every nanosecond saved here means better prices and more profit.
 * C++ compiles to native machine code with zero overhead.
 *
 * DLACZEGO C++ TUTAJ?
 * W prawdziwych systemach HFT parsowanie danych rynkowych jest na krytycznej
 * ścieżce — każda zaoszczędzona nanosekunda to lepsze ceny i większy zysk.
 *
 * Performance comparison / Porównanie wydajności:
 *   Python ITCH parser:  ~1-2 million messages/sec
 *   C++ ITCH parser:    ~50-100 million messages/sec  (50x faster)
 *
 * Protocol: NASDAQ ITCH 5.0 — binary, big-endian, fixed-length messages
 * Protokół: NASDAQ ITCH 5.0 — binarny, big-endian, wiadomości stałej długości
 *
 * Message sizes (bytes) / Rozmiary wiadomości (bajtów):
 *   A = ADD_ORDER       34 bytes
 *   D = DELETE_ORDER    17 bytes
 *   U = REPLACE_ORDER   33 bytes
 *   E = ORDER_EXECUTED  29 bytes
 *   C = ORDER_CANCELLED 21 bytes
 *   P = TRADE           42 bytes
 *   S = SYSTEM_EVENT    10 bytes
 *   R = STOCK_DIRECTORY 17 bytes
 */

#pragma once

// #pragma once: include this file only once even if #included multiple times
// #pragma once: dołącz ten plik tylko raz nawet jeśli jest #includowany wielokrotnie

// Standard C++ headers / Standardowe nagłówki C++
#include <cstdint>   // uint8_t, uint32_t, int64_t — fixed-size integer types
                     // typy całkowite o stałym rozmiarze
#include <cstring>   // memcpy — copy raw bytes / kopiowanie surowych bajtów
#include <string>    // std::string — text string / ciąg znaków
#include <endian.h>  // be64toh, be32toh — convert big-endian bytes to CPU's native order
                     // konwersja bajtów big-endian na format natywny CPU

// ─────────────────────────────────────────────
// BYTE-ORDER HELPERS / POMOCNIKI KOLEJNOŚCI BAJTÓW
// ─────────────────────────────────────────────

// ITCH uses big-endian (most significant byte first).
// ITCH używa big-endian (najbardziej znaczący bajt pierwszy).
// Intel/AMD CPUs use little-endian, so we must swap bytes.
// Procesory Intel/AMD używają little-endian, więc musimy zamieniać bajty.
//
// We use memcpy() before swapping to avoid "unaligned access" crashes.
// Używamy memcpy() przed zamianą, aby uniknąć błędów "unaligned access".
// (Data in a packet stream is not guaranteed to be aligned to word boundaries)
// (Dane w strumieniu pakietów nie są gwarantowane na granicach słów)

// Read a big-endian 64-bit integer from a raw byte pointer
// Odczytaj 64-bitową liczbę całkowitą big-endian ze wskaźnika na surowe bajty
static inline int64_t read_be64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);          // copy 8 bytes into v safely / bezpiecznie skopiuj 8 bajtów do v
    return (int64_t)be64toh(v); // swap byte order if needed / zamień kolejność bajtów jeśli potrzeba
}

// Read a big-endian 32-bit unsigned integer
// Odczytaj 32-bitową nieznakową liczbę całkowitą big-endian
static inline uint32_t read_be32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return be32toh(v);
}

// ─────────────────────────────────────────────
// MESSAGE STRUCTS / STRUKTURY WIADOMOŚCI
// ─────────────────────────────────────────────
//
// A 'struct' in C++ is like a class but all members are public by default.
// 'struct' w C++ jest jak klasa, ale wszystkie składowe są domyślnie publiczne.
// Think of it like a row in a database table — a fixed set of fields.
// Pomyśl o tym jak o wierszu w tabeli bazy danych — stały zestaw pól.

// ADD_ORDER message: someone placed a new limit order in the book
// Wiadomość ADD_ORDER: ktoś złożył nowe zlecenie z limitem ceny
struct AddOrderMsg {
    int64_t  timestamp_ns;  // nanoseconds since midnight / nanosekundy od północy
    int64_t  order_ref;     // unique order ID (like PID for processes) / unikalny ID zlecenia
    char     side;          // 'B' = BUY / KUP,  'S' = SELL / SPRZEDAJ
    uint32_t shares;        // number of shares / liczba akcji
    char     stock[9];      // stock symbol e.g. "AAPL\0" (8 chars + null) / symbol akcji
    double   price;         // price in dollars e.g. 150.25 / cena w dolarach
};

// DELETE_ORDER message: an order was cancelled/removed from the book
// Wiadomość DELETE_ORDER: zlecenie zostało anulowane/usunięte z booka
struct DeleteOrderMsg {
    int64_t timestamp_ns;
    int64_t order_ref;      // which order to remove / które zlecenie usunąć
};

// REPLACE_ORDER message: modify an existing order (new price/size)
// Wiadomość REPLACE_ORDER: modyfikuj istniejące zlecenie (nowa cena/rozmiar)
struct ReplaceOrderMsg {
    int64_t  timestamp_ns;
    int64_t  orig_order_ref; // original order being replaced / oryginalne zastępowane zlecenie
    int64_t  new_order_ref;  // new order reference / nowa referencja zlecenia
    uint32_t new_shares;     // updated quantity / zaktualizowana ilość
    double   new_price;      // updated price / zaktualizowana cena
};

// ORDER_EXECUTED message: a fill happened — shares traded
// Wiadomość ORDER_EXECUTED: realizacja — akcje zostały sprzedane/kupione
struct OrderExecutedMsg {
    int64_t  timestamp_ns;
    int64_t  order_ref;      // which order was filled / które zlecenie zostało zrealizowane
    uint32_t exec_shares;    // how many shares traded / ile akcji zostało skonsumowanych
    int64_t  match_number;   // unique trade ID / unikalny ID transakcji
};

// ORDER_CANCELLED message: partial cancellation (reduces shares remaining)
// Wiadomość ORDER_CANCELLED: częściowe anulowanie (zmniejsza pozostałą ilość)
struct OrderCancelledMsg {
    int64_t  timestamp_ns;
    int64_t  order_ref;
    uint32_t cancelled_shares; // how many shares removed / ile akcji usunięto
};

// TRADE message: a matched trade (both sides)
// Wiadomość TRADE: dopasowana transakcja (obie strony)
struct TradeMsg {
    int64_t  timestamp_ns;
    int64_t  order_ref;
    char     side;
    uint32_t shares;
    char     stock[9];
    double   price;
    int64_t  match_number;
};

// SYSTEM_EVENT message: market open/close/halted signals
// Wiadomość SYSTEM_EVENT: sygnały otwarcia/zamknięcia/wstrzymania rynku
struct SystemEventMsg {
    int64_t timestamp_ns;
    char    event_code; // 'O'=open, 'C'=close, 'H'=halt / 'O'=otwarcie, 'C'=zamknięcie
};

// STOCK_DIRECTORY message: stock metadata (symbol info)
// Wiadomość STOCK_DIRECTORY: metadane akcji (informacje o symbolu)
struct StockDirectoryMsg {
    int64_t timestamp_ns;
    char    stock[9];
};

// ─────────────────────────────────────────────
// RESULT ENUM / ENUM WYNIKU
// ─────────────────────────────────────────────

// enum class: a type-safe set of named constants
// enum class: bezpieczny typowo zestaw nazwanych stałych
// Better than plain integers (0,1,2...) because the names are self-documenting
// Lepszy niż zwykłe liczby (0,1,2...) bo nazwy dokumentują się same
enum class MsgType {
    ADD_ORDER,
    DELETE_ORDER,
    REPLACE_ORDER,
    ORDER_EXECUTED,
    ORDER_CANCELLED,
    TRADE,
    SYSTEM_EVENT,
    STOCK_DIRECTORY,
    UNKNOWN,
    ERROR
};

// ParsedMessage: a tagged union holding ONE of the 8 message types
// ParsedMessage: oznaczona unia przechowująca JEDEN z 8 typów wiadomości
//
// A 'union' shares the same memory for multiple types (only one is valid at a time)
// 'union' dzieli tę samą pamięć dla wielu typów (tylko jeden jest ważny naraz)
// Like a /proc file — same interface, different content depending on what you read
// Jak plik /proc — ten sam interfejs, różna zawartość zależnie od tego co czytasz
struct ParsedMessage {
    MsgType type;  // tells you WHICH field in 'data' is valid / mówi które pole w 'data' jest ważne

    // union: all fields share the same memory block
    // union: wszystkie pola dzielą ten sam blok pamięci
    union {
        AddOrderMsg       add_order;
        DeleteOrderMsg    delete_order;
        ReplaceOrderMsg   replace_order;
        OrderExecutedMsg  order_executed;
        OrderCancelledMsg order_cancelled;
        TradeMsg          trade;
        SystemEventMsg    system_event;
        StockDirectoryMsg stock_directory;
    } data;
};

// ─────────────────────────────────────────────
// PARSER CLASS / KLASA PARSERA
// ─────────────────────────────────────────────

class ITCHParser {
public:
    // Statistics tracked during parsing / Statystyki śledzone podczas parsowania
    // (public struct inside a class — members accessible from outside)
    // (publiczna struktura wewnątrz klasy — dostępna z zewnątrz)
    struct Stats {
        uint64_t total_parsed   = 0;
        uint64_t add_orders     = 0;
        uint64_t delete_orders  = 0;
        uint64_t replace_orders = 0;
        uint64_t executions     = 0;
        uint64_t cancels        = 0;
        uint64_t trades         = 0;
        uint64_t system_events  = 0;
        uint64_t unknowns       = 0;
    };

    // Constructor: called when ITCHParser is created, initialises stats to zero
    // Konstruktor: wywoływany przy tworzeniu ITCHParser, inicjalizuje statystyki do zera
    // 'noexcept' means this function will never throw an exception (faster)
    // 'noexcept' oznacza że ta funkcja nigdy nie rzuci wyjątku (szybciej)
    ITCHParser() noexcept = default;

    // parse(): main entry point — reads one raw ITCH message and returns its type + data
    // parse(): główny punkt wejścia — odczytuje jedną surową wiadomość ITCH i zwraca jej typ + dane
    //
    // Parameters / Parametry:
    //   data — pointer to raw bytes (the network packet / surowe bajty pakietu sieciowego)
    //   len  — number of bytes available / dostępna liczba bajtów
    // Returns / Zwraca:
    //   ParsedMessage with .type and .data filled in
    //   ParsedMessage z wypełnionym .type i .data
    //
    // 'const uint8_t*' means: pointer to bytes that we won't modify (read-only)
    // 'const uint8_t*' oznacza: wskaźnik do bajtów, których nie zmodyfikujemy (tylko do odczytu)
    ParsedMessage parse(const uint8_t* data, size_t len) noexcept {
        ParsedMessage result{};
        result.type = MsgType::ERROR;

        // Need at least 1 byte to read the message type
        // Potrzebujemy co najmniej 1 bajtu żeby odczytać typ wiadomości
        if (len < 1) return result;

        // First byte is always the message type character ('A', 'D', 'U', etc.)
        // Pierwszy bajt to zawsze znak typu wiadomości ('A', 'D', 'U' itp.)
        uint8_t msg_type = data[0];
        stats_.total_parsed++;

        // Switch statement: like a chain of if/elif, but compiled to a jump table (faster)
        // Instrukcja switch: jak łańcuch if/elif, ale skompilowana do tablicy skoków (szybciej)
        switch (msg_type) {
            case 'A': result = parse_add_order(data, len);       stats_.add_orders++;     break;
            case 'D': result = parse_delete_order(data, len);    stats_.delete_orders++;  break;
            case 'U': result = parse_replace_order(data, len);   stats_.replace_orders++; break;
            case 'E': result = parse_order_executed(data, len);  stats_.executions++;     break;
            case 'C': result = parse_order_cancelled(data, len); stats_.cancels++;        break;
            case 'P': result = parse_trade(data, len);           stats_.trades++;         break;
            case 'S': result = parse_system_event(data, len);    stats_.system_events++;  break;
            case 'R': result = parse_stock_directory(data, len); break;
            default:
                result.type = MsgType::UNKNOWN;
                stats_.unknowns++;
        }
        return result;
    }

    // const Stats& means: return a reference to stats (no copy), and don't allow modification
    // const Stats& oznacza: zwróć referencję do statystyk (bez kopiowania), bez możliwości modyfikacji
    const Stats& stats() const noexcept { return stats_; }

    // Reset counters (e.g., at start of new trading day)
    // Zresetuj liczniki (np. na początku nowego dnia handlowego)
    void reset_stats() noexcept { stats_ = Stats{}; }

private:
    // Private member: only accessible from inside this class (like a local variable for the object)
    // Prywatna składowa: dostępna tylko z wnętrza tej klasy
    // The trailing underscore is a naming convention for private member variables
    // Podkreślenie na końcu to konwencja nazewnictwa dla prywatnych zmiennych składowych
    Stats stats_;

    // ── Individual message parsers ────────────────────────────────────────
    // inline: hint to compiler to expand this code in-place instead of calling it
    // inline: wskazówka dla kompilatora żeby rozwinął ten kod w miejscu zamiast go wywoływać
    // (eliminates function call overhead — every nanosecond matters in HFT)
    // (eliminuje narzut wywołania funkcji — każda nanosekunda ma znaczenie w HFT)

    static inline ParsedMessage parse_add_order(const uint8_t* d, size_t len) noexcept {
        // ADD_ORDER layout (34 bytes total):
        // Układ ADD_ORDER (łącznie 34 bajty):
        // [0]    = msg_type  'A'     (1 byte)
        // [1..8] = timestamp_ns      (8 bytes, big-endian int64)
        // [9..16]= order_ref         (8 bytes, big-endian int64)
        // [17]   = side 'B' or 'S'  (1 byte)
        // [18..21]= shares           (4 bytes, big-endian uint32)
        // [22..29]= stock symbol     (8 bytes, ASCII text e.g. "AAPL    ")
        // [30..33]= price * 10000    (4 bytes, big-endian uint32)
        ParsedMessage m{};
        if (len < 34) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::ADD_ORDER;
        auto& msg = m.data.add_order;

        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        msg.side         = (char)d[17];            // 'B' or 'S' / 'B' lub 'S'
        msg.shares       = read_be32(d + 18);
        memcpy(msg.stock, d + 22, 8);              // copy 8 bytes of stock name / skopiuj 8 bajtów nazwy akcji
        msg.stock[8]     = '\0';                    // null-terminate the string / zakończ ciąg znakiem null
        msg.price        = read_be32(d + 30) / 10000.0; // convert fixed-point to float / konwertuj stałoprzecinkowy na zmiennoprzecinkowy
        return m;
    }

    static inline ParsedMessage parse_delete_order(const uint8_t* d, size_t len) noexcept {
        // DELETE_ORDER layout (17 bytes):
        // [0]    = msg_type 'D'
        // [1..8] = timestamp_ns
        // [9..16]= order_ref
        ParsedMessage m{};
        if (len < 17) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::DELETE_ORDER;
        auto& msg = m.data.delete_order;
        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        return m;
    }

    static inline ParsedMessage parse_replace_order(const uint8_t* d, size_t len) noexcept {
        // REPLACE_ORDER layout (33 bytes):
        // [0]     = 'U'
        // [1..8]  = timestamp_ns
        // [9..16] = orig_order_ref
        // [17..24]= new_order_ref
        // [25..28]= new_shares
        // [29..32]= new_price * 10000
        ParsedMessage m{};
        if (len < 33) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::REPLACE_ORDER;
        auto& msg = m.data.replace_order;
        msg.timestamp_ns   = read_be64(d + 1);
        msg.orig_order_ref = read_be64(d + 9);
        msg.new_order_ref  = read_be64(d + 17);
        msg.new_shares     = read_be32(d + 25);
        msg.new_price      = read_be32(d + 29) / 10000.0;
        return m;
    }

    static inline ParsedMessage parse_order_executed(const uint8_t* d, size_t len) noexcept {
        // ORDER_EXECUTED layout (29 bytes):
        // [0]     = 'E'
        // [1..8]  = timestamp_ns
        // [9..16] = order_ref
        // [17..20]= exec_shares
        // [21..28]= match_number
        ParsedMessage m{};
        if (len < 29) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::ORDER_EXECUTED;
        auto& msg = m.data.order_executed;
        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        msg.exec_shares  = read_be32(d + 17);
        msg.match_number = read_be64(d + 21);
        return m;
    }

    static inline ParsedMessage parse_order_cancelled(const uint8_t* d, size_t len) noexcept {
        // ORDER_CANCELLED layout (21 bytes):
        // [0]     = 'C'
        // [1..8]  = timestamp_ns
        // [9..16] = order_ref
        // [17..20]= cancelled_shares
        ParsedMessage m{};
        if (len < 21) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::ORDER_CANCELLED;
        auto& msg = m.data.order_cancelled;
        msg.timestamp_ns    = read_be64(d + 1);
        msg.order_ref       = read_be64(d + 9);
        msg.cancelled_shares = read_be32(d + 17);
        return m;
    }

    static inline ParsedMessage parse_trade(const uint8_t* d, size_t len) noexcept {
        // TRADE layout (42 bytes):
        // [0]     = 'P'
        // [1..8]  = timestamp_ns
        // [9..16] = order_ref
        // [17]    = side
        // [18..21]= shares
        // [22..29]= stock
        // [30..33]= price * 10000
        // [34..41]= match_number
        ParsedMessage m{};
        if (len < 42) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::TRADE;
        auto& msg = m.data.trade;
        msg.timestamp_ns = read_be64(d + 1);
        msg.order_ref    = read_be64(d + 9);
        msg.side         = (char)d[17];
        msg.shares       = read_be32(d + 18);
        memcpy(msg.stock, d + 22, 8);
        msg.stock[8]     = '\0';
        msg.price        = read_be32(d + 30) / 10000.0;
        msg.match_number = read_be64(d + 34);
        return m;
    }

    static inline ParsedMessage parse_system_event(const uint8_t* d, size_t len) noexcept {
        // SYSTEM_EVENT layout (10 bytes):
        // [0]    = 'S'
        // [1..8] = timestamp_ns
        // [9]    = event_code ('O'=open, 'C'=close, 'H'=halt)
        ParsedMessage m{};
        if (len < 10) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::SYSTEM_EVENT;
        m.data.system_event.timestamp_ns = read_be64(d + 1);
        m.data.system_event.event_code   = (char)d[9];
        return m;
    }

    static inline ParsedMessage parse_stock_directory(const uint8_t* d, size_t len) noexcept {
        // STOCK_DIRECTORY layout (17 bytes, simplified):
        // [0]    = 'R'
        // [1..8] = timestamp_ns
        // [9..16]= stock symbol
        ParsedMessage m{};
        if (len < 17) { m.type = MsgType::ERROR; return m; }

        m.type = MsgType::STOCK_DIRECTORY;
        m.data.stock_directory.timestamp_ns = read_be64(d + 1);
        memcpy(m.data.stock_directory.stock, d + 9, 8);
        m.data.stock_directory.stock[8] = '\0';
        return m;
    }
};
