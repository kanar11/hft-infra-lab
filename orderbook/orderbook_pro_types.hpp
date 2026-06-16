/*
 * orderbook_pro_types.hpp — model danych dla FullOrderBook.
 *
 * Wydzielone z orderbook_pro.hpp: stałe siatki cenowej, enumy (typy zleceń,
 * TIF, STP, statusy, eventy, reject reasons, fazy sesji) oraz POD-y księgi
 * (Order, PriceLevel, Trade, BookStats, BookEvent, TopOfBook, DepthLevel).
 *
 * Te typy są samodzielne — nie zależą od FullOrderBook ani BookCluster, więc
 * konsumenci (strategie, risk, logger) mogą includować sam model bez ciągnięcia
 * całego silnika matchingu.
 */
#pragma once

#include "../common/types.hpp"     // Side
#include <climits>                 // INT32_MAX (sentinel NO_ASK_TICKS)
#include <cstdint>

namespace orderbook_pro {

// Stałe wire / model (mogłyby trafić do common/, ale chcemy żeby orderbook_pro
// był standalone i nie wymagał innych modułów poza common/types).
inline constexpr std::int32_t PRICE_MIN_TICKS = 0;            // dolny brzeg siatki
inline constexpr std::int32_t NO_BID_TICKS    = -1;           // sentinel "brak quote"
inline constexpr std::int32_t NO_ASK_TICKS    = INT32_MAX;    // sentinel po stronie ask


// Typ zlecenia — DEC2025 NASDAQ + IEX taxonomies.
enum class OrderType : std::uint8_t {
    LIMIT       = 0,   // standardowe limit order
    IOC         = 1,   // Immediate-Or-Cancel: weź co możesz teraz, resztę KASUJ
    FOK         = 2,   // Fill-Or-Kill: cała qty albo nic
    POST_ONLY   = 3,   // ALO — odrzuć jeśli zostałbyś takerem (cross na entry)
    ICEBERG     = 4,   // pokazuj tylko `displayed_qty`, ukrywaj resztę
    STOP        = 5,   // trigger-on-price; po triggerze staje się LIMIT/MARKET
    PEG         = 6,   // peg do mid albo best bid/ask + offset
    MARKET      = 7,   // bez ceny, zjadasz aż do wyczerpania
    HIDDEN      = 8,   // NIE pokazywane w L1/L2 depth ani trade tape (dark pool semantyka)
    AON         = 9,   // All-Or-None: fill jak FOK ale persiste w księdze (czeka)
};


// Time in force.
enum class TimeInForce : std::uint8_t {
    DAY = 0,   // ważne do końca sesji
    GTC = 1,   // Good-Till-Cancel — przeżywa nocny rollover
    IOC = 2,   // Immediate-Or-Cancel (redundancja z OrderType::IOC dla wygody)
    FOK = 3,   // Fill-Or-Kill
    GTD = 4,   // Good-Til-Date — expire_ts_ns_ pokazuje kiedy
    GTX = 5,   // Good-Til-Cross — resztka kasowana po najbliższym crossie aukcyjnym
};


// Self-trade prevention — co robić gdy nasze ID konta uderza w nasze quote.
enum class SelfTradePrevention : std::uint8_t {
    NONE              = 0,   // pozwól wash trade (zwykle ZAKAZANE, ale dla testów)
    CANCEL_NEWEST     = 1,   // przychodzące zlecenie anulowane
    CANCEL_OLDEST     = 2,   // resting zlecenie anulowane
    CANCEL_BOTH       = 3,   // oba kasowane
    DECREMENT_AND_CXL = 4,   // mniejsza qty z kart wzajemnie (NASDAQ DAC)
};


// Status pojedynczego Ordera w lifecycle.
enum class OrderStatus : std::uint8_t {
    NEW              = 0,   // zaakceptowane, jeszcze nie w księdze (queue join in progress)
    OPEN             = 1,   // w księdze, czeka na egzekucję
    PARTIALLY_FILLED = 2,   // częściowo wypełnione, resztki czekają
    FILLED           = 3,   // całkowicie wypełnione
    CANCELLED        = 4,   // skasowane przez usera
    REJECTED         = 5,   // odrzucone przed wejściem (locked/crossed/POST_ONLY cross)
    EXPIRED          = 6,   // GTD/DAY/IOC/FOK expired
    REPLACED         = 7,   // cancel+resubmit przez modify()
};


// Typ eventu generowanego przez book (callback OnEvent).
enum class EventType : std::uint8_t {
    ACCEPT       = 0,   // order zaakceptowany do księgi (NEW → OPEN)
    REJECT       = 1,   // odrzucony przed dodaniem
    FILL         = 2,   // egzekucja (może być częściowa lub pełna)
    CANCEL       = 3,   // user cancel
    EXPIRE       = 4,   // expire (GTD/DAY)
    REPLACE      = 5,   // modify success
    BOOK_UPDATE  = 6,   // książka zmieniła się (top of book / depth)
};


// Reason code dla REJECT — żeby caller wiedział czemu zlecenie odrzucone.
enum class RejectReason : std::uint8_t {
    NONE                  = 0,
    PRICE_OUT_OF_RANGE    = 1,   // price < 0 lub price >= LEVELS
    QTY_ZERO_OR_NEGATIVE  = 2,
    POOL_EXHAUSTED        = 3,   // brak miejsca w order pool
    POST_ONLY_WOULD_CROSS = 4,   // POST_ONLY a zlecenie krzyżuje rynek
    FOK_NOT_FILLABLE      = 5,   // FOK i niewystarczająca płynność
    SELF_TRADE_BLOCKED    = 6,   // STP zadziałało
    DUPLICATE_ID          = 7,   // ID już istnieje w księdze
    LOCKED_MARKET         = 8,   // bid == ask (cross protection)
    CROSSED_MARKET        = 9,   // bid > ask (rynek skrzyżowany)
    HALTED                = 10,  // book halted (trading halt)
    MIN_QTY_NOT_MET       = 11,  // matched < min_qty constraint
    LULD_BAND_BREACH      = 12,  // cena poza Limit Up / Limit Down bandami
    SSR_RESTRICTED        = 13,  // short sale przy aktywnym SSR ≤ best bid (Rule 201)
    REDUCE_ONLY_NO_POSITION = 14, // reduce-only bez pozycji do zredukowania
    RATE_LIMITED          = 15,  // konto przekroczyło token bucket (msg rate)
    MARKET_CLOSED         = 16,  // sesja w fazie CLOSED — submity odrzucane
    MMP_TRIPPED           = 17,  // market maker protection aktywna — quotes blokowane do resetu
};


// Faza sesji giełdowej. Book startuje w CONTINUOUS (backward compat) —
// lifecycle używasz opt-in przez begin_pre_open()/open_market()/...
enum class SessionPhase : std::uint8_t {
    PRE_OPEN   = 0,   // orders queue do opening cross, brak continuous match
    CONTINUOUS = 1,   // normalny matching
    CLOSING    = 2,   // orders queue do closing cross
    CLOSED     = 3,   // submity odrzucane (MARKET_CLOSED); cancel dozwolony
};


// Pojedyncze zlecenie w księdze — POD, cache-aligned (jedna linia 64 B).
//
// Intrusive linked list: każdy Order trzyma next_at_level_/prev_at_level_
// w obrębie swojego PriceLevel. Cancel = O(1) unlink bez search.
struct alignas(64) Order {
    std::uint64_t  id;                  // unikalny order_id (caller-provided lub auto)
    std::uint64_t  client_id;            // ID klienta (do STP)
    std::int32_t   price_ticks;          // cena w tickach
    std::int32_t   total_qty;            // całkowite qty zlecenia
    std::int32_t   filled_qty;           // ile wypełnione do tej pory
    std::int32_t   displayed_qty;        // dla ICEBERG: ile widoczne; LIMIT: == total
    Side           side;                 // BUY / SELL
    OrderType      type;                 // typ zlecenia
    TimeInForce    tif;                  // time in force
    OrderStatus    status;               // bieżący stan
    std::uint64_t  submit_ts_ns;         // monotonic timestamp wpływu do księgi
    std::uint64_t  expire_ts_ns;         // dla GTD; 0 = nigdy
    std::int32_t   stop_trigger_ticks;   // dla STOP/PEG: cena triggera/peg base
    std::int32_t   peg_offset_ticks;     // dla PEG: offset od best bid/ask/mid
    std::int32_t   peg_cap_ticks;        // dla PEG: limit cap (BUY: max, SELL: min); 0 = brak
    std::int32_t   decision_mid_ticks;   // snapshot mid przy submit — dla IS (Implementation Shortfall)
    std::int32_t   iceberg_display_size; // dla ICEBERG: rozmiar refresh (0 = nie-iceberg)
    std::uint64_t  first_fill_ts_ns;     // ts pierwszego fill (0 jeśli nic nie wypełnione)
    Order*         next_at_level;        // intrusive list (FIFO)
    Order*         prev_at_level;
    Order*         next_free;            // free list w pool (gdy slot wolny)

