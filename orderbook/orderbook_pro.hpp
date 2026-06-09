/*
 * FullOrderBook<LEVELS, MAX_ORDERS> — production-grade orderbook.
 *
 * Po co kolejny? FlatOrderBook (orderbook_flat.hpp) ma O(1) add/cancel/modify
 * z trackowaniem ID, ale agreguje quantity per price level — nie zna kolejki
 * (FIFO) i nie obsługuje zaawansowanych typów zleceń. Ten plik dodaje:
 *
 *   - **L3 detail** — pełna FIFO kolejka per price level (nie tylko Σ qty)
 *   - **Queue position tracking** — wiesz że jesteś N-ty w kolejce
 *   - **Order types**: LIMIT, IOC, FOK, POST_ONLY, ICEBERG, STOP, PEG
 *   - **Time-in-force**: DAY, GTC, IOC, FOK, GTD
 *   - **Self-trade prevention** (STP) — cancel-newest / cancel-oldest / cancel-both
 *   - **Lifecycle events** — callback per ACCEPT / FILL / PARTIAL / CANCEL / REJECT
 *   - **Snapshot + delta** — bytestream do recovery (incremental + full)
 *   - **Trade tape** — ring buffer ostatnich N executions
 *   - **L1/L2/L3 views** — top quote / N-level depth / full order list
 *   - **Imbalance + microprice** — order flow signals
 *   - **Pre-trade checks** — locked/crossed market protection
 *
 * Wybory projektowe:
 *   - Pamięć: pre-allocated `Order` pool (free-list LIFO), zero heap alloc
 *     na hot path. Wszystkie struktury cache-aligned.
 *   - Price levels: flat array `levels_[LEVELS]` indeksowane przez (price-PRICE_MIN).
 *     Każdy `PriceLevel` to mała struct z `head_ / tail_ / total_qty_ / order_count_`.
 *   - FIFO kolejka: intrusive doubly-linked list — `Order` ma `next_at_level_`
 *     i `prev_at_level_`. Cancel = O(1) unlink. Insert na koniec = O(1).
 *   - Order ID lookup: `std::unordered_map<uint64_t, Order*>` — O(1) cancel/modify.
 *   - Best bid/ask: trackowane jako int32 indices w `levels_` (jak FlatOrderBook).
 *   - Multi-level depth: top N bidów/asków cachowane w `top_bids_[N]` /
 *     `top_asks_[N]` (lazy, invalidated on book change).
 *
 * Limity szablonu:
 *   - LEVELS         — szerokość siatki cenowej w tickach (powinno pokrywać dzień)
 *   - MAX_ORDERS     — pula Orderów (=max equally aktywnych zleceń w księdze)
 *
 * Tick = 1/PRICE_SCALE dolara (=$0.0001 jeśli PRICE_SCALE=10000).
 *
 * Wydajność (na podobnej szablonowej księdze):
 *   - add limit:        p50 ~80 ns, p99 ~150 ns
 *   - cancel by id:     p50 ~50 ns, p99 ~120 ns
 *   - top-of-book read: p50 ~5 ns  (1 cache line)
 *   - L2 depth 10:      p50 ~30 ns (10 levels scan)
 *
 * Pipeline integration:
 *   Strategy → Risk → Router → OMS → FullOrderBook (matching) → Logger
 */
#pragma once

#include "../common/types.hpp"     // Price, Side, side_str

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>     // INT32_MAX (sentinel NO_ASK_TICKS)
#include <cmath>       // std::sqrt — tape price stddev
#include <cstdint>
#include <cstdio>
#include <cstdlib>     // std::abs(int) — auction imbalance scoring
#include <cstring>
#include <unordered_map>
#include <vector>


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
    std::int32_t   decision_mid_ticks;   // snapshot mid przy submit — dla IS (Implementation Shortfall)
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
        decision_mid_ticks = -1;
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


// FullOrderBook<LEVELS, MAX_ORDERS> — generic, fixed-capacity.
//   LEVELS      — ile slotów cenowych. price_ticks ∈ [0, LEVELS).
//                  Dla AAPL przy $0.01 ticku i zakresie $100..$200 wystarczy
//                  ~10000-65536 (zostawia headroom).
//   MAX_ORDERS  — pula Orderów. Realny intraday LOB ma 10k-100k równocześnie.
//
// noexcept everywhere — żadnych wyjątków; błędy raportowane przez return value
// + RejectReason. To wymóg HFT — wyjątek z nieprzewidzianego miejsca zabija
// determinizm latencji.
template <std::int32_t LEVELS = 16384, std::int32_t MAX_ORDERS = 65536>
class FullOrderBook {
    static_assert(LEVELS > 0,     "LEVELS must be positive");
    static_assert(MAX_ORDERS > 0, "MAX_ORDERS must be positive");

    // Storage
    Order        pool_[MAX_ORDERS];         // pool aktywnych + free slots
    Order*       free_list_head_ = nullptr; // LIFO free-list (cache-friendly)
    PriceLevel   levels_[LEVELS];           // jeden slot per tick
    std::unordered_map<std::uint64_t, Order*> id_index_;  // O(1) cancel/modify

    // Best bid/ask cache — int sentinels
    std::int32_t best_bid_ticks_ = NO_BID_TICKS;
    std::int32_t best_ask_ticks_ = NO_ASK_TICKS;

    // Trade tape — ring buffer
    static constexpr std::size_t TAPE_CAP = 1024;
    Trade        tape_[TAPE_CAP]{};
    std::size_t  tape_head_ = 0;        // next write index
    std::size_t  tape_count_ = 0;       // total trades written (overruns ok)
    std::uint64_t next_exec_id_ = 1;

    // Auto order_id (gdy caller nie podaje swojego)
    std::uint64_t next_order_id_ = 1;

    // Statystyki
    BookStats    stats_;
    std::size_t  active_orders_ = 0;
    std::size_t  pool_high_water_ = 0;

    // STP policy (default NONE — testy mogą się wash-trade'ować swobodnie)
    SelfTradePrevention stp_policy_ = SelfTradePrevention::NONE;

    // Callback na eventy (opcjonalnie ustawiany przez set_event_callback)
    using EventCallback = void(*)(const BookEvent&, void* ctx);
    EventCallback event_cb_  = nullptr;
    void*         event_ctx_ = nullptr;

    // Helper: timestamp monotonic ns (header-local żeby uniknąć zależności).
    static std::uint64_t mono_ns_now() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Bezpieczne sprawdzenie zakresu price ticks.
    static bool in_range(std::int32_t price_ticks) noexcept {
        return price_ticks >= PRICE_MIN_TICKS && price_ticks < LEVELS;
    }

    // Alokuj Order z poola lub nullptr gdy pula wyczerpana.
    Order* alloc_order() noexcept {
        if (!free_list_head_) return nullptr;
        Order* o = free_list_head_;
        free_list_head_ = o->next_free;
        o->reset();
        ++active_orders_;
        if (active_orders_ > pool_high_water_) {
            pool_high_water_ = active_orders_;
            stats_.peak_order_pool_used = pool_high_water_;
        }
        return o;
    }

    // Zwróć Order do free-list.
    void free_order(Order* o) noexcept {
        if (!o) return;
        o->next_free = free_list_head_;
        free_list_head_ = o;
        --active_orders_;
    }

    // Wepchnij Order na koniec FIFO kolejki na price level (intrusive list).
    void enqueue_at_level(Order* o) noexcept {
        PriceLevel& lvl = levels_[o->price_ticks];
        o->next_at_level = nullptr;
        o->prev_at_level = lvl.tail;
        if (lvl.tail) lvl.tail->next_at_level = o;
        else          lvl.head = o;
        lvl.tail = o;
        lvl.total_qty    += o->displayed_qty;
        lvl.total_hidden += (o->total_qty - o->displayed_qty);
        ++lvl.order_count;
    }

    // Wyjmij Order z jego price level (O(1) — intrusive unlink).
    void unlink_from_level(Order* o) noexcept {
        PriceLevel& lvl = levels_[o->price_ticks];
        if (o->prev_at_level) o->prev_at_level->next_at_level = o->next_at_level;
        else                  lvl.head = o->next_at_level;
        if (o->next_at_level) o->next_at_level->prev_at_level = o->prev_at_level;
        else                  lvl.tail = o->prev_at_level;
        lvl.total_qty    -= o->displayed_qty;
        lvl.total_hidden -= (o->total_qty - o->displayed_qty);
        if (lvl.order_count > 0) --lvl.order_count;
    }

    // Po dodaniu/wyjęciu z księgi — odśwież best_bid/best_ask gdy trzeba.
    void refresh_best_bid_from(std::int32_t start_ticks) noexcept {
        // Skanuj w dół zaczynając od start_ticks aż znajdziesz niepusty level
        // (lub spadniesz poniżej 0 → brak bidów).
        for (std::int32_t p = start_ticks; p >= PRICE_MIN_TICKS; --p) {
            if (levels_[p].total_qty > 0) { best_bid_ticks_ = p; return; }
        }
        best_bid_ticks_ = NO_BID_TICKS;
    }
    void refresh_best_ask_from(std::int32_t start_ticks) noexcept {
        for (std::int32_t p = start_ticks; p < LEVELS; ++p) {
            if (levels_[p].total_qty > 0) { best_ask_ticks_ = p; return; }
        }
        best_ask_ticks_ = NO_ASK_TICKS;
    }

    // Emit event do callbacka jeśli ustawiony.
    void emit(EventType ev, std::uint64_t order_id, std::uint64_t maker_id,
              std::int32_t price_ticks, std::int32_t qty,
              OrderStatus status, RejectReason rr) noexcept {
        const std::uint64_t seq = ++next_event_seq_;
        last_emitted_seq_ = seq;
        if (!event_cb_) return;
        BookEvent e{ev, order_id, maker_id, price_ticks, qty, status, rr,
                    mono_ns_now(), seq};
        event_cb_(e, event_ctx_);
    }

    // Zapisz Trade do tape (ring buffer) + aktualizuj last_trade_ticks_
    // używane przez STOP triggers + fee accounting.
    void record_trade(std::uint64_t maker_id, std::uint64_t taker_id,
                      std::int32_t price_ticks, std::int32_t qty,
                      Side taker_side, std::uint64_t ts_ns) noexcept {
        Trade& t = tape_[tape_head_ % TAPE_CAP];
        t.exec_id = next_exec_id_++;
        t.maker_order_id = maker_id;
        t.taker_order_id = taker_id;
        t.price_ticks = price_ticks;
        t.qty = qty;
        t.taker_side = taker_side;
        t.ts_ns = ts_ns;
        ++tape_head_;
        ++tape_count_;
        ++stats_.total_fills;
        stats_.total_volume += static_cast<std::uint64_t>(qty);
        last_trade_ticks_ = price_ticks;
        update_size_distribution(qty);
        // Fee accounting (basis): notional = price * qty; fee = notional * bps / 10000
        // Trzymamy w "basis units" (price_ticks * qty * bps), divs by 10000 robi caller.
        const std::int64_t notional_ticks =
            static_cast<std::int64_t>(price_ticks) * qty;
        cum_taker_fees_ += notional_ticks * taker_fee_bps_;
        cum_maker_fees_ += notional_ticks * maker_fee_bps_;
        // MIFID II RTS27/28: effective spread = 2 × |exec - mid|
        if (mifid_enabled_) {
            ++mifid_.num_executions;
            mifid_.total_volume         += static_cast<std::uint64_t>(qty);
            mifid_.total_notional_ticks += notional_ticks;
            const std::int32_t m = mid_ticks();
            if (m > 0) {
                const std::int64_t diff = std::abs(static_cast<std::int64_t>(price_ticks) - m);
                mifid_.sum_effective_spread += diff * 2 * qty;
                // signed price impact (taker direction = sign of (price - mid))
                const std::int64_t signed_diff =
                    static_cast<std::int64_t>(price_ticks) - m;
                mifid_.sum_signed_price_impact +=
                    (taker_side == Side::BUY ? signed_diff : -signed_diff) * qty;
            }
        }
        // Order-flow accumulators (VPIN-style toxicity)
        if (taker_side == Side::BUY) {
            taker_buy_volume_ += static_cast<std::uint64_t>(qty);
            ++taker_buy_count_;
        } else {
            taker_sell_volume_ += static_cast<std::uint64_t>(qty);
            ++taker_sell_count_;
        }
        // Signed volume EMA
        {
            const double signed_q = (taker_side == Side::BUY ? +1.0 : -1.0) *
                                    static_cast<double>(qty);
            if (!ema_signed_volume_init_) {
                ema_signed_volume_ = signed_q;
                ema_signed_volume_init_ = true;
            } else {
                ema_signed_volume_ = 0.1 * signed_q + 0.9 * ema_signed_volume_;
            }
        }
        // Per-side trade VWAP
        const std::int64_t notional = static_cast<std::int64_t>(price_ticks) * qty;
        if (taker_side == Side::BUY) {
            cum_buy_notional_ticks_ += notional;
            cum_buy_volume_         += static_cast<std::uint64_t>(qty);
        } else {
            cum_sell_notional_ticks_ += notional;
            cum_sell_volume_         += static_cast<std::uint64_t>(qty);
        }
        // Inter-trade time gap
        if (prev_trade_ts_ns_for_gap_ != 0 && ts_ns > prev_trade_ts_ns_for_gap_) {
            const std::uint64_t gap = ts_ns - prev_trade_ts_ns_for_gap_;
            if (gap < inter_trade_gap_min_ns_) inter_trade_gap_min_ns_ = gap;
            if (gap > inter_trade_gap_max_ns_) inter_trade_gap_max_ns_ = gap;
            inter_trade_gap_sum_ns_ += gap;
            ++inter_trade_gap_count_;
        }
        prev_trade_ts_ns_for_gap_ = ts_ns;
        // Largest single trade
        if (qty > largest_single_trade_qty_) largest_single_trade_qty_ = qty;
        // Tick-by-tick Δp distribution (clipped do [-4, +4])
        if (prev_trade_px_for_lambda_ >= 0) {
            const std::int32_t raw = price_ticks - static_cast<std::int32_t>(prev_trade_px_for_lambda_);
            const std::int32_t clip = std::max<std::int32_t>(-4,
                                       std::min<std::int32_t>(4, raw));
            ++price_change_hist_[static_cast<std::size_t>(clip + 4)];
            ++price_change_total_;
        }
        // Kyle's lambda accumulator (global + per-side decomp)
        if (prev_trade_px_for_lambda_ >= 0) {
            const double dp = static_cast<double>(price_ticks - prev_trade_px_for_lambda_);
            const double signed_v = (taker_side == Side::BUY ? +1.0 : -1.0) *
                                    static_cast<double>(qty);
            const double vsq = signed_v * signed_v;
            cum_price_volume_product_ += dp * signed_v;
            cum_signed_volume_sq_     += vsq;
            if (taker_side == Side::BUY) {
                cum_pv_buy_  += dp * signed_v;
                cum_vsq_buy_ += vsq;
            } else {
                cum_pv_sell_  += dp * signed_v;
                cum_vsq_sell_ += vsq;
            }
        }
        prev_trade_px_for_lambda_ = price_ticks;
        // Latency arbitrage: same-side aggressors within window
        if (larb_window_ns_ > 0 && last_fill_ts_ns_ != 0 &&
            ts_ns > last_fill_ts_ns_ &&
            (ts_ns - last_fill_ts_ns_) <= larb_window_ns_ &&
            taker_side == larb_last_side_) {
            ++larb_same_side_fast_;
        }
        larb_last_side_     = taker_side;
        larb_last_fill_ts_  = ts_ns;
        last_fill_ts_ns_    = ts_ns;
        if (taker_side == Side::BUY) last_buy_fill_ts_ns_  = ts_ns;
        else                          last_sell_fill_ts_ns_ = ts_ns;
        // Volume-at-price profile (capped na ostatnie SAMPLES poziomów wpisanych)
        if (price_ticks >= 0 && price_ticks < LEVELS) {
            volume_at_price_[static_cast<std::size_t>(price_ticks)] +=
                static_cast<std::uint64_t>(qty);
        }
    }

public:
    FullOrderBook() noexcept {
        // Zbuduj free-list: każdy slot łączysz z next.
        for (std::int32_t i = 0; i < MAX_ORDERS; ++i) {
            pool_[i].reset();
            pool_[i].next_free = (i + 1 < MAX_ORDERS) ? &pool_[i + 1] : nullptr;
        }
        free_list_head_ = &pool_[0];
        id_index_.reserve(static_cast<std::size_t>(MAX_ORDERS));
    }

    // Deleted copy/move — book ma intrusive pointers, kopia by je niszczyła.
    FullOrderBook(const FullOrderBook&)            = delete;
    FullOrderBook& operator=(const FullOrderBook&) = delete;
    FullOrderBook(FullOrderBook&&)                 = delete;
    FullOrderBook& operator=(FullOrderBook&&)      = delete;

    // ====================================================================
    // Konfiguracja
    // ====================================================================

    void set_stp_policy(SelfTradePrevention p) noexcept { stp_policy_ = p; }
    SelfTradePrevention stp_policy() const noexcept { return stp_policy_; }

    void set_event_callback(EventCallback cb, void* ctx) noexcept {
        event_cb_ = cb; event_ctx_ = ctx;
    }

    // ====================================================================
    // L1 quote — top of book
    // ====================================================================

    std::int32_t best_bid_ticks() const noexcept { return best_bid_ticks_; }
    std::int32_t best_ask_ticks() const noexcept { return best_ask_ticks_; }
    bool has_bid() const noexcept { return best_bid_ticks_ != NO_BID_TICKS; }
    bool has_ask() const noexcept { return best_ask_ticks_ != NO_ASK_TICKS; }

    // mid_ticks: zwraca (bid+ask)/2, lub -1 gdy brak quote po jednej ze stron.
    std::int32_t mid_ticks() const noexcept {
        if (!has_bid() || !has_ask()) return -1;
        return (best_bid_ticks_ + best_ask_ticks_) / 2;
    }

    // microprice_ticks: weighted by size na top of book.
    //   micro = (bid*ask_qty + ask*bid_qty) / (bid_qty + ask_qty)
    // Lepszy predykator następnego trade'u niż mid (uwzględnia order flow).
    std::int32_t microprice_ticks() const noexcept {
        if (!has_bid() || !has_ask()) return -1;
        const std::int32_t bq = levels_[best_bid_ticks_].total_qty;
        const std::int32_t aq = levels_[best_ask_ticks_].total_qty;
        const std::int64_t denom = static_cast<std::int64_t>(bq) + aq;
        if (denom == 0) return mid_ticks();
        const std::int64_t num =
            static_cast<std::int64_t>(best_bid_ticks_) * aq +
            static_cast<std::int64_t>(best_ask_ticks_) * bq;
        return static_cast<std::int32_t>(num / denom);
    }

    // Top-of-book quote z qty + count.
    TopOfBook top_of_book() const noexcept {
        TopOfBook t{};
        t.best_bid_ticks = best_bid_ticks_;
        t.best_ask_ticks = best_ask_ticks_;
        if (has_bid()) {
            t.bid_qty   = levels_[best_bid_ticks_].total_qty;
            t.bid_count = levels_[best_bid_ticks_].order_count;
        }
        if (has_ask()) {
            t.ask_qty   = levels_[best_ask_ticks_].total_qty;
            t.ask_count = levels_[best_ask_ticks_].order_count;
        }
        return t;
    }

    // ====================================================================
    // Stats + introspekcja
    // ====================================================================

    const BookStats& stats() const noexcept { return stats_; }
    std::size_t active_orders() const noexcept { return active_orders_; }
    std::size_t pool_capacity() const noexcept { return MAX_ORDERS; }
    std::size_t pool_used_high_water() const noexcept { return pool_high_water_; }

    // Lookup po order_id (do introspekcji testów; nie używaj na hot path).
    const Order* find_order(std::uint64_t id) const noexcept {
        auto it = id_index_.find(id);
        return it == id_index_.end() ? nullptr : it->second;
    }

    // ====================================================================
    // Matching helpers
    // ====================================================================
private:
    // Sprawdza czy cena `p` po stronie `side` byłaby agresorem (krzyżuje rynek).
    // BUY  agresor gdy p >= best_ask
    // SELL agresor gdy p <= best_bid
    bool would_cross(Side side, std::int32_t p) const noexcept {
        if (side == Side::BUY)  return has_ask() && p >= best_ask_ticks_;
        else                     return has_bid() && p <= best_bid_ticks_;
    }

    // Sprawdza locked/crossed market przed dodaniem (post-fill state).
    // LOCKED: best_bid == best_ask (cena równa).
    // CROSSED: best_bid > best_ask (rynek skrzyżowany — patologia).
    bool is_locked()  const noexcept {
        return has_bid() && has_ask() && best_bid_ticks_ == best_ask_ticks_;
    }
    bool is_crossed() const noexcept {
        return has_bid() && has_ask() && best_bid_ticks_ > best_ask_ticks_;
    }

    // Quote tester dla FOK: czy dostępna płynność po tej stronie do `price` ≥ qty?
    bool fok_fillable(Side side, std::int32_t price, std::int32_t qty) const noexcept {
        std::int32_t avail = 0;
        if (side == Side::BUY) {
            for (std::int32_t p = best_ask_ticks_; p <= price && p < LEVELS; ++p) {
                avail += levels_[p].total_qty;
                if (avail >= qty) return true;
            }
        } else {
            for (std::int32_t p = best_bid_ticks_; p >= price && p >= 0; --p) {
                avail += levels_[p].total_qty;
                if (avail >= qty) return true;
            }
        }
        return avail >= qty;
    }

