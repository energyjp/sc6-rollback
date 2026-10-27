/* Option A M2 (rbonline): the rollback core's unit test of the PURE rules (src/rbo_logic.inc). Part of the rollback core.
 * A standalone program (it has its own main): tools/rbo_static.py compiles it as C (`clang -x c src/rbo_unit.inc`) into a temp/OUT dir and
 * runs it. Nothing here touches Windows, the game, the plugin's globals or the network. Exit 0 = every check passed.
 *
 * What it covers (and its blind spots):
 *   U1 the stock step-word rule against an independent transcription of FUN_1403fcd10 (dec_tick1.c:602-628), 10^6 random cases incl.
 *      the m12f8 and c3f0 masks and the carried prev; the XFETCH control (no c3f0) must differ on c3 frames with 0x3c0f bits.
 *   U2 the stall lift: the predicate's cases (state, results 1/3/4/5 never, the bound), and a tick-level model of IsPaused's one-tick lag
 *      (pause on the OLD PauseTime) with the remote input stalled: the bounded lift never runs a frame >= max_rb beyond SyncTime; the
 *      unbounded control (XLIFT) does; with no lift at all the model runs at most the stock frame (SyncTime - 1).
 *   U3 the state machine's legal edges.
 *   U4 HELLO verdicts: each field -> its bit, plugin_version / arm_round_min never compared.
 *   U5 the ARM agreement under every message order of two sides (both 2; 2 vs 3; a re-proposal after a missed margin).
 *   U6 the CHECK schedule (v0.32.0: + the chain: the window, the step functions, the verdict and its v0.31.0 control).
 *   U7 the drain repair = what stock holds after the confirmed sequence; the phantom-press control (a predicted prev left in 0x1498).
 *   U8 the Step-entry context accumulator on synthetic XSAVE areas (constant / varying / XSTATE_BV init / upper halves / builtin_eq).
 * Blind spots: the impure parts (the ring reads, the game writes, rb.c's rollback) are the harness's (RBO-* cases); this proves the rules
 * those parts call are the rules the spec states. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rbo_logic.inc"

static int g_fail, g_n;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint64_t g_rs = 0x2545F4914F6CDD1DULL;
static uint64_t rnd(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return g_rs; }

/* an independent transcription of FUN_1403fcd10's word construction (dec_tick1.c:602-628), online (param_4 = param_5 = 0) */
static void ref_fetch(uint32_t *prev1498, uint64_t *w14a8, uint64_t *w14b8, uint32_t raw, uint32_t m12f8, int c3) {
    *w14b8 = *w14a8;
    uint32_t uVar2 = *prev1498;
    *prev1498 = raw;
    uint32_t lo = raw, hi = (uVar2 ^ raw) & raw;
    lo &= ~m12f8; hi &= ~m12f8;
    if (c3) { lo &= 0xffffc3f0; hi &= 0xffffc3f0; }
    *w14a8 = (uint64_t)lo | (uint64_t)hi << 32;
}

static void u1_word(void) {
    uint32_t prev = 0; uint64_t w = 0, wb = 0; int differ_noc3 = 0, c3_frames = 0;
    for (int i = 0; i < 1000000; i++) {
        uint32_t raw = (rnd() & 7) ? (uint32_t)(rnd() & 0x3c1f) : (uint32_t)rnd();
        uint32_t m12 = (rnd() % 20 == 0) ? (uint32_t[]){ 0xf, 0x3c00, 0x3c0f, 0xffffffffu }[rnd() & 3] : 0;
        int c3 = rnd() % 10 == 0;
        uint64_t mine = rbo_word(raw, prev, m12, c3);
        ref_fetch(&prev, &w, &wb, raw, m12, c3);
        if (mine != w) { CHECK(0, "U1 rbo_word != stock at i=%d raw=%x m12=%x c3=%d", i, raw, m12, c3); break; }
        if (c3) { c3_frames++; if (rbo_word_noc3(raw, 0, m12) != rbo_word(raw, 0, m12, 1)) differ_noc3++; }
    }
    CHECK(c3_frames > 0 && differ_noc3 > 0, "U1 the XFETCH control (no c3f0 mask) differs on c3 frames with 0x3c0f bits (%d of %d)", differ_noc3, c3_frames);
    /* hand cases */
    CHECK(rbo_word(0x801, 0x800, 0, 0) == (0x801ULL | 0x1ULL << 32), "U1 pressed = the new bit only");
    CHECK(rbo_word(0x801, 0x801, 0, 0) == 0x801ULL, "U1 held without a new press");
    CHECK(rbo_word(0x3c1f, 0, 0x3c0f, 0) == (0x10ULL | 0x10ULL << 32), "U1 m12f8 masks both halves");
    CHECK(rbo_word(0x3c1f, 0, 0, 1) == (0x10ULL | 0x10ULL << 32), "U1 c3f0 masks 0x3c0f of both halves");
    CHECK(rbo_word(0x10000, 0, 0, 0) == (0x10000ULL | 0x10000ULL << 32), "U1 bits >= 16 pass (the local u32 raw)");
    /* v0.29.1 (review m4): the timers as stock applies them when not ignored: param_5 (0x146c) zeroes held, param_4 (0x1468) zeroes pressed */
    {   int bad = 0; uint32_t pv = 0; uint64_t w2 = 0, wb2 = 0;
        for (int i = 0; i < 200000 && !bad; i++) {
            uint32_t raw = (uint32_t)(rnd() & 0x3c1f), m12 = rnd() % 9 == 0 ? 0x3c0f : 0; int c3 = rnd() % 7 == 0; uint32_t tm = (uint32_t)(rnd() & 3);
            uint32_t prevraw = pv;
            ref_fetch(&pv, &w2, &wb2, raw, m12, c3);
            uint32_t lo = (tm & 1) ? 0 : raw, hi = (tm & 2) ? 0 : ((prevraw ^ raw) & raw);   /* dec_tick1.c:607-616 */
            lo &= ~m12; hi &= ~m12; if (c3) { lo &= 0xffffc3f0; hi &= 0xffffc3f0; }
            if (rbo_word_tm(raw, prevraw, m12, c3, tm) != ((uint64_t)lo | (uint64_t)hi << 32)) bad = 1;
            if (!tm && rbo_word_tm(raw, prevraw, m12, c3, 0) != w2) bad = 2;
        }
        CHECK(!bad, "U1 rbo_word_tm = the stock rule with the timers (bad=%d)", bad);
        CHECK(rbo_word_tm(0x801, 0, 0, 0, RBO_TM_HELD0) == (0x801ULL << 32) && rbo_word_tm(0x801, 0, 0, 0, RBO_TM_PRESSED0) == 0x801ULL, "U1 timer hand cases");
    }
}

