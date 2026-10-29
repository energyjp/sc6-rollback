/* LIPSYNC: v0.25.0-C1b3 PART B (C1B-R3-SPEC.md §3; C1B-R2-SPEC.md §3; REVERT-LIPSYNC.md). See lipsync.h.
 * Every public function returns at its first statement while lip-sync is off (lipsync=0, the key absent, or refused), so that build path
 * writes no log line, changes no title, no hash and allocates nothing. */
#include "common.h"
#include "modes.h"
#include "pres.h"
#include "rb.h"
#include "lipsync.h"

static int g_on;                     /* lipsync=1 accepted (rbreplay / rblive) */
/* v0.29.4 FACES (DESIGN.md §3.1): rbonline. g_avail = lipsync=1 accepted at process start; g_rbo = the mode is rbonline, where g_on follows the
 * armed battle (on in lipsync_battle_begin = ARM, off after its LIPSYNCSTAT = END); g_h_gate_off = harness control XLIPSG (on from startup) */
static int g_avail, g_rbo, g_h_gate_off;

/* the KEEP blocks: 8 = 2 players x k_pres_p (built once, on first use; the table is const in the plugin build; the harness's h_pres_old
 * tables have up to 7 ranges per player) */
#define LB_MAX 16
typedef struct { uintptr_t addr; uint32_t len, lo; int p; const char *name; } LBlk;
static LBlk g_b[LB_MAX]; static int g_nb; static size_t g_pbytes;
static uint8_t *g_keep;              /* KEEP after UE's call (keep_begin) */
static uint8_t *g_rest;              /* KEEP right before a restore (before_restore) */
static int g_built;

/* fix (review "P never rolls back covers core-written fields"): the core writes these PLAYER-relative spans every step (C1B-R2-SPEC.md
 * §2.3 F2.1: +0x292f0 (u16 copy of +0x19c2), +0x29304 (u32 flag of +0x16fa), +0x29690..+0x296af (the +0x2a0 vector copy at +0x296a0);
 * gate-a/dumps/dec_step1.c:12517/12547, dec_stepall.c:21529). They must roll back with the restore. v0.25: they lie in POSE (face_a, face_d),
 * so KEEP holds none of them: build_segs asserts the intersection is 0 bytes (C1B-R3-SPEC.md §3.2). Only the harness's old tables
 * (h_pres_old=1|2) still intersect: there the spans are carved out of the write-back as in v0.24 (the h_lips_skip_site=core control writes
 * them back too). */
static const struct { uint32_t lo, hi; } k_core_w[] = { { 0x292f0, 0x292f8 }, { 0x29304, 0x29308 }, { 0x29690, 0x296b0 } };
#define LS_MAX 64
typedef struct { uintptr_t addr; uint32_t len, roff; int blk; } LSeg;   /* roff = the offset in g_rest */
static LSeg g_seg[LS_MAX]; static int g_nseg; static uint32_t g_core_bytes, g_core_isect;

/* fix (review "pk_apply writes a freed UE heap pointer back"): the heap blocks the step thread freed inside forwarded calls (modes.c
 * l4_note_heap_free) since the last rollback restore, i.e. during the resims; pk_apply (lipsync_pk_fix) never puts back a live KEEP word that
 * points into one of them. */
/* C1B3 fix (round-3 review): 1024 -> 4096, as modes.c L4F_MAX: one mid-fight +0x0c0, delivered inside a rollback's confirmed resim since v0.25,
 * frees 1,898-2,749 blocks in game (round-2 L4DANGLE overflow lines); at 1024 every such event overflowed (freelist_full, a partial check).
 * Harness only: g_lips_h_lfcap (> 0) lowers the cap for the negative control KOCLX. */
#define LF_MAX 4096
#ifdef SC6RB_HARNESS
int g_lips_h_lfcap;
#define LF_CAP (g_lips_h_lfcap > 0 && g_lips_h_lfcap < LF_MAX ? g_lips_h_lfcap : LF_MAX)
#else
#define LF_CAP LF_MAX
#endif
static uintptr_t g_fl_lo[LF_MAX], g_fl_hi[LF_MAX]; static int g_nfl, g_fl_armed;

/* per battle */
static uint64_t g_bat; static int g_stat_pending;
static uint64_t g_kept_calls, g_kept_bytes, g_restores, g_guard_checks, g_nonp, g_lpd_changed, g_ptr_bad, g_resim_rev;
static uint64_t g_pk_freed, g_free_over, g_pkd_bydesign;   /* fix */
static int g_nonp_lines, g_lpd_lines, g_ptr_lines, g_pkf_lines, g_fo_lines;
static int g_pkd_lines[LB_MAX];      /* fix: PRESKEEPD per-block budget (lipsync=1) */
static int g_fail, g_trip;           /* LIPSYNCGUARD failed this battle / not yet taken by rb.c */
static int g_armed = -1;             /* the site of the hooked restore whose first following pass is checked (-1 none) */
static uint64_t g_guard_early, g_keep_armed; static int g_keep_armed_lines;   /* v0.36.0 (rule C): checks made by pres_flush before late calls; kept calls while a guard
                                                                               * was armed (the tripwire: 0 by construction outside the control LSRVX) */
static uint64_t g_lpd[2][2]; static int g_lpd_seen[2][2];   /* [player][0: +0x95ec0, 1: +0x95f50] */
static uint64_t g_pchk[2][2];        /* [player][0: +0x95ed8, 1: +0x2b270]: the last value checked */

/* the current forwarded call (keep_begin -> keep_end) */
static int g_kb_valid, g_kb_slot; static const uint8_t *g_kb_pre; static uintptr_t g_kb_lo;
static uint8_t g_chg[LB_MAX], g_drop[LB_MAX];