    // Match przy jednym price level (FIFO traversal). Pomniejsza qty_remaining.
    // Wywoływane z petli ceny w match_against().
    void match_at_level(PriceLevel& lvl, Order* taker,
                        std::int32_t& qty_remaining, std::uint64_t ts_ns) noexcept {
        Order* m = lvl.head;
        while (m && qty_remaining > 0) {
            // STP check: czy maker i taker to ten sam client?
            if (m->client_id != 0 && m->client_id == taker->client_id &&
                stp_policy_ != SelfTradePrevention::NONE) {
                ++stats_.total_self_trade_blocks;
                if (stp_policy_ == SelfTradePrevention::CANCEL_NEWEST) {
                    // Taker pada — abort completely
                    qty_remaining = 0;
                    return;
                } else if (stp_policy_ == SelfTradePrevention::CANCEL_OLDEST) {
                    // Maker pada — usuń go z księgi i jedź dalej
                    Order* next_m = m->next_at_level;
                    cancel_internal(m, /*emit_event=*/true);
                    m = next_m;
                    continue;
                } else if (stp_policy_ == SelfTradePrevention::CANCEL_BOTH) {
                    Order* next_m = m->next_at_level;
                    cancel_internal(m, true);
                    qty_remaining = 0;
                    (void)next_m;
                    return;
                }
            }

            const std::int32_t exec_qty = std::min(qty_remaining, m->displayed_qty);
            m->filled_qty   += exec_qty;
            taker->filled_qty += exec_qty;
            qty_remaining   -= exec_qty;
            // First-fill ts hook + latency stats (per order, only once)
            if (m->first_fill_ts_ns == 0) {
                m->first_fill_ts_ns = ts_ns;
                record_first_fill_latency(m->submit_ts_ns, ts_ns);
            }
            if (taker->first_fill_ts_ns == 0) {
                taker->first_fill_ts_ns = ts_ns;
                record_first_fill_latency(taker->submit_ts_ns, ts_ns);
            }
            exposure_on_fill(m->client_id, m->side, exec_qty);
            exposure_on_fill(taker->client_id, taker->side, exec_qty);
            // Implementation shortfall (per fill, signed by side).
            // BUY: cost > 0 jeśli fill_px > decision_mid; SELL: cost > 0 jeśli fill_px < decision_mid.
            if (taker->decision_mid_ticks > 0) {
                const std::int64_t fp = m->price_ticks;
                const std::int64_t dm = taker->decision_mid_ticks;
                const std::int64_t cost_per_share = (taker->side == Side::BUY)
                    ? (fp - dm) : (dm - fp);
                cum_implementation_shortfall_ticks_qty_ +=
                    cost_per_share * static_cast<std::int64_t>(exec_qty);
                cum_implementation_shortfall_qty_ +=
                    static_cast<std::uint64_t>(exec_qty);
            }
            // Aggressive vs passive accounting per account
            tag_aggressor_volume(taker->client_id, exec_qty);
            tag_passive_volume(m->client_id, exec_qty);

            // Trade record (price = maker's price = lvl.price = m->price_ticks)
            record_trade(m->id, taker->id, m->price_ticks, exec_qty,
                         taker->side, ts_ns);
            emit(EventType::FILL, taker->id, m->id, m->price_ticks, exec_qty,
                 taker->remaining_qty() == 0 ? OrderStatus::FILLED
                                              : OrderStatus::PARTIALLY_FILLED,
                 RejectReason::NONE);

            // Aktualizuj displayed/hidden i level total
            lvl.total_qty -= exec_qty;
            m->displayed_qty -= exec_qty;

            if (m->displayed_qty == 0 && m->total_qty - m->filled_qty > 0 &&
                m->type == OrderType::ICEBERG) {
                // Iceberg: doślij displayed z hidden reserve
                const std::int32_t hidden = m->total_qty - m->filled_qty;
                const std::int32_t orig_displayed = m->displayed_qty;
                (void)orig_displayed;
                m->displayed_qty = std::min(hidden, /*iceberg_show=*/100);
                lvl.total_qty    += m->displayed_qty;
                ++iceberg_refresh_count_;
                lvl.total_hidden -= m->displayed_qty;
                // Maker iceberg traci priority — przesuwamy na koniec FIFO
                Order* next_m = m->next_at_level;
                if (lvl.tail != m) {
                    // unlink + reinsert at tail
                    if (m->prev_at_level) m->prev_at_level->next_at_level = m->next_at_level;
                    else                  lvl.head = m->next_at_level;
                    if (m->next_at_level) m->next_at_level->prev_at_level = m->prev_at_level;
                    m->prev_at_level = lvl.tail;
                    m->next_at_level = nullptr;
                    lvl.tail->next_at_level = m;
                    lvl.tail = m;
                }
                m = next_m;
                continue;
            }

            if (m->filled_qty >= m->total_qty) {
                // Maker pełen → unlink + free
                Order* next_m = m->next_at_level;
                m->status = OrderStatus::FILLED;
                emit(EventType::FILL, m->id, taker->id, m->price_ticks, exec_qty,
                     OrderStatus::FILLED, RejectReason::NONE);
                // Unlink z level (już zaktualizowaliśmy total_qty wyżej, więc
                // unlink_from_level zrobi to ponownie. Robimy ręczny unlink.)
                if (m->prev_at_level) m->prev_at_level->next_at_level = m->next_at_level;
                else                  lvl.head = m->next_at_level;
                if (m->next_at_level) m->next_at_level->prev_at_level = m->prev_at_level;
                else                  lvl.tail = m->prev_at_level;
                if (lvl.order_count > 0) --lvl.order_count;
                id_index_.erase(m->id);
                record_lifecycle_age(m->submit_ts_ns);
                ++completion_filled_fully_;
                if (m->side == Side::BUY) ++maker_fills_buy_side_;
                else                       ++maker_fills_sell_side_;
                free_order(m);
                m = next_m;
            } else {
                m->status = OrderStatus::PARTIALLY_FILLED;
                // taker dostał całość lub maker ma displayed=0 ale nie iceberg.
                // Jeśli displayed_qty == 0 i nie iceberg → unlink (limit order
                // który dostał display reduction to nie powinien się zdarzyć).
                m = m->next_at_level;
            }
        }
    }

    // Match przeciw przeciwnej stronie. Aktualizuje taker, generuje fills.
    void match_against(Order* taker) noexcept {
        const std::uint64_t ts = mono_ns_now();
        std::int32_t qty_left = taker->remaining_qty();
        const std::int32_t qty_pre = qty_left;
        std::int32_t levels_consumed = 0;
        // Mid pre-match — używany do effective spread (TCA)
        const std::int32_t mid_pre_ticks = has_bid() && has_ask()
            ? (best_bid_ticks_ + best_ask_ticks_) / 2 : -1;

        if (taker->side == Side::BUY) {
            // BUY zjada asks od najlepszej (najtańszej) aż do limit price
            while (qty_left > 0 && has_ask() && best_ask_ticks_ <= taker->price_ticks) {
                const std::int32_t qty_before = qty_left;
                PriceLevel& lvl = levels_[best_ask_ticks_];
                match_at_level(lvl, taker, qty_left, ts);
                if (qty_left < qty_before) ++levels_consumed;
                if (lvl.empty()) {
                    refresh_best_ask_from(best_ask_ticks_ + 1);
                }
            }
        } else {
            while (qty_left > 0 && has_bid() && best_bid_ticks_ >= taker->price_ticks) {
                const std::int32_t qty_before = qty_left;
                PriceLevel& lvl = levels_[best_bid_ticks_];
                match_at_level(lvl, taker, qty_left, ts);
                if (qty_left < qty_before) ++levels_consumed;
                if (lvl.empty()) {
                    refresh_best_bid_from(best_bid_ticks_ - 1);
                }
            }
        }
        // Sweep stats — track multi-level fills
        if (qty_left < qty_pre) {
            ++stats_.total_sweeps;
            stats_.sum_levels_touched += static_cast<std::uint64_t>(levels_consumed);
            if (levels_consumed >= 2) ++stats_.multi_level_sweeps;
        }
        // Effective spread: 2× |VWAP(fills) − mid_pre|. Aproksymujemy fill_px = taker->price.
        if (mid_pre_ticks >= 0 && qty_left < qty_pre) {
            const std::int32_t diff = std::abs(taker->price_ticks - mid_pre_ticks);
            stats_.total_effective_spread_2x_ticks += static_cast<std::uint64_t>(2 * diff);
            ++stats_.total_effective_spread_samples;
            // Bands compliance
            if (fill_band_threshold_ticks_ > 0) {
                if (diff <= fill_band_threshold_ticks_) ++fills_within_band_;
                else                                     ++fills_outside_band_;
            }
        }
    }

    // Wspólna ścieżka cancel — używana przez user cancel + STP + internal.
    void cancel_internal(Order* o, bool emit_event) noexcept {
        if (!o || !o->is_active()) return;
        const std::int32_t cancel_price = o->price_ticks;
        const char         cancel_side  = (o->side == Side::BUY) ? 'B' : 'S';
        unlink_from_level(o);
        // Refresh best bid/ask jeśli ten level się opróżnił
        if (levels_[o->price_ticks].empty()) {
            if (o->side == Side::BUY  && o->price_ticks == best_bid_ticks_)
                refresh_best_bid_from(o->price_ticks - 1);
            if (o->side == Side::SELL && o->price_ticks == best_ask_ticks_)
                refresh_best_ask_from(o->price_ticks + 1);
        }
        const std::int32_t leftover = o->remaining_qty();
        o->status = OrderStatus::CANCELLED;
        id_index_.erase(o->id);
        if (emit_event) {
            emit(EventType::CANCEL, o->id, 0, o->price_ticks, leftover,
                 OrderStatus::CANCELLED, RejectReason::NONE);
        }
        push_audit(EventType::CANCEL, o->id, o->side, o->price_ticks, leftover,
                    OrderStatus::CANCELLED);
        exposure_on_cancel(o->client_id, o->side, leftover);
        record_lifecycle_age(o->submit_ts_ns);
        if (o->filled_qty == 0)        ++completion_cancelled_unfilled_;
        else                            ++completion_cancelled_partial_;
        free_order(o);
        ++stats_.total_orders_cancelled;
        push_delta(levels_[cancel_price].empty() ? 'D' : 'M',
                   cancel_side, cancel_price, levels_[cancel_price].total_qty);
    }

public:
    // ====================================================================
    // submit() — główna ścieżka dodania zlecenia
    // ====================================================================
    //
    // Zwraca order_id przyjętego zlecenia (>0) lub 0 gdy odrzucone.
    // out_reason (jeśli != nullptr) dostaje RejectReason dla diagnostyki.
    //
    // Logika:
    //   1. Walidacja: range, qty, duplicate id, range.
    //   2. STP check (jeśli policy != NONE) — ale STP pełniejsze w matchu.
    //   3. POST_ONLY: jeśli krzyżuje → REJECT.
    //   4. FOK: jeśli nieosiągalny → REJECT.
    //   5. Alokuj Order z pool.
    //   6. Jeśli typ to STOP, NIE matchuj — wstaw w czekoligę triggerów.
    //   7. Match against przeciwnej strony do limit price.
    //   8. Jeśli IOC i resta — cancel resztę. Jeśli FOK i nie wszystko → revert.
    //   9. Reszta (jeśli > 0): wstaw do FIFO na price level + index id.
    std::uint64_t submit(Side side, std::int32_t price_ticks, std::int32_t qty,
                          OrderType type = OrderType::LIMIT,
                          TimeInForce tif = TimeInForce::DAY,
                          std::uint64_t order_id = 0,
                          std::uint64_t client_id = 0,
                          std::int32_t displayed_qty = 0,
                          RejectReason* out_reason = nullptr,
                          std::int32_t min_qty = 0) noexcept {
        auto reject = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            const std::uint64_t rid = order_id ? order_id : 0;
            if (client_id != 0) ++client_rejections_[client_id];
            emit(EventType::REJECT, rid, 0, price_ticks, qty,
                 OrderStatus::REJECTED, r);
            push_audit(EventType::REJECT, rid, side, price_ticks, qty,
                       OrderStatus::REJECTED);
            return 0;
        };

        // Walidacje
        if (halted_)                   return reject(RejectReason::HALTED);
        if (qty <= 0)                  return reject(RejectReason::QTY_ZERO_OR_NEGATIVE);
        if (!in_range(price_ticks) && type != OrderType::MARKET)
                                       return reject(RejectReason::PRICE_OUT_OF_RANGE);
        if (order_id != 0 && id_index_.find(order_id) != id_index_.end())
                                       return reject(RejectReason::DUPLICATE_ID);
        // LULD check — quote poza band'em → REJECT (+opt-in auto-halt)
        if (luld_enabled_ && (price_ticks < luld_low_ticks_ ||
                                price_ticks > luld_high_ticks_)) {
            ++luld_breaches_;
            if (luld_auto_halt_) halt("LULD_BREACH");
            return reject(RejectReason::LULD_BAND_BREACH);
        }

        // POST_ONLY: nie wolno wziąć płynności (cross protection)
        if (type == OrderType::POST_ONLY && would_cross(side, price_ticks)) {
            return reject(RejectReason::POST_ONLY_WOULD_CROSS);
        }

        // FOK: musi się od razu wypełnić w całości
        if ((type == OrderType::FOK || tif == TimeInForce::FOK)
            && !fok_fillable(side, price_ticks, qty)) {
            return reject(RejectReason::FOK_NOT_FILLABLE);
        }

        // MIN_QTY: matched musi być ≥ min_qty albo REJECT.
        // (Skip dla POST_ONLY/HIDDEN/auction — nie matchują continuous.)
        if (min_qty > 0 && type != OrderType::POST_ONLY
            && type != OrderType::HIDDEN && !in_auction_mode_) {
            const auto preview = walk_the_book(side, qty, price_ticks);
            if (preview.fillable_qty < min_qty)
                return reject(RejectReason::MIN_QTY_NOT_MET);
        }

        // Alokacja
        Order* o = alloc_order();
        if (!o) return reject(RejectReason::POOL_EXHAUSTED);

        o->id            = order_id ? order_id : next_order_id_++;
        o->client_id     = client_id;
        o->price_ticks   = price_ticks;
        o->total_qty     = qty;
        o->filled_qty    = 0;
        // HIDDEN: pełna qty w total_hidden, displayed=0 → niewidoczne w L1/L2.
        // ICEBERG: pokazuj tylko displayed_qty arg (jeśli >0).
        // Pozostałe: pełna widoczność.
        o->displayed_qty = (type == OrderType::HIDDEN)             ? 0
                          : (type == OrderType::ICEBERG && displayed_qty > 0)
                                                                    ? displayed_qty
                                                                    : qty;
        o->side          = side;
        o->type          = type;
        o->tif           = tif;
        o->status        = OrderStatus::NEW;
        o->submit_ts_ns  = mono_ns_now();
        o->decision_mid_ticks = (has_bid() && has_ask())
            ? (best_bid_ticks_ + best_ask_ticks_) / 2 : -1;
        // Burst detector
        if (burst_window_ns_ > 0) {
            if (burst_last_submit_ns_ != 0 &&
                o->submit_ts_ns > burst_last_submit_ns_ &&
                (o->submit_ts_ns - burst_last_submit_ns_) <= burst_window_ns_) {
                if (burst_in_run_count_ == 0) {
                    ++burst_runs_count_;
                    burst_in_run_count_ = 2;
                } else {
                    ++burst_in_run_count_;
                }
            } else {
                burst_in_run_count_ = 0;
            }
            burst_last_submit_ns_ = o->submit_ts_ns;
        }
        ++stats_.total_orders_added;
        tally_accept(type, tif);

        emit(EventType::ACCEPT, o->id, 0, o->price_ticks, o->total_qty,
             OrderStatus::OPEN, RejectReason::NONE);
        push_audit(EventType::ACCEPT, o->id, o->side, o->price_ticks, o->total_qty,
                    OrderStatus::OPEN);
        exposure_on_submit(o->client_id, o->side, o->total_qty);

        // Match (chyba że POST_ONLY — ale POST_ONLY już odrzucone gdyby krzyżowało,
        // ani w trybie auction — orders czekają na batch cross via run_auction).
        // HIDDEN orders nie matchują continuous (dark-pool semantyka) — wchodzą
        // bezpośrednio do księgi jako pure-dark liquidity dla cross/auction.
        if (type != OrderType::POST_ONLY && type != OrderType::HIDDEN &&
            !in_auction_mode_) {
            match_against(o);
        }

        // Po matchu:
        if (o->filled_qty >= o->total_qty) {
            // Pełen fill → zwolnij slot (nie wchodzi do księgi)
            o->status = OrderStatus::FILLED;
            const std::uint64_t rid = o->id;
            ++completion_filled_fully_;
            free_order(o);
            return rid;
        }

        // IOC: nie zostawiaj resztek — cancel
        if (type == OrderType::IOC || tif == TimeInForce::IOC) {
            const std::int32_t left = o->remaining_qty();
            o->status = OrderStatus::CANCELLED;
            const std::uint64_t rid = o->id;
            emit(EventType::CANCEL, rid, 0, o->price_ticks, left,
                 OrderStatus::CANCELLED, RejectReason::NONE);
            if (o->filled_qty == 0) ++completion_cancelled_unfilled_;
            else                    ++completion_cancelled_partial_;
            free_order(o);
            return rid;
        }

        // Włóż do księgi (FIFO)
        o->status = OrderStatus::OPEN;
        if (o->filled_qty > 0) o->status = OrderStatus::PARTIALLY_FILLED;
        enqueue_at_level(o);
        id_index_[o->id] = o;