/* a tick model of InputSync state 2 with the stall lift: SSM computes the result, IsPaused pauses on the OLD PauseTime (or result 1),
 * Advance increments GameTime, the step fetches frame GameTime - 1. The remote input arrives up to frame `arrive` (SyncTime = arrive + 1)
 * and then stops. Returns the largest (fetched frame - SyncTime) over the run; *ran = frames run. */
static int model(int max_rb, int lift, int unbounded, int arrive, int ticks, int *ran) {
    int gt = 0, pausetime = 0, worst = -1000, n = 0;
    int synctime = arrive + 1;
    for (int t = 0; t < ticks; t++) {
        int result = gt < synctime - 1 ? 0 : 2;                         /* SSM state 2 (dec_dirO_recv.c:260-269), no result 3 here */
        if (lift && rbo_lift_ok(2, (uint32_t)result, gt, synctime, max_rb, unbounded)) result = 0;
        int paused = pausetime != 0 || result == 1;                     /* IsPaused (dec_fi3.c:376-404) */
        pausetime = result ? pausetime + 1 : 0;
        if (paused) continue;
        gt++;                                                           /* Advance */
        int fetched = gt - 1;                                           /* UpdateBattle N = 1: remaining 0 */
        n++;
        if (fetched - synctime > worst) worst = fetched - synctime;
    }
    if (ran) *ran = n;
    return worst;
}
static void u2_lift(void) {
    CHECK(!rbo_lift_ok(1, 2, 10, 5, 7, 0), "U2 state 1 never lifts");
    CHECK(!rbo_lift_ok(5, 2, 10, 5, 7, 0), "U2 state 5 never lifts");
    for (uint32_t r = 0; r < 6; r++) if (r != 2) CHECK(!rbo_lift_ok(2, r, 10, 50, 7, 1), "U2 result %u is never touched", r);
    CHECK(rbo_lift_ok(2, 2, 10, 11, 7, 0), "U2 the stall frame lifts within the bound");
    CHECK(rbo_lift_ok(2, 2, 16, 11, 7, 0) && !rbo_lift_ok(2, 2, 17, 11, 7, 0), "U2 the bound: GameTime - (SyncTime - 1) < max_rb");
    CHECK(rbo_lift_ok(2, 2, 1000, 11, 7, 1), "U2 the unbounded control lifts anything");
    for (int mr = 1; mr <= 7; mr++) {
        int ran0, ran1, ran2;
        int w0 = model(mr, 0, 0, 100, 400, &ran0), w1 = model(mr, 1, 0, 100, 400, &ran1), w2 = model(mr, 1, 1, 100, 400, &ran2);
        CHECK(w0 <= -1, "U2 max_rb=%d stock (no lift): every fetched frame is confirmed (worst fetched - SyncTime = %d)", mr, w0);
        CHECK(w1 <= mr - 1 && w1 == mr - 1, "U2 max_rb=%d lift: the worst frame run is exactly max_rb - 1 beyond SyncTime (got %d)", mr, w1);
        CHECK(w2 > mr, "U2 max_rb=%d the XLIFT control runs beyond the bound (got %d)", mr, w2);
        CHECK(ran1 == ran0 + mr, "U2 max_rb=%d the lift runs exactly max_rb more frames than stock before stalling (%d vs %d)", mr, ran1, ran0);
    }
}

static void u3_edges(void) {
    static const int ok[6][6] = {
        /* to:  P  PR PA AR DR EN */
        /*P */ { 1, 1, 0, 0, 0, 1 },
        /*PR*/ { 1, 0, 1, 0, 0, 1 },
        /*PA*/ { 1, 0, 0, 1, 1, 1 },   /* v0.29.1: PAIRED -> DRAINED = the degraded arm (review M2) */
        /*AR*/ { 0, 0, 0, 0, 1, 1 },
        /*DR*/ { 0, 0, 0, 0, 0, 1 },
        /*EN*/ { 0, 0, 0, 0, 0, 0 } };
    for (int a = 0; a < 6; a++) for (int b = 0; b < 6; b++) CHECK(rbo_edge_ok(a, b) == ok[a][b], "U3 edge %d -> %d", a, b);
}

