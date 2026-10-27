/* Option A M2 (rbonline): the PURE rules of the rollback core (OPTA-M2-SPEC.md §3, §5.2, §5.3, §5.5, §6; sc6rb/M2-INTERFACES.md §3, §5.3).
 * Part of the rollback core. Included by src/rbo.c, src/rbo_fetch.c, src/rbo_core.c, src/rbo_body.c and by the unit test src/rbo_unit.inc
 * (built and run by tools/rbo_static.py). No Windows, no game memory, no globals, no CRT, no float: integers in, integers out, so the
 * same code is what the plugin runs and what the unit test checks. Every function is static inline (an unused one emits nothing, which
 * keeps rbo_body.o free of anything it does not call). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rbomsg.h"

/* ------------------------------------------------------------------ the stock step-word rule (dec_tick1.c:585-628; OPTA-M0.md §1.6 D2)
 * held = raw, pressed = (prev ^ raw) & raw; both & ~m12f8; both & 0xffffc3f0 when FUN_1403f2220(mgr, p) (c3). The timers +0x1468 /
 * +0x146c are ignored online (CheckOnlineSessionActive forces them off, dec_tick1.c:591-594). prev = the raw of the previous fetch of
 * that player (0x1498[p]; carried across rounds). Returns the 8-byte step word: held in the low dword, pressed in the high dword. */
#define RBO_C3_MASK 0xffffc3f0u
/* v0.29.1 (review m4): the timers, exactly as stock applies them when neither CheckOnlineSessionActive (FUN_1403f2590) nor the setup's
 * +0x220 byte says "ignore": tm bit 0 = param_5 (mgr+0x146c > 0) zeroes the held half, bit 1 = param_4 (mgr+0x1468 > 0) zeroes the pressed
 * half (dec_tick1.c:607-616). Online tm is always 0 (95,154 live frames); the armed FetchInput drains if it is not. */
#define RBO_TM_HELD0    1u
#define RBO_TM_PRESSED0 2u
static inline uint64_t rbo_word_tm(uint32_t raw, uint32_t prev, uint32_t m12f8, int c3, uint32_t tm) {
    uint32_t held = (tm & RBO_TM_HELD0) ? 0u : raw, pressed = (tm & RBO_TM_PRESSED0) ? 0u : (prev ^ raw) & raw;
    held &= ~m12f8; pressed &= ~m12f8;
    if (c3) { held &= RBO_C3_MASK; pressed &= RBO_C3_MASK; }
    return (uint64_t)held | (uint64_t)pressed << 32;
}
static inline uint64_t rbo_word(uint32_t raw, uint32_t prev, uint32_t m12f8, int c3) { return rbo_word_tm(raw, prev, m12f8, c3, 0); }
/* the harness control h_rbo_fetch_no_c3f0 (XFETCH): the same rule without the c3f0 mask (must make t1 differ from REF) */
static inline uint64_t rbo_word_noc3(uint32_t raw, uint32_t prev, uint32_t m12f8) { return rbo_word(raw, prev, m12f8, 0); }

/* ------------------------------------------------------------------ the stall lift (spec §5.3; OPTA-M0.md §1.10 D4)
 * Called at IsPaused pre. IsPaused pauses on its OLD PauseTime (or result 1), then sets PauseTime = result ? PauseTime + 1 : 0 and
 * zeroes the result (dec_fi3.c:376-404). So at a tick with result 2 and PauseTime 0 the frame G = GameTime runs whether or not we lift;
 * the lift decides whether frame G + 1 runs at the NEXT tick (the one-tick lag). SyncTime = the first remote frame not yet received
 * (FUN_1403feba0: SyncTime advances while every player's slot for SyncTime is valid), so frames >= SyncTime are unconfirmed.
 * Rule (spec): lift (write +0x4425 = 0) iff armed, InputSync state 2, result == 2 and GameTime - (SyncTime - 1) < max_rb. With the lag
 * that bounds every frame that runs: the next frame G + 1 satisfies (G + 1) - SyncTime <= max_rb - 1 (the depth analysis: "no step
 * ever ran a frame >= max_rb beyond the last confirmed remote frame"). Results 1 / 3 / 4 / 5 are never touched.
 * unbounded = the harness control h_rbo_lift_unbounded (XLIFT): the bound is ignored. Returns 1 = write 0 into +0x4425. */
