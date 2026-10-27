/* Option A M2 (OPTA-M2-SPEC.md D-A = T2, sc6rb/M2-INTERFACES.md §4): the rbonline side channel over the game's own Steam P2P.
 * FROZEN interface (the interface pass, v0.29.0): the transport layer implements src/p2p.c and may APPEND declarations at the end.
 *
 * What p2p.c is (and is not):
 *   - It owns ONE I/O thread. Every Steam call happens on that thread; the game thread only pushes to / pops from lock-free queues
 *     (p2p_send / p2p_poll / p2p_open / p2p_close never block, never call Steam, never allocate after p2p_init).
 *   - Steam is reached only through a P2pApi function table. In the plugin build p2p.c fills it from the steam_api64.dll the game
 *     already loaded (GetModuleHandleA + GetProcAddress, NEVER LoadLibrary; PE timestamp 0x585894b1 = SDK 1.39 checked first).
 *     In the harness the table comes from test/fake_steam.c through p2p_set_api(): no test ever touches the real DLL.
 *   - Unreliable sends only (k_EP2PSendUnreliable = 0), on our own channel (> 65535, configurable), our own sequence numbers,
 *     acks and resends (src/rbomsg.h). Accepts datagrams only from the expected peer SteamID. Never logs a SteamID, name or address.
 *   - It never touches game memory and never calls game code. It knows nothing about rounds or frames: the core (src/rbo.c) runs the
 *     handshake / ARM / CHECK logic; p2p.c delivers each sequenced message exactly once (in any order) or reports the peer silent.
 *
 * Steam SDK 1.39 flat API, VERIFIED read-only on the game's steam_api64.dll (timestamp 0x585894b1; M2-INTERFACES.md §4.1):
 *   SteamAPI_ISteamNetworking_SendP2PPacket       rva 0x4ea0  -> ISteamNetworking vtbl+0x00 (this, CSteamID, data, len, EP2PSend, channel)
 *   SteamAPI_ISteamNetworking_IsP2PPacketAvailable rva 0x5390 -> vtbl+0x08 (this, uint32 *size, channel)
 *   SteamAPI_ISteamNetworking_ReadP2PPacket       rva 0x4e60  -> vtbl+0x10 (this, dst, cap, uint32 *size, CSteamID *remote, channel)
 *   SteamAPI_ISteamClient_GetISteamNetworking     rva 0x4150  -> ISteamClient vtbl+0x80 (this, HSteamUser, HSteamPipe, "SteamNetworking005")
 *   SteamClient rva 0x8c70 (returns the ISteamClient* global), SteamAPI_GetHSteamUser rva 0x7ea0, SteamAPI_GetHSteamPipe rva 0x7eb0,
 *   SteamInternal_CreateInterface rva 0x8cc0 (alternative to SteamClient: "SteamClient017").
 *   v0.29.7: SteamAPI_ISteamNetworking_GetP2PSessionState rva 0x4560 -> vtbl+0x30 (this, CSteamID, P2PSessionState_t *), bool in al
 *   (`sub rsp,28h; mov rax,[rcx]; call [rax+30h]`: every argument forwarded; VERIFIED read-only 2026-10-21, pefile + capstone).
 *   Every bool return is one byte (al): the table types it uint8_t. CSteamID by value = its uint64 in a register (H strong: the flat
 *   wrapper is a pure pass-through, so this is the C++ virtual's ABI; an 8-byte trivially-copyable class goes in rdx). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rbomsg.h"

#define P2P_CHANNEL_DEFAULT 0x52424F4E   /* 1,380,076,366: above the 0..65535 range UE maps ports to channels; != HorseMod's 0x484F */
#define P2P_STEAM_DLL_TS    0x585894b1u  /* the game's steam_api64.dll (Steamv139); any other timestamp -> api=none why=dll_ts */
#define P2P_SEND_UNRELIABLE 0            /* k_EP2PSendUnreliable */