static void hello_base(RbmHello *h) {
    memset(h, 0, sizeof *h);
    h->proto = RBM_PROTO; strcpy(h->plugin_version, "0.29.0-optA-M2"); h->exe_ts = 0x61717836; h->exe_soi = 0x4ba2000;
    h->content = 0x1234; h->content_n[0] = 811; h->content_n[1] = 790;
    h->ctx_xmm[0] = 0x3c888888; h->ctx_xmm[1] = 0x47a8c000; h->ctx_xmm[2] = 0x7fffffff; h->ctx_xmm[3] = 0x47a8c000; h->ctx_xmm[7] = 0x3c888888;
    h->ctx_mxcsr = 0x1f80; h->ctx_fcw = 0x27f; h->ctx_upper_zero = 1; h->ctx_const_ok = 1; h->ctx_samples = 300;
    h->d = 2; h->max_rb = 7; h->check_every = 60; h->arm_round_min = 0;
}
static void u4_verdict(void) {
    RbmHello a, b;
    hello_base(&a); hello_base(&b);
    CHECK(rbo_verdict(&a, &b) == 0, "U4 equal -> 0");
    strcpy(b.plugin_version, "0.30.0-other"); b.arm_round_min = 3;
    CHECK(rbo_verdict(&a, &b) == 0, "U4 plugin_version and arm_round_min are not compared (a visual / other build never blocks pairing on them)");
    hello_base(&b); b.proto = RBM_PROTO + 1; CHECK(rbo_verdict(&a, &b) == RBM_V_PROTO, "U4 proto");
    hello_base(&b); b.exe_ts++; CHECK(rbo_verdict(&a, &b) == RBM_V_EXE, "U4 exe");
    hello_base(&b); b.content_n[1]++; CHECK(rbo_verdict(&a, &b) == RBM_V_CONTENT, "U4 content (the RBO-CONTENT case: 0x4)");
    hello_base(&b); b.content ^= 1; CHECK(rbo_verdict(&a, &b) == RBM_V_CONTENT, "U4 content digest");
    hello_base(&b); b.ctx_xmm[3] ^= 1; CHECK(rbo_verdict(&a, &b) == RBM_V_CTX, "U4 ctx (the RBO-HELLOCTX case: 0x8)");
    hello_base(&b); b.ctx_upper_zero = 0; CHECK(rbo_verdict(&a, &b) == RBM_V_CTX, "U4 ctx upper halves");
    hello_base(&b); b.ctx_samples = 0; CHECK(rbo_verdict(&a, &b) & RBM_V_NOCTX, "U4 no measurement -> NOCTX");
    hello_base(&b); b.ctx_const_ok = 0; CHECK(rbo_verdict(&a, &b) & RBM_V_NOCTX, "U4 a varying constant register -> NOCTX");
    hello_base(&b); b.d = 3; CHECK(rbo_verdict(&a, &b) == RBM_V_CFG, "U4 cfg d");
    hello_base(&b); b.check_every = 30; CHECK(rbo_verdict(&a, &b) == RBM_V_CFG, "U4 cfg check_every");
    hello_base(&b); b.flags = RBM_HF_RANKED; CHECK(rbo_verdict(&a, &b) == RBM_V_RANKED, "U4 ranked");
    /* v0.29.1 (review M2): both measured contexts must equal the built-in L3-stock constants */
    {   static const uint32_t bi[16] = { 0, 0, 0, 0, 0, 0, 0x3c888888, 0x47a8c000, 0x7fffffff, 0x47a8c000, 0, 0, 0, 0x3c888888, 0, 0 };
        hello_base(&b); CHECK(rbo_verdict_builtin(&a, &b, bi, 0x1f80, 0x27f) == 0, "U4 builtin: the stock context on both -> 0");
        hello_base(&a); hello_base(&b); a.ctx_xmm[0] = b.ctx_xmm[0] = 0x3c800000; a.ctx_xmm[7] = b.ctx_xmm[7] = 0x3c800000;
        CHECK(rbo_verdict(&a, &b) == 0 && rbo_verdict_builtin(&a, &b, bi, 0x1f80, 0x27f) == RBM_V_BUILTIN,
              "U4 builtin: both measure the SAME non-stock dt -> rbo_verdict alone pairs (the reviewed gap), the builtin bit refuses");
        hello_base(&a); hello_base(&b); b.ctx_xmm[3] = 0;
        CHECK(rbo_verdict_builtin(&a, &b, bi, 0x1f80, 0x27f) == RBM_V_BUILTIN && rbo_verdict_builtin(&b, &a, bi, 0x1f80, 0x27f) == RBM_V_BUILTIN, "U4 builtin: symmetric");
        hello_base(&a); hello_base(&b);
    }
}