    void reset() noexcept {
        id = 0;
        client_id = 0;
        price_ticks = 0;
        total_qty = 0;
        filled_qty = 0;
        displayed_qty = 0;
        side = Side::BUY;
        type = OrderType::LIMIT;
        tif  = TimeInForce::DAY;
        status = OrderStatus::NEW;
        submit_ts_ns = 0;
        expire_ts_ns = 0;
        stop_trigger_ticks = 0;
        peg_offset_ticks = 0;
        peg_cap_ticks = 0;
        decision_mid_ticks = -1;
        iceberg_display_size = 0;
        first_fill_ts_ns = 0;
        next_at_level = nullptr;
        prev_at_level = nullptr;
        next_free = nullptr;
    }

    std::int32_t remaining_qty() const noexcept { return total_qty - filled_qty; }
    bool is_active() const noexcept {
        return status == OrderStatus::OPEN || status == OrderStatus::PARTIALLY_FILLED;
    }
};


// Pojedynczy price level: head/tail FIFO + agregaty dla L2.
struct alignas(64) PriceLevel {
    Order*        head            = nullptr;   // pierwsza w kolejce (najwcześniej zaplanowana)
    Order*        tail            = nullptr;   // ostatnia (most recent join)
    std::int32_t  total_qty       = 0;          // Σ displayed_qty (widoczna płynność)
    std::int32_t  total_hidden    = 0;          // Σ ICEBERG hidden (niewidoczne)
    std::int32_t  order_count     = 0;          // ile zleceń w kolejce (do queue position)