typedef struct {
    /* the four calls p2p.c makes; `net` is what networking() returned (an ISteamNetworking*, or the fake's context) */
    void   *(*networking)(void);                                                    /* NULL = Steam not up (yet) */
    uint8_t (*send)(void *net, uint64_t steamid, const void *data, uint32_t len, int send_type, int channel);
    uint8_t (*avail)(void *net, uint32_t *size, int channel);
    uint8_t (*read)(void *net, void *dst, uint32_t cap, uint32_t *size, uint64_t *remote_steamid, int channel);
    const char *name;                                                                /* "steam" | "fake" (RB P2P api=) */
    /* v0.29.1 (review M1 / m3, appended): */
    uint64_t (*local_id)(void *net);                                                 /* this PC's own SteamID (0 = unknown: the open fails closed) */
    uint8_t (*close_channel)(void *net, uint64_t steamid, int channel);              /* CloseP2PChannelWithUser (NULL = not available) */
    /* v0.29.7 (OPTA-M2-SPEC.md §18.10, appended): the old ISteamNetworking's GetP2PSessionState(CSteamID, P2PSessionState_t *): 1 = Steam has a
     * P2P session with that user (the call returned true); *active / *connecting = the struct's m_bConnectionActive / m_bConnecting bytes.
     * Nothing else of the struct leaves p2p.c (it holds the remote IP: never read, never logged). NULL = not available: no candidate validates. */
    uint8_t (*session_state)(void *net, uint64_t steamid, uint8_t *active, uint8_t *connecting);
} P2pApi;

typedef struct {
    int channel;            /* rbonline_channel (> 65535, != 0x484F; the core refuses anything else before calling p2p_init) */
    int probe_max;          /* datagrams to an UNVERIFIED peer per battle (rbonline_probe_max, default 10) */
    int probe_gap_ms;       /* minimum spacing of those (rbonline_probe_gap_ms, default 500) */
    int resend_ms;          /* resend period of an unacked sequenced message once the peer is verified (rbonline_resend_ms, default 250 = 4/s) */
    int peer_timeout_ms;    /* verified peer silent this long -> P2P_EV_SILENT (rbonline_peer_timeout_ms, default 3000) */
    int poll_ms;            /* I/O thread period (rbonline_poll_ms, default 5) */
    /* harness-only plants (0 in the plugin build; the core fills them from the h_p2p_* keys under SC6RB_HARNESS) */
    int h_accept_any_id;    /* h_p2p_accept_any_id: control, the peer-id filter is off */
    int h_no_guard;         /* h_p2p_no_guard: control, the foreign-packet guard at p2p_open is off */
    int h_no_resend;        /* h_p2p_no_resend: control, every sequenced message is sent once */
    int h_corrupt_every;    /* h_p2p_corrupt_every: plant, every Nth outgoing datagram gets a broken CRC */
    int h_no_self_check;    /* v0.29.1 h_p2p_no_self_check: control, the two self-loop checks (local id == peer id; nonce_tx == mine) are off */
    int h_no_sess_check;    /* v0.29.7 h_p2p_no_sess_check: control, a candidate needs no Steam P2P session (the first non-self one is taken) */
    int h_amb_first;        /* v0.29.7 (unit test only): control, two different validated candidates are not refused (the first is taken) */
} P2pCfg;