/* v0.25 LIPSYNCEV (C1B-R3-SPEC.md §3.3): the budget, its segments and the per-player frame windows */
#define EV_BUDGET_DEF 300
#define EV_WIN 30
#define EV_NSEG 3
static int g_ev_budget, g_ev_used, g_ev_seg, g_ev_seg_used, g_ev_lines_b, g_ev_mode_prev;
static uint64_t g_ev_dropped, g_ev_same;
static uint32_t g_ev_maxk;                         /* the newest frame whose pass started (frame lines and segments move on NEW frames only) */
static uint32_t g_win_lo[2], g_win_hi[2];          /* window (lo, hi] of player c (hi = 0: none) */
static uint32_t g_fp_last[2];                      /* the last frame an ev=fp line was considered for player c */
typedef struct { uint32_t act, init, vst; uint64_t lpd, ldp, l3m, l5m; int moving; } EvFld;
static EvFld g_win_last[2]; static int g_win_logged[2]; static double g_win_mmax[2];
/* the last ev=keep / ev=fp of player c: an event whose kind, slot and fields all equal it is counted (ev_same=), not logged and opens no
 * window (a listener slot that writes KEEP on every frame without moving a logged field would otherwise use up the budget) */
static EvFld g_ev_last[2]; static int g_ev_last_slot[2]; static char g_ev_last_kind[2];

/* harness-only controls (C1B-R2-SPEC.md §4.1, C1B-R3-SPEC.md §4.1; the plugin build never reads them: g_cfg fields stay 0 / empty there) */
static int g_h_no_after, g_h_skip = -1;
static const char *const k_site[LIPS_SITE_N] = { "rollback", "rpb", "rpb_scratch", "fp", "core", "pk", "pkd", "evseg", "pkd_v024", "flush" };   /* v0.36.0: + flush */

int lipsync_on(void) { return g_on; }

static void build_segs(int with_core) {
    g_nseg = 0; g_core_bytes = 0; g_core_isect = 0;
    uint32_t roff = 0;
    for (int i = 0; i < g_nb; i++) {
        uint32_t lo = g_b[i].lo, hi = g_b[i].lo + g_b[i].len, cur = lo;
        for (size_t j = 0; j < sizeof k_core_w / sizeof k_core_w[0]; j++) {
            uint32_t a = k_core_w[j].lo < lo ? lo : k_core_w[j].lo, b = k_core_w[j].hi > hi ? hi : k_core_w[j].hi;
            if (a >= b || a < cur) continue;
            g_core_isect += b - a;   /* v0.25: the intersection of k_core_w with the table (0 with KEEP) */
            if (!with_core) continue;
            if (a > cur && g_nseg < LS_MAX) { LSeg *s = &g_seg[g_nseg++]; s->addr = g_b[i].addr + (cur - lo); s->len = a - cur; s->roff = roff + (cur - lo); s->blk = i; }
            g_core_bytes += b - a;
            cur = b;
        }
        if (hi > cur && g_nseg < LS_MAX) { LSeg *s = &g_seg[g_nseg++]; s->addr = g_b[i].addr + (cur - lo); s->len = hi - cur; s->roff = roff + (cur - lo); s->blk = i; }
        roff += g_b[i].len;
    }
}
static int build(void) {
    if (g_built) return g_keep != NULL;
    g_built = 1;
    g_nb = 0; g_pbytes = 0;
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < k_pres_np && g_nb < LB_MAX; i++) {
            LBlk *b = &g_b[g_nb++];
            b->addr = VA(A_PLAYER0) + (uintptr_t)c * PLAYER_STRIDE + k_pres_p[i].lo;
            b->len = k_pres_p[i].hi - k_pres_p[i].lo; b->lo = k_pres_p[i].lo; b->p = c; b->name = k_pres_p[i].name;
            g_pbytes += b->len;
        }
    g_keep = (uint8_t *)VirtualAlloc(NULL, 2 * g_pbytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rest = g_keep ? g_keep + g_pbytes : NULL;
    if (!g_keep) LOG("LIPSYNC WARN no buffer (%zu bytes): lip-sync stays off\n", 2 * g_pbytes);
    return g_keep != NULL;
}