static inline int rbo_lift_ok(uint32_t state, uint32_t result, int32_t game_time, int32_t sync_time, int max_rb, int unbounded) {
    if (state != 2 || result != 2) return 0;
    if (unbounded) return 1;
    int64_t ahead = (int64_t)game_time - ((int64_t)sync_time - 1);
    return ahead < (int64_t)max_rb;
}

/* ------------------------------------------------------------------ the per-battle state machine (spec §3; M2-INTERFACES.md §3.1)
 * 0 passive, 1 probe, 2 paired, 3 armed, 4 drained, 5 end (= RboState). The legal edges; every other one is a bug (RB UNEXPECTED). */
static inline int rbo_edge_ok(int from, int to) {
    if (to == 5) return from != 5;                       /* any -> END (once) */
    switch (from) {
    case 0: return to == 0 || to == 1;                   /* PASSIVE -> PROBE; PASSIVE -> PASSIVE (the "stay passive" decision line) */
    case 1: return to == 0 || to == 2;                   /* PROBE -> PASSIVE (rest of battle) | PAIRED */
    case 2: return to == 0 || to == 3 || to == 4;        /* PAIRED -> PASSIVE (no agreement) | ARMED | DRAINED (v0.29.1, review M2: the
                                                          * degraded arm: agreed but this side's arm failed, or the peer armed alone; only
                                                          * the L3-stock step context, no core, no game write) */
    case 3: return to == 4;                              /* ARMED -> DRAINED */
    case 4: return 0;                                    /* DRAINED -> END only */
    default: return 0;                                   /* END -> (next battle) is a reset, not an edge */
    }
}

/* ------------------------------------------------------------------ HELLO verdicts (M2-INTERFACES.md §5.2): 0 = everything equal.
 * The mod list is never compared (a visual mod never blocks pairing); plugin_version is logged, not compared; arm_round_min is not
 * compared (the ARM exchange takes the max). */
static inline uint16_t rbo_verdict(const RbmHello *mine, const RbmHello *peer) {
    uint16_t v = 0;
    if (mine->proto != peer->proto) v |= RBM_V_PROTO;
    if (mine->exe_ts != peer->exe_ts || mine->exe_soi != peer->exe_soi) v |= RBM_V_EXE;
    if (mine->content != peer->content || mine->content_n[0] != peer->content_n[0] || mine->content_n[1] != peer->content_n[1]) v |= RBM_V_CONTENT;
    if (!mine->ctx_samples || !peer->ctx_samples || !mine->ctx_const_ok || !peer->ctx_const_ok) v |= RBM_V_NOCTX;
    {   int d = mine->ctx_mxcsr != peer->ctx_mxcsr || mine->ctx_fcw != peer->ctx_fcw || mine->ctx_upper_zero != peer->ctx_upper_zero;
        for (int i = 0; i < 10; i++) d |= mine->ctx_xmm[i] != peer->ctx_xmm[i];
        if (d) v |= RBM_V_CTX; }
    if (mine->d != peer->d || mine->max_rb != peer->max_rb || mine->check_every != peer->check_every) v |= RBM_V_CFG;
    if ((mine->flags & RBM_HF_DADAPT) != (peer->flags & RBM_HF_DADAPT)) v |= RBM_V_CFG;   /* v0.33.0 (§22.2): one PC adapts the delay, the other not */
    if ((mine->flags & RBM_HF_RANKED) != (peer->flags & RBM_HF_RANKED)) v |= RBM_V_RANKED;
    if ((mine->flags & RBM_HF_ARGC) != (peer->flags & RBM_HF_ARGC)) v |= RBM_V_ARGC;   /* v0.34.0 (§23.1): the EvaluateIf argc fix (on only when both advertise it) */
    return v;
}
/* v0.34.0 (OPTA-M2-SPEC.md §23.1): the reason a refused pairing is logged with. Both PCs see the same union (mine | peer: each verdict compares the
 * same two HELLOs), so both pick the same reason: argc_off when the argc flag is the ONLY difference, mismatch otherwise (RB HELLO names every bit) */