/* events p2p_poll returns (game thread) */
enum {
    P2P_EV_NONE = 0,
    P2P_EV_OPENED,          /* p2p_open finished: Steam resolved, the channel drained, no foreign datagram was waiting */
    P2P_EV_NOSTEAM,         /* p2p_open failed: steam_api64.dll not loaded / wrong timestamp / an export missing / networking() NULL (detail = P2P_NS_*) */
    P2P_EV_FOREIGN,         /* p2p_open found a datagram on our channel that is not ours (bad magic/crc, or from another id): stay PASSIVE */
    P2P_EV_VERIFIED,        /* the first valid datagram from the expected peer arrived (the probe budget no longer applies) */
    P2P_EV_MSG,             /* a message (RBM_HELLO..RBM_BYE once per seq, RBM_BEACON as received) */
    P2P_EV_SILENT,          /* a verified peer sent nothing valid for peer_timeout_ms */
    P2P_EV_PEER_RESTART,    /* a valid datagram from the expected id with a NEW nonce_tx after the first one (the peer's plugin restarted) */
    P2P_EV_BUDGET,          /* the probe budget ran out before the peer was verified (no more sends this battle) */
    P2P_EV_SELF,            /* v0.29.1 (review M1): the expected peer id IS this PC's own SteamID: nothing was sent, stay PASSIVE */
    P2P_EV_BADID,           /* v0.29.7: no candidate validated (p2p_open_cands): none with a Steam P2P session, or two different ones; nothing sent */
    P2P_EV_N
};
enum { P2P_NS_NONE = 0, P2P_NS_NO_MODULE, P2P_NS_DLL_TS, P2P_NS_EXPORT, P2P_NS_NO_CLIENT, P2P_NS_NO_NETWORKING,
       P2P_NS_NO_USER,      /* v0.29.1: ISteamUser / the local SteamID is not available (fail closed: no pairing without the self check) */
       P2P_NS_N };
extern const char *const k_p2p_ev[P2P_EV_N];     /* "none", "opened", "nosteam", ..., "self", "badid" */
extern const char *const k_p2p_ns[P2P_NS_N];     /* "-", "no_module", "dll_ts", "export", "no_client", "no_networking", "no_user" */

typedef struct { int kind; int detail; RbmMsg msg; } P2pEvent;

typedef struct {
    uint64_t tx_dgrams, tx_bytes, tx_resends, tx_acks, tx_probe;   /* tx_probe = datagrams sent before the peer was verified */
    uint64_t rx_dgrams, rx_ok, rx_dup, rx_bad, rx_foreign_id, rx_stale_nonce, rx_unsolicited;
    uint64_t q_drops;                                              /* game-thread queue full (a message was not queued) */
    uint32_t unacked, verified, open, api_steam;
    int      bad_err[RBM_NERR];                                    /* rx_bad by rbm_decode error */
} P2pStats;

/* lifecycle */
void p2p_set_api(const P2pApi *api);          /* harness (SC6RB_HARNESS) / unit test only: use this table instead of steam_api64.dll.
                                                 Call before p2p_init. In the plugin build the symbol exists but the core never calls it. */
int  p2p_init(const P2pCfg *cfg);             /* plugin_init (rbonline): queues + the idle I/O thread. No Steam call. 1 = ok */
void p2p_shutdown(int process_exit);          /* stop the thread (bounded wait at process exit) */

/* per battle (game thread; non-blocking; results arrive as events) */
int  p2p_open(uint64_t peer_steamid, uint32_t my_nonce);   /* PROBE entry. The core has already checked (id >> 32) == 0x01100001. 1 = queued */
void p2p_close(int bye_reason);               /* END: BYE x3 best effort (only if the peer was verified), then forget the peer and flush */
int  p2p_send(const RbmMsg *m);               /* queue one message. Sequenced types are resent every resend_ms until acked (or the battle
                                                 ends); RBM_BEACON once. While the peer is unverified only RBM_HELLO is allowed (the probe
                                                 budget applies). 1 = queued, 0 = refused (queue full / not open / budget) */
int  p2p_poll(P2pEvent *ev);                  /* next event; 1 = got one */
void p2p_stats(P2pStats *out);                /* a snapshot (any thread) */

/* ==================================================================================================================================
 * APPENDED by the transport layer (v0.29.0). Nothing above this line changed. See src/p2p.c's header for the behavior.
 *
 * Semantics the frozen declarations leave open, fixed here:
 *   - P2pStats counters (tx_*, rx_*) count since the last p2p_open the I/O thread processed (one battle); q_drops since the last
 *     p2p_open call. unacked / verified / open / api_steam describe the current open. p2p_stats_total() gives process totals.
 *   - P2P_EV_MSG: `detail` = the sender's header nonce_tx (cast to int). The peer's nonce is fixed per open (p2p.c drops any other),
 *     so this is what the core puts into RbmHelloAck.hello_nonce when it answers a HELLO. Also readable as p2p_peer_nonce().
 *   - P2P_EV_NOSTEAM: `detail` = P2P_NS_*; every other event: detail 0.
 *   - p2p_poll has ONE consumer (the game thread); p2p_send / p2p_open / p2p_close are meant for the game thread too (p2p_send is
 *     multi-producer safe, so another thread may also call it; none of them blocks).
 * ================================================================================================================================== */
