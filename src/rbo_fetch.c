/* Option A M2: the armed FetchInput and the armed timeline's per-frame record (OPTA-M2-SPEC.md §5.2, §6; sc6rb/M2-INTERFACES.md §3.5).
 * Part of the rollback core.
 *
 * rbo_fetch_armed has FUN_1403fcd10's signature (rcx = mgr, edx = p, r8d = remaining, r9b = mgr+0x1468 > 0, [rsp+0x28] = mgr+0x146c > 0;
 * V: tools/disasm_at.py 0x1403fe6b8) and is reached only through the front door (src/rbo_install.c) while ARMED / DRAINED. It implements
 * the stock rule (src/rbo_logic.inc rbo_word; dec_tick1.c:585-628; the timers are ignored online, as CheckOnlineSessionActive forces):
 *   0x14b8[p] = 0x14a8[p]; 0x1498[p] = raw; 0x14a8[p] = word(raw, prev, mgr+0x12f8, FUN_1403f2220(mgr, p))
 * raw = the ring value when the slot holds (GameRound, frame) (FUN_1403f0720; V: online its vt+0x658 is `bOnline == 0`, so no frame
 * adjustment), for the remote only within the CONFIRMED FRONTIER (every earlier armed frame confirmed), else the PREDICTION: the last
 * confirmed raw (spec D-D). A remote slot that holds a frame beyond a gap is used once the gap fills (an extra predicted frame, never a
 * wrong one: rb compares the truth when it arrives). prev = the best-known raw of the previous fetch (= 0x1498[p] in stock), which is the
 * confirmed raw whenever the frame itself is confirmed, so a confirmed frame's word is byte-identical to stock's (D2). The UE-side arrays
 * (0x1498 / 0x14a8 / 0x14b8) are written as stock writes them; the drain and END repair 0x1498/0x14a8/0x14b8[remote] to the confirmed
 * values (rbo_fetch_repair). The predicate FUN_1403f2220 is called as stock calls it at this point (reads only).
 * The DRAINED form is the same function: the stall lift is off, so the game's own stall rule only lets confirmed frames run, except the
 * one frame the IsPaused lag lets through right after the last lift (still predicted; corrected by rb while the core runs, and when the
 * core has aborted it is VERIFIED when its truth arrives and a difference is a DESYNC: v0.29.1, review B2; it is never silently kept).
 * v0.29.1 (review m4): the timers too are the stock rule now: FUN_1403f2590 and FUN_1403f00b0 are called as stock calls them (before the
 * predicate); when neither says "ignore", the timer flags apply to the word exactly as stock (and are recorded per frame for the truth)
 * and the battle drains (rbo_fetch_unexp). Online they always say "ignore" (S-A: 95,154 live frames). */
#include "rbo_int.inc"
#include "rb.h"

volatile int rbo_fetch_mode;
uint64_t rbo_fetch_calls, rbo_fetch_pred, rbo_fetch_miss_drained;
volatile uint32_t rbo_fetch_unexp;
static uint32_t g_lost;                 /* v0.29.1 (B2): frames marked lost after an abort */
uint32_t rbo_fetch_lost(void) { return g_lost; }
static RboFrame *g_fr;                  /* RBO_FR_CAP frames (allocated by rbo_fetch_init at process start) */
static uint32_t g_front;                /* the confirmed frontier (rb frames) */
static uint32_t g_front_raw;            /* the remote's confirmed raw of the latest confirmed FETCH frame (the prediction) */
static uint32_t g_last_fetch;           /* the latest rb frame with a fetch */
static uintptr_t g_mgr, g_obj; static int g_remote;
static uint32_t g_prev_raw0[2];         /* 0x1498[p] at ARM: the raw of the fetch before armed frame 1 (confirmed: lockstep before ARM) */
static uint32_t g_prev_m12, g_mispred; static uint8_t g_prev_c3[2], g_prev_tm[2];
static uint64_t g_prev_word0[2];