void lipsync_set_active(int on) {
    if (!on) return;
    g_on = 0;
    g_h_no_after = g_cfg.h_lips_no_after != 0;
    g_h_skip = -1;
    for (int i = 0; i < LIPS_SITE_N; i++) if (g_cfg.h_lips_skip_site[0] && !_stricmp(g_cfg.h_lips_skip_site, k_site[i])) g_h_skip = i;
    int old_tbl = 0;
#ifdef SC6RB_HARNESS
    old_tbl = g_cfg.h_pres_old;   /* the harness's old P tables (1 v0.23 / 2 v0.24): they still hold the core-written spans */
    g_ev_budget = g_cfg.h_lips_ev_budget;
#else
    g_ev_budget = EV_BUDGET_DEF;
#endif
    if (!build()) return;
    build_segs(g_h_skip != LIPS_SITE_CORE);
    if (g_core_isect && !old_tbl) {   /* C1B-R3-SPEC.md §3.2: KEEP never holds a core-written span; a table that does is refused loudly */
        LOG("CFG LIPSYNC REFUSE why=core_in_keep core_bytes=%u p_ranges=%d p_bytes=%zu (the KEEP table holds core-written spans: lip-sync stays off, title lips=REFUSED)\n",
            g_core_isect, g_nb, g_pbytes);
        g_title_dirty = 1;
        return;
    }
    if (g_cfg.mode == MODE_RBONLINE) { g_rbo = 1; g_avail = 1; }   /* v0.29.4 FACES: rbonline: available now, on only while a battle is armed */
#ifdef SC6RB_HARNESS
    if (g_rbo) g_h_gate_off = cfg_geti("h_lips_rbo_gate_off", 0) != 0;   /* v0.29.4 FACES: harness control XLIPSG */
#endif
    g_on = 1;
    LOG("CFG LIPSYNC on=1 mode=%s p_ranges=%d p_bytes=%zu core_bytes=%u (PART B v0.25: UE's lip/voice state writes into KEEP stand on live passes, never rolled back; "
        "POSE (face rows, face_a/c/d) and the core-written spans roll back; LIPSYNCEV budget %d/battle; REVERT-LIPSYNC.md)%s\n",
        g_mode_names[g_cfg.mode], g_nb, g_pbytes, g_core_bytes, g_ev_budget,
        g_rbo ? " gate=armed (v0.29.4: rbonline runs lip-sync from ARM to END of each armed battle only; off while PASSIVE / PROBE / PAIRED)" : "");   /* v0.29.4 FACES: + %s */
    if (g_h_no_after || g_h_skip >= 0 || g_cfg.h_lips_skip_site[0] || old_tbl || g_ev_budget != EV_BUDGET_DEF)
        LOG("CFG LIPSYNC harness control h_lips_no_after=%d h_lips_skip_site=%s h_pres_old=%d h_lips_ev_budget=%d core_isect=%u (%s)\n", g_h_no_after,
            g_h_skip >= 0 ? k_site[g_h_skip] : g_cfg.h_lips_skip_site[0] ? "UNKNOWN" : "-", old_tbl, g_ev_budget, g_core_isect,
            g_h_skip >= 0 || g_h_no_after || old_tbl ? "a negative control: one PART B protection is off" : g_cfg.h_lips_skip_site[0] ? "unknown site name: ignored" : "a test budget");
    if (g_rbo && g_h_gate_off) LOG("CFG LIPSYNC harness control h_lips_rbo_gate_off=1 (a negative control: lip-sync on from startup, PASSIVE included)\n");   /* v0.29.4 FACES: */
    if (g_rbo) g_on = g_h_gate_off;   /* v0.29.4 FACES: rbonline starts PASSIVE: off until lipsync_battle_begin (ARM) */
}

static void stat_line(const char *why) {
    LOG("LIPSYNCSTAT b=%llu why=%s kept_calls=%llu kept_bytes=%llu restores_covered=%llu guard_checks=%llu nonp=%llu lpd_changed=%llu ptr_bad=%llu resim_p_reverted=%llu "
        "pk_freed=%llu free_over=%llu pkd_bydesign=%llu ev_lines=%d ev_dropped=%llu ev_same=%llu guard_early=%llu keep_armed=%llu\n",   /* v0.36.0: + guard_early, keep_armed (appended) */
        (unsigned long long)g_bat, why, (unsigned long long)g_kept_calls, (unsigned long long)g_kept_bytes, (unsigned long long)g_restores,
        (unsigned long long)g_guard_checks, (unsigned long long)g_nonp, (unsigned long long)g_lpd_changed, (unsigned long long)g_ptr_bad, (unsigned long long)g_resim_rev,
        (unsigned long long)g_pk_freed, (unsigned long long)g_free_over, (unsigned long long)g_pkd_bydesign, g_ev_lines_b, (unsigned long long)g_ev_dropped,
        (unsigned long long)g_ev_same, (unsigned long long)g_guard_early, (unsigned long long)g_keep_armed);
    log_flush();
}
void lipsync_battle_begin(uint64_t b) {
    if (g_rbo && g_avail) g_on = 1;   /* v0.29.4 FACES: rbonline calls rb_battle_begin only from modes_rbo_arm: on from ARM */
    if (!g_on) return;
    if (g_stat_pending) stat_line("shutdown");   /* the previous battle never reached its end */
    g_bat = b; g_stat_pending = 1;
    g_kept_calls = g_kept_bytes = g_restores = g_guard_checks = g_nonp = g_lpd_changed = g_ptr_bad = g_resim_rev = 0;
    g_pk_freed = g_free_over = g_pkd_bydesign = 0;
    g_guard_early = g_keep_armed = 0; g_keep_armed_lines = 0;   /* v0.36.0 */
    g_nonp_lines = g_lpd_lines = g_ptr_lines = g_pkf_lines = g_fo_lines = 0;
    memset(g_pkd_lines, 0, sizeof g_pkd_lines);
    g_fail = 0; g_trip = 0; g_armed = -1; g_kb_valid = 0; g_nfl = 0; g_fl_armed = 0;
    memset(g_lpd, 0, sizeof g_lpd); memset(g_lpd_seen, 0, sizeof g_lpd_seen); memset(g_pchk, 0, sizeof g_pchk);
    g_ev_used = g_ev_seg = g_ev_seg_used = g_ev_lines_b = 0; g_ev_dropped = g_ev_same = 0; g_ev_maxk = 0; g_ev_mode_prev = -1;
    memset(g_ev_last_kind, 0, sizeof g_ev_last_kind);
    memset(g_win_lo, 0, sizeof g_win_lo); memset(g_win_hi, 0, sizeof g_win_hi); memset(g_fp_last, 0, sizeof g_fp_last);
    memset(g_win_logged, 0, sizeof g_win_logged);
    g_title_dirty = 1;
}
void lipsync_battle_end(uint64_t b) {
    if (!g_on) return;
    if (g_rbo && !g_stat_pending) { g_on = g_h_gate_off; return; }   /* v0.29.4 FACES: rbonline: off at END */
    if (!g_stat_pending) return;
    g_bat = b; g_stat_pending = 0;
    stat_line("battle_end");
    if (g_rbo) g_on = g_h_gate_off;   /* v0.29.4 FACES: rbonline calls rb_battle_end only from modes_rbo_end: off after END's LIPSYNCSTAT */
}