        // Update best bid/ask — TYLKO dla visible orders (HIDDEN nigdy nie
        // pojawia się w L1/L2; jest tylko dla cross/auction match'u).
        if (type != OrderType::HIDDEN) {
            if (o->side == Side::BUY) {
                if (best_bid_ticks_ == NO_BID_TICKS || o->price_ticks > best_bid_ticks_)
                    best_bid_ticks_ = o->price_ticks;
            } else {
                if (best_ask_ticks_ == NO_ASK_TICKS || o->price_ticks < best_ask_ticks_)
                    best_ask_ticks_ = o->price_ticks;
            }
            push_delta('A', o->side == Side::BUY ? 'B' : 'S',
                       o->price_ticks, levels_[o->price_ticks].total_qty);
        }
        return o->id;
    }

    // ====================================================================
    // cancel(order_id) — user cancel; zwraca true gdy znaleziono i usunięto.
    // ====================================================================
    bool cancel(std::uint64_t order_id) noexcept {
        auto it = id_index_.find(order_id);
        if (it == id_index_.end()) return false;
        cancel_internal(it->second, /*emit_event=*/true);
        return true;
    }

    // ====================================================================
    // modify(order_id, new_price, new_qty) — cancel + resubmit, GUBI priority
    // (zgodnie z większością giełd jeśli zmieniasz cenę). Jeśli tylko qty DOWN
    // (down-only), niektóre giełdy zachowują priority; tu zachowuję
    // tylko w przypadku qty DOWN ON SAME PRICE.
    // ====================================================================
    std::uint64_t modify(std::uint64_t order_id,
                          std::int32_t new_price_ticks,
                          std::int32_t new_qty,
                          RejectReason* out_reason = nullptr) noexcept {
        auto it = id_index_.find(order_id);
        if (it == id_index_.end()) {
            if (out_reason) *out_reason = RejectReason::NONE;
            return 0;
        }
        Order* o = it->second;

        // Same-price + qty DOWN → priority-preserving in-place
        // (reguła zgodna z NASDAQ ITCH/OUCH "Decrease" — zachowuje FIFO)
        if (new_price_ticks == o->price_ticks &&
            new_qty < o->total_qty &&
            new_qty > o->filled_qty) {
            const std::int32_t delta = (o->total_qty - new_qty);
            o->total_qty = new_qty;
            o->displayed_qty -= std::min(delta, o->displayed_qty);
            levels_[o->price_ticks].total_qty -= std::min(delta, levels_[o->price_ticks].total_qty);
            ++stats_.total_orders_replaced;
            ++stats_.priority_preserved_mods;
            emit(EventType::REPLACE, o->id, 0, o->price_ticks, new_qty,
                 o->status, RejectReason::NONE);
            return o->id;
        }

        // Cancel + resubmit — gubi priority (price change OR qty UP)
        const Side       side       = o->side;
        const OrderType  type       = o->type;
        const TimeInForce tif       = o->tif;
        const std::uint64_t client_id = o->client_id;
        const std::int32_t  filled  = o->filled_qty;
        cancel_internal(o, /*emit_event=*/false);
        ++stats_.total_orders_replaced;
        ++stats_.priority_lost_mods;
        return submit(side, new_price_ticks, new_qty - filled,
                      type, tif, order_id, client_id, 0, out_reason);
    }

    // ====================================================================
    // L2 depth — `n_levels` najlepszych cen per side
    // ====================================================================
    //
    // Wypełnia bid_out[] i ask_out[] zawijając do `n_levels` lub LEVELS.
    // Zwraca ile faktycznie zwrócono.
    std::int32_t depth(std::int32_t n_levels,
                       DepthLevel* bid_out, DepthLevel* ask_out,
                       std::int32_t* bid_count, std::int32_t* ask_count) const noexcept {
        std::int32_t bn = 0, an = 0;
        if (bid_out && has_bid()) {
            for (std::int32_t p = best_bid_ticks_; p >= 0 && bn < n_levels; --p) {
                const PriceLevel& lvl = levels_[p];
                if (lvl.total_qty > 0) {
                    bid_out[bn] = DepthLevel{p, lvl.total_qty, lvl.order_count};
                    ++bn;
                }
            }
        }
        if (ask_out && has_ask()) {
            for (std::int32_t p = best_ask_ticks_; p < LEVELS && an < n_levels; ++p) {
                const PriceLevel& lvl = levels_[p];
                if (lvl.total_qty > 0) {
                    ask_out[an] = DepthLevel{p, lvl.total_qty, lvl.order_count};
                    ++an;
                }
            }
        }
        if (bid_count) *bid_count = bn;
        if (ask_count) *ask_count = an;
        return bn + an;
    }

    // total_volume_at_price: Σ displayed qty na konkretnym levelu (0 gdy pusty).
    std::int32_t total_volume_at_price(std::int32_t price_ticks) const noexcept {
        if (!in_range(price_ticks)) return 0;
        return levels_[price_ticks].total_qty;
    }

    std::int32_t order_count_at_price(std::int32_t price_ticks) const noexcept {
        if (!in_range(price_ticks)) return 0;
        return levels_[price_ticks].order_count;
    }

    // imbalance_bps: (bid_qty - ask_qty)/(bid+ask) × 10000.
    //   +5000 = same bidy (bardzo buy-heavy)
    //   -5000 = same aski
    //   0     = balans
    // Signal flow — kluczowa metryka order book imbalance dla strategii.
    std::int32_t imbalance_bps() const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        const std::int64_t b = levels_[best_bid_ticks_].total_qty;
        const std::int64_t a = levels_[best_ask_ticks_].total_qty;
        const std::int64_t total = b + a;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((b - a) * 10000 / total);
    }

    // queue_position(order_id): ile zleceń przed nami w FIFO na tym levelu.
    // Strategia market making'u używa do oceny "kiedy mnie wezmą" — jeśli
    // jesteś 1000-szy w kolejce, fill probability jest niska.
    std::int32_t queue_position(std::uint64_t order_id) const noexcept {
        auto it = id_index_.find(order_id);
        if (it == id_index_.end()) return -1;
        Order* o = it->second;
        std::int32_t pos = 0;
        for (Order* p = levels_[o->price_ticks].head; p && p != o; p = p->next_at_level) {
            ++pos;
        }
        return pos;
    }

    // ====================================================================
    // Trade tape — recent executions (ring buffer)
    // ====================================================================
    std::size_t tape_size() const noexcept {
        return std::min(tape_count_, TAPE_CAP);
    }

    // Skopiuj ostatnie `max_n` egzekucji do out[]. Zwraca ile zwrócono
    // (nigdy więcej niż tape_size()).
    std::size_t recent_trades(Trade* out, std::size_t max_n) const noexcept {
        const std::size_t avail = tape_size();
        const std::size_t n = std::min(avail, max_n);
        for (std::size_t i = 0; i < n; ++i) {
            // od najnowszego w tył
            const std::size_t idx = (tape_head_ + TAPE_CAP - 1 - i) % TAPE_CAP;
            out[i] = tape_[idx];
        }
        return n;
    }

    // VWAP z trade tape (volume weighted average price) — w tickach.
    std::int32_t tape_vwap_ticks() const noexcept {
        const std::size_t n = tape_size();
        if (n == 0) return -1;
        std::int64_t price_qty = 0;
        std::int64_t qty       = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - 1 - i) % TAPE_CAP];
            price_qty += static_cast<std::int64_t>(t.price_ticks) * t.qty;
            qty       += t.qty;
        }
        return qty > 0 ? static_cast<std::int32_t>(price_qty / qty) : -1;
    }

    // ====================================================================
    // Snapshot — wire format do recovery
    // ====================================================================
    //
    // Encoder/decoder: serializuje wszystkie active orders do buforu bajtów
    // (per-order POD), zwraca bytes_written. Format wire:
    //   [4 B magic "OBPRO" prefix nadpisany, no]
    //   [4 B version (1)]
    //   [8 B active_orders count]
    //   [N × OrderRecord (66 B each)]
    //
    // To NIE jest delta — to FULL snapshot. Delta zaimplementowana przez
    // event callback z EventType (caller may build incremental log).
    //
    // Zwraca liczbę zapisanych bajtów lub 0 gdy bufor za mały.

    struct __attribute__((packed)) OrderRecord {
        std::uint64_t id;
        std::uint64_t client_id;
        std::int32_t  price_ticks;
        std::int32_t  total_qty;
        std::int32_t  filled_qty;
        std::int32_t  displayed_qty;
        std::uint8_t  side;
        std::uint8_t  type;
        std::uint8_t  tif;
        std::uint8_t  status;
        std::uint64_t submit_ts_ns;
        std::uint64_t expire_ts_ns;
        std::int32_t  stop_trigger_ticks;
        std::int32_t  peg_offset_ticks;
    };

    static constexpr std::uint32_t SNAPSHOT_VERSION = 1;
    static constexpr std::uint32_t SNAPSHOT_MAGIC   = 0x4F42504F;  // "OBPO"

    std::size_t snapshot_size_estimate() const noexcept {
        return 4 + 4 + 8 + active_orders_ * sizeof(OrderRecord);
    }

    std::size_t serialize_snapshot(std::uint8_t* buf, std::size_t cap) const noexcept {
        const std::size_t need = snapshot_size_estimate();
        if (cap < need) return 0;
        std::size_t off = 0;
        std::uint32_t magic   = SNAPSHOT_MAGIC;
        std::uint32_t version = SNAPSHOT_VERSION;
        std::uint64_t count   = active_orders_;
        std::memcpy(buf + off, &magic,   4); off += 4;
        std::memcpy(buf + off, &version, 4); off += 4;
        std::memcpy(buf + off, &count,   8); off += 8;

        // Iteruj po wszystkich levelach (oba sidey), dla każdego po FIFO
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            for (Order* o = levels_[p].head; o; o = o->next_at_level) {
                OrderRecord r{};
                r.id            = o->id;
                r.client_id     = o->client_id;
                r.price_ticks   = o->price_ticks;
                r.total_qty     = o->total_qty;
                r.filled_qty    = o->filled_qty;
                r.displayed_qty = o->displayed_qty;
                r.side          = static_cast<std::uint8_t>(o->side);
                r.type          = static_cast<std::uint8_t>(o->type);
                r.tif           = static_cast<std::uint8_t>(o->tif);
                r.status        = static_cast<std::uint8_t>(o->status);
                r.submit_ts_ns  = o->submit_ts_ns;
                r.expire_ts_ns  = o->expire_ts_ns;
                r.stop_trigger_ticks = o->stop_trigger_ticks;
                r.peg_offset_ticks   = o->peg_offset_ticks;
                std::memcpy(buf + off, &r, sizeof(r));
                off += sizeof(r);
            }
        }
        return off;
    }

    // Wczytaj snapshot — przeładowuje całą księgę. Zwraca true on success.
    bool load_snapshot(const std::uint8_t* buf, std::size_t len) noexcept {
        if (len < 16) return false;
        std::uint32_t magic, version;
        std::uint64_t count;
        std::memcpy(&magic,   buf,      4);
        std::memcpy(&version, buf + 4,  4);
        std::memcpy(&count,   buf + 8,  8);
        if (magic != SNAPSHOT_MAGIC) return false;
        if (version != SNAPSHOT_VERSION) return false;
        if (len < 16 + count * sizeof(OrderRecord)) return false;

        clear();
        std::size_t off = 16;
        for (std::uint64_t i = 0; i < count; ++i) {
            OrderRecord r;
            std::memcpy(&r, buf + off, sizeof(r));
            off += sizeof(r);
            Order* o = alloc_order();
            if (!o) return false;
            o->id            = r.id;
            o->client_id     = r.client_id;
            o->price_ticks   = r.price_ticks;
            o->total_qty     = r.total_qty;
            o->filled_qty    = r.filled_qty;
            o->displayed_qty = r.displayed_qty;
            o->side          = static_cast<Side>(r.side);
            o->type          = static_cast<OrderType>(r.type);
            o->tif           = static_cast<TimeInForce>(r.tif);
            o->status        = static_cast<OrderStatus>(r.status);
            o->submit_ts_ns  = r.submit_ts_ns;
            o->expire_ts_ns  = r.expire_ts_ns;
            o->stop_trigger_ticks = r.stop_trigger_ticks;
            o->peg_offset_ticks   = r.peg_offset_ticks;
            enqueue_at_level(o);
            id_index_[o->id] = o;
            if (o->side == Side::BUY) {
                if (best_bid_ticks_ == NO_BID_TICKS || o->price_ticks > best_bid_ticks_)
                    best_bid_ticks_ = o->price_ticks;
            } else {
                if (best_ask_ticks_ == NO_ASK_TICKS || o->price_ticks < best_ask_ticks_)
                    best_ask_ticks_ = o->price_ticks;
            }
        }
        return true;
    }

    // Pełny reset — księga pusta, statystyki wyczyszczone.
    void clear() noexcept {
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            Order* o = levels_[p].head;
            while (o) {
                Order* next = o->next_at_level;
                free_order(o);
                o = next;
            }
            levels_[p].clear();
        }
        id_index_.clear();
        stop_orders_.clear();
        peg_orders_.clear();
        best_bid_ticks_ = NO_BID_TICKS;
        best_ask_ticks_ = NO_ASK_TICKS;
        last_trade_ticks_ = -1;
        tape_head_ = 0;
        tape_count_ = 0;
    }

    // ====================================================================
    // STOP order management
    // ====================================================================
    //
    // STOP orders nie wchodzą do księgi od razu — czekają w `stop_orders_`
    // aż last_trade_ticks_ przekroczy `stop_trigger_ticks`:
    //   BUY STOP   trigger gdy last_trade >= stop_trigger
    //   SELL STOP  trigger gdy last_trade <= stop_trigger
    // Po triggerze stają się LIMIT (z `price_ticks` jako limit) lub MARKET
    // (gdy `price_ticks == 0`).
    //
    // submit_stop: zarejestruj STOP. Zwraca id albo 0 na error.
    std::uint64_t submit_stop(Side side, std::int32_t trigger_ticks,
                                std::int32_t limit_ticks, std::int32_t qty,
                                std::uint64_t order_id = 0,
                                std::uint64_t client_id = 0,
                                RejectReason* out_reason = nullptr) noexcept {
        auto reject = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            return 0;
        };
        if (qty <= 0) return reject(RejectReason::QTY_ZERO_OR_NEGATIVE);
        if (trigger_ticks < PRICE_MIN_TICKS || trigger_ticks >= LEVELS)
            return reject(RejectReason::PRICE_OUT_OF_RANGE);

        Order* o = alloc_order();
        if (!o) return reject(RejectReason::POOL_EXHAUSTED);
        o->id            = order_id ? order_id : next_order_id_++;
        o->client_id     = client_id;
        o->price_ticks   = limit_ticks;     // 0 = market on trigger
        o->total_qty     = qty;
        o->displayed_qty = qty;
        o->side          = side;
        o->type          = OrderType::STOP;
        o->status        = OrderStatus::NEW;
        o->submit_ts_ns  = mono_ns_now();
        o->stop_trigger_ticks = trigger_ticks;
        stop_orders_.push_back(o);
        id_index_[o->id] = o;
        ++stats_.total_orders_added;
        tally_accept(OrderType::STOP, TimeInForce::DAY);
        emit(EventType::ACCEPT, o->id, 0, trigger_ticks, qty,
             OrderStatus::NEW, RejectReason::NONE);
        return o->id;
    }

    // check_stop_triggers: wywołaj po każdym executed trade. Każdy STOP
    // który spełnia warunek triggera staje się aktywnym LIMIT/MARKET.
    void check_stop_triggers() noexcept {
        if (last_trade_ticks_ < 0 || stop_orders_.empty()) return;
        for (std::size_t i = 0; i < stop_orders_.size(); ) {
            Order* o = stop_orders_[i];
            const bool buy_trigger  = (o->side == Side::BUY)
                                    && (last_trade_ticks_ >= o->stop_trigger_ticks);
            const bool sell_trigger = (o->side == Side::SELL)
                                    && (last_trade_ticks_ <= o->stop_trigger_ticks);
            if (buy_trigger || sell_trigger) {
                // Wyciągnij z queue, zachowaj parametry, resubmit jako LIMIT/MARKET
                const Side       sd  = o->side;
                const std::int32_t lp = o->price_ticks;
                const std::int32_t qy = o->total_qty;
                const std::uint64_t cid = o->client_id;
                const std::uint64_t oid = o->id;
                id_index_.erase(oid);
                free_order(o);
                stop_orders_[i] = stop_orders_.back();
                stop_orders_.pop_back();
                ++stats_.total_stop_triggers;
                // limit_ticks=0 → market: użyj NO_BID/NO_ASK jako limit fallback
                const std::int32_t limit_p = (lp > 0) ? lp
                                              : (sd == Side::BUY ? LEVELS - 1 : 0);
                submit(sd, limit_p, qy, OrderType::LIMIT, TimeInForce::DAY,
                       oid, cid, 0, nullptr);
            } else {
                ++i;
            }
        }
    }

    std::size_t stop_orders_count() const noexcept { return stop_orders_.size(); }
    std::int32_t last_trade_ticks() const noexcept { return last_trade_ticks_; }

    // ====================================================================
    // PEG order management
    // ====================================================================
    //
    // PEG order pegged do best bid/ask + offset_ticks. Po każdej zmianie top
    // of book, peg orders są przemieszczane na nowy price level (re-quote).
    //
    // Konwencja:
    //   BUY PEG  → pinned to best_bid + peg_offset (offset zwykle ≤ 0)
    //   SELL PEG → pinned to best_ask + peg_offset
    std::uint64_t submit_peg(Side side, std::int32_t peg_offset_ticks,
                              std::int32_t qty,
                              std::uint64_t order_id = 0,
                              std::uint64_t client_id = 0,
                              RejectReason* out_reason = nullptr) noexcept {
        auto reject = [&](RejectReason r) -> std::uint64_t {
            if (out_reason) *out_reason = r;
            ++stats_.total_orders_rejected;
            tally_rejection(r);
            return 0;
        };
        if (qty <= 0) return reject(RejectReason::QTY_ZERO_OR_NEGATIVE);
        // Initial price compute
        std::int32_t initial_price;
        if (side == Side::BUY) {
            if (!has_bid()) initial_price = 0;
            else            initial_price = best_bid_ticks_ + peg_offset_ticks;
        } else {
            if (!has_ask()) initial_price = LEVELS - 1;
            else            initial_price = best_ask_ticks_ + peg_offset_ticks;
        }
        if (initial_price < 0 || initial_price >= LEVELS)
            return reject(RejectReason::PRICE_OUT_OF_RANGE);

        Order* o = alloc_order();
        if (!o) return reject(RejectReason::POOL_EXHAUSTED);
        o->id            = order_id ? order_id : next_order_id_++;
        o->client_id     = client_id;
        o->price_ticks   = initial_price;
        o->total_qty     = qty;
        o->displayed_qty = qty;
        o->side          = side;
        o->type          = OrderType::PEG;
        o->status        = OrderStatus::OPEN;
        o->submit_ts_ns  = mono_ns_now();
        o->peg_offset_ticks = peg_offset_ticks;
        enqueue_at_level(o);
        id_index_[o->id] = o;
        peg_orders_.push_back(o);
        if (side == Side::BUY) {
            if (best_bid_ticks_ == NO_BID_TICKS || initial_price > best_bid_ticks_)
                best_bid_ticks_ = initial_price;
        } else {
            if (best_ask_ticks_ == NO_ASK_TICKS || initial_price < best_ask_ticks_)
                best_ask_ticks_ = initial_price;
        }
        ++stats_.total_orders_added;
        tally_accept(OrderType::PEG, TimeInForce::DAY);
        emit(EventType::ACCEPT, o->id, 0, initial_price, qty,
             OrderStatus::OPEN, RejectReason::NONE);
        return o->id;
    }

    // reprice_pegs: wywołaj po zmianie top of book. Każdy peg, którego target
    // price się rozjechał z aktualnym, zostaje przeniesiony.
    void reprice_pegs() noexcept {
        if (peg_orders_.empty()) return;
        for (std::size_t i = 0; i < peg_orders_.size(); ++i) {
            Order* o = peg_orders_[i];
            if (!o->is_active()) continue;
            std::int32_t target;
            if (o->side == Side::BUY) {
                if (!has_bid()) continue;
                target = best_bid_ticks_ + o->peg_offset_ticks;
            } else {
                if (!has_ask()) continue;
                target = best_ask_ticks_ + o->peg_offset_ticks;
            }
            if (target < 0 || target >= LEVELS) continue;
            if (target == o->price_ticks) continue;
            // Move: unlink + insert at new level (priority lost)
            const std::int32_t old_price = o->price_ticks;
            unlink_from_level(o);
            if (levels_[old_price].empty()) {
                if (o->side == Side::BUY && old_price == best_bid_ticks_)
                    refresh_best_bid_from(old_price - 1);
                if (o->side == Side::SELL && old_price == best_ask_ticks_)
                    refresh_best_ask_from(old_price + 1);
            }
            o->price_ticks = target;
            enqueue_at_level(o);
            if (o->side == Side::BUY && target > best_bid_ticks_)
                best_bid_ticks_ = target;
            if (o->side == Side::SELL && target < best_ask_ticks_)
                best_ask_ticks_ = target;
            ++stats_.total_peg_reprices;
        }
    }

    std::size_t peg_orders_count() const noexcept { return peg_orders_.size(); }

    // ====================================================================
    // Mass cancel — kill switch by client_id
    // ====================================================================
    //
    // W realnym HFT to obowiązkowy guard rail: gdy ryzyko trip'uje, wszystkie
    // otwarte zlecenia danego konta muszą być instant kasowane. Zwraca ile
    // anulowano.
    std::size_t mass_cancel(std::uint64_t client_id) noexcept {
        std::size_t cancelled = 0;
        // Iterate by collecting ids first — modyfikacja id_index_ podczas
        // iteracji UB.
        std::vector<Order*> to_cancel;
        to_cancel.reserve(id_index_.size());
        for (auto& kv : id_index_) {
            if (kv.second->client_id == client_id && kv.second->is_active()) {
                to_cancel.push_back(kv.second);
            }
        }
        for (Order* o : to_cancel) {
            cancel_internal(o, /*emit_event=*/true);
            ++cancelled;
        }
        // STOP orders też (są w id_index_ więc już objęte)
        // Peg orders były objęte przez id_index_ + is_active() check
        stats_.total_mass_cancels += cancelled;
        return cancelled;
    }

    // ====================================================================
    // GTD expiry sweep
    // ====================================================================
    //
    // Wywołaj periodycznie (raz na sekundę / okazjonalnie). Każdy GTD którego
    // expire_ts_ns_ < now_ns jest kasowany jako EXPIRE event. Zwraca ile.
    std::size_t expire_gtd(std::uint64_t now_ns) noexcept {
        std::vector<Order*> to_expire;
        for (auto& kv : id_index_) {
            Order* o = kv.second;
            if (o->tif == TimeInForce::GTD && o->is_active() &&
                o->expire_ts_ns > 0 && o->expire_ts_ns <= now_ns) {
                to_expire.push_back(o);
            }
        }
        for (Order* o : to_expire) {
            const std::int32_t left = o->remaining_qty();
            const std::int32_t price = o->price_ticks;
            const std::uint64_t oid = o->id;
            unlink_from_level(o);
            if (levels_[price].empty()) {
                if (o->side == Side::BUY && price == best_bid_ticks_)
                    refresh_best_bid_from(price - 1);
                if (o->side == Side::SELL && price == best_ask_ticks_)
                    refresh_best_ask_from(price + 1);
            }
            o->status = OrderStatus::EXPIRED;
            emit(EventType::EXPIRE, oid, 0, price, left,
                 OrderStatus::EXPIRED, RejectReason::NONE);
            id_index_.erase(oid);
            if (o->filled_qty == 0) ++completion_expired_unfilled_;
            else                    ++completion_expired_partial_;
            free_order(o);
            ++stats_.total_orders_expired;
        }
        return to_expire.size();
    }

    // ====================================================================
    // Walk-the-book — preview matching bez modyfikacji księgi
    // ====================================================================
    //
    // Sprawdza ile qty z `desired` można od razu wypełnić do `limit_price`
    // (jak by submit(IOC) zrobił). Zwraca filled_qty + average_price.
    // Caller używa do pre-trade analysis ("ile ruchu jest na top 5 levels").
    struct WalkResult {
        std::int32_t  fillable_qty;
        std::int32_t  avg_price_ticks;     // ważona qty
        std::int32_t  levels_touched;
        std::int32_t  worst_price_ticks;   // najgorszy level który zostałby przeszedł
    };
    WalkResult walk_the_book(Side side, std::int32_t desired_qty,
                              std::int32_t limit_price) const noexcept {
        WalkResult r{};
        std::int64_t qty_price_sum = 0;
        std::int32_t remaining = desired_qty;
        if (side == Side::BUY) {
            for (std::int32_t p = best_ask_ticks_;
                 p <= limit_price && p < LEVELS && remaining > 0; ++p) {
                const std::int32_t avail = levels_[p].total_qty;
                if (avail <= 0) continue;
                const std::int32_t take = std::min(avail, remaining);
                qty_price_sum += static_cast<std::int64_t>(take) * p;
                r.fillable_qty += take;
                remaining -= take;
                ++r.levels_touched;
                r.worst_price_ticks = p;
            }
        } else {
            for (std::int32_t p = best_bid_ticks_;
                 p >= limit_price && p >= 0 && remaining > 0; --p) {
                const std::int32_t avail = levels_[p].total_qty;
                if (avail <= 0) continue;
                const std::int32_t take = std::min(avail, remaining);
                qty_price_sum += static_cast<std::int64_t>(take) * p;
                r.fillable_qty += take;
                remaining -= take;
                ++r.levels_touched;
                r.worst_price_ticks = p;
            }
        }
        r.avg_price_ticks = (r.fillable_qty > 0)
            ? static_cast<std::int32_t>(qty_price_sum / r.fillable_qty)
            : 0;
        return r;
    }

    // ====================================================================
    // Order Flow Imbalance (OFI) — przemysłowy signal
    // ====================================================================
    //
    // OFI mierzy NETTO zmianę top-of-book qty od ostatniej obserwacji.
    //   ΔOFI = Δbid_qty(jeśli bid level nie spadł) - Δask_qty(jeśli ask level nie wzrósł)
    // Dodatnia OFI = buy pressure, ujemna = sell pressure.
    //
    // Caller wywołuje sample_ofi() periodycznie; zwraca delta i resetuje stan.
    std::int64_t sample_ofi() noexcept {
        const std::int32_t cur_bid_qty = has_bid() ? levels_[best_bid_ticks_].total_qty : 0;
        const std::int32_t cur_ask_qty = has_ask() ? levels_[best_ask_ticks_].total_qty : 0;
        const std::int32_t cur_bid     = has_bid() ? best_bid_ticks_ : 0;
        const std::int32_t cur_ask     = has_ask() ? best_ask_ticks_ : 0;

        std::int64_t delta = 0;
        if (cur_bid >= last_ofi_bid_ticks_) delta += (cur_bid_qty - last_ofi_bid_qty_);
        if (cur_ask <= last_ofi_ask_ticks_) delta -= (cur_ask_qty - last_ofi_ask_qty_);

        last_ofi_bid_ticks_ = cur_bid;
        last_ofi_ask_ticks_ = cur_ask;
        last_ofi_bid_qty_   = cur_bid_qty;
        last_ofi_ask_qty_   = cur_ask_qty;
        cum_ofi_ += delta;
        return delta;
    }
    std::int64_t cumulative_ofi() const noexcept { return cum_ofi_; }

    // ====================================================================
    // Cumulative volume profile (volume at price)
    // ====================================================================
    //
    // Wypełnia `out` (rozmiaru capacity_levels): wektor (price, total_qty)
    // dla NIEPUSTYCH levels w `[min_ticks, max_ticks]`. Zwraca ile zapisano.
    // Sortowane rosnąco po cenie.
    std::int32_t volume_profile(std::int32_t min_ticks, std::int32_t max_ticks,
                                  DepthLevel* out, std::int32_t capacity) const noexcept {
        std::int32_t n = 0;
        const std::int32_t lo = std::max(min_ticks, PRICE_MIN_TICKS);
        const std::int32_t hi = std::min(max_ticks, LEVELS - 1);
        for (std::int32_t p = lo; p <= hi && n < capacity; ++p) {
            const PriceLevel& lvl = levels_[p];
            if (lvl.total_qty > 0) {
                out[n++] = DepthLevel{p, lvl.total_qty, lvl.order_count};
            }
        }
        return n;
    }

    // ====================================================================
    // Maker-taker fee accounting (basis points)
    // ====================================================================
    //
    // Konwencja venue: maker dostaje rebate (bps_maker zwykle ujemne dla rebate),
    // taker płaci fee. Liczone w cumulative bps × volume. Wywołujący decyduje
    // czy chce zaaplikować — domyślnie nie liczone (= 0 bps).
    void set_fee_bps(std::int32_t taker_bps, std::int32_t maker_bps) noexcept {
        taker_fee_bps_ = taker_bps;
        maker_fee_bps_ = maker_bps;
    }
    std::int64_t total_taker_fees_basis() const noexcept { return cum_taker_fees_; }
    std::int64_t total_maker_fees_basis() const noexcept { return cum_maker_fees_; }

    // ====================================================================
    // Auction matching (opening / closing cross)
    // ====================================================================
    //
    // Auction = single-price match (nie ciągłe matching). Cała kolekcja
    // zleceń jest matched at one clearing price wybranym tak żeby zmaksymalizować
    // przehandlowany wolumen. Klasyczny algorytm uctioned giełd:
    //
    //   For each candidate price p (od najniższego do najwyższego):
    //     cum_bid(p) = sum of bid qty at price >= p     (skłonność do kupna ≥ p)
    //     cum_ask(p) = sum of ask qty at price <= p     (skłonność do sprzedaży ≤ p)
    //     matched(p) = min(cum_bid(p), cum_ask(p))
    //
    //   Clearing price = arg max matched(p). Przy ties: NASDAQ wybiera średnią,
    //   NYSE last trade. Tu używamy mid-range tie-breakera.
    //
    //   Po znalezieniu clearing — wszystkie zlecenia z BID >= clearing oraz
    //   ASK <= clearing są wypełnione (do limitu min(cum_bid, cum_ask)) at
    //   clearing price. Pozostali zostają w księdze do continuous mode'u.
    //
    // Wynik:
    //   AuctionResult{clearing_price, matched_qty, surplus_bid, surplus_ask}.
    //
    // To używane przy: opening cross (przed 9:30 zbierane są ordery, o 9:30
    // jeden cross), closing cross (15:55-16:00 → 16:00), trading halt reopen.
    struct AuctionResult {
        std::int32_t  clearing_price_ticks;
        std::int32_t  matched_qty;
        std::int32_t  surplus_bid_qty;     // unmatched bid po cleared price
        std::int32_t  surplus_ask_qty;     // unmatched ask po cleared price
        bool          executed;
    };

    AuctionResult run_auction() noexcept {
        AuctionResult res{};
        res.clearing_price_ticks = -1;
        if (!has_bid() || !has_ask()) return res;

        // Walk cena range gdzie obie strony przeszły siebie — w continuous trade
        // tego nie ma (best_bid < best_ask), ale przy auction queue obie strony
        // mogą się przeciąć. Iterujemy od max(best_bid_ticks_) w dół do
        // min(best_ask_ticks_) — to interesujący zakres.
        std::int32_t best_clearing = -1;
        std::int32_t best_matched  = -1;
        std::int32_t best_imbalance = INT32_MAX;

        // cum_bid(p) liczone od HIGH w dół, cum_ask(p) od LOW w górę
        // Najpierw zlicz total per side w zakresie.
        const std::int32_t lo = std::min(best_bid_ticks_, best_ask_ticks_);
        const std::int32_t hi = std::max(best_bid_ticks_, best_ask_ticks_);

        for (std::int32_t p = lo; p <= hi; ++p) {
            // cum_bid(p) = Σ BID qty na poziomach ≥ p.
            // Walk po head listach z side filter (levels_ trzymają mix bid+ask
            // przy auction queue gdy obie strony przecinają sobie poziomy).
            std::int32_t cum_bid = 0;
            for (std::int32_t bp = hi; bp >= p; --bp) {
                for (Order* o = levels_[bp].head; o; o = o->next_at_level) {
                    if (o->side == Side::BUY && o->is_active())
                        cum_bid += o->displayed_qty;
                }
            }
            std::int32_t cum_ask = 0;
            for (std::int32_t ap = lo; ap <= p; ++ap) {
                for (Order* o = levels_[ap].head; o; o = o->next_at_level) {
                    if (o->side == Side::SELL && o->is_active())
                        cum_ask += o->displayed_qty;
                }
            }
            const std::int32_t matched = std::min(cum_bid, cum_ask);
            const std::int32_t imbalance = std::abs(cum_bid - cum_ask);
            if (matched > best_matched ||
                (matched == best_matched && imbalance < best_imbalance)) {
                best_matched = matched;
                best_clearing = p;
                best_imbalance = imbalance;
                res.surplus_bid_qty = cum_bid - matched;
                res.surplus_ask_qty = cum_ask - matched;
            }
        }
        if (best_matched <= 0) return res;

        res.clearing_price_ticks = best_clearing;
        res.matched_qty          = best_matched;
        res.executed             = true;

        // Faktyczne wypełnienie at clearing price — FIFO per side
        // (dla determinism, oversubscription = older orders win)
        std::int32_t bid_remaining = best_matched;
        std::int32_t ask_remaining = best_matched;
        const std::uint64_t ts = mono_ns_now();

        // Wykonaj BIDS od najwyższej ceny w dół, ale wszystkie po clearing_price
        for (std::int32_t p = hi; p >= best_clearing && bid_remaining > 0; --p) {
            Order* o = levels_[p].head;
            while (o && bid_remaining > 0) {
                Order* next = o->next_at_level;
                if (o->side == Side::BUY && o->is_active()) {
                    const std::int32_t take =
                        std::min(o->remaining_qty(), bid_remaining);
                    o->filled_qty += take;
                    bid_remaining -= take;
                    if (o->filled_qty >= o->total_qty) {
                        o->status = OrderStatus::FILLED;
                        emit(EventType::FILL, o->id, 0, best_clearing, take,
                             OrderStatus::FILLED, RejectReason::NONE);
                        unlink_from_level(o);
                        id_index_.erase(o->id);
                        free_order(o);
                    } else {
                        o->status = OrderStatus::PARTIALLY_FILLED;
                        emit(EventType::FILL, o->id, 0, best_clearing, take,
                             OrderStatus::PARTIALLY_FILLED, RejectReason::NONE);
                    }
                }
                o = next;
            }
        }
        // ASKS od najniższej w górę do clearing_price
        for (std::int32_t p = lo; p <= best_clearing && ask_remaining > 0; ++p) {
            Order* o = levels_[p].head;
            while (o && ask_remaining > 0) {
                Order* next = o->next_at_level;
                if (o->side == Side::SELL && o->is_active()) {
                    const std::int32_t take =
                        std::min(o->remaining_qty(), ask_remaining);
                    o->filled_qty += take;
                    ask_remaining -= take;
                    record_trade(o->id, 0, best_clearing, take,
                                 Side::BUY, ts);
                    if (o->filled_qty >= o->total_qty) {
                        o->status = OrderStatus::FILLED;
                        emit(EventType::FILL, o->id, 0, best_clearing, take,
                             OrderStatus::FILLED, RejectReason::NONE);
                        unlink_from_level(o);
                        id_index_.erase(o->id);
                        free_order(o);
                    } else {
                        o->status = OrderStatus::PARTIALLY_FILLED;
                        emit(EventType::FILL, o->id, 0, best_clearing, take,
                             OrderStatus::PARTIALLY_FILLED, RejectReason::NONE);
                    }
                }
                o = next;
            }
        }

        refresh_best_bid_from(LEVELS - 1);
        refresh_best_ask_from(0);
        ++stats_.total_auctions_executed;
        return res;
    }

    // ====================================================================
    // L2 incremental delta protocol
    // ====================================================================
    //
    // Wire format dla rozsyłania zmian top-of-book i depth do klientów
    // (alternative dla full snapshot). Format inspirowany ITCH 5.0 + NASDAQ
    // BookFeed:
    //
    //   DeltaMessage:
    //     [0]    type:    'A' = add, 'D' = delete, 'M' = modify, 'T' = trade
    //     [1]    side:    'B' = bid, 'S' = ask, 'X' = both (na trade)
    //     [2..5] price_ticks (int32 LE)
    //     [6..9] new_qty      (int32 LE) — dla M to NEW qty na tym levelu
    //     [10..17] sequence_no (uint64 LE) — monotonic
    //
    // Caller wywołuje pop_delta_queue() po każdej operacji żeby zabrać
    // accumulated deltas i wysłać po sieci. Wewnętrzna kolejka FIFO.
    struct DeltaMessage {
        char          type;       // A/D/M/T
        char          side;       // B/S/X
        std::int32_t  price_ticks;
        std::int32_t  new_qty;
        std::uint64_t sequence_no;
    };
    static constexpr std::size_t DELTA_WIRE_SIZE = 18;

    void enable_delta_queue(bool on) noexcept { delta_queue_enabled_ = on; }
    bool delta_queue_enabled() const noexcept { return delta_queue_enabled_; }
    std::size_t delta_queue_size() const noexcept { return delta_queue_.size(); }

    // pop_delta_queue: skopiuj do `out` (max max_n), wyczyść z kolejki.
    // Zwraca ile faktycznie pobrano.
    std::size_t pop_delta_queue(DeltaMessage* out, std::size_t max_n) noexcept {
        const std::size_t n = std::min(max_n, delta_queue_.size());
        for (std::size_t i = 0; i < n; ++i) out[i] = delta_queue_[i];
        delta_queue_.erase(delta_queue_.begin(),
                            delta_queue_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    // serialize_delta: pojedyncza wiadomość → 18 bajtów little-endian.
    static std::size_t serialize_delta(const DeltaMessage& d,
                                         std::uint8_t* buf) noexcept {
        buf[0] = static_cast<std::uint8_t>(d.type);
        buf[1] = static_cast<std::uint8_t>(d.side);
        std::memcpy(buf + 2,  &d.price_ticks, 4);
        std::memcpy(buf + 6,  &d.new_qty,     4);
        std::memcpy(buf + 10, &d.sequence_no, 8);
        return DELTA_WIRE_SIZE;
    }

    static bool deserialize_delta(const std::uint8_t* buf, std::size_t len,
                                    DeltaMessage& d) noexcept {
        if (len < DELTA_WIRE_SIZE) return false;
        d.type = static_cast<char>(buf[0]);
        d.side = static_cast<char>(buf[1]);
        std::memcpy(&d.price_ticks, buf + 2,  4);
        std::memcpy(&d.new_qty,     buf + 6,  4);
        std::memcpy(&d.sequence_no, buf + 10, 8);
        return true;
    }

    // ====================================================================
    // Internal — push delta from book mutations
    // ====================================================================
    void push_delta(char type, char side, std::int32_t price_ticks,
                     std::int32_t new_qty) noexcept {
        if (!delta_queue_enabled_) return;
        DeltaMessage d{type, side, price_ticks, new_qty, ++delta_seq_};
        delta_queue_.push_back(d);
    }

private:
    // Storage extension (rozszerzenia używane w 2nd-pass features)
    std::vector<Order*>  stop_orders_;   // czekają na trigger
    std::vector<Order*>  peg_orders_;    // do reprice on book change
    std::int32_t         last_trade_ticks_ = -1;

    // OFI state
    std::int32_t  last_ofi_bid_ticks_ = 0;
    std::int32_t  last_ofi_ask_ticks_ = 0;
    std::int32_t  last_ofi_bid_qty_   = 0;
    std::int32_t  last_ofi_ask_qty_   = 0;
    std::int64_t  cum_ofi_ = 0;

    // Fees state
    std::int32_t  taker_fee_bps_   = 0;
    std::int32_t  maker_fee_bps_   = 0;
    std::int64_t  cum_taker_fees_  = 0;
    std::int64_t  cum_maker_fees_  = 0;

    // L2 delta queue state
    bool                      delta_queue_enabled_ = false;
    std::vector<DeltaMessage> delta_queue_;
    std::uint64_t             delta_seq_ = 0;

    // Auction mode — gdy true, submit() pomija match_against (orders sit
    // w księdze do later batch match przez run_auction).
    bool                      in_auction_mode_ = false;

    // Halt state — trading halt (LULD, news pending, technical issue).
    // Gdy halted_, każdy submit → REJECT(HALTED). Cancel zlecenia wciąż OK.
    bool                      halted_ = false;
    char                      halt_reason_[32]{};

    // Audit log — opt-in chronological record wszystkich book mutations.
    // Używane do replay'u, forensics, compliance review (SEC 17a-4).
public:
    struct AuditRecord {
        std::uint64_t ts_ns;
        std::uint64_t seq_no;
        std::uint64_t order_id;
        std::int32_t  price_ticks;
        std::int32_t  qty;
        EventType     event;       // ACCEPT/REJECT/FILL/CANCEL/EXPIRE/REPLACE
        Side          side;
        OrderStatus   result;
    };
private:
    bool                      audit_enabled_ = false;
    std::vector<AuditRecord>  audit_log_;
    std::uint64_t             audit_seq_ = 0;

    // LULD (Limit Up Limit Down) — auto-halt mechanizm SEC Rule 605.
    // Quote poza band'ami auto-rejected lub auto-haltuje book.
    bool                      luld_enabled_ = false;
    std::int32_t              luld_low_ticks_  = 0;
    std::int32_t              luld_high_ticks_ = 0;
    bool                      luld_auto_halt_  = false;   // true → breach halt'uje
    std::uint64_t             luld_breaches_   = 0;

    void push_audit(EventType ev, std::uint64_t order_id, Side side,
                     std::int32_t price_ticks, std::int32_t qty,
                     OrderStatus result) noexcept {
        if (!audit_enabled_) return;
        AuditRecord r{mono_ns_now(), ++audit_seq_, order_id, price_ticks, qty,
                       ev, side, result};
        audit_log_.push_back(r);
    }

public:
    // ====================================================================
    // LULD (Limit Up / Limit Down) — SEC Rule 605 circuit breaker
    // ====================================================================
    //
    // Quote poza band'ami → REJECT(LULD_BAND_BREACH).
    // Jeśli auto_halt=true, breach też haltuje book (5 min standard SEC).
    //
    // Real-world: dla S&P 500 stocks 5% LULD band podczas regularnej sesji,
    // 10% pierwsze 15 min. Trigger po jednej breach = pauza market data + halt.
    void set_luld_bands(std::int32_t low_ticks, std::int32_t high_ticks,
                         bool auto_halt = true) noexcept {
        luld_enabled_    = true;
        luld_low_ticks_  = low_ticks;
        luld_high_ticks_ = high_ticks;
        luld_auto_halt_  = auto_halt;
    }
    void disable_luld() noexcept { luld_enabled_ = false; }
    bool luld_enabled() const noexcept { return luld_enabled_; }
    std::int32_t luld_low()  const noexcept { return luld_low_ticks_; }
    std::int32_t luld_high() const noexcept { return luld_high_ticks_; }
    std::uint64_t luld_breaches() const noexcept { return luld_breaches_; }

    // ====================================================================
    // MIFID II RTS27/28 best-execution metrics
    // ====================================================================
    //
    // Regulacyjny output dla EU venue reporting. Tracking continuous:
    //   - effective_spread = 2 × |exec_price - mid_at_exec| (per execution)
    //   - realized_spread  = 2 × |exec_price - mid_post_n_seconds| (proxy: trade-to-trade)
    //   - num_executions, total_volume, total_notional
    //
    // Caller wywołuje get_mifid_metrics() na koniec sesji żeby wygenerować raport.
    struct MIFIDMetrics {
        std::uint64_t num_executions          = 0;
        std::uint64_t total_volume            = 0;
        std::int64_t  total_notional_ticks    = 0;
        std::int64_t  sum_effective_spread    = 0;   // ticks × qty
        std::int64_t  sum_signed_price_impact = 0;   // realized spread proxy
    };
    MIFIDMetrics get_mifid_metrics() const noexcept { return mifid_; }
    void reset_mifid_metrics() noexcept { mifid_ = MIFIDMetrics{}; }
    void enable_mifid_metrics(bool on) noexcept { mifid_enabled_ = on; }
    bool mifid_metrics_enabled() const noexcept { return mifid_enabled_; }

private:
    MIFIDMetrics  mifid_{};
    bool          mifid_enabled_ = false;

public:
    // ====================================================================
    // Quote stuffing detection
    // ====================================================================
    //
    // Stuffing = wysyłka tysięcy ordersów per sekunda + natychmiastowe
    // cancele, żeby symbolu nie dało się odczytać w real time. SEC Rule
    // 15c3-5: każdy venue musi to wykrywać i flagować.
    //
    // Tu trzymamy per-client_id licznik cancel'i w sliding window (last
    // N samples). Powyżej threshold → emit STUFFING_FLAGGED event +
    // stats.total_stuffing_flags.
    void set_stuffing_threshold(std::uint32_t cancels_per_sec_threshold) noexcept {
        stuffing_threshold_ = cancels_per_sec_threshold;
    }
    std::uint32_t stuffing_threshold() const noexcept { return stuffing_threshold_; }
    bool is_stuffing_flagged(std::uint64_t client_id) const noexcept {
        const auto it = stuffing_flagged_.find(client_id);
        return it != stuffing_flagged_.end() && it->second;
    }
    std::uint64_t total_stuffing_flags() const noexcept { return total_stuffing_flags_; }

    // Reset stuffing window per client (np. po manual review).
    void clear_stuffing_flag(std::uint64_t client_id) noexcept {
        stuffing_flagged_[client_id] = false;
        cancel_counters_[client_id]  = 0;
    }

    // ====================================================================
    // Per-account exposure tracking
    // ====================================================================
    //
    // Per client_id: net qty (BUY - SELL) open + filled, plus gross qty.
    // Risk team używa do real-time monitoring expozycji per account.
    //
    // Tracked w submit/cancel/fill events. Caller wywołuje
    // get_account_exposure(client_id) — zwraca AccountExposure struct.
    struct AccountExposure {
        std::int64_t  open_buy_qty       = 0;     // bid orders w księdze
        std::int64_t  open_sell_qty      = 0;     // ask orders w księdze
        std::int64_t  filled_net_qty     = 0;     // realizowana pozycja
        std::int64_t  filled_gross_volume = 0;    // gross qty traded
        std::uint64_t orders_submitted    = 0;
        std::uint64_t orders_cancelled    = 0;
        std::uint64_t fills_received      = 0;
        // Aggressive (taker) vs passive (maker) volume — przydatne dla rebates
        // i toxicity scoring per account.
        std::uint64_t aggressive_volume   = 0;    // ta strona była taker
        std::uint64_t passive_volume      = 0;    // ta strona była maker
    };
    AccountExposure get_account_exposure(std::uint64_t client_id) const noexcept {
        const auto it = account_exposure_.find(client_id);
        return it == account_exposure_.end() ? AccountExposure{} : it->second;
    }
    // Aggressive ratio = taker_volume / (taker + maker). 1.0 = pure taker
    // (płaci taker fee — wysoki cost); 0.0 = pure maker (zbiera rebate).
    double aggressive_ratio_for(std::uint64_t client_id) const noexcept {
        const auto ex = get_account_exposure(client_id);
        const std::uint64_t total = ex.aggressive_volume + ex.passive_volume;
        if (total == 0) return 0.0;
        return static_cast<double>(ex.aggressive_volume) /
               static_cast<double>(total);
    }
    // Per-account cancel-to-fill ratio.
    double cancel_to_fill_ratio_for(std::uint64_t client_id) const noexcept {
        const auto ex = get_account_exposure(client_id);
        if (ex.fills_received == 0) return 0.0;
        return static_cast<double>(ex.orders_cancelled) /
               static_cast<double>(ex.fills_received);
    }

private:
    // Helper hooks dla aggressor vs passive volume tagging.
    void tag_aggressor_volume(std::uint64_t cid, std::int32_t qty) noexcept {
        if (cid == 0) return;
        account_exposure_[cid].aggressive_volume += static_cast<std::uint64_t>(qty);
    }
    void tag_passive_volume(std::uint64_t cid, std::int32_t qty) noexcept {
        if (cid == 0) return;
        account_exposure_[cid].passive_volume += static_cast<std::uint64_t>(qty);
    }

private:
    std::unordered_map<std::uint64_t, AccountExposure> account_exposure_;

    // Stuffing detection state
    std::uint32_t                            stuffing_threshold_ = 0;
    std::unordered_map<std::uint64_t, bool>  stuffing_flagged_;
    std::unordered_map<std::uint64_t, std::uint32_t> cancel_counters_;
    std::uint64_t                            total_stuffing_flags_ = 0;

    // update_exposure helpers — wywoływane z submit/cancel/fill hooks.
    void exposure_on_submit(std::uint64_t cid, Side side, std::int32_t qty) noexcept {
        if (cid == 0) return;
        AccountExposure& ex = account_exposure_[cid];
        if (side == Side::BUY) ex.open_buy_qty  += qty;
        else                   ex.open_sell_qty += qty;
        ++ex.orders_submitted;
    }
    void exposure_on_cancel(std::uint64_t cid, Side side, std::int32_t remaining) noexcept {
        if (cid == 0) return;
        AccountExposure& ex = account_exposure_[cid];
        if (side == Side::BUY) ex.open_buy_qty  -= remaining;
        else                   ex.open_sell_qty -= remaining;
        ++ex.orders_cancelled;
        // Stuffing check
        if (stuffing_threshold_ > 0) {
            const auto cnt = ++cancel_counters_[cid];
            if (cnt > stuffing_threshold_ && !stuffing_flagged_[cid]) {
                stuffing_flagged_[cid] = true;
                ++total_stuffing_flags_;
            }
        }
    }
    void exposure_on_fill(std::uint64_t cid, Side side, std::int32_t qty) noexcept {
        if (cid == 0) return;
        AccountExposure& ex = account_exposure_[cid];
        if (side == Side::BUY) {
            ex.open_buy_qty   -= qty;
            ex.filled_net_qty += qty;
        } else {
            ex.open_sell_qty  -= qty;
            ex.filled_net_qty -= qty;
        }
        ex.filled_gross_volume += qty;
        ++ex.fills_received;
    }

public:
    // ====================================================================
    // Trade size distribution + TWAP + reference price drift
    // ====================================================================
    //
    // Trade size distribution: classify executions w 4 segments dla detection
    // retail (small) vs institutional flow (block):
    //   SMALL   ≤ 100
    //   MEDIUM  101..1000
    //   LARGE   1001..10000
    //   BLOCK   > 10000
    // Per-segment counter + volume sum.
    struct TradeSizeDistribution {
        std::uint64_t  small_count    = 0;   // ≤100
        std::uint64_t  medium_count   = 0;   // 101-1000
        std::uint64_t  large_count    = 0;   // 1001-10000
        std::uint64_t  block_count    = 0;   // >10000
        std::uint64_t  small_volume   = 0;
        std::uint64_t  medium_volume  = 0;
        std::uint64_t  large_volume   = 0;
        std::uint64_t  block_volume   = 0;
    };

    TradeSizeDistribution get_size_distribution() const noexcept {
        return size_dist_;
    }
    void reset_size_distribution() noexcept { size_dist_ = TradeSizeDistribution{}; }

    // TWAP — time-weighted average price z trade tape.
    // Inaczej niż tape_vwap (volume-weighted), TWAP traktuje każdy trade
    // jednakowo. Używany do detection wash-trading (gdy wolumeny nierówne
    // ale TWAP =VWAP, podejrzanie).
    std::int32_t tape_twap_ticks() const noexcept {
        const std::size_t n = tape_size();
        if (n == 0) return -1;
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - 1 - i) % TAPE_CAP];
            sum += t.price_ticks;
        }
        return static_cast<std::int32_t>(sum / static_cast<std::int64_t>(n));
    }

    // Reference price + drift detection.
    //
    // set_reference_price(ticks) — ustal anchor (np. previous close, opening cross,
    // SIP NBBO mid). Potem reference_drift_bps() zwraca |mid - ref| / ref × 10000.
    // Drift > X bps może triggerować halt review.
    void set_reference_price(std::int32_t ref_ticks) noexcept {
        reference_price_ticks_ = ref_ticks;
        reference_set_ = true;
    }
    bool has_reference_price() const noexcept { return reference_set_; }
    std::int32_t reference_price_ticks() const noexcept { return reference_price_ticks_; }

    // reference_drift_bps: bps deviation mid od reference. Zwraca -1 gdy
    // ref nieustalone lub brak mid.
    std::int32_t reference_drift_bps() const noexcept {
        if (!reference_set_) return -1;
        const std::int32_t m = mid_ticks();
        if (m < 0 || reference_price_ticks_ <= 0) return -1;
        const std::int64_t diff = std::abs(static_cast<std::int64_t>(m) - reference_price_ticks_);
        return static_cast<std::int32_t>(diff * 10000 / reference_price_ticks_);
    }

    // Order rejection rate per client.
    double rejection_rate(std::uint64_t client_id) const noexcept {
        const auto it = account_exposure_.find(client_id);
        if (it == account_exposure_.end() || it->second.orders_submitted == 0)
            return 0.0;
        const auto rejected_it = client_rejections_.find(client_id);
        const std::uint64_t r = (rejected_it == client_rejections_.end())
                                ? 0 : rejected_it->second;
        return static_cast<double>(r) /
               static_cast<double>(it->second.orders_submitted + r);
    }