#ifdef SC6RB_HARNESS
static int g_h_timers_old;              /* XTIMERS: the v0.29.0 rule (the timers ignored unconditionally, nothing reported) */
#endif
int rbo_fetch_init(void) {
    g_fr = (RboFrame *)VirtualAlloc(NULL, sizeof(RboFrame) * RBO_FR_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#ifdef SC6RB_HARNESS
    g_h_timers_old = cfg_geti("h_rbo_timers_old", 0);
    if (g_h_timers_old) LOG("RB CFG harness control h_rbo_timers_old=1 (XTIMERS: the armed FetchInput ignores the timers)\n");
#endif
    return g_fr != NULL;
}
RboFrame *rbo_frame(uint32_t f) {
    if (!f || !g_fr) return NULL;
    RboFrame *r = &g_fr[f % RBO_FR_CAP];
    return r->valid && r->f == f ? r : NULL;
}
static RboFrame *frame_new(uint32_t f) {
    RboFrame *r = &g_fr[f % RBO_FR_CAP];
    if (!r->valid || r->f != f) { memset(r, 0, sizeof *r); r->f = f; r->valid = 1; }
    return r;
}
uint32_t rbo_fetch_frontier(void) { return g_front; }
uint32_t rbo_fetch_mispredicts(void) { return g_mispred; }
uint32_t rbo_fetch_unconfirmed(void) { uint32_t s = rbo_core_last_f(); if (g_last_fetch > s) s = g_last_fetch; return s > g_front ? s - g_front : 0; }

/* the ring slot of (player p, round, frame): {tag(round) +0, frame +4, value +8, valid +0xc}; FUN_1403f0720: the value iff both tags match */
static int ring_read(uintptr_t obj, int p, uint32_t round, uint32_t ff, uint32_t *val) {
    if ((int32_t)ff < 0 || p < 0 || p > 1) return 0;
    volatile const uint32_t *c = (volatile const uint32_t *)(obj + NOBS_O_RING + (uintptr_t)p * 0x2000 + (uintptr_t)(ff & 0x1ff) * 0x10);
    if (c[0] != round || c[1] != ff) return 0;
    *val = c[2];
    return 1;
}
/* the previous fetch frame before f (skipping repeat steps): its record, or NULL = before ARM */
static RboFrame *prev_fetch(uint32_t f) {
    for (uint32_t k = f - 1; k >= 1 && f - k < RBO_FR_CAP; k--) { RboFrame *r = rbo_frame(k); if (!r) return NULL; if (r->fetched) return r; }
    return NULL;
}
/* the best-known raw of player p at frame f's previous fetch (stock: 0x1498[p]) */
static uint32_t prev_raw(uint32_t f, int p, uint32_t *m12, uint8_t *c3, uint8_t *tm) {
    RboFrame *q = prev_fetch(f);
    if (!q) { if (m12) *m12 = g_prev_m12; if (c3) *c3 = g_prev_c3[p]; if (tm) *tm = g_prev_tm[p]; return g_prev_raw0[p]; }
    if (m12) *m12 = q->m12f8;
    if (c3) *c3 = q->c3[p];
    if (tm) *tm = q->tm[p];
    return p == g_remote && q->conf ? q->craw : q->raw[p];
}

void rbo_fetch_arm(uintptr_t mgr, uintptr_t obj, int remote) {
    g_mgr = mgr; g_obj = obj; g_remote = remote;
    if (g_fr) memset(g_fr, 0, sizeof(RboFrame) * RBO_FR_CAP);
    g_front = 0; g_last_fetch = 0; g_mispred = 0; g_lost = 0; rbo_fetch_unexp = 0;
    g_prev_tm[0] = g_prev_tm[1] = 0;
    rbo_fetch_calls = rbo_fetch_pred = rbo_fetch_miss_drained = 0;
    uintptr_t a1498 = *(volatile uintptr_t *)(mgr + NOBS_M_PREV1498), a14a8 = *(volatile uintptr_t *)(mgr + NOBS_M_WORD14A8);
    for (int p = 0; p < 2; p++) {
        g_prev_raw0[p] = a1498 ? *(volatile uint32_t *)(a1498 + 4 * (uintptr_t)p) : 0;
        g_prev_word0[p] = a14a8 ? *(volatile uint64_t *)(a14a8 + 8 * (uintptr_t)p) : 0;
        g_prev_c3[p] = 0;
    }
    g_prev_m12 = *(volatile uint32_t *)(mgr + NOBS_M_MASK12F8);
    g_front_raw = g_prev_raw0[remote];
}

typedef char (*PredFn)(void *mgr, int p);
typedef char (*SessFn)(void);
typedef uintptr_t (*SetupFn)(void *mgr);
void rbo_fetch_armed(void *mgr_, int p, int remaining, char t1468, char t146c) {
    uintptr_t mgr = (uintptr_t)mgr_;
    uintptr_t obj = *(volatile uintptr_t *)(mgr + NOBS_M_INPUTOBJ);
    if (!obj) return;                                                   /* as stock: nothing without an input object */
    if (p < 0 || p > 1) { rbo_fetch_unexp |= 4u; return; }              /* v0.29.1 (review m3 e): UpdateBattle only ever asks for 0 and 1 */
    rbo_fetch_calls++;
    /* v0.29.1 (review m4): stock's order (dec_tick1.c:591-600): CheckOnlineSessionActive, the setup's +0x220, then the predicate */
    int act = ((SessFn)VA(RBO_A_SESS_ACTIVE))() != 0;
    uintptr_t su = ((SetupFn)VA(RBO_A_SETUP_OF))(mgr_);
    int ign = act || (su && *(volatile uint8_t *)(su + 0x220) != 0);
    uint32_t tm = ign ? 0u : ((t146c ? RBO_TM_HELD0 : 0u) | (t1468 ? RBO_TM_PRESSED0 : 0u));
#ifdef SC6RB_HARNESS
    if (g_h_timers_old) { tm = 0; act = 1; }
#endif
    if (!act) rbo_fetch_unexp |= 1u;
    if (tm) rbo_fetch_unexp |= 2u;
    int c3 = ((PredFn)VA(RBO_A_FETCH_PRED))(mgr_, p) != 0;               /* FUN_1403f2220(mgr, p), as stock calls it here */
#ifdef SC6RB_HARNESS
    if (g_cfg.h_rbo_fetch_no_c3f0) c3 = 0;                               /* XFETCH control */
#endif
    uintptr_t a14a8 = *(volatile uintptr_t *)(mgr + NOBS_M_WORD14A8), a14b8 = *(volatile uintptr_t *)(mgr + NOBS_M_WORD14B8),
              a1498 = *(volatile uintptr_t *)(mgr + NOBS_M_PREV1498);
    rbo_gw64(a14b8 + 8 * (uintptr_t)p, *(volatile uint64_t *)(a14a8 + 8 * (uintptr_t)p));   /* 0x14b8[p] = 0x14a8[p] */
    uint32_t round = *(volatile uint32_t *)(obj + NOBS_O_GAMEROUND), gt = *(volatile uint32_t *)(obj + NOBS_O_GAMETIME);
    uint32_t ff = gt - (uint32_t)remaining - 1;
    uint32_t f = rbo_core_last_f() + 1;                                  /* the rb frame of the step this fetch feeds */
    RboFrame *fr = frame_new(f);
    if (p == g_remote) rbo_fetch_confirm_scan(f - 1);                   /* this tick's ReceiveInputs may have confirmed earlier frames */
    uint32_t raw = 0;
    int ok = ring_read(obj, p, round, ff, &raw);
    if (p == g_remote) {
        if (!(ok && g_front + 1 >= f)) {                                 /* beyond the confirmed frontier: the prediction */
            raw = g_front_raw; fr->pred = 1; rbo_fetch_pred++;
            if (rbo_fetch_mode) rbo_fetch_miss_drained++;
        } else { fr->pred = 0; fr->conf = 1; fr->craw = raw; }
    } else if (!ok) raw = 0;                                             /* as stock (the local ring is always ahead: never seen) */
    uint32_t m12 = *(volatile uint32_t *)(mgr + NOBS_M_MASK12F8);
    /* prev: the best-known raw of the previous fetch. A predicted frame repeats the last confirmed raw with pressed 0 (what rb's make_inputs
     * runs: rbo_fetch_pred_word), so 0x14a8 and the core always see the same word */
    uint32_t prev = (p == g_remote && fr->pred) ? raw : prev_raw(f, p, NULL, NULL, NULL);
    uint64_t w = rbo_word_tm(raw, prev, m12, c3, tm);
    rbo_gw32(a1498 + 4 * (uintptr_t)p, raw);                            /* 0x1498[p] = raw (unmasked, stored even when 0) */
    rbo_gw32(a14a8 + 8 * (uintptr_t)p, (uint32_t)w);
    rbo_gw32(a14a8 + 8 * (uintptr_t)p + 4, (uint32_t)(w >> 32));
    fr->round = round; fr->ff = ff; fr->raw[p] = raw; fr->word[p] = w; fr->m12f8 = m12; fr->c3[p] = (uint8_t)c3; fr->tm[p] = (uint8_t)tm;
    fr->fetched |= (uint8_t)(1u << p);
    if (f > g_last_fetch) g_last_fetch = f;
}

/* the step of rb frame f begins (rbo_core, before rb's drain): a frame without a fetch is a 0x1462 repeat step (UpdateBattle re-runs
 * Step without FetchInput: the same words) */
void rbo_fetch_step_begin(uint32_t f) {
    RboFrame *fr = rbo_frame(f);
    if (fr && fr->fetched) return;
    fr = frame_new(f);
    fr->repeat = 1;
    RboFrame *q = rbo_frame(f - 1);
    if (q) { fr->round = q->round; fr->ff = q->ff; fr->raw[0] = q->raw[0]; fr->raw[1] = q->raw[1]; fr->word[0] = q->word[0]; fr->word[1] = q->word[1];
             fr->m12f8 = q->m12f8; fr->c3[0] = q->c3[0]; fr->c3[1] = q->c3[1]; fr->tm[0] = q->tm[0]; fr->tm[1] = q->tm[1];
             fr->pred = q->pred; fr->craw = q->craw; fr->conf = q->conf; }
    else { fr->word[0] = g_prev_word0[0]; fr->word[1] = g_prev_word0[1]; fr->conf = 1; }
}

/* the newly confirmed remote frames: advance the frontier while each next frame's remote slot holds (round, ff) */
int rbo_fetch_confirm_scan(uint32_t upto) {
    int n = 0;
    if (g_last_fetch > upto) upto = g_last_fetch;   /* a fetch for the step about to run is scanned too */
    while (g_front < upto) {
        uint32_t f = g_front + 1;
        RboFrame *fr = rbo_frame(f);
        if (!fr) break;
        if (fr->repeat) {   /* re-runs frame f - 1's words: confirmed with it (f - 1 <= the frontier here) */
            RboFrame *q = rbo_frame(f - 1);
            if (q) fr->craw = q->craw;
            fr->conf = 1;
        } else if (!fr->conf) {
            uint32_t v;
            if (!ring_read(g_obj, g_remote, fr->round, fr->ff, &v)) {
                /* v0.29.1 (review B2): after an abort no wall forces anything, and a frame whose round's ring was reset can never be read
                 * again: it is LOST (committed as it ran, counted, never verifiable; CHECK still covers the state) and the frontier moves on.
                 * Without this the frontier stuck at the reset and the drained FetchInput predicted the whole next round (XWALL). */
                if (!rb_is_aborted() || *(volatile uint32_t *)(g_obj + NOBS_O_GAMEROUND) == fr->round) break;
                fr->craw = fr->raw[g_remote]; fr->conf = 1; fr->forced = 2; g_lost++;
            } else {
                fr->craw = v; fr->conf = 1;
                if (fr->pred && v != fr->raw[g_remote]) g_mispred++;
            }
        }
        if (!fr->repeat && fr->fetched) g_front_raw = fr->craw;
        g_front = f; n++;
    }
    return n;
}
uint32_t rbo_fetch_force_confirm(void) {
    uint32_t n = 0, last = rbo_core_last_f();
    while (g_front < last) {
        RboFrame *fr = rbo_frame(g_front + 1);
        if (!fr) break;
        if (!fr->conf) { fr->craw = fr->raw[g_remote]; fr->conf = 1; fr->forced = 1; n++; }
        if (!fr->repeat && fr->fetched) g_front_raw = fr->craw;
        g_front++;
    }
    return n;
}

/* the truth of rb frame f (f <= the frontier): the local word as fetched, the remote word by the stock rule from the confirmed raws */
void rbo_fetch_truth(uint32_t f, uint64_t *local_w, uint64_t *remote_w, int *known) {
    *known = 0; *local_w = 0; *remote_w = 0;
    RboFrame *fr = rbo_frame(f);
    if (!fr || f > g_front) return;
    int L = 1 - g_remote;
    *local_w = fr->word[L];
    if (fr->repeat) {
        RboFrame *q = prev_fetch(f + 0);   /* the fetch whose words this repeat step re-runs */
        if (q && q->f < f) { int k2; uint64_t lw; rbo_fetch_truth(q->f, &lw, remote_w, &k2); *known = k2; return; }
        *remote_w = fr->word[g_remote]; *known = 1;
        return;
    }
    uint32_t prev = prev_raw(f, g_remote, NULL, NULL, NULL);
    *remote_w = rbo_word_tm(fr->craw, prev, fr->m12f8, fr->c3[g_remote], fr->tm[g_remote]);
#ifdef SC6RB_HARNESS
    if (g_cfg.h_rbo_fetch_no_c3f0) *remote_w = rbo_word_noc3(fr->craw, prev, fr->m12f8);
#endif
    *known = fr->forced ? 2 : 1;   /* v0.29.1 (B2): 2 = forced at a wall or lost after an abort (the "truth" is only what ran: unverifiable) */
}
/* the remote word an unconfirmed frame f runs on: held = the last confirmed raw (masked with f's own masks), pressed 0 */
uint64_t rbo_fetch_pred_word(uint32_t f) {
    RboFrame *fr = rbo_frame(f);
    uint32_t m12 = fr ? fr->m12f8 : g_prev_m12; int c3 = fr ? fr->c3[g_remote] : 0; uint32_t tm = fr ? fr->tm[g_remote] : 0;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_rbo_fetch_no_c3f0) c3 = 0;
#endif
    return rbo_word_tm(g_front_raw, g_front_raw, m12, c3, tm);
}