/* two sides exchanging ARM / ARM_ACK, messages delivered in a given order. v0.30.0: last_reset = the last real round seen (-1 = none) */
typedef struct { int32_t max_reset; uint32_t prop, sent, recv; } Side;
static void side_on_arm(Side *s, uint32_t rp, uint32_t *ack_out) { uint32_t rm = rbo_r_mine(0, s->max_reset); if (s->prop > rm) rm = s->prop; uint32_t r = rbo_ack_round(rm, rp); s->sent |= 1u << r; *ack_out = r; }
static void u5_arm(void) {
    /* v0.30.0 U5' (OPTA-M2-SPEC.md §19.3): the game's round indices (0 = the first round); last_reset -1 = only the intro seen */
    CHECK(rbo_r_mine(0, -1) == 0 && rbo_r_mine(0, 0) == 1 && rbo_r_mine(2, -1) == 2 && rbo_r_mine(0, 1) == 2 && rbo_r_mine(2, 3) == 4, "U5' r_mine (game indices)");
    CHECK(rbo_r_mine_v0299(0, -1) == 1, "U5' control: v0.29.9's floor never proposes r=0 (rbo_r_mine_v0299(0,-1) = %u, so the check 'r_mine(0,-1) == 0' fails under it)",
          rbo_r_mine_v0299(0, -1));
    {   Side A = { -1, 0, 0, 0 }, B = { -1, 0, 0, 0 }; uint32_t ra, rb;   /* both in the intro, both propose 0 */
        A.prop = rbo_r_mine(0, -1); B.prop = rbo_r_mine(0, -1);
        side_on_arm(&A, B.prop, &ra); side_on_arm(&B, A.prop, &rb); A.recv |= 1u << rb; B.recv |= 1u << ra;
        CHECK(ra == 0 && rb == 0 && rbo_agreed(A.sent, A.recv, 0) && rbo_agreed(B.sent, B.recv, 0), "U5' both propose 0 -> agreement(0) (bit 0 is a round)");
    }
    {   Side A = { -1, 0, 0, 0 }, B = { 0, 1, 0, 0 }; uint32_t ra, rb;   /* B already saw r=0's reset: it proposes 1 */
        side_on_arm(&A, B.prop, &ra); side_on_arm(&B, A.prop, &rb); A.recv |= 1u << rb; B.recv |= 1u << ra;
        CHECK(ra == 1 && rb == 1 && rbo_agreed(A.sent, A.recv, 1) && rbo_agreed(B.sent, B.recv, 1) && !rbo_agreed(A.sent, A.recv, 0), "U5' 0 vs 1 -> agreement(1) only");
    }
    /* the beacon ordinal (DESIGN_A §1.4): round 9 while this PC has not passed round 8 is the peer's intro (-1); after round 8 it is round 9 */
    CHECK(rbo_beacon_ord(9, -1) == -1 && rbo_beacon_ord(9, 0) == -1 && rbo_beacon_ord(9, 7) == -1 && rbo_beacon_ord(9, 8) == 9 && rbo_beacon_ord(9, 9) == 9 &&
          rbo_beacon_ord(0, -1) == 0 && rbo_beacon_ord(1, 0) == 1 && rbo_beacon_ord(15, 14) == 15, "U5' the beacon ordinal map");
    CHECK(!(rbo_beacon_ord(9, 0) > 0) && (int32_t)9 > 0, "U5' an intro beacon is not 'ahead' of armed round 0 (the raw compare 9 > 0 is: XBCNR0)");
    for (int order = 0; order < 4; order++) {   /* both propose 2; deliveries interleaved four ways */
        Side A = { 1, 2, 0, 0 }, B = { 1, 2, 0, 0 }; uint32_t ra, rb;
        if (order & 1) { side_on_arm(&B, A.prop, &rb); side_on_arm(&A, B.prop, &ra); } else { side_on_arm(&A, B.prop, &ra); side_on_arm(&B, A.prop, &rb); }
        if (order & 2) { A.recv |= 1u << rb; B.recv |= 1u << ra; } else { B.recv |= 1u << ra; A.recv |= 1u << rb; }
        CHECK(rbo_agreed(A.sent, A.recv, 2) && rbo_agreed(B.sent, B.recv, 2), "U5 both propose 2 -> agreement(2), order %d", order);
    }
    {   Side A = { 1, 2, 0, 0 }, B = { 2, 3, 0, 0 }; uint32_t ra, rb;   /* B already saw round 2's reset */
        side_on_arm(&A, B.prop, &ra); side_on_arm(&B, A.prop, &rb);
        A.recv |= 1u << rb; B.recv |= 1u << ra;
        CHECK(ra == 3 && rb == 3 && rbo_agreed(A.sent, A.recv, 3) && rbo_agreed(B.sent, B.recv, 3) && !rbo_agreed(A.sent, A.recv, 2), "U5 2 vs 3 -> agreement(3) only");
    }
    {   Side A = { 1, 2, 0, 0 }, B = { 1, 2, 0, 0 }; uint32_t ra, rb;   /* only A's ACK(2) arrives before the round-2 reset: re-propose 3 */
        side_on_arm(&A, B.prop, &ra); side_on_arm(&B, A.prop, &rb);
        B.recv |= 1u << ra;   /* A never receives B's ACK(2) in time */
        CHECK(!rbo_agreed(A.sent, A.recv, 2) && rbo_agreed(B.sent, B.recv, 2), "U5 the two-generals residue is visible (B agreed, A not): the margin + BEACON handle it");
        A.max_reset = 2; B.max_reset = 2; A.prop = rbo_r_mine(0, 2); B.prop = rbo_r_mine(0, 2);
        side_on_arm(&A, B.prop, &ra); side_on_arm(&B, A.prop, &rb); A.recv |= 1u << rb; B.recv |= 1u << ra;
        CHECK(ra == 3 && rb == 3 && rbo_agreed(A.sent, A.recv, 3) && rbo_agreed(B.sent, B.recv, 3), "U5 the re-proposal agrees on 3");
    }
}