typedef struct {
    uint64_t tx_fail;            /* the send call returned false */
    uint64_t tx_rate_deferred;   /* a due datagram waited for the rate cap (token bucket) */
    uint64_t tx_window_drops;    /* a sequenced message dropped by the I/O thread: P2P_MAX_INFLIGHT messages already unacked */
    uint64_t tx_unseq_drops;     /* an RBM_BEACON dropped: the unsequenced queue was full */
    uint64_t tx_refused;         /* p2p_send / p2p_open refused for state (not open, unverified non-HELLO, budget, failed open, bad type/id) */
    uint64_t cmd_stale;          /* OPEN / SEND commands the I/O thread discarded because their open had been closed already */
    uint64_t tx_bye;             /* RBM_BYE datagrams sent by p2p_close */
    uint64_t rx_read_fail;       /* avail said a datagram waits, read returned false */
    uint64_t rx_window_drops;    /* a sequenced datagram more than P2P_RX_WINDOW ahead of the cumulative ack (not delivered, not acked) */
    uint64_t rx_backpressure;    /* the rx loop paused because the event queue was nearly full (datagrams stay in Steam's buffer) */
    uint64_t rx_after_budget;    /* datagrams read after P2P_EV_BUDGET (counted, never acted on) */
    uint64_t ev_stale;           /* events of a closed open dropped by p2p_poll */
    uint64_t ev_ctl_pending;     /* control events that had to wait for event-queue space */
    uint64_t open_drained;       /* datagrams found waiting on our channel at p2p_open (all opens) */
    uint64_t open_stale;         /* ... of which valid ours from the expected id (discarded: they predate this open's nonce) */
    uint64_t open_foreign;       /* ... of which foreign (bad size / bad decode / another id); P2P_EV_FOREIGN unless h_no_guard */
    uint64_t opens, closes, nosteam, foreign, budget, silent, restart, verified;
    uint32_t io_tid;             /* the I/O thread id (0 = no thread) */
    uint32_t peer_nonce;         /* the adopted nonce_tx of the peer in the current open (0 = none yet) */
    int      last_ns;            /* P2P_NS_* of the last open */
    int      shutdown_state;     /* 0 = never shut down, 1 = clean (thread joined), 2 = wait timed out (thread left running),
                                    3 = process exit (no wait; the thread was not joined) */
    /* v0.29.1 (review M1 / m3, appended) */
    uint64_t rx_self;            /* datagrams whose nonce_tx is this open's own nonce (our own send looped back): dropped, never adopted */
    uint64_t self;               /* opens refused because the expected peer id is this PC's own SteamID (P2P_EV_SELF) */
    uint64_t ch_close;           /* CloseP2PChannelWithUser calls (one per ACTIVE open, after its BYEs) */
    /* v0.29.7 (appended) */
    uint64_t badid;              /* opens refused because no candidate validated (P2P_EV_BADID) */
    uint64_t sess_calls;         /* GetP2PSessionState calls (I/O thread) */
} P2pStatsExt;

#define P2P_MAX_INFLIGHT      256   /* unacked sequenced messages per open */
#define P2P_RX_WINDOW         1024  /* receive window (seqs ahead of the cumulative ack) */
#define P2P_RATE_PER_S        120   /* default rate cap: datagrams per second (token bucket); BYE x3 is exempt */
#define P2P_RATE_BURST        40
#define P2P_SHUTDOWN_WAIT_MS  1500  /* p2p_shutdown(0) bound; p2p_shutdown(1) never waits */
#define P2P_ACK_DELAY_MS      50    /* a pure RBM_ACK goes out at most this long after an unacked receipt (min(50, resend_ms/4)) */