private:
    // Trade size distribution state
    TradeSizeDistribution  size_dist_{};

    // Reference price state
    bool          reference_set_ = false;
    std::int32_t  reference_price_ticks_ = 0;

    // Per-client rejection counter
    std::unordered_map<std::uint64_t, std::uint64_t>  client_rejections_;

    // Top-of-book change detection — snapshot of last observed best_bid/best_ask.
    // Used by tob_changed_since_last_check() which is a stateful "poll for changes" API.
    std::int32_t  last_observed_bid_ticks_ = NO_BID_TICKS;
    std::int32_t  last_observed_ask_ticks_ = NO_ASK_TICKS;

    // Quote-life accounting (mean TOB residency).
    std::uint64_t last_tob_change_ts_ns_ = 0;
    std::uint64_t total_tob_life_ns_     = 0;

    // Spread-compression detection
    std::int32_t  spread_compression_threshold_ticks_ = -1;  // -1 == off
    std::uint64_t spread_compression_events_ = 0;

    // Order-flow (VPIN-style toxicity, signed by taker side)
    std::uint64_t taker_buy_volume_   = 0;
    std::uint64_t taker_sell_volume_  = 0;
    std::uint64_t taker_buy_count_    = 0;
    std::uint64_t taker_sell_count_   = 0;
    std::uint64_t last_fill_ts_ns_    = 0;

    // Quote flicker (TOB change without trade) — needs last_fill_ts vs last_tob_change_ts
    std::uint64_t quote_flicker_count_ = 0;

    // TOB stability streak
    std::uint64_t tob_unchanged_streak_ = 0;
    std::uint64_t max_tob_unchanged_streak_ = 0;

    // Time-weighted spread (∫ spread × dt)
    std::uint64_t last_spread_sample_ts_ns_ = 0;
    std::uint64_t time_weighted_spread_ticks_x_ns_ = 0;
    std::uint64_t total_spread_dt_ns_ = 0;

    // Event sequence numbers (monotonic)
    std::uint64_t next_event_seq_   = 0;
    std::uint64_t last_emitted_seq_ = 0;

    // Spread bias counters (mid-bid vs ask-mid)
    std::uint64_t spread_bias_ask_side_ = 0;
    std::uint64_t spread_bias_bid_side_ = 0;
    std::uint64_t spread_bias_neutral_  = 0;

    // Kyle's lambda accumulators
    std::int64_t  prev_trade_px_for_lambda_ = -1;
    double        cum_price_volume_product_ = 0.0;  // Σ Δp × v
    double        cum_signed_volume_sq_     = 0.0;  // Σ v²
    // Per-side decomposition
    double        cum_pv_buy_  = 0.0;
    double        cum_pv_sell_ = 0.0;
    double        cum_vsq_buy_ = 0.0;
    double        cum_vsq_sell_ = 0.0;

    // Spread regime thresholds + counters
    std::int32_t  spread_regime_tight_thresh_  = 0;
    std::int32_t  spread_regime_wide_thresh_   = 0;
    std::uint64_t spread_regime_tight_count_   = 0;
    std::uint64_t spread_regime_normal_count_  = 0;
    std::uint64_t spread_regime_wide_count_    = 0;

    // One-sided book counter (called by poll_tob_micro)
    std::uint64_t one_sided_bid_only_ = 0;
    std::uint64_t one_sided_ask_only_ = 0;

    // Tick-by-tick price change distribution (last trade-to-trade Δp)
    // 9 bins: -4..-1, 0, +1..+4 (Δp clipped). Bin 0 = "no change", index 4.
    std::uint64_t price_change_hist_[9]{};
    std::uint64_t price_change_total_ = 0;

    // Implementation shortfall (Σ (fill_px - decision_mid) × qty × side_sign)
    std::int64_t  cum_implementation_shortfall_ticks_qty_ = 0;
    std::uint64_t cum_implementation_shortfall_qty_       = 0;

    // Per-side VWAP accumulators
    std::int64_t  cum_buy_notional_ticks_  = 0;
    std::uint64_t cum_buy_volume_          = 0;
    std::int64_t  cum_sell_notional_ticks_ = 0;
    std::uint64_t cum_sell_volume_         = 0;

    // Inter-trade time gap stats
    std::uint64_t prev_trade_ts_ns_for_gap_ = 0;
    std::uint64_t inter_trade_gap_min_ns_   = UINT64_MAX;
    std::uint64_t inter_trade_gap_max_ns_   = 0;
    std::uint64_t inter_trade_gap_sum_ns_   = 0;
    std::uint64_t inter_trade_gap_count_    = 0;

    // Largest single trade observed
    std::int32_t  largest_single_trade_qty_ = 0;

    // First-fill latency tracker (per order, recorded once on first fill)
    std::uint64_t first_fill_latency_count_   = 0;
    std::uint64_t first_fill_latency_sum_ns_  = 0;
    std::uint64_t first_fill_latency_max_ns_  = 0;
    std::uint64_t first_fill_latency_min_ns_  = UINT64_MAX;

    // Submission burst detector
    std::uint64_t burst_window_ns_   = 0;     // 0 = off
    std::uint64_t burst_last_submit_ns_ = 0;
    std::uint64_t burst_runs_count_  = 0;     // # bursts (≥2 submits within window)
    std::uint64_t burst_in_run_count_ = 0;    // counter dla bieżącego runa

    // Order completion histogram
    std::uint64_t completion_filled_fully_     = 0;  // total_qty == filled_qty
    std::uint64_t completion_cancelled_partial_= 0;  // 0 < filled_qty < total_qty
    std::uint64_t completion_cancelled_unfilled_= 0; // filled_qty == 0
    std::uint64_t completion_expired_partial_  = 0;
    std::uint64_t completion_expired_unfilled_ = 0;
    // Per-side maker fills (by m->side when fully filled in match_at_level)
    std::uint64_t maker_fills_buy_side_  = 0;
    std::uint64_t maker_fills_sell_side_ = 0;

    // Best-bid qty histogram (16 bins, log2-style: 0, 1-2, 3-4, 5-8, ...)
    static constexpr std::size_t QTY_HIST_BINS = 16;
    std::uint64_t best_bid_qty_hist_[QTY_HIST_BINS]{};
    std::uint64_t best_ask_qty_hist_[QTY_HIST_BINS]{};

    // EMA imbalance (alpha = 0.1 default; configurable)
    double  ema_imbalance_alpha_ = 0.1;
    double  ema_imbalance_bps_value_ = 0.0;
    bool    ema_imbalance_init_ = false;

    // Microprice ring buffer (last 16 samples)
    std::int32_t  microprice_ring_[MID_RING_CAP]{};
    std::size_t   microprice_ring_head_  = 0;
    std::size_t   microprice_ring_count_ = 0;

    // Signed-volume EMA (alpha 0.1)
    double  ema_signed_volume_ = 0.0;
    bool    ema_signed_volume_init_ = false;

    // Time-weighted mid (Σ mid × dt / Σ dt)
    std::uint64_t last_twmid_sample_ts_ns_ = 0;
    std::int32_t  last_twmid_sample_ticks_ = 0;
    std::int64_t  twmid_sum_ticks_x_ns_    = 0;
    std::uint64_t twmid_total_dt_ns_       = 0;

    void record_first_fill_latency(std::uint64_t submit_ts, std::uint64_t fill_ts) noexcept {
        if (submit_ts == 0 || fill_ts < submit_ts) return;
        const std::uint64_t lat = fill_ts - submit_ts;
        ++first_fill_latency_count_;
        first_fill_latency_sum_ns_ += lat;
        if (lat > first_fill_latency_max_ns_) first_fill_latency_max_ns_ = lat;
        if (lat < first_fill_latency_min_ns_) first_fill_latency_min_ns_ = lat;
    }

    // Spread histogram (32 buckets: 0..30 ticks + 31=overflow)
    static constexpr std::size_t SPREAD_HIST_BINS = 32;
    std::uint64_t spread_histogram_[SPREAD_HIST_BINS]{};
    std::uint64_t spread_hist_total_ = 0;

    // Latency arbitrage window detection
    std::uint64_t larb_window_ns_      = 0;            // 0 = off
    std::uint64_t larb_last_fill_ts_   = 0;
    Side          larb_last_side_      = Side::BUY;
    std::uint64_t larb_same_side_fast_ = 0;

    // Per-side last fill timestamps
    std::uint64_t last_buy_fill_ts_ns_  = 0;
    std::uint64_t last_sell_fill_ts_ns_ = 0;

    // Iceberg refresh counter
    std::uint64_t iceberg_refresh_count_ = 0;

    // Per-reason rejection counters (RejectReason::* indexed; 13 values)
    std::uint64_t rejections_by_reason_[13]{};
    // Per-TIF acceptance counters (TimeInForce::* indexed; 5 values)
    std::uint64_t accepts_by_tif_[5]{};
    // Per-OrderType acceptance counters (10 values)
    std::uint64_t accepts_by_type_[10]{};

    // Mid-price ring buffer (last 16 samples)
    static constexpr std::size_t MID_RING_CAP = 16;
    std::int32_t  mid_ring_[MID_RING_CAP]{};
    std::size_t   mid_ring_head_  = 0;
    std::size_t   mid_ring_count_ = 0;

    // Fill-vs-mid bands counter — narrow / wide threshold
    std::int32_t  fill_band_threshold_ticks_ = 0;     // 0 = off
    std::uint64_t fills_within_band_ = 0;
    std::uint64_t fills_outside_band_ = 0;

    // Queue replenishment / consumption counters (TOB qty deltas)
    std::int32_t  last_tob_bid_qty_observed_ = -1;
    std::int32_t  last_tob_ask_qty_observed_ = -1;
    std::uint64_t queue_replenish_bid_ = 0;
    std::uint64_t queue_replenish_ask_ = 0;
    std::uint64_t queue_consume_bid_   = 0;
    std::uint64_t queue_consume_ask_   = 0;

    // Volume-at-price profile (zero-init via {})
    std::uint64_t volume_at_price_[LEVELS]{};