/* P4 (armed): the Step-entry words against what this step's fetch produced. 0 = equal, p + 1 = player p differs */
int rbo_fetch_tripwire(const uint64_t stepin[2], int have[2]) {
    uint32_t f = rbo_core_last_f() + 1;
    RboFrame *fr = rbo_frame(f);
    if (!fr || !fr->fetched) fr = rbo_frame(g_last_fetch);   /* a repeat step: the words of the last fetch */
    if (!fr) return 0;
    for (int p = 0; p < 2; p++) if (have[p] && (fr->fetched >> p & 1) && stepin[p] != fr->word[p]) return p + 1;
    return 0;
}

/* the hand-back repair (spec §6 step 3): once the last fetched frame and the two fetches before it are confirmed, 0x1498/0x14a8/0x14b8
 * [remote] = what stock would hold (rbo_logic.inc rbo_repair). 1 = written (or nothing to repair), 0 = not yet (unconfirmed). */
int rbo_fetch_repair(void) {
    if (!g_mgr || !g_last_fetch) return 1;
    RboFrame *L = rbo_frame(g_last_fetch);
    if (!L || !L->conf || g_last_fetch > g_front) return 0;
    uint32_t m12p; uint8_t c3p, tmp;
    uint32_t raw_prev = prev_raw(L->f, g_remote, &m12p, &c3p, &tmp);
    RboFrame *P = prev_fetch(L->f);
    uint32_t raw_prev2 = P ? prev_raw(P->f, g_remote, NULL, NULL, NULL) : g_prev_raw0[g_remote];
    RboRepair r = rbo_repair_tm(L->craw, raw_prev, raw_prev2, L->m12f8, L->c3[g_remote], m12p, c3p, L->tm[g_remote], tmp);
    uintptr_t a14a8 = *(volatile uintptr_t *)(g_mgr + NOBS_M_WORD14A8), a14b8 = *(volatile uintptr_t *)(g_mgr + NOBS_M_WORD14B8),
              a1498 = *(volatile uintptr_t *)(g_mgr + NOBS_M_PREV1498);
    if (!a14a8 || !a14b8 || !a1498) return 0;
    if (*(volatile uint32_t *)(a1498 + 4 * (uintptr_t)g_remote) != r.r1498) rbo_gw32(a1498 + 4 * (uintptr_t)g_remote, r.r1498);
    if (*(volatile uint64_t *)(a14a8 + 8 * (uintptr_t)g_remote) != r.w14a8) rbo_gw64(a14a8 + 8 * (uintptr_t)g_remote, r.w14a8);
    if (!P) r.w14b8 = g_prev_word0[g_remote];
    if (*(volatile uint64_t *)(a14b8 + 8 * (uintptr_t)g_remote) != r.w14b8) rbo_gw64(a14b8 + 8 * (uintptr_t)g_remote, r.w14b8);
    return 1;
}