void     p2p_stats_ext(P2pStatsExt *out);          /* a snapshot (any thread) */
void     p2p_stats_total(P2pStats *out);           /* process totals (any thread) */
uint32_t p2p_peer_nonce(void);                     /* the peer's adopted nonce_tx in the current open (0 = none); any thread */
uint32_t p2p_io_thread_id(void);                   /* RBO-P2P-THREAD: every Steam / fake call must come from this thread */
void     p2p_set_rate_cap(int per_s, int burst);   /* tests only (0 = the defaults above); takes effect at the next p2p_open */
const char *p2p_api_name(void);                    /* RB P2P api=: the table the last open used: the p2p_set_api table's name ("fake")
                                                      whenever an override is set, else "steam" (resolved) or "none" (NOSTEAM); "none"
                                                      before any open. Final once that open's OPENED / NOSTEAM / FOREIGN was polled. Any thread. */

/* ==================================================================================================================================
 * APPENDED v0.29.7 (OPTA-M2-SPEC.md §18.10: S-B step 1's why=badid). Nothing above this line changed meaning.
 *
 * The opponent's SteamID is no longer a single value the core trusts. The core collects a short ordered list of candidates (game reads
 * only, on the game thread; each already of the individual-SteamID type, (id >> 32) == 0x01100001) and p2p_open_cands hands it to the I/O
 * thread with the OPEN. There, after Steam is resolved and before anything is sent, each candidate in order is checked:
 *   1. it is not this PC's own SteamID (ISteamUser GetSteamID; unless h_no_self_check);
 *   2. Steam reports a P2P session with it (ISteamNetworking GetP2PSessionState returned true with m_bConnectionActive or m_bConnecting set:
 *      the peer the game itself talks to; unless h_no_sess_check). session_state NULL = no candidate passes (fail closed).
 * The first candidate that passes both is the peer, unless a DIFFERENT id passes too (then nothing is chosen: ambiguous). Outcomes:
 *   one peer -> the guard and P2P_EV_OPENED (or P2P_EV_FOREIGN) as before; every candidate was this PC's own id -> P2P_EV_SELF (as v0.29.1);
 *   otherwise -> P2P_EV_BADID. Steam unavailable -> P2P_EV_NOSTEAM before any candidate check (as before). Nothing is sent before a peer is
 *   chosen. p2p_open(id, nonce) = p2p_open_cands(&id, 1, nonce).
 * Counts only, never an id: p2p_peerid_result gives the last open's counts (the core writes them in its RB PEERID line). */
#define P2P_CAND_MAX 8
typedef struct {
    uint32_t gen;          /* the open these counts belong to (p2p_peerid_result returns 0 when it is not the current one) */
    uint8_t  n;            /* candidates received */
    uint8_t  ran;          /* 1 = the checks ran (Steam resolved, the local id known); 0 = NOSTEAM first, nothing was checked */
    uint8_t  self;         /* candidates equal to this PC's own SteamID */
    uint8_t  sess;         /* candidates with a Steam P2P session (active or connecting), self excluded; ids counted once */
    uint8_t  chosen;       /* 1-based index of the chosen candidate, 0 = none */
    uint8_t  ambiguous;    /* 1 = two different candidates passed: none chosen */
    uint8_t  sess_api;     /* 1 = session_state was available */
    uint8_t  pad_;
} P2pPeerIdRes;
int p2p_open_cands(const uint64_t *ids, int n, uint32_t my_nonce);   /* PROBE entry (game thread; non-blocking): n in 1..P2P_CAND_MAX, each of the
                                                                         individual type. 1 = queued, 0 = refused (as p2p_open) */
int p2p_peerid_result(P2pPeerIdRes *out);          /* the current open's counts once its control event was produced; 1 = they are the current
                                                      open's, 0 = none yet (out zeroed). Any thread. */