/* ------------------------------------------------------------------ v0.25 LIPSYNCEV (C1B-R3-SPEC.md §3.3: diagnostic only) */
/* The fields, read from PLAYER c (after-values): act / init = +0x95ef8 / +0x95efc (u32), lpd = +0x95ed8, ldp = +0x2b270 (layer 5's data
 * pointer), l3m / l5m = the masks of face layers 3 / 5 (layer n at +0x2aef8 + n*0xb0: { h0 Q, ptr Q, clip I, t f, mask Q }, presentation analysis
 * pres/layers.py: +0x2b120 / +0x2b280), vst = +0x972b0 (u32). */
static void ev_read(int c, EvFld *e) {
    uintptr_t pb = VA(A_PLAYER0) + (uintptr_t)c * PLAYER_STRIDE;
    e->act = *(volatile uint32_t *)(pb + 0x95ef8); e->init = *(volatile uint32_t *)(pb + 0x95efc);
    e->lpd = *(volatile uint64_t *)(pb + 0x95ed8); e->ldp = *(volatile uint64_t *)(pb + 0x2b270);
    e->l3m = *(volatile uint64_t *)(pb + 0x2aef8 + 3 * 0xb0 + 0x18); e->l5m = *(volatile uint64_t *)(pb + 0x2aef8 + 5 * 0xb0 + 0x18);
    e->vst = *(volatile uint32_t *)(pb + 0x972b0);
    e->moving = 0;
}
/* mouth: max over rows 86..96 of |(row_t - row_5)_cur - (row_t - row_5)_prev| (the translation, floats 12..14 of the 4x4 row), read through
 * the bank object's +0x28 (cur) / +0x30 (prev) array pointers (PLAYER+0x35c8 / +0x35d0); both must be one of this player's three arrays
 * (base pres_bank_off(c) + b*0xc000), else -1 (mouth=-) */
static double ev_mouth(int c) {
    uintptr_t pb = VA(A_PLAYER0) + (uintptr_t)c * PLAYER_STRIDE, base = pb + pres_bank_off(c);
    uintptr_t cur = *(volatile uint64_t *)(pb + 0x35c8), prv = *(volatile uint64_t *)(pb + 0x35d0);
    int okc = 0, okp = 0;
    for (int b = 0; b < 3; b++) { uintptr_t a = base + (uintptr_t)b * PRES_BANK_STRIDE; okc |= cur == a; okp |= prv == a; }
    if (!okc || !okp) return -1;
    double m = 0;
    const float *c5 = (const float *)(cur + 5 * PRES_ROW), *p5 = (const float *)(prv + 5 * PRES_ROW);
    for (int r = 86; r <= 96; r++) {
        const float *cr = (const float *)(cur + (uintptr_t)r * PRES_ROW), *pr = (const float *)(prv + (uintptr_t)r * PRES_ROW);
        double s = 0;
        for (int k = 12; k < 15; k++) { double d = ((double)cr[k] - c5[k]) - ((double)pr[k] - p5[k]); s += d * d; }
        if (s > m) m = s;
    }
    return m > 0 ? __builtin_sqrt(m) : 0;
}
static int ev_take(void) {   /* one line of the budget (1) or a drop (0) */
    int cap = g_h_skip == LIPS_SITE_EVSEG ? g_ev_budget : g_ev_budget / EV_NSEG;
    if (g_ev_budget > 0 && cap < 1) cap = 1;
    if (g_ev_used >= g_ev_budget || (g_ev_seg >= EV_NSEG && g_h_skip != LIPS_SITE_EVSEG) || g_ev_seg_used >= cap) { g_ev_dropped++; return 0; }
    g_ev_used++; g_ev_seg_used++; g_ev_lines_b++;
    return 1;
}
static void ev_line(const char *ev, uint32_t f, int c, int slot, const char *pass, const EvFld *e, double mouth, double mmax) {
    if (!ev_take()) return;
    uint64_t tick = 0; int tstep = 0; rb_tick_info(&tick, &tstep);
    char sl[16], mo[64] = "";
    if (slot >= 0) snprintf(sl, sizeof sl, "+0x%03x", slot * 8); else snprintf(sl, sizeof sl, "-");
    if (mouth > -2) {
        if (mouth < 0) snprintf(mo, sizeof mo, " mouth=- mmax=-");
        else snprintf(mo, sizeof mo, " mouth=%.6f mmax=%.6f", mouth, mmax);
    }
    LOG("LIPSYNCEV ev=%s f=%u p=%d slot=%s pass=%s tick=%llu tstep=%d act=%u init=%u lpd=%llx ldp=%llx l3m=%llx l5m=%llx vst=%x seg=%d%s\n", ev, f, c, sl, pass,
        (unsigned long long)tick, tstep, e->act, e->init, (unsigned long long)e->lpd, (unsigned long long)e->ldp, (unsigned long long)e->l3m,
        (unsigned long long)e->l5m, e->vst, g_ev_seg, mo);
}
static void ev_event(const char *ev, uint32_t f, int c, int slot, const char *pass) {
    EvFld e; ev_read(c, &e);
    const EvFld *L = &g_ev_last[c];
    if (g_ev_last_kind[c] == ev[0] && g_ev_last_slot[c] == slot && e.act == L->act && e.init == L->init && e.vst == L->vst && e.lpd == L->lpd
        && e.ldp == L->ldp && e.l3m == L->l3m && e.l5m == L->l5m) { g_ev_same++; return; }
    g_ev_last[c] = e; g_ev_last_slot[c] = slot; g_ev_last_kind[c] = ev[0];
    ev_line(ev, f, c, slot, pass, &e, -2, 0);
    if (g_win_hi[c] < f + 1) { g_win_lo[c] = f; g_win_hi[c] = f + EV_WIN; g_win_logged[c] = 0; g_win_mmax[c] = 0; }   /* a window does not extend */
}
/* at the start of each NEW frame's pass: the segment (battle mode id 1 = the round call) and the frame lines of the open windows */
static void ev_frame(uint32_t k) {
    if (k <= g_ev_maxk) return;
    g_ev_maxk = k;
    int mode = (int)*(volatile uint32_t *)VA(0x144846364ULL);   /* the zone reader's battle mode id (rb.c pass_record) */
    if (mode == 1 && g_ev_mode_prev != 1 && g_ev_mode_prev != -1) { g_ev_seg++; g_ev_seg_used = 0; }
    else if (mode == 1 && g_ev_mode_prev == -1) { g_ev_seg = 1; g_ev_seg_used = 0; }   /* the battle begins in a round call: no intro segment */
    g_ev_mode_prev = mode;
    for (int c = 0; c < 2; c++) {
        if (!g_win_hi[c] || k <= g_win_lo[c] || k > g_win_hi[c]) continue;
        EvFld e; ev_read(c, &e);
        double m = ev_mouth(c);
        e.moving = m > 1e-4;
        if (m > g_win_mmax[c]) g_win_mmax[c] = m;
        const EvFld *L = &g_win_last[c];
        int chg = !g_win_logged[c] || k == g_win_hi[c] || e.act != L->act || e.init != L->init || e.vst != L->vst || e.lpd != L->lpd || e.ldp != L->ldp
                  || e.l3m != L->l3m || e.l5m != L->l5m || e.moving != L->moving;
        if (chg) { ev_line("frame", k, c, -1, rb_lips_pass_name(), &e, m, m < 0 ? -1 : g_win_mmax[c]); g_win_last[c] = e; g_win_logged[c] = 1; g_win_mmax[c] = 0; }
        if (k == g_win_hi[c]) g_win_hi[c] = 0;
    }
}