static inline int rbo_refuse_why(uint16_t vmine, uint16_t vpeer) {
    uint16_t u = (uint16_t)(vmine | vpeer);
    return (u & RBM_V_ARGC) && !(u & (uint16_t)~RBM_V_ARGC) ? RBW_ARGC : RBW_MISMATCH;
}
/* v0.34.0: the fix runs in an armed battle exactly when both HELLOs carry RBM_HF_ARGC (only reached with a zero verdict, so the flags are equal) */
static inline int rbo_argc_on(const RbmHello *mine, const RbmHello *peer) { return (mine->flags & RBM_HF_ARGC) && (peer->flags & RBM_HF_ARGC); }

/* ------------------------------------------------------------------ v0.33.0 (OPTA-M2-SPEC.md §22.2): the delay both PCs arm with, computed from the SAME
 * inputs (the two HELLOs' rtt_ms): d = max(d_base, clamp(ceil(maxRTT * 0.03) + 1 - max_rb, 2, 4)), 0.03 = 1 / (2 x 16.667 ms) (the one-way trip in
 * frames). The rollback window is d + max_rb frames of one-way latency; beyond it the leading PC waits (the lift stops). maxRTT = 0 (neither PC knew
 * its RTT): d_base. The cap 4 is the C1b core's proven net_delay range (rbo_core_force, rb_cfg_check: D12 rb_zone_entry_gap 21 >= max_rb 7 + D 4 + 2).
 * adapt = 0: d_base (v0.32.0's fixed delay). Thresholds with max_rb 7 and d_base 2: RTT <= 266 ms -> 2, 267..300 -> 3, >= 301 -> 4. */
static inline int rbo_d_adapt(int adapt, uint32_t rtt_mine, uint32_t rtt_peer, int d_base, int max_rb) {
    if (!adapt) return d_base;
    uint32_t rtt = rtt_mine > rtt_peer ? rtt_mine : rtt_peer;
    if (!rtt) return d_base;
    int d = (int)((rtt * 3u + 99u) / 100u) + 1 - max_rb;
    if (d < 2) d = 2;
    if (d > 4) d = 4;
    return d > d_base ? d : d_base;
}
/* v0.33.0 (OPTA-M2-SPEC.md §22.1): the at-limit badge's rule (presentation only). A window of the last RBO_LIM_WIN armed fight ticks, each 1 when the
 * stall lift stopped at the max_rb bound (this PC waited on the other). On: >= RBO_LIM_ON stops in the window (10 %); off again: <= RBO_LIM_OFF (5 %, so
 * the text does not flicker at the edge). S-B2's battles: 0.7 % stops at ~58 ms RTT, 35 % at ~346 ms, 80 % at ~607 ms. */
#define RBO_LIM_WIN 120
#define RBO_LIM_ON 12
#define RBO_LIM_OFF 6
static inline int rbo_lim_next(int on, int stops_in_window, int samples) {
    if (samples < RBO_LIM_WIN / 2) return 0;   /* fewer than 60 samples this battle: never at limit */
    if (!on) return stops_in_window >= RBO_LIM_ON;
    return stops_in_window > RBO_LIM_OFF;
}

