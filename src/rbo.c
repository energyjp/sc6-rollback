/* Option A M2: mode=rbonline, the per-battle state machine, the side-channel protocol, the armed actions (InputLag pin, stall lift,
 * reset wall, drain, END), the RB log lines and the title (src/rbo.h; OPTA-M2-SPEC.md §3-§7, §9; sc6rb/M2-INTERFACES.md §3, §5.3, §7).
 * Part of the rollback core.
 *
 * Threads. Everything below runs on the GAME thread (called from the hook bodies, src/rbo_body.c, on the thunk's private stack, or from
 * the armed step / FetchInput), except the rbo writer thread (formats the RB lines) and rbo_title_token (M1's writer thread).
 * Passive paths (PASSIVE / PROBE / PAIRED, and END of a battle that never armed): no LOG, no snprintf, no allocation, no game write, no
 * game call. They read game memory only through rbo_body.c, call the p2p queue functions (src/p2p.h: non-blocking, no Steam call on this
 * thread) and push fixed-size events into a preallocated lock-free ring (M0 §5 A3: the log lock and WriteFile stay off the game thread).
 * Armed paths may format text (still through the ring, so every RB line keeps its order) and write game memory through rbo_gw* (counted:
 * rbo_h_game_writes in harness builds). */
#include "rbo_int.inc"
#include "rb.h"
#include "lipsync.h"   /* v0.29.4 FACES: lipsync_rbo_title */
#include "overlay.h"   /* v0.31.0 (§20.1): the badge */
#include "cue.h"       /* v0.31.0 (§20.2): the sound cue */
#include "argcfix.h"   /* v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix (applied at ARM, restored at END) */
void modes_rbo_edge(char *out, size_t cap);   /* v0.31.0 (modes.c): this battle's round-edge counters (RB EDGE) */
#include <intrin.h>

const char *const k_rbo_state[RBO_NSTATE] = { "passive", "probe", "paired", "armed", "drained", "end" };

volatile LONG rbo_off, rbo_st = RBO_PASSIVE, rbo_probe_off;
volatile LONG rbo_q1sig;   /* v0.29.2 (OPTA-M2-SPEC.md §16.6): set by rbo_install */
/* v0.29.2: RBW_Q1SIG (36) is appended in src/rbomsg.h; the reason names of this file go through rbw_nm (M2-INTERFACES.md "Requests from
 * core"). v0.29.3 (review m7 / CQ-1): k_rbw_name now names it too ("q1sig"); rbw_nm stays as the NULL-safe accessor */
static const char *rbw_nm(int w) {
    if (w == RBW_Q1SIG) return "q1sig";
    const char *n = (w >= 0 && w < RBW_N) ? k_rbw_name[w] : NULL;
    return n ? n : "?";
}
int rbo_remote = 1;
int rbo_d = 2, rbo_max_rb = 7;
static int g_dadapt = 1;          /* v0.33.0 (OPTA-M2-SPEC.md §22.2): rbonline_d_adapt (rbo_runtime_init; rbo_install's gate refuses other values than 0 | 1) */
uint32_t rbo_armed_round;
uintptr_t rbo_obj, rbo_mgr;
#ifdef SC6RB_HARNESS
volatile LONG rbo_h_game_writes;
#endif
static char g_off_gate[24];
static uint64_t g_qpf;
static int g_h_force_pair;             /* harness only (h_rbo_force_pair=<round>, read with cfg_geti): PROBE skips the side channel and is PAIRED with
                                        * agreement(round) already past the margin: an in-process armed run without a peer (0 in the plugin build) */
static int g_h_peerid_old;             /* v0.29.7 harness-only control (h_rbo_peerid_old, cfg_geti; 0 in the plugin build): XPEERPM: the
                                        * opponent id is v0.29.6's single raw read of session+0xd0 (no CommSys peer source) */
static int g_h_no_ranked_gate;         /* v0.29.1 harness-only controls / plants (cfg_geti; 0 in the plugin build): XRANKED (no M3 gate), */
#ifdef SC6RB_HARNESS
static int g_h_no_asym_fix, g_h_passive_write;   /* XASYM (an arm failure neither degrades nor tells the peer: the v0.29.0 behavior),
                                                  * ZEROX / GUARDX (a raw, uncounted game write from a passive body) */
static int g_h_ver_noescape;              /* XHELLOVER: the peer's version string copied unescaped (v0.29.0) */
static int g_h_asym_early_old;            /* v0.29.5 RBO: XASYMEARLY (a DRAIN(arm_fail) that came before this side's ARM is not acted on: v0.29.4) */
static int g_h_beacon_old;                /* v0.29.6 (review MINOR 4): XASYMBCN (a degraded peer's beacon is ignored: v0.29.5) */
static int g_h_drop_armfail;              /* v0.29.6 (review MINOR 4): ASYMBCN's plant on the degrading PC (its DRAIN(arm_fail) never reaches p2p: a
                                           * sequenced DRAIN lost as tx_window_drops / q_drops lose one under heavy loss; its beacons still go out) */
static int g_h_hello_ver_inject, g_h_hack_nonce0, g_h_hack0_old;   /* HELLOVER (a forged log line in plugin_version), HACK0 (this side's
                                                  * HELLO_ACK names no HELLO) / XHACK0 (the v0.29.0 rule: a HELLO_ACK naming none is accepted) */
/* v0.30.0 (OPTA-M2-SPEC.md §19.6) harness-only controls / plants (cfg_geti; 0 in the plugin build) */
static int g_h_r_floor1;          /* XARMR0: v0.29.9's rbo_r_mine (floored at round 1: r=0 never proposed) */
static int g_h_beacon_ord_old;    /* XBCNR0: the ARMED side compares the peer's beacon round raw (an intro beacon, round 9, is "ahead" of round 0) */
static int g_h_peer_armed_old;    /* XPEERARMR0: the PAIRED side's reverse view needs peer_armed_round >= 1 (v0.29.9: a peer armed at r=0 is never seen) */
static int g_h_bcn_replay_intro;  /* BCNR0 / XBCNR0 plant: this side replays its last intro beacon (round 9, PAIRED) once, 100 ticks after its ARM */
static int g_h_bcn_replay_n = 3, g_h_bcn_drop_first;   /* v0.33.0 (n3): the replays (v0.32.0: 1) and XBCNR0L's lost first replay */
static int g_h_drop_ack0;         /* PEERARMR0 plant: this side's ARM_ACK(0) is never handed to p2p (the peer's half of the r=0 agreement is lost) */
static int g_h_zone_lag_late;     /* ZLAGLATE plant: the zone lookahead of the STOCK decision is this many frames shorter (the switch comes late) */
static int g_h_ts_plant_sum, g_h_ts_plant_on;   /* TSBOGUS plant: every report of this side carries adv_sum = this value */
static int g_h_ts_noguard;        /* XTSBOGUS: the time sync's guard off (a PC waits on the peer's report alone) */
static int g_h_ts_band16 = RBO_TS_BAND16;   /* XTSQUIET: the dead band (1/16 frame) planted to 0 */
static int g_h_chk_old;           /* v0.32.0 (OPTA-M2-SPEC.md §21.1): XCHAIN / XPROTO8: v0.31.0's CHECK (frame f's hash only: n = chain = fset = 0) */
static int g_h_dadapt_plant;      /* v0.33.0 (OPTA-M2-SPEC.md §22.2): DDIS plant: this side's computed delay + 1 (a disagreement) */
static int g_h_darm_nocheck;      /* v0.33.0: XDDIS control: the peer's ARM delay is not compared (v0.32.0 never compared it) */
static int g_h_dadapt_old;        /* v0.33.0: XDAD / XPROTO9: v0.32.0's delay rule (no RTT in the HELLO, no DADAPT flag, d = rbonline_d) */
static int g_h_lim_old;           /* v0.33.0: XLIM400: v0.32.0's badge (no at-limit state) */
static int g_h_argc_old;          /* v0.34.0 (OPTA-M2-SPEC.md §23.1): XARGCOFF: no handshake for the fix (the flag is not compared; each PC applies it at ARM on its own key) */
static int g_h_argc_noflag;       /* v0.34.0: XPROTO10: this side's HELLO never carries RBM_HF_ARGC (a v0.33.0 HELLO at protocol 10) */
static int g_h_argc_noapply;      /* v0.34.0: XARGC: the agreed fix is never applied (the bytes stay stock while ARMED: the sampler's teeth) */
#endif
const char *g_p2p_api_name;            /* "steam" | "fake" (RB P2P api=; set by rbo_install from the table in use) */

static uint64_t ms_now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return g_qpf ? (uint64_t)q.QuadPart * 1000ULL / g_qpf : 0; }
static void scpy(char *dst, size_t cap, const char *src) { size_t i = 0; if (src) for (; src[i] && i + 1 < cap; i++) dst[i] = src[i]; if (cap) dst[i] = 0; }

/* ================================================================== the event ring (MPSC, Vyukov bounded queue; as M1's record ring) */
enum { EV_TEXT = 1, EV_STATE, EV_PEER, EV_P2P, EV_CTX, EV_HELLO, EV_ENDP, EV_FLUSH, EV_PEERID,   /* v0.29.7: + EV_PEERID */
       EV_CONTENT, EV_CDUMP,   /* v0.29.8 (OPTA-M2-SPEC.md §18.11): RB CONTENT (one per player) and the dump file of the capture */
       EV_ARGC };              /* v0.34.0 (OPTA-M2-SPEC.md §23.1): RB ARGC, one per battle that probed (the EvaluateIf argc fix's state) */
typedef struct {
    int type; uint32_t b;
    uint32_t u[24];
    uint64_t q[12];
    char s[256];
} RboEv;
_Static_assert(__builtin_offsetof(RboEv, s) % 8 == 0, "v0.29.9: EV_CONTENT carries four uint64 values in s");
typedef struct { volatile LONG64 seq; LONG64 pad_; RboEv ev; } RboCell;
#define EVQ_CAP 4096u
static RboCell *g_evq;
static volatile LONG64 g_evq_enq, g_evq_drops;
static LONG64 g_evq_deq;
static HANDLE g_writer; static volatile LONG g_writer_stop;

static RboEv *ev_begin(int type, LONG64 *pos_out) {
    if (!g_evq) return NULL;
    for (;;) {
        LONG64 pos = g_evq_enq;
        RboCell *c = &g_evq[pos & (EVQ_CAP - 1)];
        LONG64 dif = c->seq - pos;
        if (dif == 0) {
            if (_InterlockedCompareExchange64(&g_evq_enq, pos + 1, pos) == pos) {
                RboEv *e = &c->ev;
                e->type = type; e->b = nobs_bat; e->s[0] = 0;
                for (int i = 0; i < 24; i++) e->u[i] = 0;
                for (int i = 0; i < 12; i++) e->q[i] = 0;
                *pos_out = pos;
                return e;
            }
        } else if (dif < 0) { _InterlockedIncrement64(&g_evq_drops); return NULL; }
    }
}
static void ev_end(LONG64 pos) { RboCell *c = &g_evq[pos & (EVQ_CAP - 1)]; _ReadWriteBarrier(); _InterlockedExchange64(&c->seq, pos + 1); }
static int ev_pop(RboEv *out) {
    RboCell *c = &g_evq[g_evq_deq & (EVQ_CAP - 1)];
    if (c->seq - (g_evq_deq + 1) != 0) return 0;
    _ReadWriteBarrier();
    *out = c->ev;
    LONG64 pos = g_evq_deq; g_evq_deq = pos + 1;
    _InterlockedExchange64(&c->seq, pos + EVQ_CAP);
    return 1;
}
void rbo_ev_text(const char *line) { LONG64 p; RboEv *e = ev_begin(EV_TEXT, &p); if (!e) return; scpy(e->s, sizeof e->s, line); ev_end(p); }
/* the ring drained by the writer (bounded wait): a line written directly after it keeps the RB lines in order */
static void ring_sync(void) {
    if (!g_evq || !g_writer || g_writer_stop) return;
    for (int i = 0; i < 2000 && g_evq_deq < g_evq_enq; i++) { if (i < 50) YieldProcessor(); else Sleep(0); }
}
/* armed / install contexts only (never a passive battle body): the ring drained first, then the line written here, synchronously, so it
 * lands in order with the C1b machinery's own LOG lines (RBABORT, RBK, ...). Passive paths push structured events instead. */
void rbo_log(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);   /* v0.32.0 review fix: 512 -> 1024 (RB SUMMARY passed 512 bytes) */
    ring_sync();
    LOG("%s\n", b);
}
void rbo_unexpected(const char *kind, const char *detail) { rbo_log("RB UNEXPECTED kind=%s b=%u detail=%s", kind, nobs_bat, detail ? detail : "-"); }

/* ================================================================== the battle (game thread) */
#define CHK_N 64
typedef struct { uint32_t f, round, gt, n, fset; uint64_t hash, chain; int have; uint32_t f0; } ChkEnt;   /* v0.32.0: + n, fset, chain (RBM_PROTO 8); review fix M1: + f0 (mine: the chain's first frame) */
#define CKF_N 1024   /* v0.32.0: the per-frame CHECK hashes this PC kept (>= rbonline_check_every's 600), named on a chain mismatch */
static struct {
    uint32_t b; int online, st_max, probe_tried, last_os, ended, armed_ever;
    uint32_t ticks, round, gt;
    uintptr_t obj0, mgr;
    uint32_t nonce; int opened, p2p_open, api;
    /* HELLO */
    int hello_sent, have_peer_hello, have_vmine, have_vpeer, hello_logged, content_done;
    RbmHello mine, peer; uint16_t vmine, vpeer; uint32_t peer_hello_nonce;
    /* ARM. v0.30.0 (OPTA-M2-SPEC.md §19.3): the game's round indices. last_reset = the last real round whose reset this PC saw (-1 = none);
     * the battle's first reset is the intro (counted: nresets), every later one with round <= 15 is a real round (reset_mask bit r, real_resets);
     * arm_pending_set says arm_pending holds a round (0 is a round now, no longer "nothing pending") */
    int32_t last_reset; uint32_t reset_mask; int nresets, real_resets, arm_pending_set, round_model_seen;
    uint32_t r_proposed, sent_ack, recv_ack, arm_pending, armed_round;
    uint32_t early_arm_r; int early_arm;   /* v0.29.0: a peer ARM that arrived while this side was still in PROBE (answered at PAIRED) */
    uint64_t t_agreed[16]; uint64_t margin_ms, t_outcome;
    /* armed */
    uint32_t lag_min, lag_max; int lag_pinned, lag_restored, fetch_set, fetch_restored, step_restored;
    int asym, ctxonly, core_armed;   /* v0.29.1 (review M2): asym = the two PCs did not arm together (title rb=ASYM); ctxonly = the degraded arm
                                      * (L3-stock steps without the core); core_armed = rbo_core_arm ran (END must close it, even after a failure) */
    int drained_why, drain_logged, desync, peer_drained, peer_drain_why, peer_bye, why_end;
    uint64_t lifts, lift_bound_stops, last_beacon, drain_t0;
    uint32_t drain_f, drain_r, drain_gt, drain_unc;
    int peer_state, peer_armed_round, peer_round, peer_gt, have_beacon;
    /* CHECK */
    ChkEnt mine_ck[CHK_N], peer_ck[CHK_N]; uint64_t checks, checks_ok, checks_lines; uint32_t last_check_f;
    /* v0.32.0 (OPTA-M2-SPEC.md §21.1): the chain of the current window (rbo_chain_window), the per-frame hashes, the counters */
    uint64_t ck_chain; uint32_t ck_n, ck_fset, ck_w; int ck_w_set;
    uint64_t chain_cmp, chain_skips, chain_mm, chain_lines;
    struct { uint32_t f, round, gt; uint64_t h; } ckf[CKF_N];   /* review fix M1: + round, gt (a flush at an earlier frame of the open chain) */
    /* v0.32.0 review fix (M1, OPTA-M2-SPEC.md §21.12): the OPEN chain = every frame this PC checked since its last CHECK, [ck_f0, ck_last] (ck_n frames);
     * it is sent at the next due frame, at a zone entry (the last checked frame before the zone), at END, or when the peer's flush names one of its
     * frames (the mirror), and only then restarts. ck_ending: the END flush is running (a mismatch there is logged, never drained) */
    uint32_t ck_f0, ck_last; int ck_ending;
    uint64_t chain_zone, chain_end, chain_mirror;
    uint32_t tripwire_f;
    /* v0.29.7 (OPTA-M2-SPEC.md §18.10): the opponent-id candidates of this battle's PROBE entry (counts and sources only; no id is kept here) */
    uint32_t pid_n_src[RBO_PSRC_N];     /* what each source found (index = RBO_PSRC_*) */
    uint8_t pid_srcs[RBO_CAND_MAX];     /* the source of each candidate handed to p2p_open_cands (in its order) */
    int pid_nsent, pid_type_ok, pid_logged, pid_queued;
    /* v0.30.0 (OPTA-M2-SPEC.md §19.2): the zone lag. lag_mode = what the game holds now (RBO_LAG_PIN = d/d, RBO_LAG_STOCK = the saved values);
     * lag_cur_* = the values this side last wrote (or found at ARM): anything else at an IsPaused is a foreign writer (lag_off: stop managing) */
    int lag_mode, lag_zl0, lag_off, lag_late_logged; uint32_t lag_cur_min, lag_cur_max;
    int passive_why;   /* v0.31.0 (§20.1): the last PASSIVE transition's reason + 1 (0 = none yet), for the badge's STOCK NETCODE (<why>) */
    uint64_t lag_pins, lag_stocks, lag_late, lag_foreign, lag_lines;
    /* v0.30.0 (OPTA-M2-SPEC.md §19.4): the time sync (this PC's window of adv samples, the debt of withheld lifts, the peer's last report) */
    int8_t ts_win[RBO_TS_WIN]; uint32_t ts_wi, ts_n; int32_t ts_sum;
    int ts_settle, ts_elig_prev, ts_debt, ts_hold, ts_gap, ts_stopped, ts_since_rep, ts_round_set;
    uint32_t ts_round, ts_waits_r;
    uint64_t ts_waits, ts_decisions, ts_tx, ts_rx, ts_stops, ts_lines;
    RbmTsync ts_peer; int ts_have_peer; uint32_t ts_peer_tick, ts_bad_tick; uint64_t ts_rx_bad; int ts_bad_logged;
    uint32_t arm_tick;
    /* v0.33.0 (OPTA-M2-SPEC.md §22.2): the delay this battle arms with (computed at pairing from the two HELLOs; the ARM carries it) */
    int d_arm, d_set, darm_refused; uint32_t early_arm_d;
    /* v0.33.0 (§22.3): the at-limit window: one bit per armed fight tick (1 = the stall lift stopped at the max_rb bound: this PC waited) */
    uint64_t lim_bits[2]; int lim_i, lim_n, lim_samples; volatile int lim_on; uint64_t lim_enter, lim_on_ticks, lim_ticks;
    /* v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix of this battle. on = both HELLOs advertised it (set at PAIRED); tried / applied / restored;
     * apply_why / restore_why = ARGC_W_*; st_arm = the four sites right after the apply; the per-battle RB ARGC line at END */
    int argc_on, argc_tried, argc_applied, argc_restored, argc_apply_why, argc_restore_why, argc_refused; int argc_st_arm[ARGC_NSITES];
#ifdef SC6RB_HARNESS
    RbmMsg h_bcn_intro; int h_bcn_have, h_bcn_done;   /* BCNR0's plant: this side's last intro beacon, replayed once after its ARM */
#endif
} B;
static volatile int g_mem_report_pending;   /* v0.29.1 (B1): keyed on the doors (module state), never on a per-battle flag */
static void ev_format(const RboEv *e);
static void drain_progress(void);