public:
    // ====================================================================
    // Top-of-book change tracking
    // ====================================================================
    //
    // poll_tob_change(): zwraca true gdy best_bid lub best_ask zmieniło się
    // od ostatniego wywołania. Resetuje state. Strategie używają jako trigger
    // do re-quote / decision points.
    //
    // total_tob_changes() — kumulatywny licznik zmian TOB (każdy submit/
    // cancel/fill który zmienił best_bid lub best_ask).
    bool poll_tob_change() noexcept {
        const bool changed = (best_bid_ticks_ != last_observed_bid_ticks_)
                          || (best_ask_ticks_ != last_observed_ask_ticks_);
        if (!changed) {
            ++tob_unchanged_streak_;
            if (tob_unchanged_streak_ > max_tob_unchanged_streak_)
                max_tob_unchanged_streak_ = tob_unchanged_streak_;
        } else {
            tob_unchanged_streak_ = 0;
        }
        if (changed) {
            const std::uint64_t now = mono_ns_now();
            // Quote-flicker: zmiana TOB pomiędzy ostatnim fillem a teraz?
            // Jeśli od poprzedniej zmiany TOB nie było ŻADNEGO trade, to flicker.
            if (last_tob_change_ts_ns_ != 0 &&
                last_fill_ts_ns_ < last_tob_change_ts_ns_) {
                ++quote_flicker_count_;
            }
            if (last_tob_change_ts_ns_ != 0 && now > last_tob_change_ts_ns_) {
                total_tob_life_ns_ += (now - last_tob_change_ts_ns_);
            }
            last_tob_change_ts_ns_ = now;
            last_observed_bid_ticks_ = best_bid_ticks_;
            last_observed_ask_ticks_ = best_ask_ticks_;
            ++stats_.total_tob_changes;
            // Spread compression check
            if (spread_compression_threshold_ticks_ > 0 &&
                has_bid() && has_ask() &&
                (best_ask_ticks_ - best_bid_ticks_) <
                spread_compression_threshold_ticks_) {
                ++spread_compression_events_;
            }
        }
        return changed;
    }

    // Quote-life — agregowany czas TOB residency w ns / liczba zmian.
    // Aproksymacja: liczone od poll do poll. Strategia powinna pollować
    // regularnie żeby wynik był adekwatny.
    std::uint64_t mean_tob_life_ns() const noexcept {
        if (stats_.total_tob_changes < 2) return 0;
        return total_tob_life_ns_ / (stats_.total_tob_changes - 1);
    }
    std::uint64_t total_tob_life_ns() const noexcept { return total_tob_life_ns_; }

    // Spread-compression detector — ustawia threshold (ticks) poniżej
    // którego każda zmiana TOB jest zliczana jako "compression event".
    void set_spread_compression_threshold(std::int32_t threshold_ticks) noexcept {
        spread_compression_threshold_ticks_ = threshold_ticks;
    }
    std::uint64_t spread_compression_count() const noexcept {
        return spread_compression_events_;
    }

    // ====================================================================
    // Order flow imbalance / VPIN-style toxicity
    // ====================================================================
    //
    // VPIN (Volume-Synchronized Probability of Informed Trading; Easley/
    // de Prado/O'Hara 2012). Aproksymacja: cumulative |buy - sell| / total.
    // Wysoka wartość = informacyjny flow (jeden side dominuje).
    std::uint64_t taker_buy_volume() const noexcept  { return taker_buy_volume_; }
    std::uint64_t taker_sell_volume() const noexcept { return taker_sell_volume_; }
    std::uint64_t taker_buy_count() const noexcept   { return taker_buy_count_; }
    std::uint64_t taker_sell_count() const noexcept  { return taker_sell_count_; }
    std::int32_t flow_imbalance_bps() const noexcept {
        const std::int64_t b = static_cast<std::int64_t>(taker_buy_volume_);
        const std::int64_t s = static_cast<std::int64_t>(taker_sell_volume_);
        const std::int64_t total = b + s;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((b - s) * 10000 / total);
    }
    // VPIN ~ |buy - sell| / (buy + sell). 0..1 (returnujemy bps).
    std::uint32_t vpin_bps() const noexcept {
        const std::int64_t b = static_cast<std::int64_t>(taker_buy_volume_);
        const std::int64_t s = static_cast<std::int64_t>(taker_sell_volume_);
        const std::int64_t total = b + s;
        if (total == 0) return 0;
        return static_cast<std::uint32_t>(std::abs(b - s) * 10000 / total);
    }

    // Quote flicker — TOB zmienił się bez intervening trade.
    // Wysokie = quote stuffing lub pure quoting noise.
    std::uint64_t quote_flicker_count() const noexcept { return quote_flicker_count_; }

    // Volume-at-price profile — kumulatywne exec qty per tick.
    // Strategie używają do detection support/resistance.
    std::uint64_t volume_at_price(std::int32_t price_ticks) const noexcept {
        if (price_ticks < 0 || price_ticks >= LEVELS) return 0;
        return volume_at_price_[static_cast<std::size_t>(price_ticks)];
    }
    // Point-of-control: tick z największą historyczną volume.
    std::int32_t point_of_control_ticks() const noexcept {
        std::int32_t best_tick = -1;
        std::uint64_t best_vol  = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            if (volume_at_price_[p] > best_vol) {
                best_vol = volume_at_price_[p];
                best_tick = p;
            }
        }
        return best_tick;
    }

    // ====================================================================
    // TOB stability streak
    // ====================================================================
    //
    // current_tob_unchanged_streak() — # konsekutywnych pollów, w których
    // TOB nie zmienił się. max_tob_unchanged_streak_observed() — historyczne max.
    // Wysoka wartość = stable book; niska = noisy/active flow.
    std::uint64_t current_tob_unchanged_streak() const noexcept {
        return tob_unchanged_streak_;
    }
    std::uint64_t max_tob_unchanged_streak_observed() const noexcept {
        return max_tob_unchanged_streak_;
    }

    // ====================================================================
    // Time-weighted spread (TWAS)
    // ====================================================================
    //
    // Strategia okresowo wywołuje sample_time_weighted_spread(); akumulujemy
    // (spread × dt) gdzie dt = czas od poprzedniego sampla. Mean TWAS =
    // Σ spread×dt / Σ dt. Bardziej miarodajne niż arithmetic mean spread.
    void sample_time_weighted_spread() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::uint64_t now = mono_ns_now();
        if (last_spread_sample_ts_ns_ == 0) {
            last_spread_sample_ts_ns_ = now;
            return;
        }
        if (now > last_spread_sample_ts_ns_) {
            const std::uint64_t dt = now - last_spread_sample_ts_ns_;
            const std::uint64_t spread =
                static_cast<std::uint64_t>(best_ask_ticks_ - best_bid_ticks_);
            time_weighted_spread_ticks_x_ns_ += spread * dt;
            total_spread_dt_ns_ += dt;
            last_spread_sample_ts_ns_ = now;
        }
    }
    double mean_time_weighted_spread_ticks() const noexcept {
        if (total_spread_dt_ns_ == 0) return 0.0;
        return static_cast<double>(time_weighted_spread_ticks_x_ns_) /
               static_cast<double>(total_spread_dt_ns_);
    }

    // ====================================================================
    // Trade tape statistics
    // ====================================================================
    //
    // Skanuje całe ring buffer (do TAPE_CAP wpisów) i wylicza statystyki
    // ostatnich N trades. min/max/mean qty + cena std-dev jako volatility
    // estimator.
    struct TapeStats {
        std::int32_t  n_samples;
        std::int32_t  min_qty;
        std::int32_t  max_qty;
        double        mean_qty;
        std::int32_t  min_price_ticks;
        std::int32_t  max_price_ticks;
        double        mean_price_ticks;
        double        price_stddev_ticks;
    };
    TapeStats tape_statistics() const noexcept {
        TapeStats s{0, 0, 0, 0.0, 0, 0, 0.0, 0.0};
        const std::size_t n = std::min(tape_count_, TAPE_CAP);
        if (n == 0) return s;
        s.n_samples = static_cast<std::int32_t>(n);
        const Trade& first = tape_[(tape_head_ + TAPE_CAP - n) % TAPE_CAP];
        s.min_qty = s.max_qty = first.qty;
        s.min_price_ticks = s.max_price_ticks = first.price_ticks;
        std::int64_t sum_qty = 0, sum_px = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - n + k) % TAPE_CAP];
            sum_qty += t.qty;
            sum_px  += t.price_ticks;
            if (t.qty < s.min_qty)        s.min_qty = t.qty;
            if (t.qty > s.max_qty)        s.max_qty = t.qty;
            if (t.price_ticks < s.min_price_ticks) s.min_price_ticks = t.price_ticks;
            if (t.price_ticks > s.max_price_ticks) s.max_price_ticks = t.price_ticks;
        }
        s.mean_qty         = static_cast<double>(sum_qty) / static_cast<double>(n);
        s.mean_price_ticks = static_cast<double>(sum_px)  / static_cast<double>(n);
        // 2nd pass dla std-dev (numerically stable byłby Welford; tu prosty).
        double var = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - n + k) % TAPE_CAP];
            const double d = static_cast<double>(t.price_ticks) - s.mean_price_ticks;
            var += d * d;
        }
        var /= static_cast<double>(n);
        // sqrt bez <cmath> – uproszczenie via std::sqrt
        s.price_stddev_ticks = std::sqrt(var);
        return s;
    }

    // ====================================================================
    // Trade arrival rate
    // ====================================================================
    //
    // trades_per_second(): liczone z first/last tape ts. 0 gdy <2 trades.
    // Wysoka wartość = active flow; spike = breakout/news.
    double trades_per_second() const noexcept {
        const std::size_t n = std::min(tape_count_, TAPE_CAP);
        if (n < 2) return 0.0;
        const std::size_t first_idx = (tape_head_ + TAPE_CAP - n) % TAPE_CAP;
        const std::size_t last_idx  = (tape_head_ + TAPE_CAP - 1) % TAPE_CAP;
        const std::uint64_t t0 = tape_[first_idx].ts_ns;
        const std::uint64_t t1 = tape_[last_idx].ts_ns;
        if (t1 <= t0) return 0.0;
        const double dt_sec = static_cast<double>(t1 - t0) / 1e9;
        if (dt_sec <= 0.0) return 0.0;
        return static_cast<double>(n - 1) / dt_sec;
    }

    // ====================================================================
    // Realized volatility (sum of squared log returns)
    // ====================================================================
    //
    // realized_volatility_log_returns(): Σ (log(p_i/p_{i-1}))² across tape.
    // Aproksymacja microstructure σ²; przemnóż przez (samples_per_year/N)
    // by uzyskać annualized vol.
    double realized_volatility_log_returns() const noexcept {
        const std::size_t n = std::min(tape_count_, TAPE_CAP);
        if (n < 2) return 0.0;
        double sum_sq = 0.0;
        std::int32_t prev_px = -1;
        for (std::size_t k = 0; k < n; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - n + k) % TAPE_CAP];
            if (prev_px > 0 && t.price_ticks > 0) {
                const double r = std::log(static_cast<double>(t.price_ticks) /
                                          static_cast<double>(prev_px));
                sum_sq += r * r;
            }
            prev_px = t.price_ticks;
        }
        return std::sqrt(sum_sq);
    }

    // ====================================================================
    // Spread bias + queue replenishment (TOB micro-dynamics)
    // ====================================================================
    //
    // Każdy poll_tob_micro() jest punktem obserwacyjnym dla:
    //  • spread_bias: porównanie mid-bid vs ask-mid; gdy aksy są blisko mid,
    //    bid trzyma; gdy bidy blisko mid, ask trzyma. Wskazówka kto presser.
    //  • queue_replenish: gdy TOB qty wzrosło względem poprzedniego polla =
    //    market makery dorzucają liquidity. Spadek = consumption.
    void poll_tob_micro() noexcept {
        if (has_bid() && !has_ask()) { ++one_sided_bid_only_; return; }
        if (!has_bid() && has_ask()) { ++one_sided_ask_only_; return; }
        if (!has_bid() || !has_ask()) return;
        const std::int32_t mid = (best_bid_ticks_ + best_ask_ticks_) / 2;
        const std::int32_t ask_offset = best_ask_ticks_ - mid;
        const std::int32_t bid_offset = mid - best_bid_ticks_;
        if (ask_offset < bid_offset)       ++spread_bias_ask_side_;
        else if (bid_offset < ask_offset)  ++spread_bias_bid_side_;
        else                                ++spread_bias_neutral_;
        // Queue replenishment
        const std::int32_t bid_qty = levels_[best_bid_ticks_].total_qty;
        const std::int32_t ask_qty = levels_[best_ask_ticks_].total_qty;
        if (last_tob_bid_qty_observed_ >= 0) {
            if (bid_qty > last_tob_bid_qty_observed_) ++queue_replenish_bid_;
            else if (bid_qty < last_tob_bid_qty_observed_) ++queue_consume_bid_;
        }
        if (last_tob_ask_qty_observed_ >= 0) {
            if (ask_qty > last_tob_ask_qty_observed_) ++queue_replenish_ask_;
            else if (ask_qty < last_tob_ask_qty_observed_) ++queue_consume_ask_;
        }
        last_tob_bid_qty_observed_ = bid_qty;
        last_tob_ask_qty_observed_ = ask_qty;
    }
    std::uint64_t spread_bias_ask_side()  const noexcept { return spread_bias_ask_side_; }
    std::uint64_t spread_bias_bid_side()  const noexcept { return spread_bias_bid_side_; }
    std::uint64_t spread_bias_neutral()   const noexcept { return spread_bias_neutral_; }
    std::uint64_t queue_replenish_bid_count() const noexcept { return queue_replenish_bid_; }
    std::uint64_t queue_replenish_ask_count() const noexcept { return queue_replenish_ask_; }
    std::uint64_t queue_consume_bid_count()  const noexcept { return queue_consume_bid_; }
    std::uint64_t queue_consume_ask_count()  const noexcept { return queue_consume_ask_; }

    // ====================================================================
    // Kyle's lambda (price impact slope)
    // ====================================================================
    //
    // λ = Σ Δp × v / Σ v² (slope of regression: price change ~ signed volume).
    // Wysokie λ = każda jednostka volume mocno rusza ceną (illiquid; toxic).
    double kyle_lambda() const noexcept {
        if (cum_signed_volume_sq_ <= 0.0) return 0.0;
        return cum_price_volume_product_ / cum_signed_volume_sq_;
    }
    double kyle_lambda_abs() const noexcept {
        return std::abs(kyle_lambda());
    }

    // ====================================================================
    // Latency arbitrage window detector
    // ====================================================================
    //
    // set_latency_arb_window_ns(ε): uzbrojenie alertu na same-side aggressors
    // w odstępie ≤ ε ns. Wysokie counts = exchange-co-located HFT chasing
    // venue updates faster niż konkurencja.
    void set_latency_arb_window_ns(std::uint64_t window_ns) noexcept {
        larb_window_ns_ = window_ns;
    }
    std::uint64_t latency_arb_same_side_fast_count() const noexcept {
        return larb_same_side_fast_;
    }

    // ====================================================================
    // Per-side last fill timestamps
    // ====================================================================
    //
    // Adverse selection: jeśli last_buy_fill znacznie świeższy niż last_sell,
    // to BUY taker dominują — bid pressing. Liquidity providers użyją tych
    // metryk do skew quote'ów.
    std::uint64_t last_buy_fill_ts_ns()  const noexcept { return last_buy_fill_ts_ns_; }
    std::uint64_t last_sell_fill_ts_ns() const noexcept { return last_sell_fill_ts_ns_; }

    // ====================================================================
    // Iceberg refresh counter
    // ====================================================================
    //
    // Iceberg orderzy z dużym ukrytym reserve robią wiele refreshy. Wysokie
    // = oznaka long-term institutional accumulation. Per-order tracking byłoby
    // optymalniejsze, ale globalny licznik wystarcza dla orientacji.
    std::uint64_t iceberg_refresh_count() const noexcept {
        return iceberg_refresh_count_;
    }

    // ====================================================================
    // Largest resting order — wall detector
    // ====================================================================
    //
    // O(LEVELS × orders_per_level). Wykorzystywane okresowo, nie hot.
    // Wall = dużo qty w jednym orderze — może oznaczać iceberg, hidden
    // intent, lub bluff/manipulację (spoofing).
    // ====================================================================
    // Depth concentration index
    // ====================================================================
    //
    // depth_concentration_bps(side, top_n): qty w top_n najlepszych levels
    // jako % całkowitej qty po tej stronie (bps). Wysokie = liquidity blisko
    // best price (tight book); niskie = depth rozłożona (thicker tail).
    std::int32_t depth_concentration_bps(Side side, std::int32_t top_n) const noexcept {
        std::int64_t top = 0, total = 0;
        std::int32_t cnt = 0;
        if (side == Side::BUY) {
            if (!has_bid()) return 0;
            for (std::int32_t p = best_bid_ticks_; p >= 0; --p) {
                const std::int32_t q = levels_[p].total_qty;
                if (q > 0) {
                    total += q;
                    if (cnt < top_n) { top += q; ++cnt; }
                }
            }
        } else {
            if (!has_ask()) return 0;
            for (std::int32_t p = best_ask_ticks_; p < LEVELS; ++p) {
                const std::int32_t q = levels_[p].total_qty;
                if (q > 0) {
                    total += q;
                    if (cnt < top_n) { top += q; ++cnt; }
                }
            }
        }
        if (total == 0) return 0;
        return static_cast<std::int32_t>(top * 10000 / total);
    }

    // ====================================================================
    // Active book averages
    // ====================================================================
    //
    // avg_resting_qty_per_order(): średnia qty per resting order (Σ qty/order_count).
    // Wysoka = institutional/iceberg-heavy; niska = retail/algo-heavy.
    double avg_resting_qty_per_order() const noexcept {
        std::int64_t total_qty = 0;
        std::int64_t total_orders = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            total_qty    += levels_[p].total_qty;
            total_orders += levels_[p].order_count;
        }
        if (total_orders == 0) return 0.0;
        return static_cast<double>(total_qty) / static_cast<double>(total_orders);
    }
    // active_price_levels(): ile poziomów cenowych ma orders.
    std::int32_t active_price_levels() const noexcept {
        std::int32_t n = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            if (levels_[p].order_count > 0) ++n;
        }
        return n;
    }

    // ====================================================================
    // Mid-price ring buffer (last 16) + momentum signal
    // ====================================================================
    //
    // sample_mid_to_ring() — strategia wywołuje okresowo. Mid-momentum =
    // (latest - oldest) per sample interval; signal trendu w short horizon.
    void sample_mid_to_ring() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t m = (best_bid_ticks_ + best_ask_ticks_) / 2;
        mid_ring_[mid_ring_head_ % MID_RING_CAP] = m;
        ++mid_ring_head_;
        if (mid_ring_count_ < MID_RING_CAP) ++mid_ring_count_;
    }
    std::size_t mid_ring_samples() const noexcept { return mid_ring_count_; }
    std::int32_t mid_momentum_ticks() const noexcept {
        if (mid_ring_count_ < 2) return 0;
        const std::size_t n = mid_ring_count_;
        const std::size_t oldest_idx = (mid_ring_head_ + MID_RING_CAP - n) % MID_RING_CAP;
        const std::size_t newest_idx = (mid_ring_head_ + MID_RING_CAP - 1) % MID_RING_CAP;
        return mid_ring_[newest_idx] - mid_ring_[oldest_idx];
    }

    // ====================================================================
    // Fill-bands compliance metric
    // ====================================================================
    //
    // set_fill_band_threshold_ticks(T) — uzbrojenie. Fills z |fill_px - mid| ≤ T
    // → fills_within_band; else outside. Ratio = within / (within + outside).
    void set_fill_band_threshold_ticks(std::int32_t t) noexcept {
        fill_band_threshold_ticks_ = t;
    }
    std::uint64_t fills_within_band() const noexcept  { return fills_within_band_; }
    std::uint64_t fills_outside_band() const noexcept { return fills_outside_band_; }
    double fill_band_compliance_ratio() const noexcept {
        const std::uint64_t total = fills_within_band_ + fills_outside_band_;
        if (total == 0) return 0.0;
        return static_cast<double>(fills_within_band_) /
               static_cast<double>(total);
    }

    // ====================================================================
    // Per-side Kyle's lambda
    // ====================================================================
    //
    // Buy-side vs sell-side decomposition. Sygnalizuje asymmetric impact:
    // buy_lambda > sell_lambda → buyers płacą drożej (asymmetric supply curve).
    double kyle_lambda_buy() const noexcept {
        if (cum_vsq_buy_ <= 0.0) return 0.0;
        return cum_pv_buy_ / cum_vsq_buy_;
    }
    double kyle_lambda_sell() const noexcept {
        if (cum_vsq_sell_ <= 0.0) return 0.0;
        return cum_pv_sell_ / cum_vsq_sell_;
    }
    double kyle_lambda_buy_abs()  const noexcept { return std::abs(kyle_lambda_buy()); }
    double kyle_lambda_sell_abs() const noexcept { return std::abs(kyle_lambda_sell()); }

    // ====================================================================
    // Spread regime classifier
    // ====================================================================
    //
    // Thresholds dzielą czas na 3 bucketsy. Use case: classify market into
    // calm / normal / stressed regimes (volatility & impact regimes differ).
    void set_spread_regime_thresholds(std::int32_t tight, std::int32_t wide) noexcept {
        spread_regime_tight_thresh_ = tight;
        spread_regime_wide_thresh_  = wide;
    }
    void sample_spread_regime() noexcept {
        if (!has_bid() || !has_ask()) return;
        if (spread_regime_tight_thresh_ <= 0 || spread_regime_wide_thresh_ <= 0)
            return;
        const std::int32_t s = best_ask_ticks_ - best_bid_ticks_;
        if (s <= spread_regime_tight_thresh_)        ++spread_regime_tight_count_;
        else if (s >= spread_regime_wide_thresh_)    ++spread_regime_wide_count_;
        else                                          ++spread_regime_normal_count_;
    }
    std::uint64_t spread_regime_tight_count()  const noexcept { return spread_regime_tight_count_; }
    std::uint64_t spread_regime_normal_count() const noexcept { return spread_regime_normal_count_; }
    std::uint64_t spread_regime_wide_count()   const noexcept { return spread_regime_wide_count_; }

    // ====================================================================
    // TOB skewness — bid_qty vs ask_qty asymmetry
    // ====================================================================
    //
    // Zwraca (best_bid_qty - best_ask_qty) / (sum) × 10000 bps.
    // Pozytywny = bid_qty dominuje (passive demand).
    std::int32_t tob_skewness_bps() const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        const std::int64_t b = levels_[best_bid_ticks_].total_qty;
        const std::int64_t a = levels_[best_ask_ticks_].total_qty;
        const std::int64_t total = b + a;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((b - a) * 10000 / total);
    }

    // ====================================================================
    // Mid-VWAP divergence
    // ====================================================================
    //
    // mid_minus_tape_vwap_ticks() — current mid względem cumulative VWAP z tape.
    // >0 = price drifted up from average; <0 = drifted down.
    std::int32_t mid_minus_tape_vwap_ticks() const noexcept {
        const std::int32_t vwap = tape_vwap_ticks();
        const std::int32_t mid = mid_ticks();
        if (vwap <= 0 || mid <= 0) return 0;
        return mid - vwap;
    }

    // ====================================================================
    // Mid-trend classifier
    // ====================================================================
    //
    // 3-state: UP / DOWN / SIDEWAYS based on mid_ring delta.
    enum class MidTrend : std::uint8_t { UNKNOWN = 0, UP = 1, DOWN = 2, SIDEWAYS = 3 };
    MidTrend classify_mid_trend(std::int32_t sideways_band_ticks = 1) const noexcept {
        if (mid_ring_count_ < 2) return MidTrend::UNKNOWN;
        const std::int32_t d = mid_momentum_ticks();
        if (d >  sideways_band_ticks) return MidTrend::UP;
        if (d < -sideways_band_ticks) return MidTrend::DOWN;
        return MidTrend::SIDEWAYS;
    }

    // ====================================================================
    // One-sided book counters (called by poll_tob_micro)
    // ====================================================================
    std::uint64_t one_sided_bid_only_count() const noexcept { return one_sided_bid_only_; }
    std::uint64_t one_sided_ask_only_count() const noexcept { return one_sided_ask_only_; }

    // ====================================================================
    // Spread histogram
    // ====================================================================
    //
    // sample_spread_to_histogram() — bucketuje current spread do 32-bin hist.
    // Bin index = min(31, spread). Bin 31 jest catch-all dla >= 31 ticków.
    void sample_spread_to_histogram() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t s = best_ask_ticks_ - best_bid_ticks_;
        const std::size_t bin = std::min(static_cast<std::size_t>(s),
                                         SPREAD_HIST_BINS - 1);
        ++spread_histogram_[bin];
        ++spread_hist_total_;
    }
    std::uint64_t spread_histogram_bin(std::size_t bin) const noexcept {
        return bin < SPREAD_HIST_BINS ? spread_histogram_[bin] : 0;
    }
    std::uint64_t spread_histogram_total() const noexcept { return spread_hist_total_; }
    // Median spread w bps — pierwszy bin z cumulative >= 50%.
    std::int32_t spread_histogram_median_ticks() const noexcept {
        if (spread_hist_total_ == 0) return -1;
        const std::uint64_t target = spread_hist_total_ / 2;
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < SPREAD_HIST_BINS; ++i) {
            cum += spread_histogram_[i];
            if (cum > target) return static_cast<std::int32_t>(i);
        }
        return static_cast<std::int32_t>(SPREAD_HIST_BINS - 1);
    }

    // ====================================================================
    // Top-K largest resting orders
    // ====================================================================
    //
    // Wypełnia out_qty[] do max K największych remaining qty. Zwraca ile
    // znaleziono. Prostą O(N×K) selection — używać tylko okresowo na małym K.
    // ====================================================================
    // Tick-by-tick price change distribution (9 bins, -4..+4 clipped)
    // ====================================================================
    //
    // Bin index 0=Δp≤-4, 1=-3, 2=-2, 3=-1, 4=0, 5=+1, 6=+2, 7=+3, 8=Δp≥+4.
    // Heavy tails (bins 0 i 8) = high impact regime; bin 4 = quiet market.
    std::uint64_t price_change_hist_bin(std::size_t i) const noexcept {
        return i < 9 ? price_change_hist_[i] : 0;
    }
    std::uint64_t price_change_hist_total() const noexcept { return price_change_total_; }
    double price_change_hist_zero_fraction() const noexcept {
        if (price_change_total_ == 0) return 0.0;
        return static_cast<double>(price_change_hist_[4]) /
               static_cast<double>(price_change_total_);
    }
    // Tail mass: fraction trades z |Δp| >= 4.
    double price_change_hist_tail_fraction() const noexcept {
        if (price_change_total_ == 0) return 0.0;
        const std::uint64_t tail = price_change_hist_[0] + price_change_hist_[8];
        return static_cast<double>(tail) /
               static_cast<double>(price_change_total_);
    }

    // ====================================================================
    // HFT toxicity composite score
    // ====================================================================
    //
    // Skalowane 0..10000 bps. Blend trzech sygnałów:
    //   • VPIN: |buy-sell|/total volume
    //   • |Kyle λ| znormalizowany przez mean_qty (proxy)
    //   • CTR (cancel-to-trade) clamped do 100
    // Każda komponenta równo ważona (33% × 33% × 33%).
    // ====================================================================
    // Per-side trade VWAP (z całego tape; buy-taker vs sell-taker)
    // ====================================================================
    //
    // buy_vwap_ticks() / sell_vwap_ticks() — VWAP osobno dla każdego kierunku
    // taker order flow. Asymmetric: buy_vwap > mid → bullish; sell_vwap < mid → bearish.
    std::int32_t buy_vwap_ticks() const noexcept {
        if (cum_buy_volume_ == 0) return 0;
        return static_cast<std::int32_t>(
            cum_buy_notional_ticks_ / static_cast<std::int64_t>(cum_buy_volume_));
    }
    std::int32_t sell_vwap_ticks() const noexcept {
        if (cum_sell_volume_ == 0) return 0;
        return static_cast<std::int32_t>(
            cum_sell_notional_ticks_ / static_cast<std::int64_t>(cum_sell_volume_));
    }
    // buy_vs_sell_vwap_spread_ticks: dodatni gdy buy_taker_vwap > sell_taker_vwap
    std::int32_t buy_vs_sell_vwap_spread_ticks() const noexcept {
        const auto b = buy_vwap_ticks();
        const auto s = sell_vwap_ticks();
        if (b == 0 || s == 0) return 0;
        return b - s;
    }

    // ====================================================================
    // Inter-trade time gap statistics
    // ====================================================================
    //
    // Mean / min / max gap ns między kolejnymi trades. Niskie = burst flow,
    // wysokie = quiet periods. Distribution shape = clustering metryka.
    std::uint64_t inter_trade_gap_min_ns() const noexcept {
        return inter_trade_gap_count_ == 0 ? 0 : inter_trade_gap_min_ns_;
    }
    std::uint64_t inter_trade_gap_max_ns() const noexcept {
        return inter_trade_gap_max_ns_;
    }
    std::uint64_t inter_trade_gap_mean_ns() const noexcept {
        if (inter_trade_gap_count_ == 0) return 0;
        return inter_trade_gap_sum_ns_ / inter_trade_gap_count_;
    }
    std::uint64_t inter_trade_gap_sample_count() const noexcept {
        return inter_trade_gap_count_;
    }

    // ====================================================================
    // Largest single trade observed (block detector)
    // ====================================================================
    std::int32_t largest_single_trade_qty() const noexcept {
        return largest_single_trade_qty_;
    }

    // ====================================================================
    // First-fill latency stats
    // ====================================================================
    //
    // Mierzone per order — od submit_ts do first_fill_ts (ns). Reflektuje:
    //  • dla maker: queue wait time przed pierwszym matchem
    //  • dla taker: matching engine latency
    std::uint64_t first_fill_latency_count() const noexcept {
        return first_fill_latency_count_;
    }
    std::uint64_t first_fill_latency_min_ns() const noexcept {
        return first_fill_latency_count_ == 0 ? 0 : first_fill_latency_min_ns_;
    }
    std::uint64_t first_fill_latency_max_ns() const noexcept {
        return first_fill_latency_max_ns_;
    }
    std::uint64_t first_fill_latency_mean_ns() const noexcept {
        if (first_fill_latency_count_ == 0) return 0;
        return first_fill_latency_sum_ns_ / first_fill_latency_count_;
    }

    // ====================================================================
    // Submission burst detector
    // ====================================================================
    //
    // set_burst_window_ns(ε): incrementuj counter "burst run" gdy następuje
    // submit w odstępie ≤ ε od poprzedniego. Burst signal = volatility regime
    // change lub coordinated order entry.
    void set_burst_window_ns(std::uint64_t window_ns) noexcept {
        burst_window_ns_ = window_ns;
    }
    std::uint64_t burst_runs_count() const noexcept { return burst_runs_count_; }
    std::uint64_t burst_current_run_count() const noexcept { return burst_in_run_count_; }

    // ====================================================================
    // Order completion histogram
    // ====================================================================
    //
    // Cztery kategorie completion:
    //  • filled_fully — match wzięło całą qty
    //  • cancelled_partial — cancel po częściowym fillu
    //  • cancelled_unfilled — cancel bez żadnego fillu
    //  • expired_partial / expired_unfilled — analogicznie ale przez GTD/DAY
    // Ratio = filled_fully / total_orders_added — book "execution efficiency".
    std::uint64_t completion_filled_fully() const noexcept     { return completion_filled_fully_; }
    std::uint64_t completion_cancelled_partial() const noexcept{ return completion_cancelled_partial_; }
    std::uint64_t completion_cancelled_unfilled() const noexcept{ return completion_cancelled_unfilled_; }
    std::uint64_t completion_expired_partial() const noexcept  { return completion_expired_partial_; }
    std::uint64_t completion_expired_unfilled() const noexcept { return completion_expired_unfilled_; }
    double fill_rate_ratio() const noexcept {
        const std::uint64_t added = stats_.total_orders_added;
        if (added == 0) return 0.0;
        return static_cast<double>(completion_filled_fully_) /
               static_cast<double>(added);
    }

    // ====================================================================
    // Time-weighted mid (TWAP-of-mid)
    // ====================================================================
    //
    // Strategia okresowo wywołuje sample_time_weighted_mid(). Akumuluje
    // (mid × dt) między samplami. mean = sum / total_dt. Lepszy benchmark
    // niż simple mid snapshot.
    void sample_time_weighted_mid() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t mid = (best_bid_ticks_ + best_ask_ticks_) / 2;
        const std::uint64_t now = mono_ns_now();
        if (last_twmid_sample_ts_ns_ == 0) {
            last_twmid_sample_ts_ns_ = now;
            last_twmid_sample_ticks_ = mid;
            return;
        }
        if (now > last_twmid_sample_ts_ns_) {
            const std::uint64_t dt = now - last_twmid_sample_ts_ns_;
            // Trapezoidal: dla stabilnej średniej używamy poprzedniego mid × dt
            twmid_sum_ticks_x_ns_ +=
                static_cast<std::int64_t>(last_twmid_sample_ticks_) *
                static_cast<std::int64_t>(dt);
            twmid_total_dt_ns_ += dt;
            last_twmid_sample_ts_ns_ = now;
            last_twmid_sample_ticks_ = mid;
        }
    }
    double mean_time_weighted_mid_ticks() const noexcept {
        if (twmid_total_dt_ns_ == 0) return 0.0;
        return static_cast<double>(twmid_sum_ticks_x_ns_) /
               static_cast<double>(twmid_total_dt_ns_);
    }
    std::uint64_t time_weighted_mid_total_dt_ns() const noexcept {
        return twmid_total_dt_ns_;
    }

    // Mean trade qty across całego cumulative tape
    double mean_trade_qty() const noexcept {
        if (stats_.total_fills == 0) return 0.0;
        return static_cast<double>(stats_.total_volume) /
               static_cast<double>(stats_.total_fills);
    }

    // ====================================================================
    // Per-side maker fills
    // ====================================================================
    //
    // Liczba maker orderów fully filled per side. BUY maker fully filled =
    // strong sell pressure ate complete bid. SELL maker fully filled =
    // strong buy pressure. Asymmetry → directional bias indicator.
    std::uint64_t maker_fills_buy_side() const noexcept  { return maker_fills_buy_side_; }
    std::uint64_t maker_fills_sell_side() const noexcept { return maker_fills_sell_side_; }

    // ====================================================================
    // Mean fill notional (Σ price × qty / Σ fills)
    // ====================================================================
    //
    // Większy = block trading / institutional flow; mniejszy = retail/algo.
    double mean_fill_notional_ticks() const noexcept {
        if (stats_.total_fills == 0) return 0.0;
        // Recompute from tape (cumulative buy + sell notionals are tape-derived)
        const std::int64_t total_notional = cum_buy_notional_ticks_ +
                                             cum_sell_notional_ticks_;
        return static_cast<double>(total_notional) /
               static_cast<double>(stats_.total_fills);
    }

    // ====================================================================
    // Most-active levels by qty (top-K)
    // ====================================================================
    //
    // top_k_active_levels_by_qty(out_prices, out_qtys, k): zwraca top-K
    // poziomów cenowych z największą total_qty. O(LEVELS × K) — okresowe.
    // ====================================================================
    // Depth pyramid score
    // ====================================================================
    //
    // depth_pyramid_steepness_bps(side, depth_n): slope between best level qty
    // i N-tego level qty. Wysokie = stromy spadek głębokości (tightly stacked
    // best level, thin tail); niskie = równomierne rozłożenie depth.
    // (qty_at_best - qty_at_n-th_level) / qty_at_best × 10000.
    std::int32_t depth_pyramid_steepness_bps(Side side, std::int32_t depth_n) const noexcept {
        if (depth_n < 2) return 0;
        std::int32_t qty_best = 0, qty_nth = 0;
        std::int32_t cnt = 0;
        if (side == Side::BUY) {
            if (!has_bid()) return 0;
            for (std::int32_t p = best_bid_ticks_; p >= 0 && cnt < depth_n; --p) {
                const std::int32_t q = levels_[p].total_qty;
                if (q > 0) {
                    if (cnt == 0) qty_best = q;
                    qty_nth = q;
                    ++cnt;
                }
            }
        } else {
            if (!has_ask()) return 0;
            for (std::int32_t p = best_ask_ticks_; p < LEVELS && cnt < depth_n; ++p) {
                const std::int32_t q = levels_[p].total_qty;
                if (q > 0) {
                    if (cnt == 0) qty_best = q;
                    qty_nth = q;
                    ++cnt;
                }
            }
        }
        if (cnt < depth_n || qty_best == 0) return 0;
        return static_cast<std::int32_t>(
            static_cast<std::int64_t>(qty_best - qty_nth) * 10000 / qty_best);
    }

    // ====================================================================
    // Cumulative resting volume per side
    // ====================================================================
    //
    // Σ qty po wszystkich BUY / SELL levels. O(LEVELS) — okresowy odczyt.
    std::int64_t cumulative_resting_volume(Side side) const noexcept {
        std::int64_t total = 0;
        if (side == Side::BUY) {
            if (!has_bid()) return 0;
            for (std::int32_t p = 0; p <= best_bid_ticks_ && p < LEVELS; ++p) {
                total += levels_[p].total_qty;
            }
        } else {
            if (!has_ask()) return 0;
            for (std::int32_t p = best_ask_ticks_; p < LEVELS; ++p) {
                total += levels_[p].total_qty;
            }
        }
        return total;
    }

    // ====================================================================
    // Best-level qty histogram (16 log2-style bins)
    // ====================================================================
    //
    // sample_best_qty_histogram() — periodyczny hook. Bin index = log2(qty)
    // clamped to [0..15]. Strategia analizuje "typical" best-level depth.
    void sample_best_qty_histogram() noexcept {
        if (has_bid()) {
            const std::int32_t bq = levels_[best_bid_ticks_].total_qty;
            ++best_bid_qty_hist_[qty_to_log2_bin(bq)];
        }
        if (has_ask()) {
            const std::int32_t aq = levels_[best_ask_ticks_].total_qty;
            ++best_ask_qty_hist_[qty_to_log2_bin(aq)];
        }
    }
    std::uint64_t best_bid_qty_hist_bin(std::size_t i) const noexcept {
        return i < QTY_HIST_BINS ? best_bid_qty_hist_[i] : 0;
    }
    std::uint64_t best_ask_qty_hist_bin(std::size_t i) const noexcept {
        return i < QTY_HIST_BINS ? best_ask_qty_hist_[i] : 0;
    }

    // ====================================================================
    // EMA imbalance (alpha-blended TOB imbalance signal)
    // ====================================================================
    //
    // sample_ema_imbalance() — periodyczny hook. EMA filtruje noise z imbalance.
    // alpha = 0.1 default (responsywność: 10 sampli ~ ε90 decay).
    void set_ema_imbalance_alpha(double a) noexcept {
        if (a > 0.0 && a <= 1.0) ema_imbalance_alpha_ = a;
    }
    void sample_ema_imbalance() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int64_t b = levels_[best_bid_ticks_].total_qty;
        const std::int64_t a = levels_[best_ask_ticks_].total_qty;
        const std::int64_t total = b + a;
        if (total == 0) return;
        const double current_bps = static_cast<double>((b - a) * 10000) /
                                    static_cast<double>(total);
        if (!ema_imbalance_init_) {
            ema_imbalance_bps_value_ = current_bps;
            ema_imbalance_init_ = true;
        } else {
            ema_imbalance_bps_value_ = ema_imbalance_alpha_ * current_bps +
                                       (1.0 - ema_imbalance_alpha_) * ema_imbalance_bps_value_;
        }
    }
    double ema_imbalance_bps() const noexcept { return ema_imbalance_bps_value_; }

    // ====================================================================
    // Microprice ring buffer (last MID_RING_CAP samples)
    // ====================================================================
    void sample_microprice_to_ring() noexcept {
        if (!has_bid() || !has_ask()) return;
        const std::int32_t mp = microprice_ticks();
        microprice_ring_[microprice_ring_head_ % MID_RING_CAP] = mp;
        ++microprice_ring_head_;
        if (microprice_ring_count_ < MID_RING_CAP) ++microprice_ring_count_;
    }
    std::size_t microprice_ring_samples() const noexcept {
        return microprice_ring_count_;
    }
    std::int32_t microprice_momentum_ticks() const noexcept {
        if (microprice_ring_count_ < 2) return 0;
        const std::size_t n = microprice_ring_count_;
        const std::size_t oldest_idx = (microprice_ring_head_ + MID_RING_CAP - n) % MID_RING_CAP;
        const std::size_t newest_idx = (microprice_ring_head_ + MID_RING_CAP - 1) % MID_RING_CAP;
        return microprice_ring_[newest_idx] - microprice_ring_[oldest_idx];
    }

    // ====================================================================
    // Signed-volume EMA (order flow filter)
    // ====================================================================
    //
    // EMA z signed qty (+qty BUY, -qty SELL) per trade. Sygnał direction
    // bardziej responsywny niż cumulative flow imbalance.
    double ema_signed_volume() const noexcept { return ema_signed_volume_; }
    bool   ema_signed_volume_ready() const noexcept { return ema_signed_volume_init_; }

