/*
 * FIXSession — warstwa sesji FIX (Logon/Logout/Heartbeat/SeqNum tracking).
 *
 * Po co osobno od FIXMessage (parser)? FIXMessage to "co JEDNA wiadomość znaczy"
 * (parsing + walidacja CheckSum/BodyLength). FIXSession to "jak STAN SESJI się
 * zmienia" — multi-message protocol: kto-do-kogo, sequence in/out, heartbeats,
 * resend requests przy lukach. Bez tego nie podpiniesz się do realnej giełdy.
 *
 * Cykl życia sesji FIX:
 *
 *   1. Initiator (client): wysyła Logon (35=A) z HeartBtInt (108=30)
 *   2. Acceptor (broker/exch): odpowiada Logon — sesja UP
 *   3. Obie strony: wysyłają wiadomości aplikacyjne (D=NewOrder, 8=Execution...)
 *      Każda z monotonicznym MsgSeqNum (tag 34). Pierwsza = 1, +1 per wiadomość.
 *   4. Każda strona: wysyła Heartbeat (35=0) co HeartBtInt sekund gdy idle.
 *      Brak heartbeat'a przez 2× HeartBtInt → druga strona wysyła TestRequest
 *      (35=1), jeszcze przez HeartBtInt brak odpowiedzi → disconnect.
 *   5. Gdy odebrany MsgSeqNum > expected → ResendRequest (35=2) "od X do Y".
 *      Druga strona wysyła zaległe albo SequenceReset-GapFill (35=4) jeśli
 *      zaginione były administracyjne (nie do replay'u).
 *   6. Logout (35=5) → graceful close.
 *
 * Co tu mamy:
 *   - inbound seq tracking + gap detection
 *   - outbound seq counter (do wstrzykiwania w wiadomości wychodzące)
 *   - timer logika do heartbeat'ów (caller wywołuje tick(now_ms) i pyta
 *     czy trzeba wysłać heartbeat / czy disconnect)
 *   - flagi stanu sesji: DISCONNECTED → LOGON_SENT → LOGGED_IN → LOGOUT
 *
 * Co domyka pętlę sesji (expansion #78):
 *   - serializacja admin messages: build_logon/heartbeat/test_request/
 *     resend_request/sequence_reset/logout — generują poprawny wire FIX
 *     (z 8/9/10) i wstrzykują outbound MsgSeqNum (tag 34)
 *   - persistencja sequence numbers: persist_seq/load_persisted_seq (atomic
 *     write jak RiskManager) — restart procesu nie reużyje już-wysłanego seq
 *
 * Czego NADAL NIE robimy (poza scope): faktyczne wysyłanie po TCP (warstwa
 * wyżej, np. network/epoll_server) i pełny store-and-forward replay zaległych
 * wiadomości aplikacyjnych (build_sequence_reset GapFill pokrywa stronę admin).
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>      // ::fsync, ::fileno — durable persist seq

#include "fix_parser.hpp"          // FIXMessage::build_message
#include "../common/types.hpp"     // Side (order-entry builders, #90)


namespace fix {


enum class SessionState : uint8_t {
    DISCONNECTED = 0,   // przed Logon albo po Logout
    LOGON_SENT   = 1,   // wysłaliśmy Logon, czekamy na odpowiedź
    LOGGED_IN    = 2,   // sesja w pełni up — wymiana aplikacyjnych wiadomości
    LOGOUT       = 3,   // jedna ze stron wysłała Logout
};


struct GapDetected {
    uint32_t expected;   // numer którego oczekiwaliśmy
    uint32_t received;   // numer który przyszedł (> expected)
    bool     valid;      // true gdy luka rzeczywiście istnieje
};


class FIXSession {
    SessionState state_ = SessionState::DISCONNECTED;

    uint32_t expected_in_seq_  = 1;   // następny oczekiwany MsgSeqNum (przychodzący)
    uint32_t next_out_seq_     = 1;   // następny do nadania w wychodzącej wiadomości
    int32_t  heartbeat_int_sec_ = 30; // negocjowane w Logon (tag 108)
    int64_t  last_inbound_ms_  = 0;
    int64_t  last_outbound_ms_ = 0;
    bool     test_request_pending_ = false;

    // Tożsamość sesji (tag 49/56) — wstawiana w wychodzące admin messages.
    char sender_comp_[32] = "SENDER";
    char target_comp_[32] = "TARGET";
    char pending_test_req_id_[32] = {0};   // niepusty = otrzymano TestRequest (#158)

    // Persistencja sequence numbers. Pusty = wyłączona (zachowanie wstecz
    // kompatybilne). Production MUSI to mieć — restart procesu nie może
    // wysłać już-użytego MsgSeqNum ani prosić o resend od złego miejsca.
    std::string persist_path_;

    // Statystyki
    uint64_t gaps_detected_   = 0;
    uint64_t heartbeats_sent_ = 0;
    uint64_t resends_requested_ = 0;

public:
    FIXSession() noexcept = default;

    // set_comp_ids: ustaw SenderCompID (tag 49) i TargetCompID (tag 56)
    // wstawiane do wszystkich budowanych admin messages.
    void set_comp_ids(const char* sender, const char* target) noexcept {
        if (sender) { std::strncpy(sender_comp_, sender, sizeof(sender_comp_) - 1);
                      sender_comp_[sizeof(sender_comp_) - 1] = '\0'; }
        if (target) { std::strncpy(target_comp_, target, sizeof(target_comp_) - 1);
                      target_comp_[sizeof(target_comp_) - 1] = '\0'; }
    }

    // === Stan ===
    SessionState state()      const noexcept { return state_; }
    bool         is_logged_in() const noexcept { return state_ == SessionState::LOGGED_IN; }

    // === Outbound sequence ===
    // Wywołaj PRZED zbudowaniem każdej wychodzącej wiadomości — zwraca numer
    // do wstawienia w tag 34, atomowo inkrementuje wewnętrzny licznik.
    uint32_t next_outbound_seq() noexcept { return next_out_seq_++; }
    uint32_t peek_outbound_seq() const noexcept { return next_out_seq_; }

    // === Inbound sequence + gap detection ===
    // observe_inbound: wywołaj na każdej przychodzącej wiadomości APLIKACYJNEJ
    // z parsowanym tag 34. Zwraca GapDetected (valid=true gdy luka), aktualizuje
    // expected_in_seq_. Duplikat (seq < expected) zwraca valid=false (caller
    // może zignorować).
    GapDetected observe_inbound(uint32_t msg_seq_num, int64_t now_ms) noexcept {
        last_inbound_ms_ = now_ms;
        test_request_pending_ = false;
        GapDetected g{0, msg_seq_num, false};
        if (msg_seq_num == expected_in_seq_) {
            ++expected_in_seq_;
            return g;
        }
        if (msg_seq_num > expected_in_seq_) {
            g.expected = expected_in_seq_;
            g.valid    = true;
            ++gaps_detected_;
            expected_in_seq_ = msg_seq_num + 1;  // przyjmujemy wiadomość, ale notujemy lukę
            return g;
        }
        // msg_seq_num < expected — duplikat / spóźnione, ignoruj.
        return g;
    }

    // mark_resend_requested: caller po wykryciu gapDetected wysyła ResendRequest
    // (35=2) i woła to żeby zaktualizować staty.
    void mark_resend_requested() noexcept { ++resends_requested_; }

    // expected_inbound_seq: nastepny oczekiwany MsgSeqNum (tag 34) przychodzacy.
    uint32_t expected_inbound_seq() const noexcept { return expected_in_seq_; }

    // apply_inbound_sequence_reset: po odebraniu SequenceReset (35=4) z NewSeqNo
    // (tag 36) ustaw oczekiwany inbound seq — druga strona przeskakuje numeracje
    // (GapFill administracyjnych albo twardy Reset). Domyka recovery po stronie
    // inbound (parze do build_sequence_reset, ktory go wysyla). Ignoruje numer
    // wsteczny (< expected) — SequenceReset nie cofa numeracji w trybie GapFill.
    void apply_inbound_sequence_reset(uint32_t new_seq_no) noexcept {
        if (new_seq_no >= expected_in_seq_) expected_in_seq_ = new_seq_no;
    }

    // process_inbound (#150): jednolity dispatcher przychodzacej wiadomosci —
    // spina parser (FIXMessage) z sesja. SequenceReset (35=4) -> reset oczekiwanego
    // seq (tag 36). Inne: obserwuj MsgSeqNum (34) -> wykryj luke; potem side-effect
    // wg typu: Logon (A) -> LOGGED_IN + HeartBtInt (108), Logout (5) -> LOGOUT.
    // Zwraca GapDetected (valid=true -> caller wysyla ResendRequest).
    GapDetected process_inbound(const FIXMessage& m, int64_t now_ms) noexcept {
        const char* mt = m.get_msg_type();
        if (mt[0] == '4') {                              // SequenceReset
            if (const char* ns = m.get_field(36)) apply_inbound_sequence_reset(
                static_cast<uint32_t>(std::atoi(ns)));
            last_inbound_ms_ = now_ms;
            return GapDetected{0, 0, false};
        }
        GapDetected gap{0, 0, false};
        if (const char* seq = m.get_field(34))
            gap = observe_inbound(static_cast<uint32_t>(std::atoi(seq)), now_ms);
        else
            last_inbound_ms_ = now_ms;

        if (mt[0] == 'A') {                              // Logon
            const char* hb = m.get_field(108);
            mark_logon_received(hb ? std::atoi(hb) : 30, now_ms);
        } else if (mt[0] == '5') {                       // Logout
            mark_logout(now_ms);
        } else if (mt[0] == '1') {                       // TestRequest (#158)
            // Counterparty zada dowodu zycia — zapamietaj TestReqID, caller
            // odpowie Heartbeatem (35=0) z tym samym 112.
            if (const char* tri = m.get_field(112)) {
                std::strncpy(pending_test_req_id_, tri, sizeof(pending_test_req_id_) - 1);
                pending_test_req_id_[sizeof(pending_test_req_id_) - 1] = '\0';
            }
        }
        return gap;
    }

    // pending_test_req_id: niepusty -> przyszedl TestRequest, odpowiedz
    // build_heartbeat(pending_test_req_id()) i wywolaj clear_pending_test_req (#158).
    const char* pending_test_req_id() const noexcept { return pending_test_req_id_; }
    void clear_pending_test_req() noexcept { pending_test_req_id_[0] = '\0'; }

    // === Transitions ===
    // mark_logon_sent: wywołaj po wysłaniu Logon (35=A). State → LOGON_SENT.
    void mark_logon_sent(int64_t now_ms) noexcept {
        state_           = SessionState::LOGON_SENT;
        last_outbound_ms_ = now_ms;
    }

    // mark_logon_received: wywołaj gdy odebrano Logon od counterparty.
    // hb_int_sec to wartość z tag 108. State → LOGGED_IN.
    void mark_logon_received(int32_t hb_int_sec, int64_t now_ms) noexcept {
        state_             = SessionState::LOGGED_IN;
        heartbeat_int_sec_ = (hb_int_sec > 0) ? hb_int_sec : 30;
        last_inbound_ms_   = now_ms;
    }

    void mark_logout(int64_t now_ms) noexcept {
        state_           = SessionState::LOGOUT;
        last_outbound_ms_ = now_ms;
    }

    void mark_disconnected() noexcept {
        state_ = SessionState::DISCONNECTED;
    }

    // === Heartbeat timer ===
    // Action — co caller ma zrobić po wywołaniu tick().
    enum class Action : uint8_t {
        NONE,                  // nic
        SEND_HEARTBEAT,        // wyślij 35=0 (idle za długo po naszej stronie)
        SEND_TEST_REQUEST,     // wyślij 35=1 (cisza od counterparty)
        DISCONNECT,            // brak reakcji na test request — zerwij
    };

    // tick: wywołuj co sekundę albo z głównej pętli. now_ms = monotonic ms.
    // Zwraca akcję jaką trzeba podjąć (NONE w 99% wywołań).
    Action tick(int64_t now_ms) noexcept {
        if (state_ != SessionState::LOGGED_IN) return Action::NONE;

        const int64_t hb_ms = static_cast<int64_t>(heartbeat_int_sec_) * 1000;

        // 1. Cisza od counterparty > 2×HeartBtInt — wyślij TestRequest.
        //    Jeśli już wysłaliśmy i nadal cisza → disconnect.
        if (now_ms - last_inbound_ms_ > 2 * hb_ms) {
            if (test_request_pending_) return Action::DISCONNECT;
            test_request_pending_ = true;
            return Action::SEND_TEST_REQUEST;
        }

        // 2. My byliśmy za długo cicho (HeartBtInt) — wyślij Heartbeat.
        if (now_ms - last_outbound_ms_ > hb_ms) {
            ++heartbeats_sent_;
            last_outbound_ms_ = now_ms;
            return Action::SEND_HEARTBEAT;
        }

        return Action::NONE;
    }

    // Wywołaj po każdej WYSŁANEJ wiadomości żeby zresetować nasz timer idle.
    void mark_outbound(int64_t now_ms) noexcept { last_outbound_ms_ = now_ms; }


    // === Persistencja sequence numbers ===
    // Atomic write (tmp + fsync + rename) — pad procesu w trakcie zapisu nie
    // zostawia uszkodzonego pliku. Ten sam wzorzec co RiskManager::persist_state.
    void set_persist_path(const char* path) noexcept {
        if (path && *path) persist_path_ = path;
        else               persist_path_.clear();
    }

    // persist_seq: zapisz next_out i expected_in. Wywołuj po zmianie seq
    // (production: PRZED wysłaniem wiadomości — restart nie może reużyć numeru).
    void persist_seq() const noexcept {
        if (persist_path_.empty()) return;
        const std::string tmp = persist_path_ + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "out=%u\nin=%u\n", next_out_seq_, expected_in_seq_);
        std::fflush(f);
        ::fsync(::fileno(f));
        std::fclose(f);
        std::rename(tmp.c_str(), persist_path_.c_str());
    }

    // load_persisted_seq: wczytaj seq z pliku (PO set_persist_path, PRZED
    // przyjmowaniem/wysyłaniem). Zwraca true gdy wczytano.
    bool load_persisted_seq() noexcept {
        if (persist_path_.empty()) return false;
        FILE* f = std::fopen(persist_path_.c_str(), "r");
        if (!f) return false;
        unsigned out = 0, in = 0;
        const int n = std::fscanf(f, "out=%u\nin=%u\n", &out, &in);
        std::fclose(f);
        if (n < 2) return false;
        next_out_seq_    = out;
        expected_in_seq_ = in;
        return true;
    }


    // === Buildery admin messages (35=A/0/1/2/4/5) ===
    // Składają poprawną wiadomość FIX (z 8/9/10) do bufora out. Każde wołanie
    // KONSUMUJE outbound seq (tag 34) — bo każda wysłana wiadomość zżera numer.
    // delim '|' = human-readable (testy/logi), SOH = prawdziwy wire.
    // Zwracają długość lub 0 gdy bufor za mały.

    int build_logon(char* out, int cap, int hb_int_sec, char delim = FIXMessage::SOH) noexcept {
        char body[160];
        const int n = std::snprintf(body, sizeof(body),
            "35=A%c49=%s%c56=%s%c34=%u%c108=%d%c",
            delim, sender_comp_, delim, target_comp_, delim,
            next_outbound_seq(), delim, hb_int_sec, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    int build_heartbeat(char* out, int cap, const char* test_req_id = nullptr,
                        char delim = FIXMessage::SOH) noexcept {
        char body[160];
        int n;
        if (test_req_id && *test_req_id) {   // odpowiedź na TestRequest niesie 112
            n = std::snprintf(body, sizeof(body),
                "35=0%c49=%s%c56=%s%c34=%u%c112=%s%c",
                delim, sender_comp_, delim, target_comp_, delim,
                next_outbound_seq(), delim, test_req_id, delim);
        } else {
            n = std::snprintf(body, sizeof(body),
                "35=0%c49=%s%c56=%s%c34=%u%c",
                delim, sender_comp_, delim, target_comp_, delim,
                next_outbound_seq(), delim);
        }
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        ++heartbeats_sent_;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    int build_test_request(char* out, int cap, const char* test_req_id,
                           char delim = FIXMessage::SOH) noexcept {
        char body[160];
        const int n = std::snprintf(body, sizeof(body),
            "35=1%c49=%s%c56=%s%c34=%u%c112=%s%c",
            delim, sender_comp_, delim, target_comp_, delim,
            next_outbound_seq(), delim, test_req_id ? test_req_id : "TEST", delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // ResendRequest (35=2): "od BeginSeqNo (7) do EndSeqNo (16)". 0 w EndSeqNo
    // = "do najnowszej" (konwencja FIX). Wołaj po wykryciu GapDetected.
    int build_resend_request(char* out, int cap, uint32_t begin_seq, uint32_t end_seq,
                             char delim = FIXMessage::SOH) noexcept {
        char body[160];
        const int n = std::snprintf(body, sizeof(body),
            "35=2%c49=%s%c56=%s%c34=%u%c7=%u%c16=%u%c",
            delim, sender_comp_, delim, target_comp_, delim,
            next_outbound_seq(), delim, begin_seq, delim, end_seq, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        ++resends_requested_;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // SequenceReset (35=4): NewSeqNo (36) + GapFillFlag (123=Y/N). gap_fill=true
    // (GapFill mode) wypełnia lukę administracyjną bez replayu; false (Reset mode)
    // twardo przestawia oczekiwany numer.
    int build_sequence_reset(char* out, int cap, uint32_t new_seq_no, bool gap_fill,
                             char delim = FIXMessage::SOH) noexcept {
        char body[160];
        const int n = std::snprintf(body, sizeof(body),
            "35=4%c49=%s%c56=%s%c34=%u%c36=%u%c123=%c%c",
            delim, sender_comp_, delim, target_comp_, delim,
            next_outbound_seq(), delim, new_seq_no, delim, gap_fill ? 'Y' : 'N', delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    int build_logout(char* out, int cap, const char* text = nullptr,
                     char delim = FIXMessage::SOH) noexcept {
        char body[192];
        int n;
        if (text && *text) {
            n = std::snprintf(body, sizeof(body),
                "35=5%c49=%s%c56=%s%c34=%u%c58=%s%c",
                delim, sender_comp_, delim, target_comp_, delim,
                next_outbound_seq(), delim, text, delim);
        } else {
            n = std::snprintf(body, sizeof(body),
                "35=5%c49=%s%c56=%s%c34=%u%c",
                delim, sender_comp_, delim, target_comp_, delim,
                next_outbound_seq(), delim);
        }
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // === Buildery order-entry (35=D/F/G) — strona aplikacyjna (#90) ===
    // Admin messages (#78) utrzymuja sesje; te skladaja faktyczne zlecenia.
    // Mapuja 1:1 na OMS: D=submit, F=cancel, G=replace. Side: BUY→'1', SELL→'2'.

    // NewOrderSingle (35=D): 11=ClOrdID, 55=Symbol, 54=Side, 38=Qty, 44=Price,
    // 40=OrdType (2=Limit).
    int build_new_order(char* out, int cap, const char* cl_ord_id, const char* symbol,
                        Side side, int32_t qty, double price,
                        char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=D%c49=%s%c56=%s%c34=%u%c11=%s%c55=%s%c54=%c%c38=%d%c44=%.2f%c40=2%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            cl_ord_id, delim, symbol, delim, fix_side(side), delim, qty, delim, price, delim, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // OrderCancelRequest (35=F): 41=OrigClOrdID identyfikuje zlecenie do anulowania.
    int build_cancel_order(char* out, int cap, const char* cl_ord_id,
                           const char* orig_cl_ord_id, const char* symbol,
                           Side side, int32_t qty, char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=F%c49=%s%c56=%s%c34=%u%c11=%s%c41=%s%c55=%s%c54=%c%c38=%d%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            cl_ord_id, delim, orig_cl_ord_id, delim, symbol, delim, fix_side(side), delim, qty, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // OrderCancelReplaceRequest (35=G): amend ceny/ilosci (mapuje na OMS replace).
    int build_cancel_replace(char* out, int cap, const char* cl_ord_id,
                             const char* orig_cl_ord_id, const char* symbol,
                             Side side, int32_t qty, double price,
                             char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=G%c49=%s%c56=%s%c34=%u%c11=%s%c41=%s%c55=%s%c54=%c%c38=%d%c44=%.2f%c40=2%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            cl_ord_id, delim, orig_cl_ord_id, delim, symbol, delim, fix_side(side), delim,
            qty, delim, price, delim, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // build_execution_report (35=8) — raport giełda→klient domykajacy cykl FIX
    // (#101): po NewOrderSingle (D) acceptor odsyla ExecutionReport z ExecType
    // (150) i OrdStatus (39). Tu wariant FILL/PARTIAL z last/cum/leaves qty.
    //   exec_type/ord_status: '0'=New, '1'=Partial, '2'=Fill, '4'=Canceled, '8'=Rejected
    int build_exec_report(char* out, int cap, const char* cl_ord_id, const char* order_id,
                          const char* exec_id, char exec_type, char ord_status,
                          const char* symbol, Side side, int32_t last_qty, double last_px,
                          int32_t cum_qty, int32_t leaves_qty,
                          char delim = FIXMessage::SOH) noexcept {
        char body[320];
        const int n = std::snprintf(body, sizeof(body),
            "35=8%c49=%s%c56=%s%c34=%u%c11=%s%c37=%s%c17=%s%c150=%c%c39=%c%c"
            "55=%s%c54=%c%c32=%d%c31=%.2f%c14=%d%c151=%d%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            cl_ord_id, delim, order_id, delim, exec_id, delim, exec_type, delim, ord_status, delim,
            symbol, delim, fix_side(side), delim, last_qty, delim, last_px, delim,
            cum_qty, delim, leaves_qty, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // build_reject (35=3) — Session-level Reject (#126). Acceptor odsyla gdy
    // przychodzaca wiadomosc lamie regule sesji (brak tagu, zla wartosc, zly
    // typ) — np. po negatywnym validate_new_order. Rozni sie od business reject
    // (ExecutionReport 150=8): to odrzucenie na poziomie PROTOKOLU.
    //   45=RefSeqNum, 372=RefMsgType, 373=SessionRejectReason, 58=Text
    int build_reject(char* out, int cap, uint32_t ref_seq_num, const char* ref_msg_type,
                     int reject_reason, const char* text,
                     char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=3%c49=%s%c56=%s%c34=%u%c45=%u%c372=%s%c373=%d%c58=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            ref_seq_num, delim, ref_msg_type ? ref_msg_type : "", delim,
            reject_reason, delim, text ? text : "", delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // build_business_reject (35=j) — BusinessMessageReject (#133). Odrzucenie na
    // poziomie BIZNESOWYM (np. nieznany symbol, konto bez uprawnien, rynek
    // zamkniety) — wiadomosc byla POPRAWNA skladniowo, ale aplikacja jej nie
    // przyjmuje. Odrebne od session-level Reject 35=3 (#126: zlamana regula sesji).
    //   372=RefMsgType, 379=BusinessRejectRefID, 380=BusinessRejectReason, 58=Text
    int build_business_reject(char* out, int cap, const char* ref_msg_type,
                              const char* business_reject_ref_id, int reject_reason,
                              const char* text, char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=j%c49=%s%c56=%s%c34=%u%c372=%s%c379=%s%c380=%d%c58=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            ref_msg_type ? ref_msg_type : "", delim,
            business_reject_ref_id ? business_reject_ref_id : "", delim,
            reject_reason, delim, text ? text : "", delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // build_cancel_reject (35=9) — OrderCancelReject (#143). Gielda odrzuca
    // KONKRETNIE request Cancel (F) lub Replace (G) — np. za pozno (zlecenie juz
    // wypelnione), nieznane OrigClOrdID. Odrebne od session (35=3) / business
    // (35=j) reject. 434=CxlRejResponseTo ('1'=Cancel, '2'=Replace),
    // 102=CxlRejReason (0=too late, 1=unknown order, ...).
    int build_cancel_reject(char* out, int cap, const char* cl_ord_id,
                            const char* orig_cl_ord_id, const char* order_id,
                            char ord_status, char cxl_rej_response_to, int cxl_rej_reason,
                            const char* text, char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=9%c49=%s%c56=%s%c34=%u%c11=%s%c41=%s%c37=%s%c39=%c%c434=%c%c102=%d%c58=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            cl_ord_id, delim, orig_cl_ord_id ? orig_cl_ord_id : "", delim,
            order_id ? order_id : "", delim, ord_status, delim,
            cxl_rej_response_to, delim, cxl_rej_reason, delim, text ? text : "", delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // fix_side: Side → FIX tag 54 ('1'=Buy, '2'=Sell).
    static char fix_side(Side s) noexcept { return (s == Side::BUY) ? '1' : '2'; }

    // === Stats ===
    uint64_t gaps_detected()     const noexcept { return gaps_detected_; }
    uint64_t heartbeats_sent()   const noexcept { return heartbeats_sent_; }
    uint64_t resends_requested() const noexcept { return resends_requested_; }
    int32_t  heartbeat_interval_sec() const noexcept { return heartbeat_int_sec_; }
};


}  // namespace fix