static void sm_set(int to, int why) {
    int from = (int)rbo_st;
    if (!rbo_edge_ok(from, to)) {   /* a bug: never silently */
        LONG64 p; RboEv *e = ev_begin(EV_TEXT, &p);
        if (e) { scpy(e->s, sizeof e->s, "RB UNEXPECTED kind=edge detail=illegal_state_edge"); ev_end(p); }
    }
    rbo_ev_state(from, to, why);
    if (to == RBO_PASSIVE) B.passive_why = why + 1;   /* v0.31.0: before the state (the badge reads them in that order) */
    InterlockedExchange(&rbo_st, to);
    if (to > B.st_max && to != RBO_END) B.st_max = to;
}
void rbo_ev_state(int from, int to, int why) {
    if (B.armed_ever) {   /* the battle armed (both players run the mod): written directly, in order with the core's lines */
        RboEv e; e.type = EV_STATE; e.b = nobs_bat;
        e.u[0] = (uint32_t)from; e.u[1] = (uint32_t)to; e.u[2] = (uint32_t)why; e.u[3] = B.round; e.u[4] = B.gt; e.u[5] = nobs_tick;
        ring_sync(); ev_format(&e);
        return;
    }
    LONG64 p; RboEv *e = ev_begin(EV_STATE, &p); if (!e) return;
    e->u[0] = (uint32_t)from; e->u[1] = (uint32_t)to; e->u[2] = (uint32_t)why; e->u[3] = B.round; e->u[4] = B.gt; e->u[5] = nobs_tick;
    ev_end(p);
}
RboState rbo_state(void) { return (RboState)rbo_st; }
uint32_t rbo_battle(void) { return nobs_bat; }
int rbo_drained(void) { return rbo_st == RBO_DRAINED; }

/* ---------------------------------------------------------------- armed game writes (counted) */
uint32_t rbo_gw32(uintptr_t a, uint32_t v) {
    volatile uint32_t *p = (volatile uint32_t *)a; uint32_t o = *p; *p = v;
#ifdef SC6RB_HARNESS
    InterlockedIncrement(&rbo_h_game_writes);
#endif
    return o;
}
uint8_t rbo_gw8(uintptr_t a, uint8_t v) {
    volatile uint8_t *p = (volatile uint8_t *)a; uint8_t o = *p; *p = v;
#ifdef SC6RB_HARNESS
    InterlockedIncrement(&rbo_h_game_writes);
#endif
    return o;
}
uint64_t rbo_gw64(uintptr_t a, uint64_t v) {
    volatile uint64_t *p = (volatile uint64_t *)a; uint64_t o = *p; *p = v;
#ifdef SC6RB_HARNESS
    InterlockedIncrement(&rbo_h_game_writes);
#endif
    return o;
}

/* ---------------------------------------------------------------- side-channel helpers (passive-safe) */
static void send_msg(uint8_t type, const RbmMsg *m) { if (B.p2p_open) p2p_send(m); (void)type; }
static void msg_zero(RbmMsg *m, uint8_t type) { uint8_t *b = (uint8_t *)m; for (size_t i = 0; i < sizeof *m; i++) b[i] = 0; m->type = type; }
static void send_arm(uint8_t type, uint32_t r) {
    RbmMsg m; msg_zero(&m, type);
    m.u.arm.round = (uint8_t)r; m.u.arm.d = (uint8_t)rbo_d; m.u.arm.max_rb = (uint8_t)rbo_max_rb; m.u.arm.check_every = (uint16_t)g_cfg.rbonline_check_every;
    send_msg(type, &m);
}
static void p2p_end(int why) { if (B.p2p_open) { p2p_close(why); B.p2p_open = 0; } }

/* ---------------------------------------------------------------- lifecycle */
static void battle_reset(uint32_t b) {
    uint8_t *p = (uint8_t *)&B; for (size_t i = 0; i < sizeof B; i++) p[i] = 0;
    B.b = b; B.last_os = -1; B.peer_state = -1;
    B.last_reset = -1;   /* v0.30.0: no real round seen yet */
    rbo_d = g_cfg.rbonline_d;   /* v0.33.0 (§22.2): every battle starts at the configured delay; pairing sets the agreed one */
    g_cfg.net_delay = rbo_d < 4 ? rbo_d : 4;   /* the C1b core's D (rbo_core_force's rule), set again at ARM from the agreed delay */
    InterlockedExchange(&rbo_st, RBO_PASSIVE);
    rbo_ctx_on = 0;
}
/* v0.30.0 (§19.3): the round this PC proposes (the harness control XARMR0 = v0.29.9's floor at 1) */
static uint32_t r_mine_now(void) {
#ifdef SC6RB_HARNESS
    if (g_h_r_floor1) return rbo_r_mine_v0299(g_cfg.rbonline_arm_round, B.last_reset);
#endif
    return rbo_r_mine(g_cfg.rbonline_arm_round, B.last_reset);
}
/* v0.30.0 (§19.2): hand the saved InputLagMin / InputLagMax back (drain, END). Only a PIN needs a write: in STOCK the game already holds them */
static void lag_restore(void) {
    if (!B.lag_pinned || B.lag_restored) return;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_rbo_lag_norestore) return;   /* XLAG: never restored (lag_restored stays 0) */
#endif
    if (B.lag_mode == RBO_LAG_PIN) { rbo_gw32(rbo_obj + RBO_O_LAGMIN, B.lag_min); rbo_gw32(rbo_obj + RBO_O_LAGMAX, B.lag_max); }
    B.lag_mode = RBO_LAG_STOCK; B.lag_cur_min = B.lag_min; B.lag_cur_max = B.lag_max;
    B.lag_restored = 1;
}
/* ---------------------------------------------------------------- v0.29.7: RB PEERID (one line per battle that reached the id step)
 * src = the chosen candidate's source; cand = what each source found; type_ok = candidates of the individual-SteamID type (the game thread's
 * check); self / p2p_ok / ambiguous = the I/O thread's checks (p2p_peerid_result); steam = ran | skipped (no typed candidate: no Steam query) |
 * none (Steam unavailable: nothing checked) | pending (the battle ended before the I/O thread answered). Classes and counts only. */
enum { PID_OK = 0, PID_BADID, PID_SELF, PID_NOSTEAM, PID_UNRESOLVED, PID_N };
enum { PST_SKIPPED = 0, PST_RAN, PST_NONE, PST_PENDING, PST_N };
static const char *const k_pid_res[PID_N] = { "ok", "badid", "self", "nosteam", "unresolved" };
static const char *const k_pid_st[PST_N] = { "skipped", "ran", "none", "pending" };
static const char *const k_pid_src[RBO_PSRC_N] = { "-", "peer", "raw" };
static void peerid_ev(int result, int steam, const P2pPeerIdRes *r) {
    if (B.pid_logged) return;
    B.pid_logged = 1;
    LONG64 p; RboEv *e = ev_begin(EV_PEERID, &p); if (!e) return;
    int srci = (r && r->chosen >= 1 && (int)r->chosen <= B.pid_nsent) ? (int)B.pid_srcs[r->chosen - 1] : 0;
    e->u[0] = (uint32_t)(srci > 0 && srci < RBO_PSRC_N ? srci : 0); e->u[1] = B.pid_n_src[RBO_PSRC_PEER]; e->u[2] = B.pid_n_src[RBO_PSRC_RAW];
    e->u[3] = (uint32_t)B.pid_type_ok; e->u[4] = r ? r->self : 0; e->u[5] = r ? r->sess : 0; e->u[6] = (uint32_t)steam; e->u[7] = (uint32_t)result;
    e->u[8] = r ? r->ambiguous : 0;
    ev_end(p);
}
static void peerid_from_p2p(int result, int steam) { P2pPeerIdRes r; p2p_peerid_result(&r); peerid_ev(result, steam, &r); }

static void end_lines(void);
static int chk_old(void);                                                    /* review fix M1 (the CHECK chain, below) */
enum { CKW_DUE = 0, CKW_ZONE, CKW_END, CKW_MIRROR };   /* why a CHECK was sent (review fix M1) */
static void ck_flush(uint32_t f, int why);
static void chain_name_frames(uint32_t f, uint32_t f0, uint32_t n, int end);
static void argc_end_restore(void);   /* v0.34.0 (§23.1), below */
static void argc_line(void);
void rbo_end(int why) {
    if (rbo_st == RBO_END) return;
    if (B.pid_queued && !B.pid_logged) peerid_from_p2p(PID_UNRESOLVED, PST_PENDING);   /* v0.29.7: the open was queued, its answer never came */
    int was_armed = rbo_st == RBO_ARMED || rbo_st == RBO_DRAINED;
    if (was_armed && !chk_old() && B.ck_n) {   /* review fix M1: the battle's last frames (after the last CHECK) as a final CHECK, before p2p_end */
        uint32_t f0 = B.ck_f0, n = B.ck_n, fl = B.ck_last;
        B.ck_ending = 1;
        ck_flush(fl, CKW_END);
        chain_name_frames(fl, f0, n, 1);   /* always: the two logs name the frame even when neither PC was still armed to compare */
        B.ck_ending = 0;
    }
    B.why_end = why;
    rbo_ctx_on = 0;
    argc_end_restore();   /* v0.34.0 (§23.1): the EvaluateIf argc fix goes back to the stock bytes with the context (a no-op unless this battle applied it) */
    if (was_armed && B.ctxonly) {   /* v0.29.1 (review M2): the degraded arm set only the step target (no fetch cell, no InputLag pin, no core run) */
        rbo_hooktab[NOBS_H_IFC_STEP * 4 + 3] = rbo_hooktab[NOBS_H_IFC_STEP * 4 + 0];
        B.fetch_restored = 1; B.step_restored = 1; B.lag_restored = 1;
        if (B.core_armed) rbo_core_end(why == RBW_ONLINESYNC0 ? "onlinesync0" : why == RBW_STATE5 ? "state5" : "endplay");   /* a partial core arm is closed */
        rbo_core_degraded_end();   /* v0.29.5 RBO (F1): the lip-sync the degraded arm turned on (LIPSYNCSTAT) */
    } else if (was_armed) {
        /* the hand-back values first (OPTA-M0.md §1.6: a wrong 0x1498 prev gives a phantom press), then the cell and the step target */
        rbo_fetch_repair();
#ifdef SC6RB_HARNESS
        int norestore = g_cfg.h_rbo_no_restore != 0;
#else
        int norestore = 0;
#endif
        if (!norestore) {
            if (rbo_fetch_cell) InterlockedExchange64((volatile LONG64 *)rbo_fetch_cell, (LONG64)VA(RBO_A_FETCH));
            rbo_hooktab[NOBS_H_IFC_STEP * 4 + 3] = rbo_hooktab[NOBS_H_IFC_STEP * 4 + 0];
            B.fetch_restored = 1; B.step_restored = 1;
        }
        lag_restore();   /* v0.30.0: a write only if the game holds the PIN (the zones run STOCK) */
        rbo_core_end(why == RBW_ONLINESYNC0 ? "onlinesync0" : why == RBW_STATE5 ? "state5" : "endplay");
    } else {
        B.fetch_restored = 1; B.step_restored = 1; B.lag_restored = 1;
        if (B.core_armed) rbo_core_end("arm_failed");   /* v0.29.1: an arm that failed inside rbo_core_arm (cells never left the originals) is closed */
    }
    p2p_end(why);
    if (B.online || B.probe_tried) sm_set(RBO_END, why); else InterlockedExchange(&rbo_st, RBO_END);
    if (B.online || B.probe_tried) {
        if (was_armed) end_lines();
        else {   /* a battle that never armed: structured (no CRT on this path) */
            LONG64 p; RboEv *e = ev_begin(EV_ENDP, &p);
            if (e) {
                P2pStats s; p2p_stats(&s);
                e->u[0] = (uint32_t)why; e->u[1] = (uint32_t)B.st_max;
                e->q[0] = s.tx_dgrams; e->q[1] = s.rx_dgrams; e->q[2] = s.rx_bad; e->q[3] = s.rx_foreign_id; e->q[4] = s.rx_stale_nonce;
                e->q[5] = s.rx_dup; e->q[6] = s.tx_resends; e->q[7] = s.tx_probe; e->q[8] = s.q_drops;
                ev_end(p);
            }
        }
        argc_line();   /* v0.34.0 (§23.1): RB ARGC, one line per battle that probed */
    }
    B.ended = 1;
}
static void end_lines(void) {
    char sm[260], cost[96]; rbo_core_summary(sm, sizeof sm); rbo_core_cost(cost, sizeof cost);
    rbo_log("RB END b=%u why=%s lag_restored=%d fetch_restored=%d step_restored=%d", B.b, rbw_nm(B.why_end), B.lag_restored, B.fetch_restored, B.step_restored);
    rbo_log("RB COST b=%u %s", B.b, cost);
    /* v0.30.0 (§19.2, §19.4): the zone-lag and time-sync counters are APPENDED (existing key=value parsers keep matching) */
    rbo_log("RB SUMMARY b=%u state_max=%s %s lifts=%llu desyncs=%d checks_ok=%llu checks=%llu drains=%d why=%s peer_drained=%d peer_drain_why=%s peer_bye=%d lift_stops=%llu "
            "asym=%d ctxonly=%d last_check_f=%u lag_pins=%llu lag_stocks=%llu lag_late=%llu lag_foreign=%llu ts_waits=%llu ts_decisions=%llu ts_tx=%llu ts_rx=%llu ts_stops=%llu "
            "ts_rx_bad=%llu chain_cmp=%llu chain_skips=%llu chain_mm=%llu chain_zone=%llu chain_end=%llu chain_mirror=%llu",
            B.b, k_rbo_state[B.st_max], sm, (unsigned long long)B.lifts, B.desync, (unsigned long long)B.checks_ok, (unsigned long long)B.checks, B.drained_why ? 1 : 0,
            rbw_nm(B.why_end), B.peer_drained, rbw_nm(B.peer_drain_why), B.peer_bye, (unsigned long long)B.lift_bound_stops, B.asym, B.ctxonly, B.last_check_f,
            (unsigned long long)B.lag_pins, (unsigned long long)B.lag_stocks, (unsigned long long)B.lag_late, (unsigned long long)B.lag_foreign,
            (unsigned long long)B.ts_waits, (unsigned long long)B.ts_decisions, (unsigned long long)B.ts_tx, (unsigned long long)B.ts_rx, (unsigned long long)B.ts_stops,
            (unsigned long long)B.ts_rx_bad, (unsigned long long)B.chain_cmp, (unsigned long long)B.chain_skips, (unsigned long long)B.chain_mm,   /* v0.32.0: appended */
            (unsigned long long)B.chain_zone, (unsigned long long)B.chain_end, (unsigned long long)B.chain_mirror);   /* review fix M1: appended */
    rbo_log("RB SUMMARY2 b=%u d=%d d_set=%d lim_enter=%llu lim_on_ticks=%llu lim_ticks=%llu lift_stops=%llu", B.b, rbo_d, B.d_set, (unsigned long long)B.lim_enter,
            (unsigned long long)B.lim_on_ticks, (unsigned long long)B.lim_ticks, (unsigned long long)B.lift_bound_stops);   /* v0.33.0 (§22.2, §22.3): a line of its own */
    {   char eg[240]; modes_rbo_edge(eg, sizeof eg); rbo_log("RB EDGE b=%u %s", B.b, eg); }   /* v0.31.0 (OPTA-M2-SPEC.md §20.3) */
    P2pStats s; p2p_stats(&s);
    rbo_log("RB P2P SUMMARY b=%u tx=%llu rx=%llu rx_bad=%llu rx_foreign_id=%llu rx_stale=%llu dup=%llu resends=%llu probe_sent=%llu q_drops=%llu", B.b,
            (unsigned long long)s.tx_dgrams, (unsigned long long)s.rx_dgrams, (unsigned long long)s.rx_bad, (unsigned long long)s.rx_foreign_id,
            (unsigned long long)s.rx_stale_nonce, (unsigned long long)s.rx_dup, (unsigned long long)s.tx_resends, (unsigned long long)s.tx_probe,
            (unsigned long long)s.q_drops);
}

static volatile int g_rbo_in_battle;          /* v0.31.0 (§20.1): between a battle's Init and its Shutdown (the badge: a battle vs the menus) */
static volatile uint32_t g_badge_rtt_bits;   /* v0.31.0: the game's RTT (+0x4408, a float's bits) at the last Tick of an online battle */
void rbo_on_init(uint64_t rule) {
    (void)rule;
    if (rbo_st != RBO_END && (B.p2p_open || rbo_st >= RBO_ARMED)) rbo_end(RBW_ENDPLAY);   /* a battle that never saw its Shutdown */
    battle_reset(nobs_bat);
    g_badge_rtt_bits = 0; g_rbo_in_battle = 1;
}
void rbo_on_shutdown(void) {
    g_rbo_in_battle = 0;
    if (rbo_st != RBO_END) rbo_end(RBW_ENDPLAY);
#ifdef SC6RB_HARNESS
    if (g_cfg.h_rbo_leak_arena && B.armed_ever) rbo_core_plant_leak();
#endif
    if (rbo_core_doors() && B.armed_ever) g_mem_report_pending = 1;   /* v0.29.1 (B1): the doors are module state; no TAINT any more */
}
void rbo_on_any_body(int hook) {
    if (!g_mem_report_pending || hook == NOBS_H_IFC_SHUTDOWN) return;
    g_mem_report_pending = 0;
    rbo_core_mem_after_shutdown(B.b);
}

