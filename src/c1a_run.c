/* v0.22.0-C1a runtime: mode=record / mode=replay (GATE-C-DESIGN §8 C1a, §5.1; audit/c1a/RECON.md).
 *
 * Every step of the battle is a straight live pass (modes.c do_c1a: L3, L4 as configured, routing to the arena, the camera block
 * pinned as in roll7, no resim, no commit). This file:
 *   - record: writes log_dir\c1a_<ts>_b<battle>.rec: one header line at the battle's first step, then one F line per step
 *     (the input the step got, the tier-1 checksum and its 25 components, the game replay flag, the UE->core calls since the
 *     previous step), a T2 line every 32 steps (tier-2: class-G bytes of the image snapshot regions in 64 KB granules,
 *     pointer-normalized), a D line per dump (c1a_<ts>_b<battle>_f<f>.dump every c1a_dump_every steps), U lines when the
 *     UE-fed camera-input hash changes, and an END line at Shutdown.
 *   - replay (replay_file=<.rec>): loads the recording at startup; every Init runs with the recorded seed (c1a_seed_for_init)
 *     and the LCG gets the recorded value; at the first step the setup hash is compared category by category: a difference in
 *     a refusing category (c1a_refuse) = SETUP_MISMATCH, no injection; otherwise each step gets the recorded input of the same
 *     battle step until the recording ends (C1A END). Tier-1 / tier-2 / UE->core calls / replay flag are compared, and the run
 *     writes its own .play file in the same format.
 * Formats: see c1a.h and the build report. Budgets: 200 lines per kind (MISMATCH tier 1, tier 2, IFCDIFF, RFLAG, UECAM). */
#include "c1a_run.h"
#include "modes.h"
#include "mem.h"
#include "fwd.h"
#include "pres.h"
#include "rb.h"
#include "netsess.h"   /* C2 */
#include "vm.h"        /* v0.32.0 review fix B1: the move-script VM diagnostics (src/vm.c) */
#include "argcfix.h"   /* v0.34.0 (OPTA-M2-SPEC.md §23.1): ARGC b= (rbreplay) */
#include <stdlib.h>
#include <errno.h>

_Static_assert(sizeof(C1aPin) == sizeof(SPin), "C1aPin has the SPin layout");
_Static_assert(offsetof(C1aPin, len) == offsetof(SPin, len), "C1aPin has the SPin layout");

#define PL0     0x1447156f0ULL
#define PSTR    0x973f0ULL
#define AIBLK   0x14470f390ULL
#define AISTR   0x3038ULL
#define CTLBLK  0x144715400ULL
#define BUDGET  200
extern int g_arena_fixed;   /* mem.c */