/* v0.30.0 U9 (OPTA-M2-SPEC.md §19.2): the zone lag's want, and U10 RBO-TS-RULE (§19.4): the time sync's decision table */
static void u9_lag(void) {
    for (int a = 0; a < 2; a++) for (int c = 0; c < 2; c++) for (int z = 0; z < 2; z++)
        CHECK(rbo_lag_want(a, c, z) == (a && c && !z ? RBO_LAG_PIN : RBO_LAG_STOCK), "U9 rbo_lag_want(armed=%d, core_on=%d, zone_ahead=%d)", a, c, z);
}
static int ts_avg(double mine, double peer, uint32_t n) {   /* both windows of n samples with the given averages (sums rounded) */
    int32_t ms = (int32_t)(mine * (double)n + (mine < 0 ? -0.5 : 0.5)), ps = (int32_t)(peer * (double)n + (peer < 0 ? -0.5 : 0.5));
    return rbo_ts_waits(ms, n, ps, n, RBO_TS_MIN_N, RBO_TS_MAXW);
}
static void u10_ts(void) {
    /* DESIGN_C §3.3's table (n = 16 unless named) */
    CHECK(ts_avg(6.0, -1.0, 16) == 4, "U10 (6, -1) -> 4 (got %d)", ts_avg(6.0, -1.0, 16));
    CHECK(rbo_ts_waits(94, 16, -16, 16, RBO_TS_MIN_N, RBO_TS_MAXW) == 3, "U10 (5.86, -0.99) -> 3 (got %d)", rbo_ts_waits(94, 16, -16, 16, RBO_TS_MIN_N, RBO_TS_MAXW));
    CHECK(rbo_ts_waits(46, 16, -16, 16, RBO_TS_MIN_N, RBO_TS_MAXW) == 2, "U10 (2.86, -1) -> 2 (got %d)", rbo_ts_waits(46, 16, -16, 16, RBO_TS_MIN_N, RBO_TS_MAXW));
    CHECK(ts_avg(1.0, -1.0, 16) == 1, "U10 (1, -1) -> 1 (got %d)", ts_avg(1.0, -1.0, 16));
    CHECK(rbo_ts_waits(14, 16, 12, 16, RBO_TS_MIN_N, RBO_TS_MAXW) == 0, "U10 (0.86, 0.72) -> 0 (the dead band)");
    CHECK(rbo_ts_waits(6, 16, -80, 16, RBO_TS_MIN_N, RBO_TS_MAXW) == 0, "U10 (0.4, -5) -> 0 (the guard: this PC is not ahead of its own input)");
    CHECK(rbo_ts_waits_x(6, 16, -80, 16, RBO_TS_MIN_N, RBO_TS_MAXW, RBO_TS_BAND16, 0) == 3, "U10 the guard's control (guard off): (0.4, -5) -> 3 (XTSBOGUS)");
    CHECK(ts_avg(-1.0, 6.0, 16) == 0, "U10 (-1, 6) -> 0 (the trailer never waits)");
    CHECK(ts_avg(6.0, -1.0, 11) == 0, "U10 n = 11 < MIN_N -> 0");
    CHECK(rbo_ts_waits(16 * 15, 16, 16 * -8, 16, RBO_TS_MIN_N, RBO_TS_MAXW) == RBO_TS_MAXW && rbo_ts_waits(16 * 15, 16, 16 * -8, 16, RBO_TS_MIN_N, 2) == 2, "U10 max_w caps");
    CHECK(rbo_ts_waits(64 * 127, 64, -64 * 127, 64, RBO_TS_MIN_N, 100) == 100 && rbo_ts_waits(-64 * 127, 64, 64 * 127, 64, RBO_TS_MIN_N, 100) == 0, "U10 no overflow at +-127 x 64");
    CHECK(rbo_ts_waits(16 * 2 - 1, 16, 0, 16, RBO_TS_MIN_N, RBO_TS_MAXW) == 0 && rbo_ts_waits(16 * 2, 16, 0, 16, RBO_TS_MIN_N, RBO_TS_MAXW) == 1, "U10 the band edge: half-diff just under 1 -> 0, 1 -> 1");
    CHECK(rbo_ts_waits_x(14, 16, 12, 16, RBO_TS_MIN_N, RBO_TS_MAXW, 0, 1) == 1, "U10 the band's control (band 0): a balanced pair (0.86, 0.72) waits (XTSQUIET)");
    /* the report plausibility (TSBOGUS): a valid sender clamps every sample to [-8, 15] */
    CHECK(rbo_ts_report_ok(-128, 16) && rbo_ts_report_ok(240, 16) && rbo_ts_report_ok(0, 1) && !rbo_ts_report_ok(-129, 16) && !rbo_ts_report_ok(241, 16) &&
          !rbo_ts_report_ok(-1600, 16) && !rbo_ts_report_ok(0, 0) && !rbo_ts_report_ok(0, 17), "U10 rbo_ts_report_ok: [-8 n, 15 n], 1 <= n <= 16");
}