/* ---------------------------------------------------------------- PROBE entry (Tick pre, +0x4405 0 -> 1) */
static void stay_passive(int why) { sm_set(RBO_PASSIVE, why); }
static void probe_entry(const RboObs *o) {
    B.probe_tried = 1;
    if (rbo_probe_off) { stay_passive(rbo_q1sig ? RBW_Q1SIG : RBW_CFG); return; }   /* v0.29.2: why=q1sig */
    if (o->cls != 2 || !o->bonline) { stay_passive(RBW_NOT_ONLINE); return; }
    if (!o->remote) { stay_passive(RBW_SPECTATOR); return; }
    /* v0.29.1 (review M3): the ranked gate fails CLOSED. No data-only detector of the match type exists (OPTA-M0 §1.11: every candidate goes
     * through game code; S-A logged mt=none), so rbonline cannot tell Ranked from Casual / Player Match. Unless the user allowed ranked
     * (rbonline_ranked=1, after S-C) or promised never to queue Ranked with this plugin installed (rbonline_unranked_ack=1), no battle probes:
     * zero datagrams, the battle stays M1-passive. */
    if (!g_cfg.rbonline_ranked && !g_cfg.rbonline_unranked_ack && !g_h_no_ranked_gate) { stay_passive(RBW_RANKED); return; }
    if (g_h_force_pair > 0 && g_h_force_pair < 16) {   /* harness only: no side channel; agreement(r) dated margin + 1 ms ago */
        B.obj0 = o->obj;
        sm_set(RBO_PROBE, RBW_PROBE);
        sm_set(RBO_PAIRED, RBW_PAIRED);
        uint32_t r = (uint32_t)g_h_force_pair;
        B.sent_ack = B.recv_ack = 1u << r; B.r_proposed = r;
        uint64_t now = ms_now(), m = (uint64_t)g_cfg.rbonline_arm_margin_ms + 1;
        B.t_agreed[r] = now > m ? now - m : 1;
        return;
    }
    /* v0.29.7 (OPTA-M2-SPEC.md §18.10; S-B step 1: session+0xd0 is a CommSys rand() handle, never a SteamID): the opponent's SteamID
     * candidates, in source order (the CommSys peer of the opponent destination, then v0.29.6's raw read), reads only (rbo_body.c). Here the
     * individual-SteamID type check, (id >> 32) == RBO_STEAMID_HI; the I/O thread then refuses this PC's own id and every id Steam has no P2P
     * session with (p2p_open_cands). No id leaves this function except into the p2p queue; RB PEERID logs counts only. */
    uint64_t cand[RBO_CAND_MAX], typed[RBO_CAND_MAX]; uint8_t csrc[RBO_CAND_MAX];
    int nc = rbo_read_opp_cands(cand, csrc, RBO_CAND_MAX, B.pid_n_src, g_h_peerid_old), nt = 0;
    for (int i = 0; i < nc; i++) if ((uint32_t)(cand[i] >> 32) == RBO_STEAMID_HI) { typed[nt] = cand[i]; B.pid_srcs[nt] = csrc[i]; nt++; }
    B.pid_type_ok = nt; B.pid_nsent = nt;
    int ok = nt > 0;
    { LONG64 p; RboEv *e = ev_begin(EV_PEER, &p); if (e) { e->u[0] = (uint32_t)ok; ev_end(p); } }
    if (!ok) { peerid_ev(PID_BADID, PST_SKIPPED, NULL); stay_passive(RBW_BADID); return; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    uint32_t nonce = (uint32_t)rbo_mix64((uint64_t)q.QuadPart ^ ((uint64_t)nobs_bat << 40) ^ (uint64_t)GetCurrentProcessId() << 20);
    if (!nonce) nonce = 1;
    B.nonce = nonce;
    if (!p2p_open_cands(typed, nt, nonce)) { peerid_ev(PID_NOSTEAM, PST_NONE, NULL); stay_passive(RBW_NOSTEAM); return; }
    B.pid_queued = 1;
    B.p2p_open = 1; B.obj0 = o->obj;
    for (size_t i = 0; i < sizeof rbo_ctx_acc; i++) ((uint8_t *)&rbo_ctx_acc)[i] = 0;
    rbo_ctx_on = 1;
    sm_set(RBO_PROBE, RBW_PROBE);
}

/* ---------------------------------------------------------------- v0.29.8: the content digest's diagnosis (OPTA-M2-SPEC.md §18.11)
 * Game thread, right after the digest: the sub-digests rbo_read_content left in rbo_cdiag (one RB CONTENT event per player) and, when it
 * captured, one EV_CDUMP that makes the writer write the dump file. Ring events only (no LOG, no snprintf, no allocation, no game write);
 * nothing here changes the HELLO or a verdict. */
static void content_events(uint64_t digest, uint32_t wok) {
    for (int p = 0; p < 2; p++) {
        const RboCdPl *d = &rbo_cdiag.pl[p];
        LONG64 q; RboEv *e = ev_begin(EV_CONTENT, &q);
        if (!e) continue;
        e->u[0] = (uint32_t)p; e->u[1] = B.gt; e->u[2] = B.round; e->u[3] = d->read; e->u[4] = d->tree_sz; e->u[5] = d->tree_n; e->u[6] = d->tree_ok;
        for (int k = 0; k < 3; k++) { e->u[7 + k] = d->kn[k]; e->u[10 + k] = d->kabs[k]; e->u[13 + k] = d->list_ok[k]; }
        e->u[16] = rbo_cdiag.toggle; e->u[17] = rbo_cdiag.toggle_ok; e->u[18] = rbo_cdiag.captured; e->u[19] = rbo_ctx_acc.samples;
        e->q[0] = d->tree_h;
        for (int k = 0; k < 3; k++) { e->q[1 + k] = d->kvt[k]; e->q[4 + k] = d->kspan[k]; e->q[7 + k] = d->kreb[k]; }
        e->q[10] = d->auth;
        {   uint64_t *x = (uint64_t *)(void *)e->s;   /* v0.29.9: ka= (the HELLO digest's list parts), v8= (v0.29.8's digest); s is 8-aligned, no CRT call */
            x[0] = d->ka[0]; x[1] = d->ka[1]; x[2] = d->ka[2]; x[3] = rbo_cdiag.digest_v8; }
        ev_end(q);
    }
    if (!rbo_cdiag.captured) return;
    LONG64 q; RboEv *e = ev_begin(EV_CDUMP, &q);
    if (!e) { InterlockedExchange(&rbo_cdiag.busy, 0); return; }   /* no event, no file: the next battle may capture again */
    e->q[0] = digest; e->u[0] = wok; e->u[1] = B.gt; e->u[2] = rbo_cdiag.toggle; e->u[3] = rbo_cdiag.toggle_ok;
    e->q[1] = rbo_cdiag.digest_v8; e->q[2] = rbo_cdiag.digest_v9;   /* v0.29.9: the H line's digest_v8= / digest_v9= */
    for (int p = 0; p < 2; p++) {   /* the P line's values travel with the event (a later rbo_read_content rewrites rbo_cdiag.pl, never the capture) */
        const RboCdPl *d = &rbo_cdiag.pl[p]; uint32_t *u = &e->u[4 + 10 * p];
        u[0] = d->read; u[1] = d->tree_sz; u[2] = d->tree_n; u[3] = d->tree_ok;
        for (int k = 0; k < 3; k++) { u[4 + k] = d->kn[k]; u[7 + k] = d->list_ok[k]; }
    }
    ev_end(q);
}

/* ---------------------------------------------------------------- HELLO */
static void hello_build(void) {
    RbmHello *h = &B.mine;
    uint8_t *p = (uint8_t *)h; for (size_t i = 0; i < sizeof *h; i++) p[i] = 0;
    h->proto = RBM_PROTO;
    h->flags = (uint16_t)((g_cfg.rbonline_ranked ? RBM_HF_RANKED : 0) | (g_cfg.rbonline_tsync ? RBM_HF_TSYNC : 0));   /* v0.30.0: + the tsync flag (never compared) */
    {   int dad = g_dadapt;   /* v0.33.0 (§22.2): the delay rule (compared) and this PC's RTT estimate (the game's +0x4408 at the last tick, 0 = unknown) */
#ifdef SC6RB_HARNESS
        if (g_h_dadapt_old) dad = 0;
#endif
        if (dad) h->flags |= RBM_HF_DADAPT;
        /* v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix: advertised when argc_fix=1 and the four sites were the verified stock jcc at install
         * (g_argc_cap), and no restore was ever refused in this process (compared: RBM_V_ARGC) */
        int argc = g_argc_cap && !g_argc_broken;
#ifdef SC6RB_HARNESS
        if (g_h_argc_noflag) argc = 0;
#endif
        if (argc) h->flags |= RBM_HF_ARGC;
        float rtt; uint32_t bits = g_badge_rtt_bits; memcpy(&rtt, &bits, 4);
        h->rtt_ms = (uint16_t)(dad && bits && rtt > 0.0f && rtt < 60000.0f ? (int)(rtt + 0.5f) : 0); }
    scpy(h->plugin_version, sizeof h->plugin_version, SC6RB_VERSION);
#ifdef SC6RB_HARNESS
    if (g_h_hello_ver_inject) scpy(h->plugin_version, sizeof h->plugin_version, "x\nRB CHECK ok b=99 f=9");   /* HELLOVER: a peer forging a log line */
    h->proto = (uint16_t)rbm_h_proto;   /* v0.29.6: PROTO4's proto-3 peer (src/rbomsg.c; RBM_PROTO unless h_rbo_proto is set) */
#endif
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    h->exe_ts = nt->FileHeader.TimeDateStamp; h->exe_soi = nt->OptionalHeader.SizeOfImage;
    uint32_t wok = 0;
    rbo_cd_capture = 1;   /* v0.29.8: the diagnosis captures this battle's content (rbo_body.c); v0.29.9: the digest is the base-independent one */
    rbo_read_content(&h->content, h->content_n, &wok);
    rbo_cd_capture = 0;
    uint32_t xor3 = 0;
#ifdef SC6RB_HARNESS
    xor3 = (uint32_t)g_cfg.h_rbo_hello_ctx_xor;
#endif
    rbo_ctx_fill(h, &rbo_ctx_acc, xor3);
    h->d = (uint8_t)g_cfg.rbonline_d; h->max_rb = (uint8_t)rbo_max_rb; h->check_every = (uint16_t)g_cfg.rbonline_check_every;   /* v0.33.0: the configured base (compared) */
    h->arm_round_min = (uint32_t)g_cfg.rbonline_arm_round;
    {   LONG64 q; RboEv *e = ev_begin(EV_CTX, &q);
        if (e) {
            e->u[0] = h->ctx_samples; for (int i = 0; i < 10; i++) e->u[1 + i] = h->ctx_xmm[i];
            e->u[11] = h->ctx_mxcsr; e->u[12] = h->ctx_fcw; e->u[13] = h->ctx_upper_zero; e->u[14] = h->ctx_const_ok;
            e->u[15] = (uint32_t)rbo_ctx_builtin_eq(h, rbo_l3s_ctx.xmm_lo, rbo_l3s_ctx.mxcsr, rbo_l3s_ctx.fcw);
            e->u[16] = h->content_n[0]; e->u[17] = h->content_n[1]; e->u[18] = wok; e->q[0] = h->content;
            ev_end(q);
        }
    }
    content_events(h->content, wok);   /* v0.29.8: RB CONTENT p=0 / p=1 after RB CTX, then the dump file */
}
/* v0.29.1 (review M2): the compiled L3-stock constants (never the runtime rbo_l3s_ctx, which a harness plant may change) */
static const uint32_t k_l3s_builtin[16] = { 0, 0, 0, 0, 0, 0, RBO_L3S_XMM6, RBO_L3S_XMM7, RBO_L3S_XMM8, RBO_L3S_XMM9, 0, 0, 0, RBO_L3S_XMM13, 0, 0 };
static void hello_verdict_send(void) {
    if (!B.hello_sent || !B.have_peer_hello || B.have_vmine) return;
    B.vmine = rbo_verdict(&B.mine, &B.peer) | rbo_verdict_builtin(&B.mine, &B.peer, k_l3s_builtin, RBO_L3S_MXCSR, RBO_L3S_FCW); B.have_vmine = 1;
    RbmMsg m; msg_zero(&m, RBM_HELLO_ACK);
    m.u.hack.hello_nonce = B.peer_hello_nonce;   /* the answered HELLO's nonce_tx (p2p.c: P2pEvent.detail of the MSG; M2-INTERFACES.md TR-1) */
#ifdef SC6RB_HARNESS
    if (g_h_hack_nonce0) m.u.hack.hello_nonce = 0;   /* HACK0: a HELLO_ACK that names no HELLO */
#endif
    m.u.hack.verdict = B.vmine;
    send_msg(RBM_HELLO_ACK, &m);
}
static void on_peer_arm(uint32_t r_peer);
/* v0.33.0 (OPTA-M2-SPEC.md §22.2): the delay of this battle, from the SAME inputs on both PCs (the two HELLOs, equal flags: the verdict compares them) */
static void d_compute(void) {
    int adapt = (B.mine.flags & RBM_HF_DADAPT) != 0;
    int d = rbo_d_adapt(adapt, B.mine.rtt_ms, B.peer.rtt_ms, g_cfg.rbonline_d, rbo_max_rb);
#ifdef SC6RB_HARNESS
    if (g_h_dadapt_plant) d += 1;   /* DDIS: a planted disagreement */
#endif
    B.d_arm = d; B.d_set = 1; rbo_d = d;
    rbo_log("RB DELAY b=%u d=%d adapt=%d rtt_mine=%u rtt_peer=%u d_base=%d max_rb=%d (computed from the two HELLOs; the ARM carries it and both must agree)", B.b, d, adapt,
            (unsigned)B.mine.rtt_ms, (unsigned)B.peer.rtt_ms, g_cfg.rbonline_d, rbo_max_rb);
}
/* the peer's ARM / ARM_ACK delay must be the one this PC computed: else never arm (both PCs see the other's ARM: symmetric), loud */
static int arm_d_ok(uint32_t d_peer) {
#ifdef SC6RB_HARNESS
    if (g_h_darm_nocheck) return 1;   /* XDDIS: v0.32.0 never compared it */
#endif
    if (!B.d_set || (int)d_peer == B.d_arm) return 1;
    if (!B.darm_refused) {
        B.darm_refused = 1;
        rbo_log("RB ARM b=%u REFUSED why=d_disagree mine=%d peer=%u (both PCs compute the delay from the two HELLOs: a different value is a bug or a mixed build; "
                "this battle stays on the stock netcode)", B.b, B.d_arm, (unsigned)d_peer);
    }
    if (rbo_st == RBO_PAIRED || rbo_st == RBO_PROBE) sm_set(RBO_PASSIVE, RBW_DARM);
    rbo_d = g_cfg.rbonline_d;
    return 0;
}
static void hello_outcome(void) {
    if (!B.have_vmine || !B.have_vpeer || B.hello_logged) return;
    B.hello_logged = 1; B.t_outcome = ms_now();
    {   LONG64 p; RboEv *e = ev_begin(EV_HELLO, &p);
        if (e) {
            e->u[0] = (uint32_t)B.hello_sent; e->u[1] = 1;
            e->u[2] = !(B.vmine & RBM_V_CONTENT); e->u[3] = !(B.vmine & (RBM_V_CTX | RBM_V_NOCTX | RBM_V_BUILTIN)); e->u[4] = !(B.vmine & RBM_V_CFG);
            e->u[5] = B.vmine; e->u[6] = B.vpeer;
            e->u[7] = (B.mine.flags & RBM_HF_TSYNC) ? 1u : 0u; e->u[8] = (B.peer.flags & RBM_HF_TSYNC) ? 1u : 0u;   /* v0.30.0: tsync= peer_tsync= */
            e->u[9] = (B.mine.flags & RBM_HF_ARGC) ? 1u : 0u; e->u[10] = (B.peer.flags & RBM_HF_ARGC) ? 1u : 0u;   /* v0.34.0: argc= peer_argc= */
            /* v0.29.1 (review m3 a): the peer's version string is the peer's bytes: anything outside printable ASCII (a newline that would
             * forge an RB line, a control byte) is logged as '?', and a space too (it would split the key=value field) */
            for (int i = 0; i < 24; i++) { char ch = B.peer.plugin_version[i]; if (!ch) { e->s[i] = 0; break; } e->s[i] = (ch > 0x20 && ch < 0x7f) ? ch : '?'; }
            e->s[24] = 0;
#ifdef SC6RB_HARNESS
            if (g_h_ver_noescape) scpy(e->s, 25, B.peer.plugin_version);   /* XHELLOVER: the v0.29.0 copy */
#endif
            ev_end(p);
        }
    }
    if (rbo_st != RBO_PROBE) return;
    if (!B.vmine && !B.vpeer) {
        B.argc_on = rbo_argc_on(&B.mine, &B.peer);   /* v0.34.0 (§23.1): both advertised the fix (equal flags: the verdict compared them) */
#ifdef SC6RB_HARNESS
        if (g_h_argc_old) B.argc_on = cfg_geti("argc_fix", 1) == 1 && g_argc_cap;   /* XARGCOFF: no handshake (this PC's own key alone) */
#endif
        sm_set(RBO_PAIRED, RBW_PAIRED);
        d_compute();                   /* v0.33.0 (§22.2): before the first ARM (it carries the delay) */
        B.r_proposed = r_mine_now();   /* v0.30.0: the game's index (0 = the first round) */
        send_arm(RBM_ARM, B.r_proposed);
        if (B.early_arm) { B.early_arm = 0; if (arm_d_ok(B.early_arm_d)) on_peer_arm(B.early_arm_r); }   /* the peer paired first: its ARM came while we were in PROBE */
    } else sm_set(RBO_PASSIVE, rbo_refuse_why(B.vmine, B.vpeer));   /* v0.34.0 (§23.1): why=argc_off when the argc flag is the only difference. The channel stays open to the battle's END: my HELLO_ACK (the peer's verdict input) is resent until acked */
}

/* ---------------------------------------------------------------- CHECK / DESYNC (armed) */
static void desync(uint32_t f, uint32_t round, uint32_t gt, uint64_t mine, uint64_t theirs, int send, const char *why) {
    if (B.desync) return;
    B.desync = 1;
    if (send) { RbmMsg m; msg_zero(&m, RBM_DESYNC); m.u.desync.round = (uint8_t)round; m.u.desync.gt = gt; m.u.desync.mine = mine; m.u.desync.theirs = theirs; send_msg(RBM_DESYNC, &m); }
    char dump[MAX_PATH] = "-";
    if (g_cfg.rbonline_dump) {
        size_t L = strlen(g_log_path);
        snprintf(dump, sizeof dump, "%.*s_rbo_desync_b%u_f%u.txt", (int)(L > 4 ? L - 4 : L), g_log_path, B.b, f);
        rbo_core_dump_frame(dump, f);
    }
    /* v0.29.1 (review M4): a DESYNC means the two games may already differ (the drain keeps the game running, it cannot re-merge them):
     * the title says rb=DESYNC:diverged and this line says the match's result is not shared */
    rbo_log("RB DESYNC b=%u f=%u r=%u gt=%u mine=0x%016llx theirs=0x%016llx why=%s dump=%s (the two games may differ from here: this match's result "
            "is not shared; abandon it)", B.b, f, round, gt, (unsigned long long)mine, (unsigned long long)theirs, why, dump);
    if (!B.ck_ending) rbo_drain(RBW_DESYNC);   /* review fix M1: a mismatch found by the END flush is logged and sent; the battle is ending (no drain) */
}
/* v0.29.1 (review B2): a desync this PC found itself without a CHECK (a committed misprediction after the core aborted) */
void rbo_desync_local(uint32_t f, uint64_t mine, uint64_t theirs, const char *why) {
    if (rbo_st != RBO_ARMED && rbo_st != RBO_DRAINED) return;
    RboFrame *fr = rbo_frame(f);
    desync(f, fr ? fr->round : B.round, fr ? fr->ff : B.gt, mine, theirs, 1, why);
}
/* v0.32.0 (OPTA-M2-SPEC.md §21.1): a chain mismatch names its frames: this PC's per-frame CHECK hashes of the window, 8 per line (the other PC
 * logs the same lines: the first frame whose hashes differ is the divergence; the light dump has the components of the last 64 frames) */
/* review fix M1: the chain's own frames [f0, f] (a carried chain can start before f - E + 1); end=1 = the END flush's lines (always logged, so the two
 * PCs' logs name a divergence in the battle's last frames even when neither side was still armed to compare the other's final CHECK) */
static void chain_name_frames(uint32_t f, uint32_t f0, uint32_t n, int end) {
    uint32_t E = (uint32_t)g_cfg.rbonline_check_every;
    if (!f0 || f0 > f) f0 = f >= E ? f - E + 1 : 1;
    if (f - f0 >= CKF_N) f0 = f - CKF_N + 1;
    char ln[480]; int o = 0, k = 0;
    for (uint32_t x = f0; x <= f; x++) {
        if (B.ckf[x % CKF_N].f != x) continue;   /* a zone frame (never checked on this PC) */
        if (!k) o = snprintf(ln, sizeof ln, "RB CHAINF b=%u f=%u n=%u%s h=", B.b, f, n, end ? " end=1" : "");
        o += snprintf(ln + o, sizeof ln - (size_t)o, "%s%u:%016llx", k ? "," : "", x, (unsigned long long)B.ckf[x % CKF_N].h);
        if (++k == 8) { rbo_log("%s", ln); k = 0; }
    }
    if (k) rbo_log("%s", ln);
}
static void check_compare(uint32_t f) {
    ChkEnt *a = &B.mine_ck[f % CHK_N], *b = &B.peer_ck[f % CHK_N];
    if (!a->have || !b->have || a->f != f || b->f != f) return;
    a->have = 0; b->have = 0;
    RboCk ca = { a->f, a->round, a->gt, a->n, a->fset, a->hash, a->chain }, cb = { b->f, b->round, b->gt, b->n, b->fset, b->hash, b->chain };
    int cmp = 0, vd = rbo_check_verdict(&ca, &cb, &cmp);
    int ok = vd == RBO_CK_OK;
    B.checks++; if (ok) B.checks_ok++;
    if (cmp) B.chain_cmp++; else if (a->n || b->n) B.chain_skips++;
    if (vd == RBO_CK_CHAIN) B.chain_mm++;
    if (f > B.last_check_f) B.last_check_f = f;
    const char *cw = cmp ? (vd == RBO_CK_CHAIN ? "MISMATCH" : "ok") : (a->n || b->n) ? "skip" : "-";
    if (!ok || B.checks_lines < 10 || (!cmp && (a->n || b->n) && B.chain_lines < 10)) {
        B.checks_lines++; if (!cmp && (a->n || b->n)) B.chain_lines++;
        rbo_log("RB CHECK b=%u f=%u r=%u gt=%u %s mine=0x%016llx peer=0x%016llx chain=%s n=%u peer_n=%u chain_mine=0x%016llx chain_peer=0x%016llx", B.b, f, a->round, a->gt,
                ok ? "ok" : "MISMATCH", (unsigned long long)a->hash, (unsigned long long)b->hash, cw, a->n, b->n, (unsigned long long)a->chain, (unsigned long long)b->chain);
    }
    if (!ok) {
        if (vd == RBO_CK_CHAIN && !B.ck_ending) chain_name_frames(f, a->f0, a->n, 0);   /* the END flush logs its frames itself (end=1) */
        if (vd == RBO_CK_CHAIN) desync(f, a->round, a->gt, a->chain, b->chain, 1, "chain_mismatch");
        else desync(f, a->round, a->gt, a->hash, b->hash, 1, "check_mismatch");
    }
}
/* the CHECK rule in force: 0 = this build's; harness controls only: 1 = v0.31.0's (frame f's hash only: XCHAIN, PROTO8), 2 = the pre-review v0.32.0
 * window rule (the chain reset at every new window and sent at the due frame only: XCHZONE, XCHEND) */
static int chk_old(void) {
#ifdef SC6RB_HARNESS
    return g_h_chk_old;
#else
    return 0;
#endif
}
/* one CHECK of this PC: its entry (f0 = the chain's first frame, for RB CHAINF), the message, the compare */
static void ck_send(uint32_t f, uint32_t round, uint32_t gt, uint64_t hash, uint32_t n, uint32_t fset, uint64_t chain, uint32_t f0) {
    ChkEnt *a = &B.mine_ck[f % CHK_N]; a->f = f; a->round = round; a->gt = gt; a->hash = hash; a->have = 1;
    a->n = n; a->fset = fset; a->chain = chain; a->f0 = f0;
    RbmMsg m; msg_zero(&m, RBM_CHECK); m.u.check.round = (uint8_t)round; m.u.check.gt = gt; m.u.check.f = f; m.u.check.hash = hash;
    m.u.check.n = a->n; m.u.check.fset = a->fset; m.u.check.chain = a->chain;
    send_msg(RBM_CHECK, &m);
    check_compare(f);
}
/* review fix M1 (OPTA-M2-SPEC.md §21.12): the open chain's frames [ck_f0, f], read back from the per-frame ring, sent as one CHECK keyed at f (f is a
 * frame this PC checked: frame f's own hash, round and gt as before); frames after f (a mirror inside the open chain) stay open */
static void ck_flush(uint32_t f, int why) {
    uint32_t f0 = B.ck_f0, n = 0, fset = RBO_FSET_BASIS, rest = 0;
    uint64_t chain = RBO_CHAIN_BASIS;
    if (!f0 || f0 > f) f0 = f;
    if (f - f0 >= CKF_N) f0 = f - CKF_N + 1;   /* never with check_every <= 600: a chain restarts at every due frame */
    for (uint32_t x = f0; x <= f; x++) {
        if (B.ckf[x % CKF_N].f != x) continue;   /* a frame this PC did not check (a zone frame) */
        chain = rbo_chain_step(chain, x, B.ckf[x % CKF_N].h); fset = rbo_fset_step(fset, x); n++;
    }
    for (uint32_t x = f + 1; x <= B.ck_last; x++) if (B.ckf[x % CKF_N].f == x) rest++;
    B.ck_f0 = f + 1; B.ck_n = rest;
    if (why == CKW_ZONE) B.chain_zone++; else if (why == CKW_END) B.chain_end++; else if (why == CKW_MIRROR) B.chain_mirror++;
    ck_send(f, B.ckf[f % CKF_N].round, B.ckf[f % CKF_N].gt, B.ckf[f % CKF_N].h, n, fset, chain, f0);
}
/* rbo_core.c: every confirmed armed frame f (outside a zone) with its tier-1-minus-D6 hash. v0.32.0: every frame goes into the chain; the CHECK at
 * f % check_every == 0 carries frame f's hash (v0.31.0) and the chain (RBM_PROTO 8). Review fix M1: the chain holds every frame since this PC's
 * last CHECK (it is never dropped at a window whose due frame is a zone frame); a zone entry and END send it too (rbo_check_zone, rbo_end), and a
 * peer's CHECK at a frame that is not due here (its flush) makes this PC send its own chain at the same frame (the mirror) */
void rbo_check_local(uint32_t f, uint32_t round, uint32_t gt, uint64_t hash) {
    if (rbo_st != RBO_ARMED && rbo_st != RBO_DRAINED) return;
    uint32_t E = (uint32_t)g_cfg.rbonline_check_every;
    if (!f || !E) return;
    B.ckf[f % CKF_N].f = f; B.ckf[f % CKF_N].h = hash; B.ckf[f % CKF_N].round = round; B.ckf[f % CKF_N].gt = gt;
    int old = chk_old();
    if (old == 1) { if (rbo_check_due(f, E)) ck_send(f, round, gt, hash, 0, 0, 0, f); return; }
    if (old == 2) {
        uint32_t w = rbo_chain_window(f, E);
        if (!B.ck_w_set || w != B.ck_w) { B.ck_w = w; B.ck_w_set = 1; B.ck_chain = RBO_CHAIN_BASIS; B.ck_n = 0; B.ck_fset = RBO_FSET_BASIS; B.ck_f0 = f; }
        B.ck_chain = rbo_chain_step(B.ck_chain, f, hash); B.ck_n++; B.ck_fset = rbo_fset_step(B.ck_fset, f);
        if (rbo_check_due(f, E)) ck_send(f, round, gt, hash, B.ck_n, B.ck_fset, B.ck_chain, B.ck_f0);
        return;
    }
    if (!B.ck_n) B.ck_f0 = f;
    B.ck_n++; B.ck_last = f;
    if (rbo_check_due(f, E)) { ck_flush(f, CKW_DUE); return; }
    {   ChkEnt *b = &B.peer_ck[f % CHK_N];
        if (b->have && b->f == f) ck_flush(f, CKW_MIRROR); }   /* the peer's flush at f arrived before this PC confirmed f */
}
/* review fix M1: a confirmed zone frame (rb.c; never checked, D6). The open chain is sent at its last frame before the zone: both PCs do it at the
 * same frame (zone membership comes from confirmed state); a frame set that still differs falls back to v0.31.0's rule (chain=skip) */
void rbo_check_zone(uint32_t f) {
    if (rbo_st != RBO_ARMED && rbo_st != RBO_DRAINED) return;
    if (chk_old() || !B.ck_n || f <= B.ck_last) return;
    ck_flush(B.ck_last, CKW_ZONE);
}

/* ---------------------------------------------------------------- DRAIN (spec §6) */
void rbo_drain(int why) {
    if (rbo_st != RBO_ARMED) return;
    B.drained_why = why;
    rbo_fetch_mode = 1;   /* stop predicting where the stall rule allows it; FetchInput stays ours (confirmed-only form) until END */
    sm_set(RBO_DRAINED, why);
    lag_restore();   /* v0.30.0 (§19.2) */
    B.ts_debt = 0; B.ts_hold = 0;   /* v0.30.0 (§19.4): no lift, no wait once drained */
    B.drain_f = rbo_core_last_f(); B.drain_r = B.round; B.drain_gt = B.gt; B.drain_unc = rbo_fetch_unconfirmed(); B.drain_t0 = ms_now();
    RbmMsg m; msg_zero(&m, RBM_DRAIN); m.u.drain.reason = (uint8_t)why; m.u.drain.round = (uint8_t)B.round; m.u.drain.gt = B.gt; send_msg(RBM_DRAIN, &m);
}
static void drain_progress(void) {   /* Tick pre while DRAINED: the run-out finished -> the repair -> RB DRAIN */
    if (B.drain_logged) return;
    uint32_t unc = rbo_fetch_unconfirmed();
    int timeout = ms_now() - B.drain_t0 > 5000;
    if (unc && !timeout) return;
    int rep = !unc && rbo_fetch_repair();
    if (!rep && !timeout) return;   /* the last fetch (the one-tick IsPaused lag frame) is not confirmed yet: next tick */
    B.drain_logged = 1;
    rbo_log("RB DRAIN b=%u why=%s f=%u r=%u gt=%u unconfirmed=%u repaired=%d lag_restored=%d ctx=l3stock peer_drained=%d", B.b, rbw_nm(B.drained_why),
            B.drain_f, B.drain_r, B.drain_gt, unc ? unc : B.drain_unc, rep, B.lag_restored, B.peer_drained);
}

/* ---------------------------------------------------------------- v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix of an armed battle
 * Applied exactly where this PC's step context switches to L3-stock at the agreed ARM (D-C: both PCs at round r's 1 -> 2, the same frame; the
 * write runs in the IsPaused pre hook body on the game thread, between two steps, so no move script runs while the four bytes change), or at a
 * degraded arm (the same switch). Never while PASSIVE / PROBE / PAIRED, never changed back mid-battle (a drain keeps it, as it keeps the context);
 * restored at the battle's END (the context goes back to stock there too). B.argc_on = both HELLOs advertised it. Returns 0 only when the apply was
 * tried and failed (do_arm then degrades: the peer is told, rb=ASYM). */
static int argc_arm(uint32_t r) {
    if (!B.argc_on) return 1;
    if (B.argc_tried) return B.argc_applied;
    B.argc_tried = 1;
#ifdef SC6RB_HARNESS
    if (g_h_argc_noapply) { rbo_log("RB ARGCARM b=%u r=%u harness control h_rbo_argc_noapply=1: the agreed fix is NOT applied (XARGC: the bytes stay stock while armed)", B.b, r); return 1; }
#endif
    ArgcRes res; argc_apply(g_base, &res);
    B.argc_apply_why = res.why; for (int i = 0; i < ARGC_NSITES; i++) B.argc_st_arm[i] = res.st[i];
    char ss[64]; argc_states_str(res.st, ss, sizeof ss);
    if (res.ok) B.argc_applied = 1;
    rbo_log("RB ARGCARM b=%u r=%u APPLY %s why=%s sites=%s written=%d rolled_back=%d first_bad=%d (%s)", B.b, r, res.ok ? "ok" : "FAIL", k_argc_why[res.why], ss, res.written,
            res.rolled_back, res.first_bad, res.ok ? "sub-ops 0x25 / 0x91 stop at args[argc-1] from this frame to the battle's END, on both PCs" :
            "nothing changed: this PC cannot run the agreed fix, so it degrades and tells the peer (rb=ASYM)");
    return res.ok;
}
static void argc_end_restore(void) {
    if (!B.argc_applied || B.argc_restored) return;
    ArgcRes res; argc_restore(g_base, &res);
    B.argc_restore_why = res.why;
    B.argc_restored = res.ok;
    char ss[64]; argc_states_str(res.st, ss, sizeof ss);
    if (!res.ok) InterlockedExchange((volatile LONG *)&g_argc_broken, 1);
    rbo_log("RB ARGCARM b=%u RESTORE %s why=%s sites=%s written=%d%s", B.b, res.ok ? "ok" : "FAIL", k_argc_why[res.why], ss, res.written,
            res.ok ? "" : " (the bytes are not as this plugin left them: never advertised again in this process)");
}
enum { AW_AGREED = 0, AW_NOT_PAIRED, AW_ARGC_OFF, AW_MISMATCH, AW_BOTH_OFF, AW_NOT_ARMED, AW_APPLY_FAILED, AW_NOAPPLY, AW_OWN_KEY, AW_N };
static const char *const k_aw[AW_N] = { "agreed", "not_paired", "argc_off", "mismatch", "both_off", "not_armed", "apply_failed", "harness_noapply", "own_key" };
/* the per-battle line (passive-safe: plugin memory, 24 bytes of the image when this PC verified them readable at install, one structured event; an
 * armed battle's is written directly, in order with the core's lines) */
static void argc_line(void) {
    RboEv ev, *e = &ev; LONG64 pos = 0;
    if (!B.armed_ever) { e = ev_begin(EV_ARGC, &pos); if (!e) return; }
    else { uint8_t *z = (uint8_t *)&ev; for (size_t i = 0; i < sizeof ev; i++) z[i] = 0; ev.type = EV_ARGC; ev.b = nobs_bat; }
    int why;
    if (B.argc_applied) why = AW_AGREED;
    else if (B.argc_tried) why = AW_APPLY_FAILED;
    else if (B.argc_on) why = AW_NOT_ARMED;
    else if (B.st_max >= RBO_PAIRED) why = AW_BOTH_OFF;
    else if (B.have_vmine && B.have_vpeer) why = rbo_refuse_why(B.vmine, B.vpeer) == RBW_ARGC ? AW_ARGC_OFF : AW_MISMATCH;
    else why = AW_NOT_PAIRED;
#ifdef SC6RB_HARNESS
    if (B.argc_tried && g_h_argc_noapply) why = AW_NOAPPLY;
    if (why == AW_AGREED && g_h_argc_old) why = AW_OWN_KEY;
#endif
    e->u[0] = (uint32_t)(B.argc_applied ? 1 : 0); e->u[1] = (uint32_t)why;
    e->u[2] = B.hello_sent ? ((B.mine.flags & RBM_HF_ARGC) ? 1u : 0u) : 2u; e->u[3] = B.have_peer_hello ? ((B.peer.flags & RBM_HF_ARGC) ? 1u : 0u) : 2u;
    e->u[4] = (uint32_t)B.argc_applied; e->u[5] = (uint32_t)B.argc_restored;
    for (int i = 0; i < ARGC_NSITES; i++) {
        e->u[6 + i] = B.argc_tried ? (uint32_t)B.argc_st_arm[i] : 9u;
        e->u[10 + i] = (uint32_t)argc_site_state_raw(g_base, i);   /* the four sites now (pure reads of a committed .text page; the passive stock-bytes check) */
    }
    e->u[14] = (uint32_t)B.argc_apply_why; e->u[15] = (uint32_t)B.argc_restore_why; e->u[16] = (uint32_t)g_argc_cap; e->u[17] = (uint32_t)g_argc_broken;
    if (!B.armed_ever) ev_end(pos);
    else { ring_sync(); ev_format(&ev); }
}

/* ---------------------------------------------------------------- ARM (IsPaused pre, round r's 1 -> 2) */
/* v0.29.1 (review M2): the degraded arm. The two PCs agreed on ARM(r) (or this side learned the peer armed at r) but this side cannot run the
 * core. D-C: once a battle arms, every remaining frame runs in L3-stock on BOTH PCs; so this side still switches its step to the L3-stock
 * live pass (rbo_step_ctxonly: no core, no prediction, no FetchInput change, no InputLag pin), tells the peer (DRAIN reason arm_fail: the
 * peer drains, its context stays L3-stock too) and both titles say rb=ASYM: the battle is not under rollback and nothing cross-checks it. */
static void degrade(uint32_t r, int why, const char *kind, const char *detail) {
    (void)argc_arm(r);   /* v0.34.0 (§23.1): the degraded arm switches the context too, so the agreed fix runs here as on the armed peer (a no-op when tried) */
    rbo_unexpected(kind, detail);
    B.asym = 1; B.armed_round = r; rbo_armed_round = r; B.armed_ever = 1;
#ifdef SC6RB_HARNESS
    if (g_h_no_asym_fix) { rbo_log("RB ARM b=%u r=%u FAILED (harness control h_rbo_no_asym_fix: no degraded arm, the peer is not told)", B.b, r); B.asym = 0; return; }
#endif
    B.ctxonly = 1;
    rbo_core_degraded_begin();   /* v0.29.5 RBO (F1): the degraded pass reverts UE's writes (L4) now: lip-sync keeps the faces, as on the armed peer */
    sm_set(RBO_DRAINED, why);
    B.drained_why = why; B.drain_logged = 1;
    RbmMsg m; msg_zero(&m, RBM_DRAIN); m.u.drain.reason = (uint8_t)RBW_ARM_FAIL; m.u.drain.round = (uint8_t)B.round; m.u.drain.gt = B.gt;
#ifdef SC6RB_HARNESS
    if (g_h_drop_armfail) rbo_log("RB ARM b=%u r=%u harness plant h_rbo_drop_armfail_drain=1: the DRAIN(arm_fail) is not handed to p2p (a lost sequenced DRAIN)", B.b, r);
    else
#endif
    send_msg(RBM_DRAIN, &m);
    {   const char *q1 = !strcmp(kind, "arm_core") ? rbo_core_q1_fail() : NULL;   /* v0.29.2 (§16.6): the ARM failed in the Q1 root walk */
        rbo_log("RB ARM b=%u r=%u DEGRADED why=%s (agreed but not armed here: every remaining step runs the L3-stock live pass without the core; rb=ASYM)%s%s",
                B.b, r, rbw_nm(why), q1 ? " q1=" : "", q1 ? q1 : ""); }
}
static void do_arm(const RboObs *o) {
    uint32_t r = B.arm_pending;
    B.arm_pending = 0; B.arm_pending_set = 0;
    if (o->onlinesync != 1 || o->state != 2 || o->round != r) {
        /* not round r's 1 -> 2 any more: the context cannot switch at the agreed frame; the peer is told (it drains, rb=ASYM on both) */
        rbo_unexpected("arm_recheck", "onlinesync_state_or_round");
        B.asym = 1;
        RbmMsg m; msg_zero(&m, RBM_DRAIN); m.u.drain.reason = (uint8_t)RBW_ARM_FAIL; m.u.drain.round = (uint8_t)B.round; m.u.drain.gt = B.gt; send_msg(RBM_DRAIN, &m);
        return;
    }
    /* v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix, at the agreed frame (before the core arms; a failure degrades: rb=ASYM on both) */
    if (!argc_arm(r)) { degrade(r, RBW_ARM_FAIL, "arm_argc", "argc_patch_failed"); return; }
    int remote;
    if (o->local == 1) remote = 1; else if (o->local == 2) remote = 0;
    else { degrade(r, RBW_ARM_FAIL, "arm_local_flags", "local_player_flags_not_1_or_2"); return; }
    if (!B.mgr) { degrade(r, RBW_ARM_FAIL, "arm_mgr", "no_manager_seen"); return; }
    rbo_remote = remote; rbo_obj = o->obj; rbo_mgr = B.mgr;
    g_cfg.net_delay = rbo_d < 4 ? rbo_d : 4;   /* v0.33.0 (§22.2): the C1b core's D = the agreed delay (both PCs: the zone rule uses it); <= 4 = D12 */
    B.core_armed = 1;   /* rbo_core_arm may have begun the core's battle even when it fails later: END closes it */
    if (!rbo_core_arm(B.mgr, o->obj, remote)) { degrade(r, RBW_ARM_FAIL, "arm_core", "core_arm_failed"); return; }
    /* the InputLag pin (spec §5.5). v0.30.0 (OPTA-M2-SPEC.md §19.2): with rbonline_zone_lag_lead > 0 (the default) nothing is written here: the
     * values are saved and the zone lag (lag_tick) pins d only on fight frames (every battle arms inside the battle_start zone, so the first
     * PIN comes at that zone's exit e - 1); rbonline_zone_lag_lead=0 is v0.29.9's pin for the whole battle (the field fallback, XZLAG) */
    B.lag_zl0 = g_cfg.rbonline_zone_lag_lead <= 0;
    if (B.lag_zl0) {
        B.lag_min = rbo_gw32(o->obj + RBO_O_LAGMIN, (uint32_t)rbo_d);
        B.lag_max = rbo_gw32(o->obj + RBO_O_LAGMAX, (uint32_t)rbo_d);
        B.lag_mode = RBO_LAG_PIN; B.lag_cur_min = B.lag_cur_max = (uint32_t)rbo_d;
    } else {
        B.lag_min = o->lagmin; B.lag_max = o->lagmax;
        B.lag_mode = RBO_LAG_STOCK; B.lag_cur_min = o->lagmin; B.lag_cur_max = o->lagmax;
    }
    B.lag_pinned = 1;   /* the lag is ours to hand back from here (drain, END) */
    B.ts_round_set = 0; B.ts_gap = 1000; B.arm_tick = B.ticks;   /* v0.30.0 (§19.4) */
    rbo_fetch_arm(B.mgr, o->obj, remote);
    rbo_fetch_mode = 0;
    InterlockedExchange64((volatile LONG64 *)rbo_fetch_cell, (LONG64)(uintptr_t)rbo_fetch_armed);
    B.fetch_set = 1;
    B.armed_round = r; rbo_armed_round = r; B.armed_ever = 1;
    sm_set(RBO_ARMED, RBW_ARMED);   /* written directly (armed_ever): in order with the core's first lines */
    rbo_log("RB ARM b=%u r=%u d=%d max_rb=%d lag_saved=%u,%u margin_ms=%llu remote=p%d lag=%s zone_lag_lead=%d tsync=%d", B.b, r, rbo_d, rbo_max_rb, B.lag_min, B.lag_max,
            (unsigned long long)B.margin_ms, remote + 1, B.lag_zl0 ? "pin" : "stock", g_cfg.rbonline_zone_lag_lead, g_cfg.rbonline_tsync);
    /* v0.29.5 RBO (F1, design_v0295/rbo/DESIGN.md §3.5): the peer's DRAIN(arm_fail) arrived BEFORE this side's own ARM (the peer reached round r
     * first in wall time; on_msg only drains an ARMED side). This side still switches at r (D-C) but leaves prediction at once, as it does when the
     * DRAIN arrives while armed: both PCs then run the plain canonical pass (no prediction on either), both titles rb=ASYM. v0.29.4 stayed ARMED
     * to END here. h_rbo_asym_early_old=1 (harness only): the v0.29.4 behavior (control XASYMEARLY). */
    int early = B.peer_drained && B.peer_drain_why == RBW_ARM_FAIL;
#ifdef SC6RB_HARNESS
    if (g_h_asym_early_old) early = 0;
#endif
    if (early) { B.asym = 1; rbo_drain(RBW_ASYM); }
}

/* ---------------------------------------------------------------- the side channel's events (Tick pre) */
static void on_peer_arm(uint32_t r_peer) {   /* the peer's ARM(r_peer): r = max(r_mine, r_peer), ARM_ACK(r) (M2-INTERFACES.md §5.3 item 3) */
    uint32_t rm = r_mine_now();
    if (B.r_proposed > rm) rm = B.r_proposed;
    uint32_t r = rbo_ack_round(rm, r_peer);
    if (r < 16 && rbo_st == RBO_PAIRED) {
        B.sent_ack |= 1u << r;
#ifdef SC6RB_HARNESS
        if (g_h_drop_ack0 && r == 0) { /* PEERARMR0's plant: this ARM_ACK(0) never leaves (the peer's half of the r=0 agreement is lost) */ }
        else
#endif
        send_arm(RBM_ARM_ACK, r);
        if (rbo_agreed(B.sent_ack, B.recv_ack, r) && !B.t_agreed[r]) B.t_agreed[r] = ms_now();
    }
}
static void on_msg(const RbmMsg *m, uint32_t sender_nonce) {
    switch (m->type) {
    case RBM_HELLO:
        if (B.have_peer_hello) break;
        B.peer = m->u.hello; B.have_peer_hello = 1; B.peer_hello_nonce = sender_nonce;
        hello_verdict_send(); hello_outcome();
        break;
    case RBM_HELLO_ACK:
        if (B.have_vpeer) break;
        {   int named = m->u.hack.hello_nonce == B.nonce;
#ifdef SC6RB_HARNESS
            if (g_h_hack0_old && !m->u.hack.hello_nonce) named = 1;   /* XHACK0: the v0.29.0 rule */
#endif
            if (!named) break;   /* a verdict on another HELLO of ours (another open), or none named (v0.29.1, review m3 c: hello_nonce 0 was
                                  * accepted for any HELLO): not this one */
        }
        B.vpeer = m->u.hack.verdict; B.have_vpeer = 1;
        hello_outcome();
        break;
    case RBM_ARM:
        /* v0.29.0 fix: the peer pairs as soon as it has BOTH verdicts, so its ARM can reach this side before this side has
         * the peer's HELLO_ACK (still PROBE). p2p delivers each seq once: dropping it here lost the agreement for that round on BOTH sides (each side
         * then held only half of the ARM_ACK pair) and the battle armed one round late. Kept and answered at PAIRED instead (on_peer_arm). */
        if (rbo_st == RBO_PROBE) { if (!B.early_arm || m->u.arm.round > B.early_arm_r) B.early_arm_r = m->u.arm.round; B.early_arm = 1; B.early_arm_d = m->u.arm.d; break; }
        if (rbo_st != RBO_PAIRED && rbo_st != RBO_ARMED && rbo_st != RBO_DRAINED) break;
        if (rbo_st == RBO_PAIRED && !arm_d_ok(m->u.arm.d)) break;   /* v0.33.0 (§22.2) */
        on_peer_arm(m->u.arm.round);
        break;
    case RBM_ARM_ACK:
        if (rbo_st == RBO_PAIRED && !arm_d_ok(m->u.arm.d)) break;   /* v0.33.0 (§22.2) */
        {   uint32_t r = m->u.arm.round;
            if (r < 16) { B.recv_ack |= 1u << r; if (rbo_agreed(B.sent_ack, B.recv_ack, r) && !B.t_agreed[r]) B.t_agreed[r] = ms_now(); } }
        break;
    case RBM_CHECK:
        if (rbo_st == RBO_ARMED || rbo_st == RBO_DRAINED) {
            uint32_t f = m->u.check.f; ChkEnt *b = &B.peer_ck[f % CHK_N];
            b->f = f; b->round = m->u.check.round; b->gt = m->u.check.gt; b->hash = m->u.check.hash; b->have = 1;
            b->n = m->u.check.n; b->fset = m->u.check.fset; b->chain = m->u.check.chain;   /* v0.32.0 (RBM_PROTO 8) */
            /* review fix M1: a CHECK at a frame that is not due here is the peer's flush (its zone entry, its END, its own mirror): when f is in
             * this PC's open chain, this PC sends its chain up to f now (the mirror), so both chains restart at the same frame */
            if (!chk_old() && !rbo_check_due(f, (uint32_t)g_cfg.rbonline_check_every) && B.ck_n && f >= B.ck_f0 && f <= B.ck_last && B.ckf[f % CKF_N].f == f)
                ck_flush(f, CKW_MIRROR);
            else check_compare(f);
        }
        break;
    case RBM_DESYNC:
        if (rbo_st == RBO_ARMED || rbo_st == RBO_DRAINED) {
            desync(rbo_core_last_f(), m->u.desync.round, m->u.desync.gt, m->u.desync.theirs, m->u.desync.mine, 0, "peer_desync");
        }
        break;
    case RBM_DRAIN:   /* the peer drained alone (spec RBO-DRAIN): this side stays armed; RB SUMMARY carries peer_drained= / peer_drain_why= */
        B.peer_drained = 1; B.peer_drain_why = m->u.drain.reason < RBW_N ? m->u.drain.reason : 0;
        if (m->u.drain.reason == RBW_ARM_FAIL) {   /* v0.29.1 (review M2): the peer agreed but could not arm: this side armed alone */
            B.asym = 1;
            if (rbo_st == RBO_ARMED) rbo_drain(RBW_ASYM);
        }
        break;
    case RBM_BYE: B.peer_bye = 1; break;
    case RBM_TSYNC:   /* v0.30.0 (OPTA-M2-SPEC.md §19.4): store only (passive-safe: no LOG, no CRT); read by ts_tick while ARMED */
        B.ts_peer = m->u.tsync; B.ts_have_peer = 1; B.ts_peer_tick = B.ticks; B.ts_rx++;
        break;
    case RBM_BEACON:
        B.have_beacon = 1; B.peer_state = m->u.beacon.state; B.peer_armed_round = m->u.beacon.armed_round; B.peer_round = m->u.beacon.round; B.peer_gt = (int)m->u.beacon.gt;
        {   /* v0.30.0 (§19.3, DESIGN_A §1.4): the peer's round as an ordinal of this battle (its intro's round 9 is -1, before round 0): with
             * armed_round 0, v0.29.9's raw compare drained a symmetric battle on a stale intro beacon (9 > 0). h_rbo_beacon_ord_old: XBCNR0 */
            int32_t ord = rbo_beacon_ord((uint32_t)B.peer_round, B.last_reset);
#ifdef SC6RB_HARNESS
            if (g_h_beacon_ord_old) ord = (int32_t)(uint32_t)B.peer_round;
#endif
            if (rbo_st == RBO_ARMED && B.peer_state != RBO_ARMED && B.peer_state != RBO_DRAINED && B.peer_state != RBO_END &&
                (ord > (int32_t)B.armed_round || (ord == (int32_t)B.armed_round && B.peer_gt >= 120))) {
                B.asym = 1;   /* v0.29.1 (review M2): loud (rb=ASYM), not "drained:asym" */
                rbo_drain(RBW_ASYM);
            }
        }
        /* v0.29.6 (review MINOR 4, OPTA-M2-SPEC.md §18.9): a beacon with state DRAINED and drained_reason arm_fail comes only from a peer that
         * degraded at its own ARM (degrade(), reason arm_fail). It stands in for that peer's sequenced DRAIN(arm_fail), which p2p can lose
         * (tx_window_drops / q_drops under heavy loss) while unsequenced beacons still arrive: this side acts as on the DRAIN itself (the
         * RBM_DRAIN case above), so an armed side drains why=asym (loud, symmetric: rb=ASYM on both) within a beacon period, and a side that has
         * not armed yet drains at its arm frame (do_arm's early path). v0.29.5 ignored such a beacon (the armed side stayed rb=armed with no
         * cross-check to END). h_rbo_beacon_old=1 (harness only): the v0.29.5 rule (control XASYMBCN). */
        {   int bcn = B.peer_state == RBO_DRAINED && m->u.beacon.drained_reason == RBW_ARM_FAIL && !(B.peer_drained && B.peer_drain_why == RBW_ARM_FAIL);
#ifdef SC6RB_HARNESS
            if (g_h_beacon_old) bcn = 0;
#endif
            if (bcn) {
                B.peer_drained = 1; B.peer_drain_why = RBW_ARM_FAIL; B.asym = 1;
                if (rbo_st == RBO_ARMED) {   /* armed: a context that may log (passive paths never do) */
                    rbo_log("RB BEACON b=%u peer=drained why=arm_fail: the peer's DRAIN(arm_fail) was not received; this side drains why=asym", B.b);
                    rbo_drain(RBW_ASYM);
                }
            }
        }
        /* v0.29.1 (review M2, Q3): the reverse view. The peer armed at round r and this side is still PAIRED although it is past r's reset (its
         * half of the agreement never completed: a side-channel blackout during the margin). Degrade now, the closest frame to r it can reach,
         * and say so (rb=ASYM on both): the frames between r's start and now ran in different contexts and nothing cross-checks them. */
        /* v0.30.0 (§19.3, DESIGN_A A5): any round 0..15 the peer armed at whose reset this PC saw (v0.29.9 required >= 1: a peer armed at r=0 was
         * never seen, so this path was dead at r=0). h_rbo_peer_armed_old: XPEERARMR0 */
        {   int seen = B.peer_armed_round >= 0 && B.peer_armed_round < 16 && ((B.reset_mask >> B.peer_armed_round) & 1u);
#ifdef SC6RB_HARNESS
            if (g_h_peer_armed_old && B.peer_armed_round < 1) seen = 0;
#endif
            if (rbo_st == RBO_PAIRED && (B.peer_state == RBO_ARMED || B.peer_state == RBO_DRAINED) && seen &&
                !(B.arm_pending_set && B.arm_pending == (uint32_t)B.peer_armed_round))   /* not about to arm at r itself */
                degrade((uint32_t)B.peer_armed_round, RBW_ASYM, "asym_peer_armed", "the_peer_armed_at_a_round_this_side_passed_unarmed");
        }
        break;
    default: break;
    }
}
static void p2p_events(void) {
    P2pEvent ev;
    for (int n = 0; n < 64 && B.p2p_open && p2p_poll(&ev); n++) {
        switch (ev.kind) {
        case P2P_EV_OPENED: peerid_from_p2p(PID_OK, PST_RAN);   /* v0.29.7 */
            B.opened = 1; { LONG64 p; RboEv *e = ev_begin(EV_P2P, &p); if (e) { e->u[0] = 1; e->u[1] = 0; scpy(e->s, 16, p2p_api_name()); ev_end(p); } } break;
        case P2P_EV_NOSTEAM:
            peerid_from_p2p(PID_NOSTEAM, PST_NONE);   /* v0.29.7 */
            { LONG64 p; RboEv *e = ev_begin(EV_P2P, &p); if (e) { e->u[0] = 0; e->u[1] = (uint32_t)ev.detail; scpy(e->s, 16, p2p_api_name()); ev_end(p); } }
            if (rbo_st == RBO_PROBE) sm_set(RBO_PASSIVE, RBW_NOSTEAM);
            p2p_end(RBW_NOSTEAM);
            break;
        case P2P_EV_FOREIGN: peerid_from_p2p(PID_OK, PST_RAN);   /* v0.29.7: a peer was chosen; then the guard found a foreign datagram */
            if (rbo_st == RBO_PROBE) sm_set(RBO_PASSIVE, RBW_FOREIGN); p2p_end(RBW_FOREIGN); break;
        case P2P_EV_SELF:   /* v0.29.1 (review M1): the session's opponent id is this PC's own SteamID: nothing was sent, never pair */
            peerid_from_p2p(PID_SELF, PST_RAN);   /* v0.29.7: every candidate was this PC's own id */
            if (rbo_st == RBO_PROBE) sm_set(RBO_PASSIVE, RBW_SELF);
            p2p_end(RBW_SELF);
            break;
        case P2P_EV_BADID:   /* v0.29.7 (OPTA-M2-SPEC.md §18.10): no candidate has a Steam P2P session (or two different ones do): nothing was sent */
            peerid_from_p2p(PID_BADID, PST_RAN);
            if (rbo_st == RBO_PROBE) sm_set(RBO_PASSIVE, RBW_BADID);
            p2p_end(RBW_BADID);
            break;
        case P2P_EV_BUDGET:  if (rbo_st == RBO_PROBE) sm_set(RBO_PASSIVE, RBW_NO_ANSWER); p2p_end(RBW_NO_ANSWER); break;
        case P2P_EV_VERIFIED: break;
        case P2P_EV_MSG: on_msg(&ev.msg, (uint32_t)ev.detail); break;
        case P2P_EV_SILENT:
            if (rbo_st == RBO_ARMED) rbo_drain(RBW_PEER_SILENT);
            else if (rbo_st == RBO_PROBE || rbo_st == RBO_PAIRED) { sm_set(RBO_PASSIVE, RBW_NO_ANSWER); p2p_end(RBW_PEER_SILENT); }
            break;
        case P2P_EV_PEER_RESTART:
            if (rbo_st == RBO_ARMED) rbo_drain(RBW_PEER_RESTART);
            else if (rbo_st == RBO_PROBE || rbo_st == RBO_PAIRED) { sm_set(RBO_PASSIVE, RBW_PEER_RESTART); p2p_end(RBW_PEER_RESTART); }
            break;
        default: break;
        }
    }
}

/* ---------------------------------------------------------------- the hook bodies' state machine entries */
void rbo_on_tick(const RboObs *o) {
    if (rbo_st == RBO_END) return;
    B.ticks++;
    if (o->mgr_ok) B.mgr = o->mgr;
    if (o->cls == 2) { B.online = 1; B.round = o->round; B.gt = o->gt; if (o->rtt_bits) g_badge_rtt_bits = o->rtt_bits; }   /* v0.31.0: + the badge's ping */
    /* EndPlay: the InputSync object of the probed battle changed or went offline */
    if (B.obj0 && rbo_st != RBO_PASSIVE && (o->cls != 2 || o->obj != B.obj0 || !o->bonline)) { rbo_end(RBW_ENDPLAY); return; }
    int os = o->cls == 2 ? (int)o->onlinesync : 0;
    if (B.last_os == 1 && os == 0 && (B.online || B.probe_tried)) { rbo_end(RBW_ONLINESYNC0); B.last_os = os; return; }
    if (B.last_os == 0 && os == 1 && rbo_st == RBO_PASSIVE && !B.probe_tried) probe_entry(o);
    B.last_os = os;
    if (B.p2p_open) p2p_events();
    if (rbo_st == RBO_END) return;
    if (rbo_st == RBO_PASSIVE && B.p2p_open && B.hello_logged) {
        /* PROBE -> PASSIVE(mismatch): the channel closes (TR-6: no more reads) once the peer surely has my verdict: nothing unacked AND a grace of
         * max(1 s, 4 resend periods) since the outcome (a HELLO_ACK still waiting in the transport's queue is not "unacked" yet: closing at once
         * flushed it and left the peer PROBE -> PASSIVE no_answer instead of mismatch, found by the core's two-process smoke) */
        uint64_t grace = (uint64_t)(g_cfg.rbonline_resend_ms * 4 > 1000 ? g_cfg.rbonline_resend_ms * 4 : 1000);
        if (ms_now() - B.t_outcome >= grace) { P2pStats s; p2p_stats(&s); if (!s.unacked) p2p_end(RBW_MISMATCH); }
    }
    if (rbo_st == RBO_PROBE && B.opened && !B.hello_sent && rbo_ctx_acc.samples >= 60) {
        rbo_ctx_on = 0;
        hello_build();
        RbmMsg m; msg_zero(&m, RBM_HELLO); m.u.hello = B.mine; send_msg(RBM_HELLO, &m);
        B.hello_sent = 1;
        hello_verdict_send(); hello_outcome();
    }
    if (rbo_st == RBO_PAIRED || rbo_st == RBO_ARMED || rbo_st == RBO_DRAINED) {
        uint64_t now = ms_now();
        if (now - B.last_beacon >= 1000) {
            B.last_beacon = now;
            RbmMsg m; msg_zero(&m, RBM_BEACON);
            m.u.beacon.state = (uint8_t)rbo_st; m.u.beacon.armed_round = (uint8_t)B.armed_round; m.u.beacon.drained_reason = (uint8_t)B.drained_why;
            m.u.beacon.round = (uint8_t)B.round; m.u.beacon.gt = B.gt; m.u.beacon.confirmed_gt = rbo_core_confirmed();
            send_msg(RBM_BEACON, &m);
#ifdef SC6RB_HARNESS
            if (g_h_bcn_replay_intro && B.round == 9 && B.real_resets == 0) { B.h_bcn_intro = m; B.h_bcn_have = 1; }   /* BCNR0: the last intro beacon */
#endif
        }
#ifdef SC6RB_HARNESS
        /* BCNR0 / XBCNR0 plant: the last intro beacon (round 9, PAIRED) once more, 100 ticks after this side's ARM: the stale beacon the side
         * channel can deliver after the peer armed at r=0 (it is not ordered with the game channel) */
        /* v0.33.0 (OPTA-M2-SPEC.md §22.4, n3 = REVIEW_FINISH n3): the beacon is unsequenced (delivered, never resent) and the armed cases' side channel
         * loses 2 % per send: one replay was lost in ~10 % of XBCNR0 runs. Now h_rbo_beacon_replay_n replays (default 3), 30 ticks apart;
         * h_rbo_bcn_drop_first=1 (XBCNR0L) drops the first one as the network would */
        if (g_h_bcn_replay_intro && B.h_bcn_have && B.h_bcn_done < g_h_bcn_replay_n && B.armed_ever && B.ticks - B.arm_tick >= 100u + 30u * (uint32_t)B.h_bcn_done) {
            int drop = g_h_bcn_drop_first && B.h_bcn_done == 0;
            B.h_bcn_done++;
            if (!drop) send_msg(RBM_BEACON, &B.h_bcn_intro);
            rbo_log("RB BEACON b=%u harness plant h_rbo_beacon_replay_intro=1: the last intro beacon (round 9, paired) %s after the ARM (replay %d of %d)", B.b,
                    drop ? "NOT sent (h_rbo_bcn_drop_first: lost)" : "sent again", B.h_bcn_done, g_h_bcn_replay_n);
        }
#endif
    }
    if ((rbo_st == RBO_ARMED || rbo_st == RBO_DRAINED) && !B.ctxonly) rbo_core_tick();   /* v0.29.1: the degraded arm has no core */
    if (rbo_st == RBO_DRAINED && !B.ctxonly) drain_progress();
#ifdef SC6RB_HARNESS
    /* v0.29.1 (review m1 d) ZEROX / GUARDX: a planted RAW write from a passive body (not through rbo_gw*, so rbo_h_game_writes does not see it):
     * RBO-ZERO's region comparison and RBO-GUARD's read-only pages must catch it dynamically */
    if (g_h_passive_write && rbo_st <= RBO_PAIRED && o->obj_ok && B.ticks % 50 == 7) *(volatile uint8_t *)(o->obj + 0x120) ^= 0x5a;
#endif
}

void rbo_on_step(NobsCtx *ctx) {
    uint64_t *tgt = &rbo_hooktab[NOBS_H_IFC_STEP * 4 + 3];
    if (rbo_st == RBO_DRAINED && B.ctxonly) { *tgt = (uint64_t)(uintptr_t)rbo_step_ctxonly; return; }   /* v0.29.1 (M2): the degraded arm */
    if (rbo_st == RBO_ARMED || rbo_st == RBO_DRAINED) {
        /* the tripwire (M2-INTERFACES.md §3.5): the words at Step entry = what rbo_fetch_armed produced (T-M2-5: a UE listener can zero a
         * pressed half between FetchInput and Step; in all live data it never did) */
        uintptr_t in = (uintptr_t)ctx->rdx;
        uint64_t w[2] = { 0, 0 }; int have[2] = { 0, 0 };
        if (in) for (int p = 0; p < 2; p++) { uintptr_t wp = *(volatile uintptr_t *)(in + 8 * (uintptr_t)p); if (wp) { w[p] = *(volatile uint64_t *)wp; have[p] = 1; } }
        int bad = rbo_fetch_tripwire(w, have);
        if (bad && rbo_st == RBO_ARMED) {
            int p = bad - 1; RboFrame *fr = rbo_frame(rbo_core_last_f() + 1);
            rbo_log("RB TRIPWIRE b=%u f=%u p=%d ours=0x%016llx stepin=0x%016llx", B.b, rbo_core_last_f() + 1, p + 1, fr ? (unsigned long long)fr->word[p] : 0ULL, (unsigned long long)w[p]);
            rbo_drain(RBW_TRIPWIRE);
        }
        *tgt = (uint64_t)(uintptr_t)rbo_step_armed;
    } else if (*tgt != rbo_hooktab[NOBS_H_IFC_STEP * 4 + 0] && !B.ended) {
        *tgt = rbo_hooktab[NOBS_H_IFC_STEP * 4 + 0];   /* never left on our step outside ARMED / DRAINED (a battle that ended without END) */
    }
}

/* ---------------------------------------------------------------- v0.30.0 (OPTA-M2-SPEC.md §19.2): the zone lag (IsPaused pre, ARMED)
 * PIN (d/d) iff the core runs and none of the next lead frames [s+1, s+lead] is a zone frame, else STOCK (the values saved at ARM). The zone
 * schedule is a function of the confirmed frames (rb.c zone_confirm), so both PCs switch at the same frames with no message; a STOCK switch
 * whose zone was already closer than lead frames when it became known is logged late=N (and RB UNEXPECTED kind=lag_late once). A value this
 * side did not write (a foreign writer) -> RB UNEXPECTED kind=lag_foreign, the saved values back once, no further lag write this battle. */
static void lag_tick(const RboObs *o) {
    if (!B.lag_pinned || B.lag_restored || B.lag_zl0 || B.lag_off) return;
    if (o->lagmin != B.lag_cur_min || o->lagmax != B.lag_cur_max) {
        B.lag_foreign++; B.lag_off = 1;
        rbo_log("RB UNEXPECTED kind=lag_foreign b=%u detail=InputLagMin/Max_%u,%u_where_this_side_left_%u,%u:_the_saved_%u,%u_restored_once,_no_further_lag_write f=%u",
                B.b, o->lagmin, o->lagmax, B.lag_cur_min, B.lag_cur_max, B.lag_min, B.lag_max, rbo_core_last_f());
        rbo_gw32(rbo_obj + RBO_O_LAGMIN, B.lag_min); rbo_gw32(rbo_obj + RBO_O_LAGMAX, B.lag_max);
        B.lag_mode = RBO_LAG_STOCK; B.lag_cur_min = B.lag_min; B.lag_cur_max = B.lag_max; B.lag_restored = 1;
        return;
    }
    int lead = g_cfg.rbonline_zone_lag_lead, det = lead;
#ifdef SC6RB_HARNESS
    if (g_h_zone_lag_late > 0) det = lead - g_h_zone_lag_late;   /* ZLAGLATE: the STOCK decision sees fewer frames ahead (it comes late) */
#endif
    uint32_t s = rbo_core_last_f();
    int on = rbo_core_on(), za = 0, zfirst = 0;
    for (int i = 1; i <= det; i++) if (rbo_core_zone(s + (uint32_t)i)) { za = 1; zfirst = i; break; }
    int want = rbo_lag_want(1, on, za);
    if (want == B.lag_mode) return;
    int late = want == RBO_LAG_STOCK && on && za && zfirst < lead ? lead - zfirst : 0;
    uint32_t mn = want == RBO_LAG_PIN ? (uint32_t)rbo_d : B.lag_min, mx = want == RBO_LAG_PIN ? (uint32_t)rbo_d : B.lag_max;
    rbo_gw32(rbo_obj + RBO_O_LAGMIN, mn); rbo_gw32(rbo_obj + RBO_O_LAGMAX, mx);
    B.lag_mode = want; B.lag_cur_min = mn; B.lag_cur_max = mx;
    if (want == RBO_LAG_PIN) B.lag_pins++; else B.lag_stocks++;
    if (B.lag_lines < 64) {
        B.lag_lines++;
        rbo_log("RB LAG b=%u f=%u to=%s min=%u max=%u lag=%u why=%s late=%d", B.b, s, want == RBO_LAG_PIN ? "pin" : "stock", mn, mx, o->lag,
                !on ? "core_off" : za ? "zone_ahead" : "fight", late);
    }
    if (late > 0) {
        B.lag_late++;
        if (!B.lag_late_logged) { B.lag_late_logged = 1; rbo_log("RB UNEXPECTED kind=lag_late b=%u detail=f=%u_the_zone_was_%d_frames_ahead_when_seen_(lead_%d):_switched_late_by_%d", B.b, s, zfirst, lead, late); }
    }
}

/* ---------------------------------------------------------------- v0.30.0 (OPTA-M2-SPEC.md §19.4): the frame-advantage time sync (IsPaused pre)
 * Eligible tick: ARMED, InputSync state 2, the core on, s+1 and s+2 outside the zones (the stall lift's own gate) and the InputLag pinned. On
 * those ticks this PC samples adv = GameTime - (SyncTime - 1) into a 16-tick window (after 8 settle ticks at each entry), reports it every 8
 * samples (RBM_TSYNC, unsequenced) and, with no debt or hold-off and a fresh report of the peer for this round, schedules
 * rbo_ts_waits(mine, peer) waits. A wait = one result-2 stall this PC does NOT lift (ts_pay): the stock stall that the max_rb bound already
 * produces, one paused tick, never an input change. Returns 1 on an eligible tick. */
static void ts_win_reset(void) { B.ts_wi = 0; B.ts_n = 0; B.ts_sum = 0; B.ts_since_rep = 0; }
static int ts_tick(const RboObs *o) {
    if (!B.ts_round_set || o->round != B.ts_round) {   /* a new round: window, debt, hold-off and the per-round count start over */
        B.ts_round_set = 1; B.ts_round = o->round; B.ts_waits_r = 0; B.ts_stopped = 0; B.ts_debt = 0; B.ts_hold = 0; B.ts_elig_prev = 0; ts_win_reset();
    }
    uint32_t s = rbo_core_last_f();
    int elig = o->state == 2 && !B.ctxonly && rbo_core_on() && !rbo_core_zone(s + 1) && !rbo_core_zone(s + 2) && B.lag_mode == RBO_LAG_PIN;
    if (!elig) { if (B.ts_elig_prev) { B.ts_debt = 0; B.ts_hold = 0; } B.ts_elig_prev = 0; return 0; }
    if (!B.ts_elig_prev) { B.ts_elig_prev = 1; B.ts_settle = RBO_TS_SETTLE; ts_win_reset(); B.ts_debt = 0; B.ts_hold = 0; }
    if (B.ts_gap < 1000) B.ts_gap++;
    if (B.ts_hold > 0) B.ts_hold--;
    if (B.ts_settle > 0) { B.ts_settle--; return 1; }
    int32_t adv = (int32_t)o->gt - ((int32_t)o->synctime - 1);
    if (adv < RBO_TS_ADV_MIN) adv = RBO_TS_ADV_MIN;
    if (adv > RBO_TS_ADV_MAX) adv = RBO_TS_ADV_MAX;
    uint32_t slot = B.ts_wi % RBO_TS_WIN;
    if (B.ts_n == RBO_TS_WIN) B.ts_sum -= B.ts_win[slot]; else B.ts_n++;
    B.ts_win[slot] = (int8_t)adv; B.ts_sum += adv; B.ts_wi++;
    if (++B.ts_since_rep >= RBO_TS_EVERY) {
        B.ts_since_rep = 0;
        RbmMsg m; msg_zero(&m, RBM_TSYNC);
        m.u.tsync.round = (uint8_t)o->round; m.u.tsync.flags = (uint8_t)(1u | (g_cfg.rbonline_tsync ? 2u : 0u) | (B.ts_debt || B.ts_hold ? 4u : 0u));
        m.u.tsync.n = (uint16_t)B.ts_n; m.u.tsync.adv_sum = B.ts_sum; m.u.tsync.gt = o->gt; m.u.tsync.waits = (uint16_t)B.ts_waits_r;
        {   float rtt; uint32_t bits = o->rtt_bits; memcpy(&rtt, &bits, 4); m.u.tsync.rtt_ms = (uint16_t)(rtt > 0.0f && rtt < 60000.0f ? rtt + 0.5f : 0.0f); }
#ifdef SC6RB_HARNESS
        if (g_h_ts_plant_on) m.u.tsync.adv_sum = g_h_ts_plant_sum;   /* TSBOGUS: a planted report */
#endif
        send_msg(RBM_TSYNC, &m); B.ts_tx++;
    }
    if (B.ts_debt || B.ts_hold || B.ts_stopped || !g_cfg.rbonline_tsync || !B.ts_have_peer) return 1;
    if (B.ticks - B.ts_peer_tick > RBO_TS_FRESH || B.ts_peer.round != (uint8_t)o->round || !(B.ts_peer.flags & 1u) || B.ts_peer.n < RBO_TS_MIN_N) return 1;
    int band = RBO_TS_BAND16, guard = 1;
#ifdef SC6RB_HARNESS
    band = g_h_ts_band16; guard = !g_h_ts_noguard;
#endif
    if (guard && !rbo_ts_report_ok(B.ts_peer.adv_sum, B.ts_peer.n)) {   /* a report no valid sender produces: refused, loud once per battle */
        if (B.ts_peer_tick != B.ts_bad_tick) { B.ts_bad_tick = B.ts_peer_tick; B.ts_rx_bad++; }
        if (!B.ts_bad_logged) {
            B.ts_bad_logged = 1;
            rbo_log("RB UNEXPECTED kind=tsync_report b=%u detail=the_peer's_report_adv_sum_%d_n_%u_is_outside_[%d,%d]_per_sample:_refused_(no_wait_decided_on_it)",
                    B.b, B.ts_peer.adv_sum, (unsigned)B.ts_peer.n, RBO_TS_ADV_MIN, RBO_TS_ADV_MAX);
        }
        return 1;
    }
    int w = rbo_ts_waits_x(B.ts_sum, B.ts_n, B.ts_peer.adv_sum, B.ts_peer.n, RBO_TS_MIN_N, RBO_TS_MAXW, band, guard);
    if (w <= 0) return 1;
    if (B.ts_waits_r + (uint32_t)w > RBO_TS_ROUND_MAX) w = (int)(RBO_TS_ROUND_MAX - (B.ts_waits_r < RBO_TS_ROUND_MAX ? B.ts_waits_r : RBO_TS_ROUND_MAX));
    if (w <= 0) {
        B.ts_stopped = 1; B.ts_stops++;
        rbo_log("RB TSYNC STOP b=%u r=%u gt=%u waits=%u (the round's %d waits are spent: no more waits this round)", B.b, o->round, o->gt, B.ts_waits_r, RBO_TS_ROUND_MAX);
        return 1;
    }
    B.ts_debt = w; B.ts_decisions++;
    if (B.ts_lines < 32) {
        B.ts_lines++;
        int32_t mine100 = (int32_t)((int64_t)B.ts_sum * 100 / (int64_t)B.ts_n), peer100 = (int32_t)((int64_t)B.ts_peer.adv_sum * 100 / (int64_t)B.ts_peer.n);
        int32_t am = mine100 < 0 ? -mine100 : mine100, ap = peer100 < 0 ? -peer100 : peer100;
        rbo_log("RB TSYNC b=%u r=%u gt=%u f=%u mine=%s%d.%02d peer=%s%d.%02d w=%d waits=%u rtt_ms=%u", B.b, o->round, o->gt, s,
                mine100 < 0 ? "-" : "", am / 100, am % 100, peer100 < 0 ? "-" : "", ap / 100, ap % 100, w, B.ts_waits_r + (uint32_t)w, (unsigned)B.ts_peer.rtt_ms);
    }
    return 1;
}
/* one withheld lift (the result-2 stall stays): at most one every RBO_TS_GAP eligible ticks; the last one of a decision starts the hold-off */
static int ts_pay(void) {
    if (B.ts_debt <= 0 || B.ts_gap < RBO_TS_GAP) return 0;
    B.ts_debt--; B.ts_waits++; B.ts_waits_r++; B.ts_gap = 0;
    if (!B.ts_debt) B.ts_hold = RBO_TS_HOLD;
    return 1;
}

/* the stall lift of one armed tick (spec §5.3); 1 = it stopped at the max_rb bound (this PC waits on the other PC) */
static int lift_tick(const RboObs *o, int elig) {
    /* state 5 / bOnlineSync 0 are END (Tick pre / SetOnlineState); a non-2 state here is a barrier (never lifted) */
    if (o->state != 2 || o->result != 2) return 0;
    int unb = 0;
#ifdef SC6RB_HARNESS
    unb = g_cfg.h_rbo_lift_unbounded != 0;
#endif
    uint32_t s = rbo_core_last_f();
    if (!rbo_core_on() || rbo_core_zone(s + 1) || rbo_core_zone(s + 2)) return 0;   /* zones never lift (spec §5.6) */
    if (rbo_lift_ok(o->state, o->result, (int32_t)o->gt, (int32_t)o->synctime, rbo_max_rb, unb)) {
        if (elig && ts_pay()) return 0;   /* v0.30.0 (§19.4): the time sync's wait: this stall stays (one paused tick); not a lift, not a bound stop */
        rbo_gw8(o->obj + RBO_O_RESULT, 0);
        B.lifts++;
        return 0;
    }
    B.lift_bound_stops++;
    return 1;
}
/* v0.33.0 (OPTA-M2-SPEC.md §22.3): the at-limit window (presentation only: the badge reads lim_on; plugin memory, no game read beyond the tick's) */
static void lim_note(int stop) {
    int i = B.lim_i; uint64_t *w = &B.lim_bits[i >> 6], m = 1ULL << (i & 63);
    if (*w & m) B.lim_n--;
    if (stop) { *w |= m; B.lim_n++; } else *w &= ~m;
    B.lim_i = (i + 1) % RBO_LIM_WIN;
    if (B.lim_samples < (1 << 30)) B.lim_samples++;
    int on = rbo_lim_next(B.lim_on, B.lim_n, B.lim_samples);
    if (on && !B.lim_on) B.lim_enter++;
    B.lim_on = on; B.lim_ticks++; if (on) B.lim_on_ticks++;
}
void rbo_on_ispaused(const RboObs *o) {
    if (rbo_st == RBO_END) return;
    B.round = o->round; B.gt = o->gt;
    if (rbo_st == RBO_PAIRED && B.arm_pending_set && o->state == 2 && o->round == B.arm_pending) do_arm(o);
    if (rbo_st != RBO_ARMED) { B.lim_on = 0; return; }
    lag_tick(o);                  /* v0.30.0 (§19.2): before the early returns (the barrier states and the zones are where it goes STOCK) */
    int elig = ts_tick(o);        /* v0.30.0 (§19.4) */
    int stop = lift_tick(o, elig);
    if (o->state == 2) lim_note(stop);   /* v0.33.0 (§22.3): every armed fight tick is one sample */
}

void rbo_on_setstate(const RboObs *o, uint32_t to) {
    (void)o;
    if (to == 5 && rbo_st != RBO_END && (B.online || B.probe_tried)) rbo_end(RBW_STATE5);
}

void rbo_on_rreset(const RboObs *o, uint32_t round) {
    (void)o;
    if (rbo_st == RBO_END) return;
    /* v0.30.0 (OPTA-M2-SPEC.md §19.3): the battle's first reset is the intro (V: r=9 from GameRound 0xffffffff in every S-B battle); every later
     * reset with round <= 15 is a real round, numbered as the game does (0 = the first round; a 10th round's r=9 is a round, not the intro).
     * Passive-safe: plugin memory only; a first reset that is not r=9 is one structured event (the model differs from the game's: say so). */
    B.nresets++;
    int real = B.nresets > 1 && round <= 15;
    if (B.nresets == 1 && round != 9 && !B.round_model_seen) { B.round_model_seen = 1; rbo_ev_text("RB UNEXPECTED kind=round_model detail=the_battle's_first_reset_is_not_round_9"); }
    if (real) { B.last_reset = (int32_t)round; B.reset_mask |= 1u << round; B.real_resets++; }
    /* the PROBE deadline: the reset of the battle's SECOND real round (the game's r=1, as v0.29.9's `round == 1`): a pairing during the first
     * round can still agree and arm at r=1 */
    if (rbo_st == RBO_PROBE && real && B.real_resets == 2) { sm_set(RBO_PASSIVE, RBW_NO_ANSWER); p2p_end(RBW_NO_ANSWER); return; }
    if (rbo_st == RBO_PAIRED && real) {
        uint64_t now = ms_now();
        if (rbo_agreed(B.sent_ack, B.recv_ack, round) && B.t_agreed[round] && now - B.t_agreed[round] >= (uint64_t)g_cfg.rbonline_arm_margin_ms) {
            B.arm_pending = round; B.arm_pending_set = 1; B.margin_ms = now - B.t_agreed[round];
        } else if (B.r_proposed <= round) {   /* too late for this round (or not agreed): the next one, as D-C's fallback */
            B.r_proposed = r_mine_now();
            send_arm(RBM_ARM, B.r_proposed);
        }
        return;
    }
    if ((rbo_st == RBO_ARMED || rbo_st == RBO_DRAINED) && !B.ctxonly) {
        /* the round-reset wall (spec §5.7): the reset invalidates both rings; nothing may be unconfirmed (the ring is read one last time first) */
        rbo_fetch_confirm_scan(rbo_core_last_f());
        uint32_t unc = rbo_fetch_unconfirmed();
        int nowall = 0;
#ifdef SC6RB_HARNESS
        nowall = g_cfg.h_rbo_no_wall != 0;
#endif
        if (unc && !nowall) {
            /* the truth of those frames can never arrive (the reset clears both rings): drain, commit them as predicted (rb confirms them as run;
             * if a prediction was wrong this is a desync, which CHECK then reports), and finish the drain line before the reset runs */
            if (rbo_st == RBO_ARMED) rbo_drain(RBW_RESET_WALL);
            uint32_t forced = rbo_fetch_force_confirm();
            rbo_log("RB UNEXPECTED kind=reset_wall b=%u detail=unconfirmed_%u_at_round_%u_reset_committed_as_predicted_%u", B.b, unc, round, forced);
            drain_progress();
        }
    }
}

/* ================================================================== the title (M1's writer thread) and the RB line writer */
void rbo_title_token(char *out, size_t cap, const char *regs) {
    const char *rg = regs ? regs : "?";
    int st = (int)rbo_st;
    if (rbo_off) snprintf(out, cap, "rb=OFF:%s b=%u regs=%s", g_off_gate[0] ? g_off_gate : "gate", rbo_battle(), rg);
    else if (rbo_q1sig) snprintf(out, cap, "rb=OFF:q1sig b=%u regs=%s", rbo_battle(), rg);   /* v0.29.2 (§16.6): PROBE off for the process */
    else if (B.desync && (st == RBO_DRAINED || st == RBO_ARMED)) snprintf(out, cap, "rb=DESYNC:diverged b=%u regs=%s", rbo_battle(), rg);   /* v0.29.1 (M4) */
    else if (B.asym && st != RBO_END) snprintf(out, cap, "rb=ASYM b=%u regs=%s", rbo_battle(), rg);                                         /* v0.29.1 (M2) */
    else if (st == RBO_ARMED) snprintf(out, cap, "rb=armed d=%d b=%u regs=%s", rbo_d, rbo_battle(), rg);
    else if (st == RBO_DRAINED) snprintf(out, cap, "rb=drained:%s b=%u regs=%s", rbw_nm(B.drained_why < RBW_N ? B.drained_why : 0), rbo_battle(), rg);
    else snprintf(out, cap, "rb=%s b=%u regs=%s", st >= 0 && st < RBO_NSTATE ? k_rbo_state[st] : "?", rbo_battle(), rg);
    lipsync_rbo_title(out, cap);   /* v0.29.4 FACES: " lips=on|FAIL" from ARM to END of an armed battle with lipsync=1; nothing otherwise */
}
void rbo_set_off(const char *gate) { scpy(g_off_gate, sizeof g_off_gate, gate); InterlockedExchange(&rbo_off, 1); }

/* ================================================================== v0.31.0 (OPTA-M2-SPEC.md §20.1, §20.2): the badge text and the sound cue (M1's writer
 * thread, ~20 Hz, and once at install). The same globals the title token reads (racy reads of game-thread state, as the title's); the text goes
 * to the render thread through overlay_publish's seqlock, only when it changed. Nothing here writes game memory, sends, or logs per tick: the
 * lines are CUE (at most two per battle) and overlay_poll's OVERLAY events. */
uint32_t rb_rbk(void);   /* rb.c: this armed battle's rollbacks */
static int badge_cue_kind(const BadgeIn *in) {
    if (!in->in_battle || !in->online) return CUEK_NEUTRAL;
    if ((in->desync && in->st >= RBO_ARMED) || in->asym || in->st == RBO_DRAINED) return CUEK_FALLBACK;   /* OFF never leaves rollback: it is the process's */
    if (in->st == RBO_ARMED) return CUEK_ROLLBACK;
    return CUEK_NEUTRAL;
}
void rbo_badge_tick(void) {
    static char ver[16];
    if (!ver[0]) { size_t i = 0; for (; SC6RB_VERSION[i] && SC6RB_VERSION[i] != '-' && i + 1 < sizeof ver; i++) ver[i] = SC6RB_VERSION[i]; ver[i] = 0; }
    BadgeIn in; memset(&in, 0, sizeof in);
    int st = (int)rbo_st;
    in.off = rbo_off || rbo_q1sig; in.off_gate = rbo_off ? (g_off_gate[0] ? g_off_gate : "gate") : "q1sig";
    in.in_battle = g_rbo_in_battle; in.online = B.online || B.probe_tried;
    in.st = st; in.armed_ever = B.armed_ever; in.desync = B.desync; in.asym = B.asym;
    in.drained_why = B.drained_why ? rbw_nm(B.drained_why < RBW_N ? B.drained_why : 0) : NULL;
    int pw = B.passive_why; in.passive_why = pw > 0 ? rbw_nm(pw - 1 < RBW_N ? pw - 1 : 0) : NULL;
    in.d = rbo_d;
    in.at_limit = st == RBO_ARMED && B.lim_on;   /* v0.33.0 (§22.3) */
#ifdef SC6RB_HARNESS
    if (g_h_lim_old) in.at_limit = 0;   /* XLIM400: v0.32.0's badge */
#endif
    {   float rtt; uint32_t bits = g_badge_rtt_bits; memcpy(&rtt, &bits, 4); in.rtt_ms = bits && rtt > 0.0f && rtt < 60000.0f ? (int)(rtt + 0.5f) : -1; }
    in.rollbacks = st == RBO_PASSIVE || st == RBO_PROBE || st == RBO_PAIRED ? 0 : rb_rbk();
    in.regs_bad = nobs_regs_bad(); in.lips_fail = lipsync_rbo_fail();
    in.version = ver;
    BadgeSnap s; overlay_badge_text(&in, &s);
    static BadgeSnap last; static int have;
    if (!have || memcmp(&s, &last, sizeof s)) { overlay_publish(&s); last = s; have = 1; }
    static CueState cs;
    int cue = cue_next(&cs, rbo_battle(), badge_cue_kind(&in));
    if (cue) {
        double us = g_cfg.rbonline_sound ? cue_play(cue) : -2;
        LOG("CUE kind=%s b=%u %s us=%.0f\n", cue == CUE_ARMED ? "armed" : "fallback", rbo_battle(),
            us >= 0 ? "played" : us < -1.5 ? "off (rbonline_sound=0)" :
#ifdef SC6RB_HARNESS
            "not_played (harness)",
#else
            "not_played (no winmm)",
#endif
            us);
    }
    overlay_poll();
}

static const char *api_name(uint32_t a) { return a == 1 ? (g_p2p_api_name ? g_p2p_api_name : "steam") : "none"; }   /* g_p2p_api_name: rbo_install */

/* ================================================================== v0.29.8: the content dump (writer thread; OPTA-M2-SPEC.md §18.11)
 * <log_dir>\rbcontent_<ts>_<pid>_b<n>.txt, named after this session's log sc6rb_<ts>_<pid>.log. Game data only: the move-record keys and the
 * KHit nodes' bytes, the image base (to rebase addresses offline) and the digest; no path, no id, no address of the PC's network. Read by
 * tools/content_diff.py. Lines:
 *   H b= version= image_base= image_size= gt= toggle= toggle_ok= digest= digest_v9= digest_v8= walk_ok=   (v0.29.9: digest = the HELLO's,
 *     digest_v9 = the base-independent digest, digest_v8 = v0.29.8's of the same read)
 *   P p= read= tree_sz= tree_n= tree_ok= keys= keys_trunc= nodes= nodes_trunc= lists=<n0>,<n1>,<n2> lists_ok=<b0><b1><b2>
 *   K p= i=<first index> <key hex> ... (up to 32 per line, walk order)
 *   N p= q= i= vt=<rva hex> cls=<sphere|area|fixarea|base|heap|other> node=<abs hex> rva=<rva hex or -> span=<0x1c bytes hex> bytes=<0xa0 bytes hex>
 *   E nodes=<total> keys=<total> */
static int cd_dump_name(uint32_t b, char *full, size_t cap, char *base, size_t bcap) {
    const char *slash = strrchr(g_log_path, '\\'), *fwd = strrchr(g_log_path, '/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    const char *nm = slash ? slash + 1 : g_log_path;
    size_t L = strlen(nm);
    if (!g_log_path[0] || L < 11 || strncmp(nm, "sc6rb_", 6) || strcmp(nm + L - 4, ".log")) return 0;
    snprintf(base, bcap, "rbcontent_%.*s_b%u.txt", (int)(L - 10), nm + 6, b);
    snprintf(full, cap, "%.*s%s", (int)(nm - g_log_path), g_log_path, base);
    return 1;
}
static const char *cd_cls(uint32_t rva) {
    return rva == 0x3e877f0u ? "sphere" : rva == 0x3e877a8u ? "area" : rva == 0x3e87760u ? "fixarea" : rva == 0x3e87838u ? "base" : rva == 0 ? "heap" : "other";
}
static void cd_hex(FILE *f, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]); }
static void cd_write_dump(const RboEv *e) {
    char full[MAX_PATH], base[128];
    if (cd_dump_name(e->b, full, sizeof full, base, sizeof base)) {
        FILE *f = fopen(full, "w");
        int written = f != NULL;
        if (f) {
            static char buf[1 << 16]; setvbuf(f, buf, _IOFBF, sizeof buf);
            fprintf(f, "# sc6rb content dump (OPTA-M2-SPEC.md 18.11): game data only; read with tools/content_diff.py\n");
            fprintf(f, "H b=%u version=%s image_base=0x%llx image_size=0x%llx gt=%u toggle=%u toggle_ok=%u digest=%016llx digest_v9=%016llx digest_v8=%016llx walk_ok=0x%x\n",
                    e->b, SC6RB_VERSION, (unsigned long long)g_base, (unsigned long long)g_image_size, e->u[1], e->u[2], e->u[3], (unsigned long long)e->q[0],
                    (unsigned long long)e->q[2], (unsigned long long)e->q[1], e->u[0]);
            uint32_t tn = 0, tk = 0;
            for (int p = 0; p < 2; p++) {
                const uint32_t *u = &e->u[4 + 10 * p];   /* read, tree_sz, tree_n, tree_ok, kn[3], list_ok[3] at the capture (content_events) */
                fprintf(f, "P p=%d read=%u tree_sz=%u tree_n=%u tree_ok=%u keys=%u keys_trunc=%u nodes=%u nodes_trunc=%u lists=%u,%u,%u lists_ok=%u%u%u\n", p, u[0], u[1],
                        u[2], u[3], rbo_cdiag.nkeys[p], rbo_cdiag.trunc_keys[p], rbo_cdiag.nnodes[p], rbo_cdiag.trunc_nodes[p], u[4], u[5], u[6], u[7], u[8], u[9]);
                for (uint32_t i = 0; i < rbo_cdiag.nkeys[p]; i += 32) {
                    fprintf(f, "K p=%d i=%u", p, i);
                    for (uint32_t j = i; j < i + 32 && j < rbo_cdiag.nkeys[p]; j++) fprintf(f, " %08x", rbo_cdiag.keys[p][j]);
                    fputc('\n', f);
                }
                for (uint32_t i = 0; i < rbo_cdiag.nnodes[p]; i++) {
                    const RboCdNode *c = &rbo_cdiag.nodes[p][i];
                    uint64_t rva = c->node >= (uint64_t)g_base && c->node < (uint64_t)g_base + g_image_size ? c->node - (uint64_t)g_base : ~0ULL;
                    fprintf(f, "N p=%d q=%u i=%u vt=0x%x cls=%s node=0x%llx rva=", p, c->q, c->idx, c->vt_rva, cd_cls(c->vt_rva), (unsigned long long)c->node);
                    if (rva == ~0ULL) fputc('-', f); else fprintf(f, "0x%llx", (unsigned long long)rva);
                    fputs(" span=", f); cd_hex(f, c->b + 0x7c, 0x1c);
                    fputs(" bytes=", f); cd_hex(f, c->b, RBO_CD_NB);
                    fputc('\n', f);
                }
                tn += rbo_cdiag.nnodes[p]; tk += rbo_cdiag.nkeys[p];
            }
            fprintf(f, "E nodes=%u keys=%u\n", tn, tk);
            fclose(f);
        }
        LOG("RB CONTENT DUMP b=%u file=%s written=%d\n", e->b, base, written);
    } else LOG("RB CONTENT DUMP b=%u file=- written=0 (no session log name)\n", e->b);
    InterlockedExchange(&rbo_cdiag.busy, 0);
}