/* ------------------------------------------------------------------ buffered writer */
typedef struct { HANDLE h; char *buf; size_t len, cap; uint64_t bytes; } Wr;
static void wr_flush(Wr *w) {
    if (w->h != INVALID_HANDLE_VALUE && w->h && w->len) { DWORD x; WriteFile(w->h, w->buf, (DWORD)w->len, &x, NULL); w->bytes += w->len; }
    w->len = 0;
}
static int wr_open(Wr *w, const char *path) {
    if (!w->buf) { w->cap = 1u << 20; w->buf = (char *)VirtualAlloc(NULL, w->cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); if (!w->buf) { w->cap = 0; return 0; } }
    w->len = 0; w->bytes = 0;
    w->h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return w->h != INVALID_HANDLE_VALUE;
}
static int wr_is_open(const Wr *w) { return w->h && w->h != INVALID_HANDLE_VALUE; }
static void wr_put(Wr *w, const char *s, size_t n) {
    if (!wr_is_open(w) || !w->buf) return;
    if (w->len + n > w->cap) wr_flush(w);
    if (n > w->cap) { DWORD x; WriteFile(w->h, s, (DWORD)n, &x, NULL); w->bytes += n; return; }
    memcpy(w->buf + w->len, s, n); w->len += n;
}
static void wr_printf(Wr *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void wr_printf(Wr *w, const char *fmt, ...) {
    static char t[16384];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (n > 0) wr_put(w, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}
static void wr_close(Wr *w) { if (!wr_is_open(w)) return; wr_flush(w); CloseHandle(w->h); w->h = INVALID_HANDLE_VALUE; }

/* ------------------------------------------------------------------ state */
static int g_cm;                       /* 0 off, 1 record, 2 replay */
static char g_ts[32];
enum { RF_NONE = 0, RF_OK, RF_NOFILE, RF_BAD };
static int g_rf;
static char g_rf_why[96];
static C1aFrame *g_rf_fr; static uint32_t g_rf_n, g_rf_cap;
typedef struct { uint32_t f; int n; C1aCell c[C1A_MAXCELL]; } T2Rec;
static T2Rec *g_rf_t2; static uint32_t g_rf_nt2, g_rf_capt2;
static uint32_t g_rf_seed, g_rf_lcg; static uint64_t g_rf_s[C1A_NSETUP]; static uint64_t g_rf_bat;
/* v0.22.1: the recording's build, s_wind present (v0.22.1 recordings), its battle-start dump (setup injection, C1-C3) */
static char g_rf_plugin[64]; static const char *g_rf_compat; static int g_rf_has_wind;
static uint8_t *g_rf_dbuf; static size_t g_rf_dn; static C1aDump g_rf_dump; static int g_rf_dump_ok; static char g_rf_dump_why[96], g_rf_dump_path[MAX_PATH];
static C1aView g_rf_view[C1A_NRANGE], g_rf_wview; static int g_rf_has_wview;

enum { PL_NONE = 0, PL_ARMED, PL_OK, PL_MISMATCH, PL_END, PL_NODUMP };
static const char *const k_pl[] = { "-", "armed", "ok", "SETUP_MISMATCH", "end", "NO_SETUP_DUMP" };
static int g_play;
/* v0.22.1: this battle's setup injection (SETUP_INJECT), the hashes before it, the wind state, the CRT counters (C5), +0xc8 (C3) */
static int g_injected; static uint32_t g_inj_cat_bytes[C1A_NSETUP]; static uint64_t g_pre_s[C1A_NSETUP]; static int32_t g_pers_rec[2], g_pers_ue[2];
/* v0.22.1: the CpuPersonality words (AI +0x2c0c, rule block +0x10) after the battle's first setup check, watched every step: a UE re-set
 * mid-fight (PlayerInfoHandler +0x1f8 / SetMatchRule +0x48 at a round start: not checked statically, C1A-AUDIT §6) is named, not hidden */
static int32_t g_pers_watch[2][2]; static uint32_t g_pers_changes, g_pers_lines;
static void pers_read_live(int32_t out[2][2]) {
    for (int s = 0; s < 2; s++) {
        out[s][0] = *(volatile int32_t *)VA(C1A_INJ_AI + (uint64_t)s * C1A_INJ_AISTR + C1A_INJ_PAI);
        out[s][1] = *(volatile int32_t *)VA(C1A_INJ_RULES + (uint64_t)s * C1A_INJ_RULESTR + C1A_INJ_PERS);
    }
}
static int g_rf_conv;                  /* C2: the recording is an rbnet battle (header conv=1) */
static C1aWind g_wind_now; static int g_wind_list; static uint64_t g_wind_nodes;
static volatile DWORD g_gt_tid; static volatile LONG g_crt_gt[2], g_crt_other;   /* game-thread CRT rand / srand not on the LCG; other threads */
static LONG g_crt_bb[2], g_crt_bb_other, g_crt_demo[2], g_crt_demo_other; static int g_demo_seen, g_seed_demo_skip;
static uint32_t g_c8_n, g_c8_moved, g_c8_pre, g_c8_pre_moved, g_c8_lines, g_c8_idx0, g_c8_w0; static uint64_t g_c8_tt0; static int g_c8_in, g_in_bat;
static uint64_t g_bat;
static uint32_t g_f, g_rec_frames;
static uint32_t g_seed_orig, g_seed_used, g_lcg0; static int g_seed_ovr;
static uint64_t g_ifc_base[IFC_NSLOTS]; static uint32_t g_setup_calls;
static C1aFrame g_cur;
static uint64_t g_s[C1A_NSETUP];
static uint64_t g_uecam; static int g_uecam_lines;
static int g_t1_lines, g_t2_lines, g_ifc_lines, g_rfl_lines;
static uint32_t g_t1mm, g_t1first, g_t2mm, g_t2first, g_ifcd, g_rfd, g_t2_cursor, g_dumps, g_t2n, g_t2_compared;
static uint8_t g_prev_rec_rp, g_prev_now_rp;
static int g_end_logged;
static Wr g_w = { INVALID_HANDLE_VALUE, NULL, 0, 0, 0 };
static char g_wname[MAX_PATH];
static int g_win_w, g_win_h;
static uint32_t g_ctl_type[2], g_ai_loaded[2], g_cpu_level[2]; static int g_stage_slot, g_stage_entries;
static double g_us_t1, g_us_t2, g_us_dump_max; static uint64_t g_n_t1, g_n_t2;
static int g_rf_t2tag;                 /* v0.23.0-C1b: the recording's t2rule (header key t2rule=heaptag; absent = raw) */
/* v0.24.0-C1b2 F1 (C1B-R2-SPEC.md §2.2): the recording's tier-1 pointer rule (0 = no t1rule key: a v0.22/v0.23 heaptag recording, 1 =
 * ptrmap1, 2 = heaptag), its t1map; this battle's T1NORM frames, T1MAP_BAD units, first-mismatch dumps; the demo battle (rblive) */
static int g_rf_t1rule; static uint64_t g_rf_t1map; static int g_rf_t1map_set;
static uint32_t g_t1norm, g_t1norm_lines, g_t1map_bad, g_mm_written, g_mm_noslot;
static uint32_t g_t1norm_rescue, g_t1norm_old;   /* v0.24.0-C1b2 fix: T1NORM split by branch (why=rescue / why=oldrule; t1norm = their sum) */
static int g_skip_bat;                 /* rblive: the demo battle (UE seed C1A_DEMO_SEED) writes no recording */
typedef struct { uint32_t f; int eq; } MmEq;
static MmEq g_mm_eq[64];               /* the tier-1 verdict of the last pass of each recent frame (F1.7 "the previous frame was equal") */
static int is_mt_tail_reg(const SnapSet *ss, int r) { return !strcmp(ss->reg[r].name, "mt_tail"); }
/* C1B3: (C1B-R3-SPEC.md §2.1) the content gate: this battle's trees (size, key hash, walk), the recording's reference, the verdict; the modset
 * listing (§2.1.3); the tier-2 rule of the recording (§2.6.2) */
typedef struct { uint64_t n, keys; int ok, has_keys; uint32_t visited; int head_null; } CTree;
static CTree g_ct[2]; static int g_ct_walkbad;
static CTree g_rf_ct[2]; static int g_rf_ct_src;           /* 0 none, 1 header (tree0=/tree1=), 2 f0dump (sizes only) */
static uint64_t g_rf_dsize[2]; static int g_rf_dsize_ok;   /* the recording's f0-dump sizes (legacy recordings) */
static int g_play_why;                                     /* PL_MISMATCH: 0 setup hash, 1 content, 2 t2rule */
static uint64_t g_ms_hash; static uint32_t g_ms_n; static int g_ms_done;
static uint64_t g_rf_ms; static int g_rf_ms_has, g_ms_diff;
static int g_rf_t2rule;                                    /* the recording's t2rule: 0 raw (absent), 1 heaptag, 2 map1, -1 unknown */
static const char *t2rule_name(int r) { return r == 2 ? (g_c1a_t2map_ver == 1 ? "map1" : "map2") : r == 1 ? "heaptag" : r == 0 ? "raw" : "unknown"; }   /* C2: + map2 */
static void modset_startup(void);   /* v0.23.0-C1b rb_mt_tail: not a dump section, not in catch */

int c1a_active(void) { return g_cm; }

/* ------------------------------------------------------------------ normalization context */
static C1aRegion g_nregs[SNAP_MAXREG];
/* committed-address test with a per-batch page cache (VirtualQuery only: never dereferences the value) */
static uint64_t g_rc_key[512]; static uint32_t g_rc_gen[512], g_rc_cur = 1; static uint8_t g_rc_val[512];
static int c1a_readable(uintptr_t v) {
    uint64_t k = v >> 12; unsigned i = (unsigned)((k * 0x9E3779B97F4A7C15ULL) >> 55);
    if (g_rc_gen[i] == g_rc_cur && g_rc_key[i] == k) return g_rc_val[i];
    int r = mem_readable((const void *)v, 1);
    g_rc_gen[i] = g_rc_cur; g_rc_key[i] = k; g_rc_val[i] = (uint8_t)r;
    return r;
}
static void norm_ctx(C1aNorm *n, int heap_mode, int want_heap_kind) {
    memset(n, 0, sizeof *n);
    if (g_arena_ok && g_arena.base) { n->arena_lo = (uintptr_t)g_arena.base; n->arena_hi = (uintptr_t)g_arena.base + g_arena.cap; }
    const SnapSet *ss = modes_snapset();
    if (modes_ss_ready() && ss->npin) { n->pins = (const C1aPin *)ss->pins; n->npin = ss->npin; }
    if (modes_ss_ready() && ss->stable) { n->pins = (const C1aPin *)ss->spins; n->npin = ss->nspin; }   /* v0.23.0-C1b: the visible pins, sorted */
    n->img_lo = g_base; n->img_hi = g_base + g_image_size;
    int k = 0;
    if (modes_ss_ready())
        for (int i = 0; i < ss->nreg && k < SNAP_MAXREG; i++)
            if (!IN_IMAGE(ss->reg[i].addr)) { g_nregs[k].lo = ss->reg[i].addr; g_nregs[k].len = ss->reg[i].len; g_nregs[k].idx = i; k++; }
    n->regs = g_nregs; n->nreg = k;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    MEMORY_BASIC_INFORMATION mbi; volatile int probe = 0;
    n->stk_lo = VirtualQuery((const void *)&probe, &mbi, sizeof mbi) ? (uintptr_t)mbi.AllocationBase : (uintptr_t)tib->StackLimit;
    n->stk_hi = (uintptr_t)tib->StackBase;
    n->heap_mode = heap_mode;
    n->readable = (heap_mode || want_heap_kind) ? c1a_readable : NULL;
    g_rc_cur++;
}

/* hashes of preferred-base ranges: raw, or normalized (a copy; the pointer blocks of tier 1 and the setup items) */
static uint64_t rh(uint64_t pref, size_t len, uint64_t h) { return hash_bytes((const void *)VA(pref), len, h); }
static uint8_t g_nb[0x8000];
static uint64_t nh(const C1aNorm *n, uint64_t pref, size_t len, uint64_t h) {
    if (len > sizeof g_nb) return rh(pref, len, h);
    memcpy(g_nb, (const void *)VA(pref), len);
    c1a_norm_buf(n, g_nb, VA(pref), len, NULL);
    return hash_bytes(g_nb, len, h);
}
static uint64_t ppref(int p, uint64_t off) { return PL0 + (uint64_t)p * PSTR + off; }

/* ------------------------------------------------------------------ the image snapshot regions, normalized */
static uint8_t *g_scr, *g_kind, *g_cls; static size_t g_scr_cap;
static int scr_need(size_t len) {
    if (len <= g_scr_cap) return 1;
    if (g_scr) { VirtualFree(g_scr, 0, MEM_RELEASE); VirtualFree(g_kind, 0, MEM_RELEASE); VirtualFree(g_cls, 0, MEM_RELEASE); }
    size_t cap = (len + 0xffff) & ~(size_t)0xffff;
    g_scr = (uint8_t *)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_kind = (uint8_t *)VirtualAlloc(NULL, cap / 8 + 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_cls = (uint8_t *)VirtualAlloc(NULL, cap / 4 + 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_scr || !g_kind || !g_cls) { g_scr_cap = 0; return 0; }
    g_scr_cap = cap;
    return 1;
}
static int is_x_va(uintptr_t va) {   /* a per-PC pointer X range of PLAYER 0/1 (pres.c) */
    uint64_t p = va - g_base + IMG_PREF;
    if (p < PL0 || p >= PL0 + 2 * PSTR) return 0;
    return pres_is_x((p - PL0) % PSTR);
}
/* copy region r into g_scr and normalize it; zero_masks: masked bytes (X, listener pointer, ...) become 0 (hashes).
 * C1B3: t2map = 1: the address-independent map rule (c1a_norm_buf_t2map, never readable()) instead of c1a_norm_buf */
static int region_norm2(int r, const C1aNorm *n, int zero_masks, int want_kinds, int t2map) {
    const SnapSet *ss = modes_snapset();
    uintptr_t a = ss->reg[r].addr; size_t len = ss->reg[r].len;
    if (!scr_need(len) || !mem_readable((const void *)a, len)) return 0;
    memcpy(g_scr, (const void *)a, len);
    if (t2map) c1a_norm_buf_t2map(n, g_scr, a, a - g_base + IMG_PREF, len, want_kinds ? g_kind : NULL);
    else c1a_norm_buf(n, g_scr, a, len, want_kinds ? g_kind : NULL);
    if (zero_masks)
        for (int m = 0; m < ss->nmask; m++) {
            uintptr_t m0 = ss->mask[m].addr, m1 = m0 + ss->mask[m].len;
            if (m1 <= a || m0 >= a + len) continue;
            uintptr_t z0 = m0 > a ? m0 : a, z1 = m1 < a + len ? m1 : a + len;
            memset(g_scr + (z0 - a), 0, z1 - z0);
        }
    return 1;
}
static int region_norm(int r, const C1aNorm *n, int zero_masks, int want_kinds) { return region_norm2(r, n, zero_masks, want_kinds, 0); }

/* tier 2: class-G bytes of image region r into cells (granule = preferred VA >> 16), appended in VA order */
typedef struct { int region; uint64_t pref; C1aCell *cells; int n; char want; uint64_t h; } SegCtx;
static void t2_cb(void *u, int region, size_t off, size_t len, char cls) {
    SegCtx *t = (SegCtx *)u;
    if (region != t->region || cls != 'G') return;
    size_t i = 0;
    while (i < len) {
        uint64_t va = t->pref + off + i; uint32_t gran = (uint32_t)(va >> 16) & 0xffffffu;
        size_t piece = (size_t)((((uint64_t)(va >> 16) + 1) << 16) - va); if (piece > len - i) piece = len - i;
        uint32_t key = (uint32_t)'G' << 24 | gran;
        if (!t->n || t->cells[t->n - 1].key != key) {
            if (t->n >= C1A_MAXCELL) return;
            t->cells[t->n].key = key; t->cells[t->n].h = key; t->n++;
        }
        t->cells[t->n - 1].h = hash_bytes(g_scr + off + i, piece, t->cells[t->n - 1].h);
        i += piece;
    }
}
static int t2_compute(C1aCell *cells) {
    /* the spec: arena / pin / image / region / stack, else raw; v0.23.0-C1b c1a_t2_heap=tag (default): a raw value that is a committed address
     * becomes C1A_TAG_HEAP, as in tier 1 and the setup hash (C1a: every tier-2 difference across launches was a raw heap pointer) */
    /* C1B3: (C1B-R3-SPEC.md §2.6.2) the resolved rule: 0 raw, 1 tag (readable(): address-dependent), 2 map (the census map, no readable()) */
    int rule = g_cfg.c1a_t2_heap_tag;
    C1aNorm n; norm_ctx(&n, rule == 1 ? 1 : 0, 0);
    if (rule == 2) n.readable = NULL;
    const SnapSet *ss = modes_snapset();
    int total = 0;
    if (!modes_ss_ready()) return 0;
    for (int r = 0; r < ss->nreg; r++) {
        if (!IN_IMAGE(ss->reg[r].addr) || !region_norm2(r, &n, 1, 0, rule == 2)) continue;
        SegCtx t = { r, ss->reg[r].addr - g_base + IMG_PREF, cells + total, 0, 'G', 0 };
        modes_seg_iter(t2_cb, &t);
        total += t.n;
        if (total >= C1A_MAXCELL) break;
    }
    for (int i = 1; i < total; i++) { C1aCell x = cells[i]; int j = i - 1; while (j >= 0 && cells[j].key > x.key) { cells[j + 1] = cells[j]; j--; } cells[j + 1] = x; }   /* by key */
    return total;
}
/* the setup catch-all: class G and O bytes of the image regions, normalized with heap pointers dropped */
static void catch_cb(void *u, int region, size_t off, size_t len, char cls) {
    SegCtx *t = (SegCtx *)u;
    if (region != t->region || (cls != 'G' && cls != 'O')) return;
    t->h = hash_bytes(g_scr + off, len, t->h);
}

/* ------------------------------------------------------------------ tier 1 */
/* v0.24.0-C1b2 F1: the ai + ctl blocks (contiguous 0x14470f390..0x1447154bf) under both pointer rules. The primary rule (c1a_t1_rule,
 * default ptrmap) gives the F line's ctl / ai; the other rule's values and the units the rules normalize differently go to alt. The heaptag
 * rule is the v0.23 nh() (c1a_norm_buf, heap_mode 1, readable()); the map rule is c1a_norm_buf_map over k_t1_ai_ptr / k_t1_ctl_ptr. */
#define T1B_LEN ((size_t)C1A_T1_AI_LEN + C1A_T1_CTL_LEN)
static uint8_t g_t1b_m[T1B_LEN], g_t1b_o[T1B_LEN], g_t1k_m[T1B_LEN / 8], g_t1k_o[T1B_LEN / 8];
static void t1_ptr_blocks(const C1aNorm *n, uint64_t *c_ctl, uint64_t *c_ai, C1aT1Alt *alt) {
    memcpy(g_t1b_o, (const void *)VA(AIBLK), T1B_LEN);
    memcpy(g_t1b_m, g_t1b_o, T1B_LEN);
    c1a_norm_buf(n, g_t1b_o, VA(AIBLK), T1B_LEN, g_t1k_o);
    c1a_norm_buf_map(n, g_t1b_m, VA(AIBLK), C1A_T1_AI_LEN, k_t1_ai_ptr, C1A_T1_NAI, g_t1k_m);
    c1a_norm_buf_map(n, g_t1b_m + C1A_T1_AI_LEN, VA(CTLBLK), C1A_T1_CTL_LEN, k_t1_ctl_ptr, C1A_T1_NCTL, g_t1k_m + C1A_T1_AI_LEN / 8);
    uint64_t ai_o = hash_bytes(g_t1b_o, C1A_T1_AI_LEN, 9), ctl_o = hash_bytes(g_t1b_o + C1A_T1_AI_LEN, C1A_T1_CTL_LEN, 8);
    uint64_t ai_m = hash_bytes(g_t1b_m, C1A_T1_AI_LEN, 9), ctl_m = hash_bytes(g_t1b_m + C1A_T1_AI_LEN, C1A_T1_CTL_LEN, 8);
    int heaptag = g_cfg.c1a_t1_rule == 1;
    *c_ctl = heaptag ? ctl_o : ctl_m; *c_ai = heaptag ? ai_o : ai_m;
    if (!alt) return;
    alt->c_ctl = heaptag ? ctl_m : ctl_o; alt->c_ai = heaptag ? ai_m : ai_o;
    alt->ndis = 0; alt->nu = 0;
    if (ai_o == ai_m && ctl_o == ctl_m) return;
    for (size_t u = 0; u < T1B_LEN / 8; u++) {
        if (!memcmp(g_t1b_o + u * 8, g_t1b_m + u * 8, 8)) continue;
        alt->ndis++;
        if (alt->nu < C1A_T1U_MAX) { alt->uoff[alt->nu] = (uint32_t)(u * 8); alt->ukm[alt->nu] = g_t1k_m[u]; alt->uko[alt->nu] = g_t1k_o[u]; alt->nu++; }
    }
}
/* v0.29.5 RBO (m6, design_v0295/rbo/DESIGN.md §2): rbonline's CHECK hash (t1x) takes ctl / ai under a CANONICAL pointer rule into which no
 * per-PC decision enters: the image -> IMAGE | RVA; WorkMemory, located from its root [A_WORKMEM_PTR] (not from the snapshot set) ->
 * REGION | 0xff << 40 | offset; the step thread's stack -> STACK; every other mapped nonzero value -> HEAP (a pinned block of any length,
 * an arena block at any offset, any other heap block). So an E8 / E4 skip on one PC, a rescan, the arena's history or the snapshot-region
 * order can no longer show as a FALSE RB CHECK MISMATCH. Unmapped units stay raw (as the map rule). t1 keeps its own rule (the recordings,
 * the rbreplay hybrid, the light dump's components). h_rbo_check_old=1 (harness only): the v0.29.4 rule (control XE8ASYM). */
#ifdef SC6RB_HARNESS
static int g_h_check_old;
#define RBO_CHECK_OLD() (g_h_check_old != 0)
/* v0.29.6 (review MINOR 7): h_rbo_canon_coarse=1 (harness only) replaces every mapped ctl / ai unit, a NULL and an image value included, by
 * one tag after the canonical rule: a canonicalization too coarse to see a pointer-class divergence. The controls XCTLNH / XCTLIH run it to
 * show that CTLNH / CTLIH (NULL vs heap, image vs heap) would catch a CHECK rule that hides a real ctl / ai divergence. */
static int g_h_canon_coarse;
static void t1_canon_coarse(uint8_t *buf, size_t len, const uint32_t *offs, int noffs) {
    const uint64_t tag = 0xc0a45e0000000000ULL;
    for (int i = 0; i < noffs; i++) if ((size_t)offs[i] + 8 <= len) memcpy(buf + offs[i], &tag, 8);
}
#else
#define RBO_CHECK_OLD() 0
#endif
static uint8_t g_t1b_c[T1B_LEN];
static void t1_ptr_canon(uint64_t *c_ctl, uint64_t *c_ai) {
    C1aNorm n; memset(&n, 0, sizeof n);   /* no arena bounds, no pin table, readable() never asked */
    n.img_lo = g_base; n.img_hi = g_base + g_image_size;
    static C1aRegion wm;
    uintptr_t w = *(volatile uintptr_t *)VA(A_WORKMEM_PTR);
    if (w) { wm.lo = w; wm.len = WORKMEM_SIZE; wm.idx = 0xff; n.regs = &wm; n.nreg = 1; }
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();   /* the stack bounds exactly as norm_ctx takes them */
    MEMORY_BASIC_INFORMATION mbi; volatile int probe = 0;
    n.stk_lo = VirtualQuery((const void *)&probe, &mbi, sizeof mbi) ? (uintptr_t)mbi.AllocationBase : (uintptr_t)tib->StackLimit;
    n.stk_hi = (uintptr_t)tib->StackBase;
    memcpy(g_t1b_c, (const void *)VA(AIBLK), T1B_LEN);
    c1a_norm_buf_map(&n, g_t1b_c, VA(AIBLK), C1A_T1_AI_LEN, k_t1_ai_ptr, C1A_T1_NAI, NULL);
    c1a_norm_buf_map(&n, g_t1b_c + C1A_T1_AI_LEN, VA(CTLBLK), C1A_T1_CTL_LEN, k_t1_ctl_ptr, C1A_T1_NCTL, NULL);
#ifdef SC6RB_HARNESS
    if (g_h_canon_coarse) {   /* v0.29.6 (MINOR 7): the controls only (above) */
        t1_canon_coarse(g_t1b_c, C1A_T1_AI_LEN, k_t1_ai_ptr, C1A_T1_NAI);
        t1_canon_coarse(g_t1b_c + C1A_T1_AI_LEN, C1A_T1_CTL_LEN, k_t1_ctl_ptr, C1A_T1_NCTL);
    }
#endif
    *c_ai = hash_bytes(g_t1b_c, C1A_T1_AI_LEN, 9);
    *c_ctl = hash_bytes(g_t1b_c + C1A_T1_AI_LEN, C1A_T1_CTL_LEN, 8);
}
static void t1_compute(C1aFrame *fr, C1aT1Alt *alt) {
    C1aNorm n; norm_ctx(&n, 1, 0);   /* pointer blocks: heap pointers dropped (C1A_TAG_HEAP) so per-PC heap addresses cannot show */
    uint64_t *c = fr->c; int i = 0;
    fr->fr = *(volatile uint32_t *)VA(A_FRAMECTR);
    c[i++] = fr->fr;                                                                     /* fc   frame counter 0x14470d0c4 */
    c[i++] = rh(0x14485eb28ULL, 0x70, 1);                                                /* tt   TT800 seed word, state, index */
    c[i++] = rh(0x14470e2b0ULL, 0x24, 2);                                                /* mrg  MRG coefficients + state */
    c[i++] = rh(0x144100ea0ULL, 0x1390, 3);                                              /* mt   std::mt19937 + distribution bounds */
    c[i++] = rh(0x1448463a4ULL, 4, rh(0x144844010ULL, 4, 4));                            /* sd   seed copy, reseed word */
    { uint64_t h = 5; static const uint64_t a[5] = { 0x1448463a0ULL, 0x144846364ULL, 0x1448463f8ULL, 0x1448462d4ULL, 0x1448462f4ULL };
      for (int k = 0; k < 5; k++) h = rh(a[k], 4, h);
      c[i++] = h; }                                                                      /* md   round, mode, replay, time scales */
    c[i++] = rh(0x14470d100ULL, 0x20, rh(0x144855700ULL, 0x10, 6));                      /* cin  core input words + third block copy */
    { uint32_t l = g_x.lcg; uint64_t rc = g_x.rand_calls; c[i++] = hash_bytes(&rc, 8, hash_bytes(&l, 4, 7)); }   /* lcg */
    t1_ptr_blocks(&n, &c[i], &c[i + 1], alt); i += 2;                                     /* ctl  player controllers, ai CPU AI blocks 0x14470f390..0x1447153ff
                                                                                          * (v0.24.0 F1: the pointer map rule) */
    {   /* rcv: the game's replay check value ((u8)P1.+0x4495a << 2 | P1.+0x444f2 & 3) << 8 | (the same for P0), computed per frame */
        uint32_t v[2];
        for (int p = 0; p < 2; p++) v[p] = ((uint32_t)*(volatile uint8_t *)VA(ppref(p, 0x4495a)) << 2) | (*(volatile uint16_t *)VA(ppref(p, 0x444f2)) & 3u);
        c[i++] = (uint16_t)((v[1] << 8) | v[0]);
    }
    for (int p = 0; p < 2; p++) {
        uint64_t s = 0x100 + (uint64_t)p * 0x10, h;
        h = rh(ppref(p, 0x94), 4, s); h = rh(ppref(p, 0xa0), 0x10, h); h = rh(ppref(p, 0xc0), 0xc, h); c[i++] = rh(ppref(p, 0x2090), 0x10, h);   /* pos */
        h = rh(ppref(p, 0x124), 4, s + 1); h = rh(ppref(p, 0x16e4), 4, h); h = rh(ppref(p, 0x23c), 4, h); c[i++] = rh(ppref(p, 0x324), 4, h);   /* side */
        {   uint32_t idx = *(volatile uint32_t *)VA(ppref(p, 0x2188));
            h = rh(ppref(p, 0x2150), 8, s + 2); h = hash_bytes(&idx, 4, h);
            if (idx < 0x140) h = rh(ppref(p, 0x2190 + (uint64_t)idx * 0xc), 0xc, h);
            c[i++] = h; }                                                                /* inp  input words, history index + last entry */
        c[i++] = rh(ppref(p, 0x1b38), 0x22, s + 3);                                      /* st   u16 status block +0x1b38..+0x1b59 */
        c[i++] = rh(ppref(p, 0x43dfc), 0x3c, s + 4);                                     /* gau  float gauge block +0x43dfc..+0x43e37 */
        c[i++] = ((uint64_t)*(volatile uint16_t *)VA(ppref(p, 0x4495a)) << 16) | *(volatile uint16_t *)VA(ppref(p, 0x444f2));   /* mv (values) */
        h = rh(ppref(p, 0x16d8), 0x28, s + 5); h = rh(ppref(p, 0x19a0), 0x60, h); c[i++] = rh(ppref(p, 0x4412a), 8, h);         /* aif  AI-read flags */
    }
    fr->t1 = hash_bytes(c, sizeof fr->c, 0x7131);
    if (alt) {   /* the other rule's t1: the same components with its ctl / ai */
        uint64_t c2[C1A_NCOMP]; memcpy(c2, c, sizeof c2);
        c2[8] = alt->c_ctl; c2[9] = alt->c_ai;
        alt->t1 = hash_bytes(c2, sizeof c2, 0x7131);
    }
}
#ifdef SC6RB_HARNESS
#define RBT_T1(x) (x)
#else
#define RBT_T1(x) 0
#endif
/* v0.24.0-C1b2 F1.6: at the first step of each battle. A mapped unit holding a nonzero non-pointer, or an unmapped unit holding an image or
 * arena pointer, means the AI / controller layout is not the census's: C1A T1MAP_BAD (loud, title token; the run continues) */
static void t1map_check(void) {
    C1aNorm n; norm_ctx(&n, 0, 0); n.readable = NULL;
    g_t1map_bad = 0;
    for (int b = 0; b < 2; b++) {
        uint64_t base = b ? CTLBLK : AIBLK; size_t len = b ? C1A_T1_CTL_LEN : C1A_T1_AI_LEN;
        const uint32_t *offs = b ? k_t1_ctl_ptr : k_t1_ai_ptr; int no = b ? C1A_T1_NCTL : C1A_T1_NAI;
        for (size_t o = 0; o + 8 <= len; o += 8) {
            uint64_t v = *(volatile uint64_t *)VA(base + o);
            const char *why = NULL;
            /* C1B3: (C1B-R3-SPEC.md §2.5) a mapped unit is a non-pointer iff (v & 3): the real AI units hold 4-byte-aligned heap interior pointers
             * (+0x7bc ...). h_old_t1align=1 (harness only) restores the v0.24 (v & 7) rule (the negative control). Blind spot: an integer pair
             * with a high word 1..0x7fff and a low word that is a multiple of 4 is not flagged. */
            uint64_t am = RBT_T1(g_cfg.h_old_t1align) ? 7 : 3;
            if (c1a_t1_mapped(offs, no, (uint32_t)o)) { if (v && (v < 0x10000 || v > 0x7fffffffffffULL || (v & am))) why = "nonptr"; }
            else if (v) { int k; c1a_norm_qword(&n, v, &k); if (k == C1A_K_IMAGE || k == C1A_K_ARENA) why = "unmapped_ptr"; }
            if (!why) continue;
            g_t1map_bad++;
            if (g_t1map_bad <= 16) LOG("C1A T1MAP_BAD comp=%s off=%x val=%016llx why=%s battle=%llu (the tier-1 pointer map does not fit this layout: tier-1 verdict FAIL; the run goes on)\n",
                                      b ? "ctl" : "ai", (unsigned)o, (unsigned long long)v, why, (unsigned long long)g_bat);
        }
    }
    if (g_t1map_bad) { g_title_dirty = 1; log_flush(); }
}

/* ------------------------------------------------------------------ setup */
static uint64_t simcfg_hash(void) { return cfg_simcfg_hash(&g_cfg); }   /* v0.23.0-C1b: config.c (the same fields; the rb keys are in rbcfg) */
/* v0.29.9 (OPTA-M2-SPEC.md §18.12): the assets hash is base-independent. v0.22.0 .. v0.29.8 hashed per KHit node the normalized vtable and
 * the raw 0x1c bytes at +0x7c ("the bone ints"): behind a KHitSphere (0x80 bytes, packed in a static pool in the image) those bytes are the
 * next node's absolute vtable or the list's array of absolute node pointers, so the value followed the module base (§18.11: 12 values over
 * 11 bases on PC 1; C1a replay #3, 2026-10-05, refused on assets after a reboot). It is in the DEFAULT refusal set (C1A_REFUSE_DEFAULT =
 * exe..assets: a replay / rbreplay ini without c1a_refuse=) and in mode=rbnet's NETSETUP setup_x (c1a_net_setup, compared across two PCs).
 * Now per node: the list, the class (the normalized vtable, as before) and the authored fields src/rbo_body.c cd_auth hashes (the same
 * table: written once at load, never a world buffer, the per-state flag +0x14, a link or a never-written byte), then per list the count, per
 * player the tree size. A recording made before v0.29.9 carries the old value, which no v0.29.9 launch reproduces: assets differs on every
 * such recording (information under the shipped refusal sets, which leave assets out since 2026-10-05; SETUP_MISMATCH fields=assets under
 * the default set, exactly as that recording already did after any reboot). old_rule = 1: the v0.29.8 hash (the harness reference only). */
static uint64_t assets_hash_r(const C1aNorm *n, int old_rule) {
    static const uint32_t heads[3] = { 0x44478, 0x44498, 0x444b8 };
    static const uint8_t sph[] = { 0x40, 0x44, 0x48, 0x74, 0x78, 0x7c }, area[] = { 0x30, 0x34, 0x38, 0x40, 0x44, 0x48, 0x90, 0x94 },
                         fix[] = { 0x30, 0x34, 0x38, 0x40, 0x44, 0x48, 0x50, 0x54, 0x58, 0x90 };   /* = rbo_body.c cd_auth's */
    uint64_t h = 0x5ea;
    for (int p = 0; p < 2; p++) {
        uintptr_t pl = VA(ppref(p, 0));
        for (int q = 0; q < 3; q++) {
            uintptr_t o = *(uintptr_t *)(pl + heads[q]);
            uint32_t cnt = 0;
            for (int guard = 0; o && guard < 512; guard++) {
                if ((o & 7) || o < 0x10000 || o > 0x7ffffffeffffULL || !mem_readable((void *)o, 0xa0)) break;
                uint64_t raw = *(uint64_t *)o;
                int k; uint64_t vt = c1a_norm_qword(n, raw, &k);
                h = hash_bytes(&vt, 8, h);
                if (old_rule) h = hash_bytes((const void *)(o + 0x7c), 0x1c, h);   /* v0.29.8: the raw span (module-base dependent) */
                else {
                    uint8_t qb = (uint8_t)q; h = hash_bytes(&qb, 1, h);
                    h = hash_bytes((const void *)(o + 0x08), 12, h);   /* +0x08 the kind bit, +0x10 the flags (not +0x14) */
                    h = hash_bytes((const void *)(o + 0x16), 2, h);    /* +0x16 the record type, +0x17 the kind */
                    const uint8_t *f = NULL; int nf = 0;
                    if (raw == VA(0x143e877f0ULL)) { f = sph; nf = (int)sizeof sph; }
                    else if (raw == VA(0x143e877a8ULL)) { f = area; nf = (int)sizeof area; }
                    else if (raw == VA(0x143e87760ULL)) { f = fix; nf = (int)sizeof fix; }
                    for (int i = 0; i < nf; i++) h = hash_bytes((const void *)(o + f[i]), 4, h);
                }
                cnt++;
                o = *(uintptr_t *)(o + 0x18);
            }
            h = hash_bytes(&cnt, 4, h);
        }
        h = rh(ppref(p, 0x455d8), 8, h);   /* CHeaderHitOffset tree size (std::map at PLAYER+0x455d0) */
    }
    return h;
}
static uint64_t assets_hash(const C1aNorm *n) { return assets_hash_r(n, 0); }
#ifdef SC6RB_HARNESS
uint64_t c1a_h_assets(int old_rule) { C1aNorm n; norm_ctx(&n, 0, 0); return assets_hash_r(&n, old_rule); }   /* v0.29.9: the CD cases (test/harness_rbo_cd.inc) */
#endif
/* v0.22.1 (C1A-AUDIT D5/C3): the wind manager 0x144100b60 ([0x14470e038], set by Init). FUN_140333fd0 (Step and IBattleCore +0xc8)
 * runs slot[+0x98][0 .. +0x9c) and flips +0x98; FUN_140334750 (Setup) queues the spawners 0x140334420 / FUN_140334430 into
 * slot[+0x98], and FUN_140334430 spawns the winds (TT800 draws) once the list +0x00 is empty. The slot index only selects which
 * array holds the queue, so the hash covers what decides behavior: the active queue, the count, +0xa0/+0xa4, the spawn parameters,
 * the list (length, node vtables). The index, the per-update output vectors and the node bytes are logged, not hashed. */
static uint8_t g_wb[C1A_WIND_LEN], g_wk[C1A_WIND_LEN / 8];
static int wind_live_view(C1aView *v) {
    if (*(volatile uintptr_t *)VA(0x14470e038ULL) != VA(C1A_WIND)) return 0;
    C1aNorm n2; norm_ctx(&n2, 0, 1);   /* the dump's rule (heap pointers raw, kind 6): comparable with the recording's f0 dump */
    memcpy(g_wb, (const void *)VA(C1A_WIND), C1A_WIND_LEN);
    c1a_norm_buf(&n2, g_wb, VA(C1A_WIND), C1A_WIND_LEN, g_wk);
    v->lo = C1A_WIND; v->len = C1A_WIND_LEN; v->b = g_wb; v->k = g_wk;
    return 1;
}
static uint64_t wind_compute(const C1aNorm *n) {
    C1aView v; memset(&g_wind_now, 0, sizeof g_wind_now); g_wind_list = -1; g_wind_nodes = 0;
    if (!wind_live_view(&v)) return 0x3a1dULL;   /* no manager (the pointer is not the image's 0x144100b60) */
    c1a_wind_view(&v, &g_wind_now);
    uint64_t hv = 0x3a1e, hn = 0x3a1f; int cnt = 0;
    uintptr_t o = *(volatile uintptr_t *)VA(C1A_WIND);
    for (int guard = 0; o && guard < 64; guard++) {
        if ((o & 7) || o < 0x10000 || o > 0x7ffffffeffffULL || !mem_readable((void *)o, 0x70)) { cnt = -2; break; }
        int k; uint64_t vt = c1a_norm_qword(n, *(uint64_t *)o, &k);
        hv = hash_bytes(&vt, 8, hv);
        memcpy(g_nb, (const void *)o, 0x70); c1a_norm_buf(n, g_nb, o, 0x70, NULL); hn = hash_bytes(g_nb, 0x70, hn);
        cnt++;
        o = *(uintptr_t *)(o + 0x10);
    }
    g_wind_list = cnt; g_wind_nodes = hn;
    const C1aWind *w = &g_wind_now;
    int32_t hd = w->head != 0;
    uint64_t h = hash_bytes(&w->cnt, 4, 0x3a20); h = hash_bytes(&w->a0, 4, h); h = hash_bytes(&w->a4, 4, h); h = hash_bytes(w->prm, 16, h);
    h = hash_bytes(w->q, sizeof(uint64_t) * (size_t)w->nq, h); h = hash_bytes(&hd, 4, h); h = hash_bytes(&cnt, 4, h);
    return hash_bytes(&hv, 8, h);
}
/* the injection ranges of this launch, normalized like the dump (tier-2 rule, heap pointers raw with kind 6) */
static uint8_t *g_lv_b, *g_lv_k; static size_t g_lv_cap;
static int live_views(C1aView v[C1A_NRANGE]) {
    size_t tot = 0;
    for (int r = 0; r < C1A_NRANGE; r++) tot += (size_t)(k_c1a_range[r].hi - k_c1a_range[r].lo);
    if (tot > g_lv_cap) {
        if (g_lv_b) { VirtualFree(g_lv_b, 0, MEM_RELEASE); VirtualFree(g_lv_k, 0, MEM_RELEASE); }
        g_lv_b = (uint8_t *)VirtualAlloc(NULL, tot, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_lv_k = (uint8_t *)VirtualAlloc(NULL, tot / 8 + 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_lv_b || !g_lv_k) { g_lv_cap = 0; return 0; }
        g_lv_cap = tot;
    }
    C1aNorm n; norm_ctx(&n, 0, 1);
    size_t o = 0;
    for (int r = 0; r < C1A_NRANGE; r++) {
        size_t len = (size_t)(k_c1a_range[r].hi - k_c1a_range[r].lo);
        memcpy(g_lv_b + o, (const void *)VA(k_c1a_range[r].lo), len);
        c1a_norm_buf(&n, g_lv_b + o, VA(k_c1a_range[r].lo), len, g_lv_k + o / 8);
        v[r].lo = k_c1a_range[r].lo; v[r].len = len; v[r].b = g_lv_b + o; v[r].k = g_lv_k + o / 8;
        o += len;
    }
    return 1;
}

/* C1B3: (C1B-R3-SPEC.md §2.1.1) the move-record tree of player p: size, an in-order walk hashing the 4-byte key at node+0x20 (bounded by
 * 4n+16 steps); every node must be a tracked / pinned / arena block start, readable, or the walk stops (walk=bad, sizes only, loud) */
static int cnode_ok(const C1aNorm *nn, uintptr_t x) {
    if (!x || (x & 7) || x < 0x10000 || x > 0x7ffffffeffffULL) return 0;
    int k; c1a_norm_qword(nn, x, &k);
    if (k != C1A_K_PIN && k != C1A_K_ARENA && !itrack_contains(x, NULL, NULL)) return 0;
    return mem_readable((const void *)x, 0x30);
}
static void content_walk(int p, CTree *t) {
    uintptr_t pl = VA(ppref(p, 0));
    memset(t, 0, sizeof *t);
    t->n = *(volatile uint64_t *)(pl + C1A_CONTENT_SIZE);
    uintptr_t head = *(volatile uintptr_t *)(pl + C1A_CONTENT_HEAD);
    uint64_t h = hash_bytes(&t->n, 8, 0xc0e7ULL + (uint64_t)p);
    if (!head && !t->n) { t->ok = 1; t->has_keys = 1; t->head_null = 1; t->keys = h; return; }   /* no tree object, size 0: consistent (logged head=0) */
    C1aNorm nn; norm_ctx(&nn, 0, 0); nn.readable = NULL;
    if (!cnode_ok(&nn, head) || !*(volatile uint8_t *)(head + 0x19)) return;   /* the sentinel is _Isnil */
    uint64_t limit = 4 * t->n + 16, steps = 0;
    uintptr_t x = *(volatile uintptr_t *)head;   /* _Left of the sentinel = the leftmost node */
    if (!cnode_ok(&nn, x)) return;
    while (x != head) {
        if (++steps > limit || *(volatile uint8_t *)(x + 0x19)) return;
        uint32_t key = *(volatile uint32_t *)(x + 0x20);
        h = hash_bytes(&key, 4, h); t->visited++;
        uintptr_t r = *(volatile uintptr_t *)(x + 0x10);
        if (!cnode_ok(&nn, r)) return;
        if (!*(volatile uint8_t *)(r + 0x19)) {   /* the right subtree's leftmost */
            x = r;
            for (;;) {
                uintptr_t l = *(volatile uintptr_t *)x;
                if (!cnode_ok(&nn, l)) return;
                if (*(volatile uint8_t *)(l + 0x19)) break;
                x = l;
                if (++steps > limit) return;
            }
        } else {   /* climb while x is a right child */
            uintptr_t y = *(volatile uintptr_t *)(x + 8);
            for (;;) {
                if (!cnode_ok(&nn, y)) return;
                if (*(volatile uint8_t *)(y + 0x19) || x != *(volatile uintptr_t *)(y + 0x10)) break;
                x = y; y = *(volatile uintptr_t *)(y + 8);
                if (++steps > limit) return;
            }
            x = y;
        }
    }
    if (t->visited != t->n) return;
    t->ok = 1; t->has_keys = 1; t->keys = h;
}
static uint64_t content_compute(void) {
    g_ct_walkbad = 0;
    uint64_t h = 0x5ee;
    for (int p = 0; p < 2; p++) {
        content_walk(p, &g_ct[p]);
        if (!g_ct[p].ok) g_ct_walkbad = 1;
        int ok = g_ct[p].ok;
        h = hash_bytes(&g_ct[p].n, 8, h); h = hash_bytes(&ok, 4, h);
        if (ok) h = hash_bytes(&g_ct[p].keys, 8, h);
    }
    return h;
}
static void ctree_str(const CTree *t, int have_keys, char *out, size_t cap) {
    if (have_keys) snprintf(out, cap, "%llu:%016llx", (unsigned long long)t->n, (unsigned long long)t->keys);
    else snprintf(out, cap, "%llu:-", (unsigned long long)t->n);
}
static void setup_compute(void) {
    C1aNorm n; norm_ctx(&n, 1, 0);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    uint32_t e[2] = { nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage };
    g_s[C1A_S_EXE] = hash_bytes(e, sizeof e, 0x5e0);
    g_s[C1A_S_PLUGIN] = hash_bytes(SC6RB_VERSION, strlen(SC6RB_VERSION), 0x5e1);
    g_s[C1A_S_SIMCFG] = simcfg_hash();
    g_s[C1A_S_RULES] = nh(&n, 0x14470f1d0ULL, 0x13c, 0x5e3);                          /* SetMatchRule +0x48: 0x14470f1d0..0x14470f30b */
    g_s[C1A_S_FLAGS] = rh(0x14470d0dbULL, 1, rh(0x14470d0d8ULL, 1, 0x5e4));             /* +0x40 flag bytes */
    for (int p = 0; p < 2; p++) {                                                       /* SetupPlayer +0x30 */
        uint64_t h = 0x5e5 + (uint64_t)p;
        h = nh(&n, ppref(p, 0x23c), 4, h); h = nh(&n, ppref(p, 0x24c), 0x18, h); h = nh(&n, ppref(p, 0x26c), 4, h);
        h = nh(&n, ppref(p, 0x2b4a4), 0x58, h); h = nh(&n, ppref(p, 0x34c8), 4, h); h = nh(&n, ppref(p, 0x43dd0), 4, h);
        g_s[C1A_S_PL0 + p] = h;
    }
    { uint64_t h = 0x5e7;                                                               /* control type, controller +0x30..+0x4f */
      for (int p = 0; p < 2; p++) h = rh(ppref(p, 0x324), 4, h);
      for (int p = 0; p < 2; p++) h = nh(&n, CTLBLK + (uint64_t)p * 0x60 + 0x30, 0x20, h);
      g_s[C1A_S_CTL] = h; }
    { uint64_t h = 0x5e8;                                                               /* CPU-AI settings: +0x1a28, +0x2bf8..+0x2d0f */
      for (int p = 0; p < 2; p++) { uint64_t a = AIBLK + (uint64_t)p * AISTR; h = nh(&n, a + 0x1a28, 4, h); h = nh(&n, a + 0x2bf8, 0x118, h); }
      g_s[C1A_S_AI] = h; }
    {   /* stage: the slot 0x14470dedc, the active stage hit table's entry count and the first 256 bytes of its blob (modes.c STAGE) */
        int slot = *(volatile uint8_t *)VA(0x14470dedcULL) ? 1 : 0;
        uintptr_t obj = VA(slot ? 0x144845e80ULL : 0x144844dd0ULL), blob = *(uintptr_t *)obj;
        uint64_t h = rh(0x14470dedcULL, 4, 0x5e9);
        g_stage_slot = slot; g_stage_entries = -1;
        if (blob && !(blob & 7) && mem_readable((void *)blob, 256)) {
            g_stage_entries = *(int16_t *)(blob + 0x2c);
            memcpy(g_nb, (const void *)blob, 256); c1a_norm_buf(&n, g_nb, blob, 256, NULL); h = hash_bytes(g_nb, 256, h);
        }
        g_s[C1A_S_STAGE] = h;
    }
    g_s[C1A_S_ASSETS] = assets_hash(&n);
    {   /* catch-all (information by default): class G and O bytes of the image regions, heap pointers dropped */
        const SnapSet *ss = modes_snapset();
        uint64_t h = 0x5eb;
        if (modes_ss_ready())
            for (int r = 0; r < ss->nreg; r++) {
                if (!IN_IMAGE(ss->reg[r].addr) || is_mt_tail_reg(ss, r) || !region_norm(r, &n, 1, 0)) continue;
                SegCtx t = { r, 0, NULL, 0, 0, h };
                modes_seg_iter(catch_cb, &t);
                h = t.h;
            }
        g_s[C1A_S_CATCH] = h;
    }
    /* UE-fed camera inputs (R4, information): CameraInfoHandler +0x20 tables, the +0x28 matrix, the director scale */
    g_s[C1A_S_UECAM] = nh(&n, 0x14470ed44ULL, 8, nh(&n, 0x14470e180ULL, 0x40, nh(&n, 0x14470def0ULL, 0x1d0, 0x5ec)));
    g_s[C1A_S_WIND] = wind_compute(&n);   /* v0.22.1 (C1A-AUDIT C3): information only */
    g_s[C1A_S_CONTENT] = content_compute();   /* C1B3: (C1B-R3-SPEC.md §2.1.1) the hard gate's own compare is CONTENTCMP (tree0/tree1) */
    for (int p = 0; p < 2; p++) {
        g_ctl_type[p] = *(volatile uint32_t *)VA(ppref(p, 0x324));
        g_ai_loaded[p] = *(volatile uint32_t *)VA(ppref(p, 0x34c8));
        g_cpu_level[p] = *(volatile uint32_t *)VA(AIBLK + (uint64_t)p * AISTR + 0x2c00);
    }
}
static uint64_t setup_total(const uint64_t *s) { return hash_bytes(s, sizeof(uint64_t) * C1A_S_CATCH, 0x5ed); }   /* exe .. assets */
static uint64_t uecam_now(void) { C1aNorm n; norm_ctx(&n, 1, 0); return nh(&n, 0x14470ed44ULL, 8, nh(&n, 0x14470e180ULL, 0x40, nh(&n, 0x14470def0ULL, 0x1d0, 0x5ec))); }

/* ------------------------------------------------------------------ window */
typedef struct { HWND best; LONG area; } WinFind;
static BOOL CALLBACK win_enum(HWND h, LPARAM lp) {
    WinFind *f = (WinFind *)lp;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT rc; if (!GetWindowRect(h, &rc)) return TRUE;
    LONG a = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (!f->best || a > f->area) { f->best = h; f->area = a; }
    return TRUE;
}
static void win_size(void) {
    WinFind f = { NULL, 0 };
    EnumThreadWindows(GetCurrentThreadId(), win_enum, (LPARAM)&f);
    RECT rc;
    if (f.best && GetClientRect(f.best, &rc)) { g_win_w = rc.right - rc.left; g_win_h = rc.bottom - rc.top; } else g_win_w = g_win_h = 0;
}

/* ------------------------------------------------------------------ files */
static void out_path(char *p, size_t cap, const char *suffix) {
    size_t dl = strlen(g_cfg.log_dir);
    snprintf(p, cap, "%s%sc1a_%s_b%llu%s", g_cfg.log_dir, (dl && (g_cfg.log_dir[dl - 1] == '\\' || g_cfg.log_dir[dl - 1] == '/')) ? "" : "\\", g_ts,
             (unsigned long long)g_bat, suffix);
}
void c1a_out_path_ext(char *p, size_t cap, const char *suffix) { out_path(p, cap, suffix); }   /* v0.32.0 review fix B1: src/vm.c's side file */
static void write_header(void) {
    char p[MAX_PATH];
    out_path(p, sizeof p, g_cm == 1 ? ".rec" : ".play");
    const char *nm = strrchr(p, '\\'); snprintf(g_wname, sizeof g_wname, "%s", nm ? nm + 1 : p);
    if (!wr_open(&g_w, p)) { LOG("C1A WARN cannot create %s err=%lu (no %s file this battle)\n", p, GetLastError(), g_cm == 1 ? "record" : "play"); return; }
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    char rs[200]; c1a_refuse_str(g_cfg.c1a_refuse | (1u << C1A_S_CONTENT), rs, sizeof rs);   /* C1B3: content is a hard gate: always listed */
    char names[400]; size_t o = 0; for (int i = 0; i < C1A_NCOMP; i++) o += (size_t)snprintf(names + o, sizeof names - o, "%s%s", i ? "," : "", k_c1a_comp[i]);
    wr_printf(&g_w, "C1AREC version=%d kind=%s plugin=%s battle=%llu ts=%s pid=%lu seed=%u seed_orig=%u seed_override=%d lcg_seed=0x%08x arena_base=0x%llx arena_fixed=%d win=%dx%d "
              "module_base=0x%llx exe_ts=0x%08x exe_soi=0x%x setup=%016llx", C1A_FORMAT_VERSION, g_cm == 1 ? "rec" : "play", SC6RB_VERSION, (unsigned long long)g_bat, g_ts,
              GetCurrentProcessId(), g_seed_used, g_seed_orig, g_seed_ovr, g_lcg0, (unsigned long long)(uintptr_t)g_arena.base, g_arena_fixed, g_win_w, g_win_h,
              (unsigned long long)g_base, (unsigned)nt->FileHeader.TimeDateStamp, (unsigned)nt->OptionalHeader.SizeOfImage, (unsigned long long)setup_total(g_s));
    for (int i = 0; i < C1A_NSETUP; i++) wr_printf(&g_w, " s_%s=%016llx", k_c1a_setup[i], (unsigned long long)g_s[i]);
    {   /* C1B3: (C1B-R3-SPEC.md §2.1.1, §2.1.3) the trees (size:keys, keys "-" on a bad walk) and the mod-set listing */
        char t0[48], t1[48]; ctree_str(&g_ct[0], g_ct[0].ok, t0, sizeof t0); ctree_str(&g_ct[1], g_ct[1].ok, t1, sizeof t1);
        wr_printf(&g_w, " tree0=%s tree1=%s modset=%016llx modset_n=%u", t0, t1, (unsigned long long)g_ms_hash, g_ms_n);
    }
    wr_printf(&g_w, " ctl_type=%u,%u ai_loaded=%u,%u cpu_level=%u,%u stage_slot=%d stage_entries=%d refuse=%s t1=%s t2_every=%d dump_every=%d",
              g_ctl_type[0], g_ctl_type[1], g_ai_loaded[0], g_ai_loaded[1], g_cpu_level[0], g_cpu_level[1], g_stage_slot, g_stage_entries, rs, names, C1A_T2_EVERY, g_cfg.c1a_dump_every);
    {   /* v0.22.1: the wind manager (C3) and the game-thread CRT counts since the demo Init / this Init (C5); information only */
        char ws[256]; c1a_wind_str(&g_wind_now, ws, sizeof ws);
        wr_printf(&g_w, " wind_state=%s wind_list=%d wind_nodes=%016llx crt_gt_demo=%ld/%ld crt_gt_init=%ld/%ld crt_other=%ld", ws, g_wind_list,
                  (unsigned long long)g_wind_nodes, g_demo_seen ? g_crt_gt[0] - g_crt_demo[0] : -1L, g_demo_seen ? g_crt_gt[1] - g_crt_demo[1] : -1L,
                  g_crt_gt[0] - g_crt_bb[0], g_crt_gt[1] - g_crt_bb[1], (long)g_crt_other);
    }
    if (g_cm == 2) {
        char is[64]; c1a_refuse_str(g_cfg.c1a_inject, is, sizeof is);
        wr_printf(&g_w, " play=%s rec_frames=%u rec_file=%s inject=%s injected=%d inj_rules=%u inj_ai=%u pers_rec=%d,%d pers_ue=%d,%d pre_rules=%016llx pre_ai=%016llx plugin_compat=%s",
                  g_rf == RF_OK ? k_pl[g_play] : g_rf == RF_NOFILE ? "NO_FILE" : "BAD_FILE", g_rf_n, g_cfg.replay_file[0] ? g_cfg.replay_file : "-",
                  g_cfg.c1a_inject ? is : "none", g_injected, g_inj_cat_bytes[C1A_S_RULES], g_inj_cat_bytes[C1A_S_AI], g_pers_rec[0], g_pers_rec[1], g_pers_ue[0], g_pers_ue[1],
                  (unsigned long long)g_pre_s[C1A_S_RULES], (unsigned long long)g_pre_s[C1A_S_AI], g_rf_compat ? g_rf_compat : "-");
    }
    {   /* v0.23.0-C1b: the rb keys (rbreplay: ghost=1 mode=rbreplay ... sched=), then the tier-2 rule, the mt tail region, rbcfg */
        char rk[640] = ""; if (is_rbcore_mode(g_mode)) rb_header_keys(rk, sizeof rk);   /* C2: + rbnet */
        char zk[160] = ""; if (is_rbcore_mode(g_mode)) rb_header_keys2(zk, sizeof zk);   /* v0.24.0-C1b2: zone keys (rb_zone=1) */
        char tm[40] = ""; if (g_cfg.c1a_t2_heap_tag == 2) snprintf(tm, sizeof tm, " t2map=%016llx", (unsigned long long)c1a_t2map_hash());   /* C1B3: */
        wr_printf(&g_w, "%s t2rule=%s mt_tail=%d rbcfg=%016llx t1rule=%s t1map=%016llx%s%s\n", rk, t2rule_name(g_cfg.c1a_t2_heap_tag), g_cfg.rb_mt_tail ? 1 : 0,
                  (unsigned long long)cfg_rbcfg_hash(&g_cfg), g_cfg.c1a_t1_rule == 1 ? "heaptag" : "ptrmap1", (unsigned long long)c1a_t1map_hash(), zk, tm);
    }
    if (g_mode == MODE_RBNET) { const char *sb = ns_setup_blob_hex(); if (sb) wr_printf(&g_w, "N 0 setup=%s\n", sb); }   /* C2 (C2-SPEC.md §6.4) */
    LOG("C1A %s open path=%s\n", g_cm == 1 ? "REC" : "PLAY", p);
}

/* dump: header 64 B ("C1ADUMP1", u32 version, u32 frame, u32 battle, u32 nsect, u64 module_base, u64 arena_base, u64 flags (bit 0
 * arena_fixed), 16 B zero); per section: u64 preferred VA, u32 len, u32 0, char name[16], data[len] (pointer-normalized as tier 2,
 * masked bytes kept), class[(len+3)/4] (one char per 4 bytes: G C F R O, X = a per-PC pointer mask, M = another snapshot mask),
 * kind[(len+7)/8] (C1A_K_* per 8-byte unit of the section; 6 = a raw value that is a committed address: a per-launch heap pointer) */
static void cls_cb(void *u, int region, size_t off, size_t len, char cls) {
    if (region != *(int *)u) return;
    for (size_t i = off; i < off + len; i += 4) g_cls[i / 4] = (uint8_t)cls;
}
/* v0.23.0-C1b: a dump is BUILT in memory right after a pass (c1a_pass_compute) and WRITTEN when its frame is confirmed (c1a_confirm);
 * the bytes are the v0.22 file's. C1A_DUMP_SLOTS buffers, one per pending dump frame. */
typedef struct { uint8_t *b; size_t n, cap; uint32_t f; int used, ok, nsect; double us; int why; char cells[96]; } DumpBuf;   /* C1B4: why 0 every, 1 at, 2 t2mm */
static DumpBuf g_db[C1A_DUMP_SLOTS + 1];   /* the last one: the battle-start dump (written at once) */
static uint64_t g_dump_noslot;
static int dump_put(DumpBuf *d, const void *p, size_t n, uint64_t *tot) {
    if (d->n + n > d->cap) {
        size_t nc = (d->n + n + (1u << 20)) & ~(size_t)((1u << 20) - 1);
        uint8_t *nb = (uint8_t *)VirtualAlloc(NULL, nc, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!nb) return 0;
        if (d->b) { memcpy(nb, d->b, d->n); VirtualFree(d->b, 0, MEM_RELEASE); }
        d->b = nb; d->cap = nc;
    }
    memcpy(d->b + d->n, p, n); d->n += n; *tot += n; return 1;
}
static void dump_build(uint32_t f, DumpBuf *d) {
    uint64_t t0 = qpc();
    d->n = 0; d->f = f; d->used = 1; d->why = 0; d->cells[0] = 0;
    C1aNorm n; norm_ctx(&n, 0, 1);
    const SnapSet *ss = modes_snapset();
    int nsect = 1;
    for (int r = 0; modes_ss_ready() && r < ss->nreg; r++) if (IN_IMAGE(ss->reg[r].addr) && !is_mt_tail_reg(ss, r)) nsect++;
    uint8_t hdr[64]; memset(hdr, 0, sizeof hdr);
    uint32_t v = 1, bt = (uint32_t)g_bat; uint64_t mb = g_base, ab = (uintptr_t)g_arena.base, fl = (uint64_t)(g_arena_fixed != 0);
    memcpy(hdr, "C1ADUMP1", 8); memcpy(hdr + 8, &v, 4); memcpy(hdr + 12, &f, 4); memcpy(hdr + 16, &bt, 4); memcpy(hdr + 20, &nsect, 4);
    memcpy(hdr + 24, &mb, 8); memcpy(hdr + 32, &ab, 8); memcpy(hdr + 40, &fl, 8);
    uint64_t tot = 0; int ok = dump_put(d, hdr, sizeof hdr, &tot);
    for (int s = 0; ok && s < nsect; s++) {
        uint64_t pref; size_t len; char name[16]; memset(name, 0, sizeof name);
        if (s < nsect - 1) {   /* the image snapshot regions (core_bss, single, camgate, ini) */
            int r = -1, k = 0;
            for (int i = 0; i < ss->nreg; i++) if (IN_IMAGE(ss->reg[i].addr) && !is_mt_tail_reg(ss, i) && k++ == s) { r = i; break; }
            if (r < 0 || !region_norm(r, &n, 0, 1)) { ok = 0; break; }
            pref = ss->reg[r].addr - g_base + IMG_PREF; len = ss->reg[r].len;
            snprintf(name, sizeof name, "%s", ss->reg[r].name);
            memset(g_cls, 'O', (len + 3) / 4);
            modes_seg_iter(cls_cb, &r);
            for (int m = 0; m < ss->nmask; m++) {
                uintptr_t a = ss->reg[r].addr, m0 = ss->mask[m].addr, m1 = m0 + ss->mask[m].len;
                if (m1 <= a || m0 >= a + len) continue;
                for (uintptr_t q = (m0 > a ? m0 : a) & ~(uintptr_t)3; q < m1 && q < a + len; q += 4) g_cls[(q - a) / 4] = (uint8_t)(is_x_va(q) ? 'X' : 'M');
            }
        } else {   /* the std::mt19937 tail outside every snapshot region (R3) */
            pref = 0x144100ee0ULL; len = 0x1350; snprintf(name, sizeof name, "mt19937_tail");
            if (!scr_need(len)) { ok = 0; break; }
            memcpy(g_scr, (const void *)VA(pref), len);
            c1a_norm_buf(&n, g_scr, VA(pref), len, g_kind);
            memset(g_cls, 'O', (len + 3) / 4);
        }
        uint32_t l32 = (uint32_t)len, z = 0;
        ok = dump_put(d, &pref, 8, &tot) && dump_put(d, &l32, 4, &tot) && dump_put(d, &z, 4, &tot) && dump_put(d, name, 16, &tot) &&
             dump_put(d, g_scr, len, &tot) && dump_put(d, g_cls, (len + 3) / 4, &tot) && dump_put(d, g_kind, (len + 7) / 8, &tot);
    }
    d->ok = ok; d->nsect = nsect;
    d->us = qpc_us(qpc() - t0);
}
static void dump_flush(DumpBuf *d) {
    uint64_t t0 = qpc();
    uint32_t f = d->f;
    char p[MAX_PATH], sfx[48]; snprintf(sfx, sizeof sfx, "_f%u.dump", f);
    out_path(p, sizeof p, sfx);
    HANDLE h = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    d->used = 0;
    if (h == INVALID_HANDLE_VALUE) { LOG("C1A WARN dump %s err=%lu\n", p, GetLastError()); return; }
    int ok = d->ok; DWORD x = 0;
    uint64_t tot = 0;
    if (d->n && (!WriteFile(h, d->b, (DWORD)d->n, &x, NULL) || x != d->n)) ok = 0;
    tot = x;
    CloseHandle(h);
    double us = d->us + qpc_us(qpc() - t0);
    if (us > g_us_dump_max) g_us_dump_max = us;
    g_dumps++;
    const char *nm = strrchr(p, '\\');
    LOG("C1A DUMP battle=%llu frame=%u path=%s bytes=%llu sections=%d ok=%d us=%.0f%s%s%s\n", (unsigned long long)g_bat, f, p, (unsigned long long)tot, d->nsect, ok, us,
        d->why == 1 ? " why=at" : d->why == 2 ? " why=t2mm cells=" : "", d->why == 2 ? d->cells : "", d->why ? " (C1B4 diagnostic dump)" : "");   /* C1B4 */
    wr_printf(&g_w, "D %u file=%s bytes=%llu ok=%d\n", f, nm ? nm + 1 : p, (unsigned long long)tot, ok);
}
static void dump_write(uint32_t f) { DumpBuf *d = &g_db[C1A_DUMP_SLOTS]; dump_build(f, d); dump_flush(d); }
/* C1B4: (round 4, item 1) c1a_dump_at: an explicit frame list (config.c sorts it); every C1a mode dumps these frames like the dump_every ones
 * (the rb modes: the confirmed pass of the frame). c1a_t2mm_dumps: mode=replay, the first tier-2 mismatch of each cell dumps the state right
 * after that step (c1a_step_end confirms at once, so memory still holds it), at most N dumps per battle. */
static uint32_t g_rt_dump[8]; static int g_rt_ndump;   /* C2: the runtime dump frames (DESYNC / KHITDIFF) */
void c1a_dump_frame_add(uint32_t g) { if (g_rt_ndump < 8) g_rt_dump[g_rt_ndump++] = g; }
static int rt_dump_has(uint32_t f) { for (int i = 0; i < g_rt_ndump; i++) if (g_rt_dump[i] == f) return 1; return 0; }
static int dump_at_has(uint32_t f) {
    int lo = 0, hi = g_cfg.c1a_dump_at_n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_cfg.c1a_dump_at[m] == f) return 1; if (g_cfg.c1a_dump_at[m] < f) lo = m + 1; else hi = m - 1; }
    return 0;
}
static uint32_t g_t2mm_cell[C1A_MAXCELL]; static int g_t2mm_ncell, g_t2mm_n; static int g_confirm_now;   /* per battle; g_confirm_now: c1a_step_end */
static uint32_t g_t2mm_new[8]; static int g_t2mm_nnew;   /* the cells of the current cmp_t2 call that differ for the first time */

/* ------------------------------------------------------------------ the replay file */
static int kv_h16(const char *line, const char *key, uint64_t *v) {
    char b[40]; if (!c1a_kv(line, key, b, sizeof b) || strlen(b) != 16) return 0;
    char *e; unsigned long long x = strtoull(b, &e, 16); if (*e) return 0;
    *v = x; return 1;
}
static void rf_bad(const char *path, const char *why) {
    g_rf = RF_BAD; snprintf(g_rf_why, sizeof g_rf_why, "%s", why);
    LOG("C1A BAD_FILE path=%s why=%s (no injection; title play=BAD_FILE)\n", path, why);
}
static void rf_dump_load(const char *recpath, const char *name);
static void rf_load(const char *path) {
    if (!path[0]) { g_rf = RF_NOFILE; LOG("C1A NO_FILE path=(empty replay_file) (no injection; title play=NO_FILE)\n"); return; }
    FILE *f = fopen(path, "rb");
    if (!f) { g_rf = RF_NOFILE; LOG("C1A NO_FILE path=%s errno=%d (no injection; title play=NO_FILE)\n", path, errno); return; }
    static char line[16384];
    if (!fgets(line, sizeof line, f) || strncmp(line, "C1AREC ", 7)) { fclose(f); rf_bad(path, "no_C1AREC_header"); return; }
    int ok = 1;
    if (c1a_kv_u64(line, "version", &ok) != C1A_FORMAT_VERSION || !ok) { fclose(f); rf_bad(path, "version"); return; }
    g_rf_seed = (uint32_t)c1a_kv_u64(line, "seed", &ok); if (!ok) { fclose(f); rf_bad(path, "seed"); return; }
    g_rf_lcg = (uint32_t)c1a_kv_u64(line, "lcg_seed", &ok); if (!ok) { fclose(f); rf_bad(path, "lcg_seed"); return; }
    g_rf_bat = c1a_kv_u64(line, "battle", &ok);
    for (int i = 0; i < C1A_NSETUP; i++) {
        char k[24]; snprintf(k, sizeof k, "s_%s", k_c1a_setup[i]);
        if (kv_h16(line, k, &g_rf_s[i])) { if (i == C1A_S_WIND) g_rf_has_wind = 1; continue; }
        if (i == C1A_S_WIND) continue;   /* v0.22.1: a v0.22.0 recording has no s_wind (compared from its f0 dump instead) */
        if (i == C1A_S_CONTENT) continue;   /* C1B3: a v0.24 (or older) recording has no s_content (its f0 dump's sizes are the reference) */
        fclose(f); rf_bad(path, k); return;
    }
    c1a_kv(line, "plugin", g_rf_plugin, sizeof g_rf_plugin);
    { char tr[16] = ""; g_rf_t2tag = c1a_kv(line, "t2rule", tr, sizeof tr) && !strcmp(tr, "heaptag"); }   /* v0.23.0-C1b: absent = raw (v0.22) */
    g_rf_t2rule = c1a_t2rule_parse(line);   /* C1B3: + map1 */
    if (g_rf_t2rule == 2) g_c1a_t2map_ver = c1a_t2rule_ver(line) == 1 ? 1 : 2;   /* C2: a map1 recording is replayed with map1 (§5.2) */
    {   /* C1B3: (C1B-R3-SPEC.md §2.1.2) the recording's trees "<n>:<h16|->" (v0.25) and its mod-set hash */
        g_rf_ct_src = 0; memset(g_rf_ct, 0, sizeof g_rf_ct);
        char tv[2][48]; int have = c1a_kv(line, "tree0", tv[0], sizeof tv[0]) && c1a_kv(line, "tree1", tv[1], sizeof tv[1]);
        for (int p = 0; have && p < 2; p++) {
            char *e = NULL; g_rf_ct[p].n = strtoull(tv[p], &e, 10);
            if (!e || *e != ':') { have = 0; break; }
            if (!strcmp(e + 1, "-")) g_rf_ct[p].has_keys = 0;
            else if (strlen(e + 1) == 16) { g_rf_ct[p].keys = strtoull(e + 1, NULL, 16); g_rf_ct[p].has_keys = 1; }
            else { have = 0; break; }
        }
        if (have) g_rf_ct_src = 1;
        g_rf_ms_has = kv_h16(line, "modset", &g_rf_ms);
    }
    {   /* v0.24.0-C1b2 F1.3/F1.4: the recording's tier-1 pointer rule (absent = the v0.22/v0.23 heaptag rule) and map */
        char tr[16] = ""; g_rf_t1rule = !c1a_kv(line, "t1rule", tr, sizeof tr) ? 0 : !strcmp(tr, "ptrmap1") ? 1 : 2;
        g_rf_t1map_set = kv_h16(line, "t1map", &g_rf_t1map);
        { char cv[8] = ""; g_rf_conv = c1a_kv(line, "conv", cv, sizeof cv) && !strcmp(cv, "1"); }   /* C2: an rbnet recording converted side 1 */
    }
    char kind[16] = "?"; c1a_kv(line, "kind", kind, sizeof kind);
    char d0name[MAX_PATH] = "";
    uint32_t badl = 0, lineno = 1;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (line[0] == 'D' && line[1] == ' ' && line[2] == '0' && line[3] == ' ') {   /* v0.22.1: the battle-start dump (setup injection) */
            char okv[8] = ""; c1a_kv(line, "ok", okv, sizeof okv);
            if (c1a_kv(line, "file", d0name, sizeof d0name) && strcmp(okv, "1")) d0name[0] = 0;
            continue;
        }
        if (line[0] == 'F' && line[1] == ' ') {
            if (g_rf_n == g_rf_cap) { uint32_t nc = g_rf_cap ? g_rf_cap * 2 : 8192; C1aFrame *nf = (C1aFrame *)realloc(g_rf_fr, sizeof(C1aFrame) * nc); if (!nf) { fclose(f); rf_bad(path, "out_of_memory"); return; } g_rf_fr = nf; g_rf_cap = nc; }
            if (!c1a_parse_frame(line, &g_rf_fr[g_rf_n]) || g_rf_fr[g_rf_n].f != g_rf_n + 1) { if (!badl) badl = lineno; break; }
            g_rf_n++;
        } else if (line[0] == 'T' && line[1] == '2' && line[2] == ' ') {
            if (g_rf_nt2 == g_rf_capt2) { uint32_t nc = g_rf_capt2 ? g_rf_capt2 * 2 : 512; T2Rec *nt = (T2Rec *)realloc(g_rf_t2, sizeof(T2Rec) * nc); if (!nt) { fclose(f); rf_bad(path, "out_of_memory"); return; } g_rf_t2 = nt; g_rf_capt2 = nc; }
            T2Rec *t = &g_rf_t2[g_rf_nt2];
            if (!c1a_parse_t2(line, &t->f, t->c, C1A_MAXCELL, &t->n)) { if (!badl) badl = lineno; break; }
            g_rf_nt2++;
        }
    }
    fclose(f);
    if (badl) { char w[64]; snprintf(w, sizeof w, "malformed_line_%u", badl); rf_bad(path, w); return; }
    if (!g_rf_n) { rf_bad(path, "no_frames"); return; }
    g_rf = RF_OK;
    g_rf_compat = g_rf_s[C1A_S_PLUGIN] != c1a_plugin_hash(SC6RB_VERSION) ? c1a_plugin_compat2(g_rf_s[C1A_S_PLUGIN]) : NULL;   /* v0.23.0-C1b: + 0.22.2 */
    LOG("C1A FILE path=%s kind=%s battle=%llu frames=%u t2=%u seed=%u lcg_seed=0x%08x setup=%016llx plugin=%s%s s_wind=%s (armed: every battle Init except the demo "
        "Init (UE seed %u) runs with the recorded seed; injection after the setup check)\n",
        path, kind, (unsigned long long)g_rf_bat, g_rf_n, g_rf_nt2, g_rf_seed, g_rf_lcg, (unsigned long long)setup_total(g_rf_s), g_rf_plugin[0] ? g_rf_plugin : "?",
        g_rf_compat ? " (an older build this build replays: PLUGIN_COMPAT)" : "", g_rf_has_wind ? "recorded" : "absent(v0.22.0: from the f0 dump)", C1A_DEMO_SEED);
    rf_dump_load(path, d0name);
    if (g_rf_ct_src == 0 && g_rf_dsize_ok) { g_rf_ct_src = 2; for (int p = 0; p < 2; p++) { g_rf_ct[p].n = g_rf_dsize[p]; g_rf_ct[p].has_keys = 0; } }   /* C1B3: legacy: sizes only */
    {   /* C1B3: (C1B-R3-SPEC.md §2.6.2) the tier-2 rule: follow took the recording's (config.c); an explicit value that differs refuses at the first step */
        int now = g_cfg.c1a_t2_heap_tag, eq = g_rf_t2rule == now;
        LOG("C1A T2RULE rec=%s now=%s verdict=%s%s\n", t2rule_name(g_rf_t2rule), t2rule_name(now), eq ? "equal" : "REFUSE",
            g_cfg.c1a_t2_follow ? " (c1a_t2_heap=follow: the recording's rule)" : eq ? "" : " (an explicit c1a_t2_heap that is not the recording's rule: the setup check refuses, title play=SETUP_MISMATCH(t2rule))");
    }
    LOG("C1A T1RULE rec=%s rec_t1map=%016llx now=%s now_t1map=%016llx%s (v0.24.0 F1: a recording without t1rule is compared with the map rule and the v0.23 rule; "
        "a frame only the v0.23 rule matches is C1A T1NORM, not a mismatch)\n", g_rf_t1rule == 1 ? "ptrmap1" : g_rf_t1rule == 2 ? "heaptag" : "absent(v0.23_heaptag)",
        (unsigned long long)(g_rf_t1map_set ? g_rf_t1map : 0), g_cfg.c1a_t1_rule == 1 ? "heaptag(CTRL)" : "ptrmap1", (unsigned long long)c1a_t1map_hash(),
        g_rf_t1map_set && g_rf_t1map != c1a_t1map_hash() ? " T1MAP_DIFFERS (another pointer map: tier-1 ai/ctl are not comparable; mismatches will show)" : "");
}

/* v0.22.1: the recording's battle-start dump (the D 0 line; next to the .rec). It holds the recorded values the setup injection writes
 * (C1-C2), and the recorded wind manager (C3). Self-check: the rules / ai setup hashes recomputed from it must equal the header's. */
static void rf_dump_load(const char *recpath, const char *name) {
    g_rf_dump_ok = 0; g_rf_dump_path[0] = 0;
    const char *why = NULL;
    if (!name[0]) why = "no_D0_line_ok1_in_the_recording";
    else {
        const char *sl = strrchr(recpath, '\\'), *sl2 = strrchr(recpath, '/');
        if (sl2 > sl) sl = sl2;
        if (strchr(name, '\\') || strchr(name, '/') || strstr(name, "..")) why = "D0_file_name_has_a_path";
        else snprintf(g_rf_dump_path, sizeof g_rf_dump_path, "%.*s%s", sl ? (int)(sl - recpath + 1) : 0, recpath, name);
    }
    if (!why) {
        HANDLE h = CreateFileA(g_rf_dump_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER sz; sz.QuadPart = 0;
        if (h == INVALID_HANDLE_VALUE) why = "cannot_open";
        else if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 64 || sz.QuadPart > (64LL << 20)) why = "size";
        else {
            g_rf_dn = (size_t)sz.QuadPart;
            g_rf_dbuf = (uint8_t *)VirtualAlloc(NULL, g_rf_dn, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            DWORD got = 0;
            if (!g_rf_dbuf) why = "out_of_memory";
            else if (!ReadFile(h, g_rf_dbuf, (DWORD)g_rf_dn, &got, NULL) || got != g_rf_dn) why = "read";
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    if (!why) {
        const char *pw = NULL;
        if (!c1a_dump_parse(g_rf_dbuf, g_rf_dn, &g_rf_dump, &pw)) { static char w[64]; snprintf(w, sizeof w, "parse_%s", pw ? pw : "?"); why = w; }
        else if (g_rf_dump.frame == 0 && g_rf_dump.battle == (uint32_t)g_rf_bat) g_rf_dsize_ok = c1a_content_sizes_from_dump(&g_rf_dump, g_rf_dsize);   /* C1B3: */
    }
    if (!why && g_rf_dump.frame != 0) why = "not_frame_0";
    if (!why && g_rf_dump.battle != (uint32_t)g_rf_bat) why = "other_battle";
    for (int r = 0; !why && r < C1A_NRANGE; r++)
        if (!c1a_dump_view(&g_rf_dump, k_c1a_range[r].lo, (size_t)(k_c1a_range[r].hi - k_c1a_range[r].lo), &g_rf_view[r])) why = "range_not_in_dump";
    char rh_s[16] = "-", ah_s[16] = "-";
    if (!why) {
        int ex1, ex2;
        uint64_t hr = c1a_hash_rules_view(&g_rf_view[C1A_R_RULES], &ex1), ha = c1a_hash_ai_view(&g_rf_view[C1A_R_AI], &ex2);
        snprintf(rh_s, sizeof rh_s, "%s", !ex1 ? "inexact" : hr == g_rf_s[C1A_S_RULES] ? "match" : "DIFFERS");
        snprintf(ah_s, sizeof ah_s, "%s", !ex2 ? "inexact" : ha == g_rf_s[C1A_S_AI] ? "match" : "DIFFERS");
        if ((ex1 && hr != g_rf_s[C1A_S_RULES]) || (ex2 && ha != g_rf_s[C1A_S_AI])) why = "dump_does_not_match_the_header";
        if (!ex1 || !ex2) LOG("C1A WARN the rules/ai hashes cannot be recomputed exactly from the dump (a partial unit holds a pointer); self-check skipped\n");
    }
    g_rf_has_wview = !why && c1a_dump_view(&g_rf_dump, C1A_WIND, C1A_WIND_LEN, &g_rf_wview);
    if (why) {
        snprintf(g_rf_dump_why, sizeof g_rf_dump_why, "%s", why);
        LOG("C1A %s path=%s why=%s (%s)\n", g_cfg.c1a_inject ? "NO_SETUP_DUMP" : "WARN setup_dump_unavailable", g_rf_dump_path[0] ? g_rf_dump_path : "-", why,
            g_cfg.c1a_inject ? "c1a_setup_inject needs the recording's battle-start dump: nothing will be injected; title play=NO_SETUP_DUMP"
                             : "c1a_setup_inject=none: not needed");
        return;
    }
    g_rf_dump_ok = 1;
    LOG("C1A SETUP_DUMP path=%s bytes=%zu frame=%u battle=%u sections=%d rules_hash=%s ai_hash=%s wind=%s ok=1 (the recording's battle-start state for the setup injection)\n",
        g_rf_dump_path, g_rf_dn, g_rf_dump.frame, g_rf_dump.battle, g_rf_dump.nsect, rh_s, ah_s, g_rf_has_wview ? "yes" : "no");
}

/* ------------------------------------------------------------------ hooks */
void c1a_init(void) {
    g_cm = (g_mode == MODE_RECORD || g_mode == MODE_RBLIVE || g_mode == MODE_RBNET || g_mode == MODE_RBONLINE) ? 1 : (g_mode == MODE_REPLAY || g_mode == MODE_RBREPLAY) ? 2 : 0;   /* v0.23.0-C1b: rbreplay replays; v0.24.0: rblive records; optA M2: rbonline records its armed battles */
    if (g_mode != MODE_RBONLINE) modset_startup();   /* C1B3: (C1B-R3-SPEC.md §2.1.3) every mode, at startup, before any battle. optA M2: not in rbonline (the mod
                                                      * list is never compared there, spec §4; no file walk of the game folder at process start) */
    if (g_cm == 1 && g_mode == MODE_RECORD && g_cfg.c1a_t2_follow) {   /* C1B3: (§4.1) follow exists only in replay / rbreplay (rblive: CFG C1B REFUSE) */
        LOG("CFG C1A REFUSE key=c1a_t2_heap value=follow need=raw|tag|map (follow takes a recording's rule: replay / rbreplay only); mode=record records nothing this launch\n");
        g_cm = 0; g_title_dirty = 1;
    }
    if (!g_cm) return;
    {   /* C1B3: (C1B-R3-SPEC.md §2.1.2) the content gate is hard: no key turns it off */
        char rs[200]; c1a_refuse_str(g_cfg.c1a_refuse, rs, sizeof rs);
        LOG("CFG C1A content_gate=hard c1a_refuse=%s%s t2heap=%s(%s)\n", rs, g_cfg.c1a_refuse == 0 ? " (content stays on)" : "",
            g_cfg.c1a_t2_heap_s[0] ? g_cfg.c1a_t2_heap_s : "tag", t2rule_name(g_cfg.c1a_t2_heap_tag));
    }
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(g_ts, sizeof g_ts, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    char rs[200]; c1a_refuse_str(g_cfg.c1a_refuse, rs, sizeof rs);
    LOG("C1A MODE mode=%s ts=%s dump_every=%d t2_every=%d refuse=%s replay_file=%s ifc_census=%d (straight live passes: l3=%d l4=%d query_policy=%s, routing on, "
        "the camera block pinned as in roll7; no resim, no commit; every step recorded%s)\n", g_cm == 1 ? "record" : "replay", g_ts, g_cfg.c1a_dump_every, C1A_T2_EVERY, rs,
        g_cfg.replay_file[0] ? g_cfg.replay_file : "-", g_cfg.ifc_census, g_cfg.l3_ini, g_cfg.l4, g_cfg.query_forward ? "forward" : "null",
        g_cm == 2 ? " and the recorded input injected" : "");
    if (g_cfg.c1a_refuse_bad) LOG("C1A WARN c1a_refuse: unknown category name ignored (allowed: exe plugin simcfg rules flags pl0 pl1 ctl ai stage assets catch uecam wind | none | default)\n");
    if (!g_cfg.ifc_census) LOG("C1A WARN ifc_census=0: the UE->core calls are not counted (ifc= has only the Setup calls)\n");
    if (g_cm == 2) {
        char is[64]; c1a_refuse_str(g_cfg.c1a_inject, is, sizeof is);
        LOG("C1A INJECT setting=%s%s (v0.22.1, C1A-AUDIT.md C1-C6: at the replay's first step, ONLY the CpuPersonality-derived bytes of a CPU side "
            "whose recorded personality differs are taken from the recording's battle-start dump (rule block +0x10, Setup copy +0x40, AI +0x2c0c, "
            "AI tables +0x38..+0x987); any other difference refuses. A pass with injection is NOT evidence that UE gives equal setups)\n",
            g_cfg.c1a_inject ? is : "none", g_cfg.c1a_inject_bad ? " (unknown name ignored; allowed: rules ai | none)" : "");
        rf_load(g_cfg.replay_file);
    }
#ifdef SC6RB_HARNESS
    if (g_cm == 1 && g_cfg.h_t2map_ver == 1) { g_c1a_t2map_ver = 1; LOG("C1A T2MAP harness control h_t2map_ver=1 (a map1 recording)\n"); }
    g_h_check_old = g_mode == MODE_RBONLINE ? cfg_geti("h_rbo_check_old", 0) : 0;   /* v0.29.5 RBO (m6): the control XE8ASYM */
    if (g_h_check_old) LOG("C1A harness control h_rbo_check_old=1 (XE8ASYM: rbonline's CHECK hash keeps the v0.29.4 pin-length rule for ctl / ai)\n");
    g_h_canon_coarse = g_mode == MODE_RBONLINE ? cfg_geti("h_rbo_canon_coarse", 0) : 0;   /* v0.29.6 (MINOR 7): the controls XCTLNH / XCTLIH */
    if (g_h_canon_coarse) LOG("C1A harness control h_rbo_canon_coarse=1 (XCTLNH / XCTLIH: every mapped ctl / ai unit, NULL and image included, hashes as one tag)\n");
#endif
    if (g_cfg.c1a_t2_heap_tag == 2)   /* C2: after rf_load (a map1 recording selects map1) */
        LOG("C1A T2MAP rule=%s units=%d hash=%016llx\n", t2rule_name(2), c1a_t2map_units(), (unsigned long long)c1a_t2map_hash());
    g_title_dirty = 1;
}

/* C1B3: (C1B-R3-SPEC.md §2.1.3) the mod-set listing: every regular file under <SoulcaliburVI>\Content (recursive), <SoulcaliburVI>\Binaries\Win64\plugins
 * (recursive) and <SoulcaliburVI>\Binaries\Win64\*.dll; not sc6rb.asi / sc6rb.ini, not .log .txt .tmp .dmp, nothing under log_dir. Read-only
 * file stats (FindFirstFile); the listing goes to <log_dir>\modset_<stamp>.txt, never the game folder. FNV-1a 64 over the sorted
 * (path, size, mtime) records. h_modset_root (harness only) names the <SoulcaliburVI> folder; h_modset=1 (harness only) changes the hash. */
typedef struct { char p[MAX_PATH]; uint64_t size, mtime; } MsEnt;
static MsEnt *g_ms; static int g_ms_cnt, g_ms_cap;
static void ms_add(const char *rel, uint64_t size, uint64_t mtime) {
    if (g_ms_cnt == g_ms_cap) { int nc = g_ms_cap ? g_ms_cap * 2 : 1024; MsEnt *ne = (MsEnt *)realloc(g_ms, sizeof(MsEnt) * (size_t)nc); if (!ne) return; g_ms = ne; g_ms_cap = nc; }
    MsEnt *e = &g_ms[g_ms_cnt++];
    size_t i = 0; for (; rel[i] && i + 1 < sizeof e->p; i++) { char c = rel[i]; e->p[i] = c == '\\' ? '/' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    e->p[i] = 0; e->size = size; e->mtime = mtime;
}
static int ms_excluded(const char *name, const char *full) {
    if (!_stricmp(name, "sc6rb.asi") || !_stricmp(name, "sc6rb.ini")) return 1;
    const char *dot = strrchr(name, '.');
    if (dot && (!_stricmp(dot, ".log") || !_stricmp(dot, ".txt") || !_stricmp(dot, ".tmp") || !_stricmp(dot, ".dmp"))) return 1;
    size_t ll = strlen(g_cfg.log_dir);
    if (ll && !_strnicmp(full, g_cfg.log_dir, ll) && (full[ll] == '\\' || full[ll] == '/' || g_cfg.log_dir[ll - 1] == '\\')) return 1;   /* our own logs */
    return 0;
}
static void ms_walk(const char *root, const char *dir, int recursive, const char *ext_only, int depth) {
    if (depth > 32) return;
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char full[MAX_PATH]; snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (recursive && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ms_walk(root, full, 1, ext_only, depth + 1); continue; }
        if (ext_only) { const char *dot = strrchr(fd.cFileName, '.'); if (!dot || _stricmp(dot, ext_only)) continue; }
        if (ms_excluded(fd.cFileName, full)) continue;
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, mt = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        ms_add(full + strlen(root) + 1, size, mt);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static int ms_cmp(const void *a, const void *b) { return strcmp(((const MsEnt *)a)->p, ((const MsEnt *)b)->p); }
static void modset_startup(void) {
    if (g_ms_done) return;
    g_ms_done = 1;
    char root[MAX_PATH] = "";
#ifdef SC6RB_HARNESS
    if (g_cfg.h_modset_root[0]) snprintf(root, sizeof root, "%s", g_cfg.h_modset_root);
#endif
    if (!root[0]) {   /* the exe is <game>\SoulcaliburVI\Binaries\Win64\SoulcaliburVI.exe: the root is <game>\SoulcaliburVI */
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
        char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        size_t l = strlen(exe), k = strlen("\\Binaries\\Win64");
        if (l > k && !_stricmp(exe + l - k, "\\Binaries\\Win64")) { exe[l - k] = 0; snprintf(root, sizeof root, "%s", exe); }
    }
    g_ms_cnt = 0;
    uint64_t h = 0xcbf29ce484222325ULL;
    if (root[0]) {
        char d[MAX_PATH];
        snprintf(d, sizeof d, "%s\\Content", root); ms_walk(root, d, 1, NULL, 0);
        snprintf(d, sizeof d, "%s\\Binaries\\Win64\\plugins", root); ms_walk(root, d, 1, NULL, 0);
        snprintf(d, sizeof d, "%s\\Binaries\\Win64", root); ms_walk(root, d, 0, ".dll", 0);
        if (g_ms_cnt > 1) qsort(g_ms, (size_t)g_ms_cnt, sizeof(MsEnt), ms_cmp);
        for (int i = 0; i < g_ms_cnt; i++) {
            const MsEnt *e = &g_ms[i];
            for (const char *c = e->p; ; c++) { h ^= (uint8_t)*c; h *= 0x100000001b3ULL; if (!*c) break; }
            for (int b = 0; b < 8; b++) { h ^= (uint8_t)(e->size >> (8 * b)); h *= 0x100000001b3ULL; }
            for (int b = 0; b < 8; b++) { h ^= (uint8_t)(e->mtime >> (8 * b)); h *= 0x100000001b3ULL; }
        }
    }
#ifdef SC6RB_HARNESS
    if (g_cfg.h_modset) h ^= 0x5eed5eed5eed5eedULL;   /* GMS: a fake mod-set change */
#endif
    g_ms_hash = h; g_ms_n = (uint32_t)g_ms_cnt;
    int newest = -1; for (int i = 0; i < g_ms_cnt; i++) if (newest < 0 || g_ms[i].mtime > g_ms[newest].mtime) newest = i;
    char lp[MAX_PATH] = "-";
    {   SYSTEMTIME st; GetLocalTime(&st);
        size_t dl = strlen(g_cfg.log_dir);
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s%smodset_%04d%02d%02d_%02d%02d%02d_%lu.txt", g_cfg.log_dir, (dl && (g_cfg.log_dir[dl - 1] == '\\' || g_cfg.log_dir[dl - 1] == '/')) ? "" : "\\",
                                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "# sc6rb %s modset root=%s n=%d hash=%016llx (relative path, size, last-write time in 100 ns)\n", SC6RB_VERSION, root[0] ? root : "-", g_ms_cnt, (unsigned long long)h);
            for (int i = 0; i < g_ms_cnt; i++) fprintf(f, "%s\t%llu\t%llu\n", g_ms[i].p, (unsigned long long)g_ms[i].size, (unsigned long long)g_ms[i].mtime);
            fclose(f);
            const char *nm = strrchr(p, '\\'); snprintf(lp, sizeof lp, "%s", nm ? nm + 1 : p);
        }
    }
    char nw[MAX_PATH + 32] = "-"; if (newest >= 0) snprintf(nw, sizeof nw, "%s@%llu", g_ms[newest].p, (unsigned long long)g_ms[newest].mtime);
    LOG("MODSET n=%u hash=%016llx newest=%s list=%s%s\n", g_ms_n, (unsigned long long)g_ms_hash, nw, lp,
        root[0] ? "" : " root=unknown (the exe is not under <game>\\SoulcaliburVI\\Binaries\\Win64: nothing listed)");
}

/* v0.22.1 (C1A-AUDIT C4): the title/menu demo Init (UE's default rule, seed 0x765) keeps UE's seed: overriding it reseeded the game thread's
 * CRT stream (Init: srand(seed >> 4) + (seed & 0xfff) rand calls) differently from the record launch (1590979972 vs 1893) */
int c1a_seed_for_init(uint32_t ue_seed, uint32_t *used) {
    *used = ue_seed;
    if (g_mode == MODE_RBNET) { g_seed_demo_skip = ue_seed == C1A_DEMO_SEED; return ns_seed_for_init(ue_seed, used); }   /* C2: the host's seed_k (§2.4) */
    g_seed_demo_skip = 0;
    if (g_cm != 2 || g_rf != RF_OK) return 0;
    if (ue_seed == C1A_DEMO_SEED) { g_seed_demo_skip = 1; return 0; }
    *used = g_rf_seed;
    return 1;
}

/* v0.22.1 (C1A-AUDIT C5): CRT rand/srand calls that do not take the LCG, counted per thread (main.c hk_rand / hk_srand) */
void c1a_crt_note(int is_srand) {
    if (!g_cm) return;
    if (GetCurrentThreadId() == g_gt_tid) g_crt_gt[is_srand ? 1 : 0]++;
    else InterlockedIncrement(&g_crt_other);
}

/* v0.22.1 (C1A-AUDIT C3): IBattleCore +0xc8 (FUN_1402dcb30: wind update FUN_140333fd0 and the bone-scale block) inside a battle: TT800
 * index 0x14485eb94, seed word 0x14485eb28 and the TT800 hash before and after each call (budget 50 lines; counts in the END line) */
static uint64_t tt_hash(void) { return hash_bytes((const void *)VA(0x14485eb28ULL), 0x70, 1); }
void c1a_ifc_pre(int slot) {
    if (!g_cm || slot != 0xc8 / 8 || !g_in_bat) return;
    g_c8_in = 1; g_c8_idx0 = *(volatile uint32_t *)VA(0x14485eb94ULL); g_c8_w0 = *(volatile uint32_t *)VA(0x14485eb28ULL); g_c8_tt0 = tt_hash();
}
void c1a_ifc_post(int slot) {
    if (!g_cm || slot != 0xc8 / 8 || !g_c8_in) return;
    g_c8_in = 0;
    uint32_t i1 = *(volatile uint32_t *)VA(0x14485eb94ULL), w1 = *(volatile uint32_t *)VA(0x14485eb28ULL); uint64_t t1 = tt_hash();
    int moved = t1 != g_c8_tt0;
    if (!g_f) { g_c8_pre++; g_c8_pre_moved += moved; return; }   /* between this battle's Init and its first step: counted (SETUP line) */
    g_c8_n++;
    if (moved) g_c8_moved++;
    if (g_c8_lines < 50) {
        g_c8_lines++;
        LOG("C1A IFCC8 battle=%llu frame=%u n=%u tt_idx=%u->%u eb28=%08x->%08x tt=%016llx->%016llx moved=%d (UE's +0xc8 inside the battle: %s)\n",
            (unsigned long long)g_bat, g_f, g_c8_n, g_c8_idx0, i1, g_c8_w0, w1, (unsigned long long)g_c8_tt0, (unsigned long long)t1, moved,
            moved ? "it drew the core RNG: UE-timed, a C1a risk (C1A-AUDIT C3)" : "no core RNG draw");
    }
}

static void play_end(const char *why) {
    if (g_end_logged) return;
    g_end_logged = 1;
    LOG("C1A END battle=%llu frames=%u tier1_mismatch_frames=%u first=%u tier2_mismatch_points=%u tier2_points=%u first_t2=%u ifc_diff_frames=%u rflag_diff_frames=%u why=%s "
        "injected_setup=%d c8_in_fight=%u c8_moved_tt=%u pers_changes=%u (the recording has %u frames; injection stops here) t1norm=%u t1norm_rescue=%u t1norm_oldrule=%u\n",
        (unsigned long long)g_bat, g_f > g_rf_n ? g_rf_n : g_f, g_t1mm, g_t1first, g_t2mm, g_t2_compared, g_t2first, g_ifcd, g_rfd, why, g_injected, g_c8_n, g_c8_moved,
        g_pers_changes, g_rf_n, g_t1norm, g_t1norm_rescue, g_t1norm_old);
    if (g_play == PL_OK) g_play = PL_END;
    g_title_dirty = 1;
    log_flush();
}

static void close_battle_file(const char *why) {
    if (g_cm == 2 && g_play == PL_OK && g_f) play_end(why);   /* the END line also when the .play file could not be created */
    if (!wr_is_open(&g_w)) return;
    if (g_cm == 2) wr_printf(&g_w, "END frames=%u play=%s tier1_mismatch_frames=%u first=%u tier2_mismatch_points=%u ifc_diff_frames=%u rflag_diff_frames=%u dumps=%u why=%s "
                             "injected=%d c8=%u c8_moved=%u pers_changes=%u\n", g_f, g_rf == RF_OK ? k_pl[g_play] : g_rf == RF_NOFILE ? "NO_FILE" : "BAD_FILE", g_t1mm,
                             g_t1first, g_t2mm, g_ifcd, g_rfd, g_dumps, why, g_injected, g_c8_n, g_c8_moved, g_pers_changes);
    else wr_printf(&g_w, "END frames=%u t2=%u dumps=%u why=%s c8=%u c8_moved=%u pers_changes=%u\n", g_f, g_t2n, g_dumps, why, g_c8_n, g_c8_moved, g_pers_changes);
    wr_close(&g_w);
    LOG("C1A %s battle=%llu path=%s frames=%u t2=%u dumps=%u bytes=%llu why=%s t1_us_avg=%.1f t2_us_avg=%.0f dump_us_max=%.0f\n", g_cm == 1 ? "RECEND" : "PLAYEND",
        (unsigned long long)g_bat, g_wname, g_f, g_t2n, g_dumps, (unsigned long long)g_w.bytes, why, g_n_t1 ? g_us_t1 / (double)g_n_t1 : 0.0,
        g_n_t2 ? g_us_t2 / (double)g_n_t2 : 0.0, g_us_dump_max);
}

void c1a_battle_begin(uint64_t battle) {
    if (!g_cm) return;
    close_battle_file("init");
    g_bat = battle; g_f = 0; g_setup_calls = 0;
    g_t1_lines = g_t2_lines = g_ifc_lines = g_rfl_lines = g_uecam_lines = 0;
    g_t1mm = g_t1first = g_t2mm = g_t2first = g_ifcd = g_rfd = g_t2_cursor = g_dumps = g_t2n = g_t2_compared = 0;
    g_t1norm = g_t1norm_lines = g_t1map_bad = g_mm_written = g_mm_noslot = 0; g_t1norm_rescue = g_t1norm_old = 0; g_skip_bat = 0; memset(g_mm_eq, 0, sizeof g_mm_eq);   /* v0.24.0-C1b2 */
    g_t2mm_ncell = g_t2mm_n = 0;   /* C1B4 */
    g_rt_ndump = 0;   /* C2 */
    for (int i = 0; i <= C1A_DUMP_SLOTS; i++) g_db[i].used = 0;
    g_prev_rec_rp = g_prev_now_rp = 0; g_end_logged = 0; g_uecam = 0;
    g_us_t1 = g_us_t2 = g_us_dump_max = 0; g_n_t1 = g_n_t2 = 0;
    for (int i = 0; i < IFC_NSLOTS; i++) g_ifc_base[i] = ifc_family.counts[i];
    g_play = g_cm == 2 && g_rf == RF_OK ? PL_ARMED : PL_NONE;
    g_injected = 0; memset(g_inj_cat_bytes, 0, sizeof g_inj_cat_bytes); memset(g_pre_s, 0, sizeof g_pre_s);
    g_pers_rec[0] = g_pers_rec[1] = g_pers_ue[0] = g_pers_ue[1] = -1;
    g_c8_n = g_c8_moved = g_c8_pre = g_c8_pre_moved = g_c8_lines = 0; g_c8_in = 0; g_in_bat = 1;
    /* C5: the game thread is the one that runs Init; the counters at this Init (before Init's own srand + draws) */
    g_gt_tid = GetCurrentThreadId();
    g_crt_bb[0] = g_crt_gt[0]; g_crt_bb[1] = g_crt_gt[1]; g_crt_bb_other = g_crt_other;
    vm_battle_begin(battle);   /* v0.32.0 review fix B1 (no-op unless a VM key is on) */
}

void c1a_after_init(uint32_t seed_orig, uint32_t seed_used, int overridden) {
    if (!g_cm) return;
    g_seed_orig = seed_orig; g_seed_used = seed_used;
    g_seed_ovr = overridden;
    if (seed_orig == C1A_DEMO_SEED) { g_demo_seen = 1; g_crt_demo[0] = g_crt_bb[0]; g_crt_demo[1] = g_crt_bb[1]; g_crt_demo_other = g_crt_bb_other; }
    if (g_seed_ovr) {
        if (g_x.lcg != g_rf_lcg) LOG("C1A WARN the LCG after Init (0x%08x) is not the recorded lcg_seed (0x%08x); set to the recorded value\n", g_x.lcg, g_rf_lcg);
        g_x.lcg = g_rf_lcg; g_x.rand_calls = 0;
    }
    g_lcg0 = g_x.lcg;
    g_skip_bat = (g_mode == MODE_RBLIVE || g_mode == MODE_RBNET) && seed_orig == C1A_DEMO_SEED;   /* v0.24.0-C1b2: rblive records no demo battle; C2: + rbnet */
    LOG("C1A SEED battle=%llu ue_seed=%u used=%u override=%d lcg=0x%08x%s crt_gt=%ld/%ld crt_other=%ld (game-thread CRT rand/srand calls off the LCG since launch, "
        "after this Init)\n", (unsigned long long)g_bat, seed_orig, seed_used, g_seed_ovr, g_lcg0,
        g_seed_demo_skip ? " why=demo_default_seed (the menu demo Init keeps UE's seed: C1A-AUDIT C4)" : "", (long)g_crt_gt[0], (long)g_crt_gt[1], (long)g_crt_other);
    g_seed_demo_skip = 0;
}

void c1a_note_setup(void) { if (g_cm) g_setup_calls++; }

static int is_query_ifc(int off) { return off == 0x08 || off == 0x10 || off == 0x80 || off == 0xa0 || off == 0xe0 || off == 0x100 || off == 0x110 || off == 0x118; }
static void ifc_collect(char *out, uint32_t *q) {
    size_t o = 0; uint32_t qs = 0;
    out[0] = 0;
    for (int i = 0; i < IFC_NSLOTS; i++) {
        uint64_t c = ifc_family.counts[i], d = c - g_ifc_base[i];
        g_ifc_base[i] = c;
        if (i * 8 == IFC_SETUP) { d = g_setup_calls; g_setup_calls = 0; }   /* Setup is hooked (not a census thunk): counted by c1a_note_setup */
        if (!d) continue;
        if (is_query_ifc(i * 8)) { qs += (uint32_t)d; continue; }
        if (o + 24 < C1A_IFC_CAP) o += (size_t)snprintf(out + o, C1A_IFC_CAP - o, "%s%x:%llu", o ? "," : "", i * 8, (unsigned long long)d);
    }
    if (!o) snprintf(out, C1A_IFC_CAP, "-");
    *q = qs;
}

/* v0.22.1: names and bytes for the SETUP_DIFF / SETUP_WRITE lines */
static void span_name(uint64_t va, char *out, size_t cap) {
    if (va >= C1A_INJ_AI && va < C1A_INJ_AI + 2 * C1A_INJ_AISTR) {
        int s = (int)((va - C1A_INJ_AI) / C1A_INJ_AISTR); unsigned off = (unsigned)((va - C1A_INJ_AI) % C1A_INJ_AISTR);
        snprintf(out, cap, "AI%d+0x%x%s", s, off, off == C1A_INJ_PAI ? "(CpuPersonality)" : off >= C1A_INJ_TAB_LO && off < 0x948 ? "(table)" :
                 off >= 0x948 && off < C1A_INJ_TAB_HI ? "(PSNL_entry)" : off == 0x1a28 || off == 0x2c00 ? "(CPU_level)" : "");
    } else if (va >= C1A_INJ_RULES && va < C1A_INJ_RULES + 2 * C1A_INJ_RULESTR) {
        int s = (int)((va - C1A_INJ_RULES) / C1A_INJ_RULESTR); unsigned off = (unsigned)((va - C1A_INJ_RULES) % C1A_INJ_RULESTR);
        snprintf(out, cap, "rule_player%d+0x%x%s", s, off, off == C1A_INJ_PERS ? "(CpuPersonality)" : "");
    } else if (va >= C1A_INJ_GAME + 0x30 && va < C1A_INJ_GAME + 0x30 + 2 * C1A_INJ_RULESTR) {
        int s = (int)((va - C1A_INJ_GAME - 0x30) / C1A_INJ_RULESTR);
        snprintf(out, cap, "setup_copy+0x%x(player%d)%s", (unsigned)(va - C1A_INJ_GAME), s, va == C1A_INJ_GAME + C1A_INJ_PCOPY + (uint64_t)s * C1A_INJ_RULESTR ? "(CpuPersonality)" : "");
    } else snprintf(out, cap, "0x%llx", (unsigned long long)va);
}
static void span_hex(const C1aView *v, uint64_t va, uint32_t len, char *out) {   /* up to 16 bytes, out >= 33 */
    static const char d[] = "0123456789abcdef"; uint32_t n = len < 16 ? len : 16;
    for (uint32_t i = 0; i < n; i++) { uint8_t b = v->b[va - v->lo + i]; out[2 * i] = d[b >> 4]; out[2 * i + 1] = d[b & 15]; }
    out[2 * n] = 0;
}
static void log_spans(const C1aPlan *p, const C1aView rec[C1A_NRANGE], const C1aView now[C1A_NRANGE], int bad, const char *tag, int budget) {
    int lines = 0, more = 0;
    for (int i = 0; i < p->nspan; i++) {
        const C1aSpan *s = &p->span[i];
        if (s->bad != bad) continue;
        if (lines >= budget) { more++; continue; }
        lines++;
        char nm[64], a[40], b[40]; span_name(s->va, nm, sizeof nm); span_hex(&rec[s->range], s->va, s->len, a); span_hex(&now[s->range], s->va, s->len, b);
        LOG("C1A %s battle=%llu range=%s va=0x%llx %s len=%u rec=%s now=%s%s\n", tag, (unsigned long long)g_bat, k_c1a_range[s->range].name, (unsigned long long)s->va, nm,
            s->len, a, b, s->ptr ? " (a pointer unit: never written)" : "");
    }
    if (more || p->nspan >= C1A_INJ_MAXSPAN) LOG("C1A %s battle=%llu ... %d more span(s) not listed (budget %d; plan spans stored %d of cap %d)\n", tag, (unsigned long long)g_bat, more, budget, p->nspan, C1A_INJ_MAXSPAN);
}
/* C3: the wind manager at the battle start, this launch vs the recording's f0 dump (and s_wind when the recording has it) */
static void wind_compare(void) {
    char a[256] = "n/a", b[256], why[200] = "-"; int eq = -1;
    c1a_wind_str(&g_wind_now, b, sizeof b);
    if (g_rf_has_wview) { C1aWind rw; c1a_wind_view(&g_rf_wview, &rw); c1a_wind_str(&rw, a, sizeof a); eq = c1a_wind_equal(&rw, &g_wind_now, why, sizeof why); }
    LOG("C1A WIND battle=%llu rec=%s now=%s behavior_equal=%s differs=%s now_list=%d now_nodes=%016llx s_wind=%s (information, C1A-AUDIT D5/C3: the slot index only "
        "selects the physical queue array; the output vectors are recomputed by every wind update)\n", (unsigned long long)g_bat, a, b,
        eq < 0 ? "n/a" : eq ? "1" : "0", why, g_wind_list, (unsigned long long)g_wind_nodes,
        !g_rf_has_wind ? "absent_in_the_recording" : g_rf_s[C1A_S_WIND] == g_s[C1A_S_WIND] ? "equal" : "DIFFERS");
}
static uint32_t setup_differs(void) {
    uint32_t m = 0;
    for (int i = 0; i < C1A_NSETUP; i++) {
        if (i == C1A_S_CONTENT) continue;                                                /* C1B3: the hard gate (CONTENTCMP), not a hash category */
        if (i == C1A_S_WIND && !g_rf_has_wind) continue;                                 /* a v0.22.0 recording: no s_wind */
        if (i == C1A_S_PLUGIN && g_rf_compat && g_s[i] == c1a_plugin_hash(SC6RB_VERSION)) continue;   /* PLUGIN_COMPAT */
        if (g_s[i] != g_rf_s[i]) m |= 1u << i;
    }
    return m;
}
static void log_mismatch(uint32_t bad, uint32_t info, const char *why) {
    char bs[200], is[200]; c1a_refuse_str(bad, bs, sizeof bs); c1a_refuse_str(info, is, sizeof is);
    char d[1400]; size_t o = 0; d[0] = 0;
    for (int i = 0; i < C1A_NSETUP; i++) if (bad & (1u << i)) o += (size_t)snprintf(d + o, sizeof d - o, " %s:rec=%016llx,now=%016llx", k_c1a_setup[i], (unsigned long long)g_rf_s[i], (unsigned long long)g_s[i]);
    LOG("C1A SETUP_MISMATCH battle=%llu fields=%s info_differs=%s%s why=%s (no injection this battle; title play=SETUP_MISMATCH)\n", (unsigned long long)g_bat, bs, info ? is : "-", d, why);
}
/* C1B3: (C1B-R3-SPEC.md §2.1.2) the content verdict: 0 equal, 1 REFUSE, 2 unknown (no reference: refuse, content_unknown) */
static int content_check(void) {
    char r0[48], r1[48], n0[48], n1[48];
    int keys = g_rf_ct_src == 1;
    for (int p = 0; p < 2; p++) if (!g_rf_ct[p].has_keys || !g_ct[p].ok) keys = 0;
    ctree_str(&g_rf_ct[0], g_rf_ct_src == 1 && g_rf_ct[0].has_keys, r0, sizeof r0); ctree_str(&g_rf_ct[1], g_rf_ct_src == 1 && g_rf_ct[1].has_keys, r1, sizeof r1);
    ctree_str(&g_ct[0], g_ct[0].ok, n0, sizeof n0); ctree_str(&g_ct[1], g_ct[1].ok, n1, sizeof n1);
    int v;
    if (!g_rf_ct_src) v = 2;
    else {
        v = 0;
        for (int p = 0; p < 2; p++) {
            if (g_rf_ct[p].n != g_ct[p].n) v = 1;
            if (keys && g_rf_ct[p].keys != g_ct[p].keys) v = 1;
        }
    }
    LOG("C1A CONTENTCMP rec_src=%s rec=%s,%s now=%s,%s verdict=%s cmp=%s battle=%llu%s\n", g_rf_ct_src == 1 ? "header" : g_rf_ct_src == 2 ? "f0dump" : "none", g_rf_ct_src ? r0 : "-",
        g_rf_ct_src ? r1 : "-", n0, n1, v == 0 ? "equal" : v == 1 ? "REFUSE" : "unknown", keys ? "sizes_and_keys" : "sizes_only", (unsigned long long)g_bat,
        v == 1 ? " (the installed move-record trees are not the recording's: the reference is invalid; no injection; title play=SETUP_MISMATCH(content))"
               : v == 2 ? " (no reference: the recording has no tree0= and no readable f0 dump; refused: content_unknown)"
               : !keys ? " (blind spot: equal sizes with different keys are not seen)" : "");
    return v;
}
static void modset_check(void) {
    int v = !g_rf_ms_has ? 2 : g_rf_ms == g_ms_hash ? 0 : 1;
    g_ms_diff = v == 1;
    char rh[24]; if (g_rf_ms_has) snprintf(rh, sizeof rh, "%016llx", (unsigned long long)g_rf_ms); else snprintf(rh, sizeof rh, "absent");
    LOG("C1A MODSETCMP rec=%s now=%016llx verdict=%s battle=%llu%s\n", rh, (unsigned long long)g_ms_hash, v == 0 ? "equal" : v == 1 ? "DIFF" : "absent", (unsigned long long)g_bat,
        v == 1 ? " (the mod-set listing differs from the recording's: information, not a refusal; title MODSET_DIFF; ckcmp FAILs it unless every differing path is allowed)" : "");
    if (g_ms_diff) g_title_dirty = 1;
}
/* v0.22.1 (C1A-AUDIT C1/C2): the setup check with the personality injection; called with g_s computed and g_play == PL_ARMED */
static void setup_check_v24(void);
static void setup_check(void) {
    g_play_why = 0;
    modset_check();
    /* C1B3 fix (round-3 review): the T2RULE verdict again, per battle with battle=, as CONTENTCMP and MODSETCMP: the readers judge item 2 per battle,
     * and the load-time line (rf_load) comes before any BATTLE init and has no battle= */
    LOG("C1A T2RULE rec=%s now=%s verdict=%s battle=%llu%s\n", t2rule_name(g_rf_t2rule), t2rule_name(g_cfg.c1a_t2_heap_tag), g_rf_t2rule == g_cfg.c1a_t2_heap_tag ? "equal" : "REFUSE",
        (unsigned long long)g_bat, g_rf_t2rule == g_cfg.c1a_t2_heap_tag ? "" : " (the setup check refuses: title play=SETUP_MISMATCH(t2rule))");
    if (g_rf_t2rule != g_cfg.c1a_t2_heap_tag) {   /* C1B3: (C1B-R3-SPEC.md §2.6.2) an explicit c1a_t2_heap that is not the recording's rule */
        g_play = PL_MISMATCH; g_play_why = 2;
        LOG("C1A SETUP_MISMATCH battle=%llu fields=t2rule info_differs=- t2rule:rec=%s,now=%s why=t2rule (no injection this battle; title play=SETUP_MISMATCH(t2rule))\n",
            (unsigned long long)g_bat, t2rule_name(g_rf_t2rule), t2rule_name(g_cfg.c1a_t2_heap_tag));
        return;
    }
    int cv = content_check();
    if (cv && RBT_T1(g_cfg.h_content_gate_off)) LOG("C1A CONTENT_GATE_OFF battle=%llu (harness-only control h_content_gate_off=1: the v0.24 behavior, the content difference is not refused)\n", (unsigned long long)g_bat);
    if (cv && !RBT_T1(g_cfg.h_content_gate_off)) {   /* C1B3: the hard gate, whatever c1a_refuse says */
        uint32_t dif = setup_differs(), bad = (dif & g_cfg.c1a_refuse) | (1u << C1A_S_CONTENT), info = dif & ~g_cfg.c1a_refuse;
        g_play = PL_MISMATCH; g_play_why = 1;
        log_mismatch(bad, info, cv == 1 ? "content" : "content_unknown");
        return;
    }
    setup_check_v24();
}
static void setup_check_v24(void) {
    uint32_t dif = setup_differs(), bad = dif & g_cfg.c1a_refuse, info = dif & ~g_cfg.c1a_refuse;
    char is[200]; c1a_refuse_str(info, is, sizeof is);
    if (g_rf_compat) LOG("C1A PLUGIN_COMPAT battle=%llu rec=%s now=%s (the recording's build; this build keeps its step path, normalization and checksums: the plugin "
                         "category is compared as equal)\n", (unsigned long long)g_bat, g_rf_compat, SC6RB_VERSION);
    wind_compare();
    if (!g_cfg.c1a_inject) {   /* the v0.22.0 behavior */
        if (bad) { g_play = PL_MISMATCH; log_mismatch(bad, info, "hash"); return; }
        g_play = PL_OK;
        LOG("C1A SETUP_OK battle=%llu setup=%016llx info_differs=%s injected=0 (injecting the recorded input of %u frames)\n", (unsigned long long)g_bat,
            (unsigned long long)setup_total(g_s), info ? is : "-", g_rf_n);
        return;
    }
    if (!g_rf_dump_ok) {
        g_play = PL_NODUMP;
        LOG("C1A NO_SETUP_DUMP battle=%llu path=%s why=%s (c1a_setup_inject needs the recording's battle-start dump; nothing injected this battle; "
            "c1a_setup_inject=none replays without it; title play=NO_SETUP_DUMP)\n", (unsigned long long)g_bat, g_rf_dump_path[0] ? g_rf_dump_path : "-", g_rf_dump_why);
        return;
    }
    static C1aView now[C1A_NRANGE];
    static C1aPlan plan;
    if (!live_views(now)) { g_play = PL_MISMATCH; log_mismatch(bad, info, "out_of_memory"); return; }
    c1a_inj_plan(g_rf_view, now, g_cfg.c1a_inject, &plan);
    for (int s = 0; s < 2; s++) { g_pers_rec[s] = plan.pers_rec[s][0]; g_pers_ue[s] = plan.pers_now[s][0]; }
    LOG("C1A SETUP_PLAN battle=%llu verdict=%s why=%s diff_bytes=%u write_bytes=%u/%u/%u bad_bytes=%u pointer_bytes=%u personality rec=%d,%d ue=%d,%d copies rec=%d,%d,%d/%d,%d,%d "
        "ue=%d,%d,%d/%d,%d,%d consistent rec=%d ue=%d hash_differs=%s\n", (unsigned long long)g_bat,
        plan.verdict == C1A_INJ_OK ? "inject" : plan.verdict == C1A_INJ_EQUAL ? "equal" : "REFUSE", plan.why, plan.diff_bytes, plan.wr_bytes[0], plan.wr_bytes[1],
        plan.wr_bytes[2], plan.bad_bytes, plan.ptr_bytes, plan.pers_rec[0][0], plan.pers_rec[1][0], plan.pers_now[0][0], plan.pers_now[1][0],
        plan.pers_rec[0][0], plan.pers_rec[0][1], plan.pers_rec[0][2], plan.pers_rec[1][0], plan.pers_rec[1][1], plan.pers_rec[1][2],
        plan.pers_now[0][0], plan.pers_now[0][1], plan.pers_now[0][2], plan.pers_now[1][0], plan.pers_now[1][1], plan.pers_now[1][2], plan.cons_rec, plan.cons_now,
        dif ? (c1a_refuse_str(dif, is, sizeof is), is) : "-");
    /* v0.22.2: the CPU difficulty per side: rule rank/waza, copy rank/waza, AI +0x2bf8/+0x2c00/+0x1a28 */
    LOG("C1A SETUP_LEVELS battle=%llu rec0=%d,%d/%d,%d/%d,%d,%d rec1=%d,%d/%d,%d/%d,%d,%d ue0=%d,%d/%d,%d/%d,%d,%d ue1=%d,%d/%d,%d/%d,%d,%d consistent rec=%d ue=%d "
        "(CpuRankLevel,CpuWazaLevel: rule / copy / AI +0x2bf8,+0x2c00,+0x1a28)\n", (unsigned long long)g_bat,
        plan.lvl_rec[0][0], plan.lvl_rec[0][1], plan.lvl_rec[0][2], plan.lvl_rec[0][3], plan.lvl_rec[0][4], plan.lvl_rec[0][5], plan.lvl_rec[0][6],
        plan.lvl_rec[1][0], plan.lvl_rec[1][1], plan.lvl_rec[1][2], plan.lvl_rec[1][3], plan.lvl_rec[1][4], plan.lvl_rec[1][5], plan.lvl_rec[1][6],
        plan.lvl_now[0][0], plan.lvl_now[0][1], plan.lvl_now[0][2], plan.lvl_now[0][3], plan.lvl_now[0][4], plan.lvl_now[0][5], plan.lvl_now[0][6],
        plan.lvl_now[1][0], plan.lvl_now[1][1], plan.lvl_now[1][2], plan.lvl_now[1][3], plan.lvl_now[1][4], plan.lvl_now[1][5], plan.lvl_now[1][6],
        plan.lvl_cons_rec, plan.lvl_cons_now);
    uint32_t noninj = bad & ~g_cfg.c1a_inject;
    if (noninj) {   /* a refusing category the injection does not cover differs */
        log_spans(&plan, g_rf_view, now, 1, "SETUP_DIFF", 40);
        g_play = PL_MISMATCH; log_mismatch(bad, info, "not_injectable_category"); return;
    }
    if (plan.verdict == C1A_INJ_REFUSE) {
        log_spans(&plan, g_rf_view, now, 1, "SETUP_DIFF", 40);
        g_play = PL_MISMATCH; log_mismatch(bad, info, plan.why); return;
    }
    if (plan.verdict == C1A_INJ_EQUAL) {
        if (bad) { g_play = PL_MISMATCH; log_mismatch(bad, info, "hash_differs_bytes_equal"); return; }
        g_play = PL_OK;
        c1a_refuse_str(info, is, sizeof is);
        LOG("C1A SETUP_OK battle=%llu setup=%016llx info_differs=%s injected=0 (nothing to inject; injecting the recorded input of %u frames)\n", (unsigned long long)g_bat,
            (unsigned long long)setup_total(g_s), info ? is : "-", g_rf_n);
        return;
    }
    /* C2: only the CPU personality differs: name it (a pass is never "UE gave the same setup"), write the recording's bytes, re-check */
    memcpy(g_pre_s, g_s, sizeof g_s);
    for (int s = 0; s < 2; s++) {
        if (plan.side_ovr[s] && plan.pers_rec[s][0] != plan.pers_now[s][0])
            LOG("C1A SETUP_OVERRIDE battle=%llu field=CpuPersonality side=%d ue=%d rec=%d copies=rule_block+0x10,setup_copy+0x%x,AI+0x2c0c tables=AI+0x38..+0x987 "
                "(FLuxBattlePlayerParam.CpuPersonality: a UE input; the recorded value and its derived bytes are written; NOT evidence of equal setups, C1A-AUDIT C6)\n",
                (unsigned long long)g_bat, s, plan.pers_now[s][0], plan.pers_rec[s][0], (unsigned)(C1A_INJ_PCOPY + (uint64_t)s * C1A_INJ_RULESTR));
        if (plan.side_ovr[s] && (plan.lvl_rec[s][0] != plan.lvl_now[s][0] || plan.lvl_rec[s][1] != plan.lvl_now[s][1]))   /* v0.22.2 */
            LOG("C1A SETUP_OVERRIDE battle=%llu field=CpuLevel side=%d ue=rank%d,waza%d rec=rank%d,waza%d copies=rule_block+0x8/+0xc,setup_copy+0x%x/+0x%x,"
                "AI+0x2bf8/+0x2c00/+0x1a28 (FLuxBattlePlayerParam.CpuRankLevel/CpuWazaLevel: a UE input; the recorded values are written; NOT evidence of equal setups)\n",
                (unsigned long long)g_bat, s, plan.lvl_now[s][0], plan.lvl_now[s][1], plan.lvl_rec[s][0], plan.lvl_rec[s][1],
                (unsigned)(C1A_INJ_PCOPY - 8 + (uint64_t)s * C1A_INJ_RULESTR), (unsigned)(C1A_INJ_PCOPY - 4 + (uint64_t)s * C1A_INJ_RULESTR));
    }
    log_spans(&plan, g_rf_view, now, 0, "SETUP_WRITE", 24);
    uint8_t *dst[C1A_NRANGE];
    for (int r = 0; r < C1A_NRANGE; r++) dst[r] = (uint8_t *)VA(k_c1a_range[r].lo);
    uint32_t wrote = c1a_inj_apply(g_rf_view, now, &plan, dst);
    uint32_t want = plan.wr_bytes[0] + plan.wr_bytes[1] + plan.wr_bytes[2];
    setup_compute();   /* the hashes again, on the injected memory */
    static C1aView now2[C1A_NRANGE];
    static C1aPlan p2;
    int lv = live_views(now2);
    if (lv) c1a_inj_plan(g_rf_view, now2, g_cfg.c1a_inject, &p2);
    uint32_t dif2 = setup_differs(), bad2 = dif2 & g_cfg.c1a_refuse, info2 = dif2 & ~g_cfg.c1a_refuse;
    int ok = lv && wrote == want && p2.verdict == C1A_INJ_EQUAL && !bad2;
    g_inj_cat_bytes[C1A_S_RULES] = plan.wr_bytes[C1A_R_RULES] + plan.wr_bytes[C1A_R_COPY];
    g_inj_cat_bytes[C1A_S_AI] = plan.wr_bytes[C1A_R_AI];
    LOG("C1A SETUP_INJECT battle=%llu cat=rules bytes=%u ranges=%u ok=%d hash_before=%016llx after=%016llx rec=%016llx (rule block %u B in %u span(s), Setup copy %u B in %u span(s))\n",
        (unsigned long long)g_bat, g_inj_cat_bytes[C1A_S_RULES], plan.wr_spans[C1A_R_RULES] + plan.wr_spans[C1A_R_COPY], ok, (unsigned long long)g_pre_s[C1A_S_RULES],
        (unsigned long long)g_s[C1A_S_RULES], (unsigned long long)g_rf_s[C1A_S_RULES], plan.wr_bytes[C1A_R_RULES], plan.wr_spans[C1A_R_RULES], plan.wr_bytes[C1A_R_COPY], plan.wr_spans[C1A_R_COPY]);
    LOG("C1A SETUP_INJECT battle=%llu cat=ai bytes=%u ranges=%u ok=%d hash_before=%016llx after=%016llx rec=%016llx\n", (unsigned long long)g_bat, g_inj_cat_bytes[C1A_S_AI],
        plan.wr_spans[C1A_R_AI], ok, (unsigned long long)g_pre_s[C1A_S_AI], (unsigned long long)g_s[C1A_S_AI], (unsigned long long)g_rf_s[C1A_S_AI]);
    if (!ok) {
        if (lv) log_spans(&p2, g_rf_view, now2, 1, "SETUP_DIFF", 20);
        g_play = PL_MISMATCH;
        log_mismatch(bad2, info2, lv && wrote == want && p2.verdict != C1A_INJ_EQUAL ? "inject_verify_bytes" : lv && wrote == want ? "inject_verify_hash" : "inject_write");
        return;
    }
    g_injected = 1; g_play = PL_OK;
    c1a_refuse_str(info2, is, sizeof is);
    LOG("C1A SETUP_OK battle=%llu setup=%016llx info_differs=%s injected=1 (setup equal after the CpuPersonality injection; injecting the recorded input of %u frames)\n",
        (unsigned long long)g_bat, (unsigned long long)setup_total(g_s), info2 ? is : "-", g_rf_n);
}

static void first_step(void) {
    win_size();
    if (g_cm == 2 && g_play == PL_ARMED && g_rf_conv) {   /* C2 (C2-SPEC.md §3.2): an rbnet recording: the same conversion before the setup check and step 1 */
        uint32_t r1 = *(volatile uint32_t *)VA(0x14470f22cULL), c1 = *(volatile uint32_t *)VA(0x144843f7cULL);
        if ((r1 == 2 || r1 == 1) && (c1 == 2 || c1 == 1)) { *(volatile uint32_t *)VA(0x14470f22cULL) = 1; *(volatile uint32_t *)VA(0x144843f7cULL) = 1;
            LOG("C1A CONV battle=%llu side=1 rule=%u->1 copy=%u->1 (the recording is an rbnet battle: conv=1)\n", (unsigned long long)g_bat, r1, c1); }
        else { g_play = PL_MISMATCH; LOG("C1A SETUP_MISMATCH(conv) battle=%llu rule=%u copy=%u (conv=1 and no conversion possible; nothing injected)\n", (unsigned long long)g_bat, r1, c1); g_title_dirty = 1; }
    }
    setup_compute();
    if (g_play == PL_ARMED) { setup_check(); g_title_dirty = 1; }
    g_uecam = g_s[C1A_S_UECAM];
    LOG("C1A SETUP battle=%llu setup=%016llx exe=%016llx plugin=%016llx simcfg=%016llx rules=%016llx flags=%016llx pl0=%016llx pl1=%016llx ctl=%016llx ai=%016llx stage=%016llx "
        "assets=%016llx catch=%016llx uecam=%016llx ctl_type=%u,%u ai_loaded=%u,%u cpu_level=%u,%u stage_slot=%d stage_entries=%d win=%dx%d arena_base=0x%llx arena_fixed=%d module_base=0x%llx\n",
        (unsigned long long)g_bat, (unsigned long long)setup_total(g_s), (unsigned long long)g_s[0], (unsigned long long)g_s[1], (unsigned long long)g_s[2],
        (unsigned long long)g_s[3], (unsigned long long)g_s[4], (unsigned long long)g_s[5], (unsigned long long)g_s[6], (unsigned long long)g_s[7], (unsigned long long)g_s[8],
        (unsigned long long)g_s[9], (unsigned long long)g_s[10], (unsigned long long)g_s[11], (unsigned long long)g_s[12], g_ctl_type[0], g_ctl_type[1], g_ai_loaded[0], g_ai_loaded[1],
        g_cpu_level[0], g_cpu_level[1], g_stage_slot, g_stage_entries, g_win_w, g_win_h, (unsigned long long)(uintptr_t)g_arena.base, g_arena_fixed, (unsigned long long)g_base);
    {   /* v0.22.1: wind (C3), the game-thread CRT calls (C5), UE's +0xc8 calls since this Init (C3); information only */
        char ws[256]; c1a_wind_str(&g_wind_now, ws, sizeof ws);
        LOG("C1A SETUP2 battle=%llu wind=%016llx wind_state=%s wind_list=%d wind_nodes=%016llx crt_gt_since_demo_init=%ld/%ld crt_gt_since_this_init=%ld/%ld "
            "crt_other_since_demo_init=%ld c8_since_init=%u c8_moved_tt=%u injected=%d (rand/srand counts on the game thread that did not take the LCG; C1A-AUDIT C5)\n",
            (unsigned long long)g_bat, (unsigned long long)g_s[C1A_S_WIND], ws, g_wind_list, (unsigned long long)g_wind_nodes,
            g_demo_seen ? g_crt_gt[0] - g_crt_demo[0] : -1L, g_demo_seen ? g_crt_gt[1] - g_crt_demo[1] : -1L, g_crt_gt[0] - g_crt_bb[0], g_crt_gt[1] - g_crt_bb[1],
            g_demo_seen ? (long)(g_crt_other - g_crt_demo_other) : -1L, g_c8_pre, g_c8_pre_moved, g_injected);
    }
    for (int p = 0; p < 2; p++) {   /* C1B3: (C1B-R3-SPEC.md §2.1.1) */
        char ks[24]; if (g_ct[p].ok) snprintf(ks, sizeof ks, "%016llx", (unsigned long long)g_ct[p].keys); else snprintf(ks, sizeof ks, "-");
        LOG("C1A CONTENT p=%d size=%llu keys=%s walk=%s visited=%u battle=%llu%s\n", p, (unsigned long long)g_ct[p].n, ks, g_ct[p].ok ? "ok" : "bad", g_ct[p].visited,
            (unsigned long long)g_bat, g_ct[p].head_null ? " head=0 (no tree object, size 0)" : !g_ct[p].ok ? " (the tree walk stopped: a node outside every tracked/pinned block, a bad link or a size mismatch; "
            "the comparison uses sizes only; title CONTENT_WALK)" : "");
    }
    if (g_ct_walkbad) g_title_dirty = 1;
    t1map_check();   /* v0.24.0-C1b2 F1.6 */
    if (g_skip_bat) { LOG("C1A REC skip battle=%llu why=demo_battle (rblive: the title/menu demo battle, UE seed %u, is not recorded)\n", (unsigned long long)g_bat, C1A_DEMO_SEED); pers_read_live(g_pers_watch); g_pers_changes = g_pers_lines = 0; return; }
    write_header();
    LOG("C1A UECAM battle=%llu frame=0 hash=%016llx (UE-fed camera inputs before the first step: tables 0x14470def0..0x14470e0bf, matrix 0x14470e180..0x14470e1bf, director 0x14470ed44..0x14470ed4b)\n",
        (unsigned long long)g_bat, (unsigned long long)g_uecam);
    wr_printf(&g_w, "U 0 uecam=%016llx\n", (unsigned long long)g_uecam);
    /* checker fix: a dump of the battle-start state (before the first step runs: the state the setup hash covers), so a
     * SETUP_MISMATCH or a frame-1 tier-1 difference can be named field by field (ckcmp --dumps) without another session */
    if (g_cfg.c1a_dump_every > 0) dump_write(0);
    pers_read_live(g_pers_watch); g_pers_changes = g_pers_lines = 0;
}

int c1a_step_begin(uint64_t seq, const C1aIn *ue, C1aIn *use, int passthrough) {
    (void)seq;
    *use = *ue;
    if (!g_cm) return 0;
    uint32_t f = ++g_f;
    memset(&g_cur, 0, sizeof g_cur);
    g_cur.f = f; g_cur.pt = (uint8_t)(passthrough != 0);
    ifc_collect(g_cur.ifc, &g_cur.q);
    if (f == 1) first_step();
    int inj = 0;
    if (g_play == PL_OK) {
        if (f <= g_rf_n) { *use = g_rf_fr[f - 1].in; inj = 1; }
        else play_end("rec_end");
    }
    g_cur.in = *use;
    return inj;
}

/* v0.24.0-C1b2 F1.4 (C1B-R2-SPEC.md §2.2): the tier-1 verdict of a frame against the recording.
 *   this run heaptag (control): equal or MISMATCH, as v0.23.
 *   this run ptrmap: the map rule equals the recording -> equal; T1NORM when the two rules normalize the frame differently (the v0.23 rule
 *   would have differed: the launch-1 artifact, rescued); the map rule differs but the recording has no t1rule (a v0.23 heaptag recording)
 *   and the v0.23 rule equals it -> T1NORM; else MISMATCH. T1NORM is counted (t1norm=), never a mismatch.
 * v0.24.0-C1b2 fix (review: "T1NORM has two meanings"): each branch carries its own token. why=rescue: the map rule equals the recording
 *   and only this process's v0.23 rule normalizes the frame differently (any unit may be named, typically the unmapped 2d18 of a heap slot
 *   that matches an AI integer). why=oldrule: the map rule differs from the recording and only the v0.23 rule equals it (a unit outside the
 *   map there can hide a real divergence: the readers FAIL it). Counted apart as t1norm_rescue= / t1norm_oldrule=; t1norm= is their sum. */
enum { V_EQ = 0, V_NORM = 1, V_MM = 2, V_NORM_OLD = 3 };   /* V_NORM = why=rescue, V_NORM_OLD = why=oldrule */
static int t1_disagree(const C1aFrame *now, const C1aT1Alt *alt) { return alt && (alt->c_ctl != now->c[8] || alt->c_ai != now->c[9]); }
static int t1_verdict(const C1aFrame *now, const C1aT1Alt *alt, const C1aFrame *r, uint32_t *maskp) {
    uint32_t mask = 0;
    for (int i = 0; i < C1A_NCOMP; i++) if (r->c[i] != now->c[i]) mask |= 1u << i;
    if (maskp) *maskp = mask;
    int eq = !mask && r->t1 == now->t1;
    if (g_cfg.c1a_t1_rule == 1) return eq ? V_EQ : V_MM;
    int dis = t1_disagree(now, alt);
    if (eq) return dis ? V_NORM : V_EQ;
    if (g_rf_t1rule != 1 && dis) {
        int eq2 = alt->t1 == r->t1;
        for (int i = 0; eq2 && i < C1A_NCOMP; i++) { uint64_t v = i == 8 ? alt->c_ctl : i == 9 ? alt->c_ai : now->c[i]; if (v != r->c[i]) eq2 = 0; }
        if (eq2) return V_NORM_OLD;
    }
    return V_MM;
}
static void t1_units_str(const C1aT1Alt *alt, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; alt && i < alt->nu; i++) { int k = snprintf(out + o, cap - o, "%s%x:%u/%u", o ? "," : "", alt->uoff[i], alt->ukm[i], alt->uko[i]); if (k > 0 && (size_t)k < cap - o) o += (size_t)k; }
    if (alt && alt->ndis > (uint32_t)alt->nu) { int k = snprintf(out + o, cap - o, ",+%u", alt->ndis - (uint32_t)alt->nu); if (k > 0 && (size_t)k < cap - o) o += (size_t)k; }
    if (!o) snprintf(out, cap, "-");
}
static void cmp_frame(const C1aPass *ps) {
    const C1aFrame *now = &ps->fr;
    const C1aFrame *r = &g_rf_fr[now->f - 1];
    uint32_t mask = 0;
    int vd = t1_verdict(now, &ps->alt, r, &mask);
    char us[200]; t1_units_str(&ps->alt, us, sizeof us);
    if (vd == V_NORM || vd == V_NORM_OLD) {
        g_t1norm++;
        if (vd == V_NORM) g_t1norm_rescue++; else g_t1norm_old++;
        if (g_t1norm_lines < 40) {
            g_t1norm_lines++;
            int dc = ps->alt.c_ctl != now->c[8], da = ps->alt.c_ai != now->c[9];
            LOG("C1A T1NORM f=%u comp=%s units=%s pass=%c battle=%llu why=%s (%s)\n", now->f, dc && da ? "ctl,ai" : dc ? "ctl" : "ai", us, ps->ps ? ps->ps : '-',
                (unsigned long long)g_bat, vd == V_NORM ? "rescue" : "oldrule",
                vd == V_NORM ? "the map rule equals the recording; only this process's v0.23 rule normalizes the frame differently: equal, not a mismatch"
                             : "the map rule differs from the recording and only the v0.23 rule equals it: not a mismatch only when every named unit is a mapped pointer unit");
        }
    }
    if (vd == V_MM && (mask & ~(1u << 8)) && g_vm_trace_on) {   /* v0.32.0 finish review fix F4 (src/vm.c vm_t1_diff): a component other than ctl (ctl differs on
                                                                  * every frame of a replay of a record-mode recording) opens the trace's difference window */
        char vc[400]; size_t vo = 0; vc[0] = 0;
        for (int i = 0; i < C1A_NCOMP; i++) if (mask & (1u << i)) { int k = snprintf(vc + vo, sizeof vc - vo, "%s%s", vo ? "," : "", k_c1a_comp[i]); if (k > 0 && (size_t)k < sizeof vc - vo) vo += (size_t)k; }
        vm_t1_diff(now->f, vc);
    }
    if (vd == V_MM) {
        g_t1mm++; if (!g_t1first) { g_t1first = now->f; g_title_dirty = 1; }
        if (g_t1_lines < BUDGET) {
            g_t1_lines++;
            char cs[400]; size_t o = 0; cs[0] = 0;
            for (int i = 0; i < C1A_NCOMP; i++) if (mask & (1u << i)) o += (size_t)snprintf(cs + o, sizeof cs - o, "%s%s", o ? "," : "", k_c1a_comp[i]);
            LOG("C1A MISMATCH frame=%u tier=1 comps=%s battle=%llu fr=%u rec_fr=%u t1=%016llx rec_t1=%016llx units=%s\n", now->f, o ? cs : "t1", (unsigned long long)g_bat, now->fr, r->fr,
                (unsigned long long)now->t1, (unsigned long long)r->t1, us);
        }
    }
    if (strcmp(r->ifc, now->ifc)) {
        g_ifcd++;
        if (g_ifc_lines < BUDGET) { g_ifc_lines++; LOG("C1A IFCDIFF frame=%u battle=%llu rec=%s now=%s\n", now->f, (unsigned long long)g_bat, r->ifc, now->ifc); }
    }
    if (r->rp != now->rp) g_rfd++;
    if (r->rp != g_prev_rec_rp || now->rp != g_prev_now_rp) {
        if (g_rfl_lines < BUDGET) { g_rfl_lines++; LOG("C1A RFLAG frame=%u battle=%llu now=%u rec=%u%s\n", now->f, (unsigned long long)g_bat, now->rp, r->rp, r->rp == now->rp ? "" : " DIFFERS"); }
        g_prev_rec_rp = r->rp; g_prev_now_rp = now->rp;
    }
}
static void cmp_t2(uint32_t f, const C1aCell *c, int n, int has_dump) {
    while (g_t2_cursor < g_rf_nt2 && g_rf_t2[g_t2_cursor].f < f) g_t2_cursor++;
    if (g_t2_cursor >= g_rf_nt2 || g_rf_t2[g_t2_cursor].f != f) return;
    const T2Rec *r = &g_rf_t2[g_t2_cursor];
    g_t2_compared++;
    char cs[1200]; size_t o = 0; cs[0] = 0; int nd = 0;
    for (int i = 0, j = 0; i < n || j < r->n;) {   /* both sorted by key */
        uint32_t ki = i < n ? c[i].key : 0xffffffffu, kj = j < r->n ? r->c[j].key : 0xffffffffu;
        int differ = 0; uint32_t key;
        if (ki == kj) { differ = c[i].h != r->c[j].h; key = ki; i++; j++; }
        else if (ki < kj) { differ = 1; key = ki; i++; }
        else { differ = 1; key = kj; j++; }
        if (differ) { nd++; char nm[16]; c1a_cell_name(key, nm); if (o + 20 < sizeof cs) o += (size_t)snprintf(cs + o, sizeof cs - o, "%s%s", o ? "," : "", nm); }
        /* C1B4: a cell that differs for the first time in this battle (mode=replay, budget c1a_t2mm_dumps): remembered for the dump below */
        if (differ && g_cfg.c1a_t2mm_dumps > 0 && g_confirm_now && g_mode == MODE_REPLAY) {
            int seen = 0; for (int q = 0; q < g_t2mm_ncell; q++) seen |= g_t2mm_cell[q] == key;
            if (!seen && g_t2mm_ncell < C1A_MAXCELL) { g_t2mm_cell[g_t2mm_ncell++] = key; g_t2mm_new[g_t2mm_nnew < 8 ? g_t2mm_nnew++ : 7] = key; }
        }
    }
    if (g_t2mm_nnew) {   /* C1B4: dump this step's state once (a regular / dump_at dump of the same frame follows in c1a_confirm: no second file) */
        if (!has_dump && g_t2mm_n < g_cfg.c1a_t2mm_dumps) {
            DumpBuf *d = &g_db[C1A_DUMP_SLOTS]; dump_build(f, d); d->why = 2;
            size_t q = 0; d->cells[0] = 0;
            for (int i = 0; i < g_t2mm_nnew && q + 12 < sizeof d->cells; i++) { char nm[16]; c1a_cell_name(g_t2mm_new[i], nm); int k = snprintf(d->cells + q, sizeof d->cells - q, "%s%s", i ? "," : "", nm); if (k > 0) q += (size_t)k; }
            dump_flush(d); g_t2mm_n++;
        } else if (has_dump) LOG("C1A T2MMDUMP frame=%u battle=%llu covered_by=the_frame_dump (a first tier-2 mismatch on a frame that is dumped anyway)\n", f, (unsigned long long)g_bat);
        else if (g_t2mm_n == g_cfg.c1a_t2mm_dumps) { g_t2mm_n++; LOG("C1A T2MMDUMP frame=%u battle=%llu budget_spent=%d (no more first-mismatch dumps this battle)\n", f, (unsigned long long)g_bat, g_cfg.c1a_t2mm_dumps); }
        g_t2mm_nnew = 0;
    }
    if (nd) {
        g_t2mm++; if (!g_t2first) g_t2first = f;
        if (g_t2_lines < BUDGET) { g_t2_lines++; LOG("C1A MISMATCH frame=%u tier=2 cells=%s n=%d battle=%llu\n", f, cs, nd, (unsigned long long)g_bat); }
    }
}

/* ------------------------------------------------------------------ v0.23.0-C1b: per pass / per confirmed frame
 * c1a_pass_compute = everything C1a computed after the step from live memory (t1 + its components, the replay flag, t2 every 32 frames,
 * the dump capture every c1a_dump_every frames); c1a_confirm = everything C1a wrote / compared for the frame. mode=record / replay call
 * both right after their one pass (c1a_step_end: the same lines, in the same order, as v0.22.2); rbreplay calls compute after EVERY
 * pass of a frame and confirm once, for its final pass, when the frame is confirmed. */
static DumpBuf *dump_slot_get(int i) { return i >= 0 && i < C1A_DUMP_SLOTS ? &g_db[i] : NULL; }
/* v0.24.0-C1b2 F1.7: first-mismatch dumps (rbreplay only). A pass whose tier-1 verdict is MISMATCH while the previous frame's last pass was
 * equal captures a dump into a spare buffer; it is written (<stem>_f<f>_mm.dump, C1A MMDUMP) only if that pass is the frame's confirmed one */
#define MM_SLOTS 2
static DumpBuf g_mmdb[MM_SLOTS];
static void mm_release(int i) { if (i >= 0 && i < MM_SLOTS) g_mmdb[i].used = 0; }
static int mm_pending(void) { int n = 0; for (int i = 0; i < MM_SLOTS; i++) n += g_mmdb[i].used; return n; }
static void mm_flush(DumpBuf *d, char ps) {
    uint64_t t0 = qpc();
    char p[MAX_PATH], sfx[48]; snprintf(sfx, sizeof sfx, "_f%u_mm.dump", d->f);
    out_path(p, sizeof p, sfx);
    HANDLE h = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    d->used = 0;
    if (h == INVALID_HANDLE_VALUE) { LOG("C1A WARN mm dump %s err=%lu\n", p, GetLastError()); return; }
    DWORD x = 0; int ok = d->ok && d->n && WriteFile(h, d->b, (DWORD)d->n, &x, NULL) && x == d->n;
    CloseHandle(h);
    g_mm_written++;
    const char *nm = strrchr(p, '\\');
    LOG("C1A MMDUMP f=%u file=%s pass=%c us=%.0f bytes=%lu ok=%d battle=%llu (the first tier-1 mismatch of a stretch, the confirmed pass)\n", d->f, nm ? nm + 1 : p, ps ? ps : '-',
        d->us + qpc_us(qpc() - t0), (unsigned long)x, ok, (unsigned long long)g_bat);
}
/* v0.32.0 (OPTA-M2-SPEC.md §21.6, D-B): the raw clash / aif spans of both players (src/c1a.h k_c1a_ab), read right after a pass (the rb modes keep
 * them with the frame's pass until its confirmation) and written as an A line on every change of the confirmed frames (c1a_confirm) */
static void ab_read(uint8_t out[2][C1A_AB_LEN]) {
    for (int p = 0; p < 2; p++)
        for (int s = 0; s < C1A_AB_NSPAN; s++) memcpy(out[p] + k_c1a_ab[s].at, (const void *)VA(ppref(p, k_c1a_ab[s].off)), k_c1a_ab[s].len);
}
static uint8_t g_ab_prev[2][C1A_AB_LEN]; static uint64_t g_ab_bat = ~0ULL;
static void ab_emit(C1aPass *p) {
    if (RBT_T1(g_cfg.h_ab_at_confirm)) { ab_read(p->ab); p->has_ab = 1; }   /* v0.32.0 control XGAB (harness only): the state at the confirmation */
    if (!p->has_ab) return;
    static char ln[2 * C1A_AB_LEN * 2 + 1024];
    int first = g_ab_bat != g_bat;
    size_t n = c1a_format_ab(ln, sizeof ln, p->fr.f, (const uint8_t (*)[C1A_AB_LEN])p->ab, first ? NULL : (const uint8_t (*)[C1A_AB_LEN])g_ab_prev);
    if (first) g_ab_bat = g_bat;
    memcpy(g_ab_prev, p->ab, sizeof g_ab_prev);
    if (!n) return;
    wr_put(&g_w, ln, n);
}
void c1a_pass_drop(C1aPass *p) { if (p->dump >= 0 && p->dump < C1A_DUMP_SLOTS) g_db[p->dump].used = 0; p->dump = -1; mm_release(p->mm); p->mm = -1; }
void c1a_pass_compute(uint32_t f, const C1aIn *used, const char *ifc, uint32_t q, C1aPass *out, double *us) {
    uint64_t t0 = qpc();
    int prev_dump = out->dump;
    memset(&out->fr, 0, sizeof out->fr);
    out->fr.f = f; out->fr.in = *used; out->fr.q = q;
    snprintf(out->fr.ifc, sizeof out->fr.ifc, "%s", ifc ? ifc : "-");
    out->fr.rp = *(volatile uint32_t *)VA(0x1448463f8ULL) != 0;
    t1_compute(&out->fr, &out->alt);
    out->has_ab = 0;
    if (g_cfg.c1a_clash_bytes && !RBT_T1(g_cfg.h_ab_at_confirm)) { ab_read(out->ab); out->has_ab = 1; }   /* v0.32.0 (D-B) */
    vm_pass_end(f, out->vm_cv);   /* v0.32.0 review fix B1: the clash vars after this pass; the pass's VM records travel with frame f (no-op unless a VM key is on) */
    out->t15 = modes_t15();   /* C2 (C2-SPEC.md §5.1): after every pass, every C1a mode; never mixed into tier-1 */
    if (g_mode == MODE_RBONLINE) {   /* optA M2 (OPTA-M2-SPEC.md §5.8, OPTA-M0.md §1.13 D6): the cross-PC CHECK hash = tier-1 with the cin component
                                      * recomputed without the third block's core copy 0x14470d100..0x14470d11f (the local player's own pad on each PC):
                                      * only the core input words 0x144855700 (0x10) stay. Zone frames are never checked (rb.c). */
        uint64_t c2[C1A_NCOMP]; memcpy(c2, out->fr.c, sizeof c2);
        c2[6] = rh(0x144855700ULL, 0x10, 6);
        if (!RBO_CHECK_OLD()) t1_ptr_canon(&c2[8], &c2[9]);   /* v0.29.5 RBO (m6): ctl / ai under the canonical rule (no pin, arena or region decision) */
        out->t1x = hash_bytes(c2, sizeof c2, 0x7132);
    }
    double u1 = qpc_us(qpc() - t0);
    g_us_t1 += u1; g_n_t1++;
    out->has_t2 = 0; out->nt2 = 0;
    if (f % C1A_T2_EVERY == 0) {
        uint64_t t2 = qpc();
        out->nt2 = t2_compute(out->t2); out->has_t2 = 1;
        g_us_t2 += qpc_us(qpc() - t2); g_n_t2++;
    }
    out->dump = -1;
    int every = g_cfg.c1a_dump_every > 0 && f % (uint32_t)g_cfg.c1a_dump_every == 0, at = !every && ((g_cfg.c1a_dump_at_n && dump_at_has(f)) || (g_rt_ndump && rt_dump_has(f)));   /* C1B4; C2: + the runtime frames */
    if (every || at) {
        int slot = prev_dump >= 0 && prev_dump < C1A_DUMP_SLOTS && g_db[prev_dump].f == f ? prev_dump : -1;
        for (int i = 0; slot < 0 && i < C1A_DUMP_SLOTS; i++) if (!g_db[i].used) slot = i;
        if (slot < 0) { if (g_dump_noslot++ < 20) LOG("C1A WARN dump frame=%u: no free capture slot (%d pending); no dump for this frame\n", f, C1A_DUMP_SLOTS); }
        else if (RBT_T1(g_cfg.h_dump_first_pass) && slot == prev_dump) out->dump = slot;   /* C1B4 control DARBX (harness only): keep the first pass's capture */
        else { dump_build(f, &g_db[slot]); g_db[slot].why = at; out->dump = slot; }
    } else if (prev_dump >= 0 && prev_dump < C1A_DUMP_SLOTS && g_db[prev_dump].f == f) g_db[prev_dump].used = 0;
    {   /* v0.24.0-C1b2 F1.7: the first-mismatch dump of a stretch (rbreplay; a later pass of the frame replaces this pass's capture) */
        int prev_mm = out->mm; out->mm = -1;
        if (prev_mm >= 0 && prev_mm < MM_SLOTS && g_mmdb[prev_mm].f == f) mm_release(prev_mm);
        if (g_mode == MODE_RBREPLAY && g_play == PL_OK && f >= 1 && f <= g_rf_n) {
            int vd = t1_verdict(&out->fr, &out->alt, &g_rf_fr[f - 1], NULL);
            const MmEq *pe = &g_mm_eq[(f - 1) & 63];
            int prev_eq = f == 1 || pe->f != f - 1 || pe->eq;
            g_mm_eq[f & 63].f = f; g_mm_eq[f & 63].eq = vd != V_MM;
            /* v0.24.0: only a pass that ran the recorded input (a mispredicted pass always differs and is never the confirmed one) */
            const C1aIn *ri = &g_rf_fr[f - 1].in;
            int rec_in = used->p1 == ri->p1 && used->p2 == ri->p2 && !memcmp(used->x, ri->x, 0x18);
            if (vd == V_MM && prev_eq && rec_in && g_cfg.c1a_mm_dumps > 0 && g_mm_written + (uint32_t)mm_pending() < (uint32_t)g_cfg.c1a_mm_dumps) {
                int slot = -1; for (int i = 0; i < MM_SLOTS && slot < 0; i++) if (!g_mmdb[i].used) slot = i;
                if (slot < 0) { if (g_mm_noslot++ < 5) LOG("C1A WARN mm dump f=%u: no free capture slot (%d pending)\n", f, MM_SLOTS); }
                else { dump_build(f, &g_mmdb[slot]); out->mm = slot; }
            }
        }
    }
    if (us) *us += qpc_us(qpc() - t0);
}
void c1a_confirm(C1aPass *p, const char *sfx) {
    const C1aFrame *fr = &p->fr;
    char line[2304];
    size_t n = c1a_format_frame(line, sizeof line - 256, fr);
    if (n && line[n - 1] == '\n') { int k = snprintf(line + n - 1, sizeof line - n + 1, " t15=%016llx\n", (unsigned long long)p->t15); if (k > 0) n = n - 1 + (size_t)k; }   /* C2: after q=, before the rb suffix (readers that cut at " ps=" see it on both sides) */
    if (n && sfx && sfx[0] && line[n - 1] == '\n') { int k = snprintf(line + n - 1, sizeof line - n + 1, " %s\n", sfx); if (k > 0) n = n - 1 + (size_t)k; }
    if (n) wr_put(&g_w, line, n);
    if (g_cfg.c1a_clash_bytes) ab_emit(p);   /* v0.32.0 (D-B): after the frame's F line */
    vm_confirm(fr->f, p->ps, (const uint16_t (*)[2])p->vm_cv);   /* v0.32.0 review fix B1: the D-F window and its lines, D-G (no-op unless a VM key is on) */
    g_rec_frames = fr->f;
    if (g_play == PL_OK && fr->f <= g_rf_n) cmp_frame(p);
    if (p->has_t2) {
        static char t2l[C1A_MAXCELL * 28 + 64];
        size_t k = c1a_format_t2(t2l, sizeof t2l, fr->f, p->t2, p->nt2);
        if (k) wr_put(&g_w, t2l, k);
        g_t2n++;
        if (g_play == PL_OK && fr->f <= g_rf_n) { if (c1a_t2_comparable()) cmp_t2(fr->f, p->t2, p->nt2, dump_slot_get(p->dump) != NULL); }   /* rules differ: incomparable (RBEND t2cmp); C1B4: + has_dump */
    }
    DumpBuf *d = dump_slot_get(p->dump);
    if (d && d->used && d->f == fr->f) dump_flush(d);
    p->dump = -1;
    if (p->mm >= 0 && p->mm < MM_SLOTS) { DumpBuf *m = &g_mmdb[p->mm]; if (m->used && m->f == fr->f) mm_flush(m, p->ps); else mm_release(p->mm); }   /* v0.24.0 F1.7 */
    p->mm = -1;
}
int c1a_t2_comparable(void) { (void)g_rf_t2tag; return g_rf_t2rule == g_cfg.c1a_t2_heap_tag; }   /* C1B3: raw / heaptag / map1 */
uint32_t c1a_t2_points(void) { return g_t2_compared; }
uint32_t c1a_t1mm(void) { return g_t1mm; }
uint32_t c1a_t1norm(void) { return g_t1norm; }
uint32_t c1a_t1norm_rescue(void) { return g_t1norm_rescue; }    /* v0.24.0-C1b2 fix: why=rescue frames */
uint32_t c1a_t1norm_oldrule(void) { return g_t1norm_old; }      /* v0.24.0-C1b2 fix: why=oldrule frames */
uint32_t c1a_mm_written(void) { return g_mm_written; }
int c1a_demo_battle(void) { return g_seed_orig == C1A_DEMO_SEED; }
void c1a_set_cur_in(const C1aIn *in) { if (g_cm) g_cur.in = *in; }
uint32_t c1a_t1map_bad(void) { return g_t1map_bad; }
uint32_t c1a_rec_frames(void) { return g_rec_frames; }
uint64_t c1a_battle_no(void) { return g_bat; }   /* C2 fix r1 */
uint32_t c1a_next_f(void) { return g_f + 1; }
uint32_t c1a_rec_n(void) { return g_rf == RF_OK ? g_rf_n : 0; }
const C1aFrame *c1a_rec_frame(uint32_t f) { return g_rf == RF_OK && f >= 1 && f <= g_rf_n ? &g_rf_fr[f - 1] : NULL; }
int c1a_play_ok(void) { return g_play == PL_OK; }
int c1a_play_refused(void) { return g_play == PL_MISMATCH || g_play == PL_NODUMP; }
void c1a_cur_ifc(char *ifc, size_t cap, uint32_t *q) { snprintf(ifc, cap, "%s", g_cur.ifc[0] ? g_cur.ifc : "-"); *q = g_cur.q; }
void c1a_step_end(uint64_t seq) {
    (void)seq;
    if (!g_cm || !g_f) return;
    static C1aPass ps; ps.dump = -1; ps.mm = -1; ps.ps = '-';
    uint8_t pt = g_cur.pt;
    c1a_pass_compute(g_f, &g_cur.in, g_cur.ifc, g_cur.q, &ps, NULL);
    ps.fr.pt = pt;
    g_cur = ps.fr;
    g_confirm_now = 1; c1a_confirm(&ps, NULL); g_confirm_now = 0;   /* C1B4: the confirm right after the step (t2mm dumps) */
    c1a_step_rt();
}
void c1a_step_rt(void) {
    if (!g_cm || !g_f) return;
    {   uint64_t u = uecam_now();
        if (u != g_uecam) {
            g_uecam = u;
            if (g_uecam_lines < BUDGET) { g_uecam_lines++; LOG("C1A UECAM battle=%llu frame=%u hash=%016llx (changed)\n", (unsigned long long)g_bat, g_f, (unsigned long long)u); }
            wr_printf(&g_w, "U %u uecam=%016llx\n", g_f, (unsigned long long)u);
        }
    }
    {   int32_t pw[2][2]; pers_read_live(pw);
        if (memcmp(pw, g_pers_watch, sizeof pw)) {
            g_pers_changes++;
            if (g_pers_lines < 10) {
                g_pers_lines++;
                LOG("C1A PERSONALITY_CHANGED battle=%llu frame=%u ai=%d,%d->%d,%d rule=%d,%d->%d,%d injected=%d (the CPU personality changed inside the battle: a UE re-set "
                    "(PlayerInfoHandler +0x1f8 / SetMatchRule +0x48) or core code; the setup injection does not cover it)\n", (unsigned long long)g_bat, g_f,
                    g_pers_watch[0][0], g_pers_watch[1][0], pw[0][0], pw[1][0], g_pers_watch[0][1], g_pers_watch[1][1], pw[0][1], pw[1][1], g_injected);
            }
            memcpy(g_pers_watch, pw, sizeof pw);
        }
    }
    if (g_f % 300 == 0) g_title_dirty = 1;   /* rec=<frames> / t1mm=<n> in the title about every 5 s */
    if (g_f % 60 == 0) wr_flush(&g_w);
}

void c1a_battle_end(void) {
    if (!g_cm) return;
    vm_battle_end();   /* v0.32.0 review fix B1: VMSTAT, the side file closed */
    argc_battle_line(g_bat);   /* v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix's state in this battle (rbreplay; nothing in other modes) */
    close_battle_file("shutdown");
    g_in_bat = 0;
    g_title_dirty = 1;
}

void c1a_title(char *out, size_t cap) { c1a_title2(out, cap, 1); }
void c1a_title2(char *out, size_t cap, int with_t1mm) {
    out[0] = 0;
    /* v0.24.0-C1b2 F1.6; C1B3: + CONTENT_WALK (a tree walk stopped: sizes only), MODSET_DIFF (the mod-set listing is not the recording's) */
    char tbb[64]; snprintf(tbb, sizeof tbb, "%s%s%s", g_t1map_bad ? " T1MAP_BAD" : "", g_cm && g_ct_walkbad && g_f ? " CONTENT_WALK" : "", g_ms_diff ? " MODSET_DIFF" : "");
    const char *tb = tbb;
    if (g_cm == 1) { snprintf(out, cap, " rec=%u%s", g_skip_bat ? 0u : g_rec_frames, tb); return; }   /* v0.24.0: the rblive demo battle records nothing */
    if (g_cm != 2) return;
    if (g_rf == RF_NOFILE) { snprintf(out, cap, " play=NO_FILE"); return; }
    if (g_rf == RF_BAD) { snprintf(out, cap, " play=BAD_FILE"); return; }
    /* v0.22.1: a missing/unusable battle-start dump with injection on is shown from the start (nothing will be injected) */
    if (g_cfg.c1a_inject && !g_rf_dump_ok && (g_play == PL_NONE || g_play == PL_ARMED || g_play == PL_NODUMP)) { snprintf(out, cap, " play=NO_SETUP_DUMP"); return; }
    const char *inj = g_injected ? "(inj)" : "";
    if ((g_play == PL_OK || g_play == PL_END) && g_t1mm && with_t1mm) snprintf(out, cap, " play=%s%s t1mm=%u%s", k_pl[g_play], inj, g_t1mm, tb);
    else snprintf(out, cap, " play=%s%s%s%s", g_play == PL_NONE ? "armed" : k_pl[g_play], g_play == PL_MISMATCH && g_play_why == 1 ? "(content)" : g_play == PL_MISMATCH && g_play_why == 2 ? "(t2rule)" : "",
                  (g_play == PL_OK || g_play == PL_END) ? inj : "", tb);   /* C1B3: SETUP_MISMATCH(content) / (t2rule) */
}

void c1a_flush_crash(void) { if (g_cm && wr_is_open(&g_w)) wr_flush(&g_w); }

/* ------------------------------------------------------------------ C2 (C2-SPEC.md §2.4): the SETUP hashes of mode=rbnet
 * setup_x   = exe, plugin, simcfg, flags, pl0, pl1, stage, assets, the trees, and the rules / copy / ai views (tier-2 normalized, a per-launch
 *             heap unit as C1A_TAG_HEAP) with the side-1 CPU-setup spans and the four control-type words zeroed: equal on both PCs before the
 *             entry writes when the picks are the same;
 * setup_full = the same with nothing masked: equal after the conversion and the guest's injection.
 * The ctl category (PLAYER+0x324, controller+0x30) is not hashed: step 1 derives it from the copy word (FUN_1402facb0; ENTRY CHECK step1). */
int c1a_live_views(C1aView v[C1A_NRANGE]) { return live_views(v); }
typedef struct { uint64_t va; uint32_t len; } NsMask;
static const NsMask k_nsmask[] = {
    { 0x14470f1d0ULL + 0x04, 4 }, { 0x14470f1d0ULL + 0x58 + 0x04, 0x10 },   /* rule type words; side 1 type + rank + waza + personality */
    { 0x144843ef0ULL + 0x34, 4 }, { 0x144843ef0ULL + 0x34 + 0x58, 4 }, { 0x144843ef0ULL + 0x90, 0x0c },   /* copy type words; side 1 rank, waza, personality */
    { AIBLK + AISTR + 0x38, 0x950 }, { AIBLK + AISTR + 0x1a28, 4 }, { AIBLK + AISTR + 0x2bf8, 0x18 },   /* AI1 CPU setup */
};
static uint64_t view_hash_ns(const C1aView *v, int masked, uint64_t h) {
    uint8_t *b = (uint8_t *)malloc(v->len); if (!b) return h ^ 0xdeadULL;
    memcpy(b, v->b, v->len);
    for (size_t u = 0; u * 8 + 8 <= v->len; u++) if (v->k[u] == C1A_K_HEAP) { uint64_t t = C1A_TAG_HEAP; memcpy(b + u * 8, &t, 8); }
    if (masked)
        for (size_t i = 0; i < sizeof k_nsmask / sizeof k_nsmask[0]; i++) {
            uint64_t lo = k_nsmask[i].va, hi = lo + k_nsmask[i].len;
            if (hi <= v->lo || lo >= v->lo + v->len) continue;
            if (lo < v->lo) lo = v->lo; if (hi > v->lo + v->len) hi = v->lo + v->len;
            memset(b + (lo - v->lo), 0, (size_t)(hi - lo));
        }
    h = hash_bytes(b, v->len, h);
    free(b);
    return h;
}
void c1a_net_setup(uint64_t *setup_x, uint64_t *setup_full, uint64_t trees[4]) {
    setup_compute();
    static const int cats[] = { C1A_S_EXE, C1A_S_PLUGIN, C1A_S_SIMCFG, C1A_S_FLAGS, C1A_S_PL0, C1A_S_PL1, C1A_S_STAGE, C1A_S_ASSETS };
    uint64_t h = 0xc2c2;
    for (size_t i = 0; i < sizeof cats / sizeof cats[0]; i++) h = hash_bytes(&g_s[cats[i]], 8, h);
    for (int p = 0; p < 2; p++) { uint64_t t[2] = { g_ct[p].n, g_ct[p].ok ? g_ct[p].keys : 0 }; h = hash_bytes(t, sizeof t, h); trees[2 * p] = t[0]; trees[2 * p + 1] = t[1]; }
    static C1aView v[C1A_NRANGE];
    uint64_t hx = h, hf = h;
    if (live_views(v)) for (int r = 0; r < C1A_NRANGE; r++) { hx = view_hash_ns(&v[r], 1, hx); hf = view_hash_ns(&v[r], 0, hf); }
    else { hx ^= 0xbad; hf ^= 0xbad; }
    *setup_x = hx; *setup_full = hf;
}