/* ------------------------------------------------------------------ §3.3 KEEP never rolls back */
static void copy_p(uint8_t *dst) { size_t o = 0; for (int i = 0; i < g_nb; i++) { memcpy(dst + o, (const void *)g_b[i].addr, g_b[i].len); o += g_b[i].len; } }
void lipsync_before_restore(int site) {
    if (!g_on) return;
    copy_p(g_rest);
    if (site == LIPS_SITE_ROLLBACK) { g_nfl = 0; g_fl_armed = 1; }   /* fix: the frees of this rollback's resims, for lipsync_pk_fix */
}
void lipsync_after_restore(int site) {
    if (!g_on) return;
    if (g_h_no_after) return;                       /* harness control LSDX: no write-back and no guard (the invariant removed) */
    if (g_h_skip == site) { g_armed = site; return; }   /* harness control LSGX: this site's write-back is skipped; the guard is armed */
    for (int i = 0; i < g_nseg; i++) memcpy((void *)g_seg[i].addr, g_rest + g_seg[i].roff, g_seg[i].len);   /* KEEP only: bank rows and core spans roll back */
    g_restores++;
    g_armed = site;
}

static int freed_idx(uint64_t v) { for (int i = 0; i < g_nfl; i++) if (v >= g_fl_lo[i] && v < g_fl_hi[i]) return i; return -1; }
/* v0.31.0 (OPTA-M2-SPEC.md §20.3): 1 while lipsync_note_free would keep a free's size (the size is read only then) */
int lipsync_free_wants(int resim) { return g_on && g_fl_armed && resim && g_nfl < LF_CAP; }
void lipsync_note_free(uintptr_t p, size_t sz, int resim) {
    if (!g_on) return;
    if (!g_fl_armed || !resim) return;   /* only the muted passes between a rollback restore and its pk_apply (an aborted rollback never fills it) */
    if (g_nfl >= LF_CAP) {
        g_free_over++;
        if (g_fo_lines < 5) { g_fo_lines++; LOG("LIPSYNC freelist_full f=%u cap=%d (the freed-pointer check of pres_keep is partial for this rollback)\n", core_frame(), LF_CAP); }
        return;
    }
    g_fl_lo[g_nfl] = p; g_fl_hi[g_nfl] = p + (sz ? sz : 16); g_nfl++;
}
/* fix: before pk_apply puts the live KEEP back (modes.c pk_apply; live = its saved copy, laid out like g_b): a differing 8-byte word whose
 * live value points into a block freed during this rollback's resims keeps the resim value (UE's newer pointer, which L4DANGLE kept). Logged
 * LIPSYNC pk_freed (a FINDING to investigate: UE freed a KEEP-referenced block in a muted forwarded call). Harness control h_lips_skip_site=pk:
 * the word is put back as before and counted as ptr_bad (case LSDMX: the check sees what the fix prevents). */