static void ev_format(const RboEv *e) {
    switch (e->type) {
    case EV_TEXT: LOG("%s\n", e->s); break;
    case EV_STATE:
        LOG("RB STATE b=%u from=%s to=%s why=%s r=%u gt=%u tick=%u\n", e->b, k_rbo_state[e->u[0] < RBO_NSTATE ? e->u[0] : 0], k_rbo_state[e->u[1] < RBO_NSTATE ? e->u[1] : 0],
            rbw_nm(e->u[2] < RBW_N ? (int)e->u[2] : 0), e->u[3], e->u[4], e->u[5]);
        break;
    case EV_PEER: LOG("RB PEER b=%u id_ok=%u\n", e->b, e->u[0]); break;
    case EV_PEERID:   /* v0.29.7: classes and counts only, never an id */
        LOG("RB PEERID b=%u src=%s cand=peer:%u,raw:%u type_ok=%u self=%u p2p_ok=%u ambiguous=%u steam=%s result=%s\n", e->b,
            k_pid_src[e->u[0] < RBO_PSRC_N ? e->u[0] : 0], e->u[1], e->u[2], e->u[3], e->u[4], e->u[5], e->u[8],
            k_pid_st[e->u[6] < PST_N ? e->u[6] : 0], k_pid_res[e->u[7] < PID_N ? e->u[7] : 0]);
        break;
    case EV_P2P: LOG("RB P2P b=%u api=%s why=%s\n", e->b, e->s[0] ? e->s : api_name(e->u[0]), k_p2p_ns[e->u[1] < P2P_NS_N ? e->u[1] : 0]); break;
    case EV_CTX:
        LOG("RB CTX b=%u samples=%u xmm6=0x%08x xmm7=0x%08x xmm8=0x%08x xmm9=0x%08x xmm10=0x%08x xmm11=0x%08x xmm12=0x%08x xmm13=0x%08x xmm14=0x%08x xmm15=0x%08x "
            "mxcsr=0x%04x fcw=0x%04x upper_zero=%u const_ok=%u builtin_eq=%u content=%016llx content_n=%u,%u walk_ok=0x%x\n", e->b, e->u[0],
            e->u[1], e->u[2], e->u[3], e->u[4], e->u[5], e->u[6], e->u[7], e->u[8], e->u[9], e->u[10], e->u[11], e->u[12], e->u[13], e->u[14], e->u[15],
            (unsigned long long)e->q[0], e->u[16], e->u[17], e->u[18]);
        break;
    case EV_HELLO:
        LOG("RB HELLO b=%u sent=%u acked=%u peer_ver=%s content_eq=%u ctx_eq=%u cfg_eq=%u verdict_mine=0x%x verdict_peer=0x%x tsync=%u peer_tsync=%u argc=%u peer_argc=%u\n", e->b,
            e->u[0], e->u[1], e->s[0] ? e->s : "-", e->u[2], e->u[3], e->u[4], e->u[5], e->u[6], e->u[7], e->u[8], e->u[9], e->u[10]);   /* v0.30.0: + tsync= peer_tsync=; v0.34.0: + argc= peer_argc= (appended) */
        break;
    case EV_ARGC: {   /* v0.34.0 (OPTA-M2-SPEC.md §23.1) */
        static const char *const fl[3] = { "0", "1", "-" };
        char at[64], now[64];
        {   int st[ARGC_NSITES], unk = 0; for (int i = 0; i < ARGC_NSITES; i++) { st[i] = (int)e->u[6 + i]; if (st[i] > 3) unk = 1; }
            if (unk) snprintf(at, sizeof at, "-"); else argc_states_str(st, at, sizeof at);
            unk = 0; for (int i = 0; i < ARGC_NSITES; i++) { st[i] = (int)e->u[10 + i]; if (st[i] > 3) unk = 1; }
            if (unk) snprintf(now, sizeof now, "-"); else argc_states_str(st, now, sizeof now); }
        int nowstock = !strcmp(now, "stock,stock,stock,stock");
        /* sites_ok: the bytes are stock at the battle's end (or were never readable here), and an applied battle's apply read back fixed (at_arm) */
        int sok = e->u[4] ? (nowstock && !strcmp(at, "fixed,fixed,fixed,fixed")) : (!strcmp(now, "-") || nowstock);
        LOG("RB ARGC b=%u state=%s why=%s mine=%s peer=%s applied=%u restored=%u apply=%s restore=%s at_arm=%s now=%s sites_ok=%d cap=%u broken=%u\n", e->b,
            e->u[0] ? "on" : "off", e->u[1] < AW_N ? k_aw[e->u[1]] : "?", fl[e->u[2] < 3 ? e->u[2] : 2], fl[e->u[3] < 3 ? e->u[3] : 2], e->u[4], e->u[5],
            e->u[4] ? k_argc_why[e->u[14] < ARGC_W_N ? e->u[14] : 0] : "-", e->u[4] ? k_argc_why[e->u[15] < ARGC_W_N ? e->u[15] : 0] : "-", at, now, sok, e->u[16], e->u[17]);
        break; }
    case EV_ENDP:
        LOG("RB END b=%u why=%s lag_restored=1 fetch_restored=1 step_restored=1\n", e->b, rbw_nm(e->u[0] < RBW_N ? (int)e->u[0] : 0));
        LOG("RB SUMMARY b=%u state_max=%s frames_armed=0 rollbacks=0 rb_hist=- max_depth=0 lifts=0 desyncs=0 checks_ok=0 checks=0 drains=0 why=%s\n", e->b,
            k_rbo_state[e->u[1] < RBO_NSTATE ? e->u[1] : 0], rbw_nm(e->u[0] < RBW_N ? (int)e->u[0] : 0));
        LOG("RB P2P SUMMARY b=%u tx=%llu rx=%llu rx_bad=%llu rx_foreign_id=%llu rx_stale=%llu dup=%llu resends=%llu probe_sent=%llu q_drops=%llu\n", e->b,
            (unsigned long long)e->q[0], (unsigned long long)e->q[1], (unsigned long long)e->q[2], (unsigned long long)e->q[3], (unsigned long long)e->q[4],
            (unsigned long long)e->q[5], (unsigned long long)e->q[6], (unsigned long long)e->q[7], (unsigned long long)e->q[8]);
        break;
    case EV_FLUSH: log_flush(); break;
    case EV_CONTENT: {   /* v0.29.8 (OPTA-M2-SPEC.md §18.11): the digest's parts, per player; compare the two PCs with tools/content_diff.py */
        char dn[160] = "-", full[MAX_PATH];
        if (e->u[18] && !cd_dump_name(e->b, full, sizeof full, dn, sizeof dn)) scpy(dn, sizeof dn, "-");
        const uint64_t *x = (const uint64_t *)(const void *)e->s;   /* v0.29.9: ka[0..2] (the HELLO digest's list parts), v8 (v0.29.8's digest of the same read) */
        LOG("RB CONTENT b=%u p=%u gt=%u r=%u samples=%u read=%u tree=%u:%016llx tree_sz=%u tree_ok=%u k0=%u:%016llx:%016llx k1=%u:%016llx:%016llx k2=%u:%016llx:%016llx "
            "reb=%016llx,%016llx,%016llx abs=%u,%u,%u auth=%016llx ka=%016llx,%016llx,%016llx v8=%016llx lists_ok=%u%u%u toggle=%u toggle_ok=%u dump=%s\n",
            e->b, e->u[0], e->u[1], e->u[2], e->u[19], e->u[3], e->u[5], (unsigned long long)e->q[0], e->u[4], e->u[6],
            e->u[7], (unsigned long long)e->q[1], (unsigned long long)e->q[4], e->u[8], (unsigned long long)e->q[2], (unsigned long long)e->q[5],
            e->u[9], (unsigned long long)e->q[3], (unsigned long long)e->q[6], (unsigned long long)e->q[7], (unsigned long long)e->q[8], (unsigned long long)e->q[9],
            e->u[10], e->u[11], e->u[12], (unsigned long long)e->q[10], (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2],
            (unsigned long long)x[3], e->u[13], e->u[14], e->u[15], e->u[16], e->u[17], dn);
        break; }
    case EV_CDUMP: cd_write_dump(e); break;
    default: break;
    }
}
static DWORD WINAPI writer_main(LPVOID p) {
    (void)p;
    RboEv e; int idle = 0;
    for (;;) {
        int stop = g_writer_stop != 0, did = 0;
        while (ev_pop(&e)) { did = 1; ev_format(&e); }
        if (stop) break;
        if (!did) { if (++idle % 50 == 0) log_flush(); Sleep(2); } else idle = 0;
    }
    if (g_evq_drops) LOG("RB UNEXPECTED kind=evq_drops b=%u detail=%lld_events_dropped\n", nobs_bat, (long long)g_evq_drops);
    log_flush();
    return 0;
}
int rbo_runtime_init(void) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = (uint64_t)f.QuadPart;
    g_dadapt = cfg_geti("rbonline_d_adapt", 1) != 0;   /* v0.33.0 (§22.2): rbo_install's gate refused any value but 0 | 1 */