private:
    static std::size_t qty_to_log2_bin(std::int32_t q) noexcept {
        if (q <= 0) return 0;
        std::size_t b = 0;
        std::uint32_t v = static_cast<std::uint32_t>(q);
        while (v > 1 && b < QTY_HIST_BINS - 1) { v >>= 1; ++b; }
        return b;
    }
public:

    std::size_t top_k_active_levels_by_qty(std::int32_t* out_prices,
                                            std::int32_t* out_qtys,
                                            std::size_t k) const noexcept {
        if (!out_prices || !out_qtys || k == 0) return 0;
        for (std::size_t i = 0; i < k; ++i) {
            out_prices[i] = -1;
            out_qtys[i] = 0;
        }
        std::size_t filled = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            const std::int32_t q = levels_[p].total_qty;
            if (q <= 0) continue;
            std::size_t pos = std::min(filled, k - 1);
            if (q > out_qtys[pos] || filled < k) {
                if (filled < k) ++filled;
                std::size_t i_ins = std::min(filled - 1, k - 1);
                while (i_ins > 0 && out_qtys[i_ins - 1] < q) {
                    out_qtys[i_ins]   = out_qtys[i_ins - 1];
                    out_prices[i_ins] = out_prices[i_ins - 1];
                    --i_ins;
                }
                out_qtys[i_ins]   = q;
                out_prices[i_ins] = p;
            }
        }
        return filled;
    }

    // ====================================================================
    // Last-N rolling VWAP z tape
    // ====================================================================
    //
    // last_n_vwap_ticks(n): VWAP ostatnich min(n, tape_count) trades. Daje
    // bardziej responsywny VWAP niż cumulative (zapomina old data).
    std::int32_t last_n_vwap_ticks(std::size_t n) const noexcept {
        const std::size_t avail = std::min(tape_count_, TAPE_CAP);
        const std::size_t use   = std::min(n, avail);
        if (use == 0) return 0;
        std::int64_t notional = 0;
        std::int64_t volume   = 0;
        for (std::size_t k = 0; k < use; ++k) {
            const Trade& t = tape_[(tape_head_ + TAPE_CAP - 1 - k) % TAPE_CAP];
            notional += static_cast<std::int64_t>(t.price_ticks) * t.qty;
            volume   += t.qty;
        }
        if (volume == 0) return 0;
        return static_cast<std::int32_t>(notional / volume);
    }

    // ====================================================================
    // Implementation Shortfall (TCA — Almgren/Chriss style)
    // ====================================================================
    //
    // IS = Σ (fill_px - decision_mid) × qty × side_sign across all fills.
    // Pozytywny = trader stracił na ruchu rynku (adverse selection).
    // mean_implementation_shortfall_ticks_per_share() — average cost per share.
    std::int64_t cumulative_implementation_shortfall_ticks_qty() const noexcept {
        return cum_implementation_shortfall_ticks_qty_;
    }
    double mean_implementation_shortfall_ticks_per_share() const noexcept {
        if (cum_implementation_shortfall_qty_ == 0) return 0.0;
        return static_cast<double>(cum_implementation_shortfall_ticks_qty_) /
               static_cast<double>(cum_implementation_shortfall_qty_);
    }

    std::uint32_t toxicity_composite_score_bps() const noexcept {
        const std::uint32_t vpin = vpin_bps();   // [0..10000]
        // |λ| normalized: rough bound by 1000 (5 ticks per 1 qty trade unit)
        const double lambda_abs = kyle_lambda_abs();
        std::uint32_t lambda_bps = static_cast<std::uint32_t>(
            std::min(lambda_abs * 1000.0, 10000.0));
        const double ctr = cancel_to_trade_ratio();
        std::uint32_t ctr_bps = static_cast<std::uint32_t>(
            std::min(ctr * 200.0, 10000.0));  // CTR=50 → 10000 bps
        return (vpin + lambda_bps + ctr_bps) / 3;
    }

    std::size_t top_k_resting_qty(std::int32_t* out_qty, std::size_t k) const noexcept {
        if (!out_qty || k == 0) return 0;
        for (std::size_t i = 0; i < k; ++i) out_qty[i] = 0;
        std::size_t filled = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            for (const Order* o = levels_[p].head; o != nullptr; o = o->next_at_level) {
                const std::int32_t left = o->total_qty - o->filled_qty;
                if (left <= 0) continue;
                // Wstaw do sortowanej tablicy (insertion-sort top-K)
                std::size_t pos = std::min(filled, k - 1);
                if (left > out_qty[pos] || filled < k) {
                    if (filled < k) ++filled;
                    // znajdź miejsce
                    std::size_t i_ins = std::min(filled - 1, k - 1);
                    while (i_ins > 0 && out_qty[i_ins - 1] < left) {
                        out_qty[i_ins] = out_qty[i_ins - 1];
                        --i_ins;
                    }
                    out_qty[i_ins] = left;
                }
            }
        }
        return filled;
    }

    std::int32_t largest_resting_order_qty() const noexcept {
        std::int32_t best = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            for (const Order* o = levels_[p].head; o != nullptr; o = o->next_at_level) {
                const std::int32_t left = o->total_qty - o->filled_qty;
                if (left > best) best = left;
            }
        }
        return best;
    }

    // ====================================================================
    // Per-reason / per-TIF / per-OrderType breakdowns
    // ====================================================================
    //
    // Compliance dashboard — gdzie się odbijają zlecenia (kategoria błędów)
    // i jakie typy/TIF preferują traderzy. Acceptance ratio = added / submitted.
    std::uint64_t rejections_by_reason(RejectReason r) const noexcept {
        const auto i = static_cast<std::size_t>(r);
        return i < 13 ? rejections_by_reason_[i] : 0;
    }
    std::uint64_t accepts_by_tif(TimeInForce tif) const noexcept {
        const auto i = static_cast<std::size_t>(tif);
        return i < 5 ? accepts_by_tif_[i] : 0;
    }
    std::uint64_t accepts_by_type(OrderType t) const noexcept {
        const auto i = static_cast<std::size_t>(t);
        return i < 10 ? accepts_by_type_[i] : 0;
    }
    double acceptance_ratio() const noexcept {
        const std::uint64_t added = stats_.total_orders_added;
        const std::uint64_t rej   = stats_.total_orders_rejected;
        const std::uint64_t total = added + rej;
        if (total == 0) return 0.0;
        return static_cast<double>(added) / static_cast<double>(total);
    }
    // Najbardziej "popularna" kategoria rejectu — diagnostic.
    RejectReason most_common_reject_reason() const noexcept {
        std::size_t best_i = 0;
        std::uint64_t best_v = 0;
        for (std::size_t i = 1; i < 13; ++i) {  // skip NONE=0
            if (rejections_by_reason_[i] > best_v) {
                best_v = rejections_by_reason_[i];
                best_i = i;
            }
        }
        return static_cast<RejectReason>(best_i);
    }