void lipsync_pk_fix(uint8_t *live, uint64_t s) {
    if (!g_on) return;
    int armed = g_fl_armed; g_fl_armed = 0;
    if (!armed || !g_nfl) return;
    size_t o = 0;
    for (int i = 0; i < g_nb; i++) {
        uint8_t *L = live + o; const uint8_t *M = (const uint8_t *)g_b[i].addr;
        o += g_b[i].len;
        for (uintptr_t a = (g_b[i].addr + 7) & ~(uintptr_t)7; a + 8 <= g_b[i].addr + g_b[i].len; a += 8) {
            uint64_t lv, mv; size_t w = a - g_b[i].addr;
            memcpy(&lv, L + w, 8); memcpy(&mv, M + w, 8);
            if (lv == mv) continue;
            int fi = freed_idx(lv);
            if (fi < 0) continue;
            if (g_h_skip == LIPS_SITE_PK) {
                g_ptr_bad++;
                if (g_ptr_lines < 20) { g_ptr_lines++; LOG("LIPSYNC ptr_bad f=%u p=%d field=+0x%x val=%llx why=pk_apply_freed (pres_keep puts back a pointer into a block freed during the resims)\n",
                                                         core_frame(), g_b[i].p, (unsigned)(g_b[i].lo + w), (unsigned long long)lv); }
                continue;
            }
            memcpy(L + w, &mv, 8);
            g_pk_freed++;
            if (g_pkf_lines < 20) { g_pkf_lines++; LOG("LIPSYNC pk_freed s=%llu p=%d blk=%s off=+0x%x live=%llx now=%llx freed=[%llx,%llx) action=adopt_resim (UE freed this block in a muted forwarded call; "
                                                       "pres_keep keeps the newer pointer)\n", (unsigned long long)s, g_b[i].p, g_b[i].name, (unsigned)(g_b[i].lo + w), (unsigned long long)lv,
                                                       (unsigned long long)mv, (unsigned long long)g_fl_lo[fi], (unsigned long long)g_fl_hi[fi]); }
        }
    }
}

static void watchdog(uint32_t k) {
    static const uint32_t lpd_off[2] = { 0x95ec0, 0x95f50 }, ptr_off[2] = { 0x95ed8, 0x2b270 };
    for (int c = 0; c < 2; c++) {
        uintptr_t base = VA(A_PLAYER0) + (uintptr_t)c * PLAYER_STRIDE;
        for (int j = 0; j < 2; j++) {
            uint64_t v = *(volatile uint64_t *)(base + lpd_off[j]);
            if (g_lpd_seen[c][j] && v != g_lpd[c][j] && g_lpd[c][j]) {
                g_lpd_changed++;
                if (g_lpd_lines < 20) { g_lpd_lines++; LOG("LIPSYNC lpd_changed f=%u p=%d field=+0x%x old=%llx new=%llx\n", k, c, lpd_off[j], (unsigned long long)g_lpd[c][j], (unsigned long long)v); }
            }
            g_lpd[c][j] = v; g_lpd_seen[c][j] = 1;
            uint64_t q = *(volatile uint64_t *)(base + ptr_off[j]);
            if (q != g_pchk[c][j]) {
                g_pchk[c][j] = q;
                if (q && !IN_IMAGE(q) && !mem_readable((const void *)(uintptr_t)q, 8)) {
                    g_ptr_bad++;
                    if (g_ptr_lines < 20) { g_ptr_lines++; LOG("LIPSYNC ptr_bad f=%u p=%d field=+0x%x val=%llx (not 0, not an image address, not committed)\n", k, c, ptr_off[j], (unsigned long long)q); }
                }
            }
        }
    }
}
/* v0.36.0 (DESIGN.md §3.4): the guard check of the last hooked restore, factored out of lipsync_pass_start (the body is unchanged): KEEP is compared with
 * the copy taken before that restore; the first difference is LIPSYNCGUARD FAIL (g_fail / g_trip); the guard is disarmed either way */
static void guard_check(uint32_t k, uint32_t s) {
    if (g_armed < 0) return;
    int site = g_armed; g_armed = -1;
    g_guard_checks++;
    for (int i = 0; i < g_nseg; i++) {   /* the written-back bytes (KEEP; the core-written spans of the old harness tables roll back by design) */
        const uint8_t *m = (const uint8_t *)g_seg[i].addr, *r = g_rest + g_seg[i].roff;
        if (!memcmp(m, r, g_seg[i].len)) continue;
        uint32_t j = 0; while (j < g_seg[i].len && m[j] == r[j]) j++;
        const LBlk *b = &g_b[g_seg[i].blk];
        if (!g_fail) {
            g_fail = 1; g_trip = 1; g_title_dirty = 1;
            LOG("LIPSYNCGUARD FAIL site=%s s=%u f=%u p=%d blk=%s off=+0x%x (KEEP differs from the copy taken before this restore: a restore site wrote old KEEP; the rollback stops)\n",
                k_site[site], s, k, b->p, b->name, (unsigned)(b->lo + (g_seg[i].addr - b->addr) + j));
            log_flush();
        }
        break;
    }
}
void lipsync_pass_start(uint32_t k, uint32_t s) {
    if (!g_on) return;
    guard_check(k, s);
    watchdog(k);
    ev_frame(k);   /* v0.25 LIPSYNCEV: segments and frame windows (new frames only) */
}
/* v0.36.0 (DESIGN.md §3.4, rule C): rb.c pres_flush, before late calls reach UE: the check of the last hooked restore runs now, so a late call that writes
 * KEEP (+0x038) is never mistaken for a restore that wrote old KEEP. Nothing writes KEEP between this point and the next pass start but those late calls
 * (rb.c confirm_upto -> pres_flush, then the live pass), so the check covers exactly what the pass-start check covered. Harness control
 * h_lips_skip_site=flush (LSRVX): v0.35.0's order (the check stays at the next pass start) */