/* ------------------------------------------------------------------ the ARM agreement (M2-INTERFACES.md §5.3)
 * v0.30.0 (OPTA-M2-SPEC.md §19.3, design_v0300/DESIGN_A.md §1): rounds are the GAME's indices. The game numbers a battle's rounds from 0 (V: the
 * intro's reset is r=9 from GameRound 0xffffffff, then r=0, 1, 2, ...); the intro is the battle's FIRST reset (counted, not by its number).
 * r_mine = the smallest round >= arm_round_cfg whose reset this PC has not yet seen (last_reset = the last real round whose reset this PC
 * saw this battle, -1 = none yet). v0.29.9 floored it at 1, so the first round (r=0) could never arm (S-B step 2: every battle armed one round
 * late). On the peer's ARM(r_peer): r = max(r_mine, r_peer) and ARM_ACK(r). Agreement(r) = ARM_ACK(r) sent AND the peer's ARM_ACK(r)
 * received (bit r of both masks; bit 0 is round 0). */
static inline uint32_t rbo_r_mine(int arm_round_cfg, int32_t last_reset) {
    int32_t r = arm_round_cfg > 0 ? arm_round_cfg : 0;
    if (last_reset + 1 > r) r = last_reset + 1;
    return (uint32_t)r;
}
/* the harness control h_rbo_r_floor1 (XARMR0): v0.29.9's rule, floored at round 1 */
static inline uint32_t rbo_r_mine_v0299(int arm_round_cfg, int32_t last_reset) { uint32_t r = rbo_r_mine(arm_round_cfg, last_reset); return r < 1 ? 1u : r; }
/* v0.30.0 (DESIGN_A §1.4): the peer's beacon round as an ordinal of this battle. The peer is at most one reset away from this PC (each reset
 * ends in a barrier that waits for the peer's code 0), so a beacon saying round 9 while this PC has not passed round 8 is the peer's INTRO
 * (ordinal -1), not a 10th round: a stale intro beacon read after this PC armed at r=0 must never look "ahead" of the armed round. */
static inline int32_t rbo_beacon_ord(uint32_t peer_round, int32_t last_reset) { return (peer_round == 9 && last_reset < 8) ? -1 : (int32_t)peer_round; }
static inline uint32_t rbo_ack_round(uint32_t r_mine, uint32_t r_peer) { return r_mine > r_peer ? r_mine : r_peer; }
static inline int rbo_agreed(uint32_t sent_mask, uint32_t recv_mask, uint32_t r) { return r < 32 && ((sent_mask & recv_mask) >> r & 1u); }

/* ------------------------------------------------------------------ v0.30.0: the zone lag (OPTA-M2-SPEC.md §19.2, design_v0300/DESIGN_A.md §2.4)
 * While ARMED, at every IsPaused pre: PIN (InputLagMin = InputLagMax = d) iff the core runs and no frame of the next `lead` frames
 * [s+1, s+lead] is a zone frame; otherwise STOCK (the game's own InputLagMin / InputLagMax, saved at ARM). The zones run lockstep (no
 * prediction, no lift): at d = 2 against a 3-5 frame round trip they waited on most frames (S-B step 2: ~33 ms per zone frame); at the game's
 * own adaptive InputLag (4-5) they run like the stock rounds. lead = rb_zone_entry_gap - max_rb - 1 (13): a KO zone (z = k + 21) is known when
 * k is confirmed, i.e. while the live frame is <= k + 6 < z - 13, so the switch frames are a function of the confirmed frames alone and are
 * the same on both PCs (no message). Returns 1 = PIN, 0 = STOCK. */
#define RBO_LAG_STOCK 0
#define RBO_LAG_PIN   1
static inline int rbo_lag_want(int armed, int core_on, int zone_ahead) { return armed && core_on && !zone_ahead ? RBO_LAG_PIN : RBO_LAG_STOCK; }