static void u6_check(void) {
    CHECK(!rbo_check_due(0, 60) && !rbo_check_due(59, 60) && rbo_check_due(60, 60) && rbo_check_due(120, 60) && !rbo_check_due(61, 60), "U6 every 60 confirmed frames");
    CHECK(!rbo_check_due(60, 0), "U6 check_every 0 never");
    /* v0.32.0 (OPTA-M2-SPEC.md §21.1): the chain */
    CHECK(rbo_chain_window(1, 60) == 0 && rbo_chain_window(60, 60) == 0 && rbo_chain_window(61, 60) == 1 && rbo_chain_window(120, 60) == 1 && rbo_chain_window(121, 60) == 2,
          "U6 the window of f: frames 1..60 = 0 (closed by the CHECK at 60), 61..120 = 1");
    {   uint64_t c = RBO_CHAIN_BASIS; uint8_t b[12] = { 0x3c, 0, 0, 0, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01 }; uint64_t ref = 0xcbf29ce484222325ULL;
        for (int i = 0; i < 12; i++) { ref ^= b[i]; ref *= 0x100000001b3ULL; }
        c = rbo_chain_step(c, 60, 0x0123456789abcdefULL);
        CHECK(c == ref, "U6 rbo_chain_step = FNV-1a 64 over le32(f) || le64(hash)");
        uint32_t s = RBO_FSET_BASIS, r32 = 0x811c9dc5u; for (int i = 0; i < 4; i++) { r32 ^= b[i]; r32 *= 0x01000193u; } s = rbo_fset_step(s, 60);
        CHECK(s == r32, "U6 rbo_fset_step = FNV-1a 32 over le32(f)"); }
    {   /* a window of 60 frames on both sides; a one-frame difference at frame 30 that is equal again at 60 */
        RboCk a = { 60, 2, 1000, 0, RBO_FSET_BASIS, 0x77, RBO_CHAIN_BASIS }, b;
        for (uint32_t k = 1; k <= 60; k++) { a.chain = rbo_chain_step(a.chain, k, 0x1000 + k); a.fset = rbo_fset_step(a.fset, k); a.n++; }
        b = a; int cmp = -1;
        CHECK(rbo_check_verdict(&a, &b, &cmp) == RBO_CK_OK && cmp == 1, "U6 the same window on both sides: ok, comparable");
        b.chain = RBO_CHAIN_BASIS; b.fset = RBO_FSET_BASIS; b.n = 0;
        for (uint32_t k = 1; k <= 60; k++) { b.chain = rbo_chain_step(b.chain, k, 0x1000 + k + (k == 30)); b.fset = rbo_fset_step(b.fset, k); b.n++; }
        CHECK(rbo_check_verdict(&a, &b, &cmp) == RBO_CK_CHAIN && cmp == 1, "U6 a one-frame difference at f=30, equal at 60: RBO_CK_CHAIN");
        RboCk a7 = a, b7 = b; a7.n = b7.n = 0;
        CHECK(rbo_check_verdict(&a7, &b7, &cmp) == RBO_CK_OK && cmp == 0, "U6 control: v0.31.0's rule (n = 0, the frame's hash only) passes the same pair");
        RboCk c2 = b; c2.n = 59; c2.fset ^= 1;
        CHECK(rbo_check_verdict(&a, &c2, &cmp) == RBO_CK_OK && cmp == 0, "U6 different frame sets (a zone frame on one side): not comparable, the frame's hash decides");
        c2.hash ^= 1;
        CHECK(rbo_check_verdict(&a, &c2, &cmp) == RBO_CK_HASH, "U6 different frame sets and frame f differs: RBO_CK_HASH (v0.31.0's rule still holds)");
        RboCk d = a; d.gt++;
        CHECK(rbo_check_verdict(&a, &d, &cmp) == RBO_CK_HASH, "U6 gt differs: RBO_CK_HASH"); }
}

static void u7_repair(void) {
    for (int trial = 0; trial < 20000; trial++) {
        uint32_t raws[8], m12[8]; int c3[8];
        for (int i = 0; i < 8; i++) { raws[i] = (uint32_t)(rnd() & 0x3c1f); m12[i] = rnd() % 8 == 0 ? 0x3c0f : 0; c3[i] = rnd() % 8 == 0; }
        uint32_t p1498 = raws[0]; uint64_t w = rbo_word(raws[0], 0, 0, 0), wb = 0;
        for (int i = 1; i < 8; i++) ref_fetch(&p1498, &w, &wb, raws[i], m12[i], c3[i]);
        RboRepair r = rbo_repair(raws[7], raws[6], raws[5], m12[7], c3[7], m12[6], c3[6]);
        if (r.r1498 != p1498 || r.w14a8 != w || r.w14b8 != wb) { CHECK(0, "U7 repair != stock after the confirmed sequence (trial %d)", trial); return; }
    }
    CHECK(1, "U7 repair == stock (20000 sequences)");
    /* the phantom-press control: hand back with a predicted prev (0) while the confirmed raw held A (0x801): stock's next fetch of the same held
     * A presses again (OPTA-M0.md §1.6: 43% of frames) unless 0x1498 is repaired */
    uint32_t confirmed = 0x801, predicted_left = 0;
    CHECK(rbo_word(0x801, predicted_left, 0, 0) >> 32 == 0x801 && rbo_word(0x801, confirmed, 0, 0) >> 32 == 0, "U7 control: an unrepaired prev gives a phantom press");
}