private:
    void tally_rejection(RejectReason r) noexcept {
        const auto i = static_cast<std::size_t>(r);
        if (i < 13) ++rejections_by_reason_[i];
    }
    void tally_accept(OrderType t, TimeInForce tif) noexcept {
        const auto ti = static_cast<std::size_t>(t);
        const auto fi = static_cast<std::size_t>(tif);
        if (ti < 10) ++accepts_by_type_[ti];
        if (fi < 5)  ++accepts_by_tif_[fi];
    }
public:

    // ====================================================================
    // for_each_order — read-only iterator po wszystkich aktywnych orderach
    // ====================================================================
    //
    // Wywołuje Visitor(const Order&) dla każdego aktywnego ordera (we wszystkich
    // levels, w FIFO order per level, najlepsze ceny pierwsze). Replay/audit.
    // ====================================================================
    // Event sequence numbers — monotonic, dla consumer gap detection / dedup
    // ====================================================================
    std::uint64_t last_event_seq_num() const noexcept { return last_emitted_seq_; }

    // ====================================================================
    // BookHealth — jednorazowy dashboard snapshot (zero alocacji)
    // ====================================================================
    //
    // Aggreguje najbardziej przydatne metryki dla operator panel / monitoring
    // / circuit-breaker decyzji w jeden tani odczyt.
    struct BookHealth {
        // Liquidity
        std::int32_t  spread_ticks;
        std::int32_t  imbalance_bps_tob;
        std::int32_t  imbalance_bps_3;
        double        hidden_liquidity_ratio;
        // Flow
        std::int32_t  flow_imbalance_bps;
        std::uint32_t vpin_bps;
        double        cancel_to_trade_ratio;
        // Microstructure
        double        mean_quoted_spread_ticks;
        double        mean_effective_spread_ticks;
        double        effective_to_quoted_ratio;
        // Stability
        std::uint64_t quote_flicker_count;
        std::uint64_t max_tob_unchanged_streak;
        std::uint64_t spread_compression_count;
        // Counts
        std::uint64_t total_fills;
        std::uint64_t total_orders_added;
        std::uint64_t total_orders_cancelled;
        std::uint64_t last_event_seq_num;
    };
    BookHealth health_snapshot() noexcept {
        BookHealth h{};
        h.spread_ticks           = spread_ticks();
        h.imbalance_bps_tob      = imbalance_bps();
        h.imbalance_bps_3        = imbalance_bps_n(3);
        h.hidden_liquidity_ratio = hidden_liquidity_ratio();
        h.flow_imbalance_bps     = flow_imbalance_bps();
        h.vpin_bps               = vpin_bps();
        h.cancel_to_trade_ratio  = cancel_to_trade_ratio();
        h.mean_quoted_spread_ticks    = mean_quoted_spread_ticks();
        h.mean_effective_spread_ticks = mean_effective_spread_ticks();
        h.effective_to_quoted_ratio   = effective_to_quoted_ratio();
        h.quote_flicker_count    = quote_flicker_count();
        h.max_tob_unchanged_streak = max_tob_unchanged_streak_observed();
        h.spread_compression_count = spread_compression_count();
        h.total_fills            = stats_.total_fills;
        h.total_orders_added     = stats_.total_orders_added;
        h.total_orders_cancelled = stats_.total_orders_cancelled;
        h.last_event_seq_num     = last_emitted_seq_;
        return h;
    }

    // ====================================================================
    // Hidden liquidity ratio (visible vs hidden capacity)
    // ====================================================================
    //
    // Iceberg/HIDDEN orders mają część qty niewidoczną w L1/L2 depth.
    // Ratio = Σ hidden / (Σ visible + Σ hidden). Wysokie = dużo dark liquidity.
    double hidden_liquidity_ratio() const noexcept {
        std::int64_t vis = 0, hid = 0;
        for (std::int32_t p = 0; p < LEVELS; ++p) {
            vis += levels_[p].total_qty;
            hid += levels_[p].total_hidden;
        }
        const std::int64_t total = vis + hid;
        if (total == 0) return 0.0;
        return static_cast<double>(hid) / static_cast<double>(total);
    }

    // Per-side resting order counts (visible only — hidden zliczone osobno).
    // O(LEVELS) — używaj okresowo, nie w hot pathy.
    std::int32_t resting_order_count(Side side) const noexcept {
        std::int32_t n = 0;
        if (side == Side::BUY) {
            for (std::int32_t p = 0; p <= best_bid_ticks_ && p < LEVELS; ++p) {
                n += levels_[p].order_count;
            }
        } else {
            for (std::int32_t p = best_ask_ticks_; p < LEVELS; ++p) {
                n += levels_[p].order_count;
            }
        }
        return n;
    }

    template <typename Visitor>
    void for_each_order(Visitor v) const noexcept {
        // Bids od best w dół
        for (std::int32_t p = best_bid_ticks_; p >= 0 && p != NO_BID_TICKS; --p) {
            const PriceLevel& lvl = levels_[p];
            for (const Order* o = lvl.head; o != nullptr; o = o->next_at_level) {
                v(*o);
            }
            if (p == 0) break;
        }
        // Asks od best w górę
        for (std::int32_t p = best_ask_ticks_;
             p < LEVELS && p != NO_ASK_TICKS; ++p) {
            const PriceLevel& lvl = levels_[p];
            for (const Order* o = lvl.head; o != nullptr; o = o->next_at_level) {
                v(*o);
            }
        }
    }

    // current_tob_snapshot(): immutable read bez resetu state.
    bool tob_has_changed_since_last_poll() const noexcept {
        return (best_bid_ticks_ != last_observed_bid_ticks_)
            || (best_ask_ticks_ != last_observed_ask_ticks_);
    }

    // ====================================================================
    // Multi-level imbalance
    // ====================================================================
    //
    // imbalance_bps_n(n_levels): agregowany imbalance po N najgłębszych
    // poziomach per side, nie tylko TOB. Realny order flow signal — TOB
    // imbalance łatwo manipulowany, ale depth-3 lub depth-5 trudniej.
    std::int32_t imbalance_bps_n(std::int32_t n_levels) const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        std::int64_t b = 0, a = 0;
        std::int32_t bn = 0, an = 0;
        for (std::int32_t p = best_bid_ticks_; p >= 0 && bn < n_levels; --p) {
            const std::int32_t q = levels_[p].total_qty;
            if (q > 0) { b += q; ++bn; }
        }
        for (std::int32_t p = best_ask_ticks_; p < LEVELS && an < n_levels; ++p) {
            const std::int32_t q = levels_[p].total_qty;
            if (q > 0) { a += q; ++an; }
        }
        const std::int64_t total = b + a;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((b - a) * 10000 / total);
    }

    // ====================================================================
    // Market impact estimator (pre-trade analytics)
    // ====================================================================
    //
    // predicted_vwap_ticks(side, qty): symuluje walk-the-book bez modyfikacji
    // księgi. Zwraca expected VWAP (ticks) jeśli order o `qty` byłby IOC.
    // 0 = brak liquidity. Side::BUY zjada asks, Side::SELL zjada bids.
    std::int32_t predicted_vwap_ticks(Side side, std::int32_t qty) const noexcept {
        if (qty <= 0) return 0;
        std::int64_t notional_ticks = 0;
        std::int32_t filled = 0;
        if (side == Side::BUY) {
            if (!has_ask()) return 0;
            for (std::int32_t p = best_ask_ticks_; p < LEVELS && filled < qty; ++p) {
                const std::int32_t avail = levels_[p].total_qty;
                if (avail <= 0) continue;
                const std::int32_t take = std::min(qty - filled, avail);
                notional_ticks += static_cast<std::int64_t>(p) * take;
                filled        += take;
            }
        } else {
            if (!has_bid()) return 0;
            for (std::int32_t p = best_bid_ticks_; p >= 0 && filled < qty; --p) {
                const std::int32_t avail = levels_[p].total_qty;
                if (avail <= 0) continue;
                const std::int32_t take = std::min(qty - filled, avail);
                notional_ticks += static_cast<std::int64_t>(p) * take;
                filled        += take;
            }
        }
        if (filled == 0) return 0;
        return static_cast<std::int32_t>(notional_ticks / filled);
    }

    // slippage_ticks: predicted_vwap - mid. Signed dla BUY positive (płacę więcej),
    // SELL negative (dostaję mniej). Wartość bezwzględna = oczekiwany koszt.
    std::int32_t predicted_slippage_ticks(Side side, std::int32_t qty) const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        const std::int32_t mid = (best_bid_ticks_ + best_ask_ticks_) / 2;
        const std::int32_t vwap = predicted_vwap_ticks(side, qty);
        if (vwap == 0) return 0;
        return vwap - mid;
    }

    // depth_available_ticks(side, max_price_offset): suma qty dostępnej do
    // ceny mid ± offset. Używane do szybkiego "ile mogę kupić bez przeskakiwania
    // > N ticków od mid?".
    std::int32_t depth_within_offset(Side side, std::int32_t max_offset) const noexcept {
        if (!has_bid() || !has_ask()) return 0;
        const std::int32_t mid = (best_bid_ticks_ + best_ask_ticks_) / 2;
        std::int32_t total = 0;
        if (side == Side::BUY) {
            const std::int32_t cap = std::min<std::int32_t>(LEVELS - 1, mid + max_offset);
            for (std::int32_t p = best_ask_ticks_; p <= cap; ++p) {
                total += levels_[p].total_qty;
            }
        } else {
            const std::int32_t floor_p = std::max<std::int32_t>(0, mid - max_offset);
            for (std::int32_t p = best_bid_ticks_; p >= floor_p; --p) {
                total += levels_[p].total_qty;
            }
        }
        return total;
    }

    // ====================================================================
    // Sweep-to-fill metrics
    // ====================================================================
    //
    // avg_levels_per_sweep — średnia liczba price levels per egzekucja.
    // Wysoka wartość = thin book / large orders (toxic flow indicator).
    double avg_levels_per_sweep() const noexcept {
        if (stats_.total_sweeps == 0) return 0.0;
        return static_cast<double>(stats_.sum_levels_touched) /
               static_cast<double>(stats_.total_sweeps);
    }

    // multi_level_sweep_ratio — proporcja sweepów które uderzyły w >=2 levels.
    double multi_level_sweep_ratio() const noexcept {
        if (stats_.total_sweeps == 0) return 0.0;
        return static_cast<double>(stats_.multi_level_sweeps) /
               static_cast<double>(stats_.total_sweeps);
    }

    // ====================================================================
    // Order age stats — kanibalizm flow / queue residency
    // ====================================================================
    //
    // Tracked w match_at_level po fill + w cancel_internal.
    // Dla każdego completed lifecycle (fill or cancel), record age_ns
    // = now - submit_ts_ns_. Wykorzystywane do detection toxic queue
    // (gdy orders szybko fillowane = aggressive flow; długo czekające =
    // resting MM).
    std::uint64_t total_completed_lifecycles() const noexcept {
        return age_stats_count_;
    }
    std::uint64_t total_age_ns() const noexcept { return age_stats_sum_ns_; }
    std::uint64_t max_age_ns_observed() const noexcept { return age_stats_max_ns_; }
    std::uint64_t avg_age_ns_at_completion() const noexcept {
        return age_stats_count_ == 0 ? 0
            : age_stats_sum_ns_ / age_stats_count_;
    }

    // ====================================================================
    // Execution quality / TCA metrics
    // ====================================================================
    //
    // Quoted spread — okresowo sampluj wywołując sample_quoted_spread() z
    // pętli marketdata. Mean quoted spread = Σ obs / N.
    // Effective spread — accumulowany per fill w match_against():
    //   eff_spread = 2 × |fill_px - mid_pre_match|
    // Relacja eff/quoted < 1 → price improvement; > 1 → adverse selection.
    void sample_quoted_spread() noexcept {
        if (!has_bid() || !has_ask()) return;
        stats_.total_quoted_spread_ticks_obs +=
            static_cast<std::uint64_t>(best_ask_ticks_ - best_bid_ticks_);
        ++stats_.total_quoted_spread_samples;
    }
    double mean_quoted_spread_ticks() const noexcept {
        if (stats_.total_quoted_spread_samples == 0) return 0.0;
        return static_cast<double>(stats_.total_quoted_spread_ticks_obs) /
               static_cast<double>(stats_.total_quoted_spread_samples);
    }
    double mean_effective_spread_ticks() const noexcept {
        if (stats_.total_effective_spread_samples == 0) return 0.0;
        return static_cast<double>(stats_.total_effective_spread_2x_ticks) /
               static_cast<double>(stats_.total_effective_spread_samples);
    }
    // Realized vs quoted ratio (TCA klasyka). 0 == brak danych.
    double effective_to_quoted_ratio() const noexcept {
        const double q = mean_quoted_spread_ticks();
        if (q <= 0.0) return 0.0;
        return mean_effective_spread_ticks() / q;
    }

    // ====================================================================
    // Cancel-to-trade ratio (CTR) — per-book i per-account
    // ====================================================================
    //
    // SEC Rule 15c3-5 / MiFID II RTS 9: nadmierny CTR (>50:1) wskazuje na
    // quote stuffing / spoofing. Wyliczane na podstawie cumulative stats.
    double cancel_to_trade_ratio() const noexcept {
        if (stats_.total_fills == 0) return 0.0;
        return static_cast<double>(stats_.total_orders_cancelled) /
               static_cast<double>(stats_.total_fills);
    }
    double priority_loss_ratio() const noexcept {
        const std::uint64_t total = stats_.priority_preserved_mods +
                                     stats_.priority_lost_mods;
        if (total == 0) return 0.0;
        return static_cast<double>(stats_.priority_lost_mods) /
               static_cast<double>(total);
    }