/* ------------------------------------------------------------------ v0.30.0: the frame-advantage time sync (OPTA-M2-SPEC.md §19.4, DESIGN_C §2.4)
 * mine_sum / mine_n = this PC's adv samples over its window (adv = GameTime - (SyncTime - 1) at IsPaused pre on eligible fight ticks), peer_sum /
 * peer_n = the peer's last fresh report. Returns the waits to schedule (0..max_w): only when this PC runs >= 0.5 frame ahead of its newest
 * remote input on average (the guard; guard = 0 is the harness control XTSBOGUS) AND half the difference of the two averages is >= band16 / 16
 * frame (the dead band, 16 = 1 frame; band16 = 0 is the control XTSQUIET: any positive half-difference waits at least once). Integers only. */
#define RBO_TS_WIN       16    /* the window, in eligible ticks */
#define RBO_TS_EVERY     8     /* a report every 8 samples */
#define RBO_TS_SETTLE    8     /* eligible ticks not sampled after each zone exit (the adv climb, the InputLag ramp to d) */
#define RBO_TS_MIN_N     12    /* samples both windows need */
#define RBO_TS_FRESH     24    /* a peer report older than this many of this PC's ticks is ignored */
#define RBO_TS_MAXW      4     /* waits per decision */
#define RBO_TS_GAP       3     /* ticks between two waits (one withheld lift = exactly one paused tick) */
#define RBO_TS_HOLD      32    /* eligible ticks after a decision's last wait */
#define RBO_TS_ROUND_MAX 60    /* waits per round; past it the time sync stops for the round (RB TSYNC STOP) */
#define RBO_TS_BAND16    16    /* the dead band in 1/16 frame */
static inline int rbo_ts_waits_x(int32_t mine_sum, uint32_t mine_n, int32_t peer_sum, uint32_t peer_n, uint32_t min_n, int max_w, int band16, int guard) {
    if (mine_n < min_n || peer_n < min_n || max_w <= 0 || !mine_n || !peer_n) return 0;
    if (guard && 2 * (int64_t)mine_sum < (int64_t)mine_n) return 0;                                                   /* guard: mine_avg < 0.5 */
    int64_t w16 = 8 * ((int64_t)mine_sum * (int64_t)peer_n - (int64_t)peer_sum * (int64_t)mine_n) / ((int64_t)mine_n * (int64_t)peer_n);   /* half-diff, 1/16 frame */
    if (w16 <= 0 || w16 < band16) return 0;                                                                            /* dead band */
    int64_t w = (w16 + 8) / 16;                                                                                        /* rounded to frames */
    if (w < 1) w = 1;                                                                                                  /* only reachable with band16 < 8 */
    return (int)(w > max_w ? max_w : w);
}
static inline int rbo_ts_waits(int32_t mine_sum, uint32_t mine_n, int32_t peer_sum, uint32_t peer_n, uint32_t min_n, int max_w) {
    return rbo_ts_waits_x(mine_sum, mine_n, peer_sum, peer_n, min_n, max_w, RBO_TS_BAND16, 1);
}
/* v0.30.0 (§19.4): a peer report a valid sender can produce: 1..16 samples, each clamped to [-8, 15] by the sender, so the sum lies in
 * [-8 n, 15 n]. Anything else is refused (never decided on). Found by TSBOGUS in the coupled model: the guard alone does not bound a bogus
 * report there (the honest peer re-balances this PC's waits, so the guard passes again; 32 waits per round on each PC, no desync) */
#define RBO_TS_ADV_MIN (-8)
#define RBO_TS_ADV_MAX 15
static inline int rbo_ts_report_ok(int32_t sum, uint32_t n) {
    return n >= 1 && n <= RBO_TS_WIN && (int64_t)sum >= (int64_t)RBO_TS_ADV_MIN * (int64_t)n && (int64_t)sum <= (int64_t)RBO_TS_ADV_MAX * (int64_t)n;
}