static void xsave_put(uint8_t *xa, uint32_t bv, const uint32_t lo[16], uint32_t upper, uint32_t mx, uint16_t fcw, uint32_t avx_off) {
    memset(xa, 0, 1024);
    xa[0] = (uint8_t)fcw; xa[1] = (uint8_t)(fcw >> 8);
    memcpy(xa + 24, &mx, 4);
    for (int i = 0; i < 16; i++) { memcpy(xa + 160 + 16 * i, &lo[i], 4); if (upper && i == 9) memcpy(xa + 160 + 16 * i + 8, &upper, 4); }
    memcpy(xa + 512, &bv, 4);
    (void)avx_off;
}
static void u8_ctx(void) {
    static const uint32_t stock[16] = { 0, 0, 0, 0, 0, 0, 0x3c888888, 0x47a8c000, 0x7fffffff, 0x47a8c000, 0, 0, 0, 0x3c888888, 0, 0 };
    uint8_t xa[1024]; RboCtxAcc a; RbmHello h;
    memset(&a, 0, sizeof a);
    for (int i = 0; i < 100; i++) { uint32_t lo[16]; memcpy(lo, stock, sizeof lo); if (i % 50 == 7) { lo[6] = lo[13] = 0x3c8f5c29; } xsave_put(xa, 7, lo, 0, 0x1fa0, 0x27f, 576); rbo_ctx_add(&a, xa, 576); }
    memset(&h, 0, sizeof h); rbo_ctx_fill(&h, &a, 0);
    CHECK(h.ctx_samples == 100 && h.ctx_const_ok == 1 && h.ctx_upper_zero == 1, "U8 the stock context: const_ok, upper_zero, 100 samples");
    CHECK(h.ctx_xmm[0] == 0x3c888888 && h.ctx_xmm[7] == 0x3c888888, "U8 xmm6 / xmm13 report the most frequent value (hitch frames ignored)");
    CHECK(h.ctx_mxcsr == 0x1f80, "U8 MXCSR control bits only (the sticky status bits masked)");
    CHECK(rbo_ctx_builtin_eq(&h, stock, 0x1f80, 0x27f), "U8 builtin_eq on the stock constants");
    {   RbmHello g = h; rbo_ctx_fill(&g, &a, 1); CHECK(g.ctx_xmm[3] == (h.ctx_xmm[3] ^ 1), "U8 the h_rbo_hello_ctx_xor plant lands in ctx_xmm[3] (xmm9)"); }
    memset(&a, 0, sizeof a);
    for (int i = 0; i < 10; i++) { uint32_t lo[16]; memcpy(lo, stock, sizeof lo); if (i == 5) lo[9] = 0; xsave_put(xa, 7, lo, 0, 0x1f80, 0x27f, 576); rbo_ctx_add(&a, xa, 576); }
    memset(&h, 0, sizeof h); rbo_ctx_fill(&h, &a, 0);
    CHECK(h.ctx_const_ok == 0, "U8 a varying xmm9 -> const_ok 0 (pairing refuses: NOCTX)");
    memset(&a, 0, sizeof a);
    { uint32_t lo[16]; memcpy(lo, stock, sizeof lo); xsave_put(xa, 7, lo, 0x1234, 0x1f80, 0x27f, 576); rbo_ctx_add(&a, xa, 576); }
    memset(&h, 0, sizeof h); rbo_ctx_fill(&h, &a, 0);
    CHECK(h.ctx_upper_zero == 0, "U8 a nonzero dword above dword 0 -> upper_zero 0");
    memset(&a, 0, sizeof a);
    { uint32_t lo[16]; memcpy(lo, stock, sizeof lo); xsave_put(xa, 4 /* SSE and x87 in their init state */, lo, 0, 0x1f80, 0x1234, 576); rbo_ctx_add(&a, xa, 576); }
    memset(&h, 0, sizeof h); rbo_ctx_fill(&h, &a, 0);
    CHECK(h.ctx_xmm[3] == 0 && h.ctx_fcw == 0x37f, "U8 XSTATE_BV init components read as their init values (xmm 0, FCW 0x037f), not the stale bytes");
    CHECK(!rbo_ctx_builtin_eq(&h, stock, 0x1f80, 0x27f), "U8 builtin_eq 0 for a zero context (the XCTX plant's shape)");
}

/* v0.33.0 (OPTA-M2-SPEC.md §22.2, §22.3): U11 the delay both PCs compute from the two HELLOs (and its v0.32.0 control), the HELLO flag compare;
 * U12 the at-limit rule (on at 12 of 120, off at <= 6, never before 60 samples) on S-B2's three stop rates */