    bool empty() const noexcept { return head == nullptr; }
    void clear() noexcept {
        head = tail = nullptr;
        total_qty = total_hidden = 0;
        order_count = 0;
    }
};


// Pojedyncza egzekucja na trade tape.
struct alignas(32) Trade {
    std::uint64_t  exec_id;          // unikalny ID egzekucji (monotoniczny)
    std::uint64_t  maker_order_id;
    std::uint64_t  taker_order_id;
    std::int32_t   price_ticks;
    std::int32_t   qty;
    Side           taker_side;       // która strona była agresorem
    std::uint64_t  ts_ns;
};


// Snapshot statyk księgi (informational, lazy).
struct BookStats {
    std::uint64_t  total_orders_added       = 0;
    std::uint64_t  total_orders_cancelled   = 0;
    std::uint64_t  total_orders_replaced    = 0;
    std::uint64_t  total_orders_rejected    = 0;
    std::uint64_t  total_orders_expired     = 0;
    std::uint64_t  total_fills              = 0;
    std::uint64_t  total_volume             = 0;     // Σ qty wszystkich fills
    std::uint64_t  total_locked_rejects     = 0;
    std::uint64_t  total_self_trade_blocks  = 0;
    std::uint64_t  peak_order_pool_used     = 0;
    std::uint64_t  total_stop_triggers      = 0;
    std::uint64_t  total_peg_reprices       = 0;
    std::uint64_t  total_mass_cancels       = 0;
    std::uint64_t  total_auctions_executed  = 0;
    std::uint64_t  total_mass_quotes        = 0;
    std::uint64_t  total_tob_changes        = 0;
    std::uint64_t  total_sweeps             = 0;          // taker fills (regardless of levels)
    std::uint64_t  multi_level_sweeps       = 0;          // taker matched >= 2 price levels
    std::uint64_t  sum_levels_touched       = 0;          // Σ levels traversed (per sweep)
    std::uint64_t  priority_preserved_mods  = 0;          // modify qty-DOWN same-price
    std::uint64_t  priority_lost_mods       = 0;          // modify zmieniający cenę / qty UP
    std::uint64_t  total_quoted_spread_ticks_obs = 0;     // Σ best_ask-best_bid przy obserwacji
    std::uint64_t  total_quoted_spread_samples   = 0;     // ile próbek (do mean spread)
    std::uint64_t  total_effective_spread_2x_ticks = 0;   // Σ 2×|fill_px - mid| (signed)
    std::uint64_t  total_effective_spread_samples  = 0;   // ile fillów (do mean effective)
};


// Event przekazywany do callbacka.
struct BookEvent {
    EventType     type;
    std::uint64_t order_id;          // dla ACCEPT/REJECT/CANCEL/EXPIRE/REPLACE/FILL
    std::uint64_t maker_id;          // dla FILL: maker side
    std::int32_t  price_ticks;
    std::int32_t  qty;               // dla FILL: ile, dla CANCEL: pozostała qty
    OrderStatus   resulting_status;
    RejectReason  reject_reason;     // tylko dla REJECT
    std::uint64_t ts_ns;
    std::uint64_t seq_num = 0;       // monotonic — consumer dedup / gap detect
    std::uint64_t client_id = 0;     // konto (0 = nieznane/anonim) — drop copy
};


// Top-of-book quote — najczęstsza struktura konsumowana przez strategie.
struct TopOfBook {
    std::int32_t  best_bid_ticks;
    std::int32_t  best_ask_ticks;
    std::int32_t  bid_qty;            // displayed na best bid
    std::int32_t  ask_qty;            // displayed na best ask
    std::int32_t  bid_count;          // ile zleceń na best bid
    std::int32_t  ask_count;
};


// L2 depth row.
struct DepthLevel {
    std::int32_t  price_ticks;
    std::int32_t  qty;
    std::int32_t  order_count;
};

}  // namespace orderbook_pro