/* ------------------------------------------------------------------ the CHECK schedule (M2-INTERFACES.md §5.3 item 4) */
static inline int rbo_check_due(uint32_t f, uint32_t check_every) { return check_every && f && f % check_every == 0; }
/* v0.32.0 (OPTA-M2-SPEC.md §21.1): the CHECK over the chain of every confirmed frame. The window of an armed frame f >= 1 is (f - 1) / E (frames
 * 1..E = window 0, the CHECK at E closes it): it depends on the frame index alone, so both PCs' windows align even when one side's due frame is a
 * zone frame (that window is then never sent, as v0.31.0's CHECK at that frame). Each confirmed frame k this PC checks (outside a zone) is folded
 * into the window's chain (FNV-1a 64 over le32(k) || le64(t1x_k)), its count n and its frame set fset (FNV-1a 32 over le32(k)). A one-frame
 * divergence anywhere in the window changes the chain even when the window's last frame is equal again (S-B2: aif1 from 6999 passed the 7020 CHECK). */
#define RBO_CHAIN_BASIS 0xcbf29ce484222325ULL
#define RBO_FSET_BASIS  0x811c9dc5u
static inline uint32_t rbo_chain_window(uint32_t f, uint32_t check_every) { return check_every && f ? (f - 1) / check_every : 0; }
static inline uint64_t rbo_chain_step(uint64_t c, uint32_t f, uint64_t h) {
    for (int i = 0; i < 4; i++) { c ^= (uint8_t)(f >> (8 * i)); c *= 0x100000001b3ULL; }
    for (int i = 0; i < 8; i++) { c ^= (uint8_t)(h >> (8 * i)); c *= 0x100000001b3ULL; }
    return c;
}
static inline uint32_t rbo_fset_step(uint32_t s, uint32_t f) { for (int i = 0; i < 4; i++) { s ^= (uint8_t)(f >> (8 * i)); s *= 0x01000193u; } return s; }
/* one side's CHECK as compared (n = 0: no chain, v0.31.0's rule: the harness control h_rbo_chk_old) */
typedef struct { uint32_t f, round, gt, n, fset; uint64_t hash, chain; } RboCk;
enum { RBO_CK_OK = 0, RBO_CK_HASH = 1, RBO_CK_CHAIN = 2 };
/* the verdict of a CHECK pair: RBO_CK_HASH = frame f itself differs (v0.31.0's rule, or round / gt); RBO_CK_CHAIN = f is equal but an earlier frame
 * of the window differs (the chains differ over the same frame set); *comparable = both chained the same frames (n and fset equal, n > 0). A
 * window chained over different frame sets (a zone or an abort's tail rule seen on one side only) falls back to v0.31.0's rule: never a false DESYNC */
static inline int rbo_check_verdict(const RboCk *a, const RboCk *b, int *comparable) {
    int cmp = a->n && a->n == b->n && a->fset == b->fset;
    if (comparable) *comparable = cmp;
    if (a->hash != b->hash || a->round != b->round || a->gt != b->gt) return RBO_CK_HASH;
    if (cmp && a->chain != b->chain) return RBO_CK_CHAIN;
    return RBO_CK_OK;
}

/* ------------------------------------------------------------------ the drain repair (spec §6 step 3; OPTA-M0.md §1.6, input notes §4)
 * After the run-out every remote frame up to the present is confirmed. The values stock would hold had it run the confirmed inputs:
 *   0x1498[remote] = raw(S_last) (unmasked), 0x14a8[remote] = w(S_last), 0x14b8[remote] = w(S_last - 1)
 * with w(k) = rbo_word(raw(k), raw(k - 1), m12f8(k), c3(k)). raw_prev2 = raw(S_last - 2) feeds w(S_last - 1). */