private:
    // Order age stats state
    std::uint64_t  age_stats_count_  = 0;
    std::uint64_t  age_stats_sum_ns_ = 0;
    std::uint64_t  age_stats_max_ns_ = 0;

    void record_lifecycle_age(std::uint64_t submit_ts_ns) noexcept {
        const std::uint64_t now = mono_ns_now();
        if (submit_ts_ns == 0 || now < submit_ts_ns) return;
        const std::uint64_t age = now - submit_ts_ns;
        ++age_stats_count_;
        age_stats_sum_ns_ += age;
        if (age > age_stats_max_ns_) age_stats_max_ns_ = age;
    }

public:

    // Hook: update size distribution z record_trade()
    void update_size_distribution(std::int32_t qty) noexcept {
        if (qty <= 100) {
            ++size_dist_.small_count;
            size_dist_.small_volume += static_cast<std::uint64_t>(qty);
        } else if (qty <= 1000) {
            ++size_dist_.medium_count;
            size_dist_.medium_volume += static_cast<std::uint64_t>(qty);
        } else if (qty <= 10000) {
            ++size_dist_.large_count;
            size_dist_.large_volume += static_cast<std::uint64_t>(qty);
        } else {
            ++size_dist_.block_count;
            size_dist_.block_volume += static_cast<std::uint64_t>(qty);
        }
    }

public:
    // ====================================================================
    // Spread + microstructure analytics
    // ====================================================================
    //
    // spread_ticks — best_ask - best_bid (>=0 in continuous mode).
    // Zwraca -1 gdy nie ma quote po którejś stronie.
    std::int32_t spread_ticks() const noexcept {
        if (!has_bid() || !has_ask()) return -1;
        return best_ask_ticks_ - best_bid_ticks_;
    }

    // half_spread_bps — relative spread w bps (mid_price reference).
    // Standardowa miara liquidity cost. < 5 bps = tight, > 50 bps = wide.
    std::int32_t half_spread_bps() const noexcept {
        if (!has_bid() || !has_ask()) return -1;
        const std::int64_t mid    = (best_bid_ticks_ + best_ask_ticks_) / 2;
        const std::int64_t sprd   = best_ask_ticks_ - best_bid_ticks_;
        if (mid <= 0) return -1;
        return static_cast<std::int32_t>((sprd * 10000 / 2) / mid);
    }

    // weighted_mid_ticks — alias dla microprice (popularna nazwa w lit).
    std::int32_t weighted_mid_ticks() const noexcept { return microprice_ticks(); }

    // ====================================================================
    // Mass quote — atomic batch submission (market maker scenario)
    // ====================================================================
    //
    // MM zwykle wystawia 2-stronną kwotę (bid + ask) atomowo, żeby nie być
    // expozowany na chwilowy ruch przy non-atomic submissions. mass_quote()
    // atomic: ALL-OR-NONE (jeśli jeden by failed, NIC nie idzie).
    //
    // Caller dostarcza array `quotes[n]`. Każdy element to {side, price, qty}.
    // Zwraca liczbę przyjętych zleceń (== n on success, 0 na pierwszy fail).
    struct Quote {
        Side          side;
        std::int32_t  price_ticks;
        std::int32_t  qty;
    };
    std::size_t mass_quote(const Quote* quotes, std::size_t n,
                            std::uint64_t client_id,
                            std::uint64_t* out_ids = nullptr) noexcept {
        // 2-pass: faza walidacji (sprawdzimy że pula i ceny OK), faza submit.
        if (active_orders_ + n > MAX_ORDERS) {
            ++stats_.total_orders_rejected;
            tally_rejection(RejectReason::POOL_EXHAUSTED);
            return 0;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const auto& q = quotes[i];
            if (q.qty <= 0 || !in_range(q.price_ticks)) {
                ++stats_.total_orders_rejected;
                tally_rejection(q.qty <= 0 ? RejectReason::QTY_ZERO_OR_NEGATIVE
                                            : RejectReason::PRICE_OUT_OF_RANGE);
                return 0;
            }
            // POST_ONLY semantic — quote nie może krzyżować rynku
            if (would_cross(q.side, q.price_ticks)) {
                ++stats_.total_orders_rejected;
                tally_rejection(RejectReason::POST_ONLY_WOULD_CROSS);
                return 0;
            }
        }
        // Wszystko OK — submit each
        for (std::size_t i = 0; i < n; ++i) {
            const auto& q = quotes[i];
            const std::uint64_t id = submit(q.side, q.price_ticks, q.qty,
                                              OrderType::POST_ONLY,
                                              TimeInForce::DAY, 0, client_id);
            if (out_ids) out_ids[i] = id;
        }
        ++stats_.total_mass_quotes;
        return n;
    }

    // ====================================================================
    // Liquidity heatmap data — N levels per side z rozszerzonym info
    // ====================================================================
    //
    // Zwraca strukturę gęstej płynności wokół top of book.
    struct LiquiditySnapshot {
        DepthLevel    bid_levels[10];
        DepthLevel    ask_levels[10];
        std::int32_t  bid_count;
        std::int32_t  ask_count;
        std::int32_t  spread_ticks;
        std::int32_t  imbalance_bps;
        std::int64_t  total_bid_volume;
        std::int64_t  total_ask_volume;
        std::int32_t  microprice_ticks;
    };

    // Auction mode — caller wywołuje przed batch submit dla cross.
    // Po enter_auction_mode(), każdy submit pomija matching i siedzi w księdze.
    // Po exit_auction_mode() + run_auction(), single-price cross matched FIFO.
    void enter_auction_mode() noexcept { in_auction_mode_ = true; }
    void exit_auction_mode()  noexcept { in_auction_mode_ = false; }
    bool in_auction_mode()    const noexcept { return in_auction_mode_; }

    // Halt/resume — trading halt.
    // Audit log API.
    void enable_audit_log(bool on) noexcept { audit_enabled_ = on; }
    bool audit_log_enabled() const noexcept { return audit_enabled_; }
    std::size_t audit_log_size() const noexcept { return audit_log_.size(); }

    // pop_audit_records: skopiuj do `out` (max max_n), wyczyść z bufora.
    // Zwraca ile pobranych.
    std::size_t pop_audit_records(AuditRecord* out, std::size_t max_n) noexcept {
        const std::size_t n = std::min(max_n, audit_log_.size());
        for (std::size_t i = 0; i < n; ++i) out[i] = audit_log_[i];
        audit_log_.erase(audit_log_.begin(),
                          audit_log_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    void halt(const char* reason) noexcept {
        halted_ = true;
        std::memset(halt_reason_, 0, sizeof(halt_reason_));
        if (reason) {
            const std::size_t n = std::min(sizeof(halt_reason_) - 1,
                                             std::strlen(reason));
            std::memcpy(halt_reason_, reason, n);
        }
    }
    void resume() noexcept { halted_ = false; halt_reason_[0] = '\0'; }
    bool is_halted() const noexcept { return halted_; }
    const char* halt_reason() const noexcept { return halt_reason_; }

    LiquiditySnapshot liquidity_snapshot() const noexcept {
        LiquiditySnapshot s{};
        depth(10, s.bid_levels, s.ask_levels, &s.bid_count, &s.ask_count);
        s.spread_ticks      = spread_ticks();
        s.imbalance_bps     = imbalance_bps();
        s.microprice_ticks  = microprice_ticks();
        for (std::int32_t i = 0; i < s.bid_count; ++i)
            s.total_bid_volume += s.bid_levels[i].qty;
        for (std::int32_t i = 0; i < s.ask_count; ++i)
            s.total_ask_volume += s.ask_levels[i].qty;
        return s;
    }
};


// ====================================================================
// BookCluster<N_SYMBOLS, ...> — wielo-symbolowa książka
// ====================================================================
//
// Wrapper na N osobnych FullOrderBook'ów z O(1) lookup po nazwie symbolu.
// Symbole są kodowane jako 8-char ASCII pakowane do uint64 (sym_to_key
// idea: każdy znak w 8 bitach low-byte first).
//
// Po co? Realne venues handlują tysiące symboli — separate book per symbol
// daje pełną izolację (one symbol's halt nie blokuje innych) + cache locality
// (każdy book ma własne levels[], orders[]).
//
// Konfiguracja: N_SYMBOLS = max liczba różnych symboli. LEVELS / MAX_ORDERS_PER_SYM
// per-book.
//
// Cross-symbol features:
//   - total_volume_across_all() — Σ wolumen wszystkich symboli
//   - avg_spread_ticks() — średni spread w klastrze (proxy dla market quality)
//   - busiest_symbol() — symbol z największą ilością aktywnych zleceń
template <std::size_t N_SYMBOLS = 16,
          std::int32_t LEVELS = 16384,
          std::int32_t MAX_ORDERS_PER_SYM = 8192>
class BookCluster {
    using BookT = FullOrderBook<LEVELS, MAX_ORDERS_PER_SYM>;

    BookT  books_[N_SYMBOLS];
    char   symbols_[N_SYMBOLS][9];    // 8 chars + null
    bool   slot_used_[N_SYMBOLS]      = {};
    std::size_t active_count_         = 0;

    // sym → uint64 packing (8 chars LSB first)
    static std::uint64_t pack(const char* sym) noexcept {
        // Najpierw zmierz długość przez memchr (cppcheck rozumie ten wzorzec
        // i nie zgłasza arrayIndexOutOfBoundsCond na short-circuit z `sym[i]`).
        std::uint64_t k = 0;
        const void* nul = std::memchr(sym, '\0', 8);
        const std::size_t n = nul
            ? static_cast<std::size_t>(static_cast<const char*>(nul) - sym)
            : 8;
        for (std::size_t i = 0; i < n; ++i) {
            k |= static_cast<std::uint64_t>(static_cast<unsigned char>(sym[i])) << (i * 8);
        }
        return k;
    }

    // Liniowy search po slotach. N_SYMBOLS ≤ 16 — szybsze niż unordered_map
    // (1-2 cache lines mieści się w cache L1, branch predictor radzi sobie).
    std::int32_t find_slot(const char* sym) const noexcept {
        const std::uint64_t key = pack(sym);
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            if (pack(symbols_[i]) == key) return static_cast<std::int32_t>(i);
        }
        return -1;
    }

public:
    BookCluster() noexcept {
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) symbols_[i][0] = '\0';
    }

    BookCluster(const BookCluster&)            = delete;
    BookCluster& operator=(const BookCluster&) = delete;
    BookCluster(BookCluster&&)                 = delete;
    BookCluster& operator=(BookCluster&&)      = delete;

    // Zarejestruj nowy symbol. Zwraca true on success, false gdy slot
    // wyczerpany albo symbol już istnieje.
    bool register_symbol(const char* sym) noexcept {
        if (find_slot(sym) >= 0) return false;  // already registered
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) {
                slot_used_[i] = true;
                std::strncpy(symbols_[i], sym, 8);
                symbols_[i][8] = '\0';
                ++active_count_;
                return true;
            }
        }
        return false;
    }

    BookT* book(const char* sym) noexcept {
        const std::int32_t slot = find_slot(sym);
        return slot >= 0 ? &books_[slot] : nullptr;
    }
    const BookT* book(const char* sym) const noexcept {
        const std::int32_t slot = find_slot(sym);
        return slot >= 0 ? &books_[slot] : nullptr;
    }

    std::size_t active_symbol_count() const noexcept { return active_count_; }
    std::size_t capacity_symbols()    const noexcept { return N_SYMBOLS; }

    // Cross-symbol aggregations
    std::uint64_t total_volume_across_all() const noexcept {
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) total += books_[i].stats().total_volume;
        }
        return total;
    }

    // avg_spread_ticks: średni spread po symbolach które mają obie strony quote.
    // Zwraca -1 gdy żaden symbol nie ma TOB.
    std::int32_t avg_spread_ticks() const noexcept {
        std::int64_t sum = 0;
        std::int32_t cnt = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            const std::int32_t s = books_[i].spread_ticks();
            if (s >= 0) { sum += s; ++cnt; }
        }
        return cnt > 0 ? static_cast<std::int32_t>(sum / cnt) : -1;
    }

    // busiest_symbol: symbol z największym active_orders(). Zwraca nullptr gdy puste.
    const char* busiest_symbol() const noexcept {
        std::size_t best = N_SYMBOLS;
        std::size_t best_count = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            const std::size_t c = books_[i].active_orders();
            if (c > best_count) { best_count = c; best = i; }
        }
        return best < N_SYMBOLS ? symbols_[best] : nullptr;
    }

    // total_active_orders_across_all
    std::size_t total_active_orders() const noexcept {
        std::size_t t = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) t += books_[i].active_orders();
        }
        return t;
    }

    // Cluster-wide cumulative fills (Σ stats_.total_fills per symbol).
    std::uint64_t cluster_total_fills() const noexcept {
        std::uint64_t t = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) t += books_[i].stats().total_fills;
        }
        return t;
    }
    std::uint64_t cluster_total_volume() const noexcept {
        std::uint64_t t = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) t += books_[i].stats().total_volume;
        }
        return t;
    }
    std::uint64_t cluster_total_orders_added() const noexcept {
        std::uint64_t t = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (slot_used_[i]) t += books_[i].stats().total_orders_added;
        }
        return t;
    }
    // Volume-weighted average spread across cluster (ważone total_volume per symbol).
    // Bardziej realistyczny niż simple avg — symbole z większym flow większy weight.
    double volume_weighted_avg_spread_ticks() const noexcept {
        std::int64_t weighted_spread = 0;
        std::uint64_t total_volume = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            const std::int32_t s = books_[i].spread_ticks();
            if (s < 0) continue;
            const std::uint64_t v = books_[i].stats().total_volume;
            if (v == 0) continue;
            weighted_spread += static_cast<std::int64_t>(s) * v;
            total_volume += v;
        }
        if (total_volume == 0) return 0.0;
        return static_cast<double>(weighted_spread) /
               static_cast<double>(total_volume);
    }

    // Cluster-wide cumulative flow imbalance ważony po volumie per symbol.
    // Σ buy_vol / Σ (buy + sell) vol × 10000 (bps).
    std::int32_t cluster_flow_imbalance_bps() const noexcept {
        std::int64_t buy = 0, sell = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            buy  += static_cast<std::int64_t>(books_[i].taker_buy_volume());
            sell += static_cast<std::int64_t>(books_[i].taker_sell_volume());
        }
        const std::int64_t total = buy + sell;
        if (total == 0) return 0;
        return static_cast<std::int32_t>((buy - sell) * 10000 / total);
    }

    // Symbol z najwyższym book-level flow_imbalance (informational flow magnet).
    const char* most_imbalanced_symbol() const noexcept {
        std::size_t best = N_SYMBOLS;
        std::int32_t best_abs = -1;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i]) continue;
            const std::int32_t imb = std::abs(books_[i].flow_imbalance_bps());
            if (imb > best_abs) { best_abs = imb; best = i; }
        }
        return best < N_SYMBOLS ? symbols_[best] : nullptr;
    }

    // ====================================================================
    // Cross-symbol arbitrage detection
    // ====================================================================
    //
    // Skanuje pary symboli w klastrze i raportuje sytuacje gdzie:
    //   bid_X >= ask_Y  (możemy kupić na Y, sprzedać na X z profitem)
    //
    // Use case: arbitrage detection przy listing tej samej akcji na wielu
    // venues (NASDAQ vs NYSE) lub ETF arb (SPY vs IVV vs VOO). W praktyce
    // realny arb wymaga uwzględnienia opłat + slippage, ale ta funkcja daje
    // RAW signal.
    struct ArbOpportunity {
        char         long_symbol[9];   // gdzie ASK (kupimy)
        char         short_symbol[9];  // gdzie BID (sprzedamy)
        std::int32_t buy_price_ticks;  // ask na long_symbol
        std::int32_t sell_price_ticks; // bid na short_symbol
        std::int32_t spread_ticks;     // sell - buy (zysk pre-fees)
        std::int32_t max_qty;          // min(ask_qty_long, bid_qty_short)
    };

    // detect_cross_arb: wypełnia `out[]` (max max_n) lukami arb. Zwraca ile.
    std::size_t detect_cross_arb(ArbOpportunity* out, std::size_t max_n) const noexcept {
        std::size_t found = 0;
        for (std::size_t i = 0; i < N_SYMBOLS && found < max_n; ++i) {
            if (!slot_used_[i]) continue;
            if (!books_[i].has_bid()) continue;
            for (std::size_t j = 0; j < N_SYMBOLS && found < max_n; ++j) {
                if (i == j || !slot_used_[j]) continue;
                if (!books_[j].has_ask()) continue;
                const std::int32_t bid_i = books_[i].best_bid_ticks();
                const std::int32_t ask_j = books_[j].best_ask_ticks();
                if (bid_i > ask_j) {
                    // Arb: kupić na j @ ask_j, sprzedać na i @ bid_i
                    ArbOpportunity& o = out[found++];
                    std::strncpy(o.long_symbol,  symbols_[j], 8);
                    o.long_symbol[8] = '\0';
                    std::strncpy(o.short_symbol, symbols_[i], 8);
                    o.short_symbol[8] = '\0';
                    o.buy_price_ticks  = ask_j;
                    o.sell_price_ticks = bid_i;
                    o.spread_ticks     = bid_i - ask_j;
                    const auto tob_i = books_[i].top_of_book();
                    const auto tob_j = books_[j].top_of_book();
                    o.max_qty = std::min(tob_j.ask_qty, tob_i.bid_qty);
                }
            }
        }
        return found;
    }

    // count_cross_arb: ile arb opportunities. Bez kopiowania do bufora.
    std::size_t count_cross_arb() const noexcept {
        std::size_t cnt = 0;
        for (std::size_t i = 0; i < N_SYMBOLS; ++i) {
            if (!slot_used_[i] || !books_[i].has_bid()) continue;
            for (std::size_t j = 0; j < N_SYMBOLS; ++j) {
                if (i == j || !slot_used_[j] || !books_[j].has_ask()) continue;
                if (books_[i].best_bid_ticks() > books_[j].best_ask_ticks()) ++cnt;
            }
        }
        return cnt;
    }
};


}  // namespace orderbook_pro

