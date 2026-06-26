/*
 * FIXSession — the FIX session layer (Logon/Logout/Heartbeat/SeqNum tracking).
 *
 * Why separate from FIXMessage (the parser)? FIXMessage is "what ONE message means"
 * (parsing + CheckSum/BodyLength validation). FIXSession is "how the SESSION STATE
 * changes" — a multi-message protocol: who-to-whom, sequence in/out, heartbeats,
 * resend requests on gaps. Without it you can't connect to a real exchange.
 *
 * FIX session lifecycle:
 *
 *   1. Initiator (client): sends Logon (35=A) with HeartBtInt (108=30)
 *   2. Acceptor (broker/exch): replies with Logon — the session is UP
 *   3. Both sides: send application messages (D=NewOrder, 8=Execution...)
 *      Each with a monotonic MsgSeqNum (tag 34). First = 1, +1 per message.
 *   4. Each side: sends a Heartbeat (35=0) every HeartBtInt seconds when idle.
 *      No heartbeat for 2× HeartBtInt → the other side sends a TestRequest
 *      (35=1); another HeartBtInt with no reply → disconnect.
 *   5. When a received MsgSeqNum > expected → ResendRequest (35=2) "from X to Y".
 *      The other side resends the missing ones or a SequenceReset-GapFill (35=4) if
 *      the lost ones were administrative (not to be replayed).
 *   6. Logout (35=5) → graceful close.
 *
 * What we have here:
 *   - inbound seq tracking + gap detection
 *   - outbound seq counter (to inject into outgoing messages)
 *   - timer logic for heartbeats (the caller calls tick(now_ms) and asks
 *     whether to send a heartbeat / whether to disconnect)
 *   - session state flags: DISCONNECTED → LOGON_SENT → LOGGED_IN → LOGOUT
 *
 * What closes the session loop (expansion #78):
 *   - serialization of admin messages: build_logon/heartbeat/test_request/
 *     resend_request/sequence_reset/logout — generate correct FIX wire
 *     (with 8/9/10) and inject the outbound MsgSeqNum (tag 34)
 *   - sequence-number persistence: persist_seq/load_persisted_seq (atomic
 *     write like RiskManager) — a process restart won't reuse an already-sent seq
 *
 * What we STILL do NOT do (out of scope): actually sending over TCP (a higher
 * layer, e.g. network/epoll_server) and full store-and-forward replay of missed
 * application messages (build_sequence_reset GapFill covers the admin side).
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
    DISCONNECTED = 0,   // before Logon or after Logout
    LOGON_SENT   = 1,   // we sent Logon, waiting for the reply
    LOGGED_IN    = 2,   // session fully up — exchanging application messages
    LOGOUT       = 3,   // one of the sides sent Logout
};


struct GapDetected {
    uint32_t expected;   // the number we expected
    uint32_t received;   // the number that arrived (> expected)
    bool     valid;      // true when a gap really exists
};


class FIXSession {
    SessionState state_ = SessionState::DISCONNECTED;

    uint32_t expected_in_seq_  = 1;   // next expected MsgSeqNum (inbound)
    uint32_t next_out_seq_     = 1;   // next to send in an outgoing message
    int32_t  heartbeat_int_sec_ = 30; // negotiated in Logon (tag 108)
    int64_t  last_inbound_ms_  = 0;
    int64_t  last_outbound_ms_ = 0;
    bool     test_request_pending_ = false;

    // Session identity (tag 49/56) — inserted into outgoing admin messages.
    char sender_comp_[32] = "SENDER";
    char target_comp_[32] = "TARGET";
    char pending_test_req_id_[32] = {0};   // non-empty = a TestRequest was received (#158)

    // Sequence-number persistence. Empty = disabled (backward-compatible
    // behavior). Production MUST have it — a process restart cannot
    // send an already-used MsgSeqNum or ask for a resend from the wrong place.
    std::string persist_path_;

    // Statistics
    uint64_t gaps_detected_   = 0;
    uint64_t heartbeats_sent_ = 0;
    uint64_t resends_requested_ = 0;

public:
    FIXSession() noexcept = default;

    // set_comp_ids: set the SenderCompID (tag 49) and TargetCompID (tag 56)
    // inserted into all built admin messages.
    void set_comp_ids(const char* sender, const char* target) noexcept {
        if (sender) { std::strncpy(sender_comp_, sender, sizeof(sender_comp_) - 1);
                      sender_comp_[sizeof(sender_comp_) - 1] = '\0'; }
        if (target) { std::strncpy(target_comp_, target, sizeof(target_comp_) - 1);
                      target_comp_[sizeof(target_comp_) - 1] = '\0'; }
    }

    // === State ===
    SessionState state()      const noexcept { return state_; }
    bool         is_logged_in() const noexcept { return state_ == SessionState::LOGGED_IN; }

    // === Outbound sequence ===
    // Call BEFORE building each outgoing message — returns the number
    // to insert in tag 34, atomically increments the internal counter.
    uint32_t next_outbound_seq() noexcept { return next_out_seq_++; }
    uint32_t peek_outbound_seq() const noexcept { return next_out_seq_; }

    // === Inbound sequence + gap detection ===
    // observe_inbound: call on every incoming APPLICATION message
    // with the parsed tag 34. Returns GapDetected (valid=true on a gap), updates
    // expected_in_seq_. A duplicate (seq < expected) returns valid=false (the caller
    // can ignore it).
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
            expected_in_seq_ = msg_seq_num + 1;  // we accept the message but note the gap
            return g;
        }
        // msg_seq_num < expected — a duplicate / late, ignore.
        return g;
    }

    // mark_resend_requested: after detecting GapDetected the caller sends a ResendRequest
    // (35=2) and calls this to update stats.
    void mark_resend_requested() noexcept { ++resends_requested_; }

    // expected_inbound_seq: the next expected inbound MsgSeqNum (tag 34).
    uint32_t expected_inbound_seq() const noexcept { return expected_in_seq_; }

    // apply_inbound_sequence_reset: after receiving a SequenceReset (35=4) with NewSeqNo
    // (tag 36) set the expected inbound seq — the other side skips numbering
    // (GapFill of administrative ones or a hard Reset). Closes recovery on the
    // inbound side (paired with build_sequence_reset that sends it). Ignores a
    // backward number (< expected) — SequenceReset does not roll numbering back in GapFill mode.
    void apply_inbound_sequence_reset(uint32_t new_seq_no) noexcept {
        if (new_seq_no >= expected_in_seq_) expected_in_seq_ = new_seq_no;
    }

    // process_inbound (#150): a unified dispatcher for an incoming message —
    // ties the parser (FIXMessage) to the session. SequenceReset (35=4) -> reset the expected
    // seq (tag 36). Others: observe MsgSeqNum (34) -> detect a gap; then a side-effect
    // by type: Logon (A) -> LOGGED_IN + HeartBtInt (108), Logout (5) -> LOGOUT.
    // Returns GapDetected (valid=true -> the caller sends a ResendRequest).
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
            // The counterparty asks for proof of life — remember the TestReqID, the caller
            // replies with a Heartbeat (35=0) carrying the same 112.
            if (const char* tri = m.get_field(112)) {
                std::strncpy(pending_test_req_id_, tri, sizeof(pending_test_req_id_) - 1);
                pending_test_req_id_[sizeof(pending_test_req_id_) - 1] = '\0';
            }
        }
        return gap;
    }

    // pending_test_req_id: non-empty -> a TestRequest arrived, reply with
    // build_heartbeat(pending_test_req_id()) and call clear_pending_test_req (#158).
    const char* pending_test_req_id() const noexcept { return pending_test_req_id_; }
    void clear_pending_test_req() noexcept { pending_test_req_id_[0] = '\0'; }

    // === Transitions ===
    // mark_logon_sent: call after sending Logon (35=A). State → LOGON_SENT.
    void mark_logon_sent(int64_t now_ms) noexcept {
        state_           = SessionState::LOGON_SENT;
        last_outbound_ms_ = now_ms;
    }

    // mark_logon_received: call when a Logon was received from the counterparty.
    // hb_int_sec is the value from tag 108. State → LOGGED_IN.
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
    // Action — what the caller should do after calling tick().
    enum class Action : uint8_t {
        NONE,                  // nothing
        SEND_HEARTBEAT,        // send 35=0 (idle too long on our side)
        SEND_TEST_REQUEST,     // send 35=1 (silence from the counterparty)
        DISCONNECT,            // no reaction to the test request — drop
    };

    // tick: call every second or from the main loop. now_ms = monotonic ms.
    // Returns the action to take (NONE in 99% of calls).
    Action tick(int64_t now_ms) noexcept {
        if (state_ != SessionState::LOGGED_IN) return Action::NONE;

        const int64_t hb_ms = static_cast<int64_t>(heartbeat_int_sec_) * 1000;

        // 1. Silence from the counterparty > 2×HeartBtInt — send a TestRequest.
        //    If we already sent one and there's still silence → disconnect.
        if (now_ms - last_inbound_ms_ > 2 * hb_ms) {
            if (test_request_pending_) return Action::DISCONNECT;
            test_request_pending_ = true;
            return Action::SEND_TEST_REQUEST;
        }

        // 2. We were silent too long (HeartBtInt) — send a Heartbeat.
        if (now_ms - last_outbound_ms_ > hb_ms) {
            ++heartbeats_sent_;
            last_outbound_ms_ = now_ms;
            return Action::SEND_HEARTBEAT;
        }

        return Action::NONE;
    }

    // Call after every SENT message to reset our idle timer.
    void mark_outbound(int64_t now_ms) noexcept { last_outbound_ms_ = now_ms; }


    // === Sequence-number persistence ===
    // Atomic write (tmp + fsync + rename) — a process crash mid-write does not
    // leave a corrupted file. The same pattern as RiskManager::persist_state.
    void set_persist_path(const char* path) noexcept {
        if (path && *path) persist_path_ = path;
        else               persist_path_.clear();
    }

    // persist_seq: save next_out and expected_in. Call after a seq change
    // (production: BEFORE sending a message — a restart must not reuse a number).
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

    // load_persisted_seq: load seq from the file (AFTER set_persist_path, BEFORE
    // accepting/sending). Returns true when loaded.
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


    // === Admin message builders (35=A/0/1/2/4/5) ===
    // They assemble a correct FIX message (with 8/9/10) into the buffer out. Each call
    // CONSUMES an outbound seq (tag 34) — because every sent message eats a number.
    // delim '|' = human-readable (tests/logs), SOH = the real wire.
    // They return the length or 0 if the buffer is too small.

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
        if (test_req_id && *test_req_id) {   // a reply to a TestRequest carries 112
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

    // ResendRequest (35=2): "from BeginSeqNo (7) to EndSeqNo (16)". 0 in EndSeqNo
    // = "up to the latest" (FIX convention). Call after detecting GapDetected.
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
    // (GapFill mode) fills an administrative gap without replay; false (Reset mode)
    // hard-sets the expected number.
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

    // === Order-entry builders (35=D/F/G) — the application side (#90) ===
    // Admin messages (#78) keep the session alive; these assemble actual orders.
    // They map 1:1 to OMS: D=submit, F=cancel, G=replace. Side: BUY→'1', SELL→'2'.

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

    // OrderCancelRequest (35=F): 41=OrigClOrdID identifies the order to cancel.
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

    // OrderCancelReplaceRequest (35=G): amend price/quantity (maps to OMS replace).
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

    // OrderStatusRequest (35=H): the client asks for the current status of a working order
    // (#185). Used for RECONCILIATION after a reconnect / sequence gap — the client
    // does not know whether fills arrived during the disconnect. 11=ClOrdID + 55=Symbol +
    // 54=Side. The exchange replies with an ExecutionReport (35=8) with the current OrdStatus.
    int build_order_status_request(char* out, int cap, const char* cl_ord_id,
                                   const char* symbol, Side side,
                                   char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=H%c49=%s%c56=%s%c34=%u%c11=%s%c55=%s%c54=%c%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            cl_ord_id, delim, symbol, delim, fix_side(side), delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // OrderMassCancelRequest (35=q): mass-cancel working orders (#193) —
    // a risk/ops panic button. request_type: '1' = by symbol (requires symbol),
    // '7' = ALL of the firm's orders. 530=MassCancelRequestType. The exchange replies with
    // an OrderMassCancelReport (35=r). symbol is ignored for type '7'.
    int build_mass_cancel(char* out, int cap, const char* cl_ord_id, char request_type,
                          const char* symbol = nullptr, char delim = FIXMessage::SOH) noexcept {
        char body[256];
        int n;
        if (request_type == '1' && symbol) {
            n = std::snprintf(body, sizeof(body),
                "35=q%c49=%s%c56=%s%c34=%u%c11=%s%c530=1%c55=%s%c",
                delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
                cl_ord_id, delim, delim, symbol, delim);
        } else {
            n = std::snprintf(body, sizeof(body),
                "35=q%c49=%s%c56=%s%c34=%u%c11=%s%c530=7%c",
                delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
                cl_ord_id, delim, delim);
        }
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // OrderMassCancelReport (35=r): the exchange's reply to OrderMassCancelRequest
    // (35=q, #193) — closes the mass-cancel loop (#201). response =
    // 531=MassCancelResponse ('0'=rejected, '1'=by symbol, '7'=all);
    // affected = 533=TotalAffectedOrders (how many orders were actually cancelled).
    int build_mass_cancel_report(char* out, int cap, const char* cl_ord_id,
                                 char response, int32_t affected,
                                 char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=r%c49=%s%c56=%s%c34=%u%c11=%s%c531=%c%c533=%d%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            cl_ord_id, delim, response, delim, affected, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // MarketDataRequest (35=V): subscribe to market data for a symbol (#209).
    // 263=SubscriptionRequestType ('0'=snapshot, '1'=snapshot+updates, '2'=
    // unsubscribe), 264=MarketDepth (0=full book, 1=top-of-book), 146=1 sym +
    // 55. The client sends it at session start to receive quotes for that instrument.
    int build_market_data_request(char* out, int cap, const char* md_req_id,
                                  char sub_type, int depth, const char* symbol,
                                  char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=V%c49=%s%c56=%s%c34=%u%c262=%s%c263=%c%c264=%d%c146=1%c55=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            md_req_id, delim, sub_type, delim, depth, delim, delim, symbol, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // MarketDataSnapshotFullRefresh (35=W): a reply to MarketDataRequest (35=V,
    // #209) — a full top-of-book snapshot (#217). 262=MDReqID, 55=Symbol, 268=
    // NoMDEntries (here 2), then a repeating group: 269=MDEntryType ('0'=bid,
    // '1'=offer), 270=MDEntryPx, 271=MDEntrySize. Closes the market-data loop.
    int build_md_snapshot(char* out, int cap, const char* md_req_id, const char* symbol,
                          double bid_px, int32_t bid_sz, double ask_px, int32_t ask_sz,
                          char delim = FIXMessage::SOH) noexcept {
        char body[320];
        const int n = std::snprintf(body, sizeof(body),
            "35=W%c49=%s%c56=%s%c34=%u%c262=%s%c55=%s%c268=2%c"
            "269=0%c270=%.2f%c271=%d%c269=1%c270=%.2f%c271=%d%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            md_req_id, delim, symbol, delim, delim,
            delim, bid_px, delim, bid_sz, delim, delim, ask_px, delim, ask_sz, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // MarketDataIncrementalRefresh (35=X): an incremental update after the snapshot
    // (35=W, #217) — the exchange sends ONLY changes (#225), not the whole book. 262=
    // MDReqID, 268=1, 279=MDUpdateAction ('0'=new, '1'=change, '2'=delete), 269=
    // MDEntryType ('0'=bid/'1'=offer), 55, 270=Px, 271=Size. A stream after subscription.
    int build_md_incremental(char* out, int cap, const char* md_req_id, char update_action,
                             char entry_type, const char* symbol, double px, int32_t sz,
                             char delim = FIXMessage::SOH) noexcept {
        char body[320];
        const int n = std::snprintf(body, sizeof(body),
            "35=X%c49=%s%c56=%s%c34=%u%c262=%s%c268=1%c279=%c%c269=%c%c55=%s%c270=%.2f%c271=%d%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            md_req_id, delim, delim, update_action, delim, entry_type, delim, symbol, delim,
            px, delim, sz, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // MarketDataRequestReject (35=Y): the exchange REJECTS a MarketDataRequest (35=V,
    // #209) (#233) — e.g. unknown symbol, no permission, unsupported depth.
    // 262=MDReqID (echo), 281=MDReqRejReason ('0'=unknown symbol, '1'=duplicate,
    // '4'=unsupported depth, '5'=unsupported sub type). Closes the V -> Y handshake.
    int build_md_request_reject(char* out, int cap, const char* md_req_id, char reason,
                                char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=Y%c49=%s%c56=%s%c34=%u%c262=%s%c281=%c%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            md_req_id, delim, reason, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // Quote (35=S): a two-sided quote from a market maker (#249). 117=QuoteID,
    // 55=Symbol, 132=BidPx, 133=OfferPx, 134=BidSize, 135=OfferSize. Quote-driven
    // markets (vs order-driven): the MM streams quotes the venue can hit, instead
    // of resting individual orders.
    int build_quote(char* out, int cap, const char* quote_id, const char* symbol,
                    double bid_px, double offer_px, int32_t bid_size, int32_t offer_size,
                    char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=S%c49=%s%c56=%s%c34=%u%c117=%s%c55=%s%c132=%.2f%c133=%.2f%c134=%d%c135=%d%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            quote_id, delim, symbol, delim, bid_px, delim, offer_px, delim,
            bid_size, delim, offer_size, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // QuoteRequest (35=R): a client asks for a quote on a symbol (RFQ markets)
    // (#256). 131=QuoteReqID, 146=1 sym + 55=Symbol. The market maker responds with
    // a Quote (35=S, #249) — this is the request side of the RFQ flow.
    int build_quote_request(char* out, int cap, const char* quote_req_id, const char* symbol,
                            char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=R%c49=%s%c56=%s%c34=%u%c131=%s%c146=1%c55=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            quote_req_id, delim, delim, symbol, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // QuoteCancel (35=Z): a market maker pulls quote(s) (#271). 117=QuoteID,
    // 298=QuoteCancelType ('1'=cancel for one symbol, '4'=cancel all quotes),
    // 55=Symbol. Completes the quote lifecycle: 35=R request -> 35=S quote -> 35=Z
    // cancel. MMs pull quotes on adverse news or when inventory limits are hit.
    int build_quote_cancel(char* out, int cap, const char* quote_id, char cancel_type,
                           const char* symbol, char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=Z%c49=%s%c56=%s%c34=%u%c117=%s%c298=%c%c55=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            quote_id, delim, cancel_type, delim, symbol, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // MassQuote (35=i): a market maker streams a SET of quotes in one message
    // (#279) — efficient two-sided quoting across many symbols at once. 117=QuoteID,
    // 295=NoQuoteEntries, then a repeating group per symbol: 299=QuoteEntryID,
    // 55=Symbol, 132=BidPx, 133=OfferPx. Here a 2-symbol set; read it back with the
    // repeating-group accessors (#263). Used by MMs to refresh a whole book at once.
    int build_mass_quote(char* out, int cap, const char* quote_id,
                         const char* sym1, double bid1, double ask1,
                         const char* sym2, double bid2, double ask2,
                         char delim = FIXMessage::SOH) noexcept {
        char body[320];
        const int n = std::snprintf(body, sizeof(body),
            "35=i%c49=%s%c56=%s%c34=%u%c117=%s%c296=1%c295=2%c"
            "299=1%c55=%s%c132=%.2f%c133=%.2f%c"
            "299=2%c55=%s%c132=%.2f%c133=%.2f%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            quote_id, delim, delim, delim,
            delim, sym1, delim, bid1, delim, ask1, delim,
            delim, sym2, delim, bid2, delim, ask2, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // TradeCaptureReport (35=AE): a post-trade record for clearing / regulatory
    // reporting (#287). 571=TradeReportID, 55=Symbol, 54=Side, 32=LastQty,
    // 31=LastPx, 75=TradeDate. Used to report or confirm executed trades to the
    // clearing/post-trade layer, independent of the order-entry ExecutionReport.
    int build_trade_capture_report(char* out, int cap, const char* trade_report_id,
                                   const char* symbol, Side side, int32_t qty, double px,
                                   const char* trade_date, char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=AE%c49=%s%c56=%s%c34=%u%c571=%s%c55=%s%c54=%c%c32=%d%c31=%.2f%c75=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            trade_report_id, delim, symbol, delim, fix_side(side), delim, qty, delim, px, delim,
            trade_date, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // BusinessMessageReject (35=j): reject an application message that passed session
    // checks but can't be processed at the business layer (#295) — e.g. unknown
    // security, unsupported request type, a conditionally-required field missing.
    // 45=RefSeqNum, 372=RefMsgType, 380=BusinessRejectReason, 58=Text. Distinct from
    // the session-level Reject (35=3) which handles malformed/out-of-sequence frames.
    int build_business_reject(char* out, int cap, uint32_t ref_seq, const char* ref_msg_type,
                              int reject_reason, const char* text,
                              char delim = FIXMessage::SOH) noexcept {
        char body[256];
        const int n = std::snprintf(body, sizeof(body),
            "35=j%c49=%s%c56=%s%c34=%u%c45=%u%c372=%s%c380=%d%c58=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            ref_seq, delim, ref_msg_type, delim, reject_reason, delim, text, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // TradingSessionStatus (35=h): the exchange broadcasts the trading session phase
    // (#303) — 336=TradingSessionID, 340=TradSesStatus (1=Halted, 2=Open, 3=Closed,
    // 4=PreOpen, 5=PreClose). Lets a trading app gate order flow on the venue's state
    // (e.g. stop quoting during a halt). Counterpart to TradingSessionStatusRequest
    // (35=g) the client would send to ask for it.
    int build_trading_session_status(char* out, int cap, const char* session_id,
                                     int status, char delim = FIXMessage::SOH) noexcept {
        char body[160];
        const int n = std::snprintf(body, sizeof(body),
            "35=h%c49=%s%c56=%s%c34=%u%c336=%s%c340=%d%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            session_id, delim, status, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // OrderMassStatusRequest (35=AF): ask the exchange for the status of MANY orders
    // at once (#311) — 584=MassStatusReqID, 585=MassStatusReqType (1=AllOrders,
    // 7=Status for a symbol), 55=Symbol when scoped. The exchange replies with an
    // ExecutionReport per matching order. Bulk reconciliation after a gap / reconnect,
    // vs the single-order OrderStatusRequest (35=H, #185).
    int build_mass_status_request(char* out, int cap, const char* req_id, int req_type,
                                  const char* symbol, char delim = FIXMessage::SOH) noexcept {
        char body[200];
        const int n = std::snprintf(body, sizeof(body),
            "35=AF%c49=%s%c56=%s%c34=%u%c584=%s%c585=%d%c55=%s%c",
            delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
            req_id, delim, req_type, delim, symbol, delim);
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // build_execution_report (35=8) — an exchange→client report closing the FIX cycle
    // (#101): after a NewOrderSingle (D) the acceptor sends back an ExecutionReport with ExecType
    // (150) and OrdStatus (39). Here a FILL/PARTIAL variant with last/cum/leaves qty.
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

    // ExecReport — the key ExecutionReport (35=8) fields in a typed struct (#241).
    struct ExecReport {
        char    ord_status = '\0';   // 39
        char    exec_type  = '\0';   // 150
        int32_t last_qty   = 0;      // 32
        double  last_px    = 0.0;    // 31
        int32_t cum_qty    = 0;      // 14
        int32_t leaves_qty = 0;      // 151
        bool    valid      = false;  // true when the message really is 35=8
    };

    // parse_exec_report: extract ExecReport from a parsed message (#241). The client
    // consumes execution reports without manually digging through tags. valid=false when
    // it is not 35=8. Symmetric to build_exec_report (#101) — closes the round-trip.
    static ExecReport parse_exec_report(const FIXMessage& m) noexcept {
        ExecReport r;
        if (m.get_msg_type()[0] != '8') return r;     // valid stays false
        const char* os = m.get_field(39);
        const char* et = m.get_field(150);
        r.ord_status = os ? os[0] : '\0';
        r.exec_type  = et ? et[0] : '\0';
        r.last_qty   = m.get_int(32);
        r.last_px    = m.get_double(31);
        r.cum_qty    = m.get_int(14);
        r.leaves_qty = m.get_int(151);
        r.valid      = true;
        return r;
    }

    // build_reject (35=3) — a Session-level Reject (#126). The acceptor sends it when
    // an incoming message breaks a session rule (missing tag, bad value, wrong
    // type) — e.g. after a negative validate_new_order. Differs from a business reject
    // (ExecutionReport 150=8): this is a PROTOCOL-level rejection.
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

    // build_business_reject (35=j) — BusinessMessageReject (#133). A rejection at the
    // BUSINESS level (e.g. unknown symbol, an account without permission, a closed
    // market) — the message was syntactically CORRECT, but the application does not
    // accept it. Separate from the session-level Reject 35=3 (#126: a broken session rule).
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

    // build_cancel_reject (35=9) — OrderCancelReject (#143). The exchange rejects
    // SPECIFICALLY a Cancel (F) or Replace (G) request — e.g. too late (the order is already
    // filled), unknown OrigClOrdID. Separate from session (35=3) / business
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

    // QuoteStatusReport (35=AI): the venue's feedback on a Quote (35=S, #249) —
    // whether the quote was accepted, rejected, or pulled from the market (#319).
    // Closes the RFQ lifecycle:
    //   35=R (QuoteRequest, #256) → 35=S (Quote, #249)
    //   → 35=AI (QuoteStatusReport, here) → optional 35=Z (QuoteCancel, #271).
    //
    // 297=QuoteStatus:
    //   0 = Accepted, 4 = Rejected, 5 = RemovedFromMarket,
    //   8 = Pending,  14 = Active   (FIX 4.2 subset)
    // 58=Text: optional human-readable reason for non-0 status.
    int build_quote_status_report(char* out, int cap, const char* quote_id,
                                  const char* symbol, int quote_status,
                                  const char* text = nullptr,
                                  char delim = FIXMessage::SOH) noexcept {
        char body[256];
        int n;
        if (text && *text) {
            n = std::snprintf(body, sizeof(body),
                "35=AI%c49=%s%c56=%s%c34=%u%c117=%s%c55=%s%c297=%d%c58=%s%c",
                delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
                quote_id, delim, symbol, delim, quote_status, delim, text, delim);
        } else {
            n = std::snprintf(body, sizeof(body),
                "35=AI%c49=%s%c56=%s%c34=%u%c117=%s%c55=%s%c297=%d%c",
                delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
                quote_id, delim, symbol, delim, quote_status, delim);
        }
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // QuoteStatusReport — typed view of a parsed 35=AI message (#319).
    // Symmetric to ExecReport (#241): parse once, access by field name.
    struct QuoteStatusReport {
        int  status = -1;   // 297=QuoteStatus (-1 = absent)
        bool valid  = false; // true when msg type == "AI"
    };

    static QuoteStatusReport parse_quote_status(const FIXMessage& m) noexcept {
        QuoteStatusReport r;
        if (std::strcmp(m.get_msg_type(), "AI") != 0) return r;
        const char* s = m.get_field(297);
        r.status = s ? std::atoi(s) : -1;
        r.valid  = true;
        return r;
    }

    // DontKnowTrade (35=Q): the client repudiates an ExecutionReport (35=8, #101) it
    // cannot reconcile — an execution for an order it has no record of (#327). Instead
    // of silently dropping a mystery fill (which would desync the position), the client
    // formally "DK"s it back to the venue, citing why. The defensive counterpart to the
    // exchange→client ExecReport: it closes the loop on a desync rather than ignoring it.
    //
    // 127=DKReason (char):
    //   A = Unknown symbol, B = Wrong side, C = Quantity exceeds order,
    //   D = No matching order, E = Price exceeds limit, F = Calculation difference,
    //   Z = Other.
    // 58=Text: optional human-readable detail.
    int build_dont_know_trade(char* out, int cap, const char* order_id, const char* exec_id,
                              char dk_reason, const char* symbol, char side,
                              uint32_t order_qty, const char* text = nullptr,
                              char delim = FIXMessage::SOH) noexcept {
        char body[256];
        int n;
        if (text && *text) {
            n = std::snprintf(body, sizeof(body),
                "35=Q%c49=%s%c56=%s%c34=%u%c37=%s%c17=%s%c127=%c%c55=%s%c54=%c%c38=%u%c58=%s%c",
                delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
                order_id, delim, exec_id, delim, dk_reason, delim, symbol, delim, side, delim,
                order_qty, delim, text, delim);
        } else {
            n = std::snprintf(body, sizeof(body),
                "35=Q%c49=%s%c56=%s%c34=%u%c37=%s%c17=%s%c127=%c%c55=%s%c54=%c%c38=%u%c",
                delim, sender_comp_, delim, target_comp_, delim, next_outbound_seq(), delim,
                order_id, delim, exec_id, delim, dk_reason, delim, symbol, delim, side, delim,
                order_qty, delim);
        }
        if (n < 0 || n >= (int)sizeof(body)) return 0;
        return FIXMessage::build_message(out, cap, body, "FIX.4.2", delim);
    }

    // DontKnowTrade — typed view of a parsed 35=Q message (#327). Symmetric to
    // ExecReport (#241) / QuoteStatusReport (#319): parse once, access by field name.
    struct DontKnowTrade {
        char dk_reason = '\0';   // 127=DKReason ('\0' = absent)
        bool valid     = false;  // true when msg type == "Q"
    };

    static DontKnowTrade parse_dont_know_trade(const FIXMessage& m) noexcept {
        DontKnowTrade r;
        if (std::strcmp(m.get_msg_type(), "Q") != 0) return r;
        const char* s = m.get_field(127);
        r.dk_reason = (s && *s) ? s[0] : '\0';
        r.valid     = true;
        return r;
    }

    // TradeCaptureReport — typed view of a parsed 35=AE message (#336). Closes the
    // round-trip with build_trade_capture_report (#287), symmetric to ExecReport
    // (#241) / QuoteStatusReport (#319) / DontKnowTrade (#327): parse once, read by
    // field. The post-trade / clearing layer consumes capture reports without
    // digging through tags. valid=false when the message is not 35=AE.
    struct TradeCaptureReport {
        char    trade_report_id[32] = {};   // 571=TradeReportID
        char    symbol[16]          = {};   // 55=Symbol
        char    side                = '\0'; // 54=Side ('1'=Buy, '2'=Sell)
        int32_t last_qty            = 0;    // 32=LastQty
        double  last_px             = 0.0;  // 31=LastPx
        char    trade_date[16]      = {};   // 75=TradeDate (YYYYMMDD)
        bool    valid               = false;// true when msg type == "AE"
    };

    static TradeCaptureReport parse_trade_capture_report(const FIXMessage& m) noexcept {
        TradeCaptureReport r;
        if (std::strcmp(m.get_msg_type(), "AE") != 0) return r;   // valid stays false
        const char* tri = m.get_field(571);
        const char* sym = m.get_field(55);
        const char* sd  = m.get_field(54);
        const char* td  = m.get_field(75);
        // sources are runtime pointers (no compile-time length) so strncpy bounded
        // to size-1 over a value-initialized array stays nul-terminated.
        if (tri) std::strncpy(r.trade_report_id, tri, sizeof(r.trade_report_id) - 1);
        if (sym) std::strncpy(r.symbol,          sym, sizeof(r.symbol) - 1);
        r.side     = (sd && *sd) ? sd[0] : '\0';
        r.last_qty = m.get_int(32);
        r.last_px  = m.get_double(31);
        if (td)  std::strncpy(r.trade_date,      td,  sizeof(r.trade_date) - 1);
        r.valid    = true;
        return r;
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