void lipsync_guard_before_late(uint32_t s) {
    if (!g_on) return;
    if (g_h_skip == LIPS_SITE_FLUSH) return;
    if (g_armed >= 0) g_guard_early++;
    guard_check(s, s);
}
int lipsync_guard_tripped(void) {
    if (!g_on) return 0;
    if (!g_trip) return 0;
    g_trip = 0;
    return 1;
}

/* ------------------------------------------------------------------ PRESKEEPD with lip-sync on (fix; v0.25 C1B-R3-SPEC.md §3.2) */
/* With lipsync=1 the resims advance KEEP from the live KEEP, so pk_apply finds the KEEP blocks different after nearly every rollback BY DESIGN.
 * v0.25: KEEP holds only step-computed expression / voice state (face_w, face_layers, lipsync_ctl, voice_ctl), so every KEEP granule is by
 * design: counted (pkd_bydesign=), not logged; the round-2 FINDING face_b +0x2b180..+0x2b1bf (face_layers) disappears. Granules of any other
 * block (only the harness's old tables have one: bank rows, face_a, face_b) are logged with a budget of 60 lines PER BLOCK. Returns 1 when it
 * handled the call (lip-sync on). Harness controls: h_lips_skip_site=pkd returns 0 (the PART A path with its shared budget);
 * h_lips_skip_site=pkd_v024 judges with the v0.24 by-design list (the known lip-sync bytes only: +0x295d0..+0x295d7, +0x2af18..+0x2b17f,
 * +0x2b270..+0x2b277, lipsync_ctl, voice_ctl), so the layer granules 2b180:16 are logged again (case LSPX). */
static int pkd_by_design(int blk, uint32_t off) {   /* off = PLAYER-relative granule start */
    const char *n = g_b[blk].name;
    if (!strcmp(n, "lipsync_ctl") || !strcmp(n, "voice_ctl")) return 1;
    if (g_h_skip == LIPS_SITE_PKDV024) return (off >= 0x295d0 && off < 0x295d8) || (off >= 0x2af18 && off < 0x2b180) || (off >= 0x2b270 && off < 0x2b278);
    return !strcmp(n, "face_w") || !strcmp(n, "face_layers");
}
int lipsync_pkd_note(uint64_t s, const uint8_t *live, const uint8_t *diff, int n) {
    if (!g_on) return 0;
    if (g_h_skip == LIPS_SITE_PKD) return 0;
    size_t o = 0;
    for (int i = 0; i < n && i < g_nb; i++) {
        const uint8_t *L = live + o, *M = (const uint8_t *)g_b[i].addr;
        uint32_t len = g_b[i].len, lo = g_b[i].lo, hi = lo + len;
        o += len;
        if (!diff[i]) continue;
        char gs[400]; size_t go = 0; int k = 0;
        for (uint32_t g = lo & ~15u; g < hi; g += 16) {
            uint32_t a = g < lo ? lo : g, b = g + 16 > hi ? hi : g + 16;
            if (!memcmp(L + (a - lo), M + (a - lo), b - a)) continue;
            if (pkd_by_design(i, a)) { g_pkd_bydesign++; continue; }
            k++;
            if (k <= 8) { int w = snprintf(gs + go, sizeof gs - go, "%s%x:%u", go ? "," : "", a, b - a); if (w > 0 && (size_t)w < sizeof gs - go) go += (size_t)w; }
        }
        if (!k || g_pkd_lines[i] >= 60) continue;
        g_pkd_lines[i]++;
        LOG("PRESKEEPD s=%llu p=%d blk=%s n=%d g=%s\n", (unsigned long long)s, g_b[i].p, g_b[i].name, k, go ? gs : "-");
    }
    return 1;
}