#ifdef SC6RB_HARNESS
    g_h_force_pair = cfg_geti("h_rbo_force_pair", 0);
    if (g_h_force_pair) LOG("RB CFG harness plant h_rbo_force_pair=%d: PROBE pairs without the side channel (an in-process armed run)\n", g_h_force_pair);
    g_h_no_ranked_gate = cfg_geti("h_rbo_no_ranked_gate", 0); g_h_no_asym_fix = cfg_geti("h_rbo_no_asym_fix", 0); g_h_passive_write = cfg_geti("h_rbo_passive_write", 0);
    g_h_hello_ver_inject = cfg_geti("h_rbo_hello_ver_inject", 0); g_h_hack_nonce0 = cfg_geti("h_rbo_hack_nonce0", 0); g_h_hack0_old = cfg_geti("h_rbo_hack0_old", 0);
    g_h_ver_noescape = cfg_geti("h_rbo_ver_noescape", 0);
    g_h_asym_early_old = cfg_geti("h_rbo_asym_early_old", 0);   /* v0.29.5 RBO */
    if (g_h_asym_early_old) LOG("RB CFG harness control h_rbo_asym_early_old=1 (XASYMEARLY: a DRAIN(arm_fail) received before this side's ARM leaves it ARMED)\n");
    g_h_beacon_old = cfg_geti("h_rbo_beacon_old", 0); g_h_drop_armfail = cfg_geti("h_rbo_drop_armfail_drain", 0);   /* v0.29.6 (MINOR 4) */
    g_h_peerid_old = cfg_geti("h_rbo_peerid_old", 0);   /* v0.29.7: XPEERPM */
    if (g_h_peerid_old) LOG("RB CFG harness control h_rbo_peerid_old=1 (the opponent id is v0.29.6's single read of session+0xd0)\n");
    if (g_h_beacon_old) LOG("RB CFG harness control h_rbo_beacon_old=1 (XASYMBCN: a degraded peer's beacon is ignored, v0.29.5)\n");
    rbm_h_proto = cfg_geti("h_rbo_proto", RBM_PROTO);   /* v0.29.6 (MINOR 7): PROTO4's peer of another protocol version (src/rbomsg.c) */
    if (rbm_h_proto != RBM_PROTO) LOG("RB CFG harness plant h_rbo_proto=%d: this process encodes and accepts protocol %d (this build: %d)\n", rbm_h_proto, rbm_h_proto, RBM_PROTO);
    rbo_h_content_old = cfg_geti("h_rbo_content_old", 0);   /* v0.29.9 (OPTA-M2-SPEC.md §18.12): XCDBASE / XCDWORLD / XCDLEFT / PROTO5 / XPROTO5 */
    if (rbo_h_content_old) LOG("RB CFG harness control h_rbo_content_old=1 (the HELLO carries v0.29.8's content digest: the raw span +0x7c..+0x97)\n");
    if (g_h_no_ranked_gate || g_h_no_asym_fix || g_h_passive_write)
        LOG("RB CFG harness controls h_rbo_no_ranked_gate=%d h_rbo_no_asym_fix=%d h_rbo_passive_write=%d\n", g_h_no_ranked_gate, g_h_no_asym_fix, g_h_passive_write);
    /* v0.30.0 (OPTA-M2-SPEC.md §19.6) */
    g_h_r_floor1 = cfg_geti("h_rbo_r_floor1", 0); g_h_beacon_ord_old = cfg_geti("h_rbo_beacon_ord_old", 0); g_h_peer_armed_old = cfg_geti("h_rbo_peer_armed_old", 0);
    g_h_bcn_replay_intro = cfg_geti("h_rbo_beacon_replay_intro", 0); g_h_bcn_replay_n = cfg_geti("h_rbo_beacon_replay_n", 3); g_h_bcn_drop_first = cfg_geti("h_rbo_bcn_drop_first", 0);   /* v0.33.0: + n3 */ g_h_drop_ack0 = cfg_geti("h_rbo_drop_ack0", 0); g_h_zone_lag_late = cfg_geti("h_rbo_zone_lag_late", 0);
    g_h_ts_plant_sum = cfg_geti("h_rbo_ts_plant_sum", 0); g_h_ts_plant_on = cfg_geti("h_rbo_ts_plant", 0); g_h_ts_noguard = cfg_geti("h_rbo_ts_noguard", 0);
    g_h_ts_band16 = cfg_geti("h_rbo_ts_band16", RBO_TS_BAND16);
    g_h_chk_old = cfg_geti("h_rbo_chk_old", 0);   /* v0.32.0 (OPTA-M2-SPEC.md §21.1) */
    g_h_dadapt_plant = cfg_geti("h_rbo_dadapt_plant", 0); g_h_darm_nocheck = cfg_geti("h_rbo_darm_nocheck", 0); g_h_dadapt_old = cfg_geti("h_rbo_dadapt_old", 0);   /* v0.33.0 */
    g_h_lim_old = cfg_geti("h_rbo_lim_old", 0);
    g_h_argc_old = cfg_geti("h_rbo_argc_old", 0); g_h_argc_noflag = cfg_geti("h_rbo_argc_noflag", 0); g_h_argc_noapply = cfg_geti("h_rbo_argc_noapply", 0);   /* v0.34.0 */
    if (g_h_argc_old || g_h_argc_noflag || g_h_argc_noapply)
        LOG("RB CFG harness v0.34.0 h_rbo_argc_old=%d h_rbo_argc_noflag=%d h_rbo_argc_noapply=%d\n", g_h_argc_old, g_h_argc_noflag, g_h_argc_noapply);
    if (g_h_dadapt_plant || g_h_darm_nocheck || g_h_dadapt_old || g_h_lim_old)
        LOG("RB CFG harness v0.33.0 h_rbo_dadapt_plant=%d h_rbo_darm_nocheck=%d h_rbo_dadapt_old=%d h_rbo_lim_old=%d\n", g_h_dadapt_plant, g_h_darm_nocheck, g_h_dadapt_old, g_h_lim_old);
    if (g_h_chk_old) LOG("RB CFG harness control h_rbo_chk_old=%d (%s)\n", g_h_chk_old, g_h_chk_old == 2 ? "the pre-review v0.32.0 chain: reset at every new window, "
                         "sent at the due frame only, no zone / END flush, no mirror" : "v0.31.0's CHECK: frame f's hash only, no chain");
    rbm_h_notsync = cfg_geti("h_rbo_ts_notype", 0);
    if (g_h_r_floor1 || g_h_beacon_ord_old || g_h_peer_armed_old || g_h_bcn_replay_intro || g_h_drop_ack0 || g_h_zone_lag_late || g_h_ts_plant_on || g_h_ts_noguard ||
        g_h_ts_band16 != RBO_TS_BAND16 || rbm_h_notsync)
        LOG("RB CFG harness v0.30.0 h_rbo_r_floor1=%d h_rbo_beacon_ord_old=%d h_rbo_peer_armed_old=%d h_rbo_beacon_replay_intro=%d h_rbo_drop_ack0=%d h_rbo_zone_lag_late=%d "
            "h_rbo_ts_plant=%d h_rbo_ts_plant_sum=%d h_rbo_ts_noguard=%d h_rbo_ts_band16=%d h_rbo_ts_notype=%d\n", g_h_r_floor1, g_h_beacon_ord_old, g_h_peer_armed_old,
            g_h_bcn_replay_intro, g_h_drop_ack0, g_h_zone_lag_late, g_h_ts_plant_on, g_h_ts_plant_sum, g_h_ts_noguard, g_h_ts_band16, rbm_h_notsync);