typedef struct { uint32_t r1498; uint64_t w14a8, w14b8; } RboRepair;
static inline RboRepair rbo_repair_tm(uint32_t raw_last, uint32_t raw_prev, uint32_t raw_prev2, uint32_t m12_last, int c3_last, uint32_t m12_prev, int c3_prev,
                                      uint32_t tm_last, uint32_t tm_prev) {
    RboRepair r;
    r.r1498 = raw_last;
    r.w14a8 = rbo_word_tm(raw_last, raw_prev, m12_last, c3_last, tm_last);
    r.w14b8 = rbo_word_tm(raw_prev, raw_prev2, m12_prev, c3_prev, tm_prev);
    return r;
}
static inline RboRepair rbo_repair(uint32_t raw_last, uint32_t raw_prev, uint32_t raw_prev2, uint32_t m12_last, int c3_last, uint32_t m12_prev, int c3_prev) {
    return rbo_repair_tm(raw_last, raw_prev, raw_prev2, m12_last, c3_last, m12_prev, c3_prev, 0, 0);
}

/* ------------------------------------------------------------------ the Step-entry context measurement (spec §2 D-B; M2-INTERFACES.md §3.7)
 * From the M1 thunk's XSAVE area of the IFC Step hook: legacy area FCW +0 (u16), MXCSR +24, xmm i at +160 + 16 i; XSTATE_BV (u64) at +512;
 * the AVX component (ymm upper halves) at avx_off (CPUID 0xD.2 EBX). A component whose XSTATE_BV bit is clear is in its init state
 * (xmm = 0, FCW = 0x037f, ymm uppers = 0). xmm6 / xmm13 vary on hitch frames (the frame dt): their most frequent value is reported. */