static void u11_delay(void) {
    CHECK(rbo_d_adapt(1, 60, 58, 2, 7) == 2 && rbo_d_adapt(1, 266, 0, 2, 7) == 2 && rbo_d_adapt(1, 267, 0, 2, 7) == 3 && rbo_d_adapt(1, 300, 120, 2, 7) == 3 &&
          rbo_d_adapt(1, 301, 0, 2, 7) == 4 && rbo_d_adapt(1, 346, 330, 2, 7) == 4 && rbo_d_adapt(1, 370, 365, 2, 7) == 4 && rbo_d_adapt(1, 607, 650, 2, 7) == 4,
          "U11 d = clamp(ceil(maxRTT * 0.03) + 1 - max_rb, 2, 4): 60 / 266 ms -> 2, 267 / 300 -> 3, 301 / 346 / 370 / 650 -> 4 (the cap)");
    CHECK(rbo_d_adapt(1, 0, 0, 2, 7) == 2 && rbo_d_adapt(1, 0, 370, 2, 7) == 4 && rbo_d_adapt(1, 370, 0, 2, 7) == 4, "U11 an unknown RTT (0) on one side: the other's; on both: d_base");
    CHECK(rbo_d_adapt(1, 370, 60, 2, 7) == rbo_d_adapt(1, 60, 370, 2, 7), "U11 the same inputs on both PCs give the same d (max of the two RTTs, either order)");
    CHECK(rbo_d_adapt(1, 60, 60, 3, 7) == 3 && rbo_d_adapt(1, 120, 120, 2, 4) == 2 && rbo_d_adapt(1, 200, 200, 2, 4) == 3, "U11 d_base is the floor; a smaller max_rb needs a larger d (120 ms -> 2, 200 ms -> 3 with max_rb 4)");
    CHECK(rbo_d_adapt(0, 370, 370, 2, 7) == 2, "U11 control: adapt = 0 (v0.32.0's fixed delay) keeps d = 2 at 370 ms");
    RbmHello a, b; memset(&a, 0, sizeof a); a.proto = RBM_PROTO; a.ctx_samples = 60; a.ctx_const_ok = 1; a.d = 2; a.max_rb = 7; a.check_every = 60;
    b = a; a.flags = RBM_HF_DADAPT; b.flags = RBM_HF_DADAPT; a.rtt_ms = 60; b.rtt_ms = 370;
    CHECK(rbo_verdict(&a, &b) == 0, "U11 the RTTs are not compared (each PC's own measurement)");
    b.flags = 0; CHECK(rbo_verdict(&a, &b) == RBM_V_CFG, "U11 one PC adapts, the other not: RBM_V_CFG (never pair on different delay rules)");
}
static void u12_limit(void) {
    struct { double rate; int want; const char *what; } k[] = { { 0.007, 0, "S-B2 battle 1 (~58 ms, 0.7 %)" }, { 0.35, 1, "S-B2 battle 3 (~346 ms, 35 %)" },
                                                              { 0.80, 1, "S-B2 battle 2 (~607 ms, 80 %)" }, { 0.05, 0, "5 %" }, { 0.15, 1, "15 %" } };
    for (size_t t = 0; t < sizeof k / sizeof k[0]; t++) {
        uint8_t win[RBO_LIM_WIN]; memset(win, 0, sizeof win); int n = 0, on = 0, ons = 0, samples = 0; double acc = 0;
        for (int i = 0; i < 2000; i++) {
            acc += k[t].rate; int stop = acc >= 1.0; if (stop) acc -= 1.0;   /* evenly spread stops at the rate */
            int j = i % RBO_LIM_WIN; n -= win[j]; win[j] = (uint8_t)stop; n += stop; samples++;
            on = rbo_lim_next(on, n, samples); if (i >= 1000) ons += on;
        }
        CHECK((ons > 900) == k[t].want && (ons < 100) == !k[t].want, "U12 %s: at limit %s (%d of the last 1000 ticks on)", k[t].what, k[t].want ? "on" : "off", ons);
    }
    CHECK(!rbo_lim_next(0, 120, 59) && rbo_lim_next(0, 12, 60) && !rbo_lim_next(0, 11, 120) && rbo_lim_next(1, 7, 120) && !rbo_lim_next(1, 6, 120),
          "U12 never before 60 samples; on at 12 stops of 120, off at <= 6 (hysteresis)");
}

/* v0.34.0 (OPTA-M2-SPEC.md §23.1): U13 the argc flag in the verdict, the refusal reason (the same on both PCs) and the on rule */
static void u13_argc(void) {
    RbmHello a, b; memset(&a, 0, sizeof a); a.proto = RBM_PROTO; a.ctx_samples = 60; a.ctx_const_ok = 1; a.d = 2; a.max_rb = 7; a.check_every = 60;
    a.flags = RBM_HF_DADAPT | RBM_HF_ARGC; b = a;
    CHECK(rbo_verdict(&a, &b) == 0 && rbo_argc_on(&a, &b), "U13 both advertise the argc fix: verdict 0, on");
    b.flags = RBM_HF_DADAPT;
    uint16_t va = rbo_verdict(&a, &b), vb = rbo_verdict(&b, &a);
    CHECK(va == RBM_V_ARGC && vb == RBM_V_ARGC, "U13 one advertises it, the other not: RBM_V_ARGC both ways (0x%x / 0x%x)", va, vb);
    CHECK(rbo_refuse_why(va, vb) == RBW_ARGC && rbo_refuse_why(vb, va) == RBW_ARGC, "U13 the refusal reason is argc_off on both PCs (the same union)");
    CHECK(!rbo_argc_on(&a, &b) && !rbo_argc_on(&b, &a), "U13 the fix is never on when only one PC advertises it");
    b.content = 1; va = rbo_verdict(&a, &b); vb = rbo_verdict(&b, &a);
    CHECK(rbo_refuse_why(va, vb) == RBW_MISMATCH && rbo_refuse_why(vb, va) == RBW_MISMATCH, "U13 argc plus another difference: why=mismatch on both (RB HELLO names the bits)");
    CHECK(rbo_refuse_why(RBM_V_CFG, 0) == RBW_MISMATCH && rbo_refuse_why(0, RBM_V_ARGC) == RBW_ARGC, "U13 a one-sided verdict union decides the same way");
    b = a; b.flags = RBM_HF_DADAPT; a.flags = RBM_HF_DADAPT;
    CHECK(rbo_verdict(&a, &b) == 0 && !rbo_argc_on(&a, &b), "U13 neither advertises it: they pair, the fix stays off (stock bytes)");
    CHECK(RBW_ARGC == 38 && RBW_N == 39 && RBM_PROTO == 10, "U13 RBW_ARGC 38 (appended), RBW_N 39, RBM_PROTO 10 (the name: unit_rbo RBO-P2P-WHY)");
}

int main(void) {
    u1_word(); u2_lift(); u3_edges(); u4_verdict(); u5_arm(); u6_check(); u7_repair(); u8_ctx(); u9_lag(); u10_ts();   /* v0.30.0: + U9 U10 */
    u11_delay(); u12_limit();   /* v0.33.0 */
    u13_argc();                 /* v0.34.0 */
    printf("RBO UNIT %d checks, %d failures\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