#endif
    g_evq = (RboCell *)VirtualAlloc(NULL, sizeof(RboCell) * EVQ_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_evq) return 0;
    for (uint32_t i = 0; i < EVQ_CAP; i++) g_evq[i].seq = i;
    battle_reset(0);
    InterlockedExchange(&rbo_st, RBO_PASSIVE);
    g_writer = CreateThread(NULL, 1u << 20, writer_main, NULL, 0, NULL);
    return g_writer != NULL;
}
void rbo_shutdown(int process_exit) {
    if (process_exit) log_set_bounded(1);   /* M1's m7: at process exit never spin forever on a log lock a dead thread may hold */
    if (!process_exit && rbo_st != RBO_END && rbo_st != RBO_PASSIVE) rbo_end(RBW_ENDPLAY);
    p2p_shutdown(process_exit);
    g_writer_stop = 1;
    DWORD wr = g_writer ? WaitForSingleObject(g_writer, process_exit ? 0 : 3000) : WAIT_OBJECT_0;
    if (wr == WAIT_OBJECT_0 && g_evq) { RboEv e; while (ev_pop(&e)) ev_format(&e); }   /* single consumer: only when the writer is gone */
    LOG("RB SHUTDOWN process_exit=%d writer_wait=%lu ev_drops=%lld doors=%d\n", process_exit, wr, (long long)g_evq_drops, rbo_core_doors());
    nobs_shutdown(process_exit);
    log_flush();
}