/* ------------------------------------------------------------------ §3.2 the KEEP allowlist inside l4=1 */
static int in_p(uintptr_t a) { for (int i = 0; i < g_nb; i++) if (a >= g_b[i].addr && a < g_b[i].addr + g_b[i].len) return 1; return 0; }
/* the first byte UE changed in [a0, a1) that lies outside every KEEP range: its address and the length of the changed run (0 = none) */
static uintptr_t nonp_scan(uintptr_t a0, uintptr_t a1, uint32_t *len) {
    if (a0 < g_kb_lo) a0 = g_kb_lo;
    for (uintptr_t a = a0; a < a1; a++) {
        const uint8_t *pre = g_kb_pre + (a - g_kb_lo);
        if (*(const uint8_t *)a == *pre || in_p(a)) continue;
        uintptr_t e = a; while (e < a1 && *(const uint8_t *)e != *(g_kb_pre + (e - g_kb_lo)) && !in_p(e)) e++;
        *len = (uint32_t)(e - a);
        return a;
    }
    *len = 0;
    return 0;
}
void lipsync_l4_keep_begin(int slot, int resim, const uint8_t *pre, uintptr_t bss_lo, size_t bss_len) {
    if (!g_on) return;
    g_kb_valid = 0;
    if (!pre) return;
    int any = 0; size_t o = 0;
    for (int i = 0; i < g_nb; i++) {
        const uint8_t *m = (const uint8_t *)g_b[i].addr, *p = pre + (g_b[i].addr - bss_lo);
        g_chg[i] = (uint8_t)(g_b[i].addr >= bss_lo && g_b[i].addr + g_b[i].len <= bss_lo + bss_len && memcmp(m, p, g_b[i].len) != 0);
        g_drop[i] = 0;
        if (g_chg[i]) { memcpy(g_keep + o, m, g_b[i].len); any = 1; }
        o += g_b[i].len;
    }
    if (!any) return;
    if (resim) { g_resim_rev++; return; }   /* muted pass (confirmed resim / RPB): UE's KEEP writes stay reverted */
    g_kb_pre = pre; g_kb_lo = bss_lo; g_kb_slot = slot;
    uintptr_t hi = bss_lo + bss_len;
    for (int i = 0; i < g_nb; i++) {   /* NONP: all or nothing per range */
        if (!g_chg[i]) continue;
        uintptr_t lo = g_b[i].addr, e = g_b[i].addr + g_b[i].len;
        uint32_t len = 0;
        uintptr_t at = nonp_scan(lo >= bss_lo + 64 ? lo - 64 : bss_lo, lo, &len);
        if (!at) at = nonp_scan(e, e + 64 <= hi ? e + 64 : hi, &len);
        if (!at) continue;
        g_drop[i] = 1; g_nonp++;
        if (g_nonp_lines < 40) {
            g_nonp_lines++;
            uintptr_t pb = VA(A_PLAYER0) + (uintptr_t)g_b[i].p * PLAYER_STRIDE;
            LOG("LIPSYNC NONP f=%u p=%d slot=+0x%03x at=+0x%llx len=%u action=revert_range blk=%s (the same call wrote outside KEEP next to this range: the range is reverted whole)\n",
                core_frame(), g_b[i].p, slot * 8, (unsigned long long)(at - pb), len, g_b[i].name);
        }
    }
    g_kb_valid = 1;
}
void lipsync_l4_keep_end(void) {
    if (!g_on) return;
    if (!g_kb_valid) return;
    g_kb_valid = 0;
    size_t o = 0; uint64_t kept = 0; int pk[2] = { 0, 0 };
    for (int i = 0; i < g_nb; i++) {
        uint32_t n = g_b[i].len;
        if (g_chg[i] && !g_drop[i]) {
            const uint8_t *post = g_keep + o, *pre = g_kb_pre + (g_b[i].addr - g_kb_lo);
            uint8_t *m = (uint8_t *)g_b[i].addr;
            for (uint32_t j = 0; j < n; j++) if (post[j] != pre[j]) { m[j] = post[j]; kept++; pk[g_b[i].p & 1] = 1; }
        }
        o += n;
    }
    if (kept) { g_kept_calls++; g_kept_bytes += kept; }
    if (kept && g_armed >= 0 && rb_is_on()) {   /* v0.36.0 (rule C) tripwire: a kept call while a restore's guard is pending (its check would read the kept bytes as old KEEP) */
        g_keep_armed++;
        if (!g_keep_armed_lines++) LOG("LIPSYNC keep_while_armed slot=+0x%03x site=%s (a kept call while a restore's guard is pending)\n", g_kb_slot * 8, k_site[g_armed]);
    }
    for (int c = 0; c < 2; c++) if (pk[c]) ev_event("keep", core_frame(), c, g_kb_slot, rb_lips_pass_name());   /* v0.25 LIPSYNCEV: one line per kept call and player */
}

/* ------------------------------------------------------------------ foreign patches: KEEP spans are skipped */
int lipsync_fp_skip(void) {
    if (!g_on) return 0;
    return g_h_skip != LIPS_SITE_FP;   /* harness control LSFX: the skip is off */
}
int lipsync_fp_run(uintptr_t a, size_t len, size_t *run) {
    if (!len) { *run = 1; return 0; }
    if (!IN_IMAGE(a)) { *run = len; return 0; }
    uint64_t pv = a - g_base + IMG_PREF, end;
    int k = pres_va_kind(pv, pv + len, &end);
    size_t r = end > pv ? (size_t)(end - pv) : len;
    *run = r > len ? len : r;
    if (k == PRES_K_P) {   /* v0.25 LIPSYNCEV ev=fp: a between-step UE write touched KEEP (one line per frame and player) */
        int c = (int)((pv - A_PLAYER0) / PLAYER_STRIDE) & 1;
        uint32_t f = core_frame();
        if (g_fp_last[c] != f + 1) { g_fp_last[c] = f + 1; ev_event("fp", f, c, -1, "between"); }
    }
    return k == PRES_K_P;
}

/* ------------------------------------------------------------------ the window title */
void lipsync_title(char *out, size_t cap, int fight) {
    if (!g_cfg.lipsync) return;
    if (!fight) return;
    size_t o = strlen(out);
    if (o + 1 >= cap) return;
    snprintf(out + o, cap - o, " lips=%s", !g_on ? "REFUSED" : g_fail ? "FAIL" : "on");
}

/* ------------------------------------------------------------------ v0.29.4 FACES: rbonline (DESIGN.md §3.1 item 5, §3.4) */
/* rbo_title_token's tail, on M1's writer thread (two int reads that race the game thread benignly): " lips=on" (" lips=FAIL" after a
 * LIPSYNCGUARD FAIL) while lip-sync runs, i.e. from ARM to END of an armed battle; nothing while passive, so every passive title keeps its
 * v0.29.3 bytes */
int lipsync_rbo_fail(void) { return g_rbo && *(volatile int *)&g_on && *(volatile int *)&g_fail; }   /* v0.31.0: the badge's lips=FAIL */
void lipsync_rbo_title(char *out, size_t cap) {
    if (!g_rbo || !*(volatile int *)&g_on) return;
    size_t o = strlen(out);
    if (o + 1 >= cap) return;
    snprintf(out + o, cap - o, " lips=%s", *(volatile int *)&g_fail ? "FAIL" : "on");
}
/* harness control XLIPS (h_lips_rbo_force_off=1): rbo_core_force forces lipsync=0 as v0.29.3 did; the plugin build never reads the key */
int lipsync_h_force_off(void) {
#ifdef SC6RB_HARNESS
    return cfg_geti("h_lips_rbo_force_off", 0) != 0;
#else
    return 0;
#endif
}