#define RBO_CTX_MODES 4
typedef struct {
    uint32_t samples;
    uint32_t first[10];                 /* xmm6..15 dword 0 of the first sample */
    uint32_t mx_first, fcw_first;
    uint32_t vary;                      /* bit i: xmm(6+i) dword 0 differed from the first sample (i = 0 and 7 are expected to vary) */
    uint32_t mx_vary, fcw_vary, upper_nz;
    uint32_t mode_v[2][RBO_CTX_MODES], mode_n[2][RBO_CTX_MODES], mode_other[2];   /* [0] = xmm6, [1] = xmm13 */
} RboCtxAcc;
static inline uint32_t rbo_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void rbo_ctx_mode_add(RboCtxAcc *a, int k, uint32_t v) {
    for (int j = 0; j < RBO_CTX_MODES; j++) if (a->mode_n[k][j] && a->mode_v[k][j] == v) { a->mode_n[k][j]++; return; }
    for (int j = 0; j < RBO_CTX_MODES; j++) if (!a->mode_n[k][j]) { a->mode_v[k][j] = v; a->mode_n[k][j] = 1; return; }
    a->mode_other[k]++;
}
static inline void rbo_ctx_add(RboCtxAcc *a, const uint8_t *xa, uint32_t avx_off) {
    uint32_t bv = rbo_rd32(xa + 512);
    int sse = (bv & 2) != 0, avx = (bv & 4) != 0, x87 = (bv & 1) != 0;
    uint32_t v[10], upper = 0;
    for (int i = 0; i < 10; i++) {
        const uint8_t *x = xa + 160 + 16 * (6 + i);
        v[i] = sse ? rbo_rd32(x) : 0;
        if (sse) upper |= rbo_rd32(x + 4) | rbo_rd32(x + 8) | rbo_rd32(x + 12);
        if (avx && avx_off) { const uint8_t *y = xa + avx_off + 16 * (6 + i); upper |= rbo_rd32(y) | rbo_rd32(y + 4) | rbo_rd32(y + 8) | rbo_rd32(y + 12); }
    }
    uint32_t mx = rbo_rd32(xa + 24) & 0xffc0u, fcw = x87 ? ((uint32_t)xa[0] | (uint32_t)xa[1] << 8) : 0x037fu;
    if (!a->samples) { for (int i = 0; i < 10; i++) a->first[i] = v[i]; a->mx_first = mx; a->fcw_first = fcw; }
    else {
        for (int i = 0; i < 10; i++) if (v[i] != a->first[i]) a->vary |= 1u << i;
        if (mx != a->mx_first) a->mx_vary = 1;
        if (fcw != a->fcw_first) a->fcw_vary = 1;
    }
    if (upper) a->upper_nz = 1;
    rbo_ctx_mode_add(a, 0, v[0]); rbo_ctx_mode_add(a, 1, v[7]);
    a->samples++;
}
static inline uint32_t rbo_ctx_mode(const RboCtxAcc *a, int k) {
    uint32_t best = 0, bn = 0;
    for (int j = 0; j < RBO_CTX_MODES; j++) if (a->mode_n[k][j] > bn) { bn = a->mode_n[k][j]; best = a->mode_v[k][j]; }
    return best;
}
/* const_ok: xmm7-12, xmm14, xmm15, MXCSR control and FCW had exactly one value on every sample */
static inline int rbo_ctx_const_ok(const RboCtxAcc *a) { return a->samples && !(a->vary & ~((1u << 0) | (1u << 7))) && !a->mx_vary && !a->fcw_vary; }
/* the HELLO's ctx fields from the accumulator (xor3 = the harness plant h_rbo_hello_ctx_xor, XORed into ctx_xmm[3]) */
static inline void rbo_ctx_fill(RbmHello *h, const RboCtxAcc *a, uint32_t xor3) {
    for (int i = 0; i < 10; i++) h->ctx_xmm[i] = a->first[i];
    h->ctx_xmm[0] = rbo_ctx_mode(a, 0); h->ctx_xmm[7] = rbo_ctx_mode(a, 1);
    h->ctx_xmm[3] ^= xor3;
    h->ctx_mxcsr = a->mx_first; h->ctx_fcw = (uint16_t)a->fcw_first;
    h->ctx_upper_zero = (uint8_t)(a->samples && !a->upper_nz);
    h->ctx_const_ok = (uint8_t)rbo_ctx_const_ok(a);
    h->ctx_samples = a->samples;
}
/* the measured values equal the built-in L3-stock constants (xmm_lo[6..15], MXCSR, FCW; RB CTX builtin_eq=) */
static inline int rbo_ctx_builtin_eq(const RbmHello *h, const uint32_t xmm_lo16[16], uint32_t mxcsr, uint32_t fcw) {
    for (int i = 0; i < 10; i++) if (h->ctx_xmm[i] != xmm_lo16[6 + i]) return 0;
    return h->ctx_mxcsr == (mxcsr & 0xffc0u) && h->ctx_fcw == (uint16_t)fcw && h->ctx_upper_zero;
}
/* v0.29.1 (review M2): pairing also needs BOTH measured contexts to equal the built-in L3-stock constants (the compiled RBO_L3S_* values,
 * never a runtime copy): only then can a pass we run equal the pass the game would have run, which is what an asymmetric arm (one side
 * armed, the other native) relies on. RBM_V_BUILTIN otherwise (symmetric: both sides compute it from both HELLOs). */
static inline uint16_t rbo_verdict_builtin(const RbmHello *mine, const RbmHello *peer, const uint32_t xmm_lo16[16], uint32_t mxcsr, uint32_t fcw) {
    return (!rbo_ctx_builtin_eq(mine, xmm_lo16, mxcsr, fcw) || !rbo_ctx_builtin_eq(peer, xmm_lo16, mxcsr, fcw)) ? RBM_V_BUILTIN : 0;
}

/* ------------------------------------------------------------------ the 64-bit mixer (nonces, the content digest) */
static inline uint64_t rbo_mix64(uint64_t z) { z += 0x9e3779b97f4a7c15ULL; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL; z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL; return z ^ (z >> 31); }
static inline uint64_t rbo_fnv(uint64_t h, uint64_t v, int nbytes) { for (int i = 0; i < nbytes; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 0x100000001b3ULL; } return h; }
