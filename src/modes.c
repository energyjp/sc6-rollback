/* The step hook and the four modes (observe / sync1 / roll7 / loopback). */
#include "modes.h"
#include <xmmintrin.h>
#include "mem.h"
#include "snap.h"
#include "scan.h"
#include "fwd.h"
#include "writeset.h"
#include "l1.h"
#include "vm.h"   /* v0.32.0 review fix B1 */
#include "l4.h"
#include "pres.h"
#include "c1a_run.h"
#include "rb.h"
#include "netsess.h"   /* C2 */
#include "lipsync.h"   /* LIPSYNC: PART B hooks (REVERT-LIPSYNC.md) */
#include "rbo.h"       /* optA M2: rbonline's L3-stock wrapper (run_step) and the armed path's entry points (end of file) */
#include <stdlib.h>
static inline int is_c1a(int m) { return m == MODE_RECORD || m == MODE_REPLAY || is_rbcore_mode(m); }   /* v0.22.0-C1a; v0.23.0-C1b + rbreplay; v0.24.0 + rblive */
static void sw_add_pins(SPin *pins, int *npins, int max);
static uint64_t g_battle_id, g_battle_steps;   /* v0.20: declared early (run_step and L4 use them) */
static int g_resim_fwd;                        /* v0.20 C0b: the current resim pass forwards notifications to UE */
static int g_l4;                               /* v0.20 C0b: L4 canonical listener on (fixed per launch) */
static uint8_t g_lp_fwd[64];                   /* per listener slot: the current pass forwards it to UE */
static int ifc_pre(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp);
static int ifc_post(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp);
static void r7_c0b_report(const char *why);
static void seg_reset(const char *why);
volatile int g_pending_l3 = -1;
static void wsx_add_pins(SPin *pins, int *npins, int max);
static void an_add_pins(SPin *pins, int *npins, int max);
static volatile int g_repin_request;
static void fp_lsn_pre(void);
static void fp_lsn_post(int slot);   /* v0.36.0: modes_lsn_inject brackets its call with the FP guard */
static int g_fp_slot;
static int g_wsl_depth;
static int g_bigscrub;

StepFn o_step;
static void *g_iface;
volatile int g_online, g_in_tick, g_tick_steps;
volatile int g_pending_mode = -1, g_pending_lbp = -1, g_mark_pending;
extern volatile uint32_t lp_bank;

/* v0.23.0-C1b: FrameIn and FPatch are in rb.h */

/* v0.21.0-S1: CL_FX ('F') = effects + presentation (the P table in pres.c), never gameplay. The 5-letter GCFRO class
 * strings stay as they were so old logs and tools still parse. */
enum { CL_GAME = 0, CL_CAM, CL_FX, CL_REPLAY, CL_OTHER, NCL };
static const char k_cl_char[NCL] = { 'G', 'C', 'F', 'R', 'O' };
typedef struct { uint64_t lo, hi; int cls; } ClRange;
static const ClRange k_cl_ranges[] = {   /* sorted, non-overlapping; PLAYER handled separately */
    { 0x1440f4a84, 0x1440f4a88, CL_CAM },     /* v0.20.2: throw camera gate (region "camgate"; saved by the game's camera SaveState) */
    { 0x14470d0c0, 0x14470d0dc, CL_GAME },    /* frame counter, input copies */
    { 0x14470d0dc, 0x14470d0e0, CL_CAM },     /* camera yaw in turns (= director 0x14470ea0c / 360, step tail); no reader in the exe */
    { 0x14470d0e0, 0x14470d198, CL_GAME },
    { 0x14470d198, 0x14470d19c, CL_CAM },     /* camera +0x80 copy */
    { 0x14470d19c, 0x14470d1a0, CL_GAME },    /* read/written by gameplay (0x1402edb90, 0x1402e79c0, 0x1402f54c0) */
    { 0x14470d1a0, 0x14470d200, CL_CAM },     /* camera output (step tail) */
    { 0x14470d200, 0x14470ddf0, CL_GAME },    /* globals block (SaveState 0xbf0) */
    { 0x14470de90, 0x14470dea0, CL_GAME },    /* chara table */
    { 0x14470e180, 0x14470e1c0, CL_CAM },     /* camera matrix (0x14031e950) */
    { 0x14470e2b0, 0x14470e2c8, CL_FX },      /* 2nd RNG (0x14034f3c0, callers 0x1403311f0..0x140333550 = effects) */
    { 0x14470e890, 0x14470e9e0, CL_FX },      /* effect/scrape block + UE-fed segment */
    { 0x14470e9f0, 0x14470f198, CL_CAM },     /* GT_CameraDirectorClass + camera objects + pointers */
    { 0x14470f1c0, 0x14470f1d0, CL_FX },      /* emitter list */
    { 0x14470f1d0, 0x14470f388, CL_GAME },    /* rule block, resources */
    { 0x144715400, 0x1447156f0, CL_GAME },    /* player controllers, init objects */
    { 0x144843ed0, 0x144843f10, CL_GAME },    /* battle mode ptr, workmem owner */
    { 0x144844170, 0x144845e80, CL_GAME },    /* stage/wall blocks (SaveState) */
    { 0x1448462d0, 0x144846470, CL_GAME },    /* timing block, globals */
    { 0x1448545a8, 0x1448554f0, CL_GAME },    /* globals (SaveState 0x1448545f8/0xef0) */
    { 0x144855700, 0x144855710, CL_GAME },    /* step input words */
    { 0x14485eb30, 0x14485ebb8, CL_GAME },    /* TT800 RNG + stage block */
};
/* camera ranges (excluded from snapshot/hash with camera=exclude) */
static const ClRange k_cam_ranges[] = {
    { 0x14470d198, 0x14470d19c, CL_CAM }, { 0x14470d1a0, 0x14470d200, CL_CAM },
    { 0x14470e180, 0x14470e1c0, CL_CAM }, { 0x14470e9f0, 0x14470f198, CL_CAM },
};
#define NCAMR (int)(sizeof k_cam_ranges / sizeof k_cam_ranges[0])
void seg_build(void);
/* round 6: camera layer buffers found by the last scan, extra pins added by EXTW, re-pin request */
static uintptr_t g_cam_bufs[16]; static int g_ncam_bufs;
static uint32_t g_cam_buf_sz;                              /* v0.20.1: layer buffer size from the parsed getter */
static uintptr_t g_camf_base[32]; static int g_ncamf;     /* v0.20.1: camera function buffer bases (4 typed slots each) */
static uint8_t g_camf_full[32];                            /* v0.20.2: block b is pinned whole (1) or as its 4 slot heads (0); v0.21.1: 2 = not pinned (CAMPIN FAIL) */
static int g_cam_pinned_full;                              /* v0.20.2: a scan pinned a whole block (sticky; window title) */
/* v0.21.1: cam_pin_full=1 and the last applied scan could not pin the block whole (no base, or unreadable): rollback is OFF
 * (steps pass straight through, hk_step) until a rescan pins it. The v0.20.2 silent slot fallback is gone. */
static int g_cam_fail, g_cam_ok_pending;
static uint64_t g_cam_fail_steps, g_cam_fail_total, g_cam_fail_scans, g_cam_fail_oks;
#define CAMF_RESCAN 300                                    /* passed-through steps between rescans while failed (about 5 s) */
#define XPIN_MAX 256
static SPin g_xpins[XPIN_MAX]; static int g_nxpins;
static volatile int g_repin_request;
static int is_cam_buffer(uintptr_t a) { for (int i = 0; i < g_ncam_bufs; i++) if (g_cam_bufs[i] == a) return 1; return 0; }
/* round 7: camera function objects (H_CameraFuncCenter etc.) are placement-constructed at fixed slots of
 * one heap buffer [owner+8] by 0x14032f1a0..0x14032f610 (memset(slot, 0, size) then the vtable):
 *   slot 0x7c0: vt 0x143e88be0 / 0x143e88c18 (0xa0), 0x143e88b70 (0x130)
 *   slot 0xf80: vt 0x143e88ab8 (0x68), 0x14032f4c0's object (0x320)
 *   slot 0x1f00: H_CameraFuncCenter vt 0x143e88af0 (0x60)
 *   slot 0x3e00: vt 0x143e88c50 (0xe0)
 * A pointer to any of them gives the buffer base; all four slots are pinned at their largest size. */
typedef struct { uint64_t vt; uint32_t slot; } TypedVt;
static const TypedVt k_typed[] = { { 0x143e88be0ULL, 0x7c0 }, { 0x143e88c18ULL, 0x7c0 }, { 0x143e88b70ULL, 0x7c0 },
                                   { 0x143e88ab8ULL, 0xf80 }, { 0x143e88af0ULL, 0x1f00 }, { 0x143e88c50ULL, 0x3e00 } };
static const uint32_t k_slot_off[4] = { 0x7c0, 0xf80, 0x1f00, 0x3e00 };
static const uint32_t k_slot_len[4] = { 0x130, 0x320, 0x60, 0xe0 };
static int typed_slot_of(uintptr_t vt) {
    for (size_t i = 0; i < sizeof k_typed / sizeof k_typed[0]; i++) if (vt == VA(k_typed[i].vt)) return (int)k_typed[i].slot;
    return -1;
}
/* v0.20.2: the objects that live at fixed slots of the block, at their largest size (audit/camdet/CHECK.md A3, A11, A13):
 *   0x7c0  KO camera (H_CameraFuncIchigekiEnd/End2 0xa0..0xb0, Attack 0x130)
 *   0xf80  H_CameraFuncThrow 0x3e0 (vt 0x143e88858; the other types at 0xf80 are smaller: Critical 0x320, 0x68)
 *   0x1740 H_CameraFuncWinMotion 0x300 (vt 0x143e88a70; H_CameraFuncWin 0x58)
 *   0x1f00 H_CameraFuncCenter 0x60, 0x3e00 the 0xe0 object (the "Producer" slot) */
typedef struct { uint32_t off, len; const char *name; } CamSlot;
static const CamSlot k_cam_slots[5] = { { 0x7c0, 0x130, "KO" }, { 0xf80, 0x3e0, "Throw" }, { 0x1740, 0x300, "WinMotion" },
                                        { 0x1f00, 0x60, "Center" }, { 0x3e00, 0xe0, "Producer" } };
/* v0.20.1: name a heap address inside a camera layer buffer or a camera function block (0 = not camera-owned).
 * v0.20.2: any address of the whole 0x41e0 block: "camera_func_block+0x1338(slot_0xf80 Throw+0x3b8)" */
static int cam_heap_name(uintptr_t va, char *out, size_t cap) {
    for (int i = 0; i < g_ncam_bufs; i++)
        if (g_cam_buf_sz && va >= g_cam_bufs[i] && va < g_cam_bufs[i] + g_cam_buf_sz) { snprintf(out, cap, "camera_layer_buffer[%d]+0x%llx", i, (unsigned long long)(va - g_cam_bufs[i])); return 1; }
    for (int b = 0; b < g_ncamf; b++) {
        if (va < g_camf_base[b] || va >= g_camf_base[b] + CAMF_SIZE) continue;
        uint32_t off = (uint32_t)(va - g_camf_base[b]);
        char bn[32]; if (b) snprintf(bn, sizeof bn, "camera_func_block[%d]", b); else snprintf(bn, sizeof bn, "camera_func_block");
        for (int q = 0; q < 5; q++)
            if (off >= k_cam_slots[q].off && off < k_cam_slots[q].off + k_cam_slots[q].len) {
                snprintf(out, cap, "%s+0x%x(slot_0x%x %s+0x%x)", bn, off, k_cam_slots[q].off, k_cam_slots[q].name, off - k_cam_slots[q].off);
                return 1;
            }
        snprintf(out, cap, "%s+0x%x", bn, off);
        return 1;
    }
    return 0;
}
/* v0.20.2: snapshot regions whose bytes are classified by address (class_of_va): core .bss, the singletons, the camera gate */
static int reg_in_img(const char *nm) { return !strncmp(nm, "core", 4) || !strcmp(nm, "single") || !strcmp(nm, "camgate"); }

/* ------------------------------------------------------------------ listener proxy */
static struct { void **vt; void *pad; } g_proxy;
static uint64_t g_lp_last[FWD_BANKS * FWD_MAXSLOTS];
static int is_query_slot(int i) { return i == 0x138 / 8 || i == 0x140 / 8; }
static uint64_t g_listener_changed, g_listener_nonproxy;

static int lsn_pre(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp);
static void hws_report(const char *why);
static void hws_begin(void);
static const char *field_name(uintptr_t va, uint64_t *rel);
static int class_of_va(uint64_t p);
static int lsn_post(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp);
static void lsn_after_fwd(void);
/* NullListener answers for the two queries (query_policy=null): +0x138 'mov eax,1', +0x140 'xor al,al' */
static int64_t null_q138(void *self) { (void)self; return 1; }
static int64_t null_q140(void *self) { (void)self; return 0; }
void lp_install_family(void) {
    g_proxy.vt = lp_thunk_table;
    lp_family.tidvar = (volatile uint32_t *)&g_ctx_tid;
    lp_family.pre = lsn_pre;
    lp_family.post = lsn_post;
    lp_family.after = lsn_after_fwd;
    lp_family.clean_zero = g_cfg.clean_zero ? 1 : 0;
    ifc_family.pre = ifc_pre;   /* v0.20: L4REENTRY check on the interface census thunks */
    if (is_c1a(g_cfg.mode)) ifc_family.post = ifc_post;   /* v0.22.1-C1a: TT800 around UE's +0xc8 calls (record/replay only) */
}

/* v0.20: with l4=1 the two queries get the NullListener answers in EVERY pass (live too) and never
 * reach UE; with r7_forward=1 resim passes forward notifications to UE like live passes (L4 reverts
 * their writes). g_lp_fwd[] records which slots really call UE in this pass (the L4 revert set).
 * v0.21.0-S1: with l4=2 (guard) the queries are vanilla in live passes (UE answers) and keep the muted/null
 * behavior in resims (query_policy), as without L4. */
volatile uint64_t g_lp_extra_fwd;   /* v0.23.0-C1b: slots a muted pass still forwards (rb.c sets it per pass) */
static void lp_configure(void *real, int muted) {
    void **rvt = *(void ***)real;
    int mute = muted && !g_resim_fwd;
    for (int i = 0; i < LISTENER_NSLOTS; i++) {
        void *fn = (mute && i != 0 && !is_query_slot(i) && !((g_lp_extra_fwd >> i) & 1)) ? NULL : rvt[i];
        if (is_query_slot(i) && (g_l4 == 1 || (muted && !g_cfg.query_forward))) fn = i == 0x138 / 8 ? (void *)null_q138 : (void *)null_q140;
        lp_family.self[i] = real;
        lp_family.fn[i] = fn;
        g_lp_fwd[i] = fn != NULL && fn != (void *)null_q138 && fn != (void *)null_q140;
    }
    lp_bank = muted ? 1 : 0;
}

static uint64_t lp_total(int bank) {
    uint64_t t = 0;
    for (int i = 0; i < LISTENER_NSLOTS; i++) t += lp_family.counts[bank * FWD_MAXSLOTS + i];
    return t;
}

static void lp_report(const char *why) {
    for (int i = 0; i < LISTENER_NSLOTS; i++) {
        uint64_t l = lp_family.counts[i], m = lp_family.counts[FWD_MAXSLOTS + i];
        uint64_t dl = l - g_lp_last[i], dm = m - g_lp_last[FWD_MAXSLOTS + i];
        if (dl || dm) LOG("LSN why=%s slot=+0x%03x live=%llu muted=%llu nonzero=%llu\n", why, i * 8,
                          (unsigned long long)dl, (unsigned long long)dm, (unsigned long long)lp_family.nonzero[i]);
    }
    memcpy(g_lp_last, lp_family.counts, sizeof g_lp_last);
    LOG("LSN why=%s listener_changed=%llu nonproxy_steps=%llu\n", why, (unsigned long long)g_listener_changed, (unsigned long long)g_listener_nonproxy);
}

/* ------------------------------------------------------------------ interface census */
static uint64_t g_ifc_last[IFC_NSLOTS], g_ifc_rep[IFC_NSLOTS];
static uint32_t g_ifc_logged[IFC_NSLOTS];
/* interface calls made by the manager between steps: the first 20 per slot and battle are logged
 * with their step number (setters here are potential foreign writers), the rest are summed */
static void ifc_check(uint64_t seq) {
    for (int i = 0; i < IFC_NSLOTS; i++) {
        uint64_t c = ifc_family.counts[i];
        if (c != g_ifc_last[i]) {
            if (g_ifc_logged[i]++ < 20)
                LOG("IFC seq=%llu fr=%u slot=+0x%02x n=%llu\n", (unsigned long long)seq, core_frame(), i * 8,
                    (unsigned long long)(c - g_ifc_last[i]));
            g_ifc_last[i] = c;
        }
    }
}
static void ifc_report(const char *why) {
    for (int i = 0; i < IFC_NSLOTS; i++) {
        uint64_t c = ifc_family.counts[i];
        if (c != g_ifc_rep[i]) LOG("IFCSUM why=%s slot=+0x%02x n=%llu\n", why, i * 8, (unsigned long long)(c - g_ifc_rep[i]));
        g_ifc_rep[i] = c;
    }
}

/* ------------------------------------------------------------------ region naming */
void describe_va(uintptr_t va, char *out, size_t cap) {
    uintptr_t p0 = VA(A_PLAYER0);
    uintptr_t wm = g_base ? *(uintptr_t *)VA(A_WORKMEM_PTR) : 0;
    if (va >= p0 && va < p0 + PLAYER_STRIDE) snprintf(out, cap, "PLAYER0+0x%llx", (unsigned long long)(va - p0));
    else if (va >= p0 + PLAYER_STRIDE && va < p0 + 2 * PLAYER_STRIDE) snprintf(out, cap, "PLAYER1+0x%llx", (unsigned long long)(va - p0 - PLAYER_STRIDE));
    else if (va >= VA(A_CORE_BSS_LO) && va < VA(A_CORE_BSS_HI)) snprintf(out, cap, "core:0x%llx", (unsigned long long)(va - g_base + IMG_PREF));
    else if (va >= VA(A_DATA_SINGLE_LO) && va < VA(A_DATA_SINGLE_HI)) snprintf(out, cap, "single:0x%llx", (unsigned long long)(va - g_base + IMG_PREF));
    else if (wm && va >= wm && va < wm + WORKMEM_SIZE) snprintf(out, cap, "workmem+0x%llx", (unsigned long long)(va - wm));
    else if (g_arena_ok && arena_owns(&g_arena, (void *)va)) snprintf(out, cap, "arena+0x%llx", (unsigned long long)(va - (uintptr_t)g_arena.base));
    else if (va >= VA(A_IMG_DATA_LO) && va < VA(A_IMG_DATA_HI)) snprintf(out, cap, "img:0x%llx", (unsigned long long)(va - g_base + IMG_PREF));
    else snprintf(out, cap, "heap:%p", (void *)va);
}

/* ------------------------------------------------------------------ snapshot set + ring */
static SnapSet g_ss;
static int g_ss_ready;
static uintptr_t g_ss_workmem;
#define RING_MAX 40

/* ---- listener record/replay (round 2) ----
 * Live runs record every listener call: slot, rax, xmm0, and the bytes UE changed in a
 * 128-byte window at rdx when rdx points into the stack, core state or the arena (slot +0x100
 * at 0x140308433, for example, passes a struct the listener writes back into; the core then
 * stores it at [rbx+0x2c]). Replay runs (same inputs) skip UE and reproduce those effects
 * by call index. */
#define LREC_MAXCALLS 512
#define LREC_WIN 128
typedef struct { uint8_t slot, has_win, nested, pad; uint32_t nchg; uint64_t rax, xmm0; uint8_t mask[LREC_WIN / 8]; uint8_t after[LREC_WIN];
                 uint32_t lw_off, lw_len, sk_off, sk_len; } LCall;   /* lw_*: learned core-state writes made during this call (pool bytes) */
#define LREC_POOL (192 * 1024)
typedef struct { int n, overflow; uint32_t pool_used, pool_overflow; LCall c[LREC_MAXCALLS]; uint8_t pool[LREC_POOL]; } LRec;
/* round 5: UE writes into core state during listener calls (round 4: replay != live in PLAYER fields).
 * LSNW probe: for the first extw_frames sync1x frames, core .bss is diffed around every live listener
 * call; the changed ranges are learned per slot, then recorded after each live call and replayed. */
typedef struct { uint8_t slot; uintptr_t addr; uint32_t len; uint32_t hits; } LWin;
#define LW_MAX 512
static LWin g_lw[LW_MAX];
static int g_nlw, g_lsnw_on;
static uint8_t *g_lsnw_buf;
static uint64_t g_lsnw_calls, g_lw_replayed;
/* round 8: out-parameters in the caller's stack frame (beyond the 128-byte rdx window): bytes in
 * [caller_rsp, caller_rsp + STKWIN) changed by a live listener call are recorded and replayed */
#define STKWIN 0x400
static uint8_t g_stk_before[STKWIN];
static int g_stk_ok;
static uint64_t g_stk_calls, g_stk_bytes;
static int stk_ok(uint64_t p) { NT_TIB *tib = (NT_TIB *)NtCurrentTeb(); return p >= (uintptr_t)tib->StackLimit && p + STKWIN <= (uintptr_t)tib->StackBase; }
static void apply_stack_runs(LRec *r, LCall *c, uint64_t crsp) {
    if (!c->sk_len || !stk_ok(crsp)) return;
    for (uint32_t o = c->sk_off; o + 8 <= c->sk_off + c->sk_len;) {
        uint32_t off, n; memcpy(&off, r->pool + o, 4); memcpy(&n, r->pool + o + 4, 4);
        if (off + n <= STKWIN) memcpy((void *)(crsp + off), r->pool + o + 8, n);
        o += 8 + n;
    }
}
static LRec *g_lrec_cur;           /* record target or replay source */
static int g_lrec_mode;            /* 0 off, 1 record, 2 replay */
static int g_lrec_idx, g_lrec_diverged, g_lrec_depth;
static uint8_t g_lrec_before[LREC_WIN];
static int g_lrec_before_ok;
static uint64_t g_lrec_replayed, g_lrec_divergences, g_lrec_outparam_calls;

/* ---- foreign patches: core-state bytes written between steps, re-applied in resims ---- */

typedef struct { uint64_t seq; int valid; int is_post; Snap snap; FrameIn fi; uint64_t hlive; uint64_t hcls[5]; LRec *lrec; FPatch fp; int resid; } RingEnt;   /* v0.21.0-S1 resid: class mask the commit of this frame left vs its live result */
static RingEnt g_ring[RING_MAX];
static int g_R = 8;
static Snap g_s0, g_p, g_pm, g_lbpost;   /* sync1/sync1x scratch, loopback post-step */
static Snap g_r7post;                    /* round 18: roll7 unified post-step snapshot */
static uint64_t g_lbpost_seq;
static LRec *g_lrec_x;                   /* sync1x record */

volatile int g_npins;
static int pin_index_of(uintptr_t v) {   /* index of a pin covering v, or -1; v0.23.0-C1b: the slot of a visible pin in the stable layout */
    return snapset_lookup(&g_ss, v);
}
static Spin g_pin_lock;
static volatile int g_pins_freed;
static uintptr_t g_pins_freed_list[256];
static volatile LONG g_pins_freed_n;
/* v0.20.0 fix R1: the listener slot in progress (L4-tracked call) when a pinned block was freed, -1 = none known */
static volatile int g_pinfree_slot = -1;
static int g_l4_top_slot;   /* tentative definition; the L4 state below defines it */

static int covered_cb(uintptr_t a, size_t n) {
    for (int i = 0; i < g_ss.nreg; i++)
        if (a >= g_ss.reg[i].addr && a + n <= g_ss.reg[i].addr + g_ss.reg[i].len) return 1;
    if (g_ss.arena && arena_owns(g_ss.arena, (void *)a)) return 1;
    return 0;
}

void pins_note_free(uintptr_t p) {
    if (!g_npins) return;
    if (g_rb_mem_on) { rb_pin_freed_confirm(p); return; }   /* v0.23.0-C1b: a confirmed deferred free of a pin: marked dead, no ring clear */
    spin_lock(&g_pin_lock);
    /* pins start at node starts; a freed block p covers pins at p */
    if (snapset_find_pin(&g_ss, p) >= 0) {
        LONG k = InterlockedIncrement(&g_pins_freed_n) - 1;
        if (k < 256) g_pins_freed_list[k] = p;
        g_pins_freed = 1;
        g_pinfree_slot = g_l4_in_call ? g_l4_top_slot : -1;
    }
    spin_unlock(&g_pin_lock);
}

static int g_ring_cleared;   /* loopback restarts its prediction window after a clear */
static void ring_clear(const char *why) {
    if (is_rbcore_mode(g_mode) && rb_on() && rb_ring_clear(why)) return;   /* v0.23.0-C1b: re-pins are barriers; clears abort (rb.c) */
    int had = 0;
    g_ring_cleared = 1;
    for (int i = 0; i < RING_MAX; i++) { had |= g_ring[i].valid; g_ring[i].valid = 0; }
    g_s0.valid = 0; g_p.valid = 0; g_pm.valid = 0; g_lbpost.valid = 0; g_r7post.valid = 0;
    int nconf = def_confirm_all();
    if (had || nconf) LOG("RING clear why=%s confirmed_frees=%d\n", why, nconf);
}

static RingEnt *ring_at(uint64_t seq) {
    RingEnt *e = &g_ring[seq % (uint64_t)g_R];
    return (e->valid && e->seq == seq) ? e : NULL;
}

/* Round 10: heap state the game's own SaveState (0x1403841e0) saves, which our snapshot lacked.
 * Per chara, 0x14030a980 -> 0x14030ec90 serializes three KHit object lists hanging off
 * PLAYER+0x44078 (heads at +0x400/+0x420/+0x440 = PLAYER+0x44478/+0x44498/+0x444b8, next at +0x18),
 * each object through its own vt+0x30: +0x14 (2 B) and, by class, +0x50..+0x90 (KHitArea),
 * +0x60..+0x90 (KHitFixArea), +0x50..+0x74 (KHitSphere). The hit test 0x14030d6a0 writes +0x54/+0x70
 * every frame (a decaying value), so a restore that skips them resumes with "now" values.
 * The stage hit table (0x144844dd0 or 0x144845e80 by 0x14470dedc; 0x140391ca0) saves +0xc/+0x1c of
 * each 0x40-byte entry of the array at obj+0x408 (count at *(obj)+0x2c), and 0x1403841e0 saves
 * 0x360 bytes at [0x14470dee8]. These ranges are pinned whole (object start .. last saved byte). */
#define VT_KHIT_FIXAREA 0x143e87760ULL
#define VT_KHIT_AREA    0x143e877a8ULL
#define VT_KHIT_SPHERE  0x143e877f0ULL
#define VT_KHIT_BASE    0x143e87838ULL
typedef struct { int khit, area, fix, sphere, base, unknown, covered, pinned, stage_n, dee8; uint64_t bytes; } GsStat;
static size_t khit_extent(uintptr_t vt, GsStat *st) {
    if (vt == VA(VT_KHIT_AREA)) { if (st) st->area++; return 0x90; }
    if (vt == VA(VT_KHIT_FIXAREA)) { if (st) st->fix++; return 0x90; }
    if (vt == VA(VT_KHIT_SPHERE)) { if (st) st->sphere++; return 0x78; }
    if (vt == VA(VT_KHIT_BASE)) { if (st) st->base++; return 0x20; }
    if (st) st->unknown++;
    return 0x20;   /* unknown class: id (+0x14) and the link (+0x18) at least */
}
static int pin_covers(uintptr_t a, size_t n) {
    int k = pin_index_of(a);
    return k >= 0 && a + n <= g_ss.pins[k].addr + g_ss.pins[k].len;
}
/* visit every KHit object of both charas; cb returns nonzero to stop */
static int khit_walk(int (*cb)(uintptr_t obj, size_t ext, void *ctx), void *ctx, GsStat *st) {
    static const uint32_t heads[3] = { 0x44478, 0x44498, 0x444b8 };
    int n = 0;
    for (int c = 0; c < 2; c++) {
        uintptr_t pl = VA(0x1447156f0ULL) + (uintptr_t)c * 0x973f0;
        for (int h = 0; h < 3; h++) {
            uintptr_t o = *(uintptr_t *)(pl + heads[h]);
            for (int guard = 0; o && guard < 512; guard++) {
                if ((o & 7) || o < 0x10000 || o > 0x7ffffffeffffULL || !mem_readable((void *)o, 0x20)) break;
                uintptr_t vt = *(uintptr_t *)o;
                size_t ext = khit_extent(vt, st);
                if (!mem_readable((void *)o, ext)) break;
                n++;
                if (cb && cb(o, ext, ctx)) return n;
                o = *(uintptr_t *)(o + 0x18);
            }
        }
    }
    return n;
}
/* C2 (C2-SPEC.md §5.1): t15, the KHit world-center tripwire. FNV-1a 64 over both charas' KHit lists in list order (the khit_walk heads and
 * links): per object its +0x14 (2 B) and, by class, KHitSphere +0x50..+0x73, KHitArea +0x50..+0x8f, KHitFixArea +0x60..+0x8b; then the
 * global toggle 0x14470dec4. The layout is Fotten's hitbox-system.md (HYPOTHESIS); the class comes from the vtable (V, khit_walk). An
 * unknown vtable: T15 UNKNOWN (once per vt), +0x14 only. An unreadable object: T15 BAD, t15 = all ones. Read-only; never in tier-1. */
static uint64_t t15_fnv(const void *p, size_t n, uint64_t h) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; } return h; }
static uintptr_t g_t15_unk[16]; static int g_t15_nunk, g_t15_bad_lines;
uint64_t modes_t15(void) {
    static const uint32_t heads[3] = { 0x44478, 0x44498, 0x444b8 };
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int c = 0; c < 2; c++) {
        uintptr_t pl = VA(0x1447156f0ULL) + (uintptr_t)c * 0x973f0;
        for (int hd = 0; hd < 3; hd++) {
            uintptr_t o = *(uintptr_t *)(pl + heads[hd]);
            for (int guard = 0; o && guard < 512; guard++) {
                if ((o & 7) || o < 0x10000 || o > 0x7ffffffeffffULL || !mem_readable((void *)o, 0x20)) {
                    if (g_t15_bad_lines++ < 20) LOG("T15 BAD f=%u obj=0x%llx (an unreadable KHit object: t15 = ffffffffffffffff)\n", core_frame(), (unsigned long long)o);
                    return ~0ULL;
                }
                uintptr_t vt = *(uintptr_t *)o;
                size_t lo = 0, hi = 0;
                if (vt == VA(VT_KHIT_SPHERE)) { lo = 0x50; hi = 0x74; }
                else if (vt == VA(VT_KHIT_AREA)) { lo = 0x50; hi = 0x90; }
                else if (vt == VA(VT_KHIT_FIXAREA)) { lo = 0x60; hi = 0x8c; }
                else {
                    int seen = 0; for (int i = 0; i < g_t15_nunk; i++) if (g_t15_unk[i] == vt) seen = 1;
                    if (!seen && g_t15_nunk < 16) { g_t15_unk[g_t15_nunk++] = vt; LOG("T15 UNKNOWN vt=%s0x%llx f=%u (only its +0x14 is hashed)\n", IN_IMAGE(vt) ? "game+" : "abs:", (unsigned long long)(IN_IMAGE(vt) ? RVA(vt) : vt), core_frame()); }
                }
                h = t15_fnv((const void *)(o + 0x14), 2, h);
                if (hi) {
                    if (!mem_readable((void *)o, hi)) { if (g_t15_bad_lines++ < 20) LOG("T15 BAD f=%u obj=0x%llx\n", core_frame(), (unsigned long long)o); return ~0ULL; }
                    h = t15_fnv((const void *)(o + lo), hi - lo, h);
                }
                o = *(uintptr_t *)(o + 0x18);
            }
        }
    }
    return t15_fnv((const void *)VA(0x14470dec4ULL), 4, h);
}
typedef struct { SPin *pins; int *npins; int max; GsStat *st; } GsCtx;
static int gs_add(uintptr_t a, size_t n, GsCtx *g) {
    if (!n || !mem_readable((void *)a, n)) return 0;
    if (covered_cb(a, n)) { g->st->covered++; return 0; }
    for (int i = 0; i < *g->npins; i++) if (g->pins[i].addr <= a && a + n <= g->pins[i].addr + g->pins[i].len) { g->st->covered++; return 0; }
    if (*g->npins >= g->max) return 0;
    g->pins[*g->npins].addr = a; g->pins[*g->npins].len = n; (*g->npins)++;
    g->st->pinned++; g->st->bytes += n;
    return 0;
}
static int gs_khit_cb(uintptr_t obj, size_t ext, void *ctx) { GsCtx *g = (GsCtx *)ctx; g->st->khit++; gs_add(obj, ext, g); return 0; }
static void gs_collect(SPin *pins, int *npins, int max, GsStat *st) {
    GsCtx g = { pins, npins, max, st };
    khit_walk(gs_khit_cb, &g, st);
    static const uint64_t stage[2] = { 0x144844dd0ULL, 0x144845e80ULL };
    for (int s = 0; s < 2; s++) {
        uintptr_t obj = VA(stage[s]);
        uintptr_t blob = *(uintptr_t *)obj, arr = *(uintptr_t *)(obj + 0x408);
        if (!blob || !arr || !mem_readable((void *)(blob + 0x2c), 2)) continue;
        int n = *(int16_t *)(blob + 0x2c);
        if (n <= 0 || n > 4096) continue;
        st->stage_n += n;
        gs_add(arr, (size_t)n * 0x40, &g);
    }
    uintptr_t d = *(uintptr_t *)VA(0x14470dee8ULL);
    if (d && !(d & 7) && d > 0x10000 && d < 0x7ffffffeffffULL) { st->dee8 = 1; gs_add(d, 0x360, &g); }
}
/* per step (routing modes): a KHit object that is not pinned (a list changed) asks for a re-pin */
static int gs_unpinned_cb(uintptr_t obj, size_t ext, void *ctx) {
    if (covered_cb(obj, ext) || pin_covers(obj, ext)) return 0;
    *(int *)ctx = 1;
    return 1;
}
static uint64_t g_gs_repins;
static int gs_check(void) { int miss = 0; khit_walk(gs_unpinned_cb, &miss, NULL); return miss; }

/* v0.21.0-S1 KHit capsule bone census (khit_census=1; diagnostic, read-only): once per player at the first step of each
 * battle (after Setup), the bone ints of every KHit object in the three lists PLAYER+0x44478/+0x44498/+0x444b8 (next at
 * +0x18), read where each class's transform (its vt+0x08) reads its matrix row index (movsxd; shl 6; add rdx):
 *   KHitArea vt 0x143e877a8 -> 0x14030e480: +0x90 and +0x94;  KHitFixArea vt 0x143e87760 -> 0x14030e690: +0x90;
 *   KHitSphere vt 0x143e877f0 -> 0x14030e1a0: +0x7c.  Other classes have no known bone field (counted, not read).
 * The row index is a bone index of the PLAYER bank only if rdx is that bank (P-fields.md §1.4: not verified). face60_95 /
 * above95 count bone references (not unique bones). Never faults: every read is behind mem_readable. */
static uint64_t g_khit_census_battle = ~0ULL;
static int int_cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }
static void khit_census(void) {
    if (!g_cfg.khit_census || g_khit_census_battle == g_battle_id) return;
    g_khit_census_battle = g_battle_id;
    static const uint32_t heads[3] = { 0x44478, 0x44498, 0x444b8 };
    for (int c = 0; c < 2; c++) {
        uintptr_t pl = VA(A_PLAYER0) + (uintptr_t)c * PLAYER_STRIDE;
        int bones[1024], nb = 0, caps = 0, unread = 0, face = 0, above = 0, na = 0, nf = 0, ns = 0, no = 0;
        for (int h = 0; h < 3; h++) {
            if (!mem_readable((void *)(pl + heads[h]), 8)) { unread++; continue; }
            uintptr_t o = *(uintptr_t *)(pl + heads[h]);
            for (int guard = 0; o && guard < 512; guard++) {
                if ((o & 7) || o < 0x10000 || o > 0x7ffffffeffffULL || !mem_readable((void *)o, 0x20)) { unread++; break; }
                uintptr_t vt = *(uintptr_t *)o;
                uint32_t offs[2]; int nofs = 0;
                if (vt == VA(VT_KHIT_AREA)) { offs[0] = 0x90; offs[1] = 0x94; nofs = 2; na++; }
                else if (vt == VA(VT_KHIT_FIXAREA)) { offs[0] = 0x90; nofs = 1; nf++; }
                else if (vt == VA(VT_KHIT_SPHERE)) { offs[0] = 0x7c; nofs = 1; ns++; }
                else no++;
                caps++;
                for (int q = 0; q < nofs; q++) {
                    if (!mem_readable((void *)(o + offs[q]), 4)) { unread++; continue; }
                    int v = *(int32_t *)(o + offs[q]);
                    if (v >= 60 && v <= 95) face++; else if (v > 95) above++;
                    if (nb < 1024) bones[nb++] = v;
                }
                o = *(uintptr_t *)(o + 0x18);
            }
        }
        qsort(bones, (size_t)nb, sizeof bones[0], int_cmp);
        char bs[2048]; size_t bo = 0; bs[0] = 0;
        for (int i = 0; i < nb && bo + 16 < sizeof bs; i++) if (!i || bones[i] != bones[i - 1]) bo += (size_t)snprintf(bs + bo, sizeof bs - bo, "%s%d", bo ? "," : "", bones[i]);
        LOG("KHITBONES battle=%llu p=%d capsules=%d bones=%s face60_95=%d above95=%d unreadable=%d layout=Area+0x90/+0x94(n=%d);FixArea+0x90(n=%d);Sphere+0x7c(n=%d);other_no_bone_field(n=%d);index=the_class_transform_row_(vt+0x08);bank=PLAYER_unverified\n",
            (unsigned long long)g_battle_id, c, caps, bo ? bs : "-", face, above, unread, na, nf, ns, no);
    }
}

/* Round 11 census (read-only): weapon sway nodes and CPU AI objects.
 * Sway: 0x140312040 builds every KSway / aux node with placement-new into PLAYER+0x45700.. (cursor at
 * PLAYER+0x95700), i.e. inside PLAYER and inside every snapshot; the census confirms it live.
 * CPU: HgCpuDirect / CCpu objects (vtables from RTTI) referenced from core state or pins; counted as
 * covered (inside a snapshot range or pin) or not. Pinning waits until their sizes are known. */
#define VT_KSWAY_WEAPON 0x143e88d78ULL
#define VT_KSWAY_NODE   0x143e88db8ULL
#define VT_KSWAY_HIT    0x143e88e38ULL
static const uint64_t k_cpu_vts[] = { 0x143e85578,0x143e855c0,0x143e85608,0x143e85650,0x143e85698,0x143e856e0,0x143e85728,0x143e85770,0x143e857b8,0x143e85800,0x143e85848,0x143e85890,0x143e858d8,0x143e85920,0x143e85968,0x143e859b0,0x143e859f8,0x143e85a40,0x143e85a88,0x143e85ad0,0x143e85b18,0x143e85b60,0x143e85ba8,0x143e85bf0,0x143e85c38,0x143e85c80,0x143e85cc8,0x143e85d10,0x143e85d58,0x143e85da0,0x143e85de8,0x143e85e30,0x143e85e78,0x143e85ec0,0x143e85f08,0x143e85f50,0x143e85f98,0x143e85fe0,0x143e86028,0x143e86070,0x143e860b8,0x143e86100,0x143e86148,0x143e86190,0x143e861d8,0x143e86220,0x143e86268,0x143e862b0,0x143e862f8,0x143e86340,0x143e86388,0x143e863d0,0x143e86418,0x143e86460,0x143e864a8,0x143e864f0,0x143e86538,0x143e86548,0x143e86590,0x143e865d8,0x143e86620,0x143e86668,0x143e866b0,0x143e866f8,0x143e86740,0x143e86788,0x143e867d0,0x143e86818,0x143e86860,0x143e868a8,0x143e868f0,0x143e86938,0x143e86980,0x143e869c8,0x143e86a10,0x143e86a58,0x143e86aa0,0x143e86ae8,0x143e86b30,0x143e86b78,0x143e86bc0,0x143e86c08,0x143e86c50,0x143e86c98,0x143e86ce0,0x143e86d28,0x143e86d70,0x143e86db8,0x143e86e00,0x143e86e48,0x143e86e90,0x143e86ed8,0x143e86f20,0x143e86f68,0x143e86fb0,0x143e86ff8,0x143e891b8,0x143e89200,0x143e89248,0x143e89290 };
static int is_cpu_vt(uintptr_t vt) {
    if (!IN_IMAGE(vt)) return 0;
    uint64_t v = (uint64_t)(vt - g_base + IMG_PREF);
    int lo = 0, hi = (int)(sizeof k_cpu_vts / sizeof k_cpu_vts[0]) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (k_cpu_vts[m] == v) return 1; if (k_cpu_vts[m] < v) lo = m + 1; else hi = m - 1; }
    return 0;
}
/* v0.31.0 (OPTA-M2-SPEC.md §20.3): in rbonline the census (it pins nothing: two LOG lines) runs at the ARM scan only, not at the rescans of the
 * replay edges and the re-pins (V, S-B on v0.30.0: 8.5 ms per rescan on PC 1, 30-38 ms on PC 2, twice per armed round start) */
static int census_skip(const char *why) { return g_mode == MODE_RBONLINE && !RS_OLD(RS_OLD_CENSUS) && strcmp(why, "rbo_arm") != 0; }
static void census_r11(const char *why) {
    uint64_t t0 = qpc();   /* v0.31.0: the census's cost on the CPU line (rbonline) */
    int sw[3] = { 0, 0, 0 }, sw_in = 0;
    for (int c = 0; c < 2; c++) {
        uintptr_t pl = VA(0x1447156f0ULL) + (uintptr_t)c * 0x973f0;
        for (uintptr_t a = pl + 0x45700; a + 8 <= pl + 0x95700; a += 8) {
            uintptr_t v = *(uintptr_t *)a;
            int k = v == VA(VT_KSWAY_WEAPON) ? 0 : v == VA(VT_KSWAY_NODE) ? 1 : v == VA(VT_KSWAY_HIT) ? 2 : -1;
            if (k < 0) continue;
            sw[k]++;
            if (covered_cb(a, 8)) sw_in++;
        }
    }
    LOG("SWAY why=%s weapon=%d node=%d hitnode=%d in_snapshot=%d (pool PLAYER+0x45700..+0x95700, inside PLAYER)\n", why, sw[0], sw[1], sw[2], sw_in);
    /* CPU objects: pointers in core .bss and in pinned blocks whose target starts with a CPU vtable */
    int found = 0, covered = 0;
    static uintptr_t seen[256]; int nseen = 0;
    uintptr_t ranges[2][2] = { { VA(A_CORE_BSS_LO), VA(A_CORE_BSS_HI) }, { 0, 0 } };
    for (int pass = 0; pass < 2; pass++) {
        int npr = pass == 0 ? 1 : g_ss.npin;
        for (int r = 0; r < npr; r++) {
            if (pass == 1 && g_ss.stable && g_ss.pstate[r] != SPIN_ALIVE) continue;   /* v0.23.0-C1b: a dead slot's block may be gone */
            uintptr_t lo = pass == 0 ? ranges[0][0] : g_ss.pins[r].addr, hi = pass == 0 ? ranges[0][1] : g_ss.pins[r].addr + g_ss.pins[r].len;
            for (uintptr_t a = (lo + 7) & ~(uintptr_t)7; a + 8 <= hi; a += 8) {
                uintptr_t v = *(uintptr_t *)a;
                if (v < 0x10000 || v > 0x7ffffffeffffULL || (v & 7) || IN_IMAGE(v)) continue;
                int dup = 0; for (int i = 0; i < nseen; i++) if (seen[i] == v) { dup = 1; break; }
                if (dup) continue;
                if (!mem_readable((void *)v, 16)) continue;
                if (!is_cpu_vt(*(uintptr_t *)v)) continue;
                if (nseen < 256) seen[nseen++] = v;
                found++;
                if (covered_cb(v, 16) || pin_index_of(v) >= 0) covered++;
                else if (found - covered <= 8) LOG("CPUOBJ why=%s obj=%p vt_rva=0x%llx ref=%p uncovered\n", why, (void *)v, (unsigned long long)RVA(*(uintptr_t *)v), (void *)a);
            }
        }
    }
    if (g_mode == MODE_RBONLINE)   /* v0.31.0: + us= (the census's own cost) */
        LOG("CPU why=%s objects=%d covered=%d uncovered=%d (HgCpu*/CCpu* objects referenced from core state or pins) us=%.0f\n", why, found, covered, found - covered, qpc_us(qpc() - t0));
    else LOG("CPU why=%s objects=%d covered=%d uncovered=%d (HgCpu*/CCpu* objects referenced from core state or pins)\n", why, found, covered, found - covered);
}
/* v0.24.0-C1b2 S1BFIX (C1B-R2-SPEC.md §2.4). G2 PINDENY: a pinned block whose itrack caller is in the UE TaskGraph small-task allocator's
 * ranges (mem_deny_rva) -> rb modes RBPINDENY + RBABORT cause=pindeny; roll7 / C0 modes PINDENY + rollback off for the battle (RING clear
 * why=pindeny); record / replay: PINDENY (no rollback to turn off). G3 PINPOOL: a pinned block >= 0x800 whose slots (S in 0x40..0x400
 * dividing its length) mostly start with 0 or a link to a slot start inside the block: RBPINPOOL (a FINDING, never an abort; budget 20).
 * v0.24.0 deviation: a hit also needs >= 2 actual links (an all-zero block is not a free list). */
static int g_pindeny_bat, g_pinpool_n, g_pinpool_lines; static uintptr_t g_pinpool_seen[64];
int modes_pinpool_count(void) { return g_pinpool_n; }
static void s1b_pin_checks(const char *why) {
    for (int i = 0; i < g_ss.npin; i++) {
        if (g_ss.stable && g_ss.pstate[i] != SPIN_ALIVE) continue;
        uintptr_t a = g_ss.pins[i].addr; uint32_t len = g_ss.pins[i].len;
        uint32_t caller = 0; int kind = 0; int tracked = itrack_contains(a, &caller, &kind);
        if (tracked && mem_deny_rva(caller, 2)) {
            if (is_rbcore_mode(g_mode)) rb_pindeny(a, len, caller);
            else {
                LOG("PINDENY addr=0x%llx len=%u caller=0x%x scan=%s mode=%s (a UE TaskGraph small-task block is pinned: S1BFIX G1 missed it)\n", (unsigned long long)a, len, caller, why, g_mode_names[g_mode]);
                if (!is_c1a(g_mode) && !g_pindeny_bat) { g_pindeny_bat = 1; ring_clear("pindeny"); LOG("PINDENY rollback=off battle=%llu (steps pass straight through until the next battle)\n", (unsigned long long)g_battle_id); }
            }
        }
        if (len < 0x800 || !mem_readable((void *)a, len)) continue;
        int seen = 0; for (int k = 0; k < g_pinpool_n && k < 64; k++) if (g_pinpool_seen[k] == a) seen = 1;
        if (seen) continue;
        uint32_t best_s = 0, best_links = 0, best_match = 0;
        for (uint32_t S = 0x40; S <= 0x400; S <<= 1) {
            if (len % S) continue;
            uint32_t ns = len / S, match = 0, links = 0;
            for (uint32_t q = 0; q < ns; q++) {
                uint64_t v = *(const uint64_t *)(a + (uintptr_t)q * S);
                if (!v) { match++; continue; }
                if (v >= a && v < a + len && (v - a) % S == 0) { match++; links++; }
            }
            if (ns >= 8 && match >= 8 && match * 2 >= ns && links >= 2 && (links > best_links || (links == best_links && S > best_s))) { best_s = S; best_links = links; best_match = match; }
        }
        if (!best_s) continue;
        if (g_pinpool_n < 64) g_pinpool_seen[g_pinpool_n] = a;
        g_pinpool_n++;
        if (g_pinpool_lines < 20) {
            g_pinpool_lines++;
            LOG("RBPINPOOL addr=0x%llx len=%u slot=0x%x links=%u caller=0x%x match=%u scan=%s tracked=%d (a pinned block shaped like an allocator pool: a FINDING to investigate)\n",
                (unsigned long long)a, len, best_s, best_links, tracked ? caller : 0, best_match, why, tracked);
        }
    }
}
/* v0.24.0-C1b2 F2.3 SWAYROWS (should): once per battle at the first step (rb modes), per player the sway nodes in the pool PLAYER+0x45700..
 * +0x95700 (KSwayWeapon / KSwayNode / KSwayHit vtables), their bank rows (node +0x20, + 2) and the overlap with the KHit bone ints */
static void sway_rows(void) {
    static const uint32_t heads[3] = { 0x44478, 0x44498, 0x444b8 };
    for (int c = 0; c < 2; c++) {
        uintptr_t pl = VA(A_PLAYER0) + (uintptr_t)c * PLAYER_STRIDE;
        static uint8_t row[1024], kb[1024]; memset(row, 0, sizeof row); memset(kb, 0, sizeof kb);
        int nodes = 0;
        for (uintptr_t a = pl + 0x45700; a + 0x28 <= pl + 0x95700; a += 8) {
            uintptr_t v = *(uintptr_t *)a;
            if (v != VA(VT_KSWAY_WEAPON) && v != VA(VT_KSWAY_NODE) && v != VA(VT_KSWAY_HIT)) continue;
            nodes++;
            int32_t r = *(int32_t *)(a + 0x20) + 2;
            if (r >= 0 && r < 1024) row[r] = 1;
        }
        for (int h = 0; h < 3; h++) {
            if (!mem_readable((void *)(pl + heads[h]), 8)) continue;
            uintptr_t o = *(uintptr_t *)(pl + heads[h]);
            for (int guard = 0; o && guard < 512; guard++) {
                if ((o & 7) || o < 0x10000 || o > 0x7ffffffeffffULL || !mem_readable((void *)o, 0x98)) break;
                uintptr_t vt = *(uintptr_t *)o; int32_t b[2]; int nb = 0;
                if (vt == VA(VT_KHIT_AREA)) { b[0] = *(int32_t *)(o + 0x90); b[1] = *(int32_t *)(o + 0x94); nb = 2; }
                else if (vt == VA(VT_KHIT_FIXAREA)) { b[0] = *(int32_t *)(o + 0x90); nb = 1; }
                else if (vt == VA(VT_KHIT_SPHERE)) { b[0] = *(int32_t *)(o + 0x7c); nb = 1; }
                for (int q = 0; q < nb; q++) if (b[q] >= 0 && b[q] < 1024) kb[b[q]] = 1;
                o = *(uintptr_t *)(o + 0x18);
            }
        }
        char rs[512], ks[512]; size_t ro = 0, ko = 0;
        for (int which = 0; which < 2; which++)
            for (int r = 0; r < 1024; r++) {
                int on = which ? row[r] && kb[r] : row[r];
                if (!on || (r && (which ? row[r - 1] && kb[r - 1] : row[r - 1]))) continue;
                int e = r; while (e + 1 < 1024 && (which ? row[e + 1] && kb[e + 1] : row[e + 1])) e++;
                char *buf = which ? ks : rs; size_t *bo = which ? &ko : &ro;
                int k = e > r ? snprintf(buf + *bo, 512 - *bo, "%s%d..%d", *bo ? "," : "", r, e) : snprintf(buf + *bo, 512 - *bo, "%s%d", *bo ? "," : "", r);
                if (k > 0 && (size_t)k < 512 - *bo) *bo += (size_t)k;
            }
        LOG("SWAYROWS p=%d nodes=%d rows=%s khit_overlap=%s battle=%llu\n", c, nodes, ro ? rs : "-", ko ? ks : "none", (unsigned long long)g_battle_id);
    }
}
/* ------------------------------------------------------------------ v0.29.2-optA-M2: Q1, the Init/Setup blocks (OPTA-M2-SPEC.md §16). ADDITIVE:
 * every function below is reached only in rbonline, or in rbreplay with a q1 key in the ini (q1_present); every other mode keeps its bytes.
 *   - the root walk (src/rbo_q1.inc): at every apply pin scan from ARM on (rbonline: always; rbreplay: q1_roots=1);
 *   - Q1b (§16.4): the tracked blocks of generation >= the ARM generation are pinned (rbonline; rbreplay with rb_init_pins=0);
 *   - the rbreplay hybrid (§16.9): rb_arm_f (frames < F unrouted and lockstep: v0.29.3 M2, rb.c pre_arm_lockstep; ARM = the rebuild at F),
 *     rb_l3stock (run_step), Q1COV.
 * v0.29.3 (the M2 review fixes, OPTA-M2-SPEC.md §16.14): m3 an apply scan whose walk the rbonline gate refused while the rb core runs is a
 * failed walk (fail closed), never a silent set without the Q1 pins; m4 walk or late pins that do not fit the pin array fail closed (dropped=
 * is logged), never a silent truncation. */
#include "rbo_q1.inc"
#ifdef SC6RB_HARNESS
volatile LONG rbo_h_q1_walks, rbo_h_q1_violations, rbo_h_l3s_calls;
volatile LONG rbo_h_q1_scan_fails;   /* v0.29.3: apply scans whose Q1 part counted as failed (q1_failed) */
/* v0.29.3 (review m3, m4): the overrides of rbo_h_q1_pin_scan (-1 = none): the rbonline state the walk's gate reads, rb_on() as the failure
 * rule reads it, the free slots of the pin array before the late pins / the walk, and the v0.29.2 rules as controls (bit 1: a gated walk is a
 * success; bit 2: pins that do not fit are dropped silently) */
static int g_q1_h_state = -1, g_q1_h_rbon = -1, g_q1_h_room = -1, g_q1_h_room_late = -1, g_q1_h_old;
#define Q1H_OLD_GATE (g_q1_h_old & 1)
#define Q1H_OLD_CAP (g_q1_h_old & 2)
#define Q1H_NO_RESCAN_ABORT (g_q1_h_old & 4)   /* XQ1RESCAN (m2's control): a failed rescan walk does not abort the core */
#define Q1H_OLD_SCAN (g_q1_h_old & 8)          /* v0.29.4 Q1: IQ1M-ONX's control: v0.29.3's container classification (an empty map head = a list) */
/* v0.29.5 Q1R: bit 16 = v0.29.4's E7 (no overflow / unchecked gate: m4's control), bit 32 = v0.29.4's E5 rules (m5's control) */
#define Q1H_OLD_ENV (((g_q1_h_old & 16) ? Q1R_OLD_E7 : 0) | ((g_q1_h_old & 32) ? Q1R_OLD_E5 : 0))
volatile LONG rbo_h_qc_calls;   /* v0.29.5 Q1R (m10): Q1COV entries past their gate (allocation, begin, every step phase, the report, E5CHK) */
#define QC_H_COUNT() InterlockedIncrement(&rbo_h_qc_calls)
#else
#define Q1H_OLD_GATE 0
#define Q1H_OLD_CAP 0
#define Q1H_NO_RESCAN_ABORT 0
#define Q1H_OLD_SCAN 0
#define Q1H_OLD_ENV 0
#define QC_H_COUNT() ((void)0)
#endif
static int g_q1_sig = -1, g_q1_sig_rows; static char g_q1_sig_bad[600];
static int g_q1_walk_on;          /* this mode walks once armed (rbonline; rbreplay q1_roots=1) */
static int g_q1_late_on;          /* the late-pin rule replaces C1b's tracked-block pins (rbonline; rbreplay rb_init_pins=0) */
static int g_q1_rb;               /* rbreplay with a q1 key in the ini (the title token, the ARM point) */
static int g_q1_armed;            /* this battle's ARM point passed (rbonline: modes_rbo_arm; rbreplay: frame rb_arm_f, or the first build) */
static int g_q1_arming;           /* inside the ARM-time ss_build */
static int g_q1_noroute;          /* rbreplay rb_arm_f > 0: the frames before F run without arena routing (run_step) */
static uint32_t g_q1_arm_gen, g_q1_arm_f;
static int g_q1_scan_fail;        /* the last walk failed a gameplay entry (ss_build refuses in rbonline) */
static char g_q1_fail[32];        /* "<E#>:<why>" of the last failed walk, "" = none */
static char g_q1_fail_bat[32];    /* the first failure of this battle (rbreplay title q1=FAIL:E#) */
static int g_q1_walked_bat;       /* a walk of this battle passed (rbreplay title q1=ok) */
static uint32_t g_q1_skip; static int g_q1_hoff __attribute__((unused)), g_q1b_hoff;   /* harness: h_rbo_q1_skip, h_rbo_q1_off, h_rbo_q1b_off (0 in the plugin) */
static uint32_t g_q1_skip_arm; static int g_qc_hold;   /* v0.29.4 Q1 (harness only, 0 in the plugin): h_rbo_q1_skip_arm (entries skipped at the ARM walk only), h_q1cov_old (v0.29.3's covered rule) */
static int g_q1_viol_logged;
static uintptr_t g_q1_unk_seen[16]; static int g_q1_nunk_seen;   /* v0.29.4 Q1: the empty unknown containers already named this battle (Q1 CONT) */
static int g_qc_on;               /* Q1COV: q1_cov=1 in rbreplay (per process) */
static int qc_alloc(void);

int modes_q1_sig(void) {
    if (g_q1_sig < 0) {
        g_q1_sig = q1_sig_check(g_q1_sig_bad, sizeof g_q1_sig_bad, &g_q1_sig_rows);
        g_q1_sig_rows += Q1_NVT;
        LOG("Q1 SIG ok=%d rows=%d bad=%s\n", g_q1_sig, g_q1_sig_rows, g_q1_sig_bad);
    }
    return g_q1_sig;
}
int modes_q1_rows(void) { return Q1_NSIG + Q1_NPTR + Q1_NVT; }   /* v0.29.3: + the Q1PTR rows (B1) */
const char *modes_q1_fail(void) { return g_q1_fail[0] ? g_q1_fail : NULL; }
static uint32_t q1_battle(void) { return g_mode == MODE_RBONLINE ? rbo_battle() : (uint32_t)rb_battle_no(); }
/* the per-process setup (modes_init for rbreplay, modes_rbo_init for rbonline): which rules run, the harness keys */
static void q1_setup(void) {
    if (g_cfg.mode == MODE_RBONLINE) { g_q1_walk_on = 1; g_q1_late_on = 1; g_q1_rb = 0; }
    else if (g_cfg.mode == MODE_RBREPLAY && g_cfg.q1_present) { g_q1_rb = 1; g_q1_walk_on = g_cfg.q1_roots == 1; g_q1_late_on = g_cfg.rb_init_pins == 0; g_qc_on = g_cfg.q1_cov == 1; }
    else return;
    if (g_qc_on) qc_alloc();
    g_scan_q1 = 1;   /* v0.29.4 Q1: the container scan classifies an empty std::map head as a tree in the Q1 modes (scan.h) */
#ifdef SC6RB_HARNESS
    g_q1_hoff = cfg_geti("h_rbo_q1_off", 0); g_q1b_hoff = cfg_geti("h_rbo_q1b_off", 0);
    g_q1_h_room_late = cfg_geti("h_q1_late_room", -1);   /* v0.29.3 (m4): the free pin slots before the late pins of every apply scan (IQ1QCAP) */
    g_q1_h_old = cfg_geti("h_q1_old_rules", 0);          /* v0.29.3: the v0.29.2 rules as controls (1 gate, 2 cap); v0.29.4 Q1: 8 = v0.29.3's classification */
    if (Q1H_OLD_SCAN) g_scan_q1 = 0;
    if (g_q1_h_room_late >= 0 || g_q1_h_old) LOG("Q1 harness plants h_q1_late_room=%d h_q1_old_rules=%d\n", g_q1_h_room_late, g_q1_h_old);
    {   char sk[64]; cfg_gets("h_rbo_q1_skip", "", sk, sizeof sk);
        for (int i = 0; i < Q1E_N; i++) if (sk[0] && strstr(sk, k_q1_ent[i])) g_q1_skip |= 1u << i; }
    {   char sk[64]; cfg_gets("h_rbo_q1_skip_arm", "", sk, sizeof sk);   /* v0.29.4 Q1: IQ1L-LATE (a block written uncovered from ARM to the first rescan) */
        for (int i = 0; i < Q1E_N; i++) if (sk[0] && strstr(sk, k_q1_ent[i])) g_q1_skip_arm |= 1u << i; }
    g_qc_hold = cfg_geti("h_q1cov_old", 0);   /* v0.29.4 Q1: IQ1L-LATEX: the report ignores writes made while uncovered (v0.29.3's rule) */
    if (g_q1_skip_arm || g_qc_hold) LOG("Q1 harness plants h_rbo_q1_skip_arm=0x%x h_q1cov_old=%d\n", g_q1_skip_arm, g_qc_hold);
    if (g_cfg.mode == MODE_RBONLINE && cfg_geti("h_q1cov_rbo", 0)) {   /* v0.29.5 Q1R (m10): Q1COVRB, the teeth of "Q1COV never runs in rbonline" */
        g_qc_on = 1; qc_alloc();
        LOG("Q1 harness plants h_q1cov_rbo=1 (Q1COV forced on in rbonline: the control of rbo_h_qc_calls == 0)\n");
    }
    if (g_q1_hoff) g_q1_walk_on = 0;
    if (g_q1_hoff || g_q1b_hoff || g_q1_skip)
        LOG("Q1 harness plants h_rbo_q1_off=%d h_rbo_q1b_off=%d h_rbo_q1_skip=0x%x (the walk / the late pins / single entries off on this peer)\n", g_q1_hoff, g_q1b_hoff, g_q1_skip);
#endif
}
static void q1_battle_reset(void) {
    g_q1_armed = 0; g_q1_arming = 0; g_q1_scan_fail = 0; g_q1_fail[0] = 0; g_q1_fail_bat[0] = 0; g_q1_walked_bat = 0; g_q1_arm_f = 0;
    g_q1_noroute = g_q1_rb && g_cfg.rb_arm_f > 0;
    g_q1_nunk_seen = 0;   /* v0.29.4 Q1 */
}
static int q1_late_rule(void) { return g_q1_late_on && (g_mode == MODE_RBONLINE || g_mode == MODE_RBREPLAY); }

/* Q1COV (rbreplay q1_cov=1, §16.9): the coverage of every tracked block the live passes write. Pin sources of the last scans (tags) */
enum { QT_NONE = 0, QT_E1, QT_E2, QT_E3, QT_E4, QT_E6, QT_REGION, QT_CAM, QT_CONT, QT_GS, QT_LATE, QT_ARENA, QT_PIN, QT_E5, QT_E7, QT_E8, QT_N };   /* v0.29.4 Q1: + E5 E7 E8 (appended: by= keeps its prefix) */
static const char *const k_qt[QT_N] = { "none", "E1", "E2", "E3", "E4", "E6", "region", "cam", "cont", "gs", "late", "arena", "pin", "E5", "E7", "E8" };
static int q1_qt_of(int ent) {   /* v0.29.4 Q1: a walk entry's pin-source tag */
    static const uint8_t t[Q1E_N] = { QT_E1, QT_E2, QT_E3, QT_E4, QT_E6, QT_E5, QT_E7, QT_E8 };
    return ent >= 0 && ent < Q1E_N ? t[ent] : QT_PIN;
}
static int g_qc_active;            /* this battle's coverage window is open (from the ARM frame) */
static uint8_t g_ptag[40960];      /* do_pin_scan: the source tag of each collected pin (only while g_qc_active) */
static void q1_tag(int from, int to, int tag) { if (g_qc_active) for (int i = from; i < to && i < 40960; i++) g_ptag[i] = (uint8_t)tag; }
static void qc_note_pins(const SPin *pins, int n);
static void qc_note_census(const FoundCont *f, int n);   /* v0.29.4 Q1 */
static void qc_eval_all(void);
static void qc_reset(void);
static void qc_begin(uint32_t f);
/* the ARM point (rbonline: modes_rbo_arm; rbreplay: frame rb_arm_f, or the first build of the battle): from here the walk runs and the
 * tracked blocks of the new generation are the late pins; Q1COV's window opens (q1_cov=1) */
static void q1_arm_begin(const char *why) {
    g_q1_arm_gen = itrack_gen_bump();
    g_q1_armed = 1; g_q1_arming = 1; g_q1_noroute = 0;
    g_q1_arm_f = c1a_next_f();
    LOG("Q1 ARM mode=%s b=%u f=%u why=%s gen=%u walk=%d late=%d cov=%d\n", g_mode_names[g_mode], q1_battle(), g_q1_arm_f, why, g_q1_arm_gen, g_q1_walk_on, g_q1_late_on, g_qc_on);
    if (g_qc_on) qc_begin(g_q1_arm_f);
    g_title_dirty = 1;
}

/* Q1b: the tracked blocks of generation >= the ARM generation (all allocated after ARM), pinned as C1b's track_init path pins them.
 * v0.29.3 (review m4): returns the blocks that did not fit the pin array (dropped=, logged only when > 0): the caller fails closed */
static int q1_late_pins(SPin *pins, int *npins, int max) {
    static ItInfo it[24576];
    uint64_t tb = 0;
    int n = itrack_list_ex(it, 24576, g_q1_arm_gen, &tb), added = 0, denied = 0, dropped = 0;
    uint64_t ab = 0;
    for (int i = 0; i < n; i++) {
        if (mem_deny_rva(it[i].caller, 2)) denied++;   /* S1BFIX G1 never tracks these; G2 (s1b_pin_checks) aborts if one is pinned */
        if (g_q1b_hoff) continue;
        if (covered_cb(it[i].p, it[i].size)) continue;
        if (!mem_readable((void *)it[i].p, it[i].size)) continue;
        if (*npins >= max) { if (Q1H_OLD_CAP) break; dropped++; continue; }   /* v0.29.2 stopped here silently (Q1H_OLD_CAP: the harness control) */
        pins[*npins].addr = it[i].p; pins[*npins].len = it[i].size; (*npins)++; added++; ab += it[i].size;
    }
    char dr[48] = ""; if (dropped) snprintf(dr, sizeof dr, " dropped=%d", dropped);
    LOG("Q1 LATE b=%u f=%u after_arm=%d pinned=%d bytes=%llu denied=%d%s%s\n", q1_battle(), c1a_next_f(), n, added, (unsigned long long)ab, denied, dr,
        g_q1b_hoff ? " (harness h_rbo_q1b_off=1: none pinned)" : dropped ? " (the pin array is full: the set would miss these blocks, so the scan fails closed)" : "");
    return dropped;
}

/* the walk's log fields */
static void q1_fmt_ent(char *o, size_t cap, int i, const Q1Ent *e) {
    if (e->harness_skip) { snprintf(o, cap, "skip:harness"); return; }
    if (e->st == 1) { snprintf(o, cap, "absent"); return; }
    if (q1_skippable(i) && e->st == 2) { snprintf(o, cap, "skip:%s", k_q1_why[e->why < Q1W_N ? e->why : 0]); return; }   /* v0.29.4 Q1: E4 and E8 */
    switch (i) {
    case Q1E_E1: snprintf(o, cap, "%u/%u/%llu", e->a, e->b, (unsigned long long)e->bytes); break;
    case Q1E_E2: snprintf(o, cap, "%u/%u/%llu", e->a, e->b, (unsigned long long)e->bytes); break;
    case Q1E_E3: if (e->grown) snprintf(o, cap, "%u/%llu grown=%u", e->a, (unsigned long long)e->bytes, e->grown); else snprintf(o, cap, "%u/%llu", e->a, (unsigned long long)e->bytes); break;
    case Q1E_E4: snprintf(o, cap, "%u/%u/%llu", e->a, e->b, (unsigned long long)e->bytes); break;
    /* v0.29.4 Q1: E5 tables/bytes (+ idle= inactive slots with a table), E7 known/whole blocks/bytes, E8 objects/bytes (+ other= another class) */
    case Q1E_E5: if (e->b) snprintf(o, cap, "%u/%llu idle=%u", e->a, (unsigned long long)e->bytes, e->b); else snprintf(o, cap, "%u/%llu", e->a, (unsigned long long)e->bytes); break;
    case Q1E_E7: snprintf(o, cap, "%u/%u/%llu", e->a, e->b, (unsigned long long)e->bytes); break;
    case Q1E_E8: if (e->b) snprintf(o, cap, "%u/%llu other=%u", e->a, (unsigned long long)e->bytes, e->b); else snprintf(o, cap, "%u/%llu", e->a, (unsigned long long)e->bytes); break;
    default: snprintf(o, cap, "%u/%llu", e->a, (unsigned long long)e->bytes); break;
    }
}
static int q1_fmt_walk(char *out, size_t cap, const char *mode, uint32_t b, uint32_t f, const char *why, const Q1Res *r, double us) {
    char es[Q1E_N][48];
    for (int i = 0; i < Q1E_N; i++) q1_fmt_ent(es[i], sizeof es[i], i, &r->e[i]);
    char fl[32];
    if (r->ok) snprintf(fl, sizeof fl, "-"); else snprintf(fl, sizeof fl, "%s:%s", k_q1_ent[r->fail_ent], k_q1_why[r->fail_why < Q1W_N ? r->fail_why : 0]);
    /* v0.29.4 Q1: e5= e7= unknown= e8= appended after fail= (every existing field and its order kept) */
    return snprintf(out, cap, "Q1 WALK mode=%s b=%u f=%u why=%s ok=%d e1=%s e2=%s e3=%s e4=%s e6=%s arena_skipped=%u pins=%u bytes=%llu us=%.0f fail=%s e5=%s e7=%s unknown=%u e8=%s",
                    mode, b, f, why, r->ok, es[0], es[1], es[2], es[3], es[4], r->arena_skipped, r->pins, (unsigned long long)r->bytes, us, fl,
                    es[Q1E_E5], es[Q1E_E7], r->unk_n, es[Q1E_E8]);
}
/* the walk at an apply pin scan. 1 = ok (its pins appended), 0 = a gameplay entry failed (nothing appended), -1 = not run */
static SPin g_q1_wp[16384]; static uint8_t g_q1_wt[16384];
static void q1_log_extra(const Q1Res *r, const Q1Env *env);   /* v0.29.4 Q1: the Q1 CONT lines and Q1COV E5CHK (below the Q1COV block); v0.29.5 Q1R: + env */
/* v0.29.4 Q1: + the census (E7); v0.29.5 Q1R (m4): + nfound_total (the scan's count: nfound is at most 512) and the scan's ScanQ1Stat */
static int q1_walk_scan(const char *why, SPin *pins, int *npins, int max, const FoundCont *found, int nfound, int nfound_total) {
    if (g_mode == MODE_RBONLINE) {   /* §16.5 item 2: only while ARMED / DRAINED or inside the ARM (never PASSIVE / PROBE / PAIRED): nothing read */
        RboState st = rbo_state();
#ifdef SC6RB_HARNESS
        if (g_q1_h_state >= 0) st = (RboState)g_q1_h_state;   /* v0.29.3: rbo_h_q1_pin_scan's forced state (Q1GATE) */
#endif
        if (!(st == RBO_ARMED || st == RBO_DRAINED || g_q1_arming)) {
#ifdef SC6RB_HARNESS
            InterlockedIncrement(&rbo_h_q1_violations);
#endif
            if (!g_q1_viol_logged++) LOG("Q1 UNEXPECTED kind=walk_while_%s why=%s (nothing read)\n", k_rbo_state[st < RBO_NSTATE ? st : 0], why);
            return -1;
        }
    }
    Q1Env env; env.arena = g_arena_ok ? &g_arena : NULL; env.skip_mask = g_q1_skip | (g_q1_arming ? g_q1_skip_arm : 0);   /* v0.29.3: g_q1_scan_fail is reset by do_pin_scan (the late pins come first); v0.29.4 Q1: + skip_arm (harness) */
    env.census = found; env.ncensus = nfound;   /* v0.29.4 Q1: E7 */
    env.ncensus_total = nfound_total; env.scan = &g_scan_q1_stat; env.h_old = Q1H_OLD_ENV;   /* v0.29.5 Q1R (m4, m5) */
    Q1Out o; o.pins = g_q1_wp; o.tag = g_q1_wt; o.n = 0; o.max = 16384; o.overflow = 0;
    Q1Res r;
    uint64_t t0 = qpc();
    int ok = q1_walk_run(&env, &r, &o);
    double us = qpc_us(qpc() - t0);
#ifdef SC6RB_HARNESS
    InterlockedIncrement(&rbo_h_q1_walks);
#endif
    int need = 0, room = max - *npins, dropped = 0, drop_ent = -1;   /* v0.29.3 (review m4): the walk's pins must all fit, or the walk fails */
    if (room < 0) room = 0;
    if (ok) {
        for (int i = 0; i < o.n; i++) {
            if (covered_cb(o.pins[i].addr, o.pins[i].len)) continue;
            if (++need > room && drop_ent < 0) drop_ent = o.tag[i];
        }
        if (need > room && !Q1H_OLD_CAP) { dropped = need - room; ok = 0; r.ok = 0; r.fail_ent = drop_ent; r.fail_why = Q1W_BOUND; }
    }
    char wy[64]; if (g_q1_arming) snprintf(wy, sizeof wy, "arm"); else snprintf(wy, sizeof wy, "rescan:%s", why);
    char line[800]; q1_fmt_walk(line, sizeof line, g_mode_names[g_mode], q1_battle(), c1a_next_f(), wy, &r, us);   /* v0.29.4 Q1: 640 -> 800 (e5= e7= e8=) */
    LOG("%s\n", line);
    q1_log_extra(&r, &env);   /* v0.29.4 Q1: Q1 CONT (E7's unknown containers), Q1COV E5CHK; v0.29.5 Q1R: + Q1 CONT overflow / unchecked */
    if (dropped) LOG("Q1 PINS FULL b=%u f=%u need=%d room=%d dropped=%d first=%s (the walk's pins do not fit the pin array: the walk fails closed, fail=%s:bound)\n",
                     q1_battle(), c1a_next_f(), need, room, dropped, k_q1_ent[drop_ent], k_q1_ent[drop_ent]);
    if (!ok) {
        g_q1_scan_fail = 1;
        snprintf(g_q1_fail, sizeof g_q1_fail, "%s:%s", k_q1_ent[r.fail_ent], k_q1_why[r.fail_why < Q1W_N ? r.fail_why : 0]);
        if (!g_q1_fail_bat[0]) snprintf(g_q1_fail_bat, sizeof g_q1_fail_bat, "%s", k_q1_ent[r.fail_ent]);
        g_title_dirty = 1;
        return 0;
    }
    g_q1_walked_bat = 1; g_title_dirty = 1;
    for (int i = 0; i < o.n && *npins < max; i++) {
        if (covered_cb(o.pins[i].addr, o.pins[i].len)) continue;
        if (g_qc_active && *npins < 40960) g_ptag[*npins] = (uint8_t)q1_qt_of(o.tag[i]);   /* v0.29.4 Q1: was QT_E1 + tag (E5 E7 E8 come after QT_PIN) */
        pins[(*npins)++] = o.pins[i];
    }
    return 1;
}
#ifdef SC6RB_HARNESS
void do_pin_scan(const char *why, int apply);
/* v0.29.3 (review m3, m4): one REAL do_pin_scan(apply=1) with the overrides above (state / rbon / room: -1 = the real value; old: the v0.29.2
 * rules as controls). Returns 1 when the scan's Q1 part counted as failed (the production q1_failed), else 0. */
int rbo_h_q1_pin_scan(int state, int rbon, int room, int old) {
    g_q1_h_state = state; g_q1_h_rbon = rbon; g_q1_h_room = room; int old0 = g_q1_h_old; g_q1_h_old = old;
    LONG f0 = rbo_h_q1_scan_fails;
    do_pin_scan("h_q1_probe", 1);
    g_q1_h_state = g_q1_h_rbon = g_q1_h_room = -1; g_q1_h_old = old0;
    return rbo_h_q1_scan_fails != f0;
}
int rbo_h_q1_walk(int apply, char *line, size_t cap) {
    if (apply != 0 && apply != 2) return -1;   /* v0.29.4 Q1: apply 2 = the walk with a census (a read-only container scan first, nothing applied) */
    if (!line || cap < 64) return -1;
    Q1Env env; env.arena = g_arena_ok ? &g_arena : NULL; env.skip_mask = g_q1_skip;
    env.census = NULL; env.ncensus = 0;
    env.ncensus_total = 0; env.scan = NULL; env.h_old = Q1H_OLD_ENV;   /* v0.29.5 Q1R */
    static FoundCont hf[512]; static SPin hp[16384];
    int nf = 0;
    if (apply == 2) {   /* the same ranges as do_pin_scan; its pins go nowhere */
        ScanRange rr[3]; int nr = 0, np = 0;
        rr[nr].lo = VA(A_CORE_BSS_LO); rr[nr].hi = VA(A_CORE_BSS_HI); nr++;
        rr[nr].lo = VA(A_DATA_SINGLE_LO); rr[nr].hi = VA(A_DATA_SINGLE_HI); nr++;
        uintptr_t wm = *(uintptr_t *)VA(A_WORKMEM_PTR);
        if (wm && mem_readable((void *)wm, WORKMEM_SIZE)) { rr[nr].lo = wm; rr[nr].hi = wm + WORKMEM_SIZE; nr++; }
        nf = scan_containers(rr, nr, covered_cb, hf, 512, hp, 16384, &np);
        env.census = hf; env.ncensus = nf < 512 ? nf : 512;
        env.ncensus_total = nf; env.scan = &g_scan_q1_stat;   /* v0.29.5 Q1R (m4) */
    }
    Q1Out o; o.pins = g_q1_wp; o.tag = g_q1_wt; o.n = 0; o.max = 16384; o.overflow = 0;
    Q1Res r;
    int ok = q1_walk_run(&env, &r, &o);
    int k = q1_fmt_walk(line, cap, g_mode_names[g_mode], 0, 0, "test", &r, 0.0);
    size_t p = k > 0 && (size_t)k < cap ? (size_t)k : cap - 1;
    int w = snprintf(line + p, cap - p, "\nQ1 PINS n=%d ", o.n);
    if (w > 0 && p + (size_t)w < cap) p += (size_t)w;
    for (int i = 0; i < o.n && p + 40 < cap; i++) { w = snprintf(line + p, cap - p, "%s0x%llx:0x%x", i ? "," : "", (unsigned long long)o.pins[i].addr, o.pins[i].len); if (w > 0) p += (size_t)w; }
    if (p + 2 <= cap) { line[p++] = '\n'; line[p] = 0; } else line[cap - 1] = 0;
    /* v0.29.4 Q1: the pins' entries (E-names, walk order) and E7's unknown containers, for the in-process cases */
    w = snprintf(line + p, cap > p ? cap - p : 0, "Q1 PINENT ");
    if (w > 0 && p + (size_t)w < cap) p += (size_t)w;
    for (int i = 0; i < o.n && p + 8 < cap; i++) { w = snprintf(line + p, cap - p, "%s%s", i ? "," : "", k_q1_ent[o.tag[i] < Q1E_N ? o.tag[i] : 0]); if (w > 0) p += (size_t)w; }
    w = snprintf(line + p, cap > p ? cap - p : 0, "\nQ1 UNK n=%u heap=%u", r.unk_n, r.unk_heap);
    if (w > 0 && p + (size_t)w < cap) p += (size_t)w;
    for (uint32_t i = 0; i < r.unk_n && i < Q1_UNK_MAX && p + 80 < cap; i++) {
        w = snprintf(line + p, cap - p, " 0x%llx:%s:%llu:%d", (unsigned long long)r.unk[i].obj, r.unk[i].kind == 1 ? "list" : "tree", (unsigned long long)r.unk[i].size, r.unk[i].nodes_out);
        if (w > 0) p += (size_t)w;
    }
    if (apply == 2 && p + 160 < cap) {   /* v0.29.5 Q1R (m4): what the scan could not record (scan.h ScanQ1Stat) and the first unchecked heads */
        const ScanQ1Stat *s = &g_scan_q1_stat;
        w = snprintf(line + p, cap - p, "\nQ1 SCANQ1 containers=%d recorded=%d unchecked=%d oversize=%d budget=%d pins_dropped=%d nodes_cut=%d", nf, env.ncensus, s->unchecked,
                     s->oversize, s->budget, s->pins_dropped, s->nodes_cut);
        if (w > 0 && p + (size_t)w < cap) p += (size_t)w;
        for (int i = 0; i < s->unchecked && i < SCANQ1_U && p + 96 < cap; i++) {
            w = snprintf(line + p, cap - p, " 0x%llx:0x%llx:%s:%llu:%d", (unsigned long long)s->u[i].obj, (unsigned long long)s->u[i].head, s->u[i].kind == 1 ? "list" : "tree",
                         (unsigned long long)s->u[i].size, s->u[i].why);
            if (w > 0) p += (size_t)w;
        }
    }
    if (p + 2 <= cap) { line[p++] = '\n'; line[p] = 0; } else line[cap - 1] = 0;
    return ok;
}
/* v0.29.5 Q1R: the in-process controls (Q1WALK5): h_q1_old_rules set at run time (16 = v0.29.4's E7, 32 = v0.29.4's E5); returns the old bits */
int rbo_h_q1_old_rules(int bits) { int o = g_q1_h_old; g_q1_h_old = bits; return o; }
#endif

void do_pin_scan(const char *why, int apply) {
    static FoundCont found[512];
    static SPin pins[40960];
    ScanRange rr[4]; int nr = 0;
    rr[nr].lo = VA(A_CORE_BSS_LO); rr[nr].hi = VA(A_CORE_BSS_HI); nr++;
    rr[nr].lo = VA(A_DATA_SINGLE_LO); rr[nr].hi = VA(A_DATA_SINGLE_HI); nr++;
    uintptr_t wm = *(uintptr_t *)VA(A_WORKMEM_PTR);
    if (wm && mem_readable((void *)wm, WORKMEM_SIZE)) { rr[nr].lo = wm; rr[nr].hi = wm + WORKMEM_SIZE; nr++; }
    int npins = 0;
    uint64_t t0 = qpc();
    int nf = scan_containers(rr, nr, covered_cb, found, 512, pins, 16384, &npins);
    double us = qpc_us(qpc() - t0);
    q1_tag(0, npins, QT_CONT);   /* v0.29.2 Q1COV: the pin sources (only while a coverage window is open) */
    int shown = nf < 512 ? nf : 512;
    int lists = 0, trees = 0, outside = 0;
    for (int i = 0; i < shown; i++) {
        if (found[i].kind == 1) lists++; else trees++;
        if (!found[i].sentinel_covered || found[i].nodes_out) outside++;
    }
    if (g_mode == MODE_RBONLINE)   /* v0.31.0 (§20.3): + the probes the scan made (ReadProcessMemory, VirtualQuery) and the memo's answers */
        LOG("SCAN why=%s containers=%d lists=%d trees=%d with_outside_links=%d pins=%d pin_bytes=%d us=%.0f rpm=%u vq=%u memo=%u\n", why, nf, lists, trees, outside,
            npins, npins * 24, us, g_scan_cost.rpm, g_scan_cost.vq, g_scan_cost.memo);
    else LOG("SCAN why=%s containers=%d lists=%d trees=%d with_outside_links=%d pins=%d pin_bytes=%d us=%.0f\n", why, nf, lists, trees, outside,
        npins, npins * 24, us);
    for (int i = 0; i < shown; i++) {
        char d[96]; describe_va(found[i].obj, d, sizeof d);
        char s[96]; describe_va(found[i].sentinel, s, sizeof s);
        LOG("SCANC kind=%s obj=%s size=%llu sentinel=%s sentinel_covered=%d nodes_seen=%d nodes_out=%d\n",
            found[i].kind == 1 ? "list" : "tree", d, (unsigned long long)found[i].size, s, found[i].sentinel_covered,
            found[i].nodes_seen, found[i].nodes_out);
    }
    if (apply) {
        int q1c0 = npins;   /* v0.29.2 Q1COV */
        /* camera layer buffers (round 4): GT camera object 0x14470ed50 (also *0x14470df28) owns up to 16
         * heap buffers at +0x10; 0x140322d30/0x140322c60 placement-construct layers into them after
         * memset(buf, 0, owner->vtbl[0]()). They are allocated at static init, before our hooks. The
         * size getter is parsed (mov eax, imm32; ret), never called. */
        uintptr_t owner = VA(0x14470ed50ULL);
        uintptr_t vt = *(uintptr_t *)owner;
        uint32_t sz = 0;
        if (IN_IMAGE(vt) && mem_readable((void *)vt, 8)) {
            const uint8_t *f = *(const uint8_t **)vt;
            if (IN_IMAGE(f) && f[0] == 0xb8 && f[5] == 0xc3) memcpy(&sz, f + 1, 4);
        }
        int nb = 0;
        g_ncam_bufs = 0; g_cam_buf_sz = sz && sz < 0x10000 ? sz : 0;
        if (sz && sz < 0x10000) {
            for (int i = 0; i < 16 && npins < 40960 - 1; i++) {
                uintptr_t b = *(uintptr_t *)(owner + 0x10 + 8 * (uintptr_t)i);
                if (!b || !mem_readable((void *)b, sz)) continue;
                if (g_ncam_bufs < 16) g_cam_bufs[g_ncam_bufs++] = b;   /* v0.20.1: named and kept even when another range covers it */
                if (covered_cb(b, sz)) continue;
                pins[npins].addr = b; pins[npins].len = sz; npins++; nb++;
            }
        }
        LOG("SCAN camera_layer_buffers owner_vt_rva=0x%llx size=%u pinned=%d\n", (unsigned long long)(IN_IMAGE(vt) ? RVA(vt) : 0), sz, nb);
        {   /* camera function buffers */
            static uintptr_t bases[32]; int nbase = 0, npin0 = npins;
            uintptr_t rr[3][2] = { { VA(A_CORE_BSS_LO), VA(A_CORE_BSS_HI) }, { VA(A_DATA_SINGLE_LO), VA(A_DATA_SINGLE_HI) }, { 0, 0 } };
            for (int r = 0; r < 2 + g_ncam_bufs; r++) {
                uintptr_t lo2 = r < 2 ? rr[r][0] : g_cam_bufs[r - 2], hi2 = r < 2 ? rr[r][1] : g_cam_bufs[r - 2] + sz;
                for (uintptr_t a = lo2; a + 8 <= hi2; a += 8) {
                    uintptr_t v = *(uintptr_t *)a;
                    if (v < 0x10000 || v > 0x7ffffffeffffULL || (v & 15) || IN_IMAGE(v)) continue;
                    if (!mem_readable((void *)v, 8)) continue;
                    int slot = typed_slot_of(*(uintptr_t *)v);
                    if (slot < 0) continue;
                    uintptr_t base = v - (uintptr_t)slot;
                    int dup = 0;
                    for (int q = 0; q < nbase; q++) if (bases[q] == base) dup = 1;
                    if (dup || nbase >= 32) continue;
                    bases[nbase++] = base;
                }
            }
            /* v0.20.2: the whole block as ONE pin (CHECK.md §5). v0.21.1: whenever the typed-vtable scan found the base and the
             * block is readable. The size 0x41e0 is a code constant (allocation 0x1403234b9 'mov ecx,0x41e0', the game's camera
             * SaveState 0x140324932 'mov r8d,0x41e0'), so no layout check is needed: v0.20.2 also required camera layer buffer 0
             * at base + 0x41e0 + 0x10 (session 2b's heap) and fell back silently to the 4 slot heads otherwise; in the S1 session
             * the heap put layer buffer 0 BEFORE the block, every scan fell back and the throw/outro camera bug returned.
             * The adjacency is still logged (information only). With cam_pin_full=1 a block that cannot be pinned (no base, or
             * unreadable) is a LOUD failure: CAMPIN FAIL, title cam=FAIL, rollback off (hk_step) until a rescan pins it.
             * cam_pin_full=0 pins the 4 slot heads explicitly (the negative control, title cam=slots). */
            uintptr_t layer0 = g_ncam_bufs ? g_cam_bufs[0] : 0;
            int fail_why = g_cfg.cam_pin_full && !nbase ? 1 : 0;   /* 1 no_base, 2 unreadable */
            uintptr_t fail_base = 0;
            for (int b = 0; b < nbase; b++) {
                uintptr_t base = bases[b];
                int readable = mem_readable((void *)base, CAMF_SIZE);
                int adjacent = layer0 && base + CAMF_SIZE + 0x10 == layer0;
                int full = g_cfg.cam_pin_full && readable;
                g_camf_full[b] = (uint8_t)(full ? 1 : g_cfg.cam_pin_full ? 2 : 0);
                if (full) {
                    if (!covered_cb(base, CAMF_SIZE) && npins < 40960 - 1) { pins[npins].addr = base; pins[npins].len = CAMF_SIZE; npins++; }
                    if (!g_cam_pinned_full) { g_cam_pinned_full = 1; g_title_dirty = 1; }
                } else if (!g_cfg.cam_pin_full) {
                    for (int q = 0; q < 4 && npins < 40960 - 1; q++) {
                        uintptr_t pa = base + k_slot_off[q];
                        if (covered_cb(pa, k_slot_len[q]) || !mem_readable((void *)pa, k_slot_len[q])) continue;
                        pins[npins].addr = pa; pins[npins].len = k_slot_len[q]; npins++;
                    }
                } else if (!fail_why) { fail_why = 2; fail_base = base; }
                LOG("SCAN camera_func_block base=0x%llx size=0x%x layer0=0x%llx adjacent=%d mode=%s\n", (unsigned long long)base, CAMF_SIZE,
                    (unsigned long long)layer0, adjacent, full ? "full" : g_cfg.cam_pin_full ? "fail" : "slots");
            }
            if (!nbase) LOG("SCAN camera_func_block base=0 size=0x%x layer0=0x%llx adjacent=0 mode=none\n", CAMF_SIZE, (unsigned long long)layer0);
            if (fail_why) {
                g_cam_fail_scans++;
                if (!g_cam_fail) { g_cam_fail = 1; g_cam_fail_steps = 0; g_cam_ok_pending = 0; g_title_dirty = 1; }
                LOG("CAMPIN FAIL why=%s base=0x%llx holder=0x%llx scan=%s seq=%llu fr=%u rollback=off (cam_pin_full=1: the camera function block cannot be pinned whole; "
                    "steps pass straight through, no rollback, until a rescan pins it; title cam=FAIL)\n", fail_why == 1 ? "no_base" : "unreadable",
                    (unsigned long long)fail_base, (unsigned long long)*(uintptr_t *)(VA(A_CAMF_HOLDER) + 8), why, (unsigned long long)g_seq, core_frame());
            } else if (g_cam_fail) {
                g_cam_fail = 0; g_cam_ok_pending = 1; g_cam_fail_oks++; g_title_dirty = 1;
                LOG("CAMPIN ok base=0x%llx scan=%s seq=%llu fr=%u passthrough_steps=%llu (a rescan pinned the camera function block whole; rollback resumes on a fresh ring)\n",
                    (unsigned long long)(nbase ? bases[0] : 0), why, (unsigned long long)g_seq, core_frame(), (unsigned long long)g_cam_fail_steps);
            }
            g_ncamf = nbase; memcpy(g_camf_base, bases, sizeof(uintptr_t) * (size_t)nbase);
            LOG("SCAN camera_func_buffers=%d pins_added=%d\n", nbase, npins - npin0);
            {   /* v0.20.1: addresses, so heap DIFF / residue lines can be mapped to camera blocks */
                char cb[16 * 20 + 32 * 20 + 64]; size_t o = 0;
                for (int i = 0; i < g_ncam_bufs; i++) o += (size_t)snprintf(cb + o, sizeof cb - o, "%s%llx", i ? "," : "", (unsigned long long)g_cam_bufs[i]);
                o += (size_t)snprintf(cb + o, sizeof cb - o, " func_bases=");
                for (int i = 0; i < nbase; i++) o += (size_t)snprintf(cb + o, sizeof cb - o, "%s%llx", i ? "," : "", (unsigned long long)bases[i]);
                LOG("SCAN camera_blocks layer_size=%u layers=%s\n", g_cam_buf_sz, cb);
            }
        }
        q1_tag(q1c0, npins, QT_CAM);   /* v0.29.2 Q1COV */
        int nx = 0, q1x0 = npins;
        for (int i = 0; i < g_nxpins && npins < 40960 - 1; i++)
            if (!covered_cb(g_xpins[i].addr, g_xpins[i].len) && mem_readable((void *)g_xpins[i].addr, g_xpins[i].len)) { pins[npins++] = g_xpins[i]; nx++; }
        if (g_nxpins) LOG("SCAN extw_extra_pins=%d added=%d\n", g_nxpins, nx);
        q1_tag(q1x0, npins, QT_PIN);   /* v0.29.2 Q1COV */
    }
    int q1t0 = npins;   /* v0.29.2 Q1COV */
    if (apply && g_cfg.track_init && !q1_late_rule()) {   /* v0.29.2: + !q1_late_rule() (rbonline forces track_init=0; rbreplay rb_init_pins=0 pins the late blocks below instead) */
        /* add live blocks allocated inside Init/Setup that are not already covered */
        static SPin tracked[24576];
        uint64_t tb = 0;
        int nt = itrack_list(tracked, 24576, &tb), added = 0;
        uint64_t ab = 0;
        for (int i = 0; i < nt && npins < 40960 - 1; i++) {
            if (covered_cb(tracked[i].addr, tracked[i].len)) continue;
            if (!mem_readable((void *)tracked[i].addr, tracked[i].len)) continue;
            pins[npins++] = tracked[i]; added++; ab += tracked[i].len;
        }
        LOG("SCAN tracked_init_blocks=%d bytes=%llu added_as_pins=%d added_bytes=%llu\n", nt, (unsigned long long)tb, added, (unsigned long long)ab);
    }
    q1_tag(q1t0, npins, QT_PIN);   /* v0.29.2 Q1COV */
    int q1_failed = 0;   /* v0.29.2 (§16.3, §16.6); v0.29.3: + the late pins that do not fit (m4) and a gated walk while the core runs (m3) */
    if (apply) g_q1_scan_fail = 0;   /* v0.29.3: this scan's Q1 verdict (read by ss_build in rbonline); was reset inside the walk, after the late pins */
    if (apply && q1_late_rule() && g_q1_armed) {   /* v0.29.2 Q1b (§16.4) */
        int l0 = npins, lmax = 40960 - 1;
#ifdef SC6RB_HARNESS
        if (g_q1_h_room_late >= 0 && npins + g_q1_h_room_late < lmax) lmax = npins + g_q1_h_room_late;   /* v0.29.3: IQ1QCAP (m4) */
#endif
        if (q1_late_pins(pins, &npins, lmax) > 0) {   /* v0.29.3 (m4): never a silent partial set */
            q1_failed = 1; g_q1_scan_fail = 1; snprintf(g_q1_fail, sizeof g_q1_fail, "late:bound");
            if (!g_q1_fail_bat[0]) snprintf(g_q1_fail_bat, sizeof g_q1_fail_bat, "late");
            g_title_dirty = 1;
        }
        q1_tag(l0, npins, QT_LATE);
    }
    int q1s0 = npins;   /* v0.29.2 Q1COV */
    if (g_cfg.static_learn) sw_add_pins(pins, &npins, 40960 - 1);
    wsx_add_pins(pins, &npins, 40960 - 1);
    an_add_pins(pins, &npins, 40960 - 1);
    q1_tag(q1s0, npins, QT_PIN);   /* v0.29.2 Q1COV */
    int q1g0 = npins;   /* v0.29.2 Q1COV */
    if (g_cfg.gs_pins) {
        GsStat st; memset(&st, 0, sizeof st);
        gs_collect(pins, &npins, 40960 - 1, &st);
        LOG("GSPINS why=%s khit=%d area=%d fixarea=%d sphere=%d base=%d unknown_class=%d stage_entries=%d dee8=%d already_covered=%d pinned=%d bytes=%llu applied=%d\n",
            why, st.khit, st.area, st.fix, st.sphere, st.base, st.unknown, st.stage_n, st.dee8, st.covered, st.pinned, (unsigned long long)st.bytes, apply);
    }
    q1_tag(q1g0, npins, QT_GS);   /* v0.29.2 Q1COV */
    /* v0.29.2 (§16.3, §16.6): the root walk, at every apply scan from ARM on (rbonline; rbreplay q1_roots=1).
     * rbonline: q1_walk_scan's own gate (ARMED / DRAINED / inside the ARM) decides, a call outside it counts a violation and reads nothing */
    if (apply && g_q1_walk_on && (g_mode == MODE_RBONLINE || (g_mode == MODE_RBREPLAY && g_q1_armed))) {
        int wmax = 40960 - 1;
#ifdef SC6RB_HARNESS
        if (g_q1_h_room >= 0 && npins + g_q1_h_room < wmax) wmax = npins + g_q1_h_room;   /* v0.29.3: Q1CAP (m4) */
#endif
        int wr = q1_walk_scan(why, pins, &npins, wmax, found, shown, nf);   /* v0.29.4 Q1: + this scan's census (E7); v0.29.5 Q1R (m4): + nf */
        if (wr == 0) q1_failed = 1;
        else if (wr < 0 && g_mode == MODE_RBONLINE && !Q1H_OLD_GATE) {   /* v0.29.3 (review m3): the gate refused the walk */
            int rbon = rb_on();
#ifdef SC6RB_HARNESS
            if (g_q1_h_rbon >= 0) rbon = g_q1_h_rbon;
#endif
            if (rbon) {   /* the rb core runs on whatever this scan applies: a set without the Q1 pins is a silent partial set. Fail closed */
                RboState st = rbo_state();
#ifdef SC6RB_HARNESS
                if (g_q1_h_state >= 0) st = (RboState)g_q1_h_state;
#endif
                LOG("Q1 WALK GATED b=%u f=%u why=%s state=%s rb=on (an apply scan while the core runs, outside the walk's gate: treated as a failed walk)\n",
                    q1_battle(), c1a_next_f(), why, k_rbo_state[st < RBO_NSTATE ? st : 0]);
                q1_failed = 1; g_q1_scan_fail = 1; snprintf(g_q1_fail, sizeof g_q1_fail, "gate:%s", k_rbo_state[st < RBO_NSTATE ? st : 0]);
                g_title_dirty = 1;
            }
        }
    }
#ifdef SC6RB_HARNESS
    if (q1_failed) InterlockedIncrement(&rbo_h_q1_scan_fails);
#endif
    if (apply) {
        int st_old = g_ss.npin, st_app = 0, st_van = 0;
        spin_lock(&g_pin_lock);
        if (g_ss.stable) { if (!snapset_merge_pins(&g_ss, pins, npins, &st_app, &st_van)) LOG("RBPINS merge_failed why=%s (out of memory)\n", why); }   /* v0.23.0-C1b */
        else snapset_set_pins(&g_ss, pins, npins);
        if (g_mode == MODE_RBONLINE) rb_pinf_rebuild(&g_ss);   /* v0.31.0 (§20.3): the step thread's pin filter (under the pin lock: rb.c) */
        g_npins = g_ss.npin;
        g_pins_freed = 0; g_pins_freed_n = 0;
        spin_unlock(&g_pin_lock);
        if (g_ss.stable) rb_note_extend(st_old, g_ss.npin, st_app, st_van);
        LOG("SCAN pinset why=%s collected=%d pins=%d covered_dropped=%d dropped_bytes=%zu pin_bytes=%zu (v0.20.2: a pin wholly inside another pin is kept once)\n", why, npins,
            g_ss.npin, g_ss.pins_dropped, g_ss.pins_dropped_bytes, g_ss.pin_bytes);
        s1b_pin_checks(why);   /* v0.24.0-C1b2 S1BFIX G2 / G3 */
        for (int b = 0; b < g_ncamf; b++) {   /* v0.20.2: say so when the whole-block pin is not its own pin */
            if (g_camf_full[b] != 1) continue;
            int k = pin_index_of(g_camf_base[b]);
            if (k < 0) LOG("CAMPIN block_not_a_pin base=0x%llx (covered by a region or the arena)\n", (unsigned long long)g_camf_base[b]);
            else if (g_ss.pins[k].addr != g_camf_base[b] || g_ss.pins[k].len != CAMF_SIZE)
                LOG("CAMPIN covered base=0x%llx by_pin=0x%llx len=0x%x (the block lies inside a larger pin; that pin restores it)\n", (unsigned long long)g_camf_base[b],
                    (unsigned long long)g_ss.pins[k].addr, g_ss.pins[k].len);
        }
        if (g_qc_active) { qc_note_pins(pins, npins); qc_eval_all(); qc_note_census(found, shown); }   /* v0.29.2 Q1COV; v0.29.4 Q1: + the census (owner names) */
    }
    if (!census_skip(why)) census_r11(why);   /* v0.31.0: rbonline's rescans skip the census (it pins nothing) */
    if (q1_failed) {   /* v0.29.2 (§16.6): never a silent partial set. rbonline: ARM refuses the set (ss_build), a rescan aborts the core; rbreplay: RBABORT */
        if (g_mode == MODE_RBONLINE) { if (!g_q1_arming && !Q1H_NO_RESCAN_ABORT) rb_abort_ext("q1_walk"); }   /* v0.29.3: + the XQ1RESCAN control switch (harness only) */
        else rb_q1_walk_failed();
    }
}

static int ss_build(const char *why) {
    for (int i = 0; i < RING_MAX; i++) g_ring[i].valid = 0;
    spin_lock(&g_pin_lock);
    snapset_clear_pins(&g_ss);
    g_npins = 0;
    spin_unlock(&g_pin_lock);
    SPin *keep_pins = g_ss.pins; int keep_cap = g_ss.pincap; uint8_t *keep_scr = g_ss.pin_scratch; size_t keep_scr_cap = g_ss.pin_scratch_cap;
    /* v0.23.0-C1b: the stable-layout arrays survive too */
    uint8_t *k_pst = g_ss.pstate, *k_phd = g_ss.phidden; int32_t *k_pdq = g_ss.pdq; int k_pstcap = g_ss.pstcap;
    SPin *k_sp = g_ss.spins; int *k_ss = g_ss.sslot; int k_spcap = g_ss.spcap; uint64_t k_lsk = g_ss.layout_skips;
    spin_lock(&g_pin_lock);
    snapset_init(&g_ss);
    g_ss.pins = keep_pins; g_ss.pincap = keep_cap; g_ss.pin_scratch = keep_scr; g_ss.pin_scratch_cap = keep_scr_cap;
    g_ss.pstate = k_pst; g_ss.phidden = k_phd; g_ss.pdq = k_pdq; g_ss.pstcap = k_pstcap; g_ss.spins = k_sp; g_ss.sslot = k_ss; g_ss.spcap = k_spcap;
    g_ss.layout_skips = k_lsk;
    if (rb_ss_stable()) snapset_stable_reset(&g_ss, rb_layout_gen());   /* v0.23.0-C1b: mode=rbreplay: stable pin slots (rb.c) */
    spin_unlock(&g_pin_lock);
    snapset_add_region(&g_ss, VA(A_CORE_BSS_LO), (size_t)(A_CORE_BSS_HI - A_CORE_BSS_LO), "core_bss");
    snapset_add_region(&g_ss, VA(A_DATA_SINGLE_LO), (size_t)(A_DATA_SINGLE_HI - A_DATA_SINGLE_LO), "single");
    uintptr_t wm = *(uintptr_t *)VA(A_WORKMEM_PTR);
    g_ss_workmem = wm;
    int wm_in_arena = wm && g_arena_ok && arena_owns(&g_arena, (void *)wm);
    if (wm && !wm_in_arena) {
        if (!mem_readable((void *)wm, WORKMEM_SIZE)) { LOG("SS workmem_unreadable %p\n", (void *)wm); return 0; }
        snapset_add_region(&g_ss, wm, WORKMEM_SIZE, "workmem");
    }
    snapset_add_region(&g_ss, (uintptr_t)&g_x, sizeof g_x, "extra");
    {   /* v0.20.2: the throw camera gate (class C), before the ini regions so it always fits */
        int ok = mem_readable((void *)VA(A_CAM_GATE), 4) && snapset_add_region(&g_ss, VA(A_CAM_GATE), 4, "camgate");
        LOG("SS camgate va=0x%llx len=4 class=C added=%d\n", (unsigned long long)A_CAM_GATE, ok);
    }
    for (int i = 0; i < g_cfg.nregion; i++) {
        uintptr_t a = VA(g_cfg.region_va[i]);
        if (mem_readable((void *)a, g_cfg.region_len[i])) snapset_add_region(&g_ss, a, g_cfg.region_len[i], "ini");
    }
    if (g_cfg.rb_mt_tail) {   /* v0.23.0-C1b rb_mt_tail=1: the std::mt19937 tail outside "single" (every mode; LAST, so no region index moves;
                               * outside g_cfg.region_*, so simcfg is unchanged; class O; the C1a dumps and the catch-all skip it by name) */
        int ok = mem_readable((void *)VA(0x144100ee0ULL), 0x1350) && snapset_add_region(&g_ss, VA(0x144100ee0ULL), 0x1350, "mt_tail");
        LOG("SS mt_tail va=0x144100ee0 len=0x1350 added=%d (rb_mt_tail=1)\n", ok);
    }
    if (g_arena_ok) { g_ss.arena = &g_arena; g_ss.arena_lock = (volatile long *)&g_arena_lock; }
    snapset_add_mask(&g_ss, VA(A_LISTENER_PTR), 8);
    snapset_add_mask(&g_ss, VA(A_SCRAPE_CTR0), 4);
    snapset_add_mask(&g_ss, VA(A_SCRAPE_CTR1), 4);
    {   /* v0.21.0-S1: the X masks (per-PC pointer values in PLAYER, pres.c): out of every hash and diff, still restored */
        int nx = 0;
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < k_pres_nx; i++)
                nx += snapset_add_mask(&g_ss, VA(A_PLAYER0) + (uintptr_t)c * PLAYER_STRIDE + k_pres_x[i].lo, k_pres_x[i].hi - k_pres_x[i].lo);
        LOG("SS xmasks=%d of %d (PLAYER+0x3590:8 std::map head, PLAYER+0x2b270:8 lip data pointer; per-PC pointers out of the hashes and diffs, still restored)\n", nx, 2 * k_pres_nx);
    }
    for (int i = 0; i < g_cfg.nmask; i++) snapset_add_mask(&g_ss, VA(g_cfg.mask_va[i]), g_cfg.mask_len[i]);
    int need = g_cfg.roll_depth + 2;
    if (g_cfg.fake_delay + 2 > need) need = g_cfg.fake_delay + 2;
    if (need < 8) need = 8;
    if (need > RING_MAX) need = RING_MAX;
    g_R = need;
    g_ss_ready = 1;
    if (g_ss.stable) rb_note_rebuild(why);
    int q1_arm_here = g_q1_rb && !g_q1_armed && g_cfg.rb_arm_f == 0 && g_mode == MODE_RBREPLAY;   /* v0.29.2: rbreplay's ARM = the battle's first build */
    if (q1_arm_here) q1_arm_begin(why);
    if (g_cfg.pin_containers) do_pin_scan(why, 1);
    if (q1_arm_here) g_q1_arming = 0;
    if (g_mode == MODE_RBONLINE && g_q1_scan_fail) {   /* v0.29.2 (§16.6): a gameplay entry failed: never a partial snapshot set */
        LOG("SS refused why=%s q1=%s (the Q1 root walk failed a gameplay entry: no snapshot set)\n", why, g_q1_fail);
        g_ss_ready = 0;
        return 0;
    }
    seg_build();
    LOG("SS built why=%s regions=%d workmem=%p wm_in_arena=%d arena_used=%zu pins=%d masks=%d bytes=%zu ring=%d\n", why, g_ss.nreg,
        (void *)wm, wm_in_arena, arena_used(&g_arena), g_ss.npin, g_ss.nmask, snap_bytes(&g_ss), g_R);
    return 1;
}

/* v0.20.0 fix R1: a pinned block freed inside a listener call (immediate: UE runs outside the step context)
 * while this frame still has restores to do. The frame's resims are skipped or aborted and only regions and
 * the arena are restored (never pins); pins_process_freed removes the pin and clears the ring next tick. */
static uint64_t g_pinfree_incall;
static void pinfree_incall(uint64_t s, const char *pass, uint64_t k) {
    g_pinfree_incall++;
    if (g_pinfree_incall <= 200)
        LOG("PINFREE_INCALL seq=%llu fr=%u pass=%s k=%llu slot=%s%03x freed=%ld first=%p (a pinned block was freed inside a listener call: this frame's resims are skipped, pins are not restored)\n",
            (unsigned long long)s, core_frame(), pass, (unsigned long long)k, g_pinfree_slot >= 0 ? "+0x" : "none:", g_pinfree_slot >= 0 ? g_pinfree_slot * 8 : 0,
            (long)g_pins_freed_n, (void *)g_pins_freed_list[0]);
}
static void pins_process_freed(void) {
    if (!g_pins_freed) return;
    spin_lock(&g_pin_lock);
    LONG n = g_pins_freed_n; if (n > 256) n = 256;
    int removed = 0;
    for (LONG i = 0; i < n; i++) removed += snapset_remove_pins_in(&g_ss, g_pins_freed_list[i], g_pins_freed_list[i] + 1);
    g_npins = g_ss.npin;
    if (g_pins_freed_n > 256) { snapset_clear_pins(&g_ss); g_npins = 0; }
    g_pins_freed = 0; g_pins_freed_n = 0;
    spin_unlock(&g_pin_lock);
    LOG("PINS freed=%ld removed=%d left=%d\n", n, removed, g_npins);
    ring_clear("pin_freed");
}

/* ------------------------------------------------------------------ inputs */
static void fi_capture(FrameIn *f, const StepIn *in) {
    memset(f, 0, sizeof *f);
    if (in->p1) { memcpy(&f->p1, in->p1, 8); f->flags |= 1; }
    if (in->p2) { memcpy(&f->p2, in->p2, 8); f->flags |= 2; }
    if (in->extra) { memcpy(f->extra, in->extra, 0x18); f->flags |= 4; }
}
static void fi_make(StepIn *in, FrameIn *f) {
    in->p1 = (f->flags & 1) ? (uint32_t *)&f->p1 : NULL;
    in->p2 = (f->flags & 2) ? (uint32_t *)&f->p2 : NULL;
    in->extra = (f->flags & 4) ? f->extra : NULL;
}
static int extra_nonzero(const FrameIn *f) {
    for (int i = 0; i < 0x18; i++) if (f->extra[i]) return 1;
    return 0;
}
static inline uint64_t word_of(const FrameIn *f, int pl) { return pl == 1 ? f->p1 : f->p2; }
static inline void set_word(FrameIn *f, int pl, uint64_t w) { if (pl == 1) { f->p1 = w; f->flags |= 1; } else { f->p2 = w; f->flags |= 2; } }
static inline uint64_t predict(uint64_t prev_used, uint64_t base) {
    uint32_t held = (uint32_t)base, prev = (uint32_t)prev_used;
    return (uint64_t)held | ((uint64_t)(held & ~prev) << 32);
}

/* ------------------------------------------------------------------ listener record/replay hooks */
static int win_ok(uint64_t p) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uintptr_t lo = (uintptr_t)tib->StackLimit, hi = (uintptr_t)tib->StackBase;
    if (p >= lo && p + LREC_WIN <= hi) return 1;
    if (p >= VA(A_CORE_BSS_LO) && p + LREC_WIN <= VA(A_CORE_BSS_HI)) return 1;
    if (g_arena_ok && arena_owns(&g_arena, (void *)p) && arena_owns(&g_arena, (void *)(p + LREC_WIN - 1))) return 1;
    return 0;
}

static LRec *lrec_alloc(void) { return (LRec *)VirtualAlloc(NULL, sizeof(LRec), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

static int lsn_pre_v19(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp) {
    if (g_ctx_flags) { fp_lsn_pre(); g_fp_slot = slot; if (g_cfg.ws_lsn_attrib && g_ws_active && g_wsl_depth++ == 0) ws_lsn_pre(); }
    LRec *r = g_lrec_cur;
    if (!r) return 0;
    if (g_lrec_mode == 1) {                      /* record: remember the window before the call */
        g_lrec_depth++;
        g_lrec_before_ok = g_lrec_depth == 1 && win_ok(rdx);
        if (g_lrec_before_ok) memcpy(g_lrec_before, (void *)rdx, LREC_WIN);
        if (g_lsnw_on && g_lrec_depth == 1 && g_lsnw_buf) memcpy(g_lsnw_buf, (void *)VA(A_CORE_BSS_LO), (size_t)(A_CORE_BSS_HI - A_CORE_BSS_LO));
        g_stk_ok = g_cfg.stack_outparams && g_lrec_depth == 1 && stk_ok(crsp);
        if (g_stk_ok) memcpy(g_stk_before, (void *)crsp, STKWIN);
        return 0;
    }
    if (g_lrec_mode == 3) {                      /* muted test pass: replay queries, drop notifications, never call UE */
        int q = slot == 0x138 / 8 || slot == 0x140 / 8;
        LCall *c = (g_lrec_diverged < 0 && g_lrec_idx < r->n && r->c[g_lrec_idx].slot == (uint8_t)slot) ? &r->c[g_lrec_idx] : NULL;
        if (!c && g_lrec_diverged < 0) g_lrec_diverged = g_lrec_idx;
        g_lrec_idx++;
        if (q && c) {
            for (uint32_t o = c->lw_off; o + 12 <= c->lw_off + c->lw_len;) {
                uintptr_t a; uint32_t n;
                memcpy(&a, r->pool + o, 8); memcpy(&n, r->pool + o + 8, 4);
                memcpy((void *)a, r->pool + o + 12, n);
                o += 12 + n;
            }
            apply_stack_runs(r, c, crsp);
            res[0] = c->rax; res[1] = c->xmm0;
        } else if (q) { res[0] = slot == 0x138 / 8 ? 1 : 0; res[1] = 0; }   /* NullListener answers */
        else { res[0] = 0; res[1] = 0; }
        return 1;
    }
    if (g_lrec_mode == 2) {                      /* replay */
        if (g_lrec_diverged >= 0) return 0;
        if (g_lrec_idx >= r->n || r->c[g_lrec_idx].slot != (uint8_t)slot) {
            g_lrec_diverged = g_lrec_idx;
            g_lrec_divergences++;
            return 0;                            /* fall back to the configured (muted) target */
        }
        LCall *c = &r->c[g_lrec_idx++];
        if (c->has_win && c->nchg && win_ok(rdx)) {
            uint8_t *w = (uint8_t *)rdx;
            for (int i = 0; i < LREC_WIN; i++) if (c->mask[i >> 3] & (1 << (i & 7))) w[i] = c->after[i];
        }
        for (uint32_t o = c->lw_off; o + 12 <= c->lw_off + c->lw_len;) {
            uintptr_t a; uint32_t n;
            memcpy(&a, r->pool + o, 8); memcpy(&n, r->pool + o + 8, 4);
            memcpy((void *)a, r->pool + o + 12, n);
            o += 12 + n; g_lw_replayed++;
        }
        apply_stack_runs(r, c, crsp);
        res[0] = c->rax; res[1] = c->xmm0;
        g_lrec_replayed++;
        return 1;
    }
    return 0;
}

static int lsn_post_v19(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp) {
    LRec *r = g_lrec_cur;
    if (!r || g_lrec_mode != 1) return 0;
    int top = g_lrec_depth == 1;
    if (g_lrec_depth > 0) g_lrec_depth--;
    if (!top) return 0;
    if (r->n >= LREC_MAXCALLS) { r->overflow++; return 0; }
    LCall *c = &r->c[r->n++];
    memset(c, 0, offsetof(LCall, mask) + sizeof c->mask);
    c->slot = (uint8_t)slot; c->rax = res[0]; c->xmm0 = res[1];
    if (g_lrec_before_ok && win_ok(rdx)) {
        const uint8_t *w = (const uint8_t *)rdx;
        c->has_win = 1;
        for (int i = 0; i < LREC_WIN; i++)
            if (w[i] != g_lrec_before[i]) { c->mask[i >> 3] |= (uint8_t)(1 << (i & 7)); c->after[i] = w[i]; c->nchg++; }
        if (c->nchg) g_lrec_outparam_calls++;
    }
    if (g_lsnw_on && g_lsnw_buf) {   /* learn: which core .bss bytes did this listener call change? */
        const uint8_t *now = (const uint8_t *)VA(A_CORE_BSS_LO), *was = g_lsnw_buf;
        size_t len = (size_t)(A_CORE_BSS_HI - A_CORE_BSS_LO);
        uintptr_t lst = VA(A_LISTENER_PTR) - VA(A_CORE_BSS_LO);
        g_lsnw_calls++;
        for (size_t off = 0; off < len; off += 64) {
            size_t cl = len - off < 64 ? len - off : 64;
            if (!memcmp(now + off, was + off, cl)) continue;
            size_t i0 = off, i1 = off + cl;
            while (i0 < i1 && now[i0] == was[i0]) i0++;
            while (i1 > i0 && now[i1 - 1] == was[i1 - 1]) i1--;
            if (i0 <= lst + 8 && i1 >= lst && i1 - i0 <= 16) continue;   /* our own proxy swap */
            i0 &= ~(size_t)15; i1 = (i1 + 15) & ~(size_t)15; if (i1 > len) i1 = len;   /* whole 16-byte granules: a field's other bytes change later */
            uintptr_t a = VA(A_CORE_BSS_LO) + i0;
            int k = 0;
            for (; k < g_nlw; k++)
                if (g_lw[k].slot == (uint8_t)slot && a <= g_lw[k].addr + g_lw[k].len + 16 && a + (i1 - i0) + 16 >= g_lw[k].addr) {
                    uintptr_t lo2 = a < g_lw[k].addr ? a : g_lw[k].addr, hi2 = a + (i1 - i0) > g_lw[k].addr + g_lw[k].len ? a + (i1 - i0) : g_lw[k].addr + g_lw[k].len;
                    g_lw[k].addr = lo2; g_lw[k].len = (uint32_t)(hi2 - lo2); g_lw[k].hits++;
                    break;
                }
            if (k == g_nlw && g_nlw < LW_MAX) { g_lw[g_nlw].slot = (uint8_t)slot; g_lw[g_nlw].addr = a; g_lw[g_nlw].len = (uint32_t)(i1 - i0); g_lw[g_nlw].hits = 1; g_nlw++; }
        }
    }
    c->sk_off = r->pool_used; c->sk_len = 0;
    if (g_stk_ok && stk_ok(crsp)) {
        const uint8_t *now = (const uint8_t *)crsp;
        /* skip the 0x20-byte home area (callee-owned scratch); keep whole 8-byte granules so a field
         * whose other bytes happened to stay equal is still replayed whole */
        for (uint32_t i = 0x20; i < STKWIN;) {
            if (!memcmp(now + i, g_stk_before + i, 8)) { i += 8; continue; }
            uint32_t j = i + 8;
            while (j < STKWIN && memcmp(now + j, g_stk_before + j, 8)) j += 8;
            uint32_t n = j - i;
            if (r->pool_used + 8 + n > LREC_POOL) { r->pool_overflow++; break; }
            memcpy(r->pool + r->pool_used, &i, 4); memcpy(r->pool + r->pool_used + 4, &n, 4);
            memcpy(r->pool + r->pool_used + 8, now + i, n);
            r->pool_used += 8 + n; c->sk_len += 8 + n; g_stk_bytes += n;
            i = j;
        }
        if (c->sk_len) {
            static uint8_t seen[64];
            g_stk_calls++;
            if (slot >= 0 && slot < 64 && !seen[slot]) {
                uint32_t off0, n0; memcpy(&off0, r->pool + c->sk_off, 4); memcpy(&n0, r->pool + c->sk_off + 4, 4);
                seen[slot] = 1;
                LOG("LSNSTK slot=+0x%03x first_run_off=0x%x len=%u total_run_bytes=%u seq=%llu (listener call changed the caller's stack frame)\n",
                    slot * 8, off0, n0, c->sk_len, (unsigned long long)g_cur_seq);
            }
        }
    }
    g_stk_ok = 0;
    c->lw_off = r->pool_used; c->lw_len = 0;
    for (int k = 0; k < g_nlw; k++) {   /* record the learned windows of this slot as they are now */
        if (g_lw[k].slot != (uint8_t)slot) continue;
        uint32_t n = g_lw[k].len;
        if (r->pool_used + 12 + n > LREC_POOL) { r->pool_overflow++; break; }
        memcpy(r->pool + r->pool_used, &g_lw[k].addr, 8); memcpy(r->pool + r->pool_used + 8, &n, 4);
        memcpy(r->pool + r->pool_used + 12, (void *)g_lw[k].addr, n);
        r->pool_used += 12 + n; c->lw_len += 12 + n;
    }
    return 0;
}

static void lsnw_report(void) {
    LOG("LSNW report calls=%llu learned_windows=%d (core .bss bytes UE changed during live listener calls)\n", (unsigned long long)g_lsnw_calls, g_nlw);
    for (int k = 0; k < g_nlw && k < 120; k++) {
        char d[96]; describe_va(g_lw[k].addr, d, sizeof d);
        uint64_t rel; const char *fn = field_name(g_lw[k].addr, &rel);
        LOG("LSNWW slot=+0x%03x at=%s len=%u hits=%u field=%s+0x%llx class=%c\n", g_lw[k].slot * 8, d, g_lw[k].len, g_lw[k].hits, fn,
            (unsigned long long)rel, k_cl_char[class_of_va(g_lw[k].addr - g_base + IMG_PREF) < 0 ? CL_OTHER : class_of_va(g_lw[k].addr - g_base + IMG_PREF)]);
    }
}

/* ------------------------------------------------------------------ v0.20.0 L4: the canonical listener (GATE-C-DESIGN §3.6)
 * With l4=1, every listener call the plugin forwards to UE (live notifications; resim notifications
 * too with r7_forward=1; slot 0, the destructor, in any pass) is made invisible to the simulation, by
 * construction rather than by a learned list:
 *   - core .bss 0x14470d000..0x14485f000 (both PLAYER blocks incl. +0x972a0/+0x295d0/+0x95ed0/+0x2b270,
 *     the e9b0 block, TT800) is copied before the call and every changed 64-byte chunk is put back;
 *     only the listener pointer is exempt (a new listener installed by UE is kept, as in v0.19);
 *   - the 128-byte window at rdx when rdx points into fight state outside core .bss (stack, arena,
 *     WorkMemory, a pinned block): slot +0x100's out-param struct;
 *   - the caller's frame [rsp, rsp+0x400) at the call: stack out-params (the +0x108/+0x140 kind);
 *   - the result is canonical: rax = 0, xmm0 = 0 (what a muted call returns).
 * The queries +0x138/+0x140 get the NullListener answers in every pass (lp_configure) and never reach UE.
 * L4MISS (watchdog, l4_miss=1 live passes, 2 every pass): the whole snapshot set is copied before the
 * call and diffed after the revert; whatever still differs is a UE write into fight state outside the
 * windows. It is logged and NOT reverted (production has no watchdog, so C0 must see its effect).
 * L4REENTRY: an IBattleCore entry (census thunk, Step, Init, Setup, Shutdown) while a forwarded call is
 * in progress. Only the step thread's calls are handled; calls through the proxy from other threads
 * are counted (other_thread) and left alone. */
#define CORE_BSS_LEN ((size_t)(A_CORE_BSS_HI - A_CORE_BSS_LO))
static uint8_t *g_l4_buf;
static DWORD g_l4_tid;
static int g_l4_depth, g_l4_top_fwd, g_l4_top_slot, g_l4_pass_resim;
static int g_l4_late;   /* v0.33.0 (OPTA-M2-SPEC.md §22.1): a late presentation delivery between passes (modes_lsn_deliver): L4 guards it like a forwarded live call */
static volatile LONG g_l4f_n;   /* fix R2: frees during the current call (defined with l4_note_heap_free) */
volatile int g_l4_in_call;   /* v0.20.0: extern in common.h (fix R2: mem.c reports frees made during a call) */
static uint64_t g_l4_miss_pins, g_l4_miss_arena; static int g_l4_miss_heap_lines;   /* fix C2: L4MISS in heap fight state */
static uint32_t g_l4_re_pair[LISTENER_NSLOTS][IFC_NSLOTS];                         /* fix C1: re-entry pairs */
static uint64_t g_l4_rdx; static uint32_t g_l4_rdx_len; static int g_l4_rdx_kind; static uint8_t g_l4_rdx_before[LREC_WIN];   /* kind: 1 stack, 2 arena, 3 workmem, 4 pin */
static LONG g_l4_freed0;              /* pinned-block frees seen before the call (a free during the call disables pin reads) */
static uint64_t g_l4_miss_skipped;
static uint64_t g_l4_crsp; static int g_l4_stk; static uint8_t g_l4_stk_before[STKWIN];
static uintptr_t g_l4_lst_before;
static Snap g_l4_snap; static int g_l4_snap_ok;
static uint64_t g_l4_calls[2], g_l4_rev_calls[2], g_l4_rev_bytes[2], g_l4_win_rev, g_l4_stk_rev, g_l4_lst_kept, g_l4_nobuf;
static uint64_t g_l4_miss_checked, g_l4_miss_calls, g_l4_miss_ranges, g_l4_miss_bytes, g_l4_reentry, g_l4_other_thread, g_l4_depth_leak;
static uint64_t g_l4_frame_rev, g_l4_frame_miss;
static double g_l4_us, g_l4_us_max, g_l4_miss_us, g_l4_miss_us_max, g_l4_t_pre;
static uint64_t g_l4_call_t0, g_l4_big_calls; static double g_l4_big_us, g_l4_big_us_max;   /* v0.31.0: the calls past L4F_MAX frees (rbonline: RB EDGE) */
void modes_l4_times(double *l4_us, double *l4miss_us) { *l4_us = g_l4_us; *l4miss_us = g_l4_miss_us; }   /* C2 (§5.3): the cumulative L4 / L4MISS times */
static int g_l4_miss_lines, g_l4_re_lines;
static LWin g_l4w[LW_MAX]; static int g_nl4w;
static void l4w_note(int slot, uintptr_t a, size_t len) {
    for (int k = 0; k < g_nl4w; k++)
        if (g_l4w[k].slot == (uint8_t)slot && a <= g_l4w[k].addr + g_l4w[k].len + 16 && a + len + 16 >= g_l4w[k].addr) {
            uintptr_t lo2 = a < g_l4w[k].addr ? a : g_l4w[k].addr, hi2 = a + len > g_l4w[k].addr + g_l4w[k].len ? a + len : g_l4w[k].addr + g_l4w[k].len;
            g_l4w[k].addr = lo2; g_l4w[k].len = (uint32_t)(hi2 - lo2); g_l4w[k].hits++;
            return;
        }
    if (g_nl4w < LW_MAX) { g_l4w[g_nl4w].slot = (uint8_t)slot; g_l4w[g_nl4w].addr = a; g_l4w[g_nl4w].len = (uint32_t)len; g_l4w[g_nl4w].hits = 1; g_nl4w++; }
}
/* bytes of the rdx window to protect: fight state outside core .bss only (core .bss is reverted whole) */
static uint32_t l4_rdx_window(uint64_t p) {
    if (p < 0x10000) return 0;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uintptr_t lo = (uintptr_t)tib->StackLimit, hi = (uintptr_t)tib->StackBase;
    g_l4_rdx_kind = 0;
    if (p >= lo && p < hi) { g_l4_rdx_kind = 1; return (uint32_t)(hi - p < LREC_WIN ? hi - p : LREC_WIN); }
    if (p >= VA(A_CORE_BSS_LO) && p < VA(A_CORE_BSS_HI)) return 0;
    if (g_arena_ok && arena_owns(&g_arena, (void *)p) && arena_owns(&g_arena, (void *)(p + LREC_WIN - 1))) { g_l4_rdx_kind = 2; return LREC_WIN; }
    if (g_ss_workmem && p >= g_ss_workmem && p + LREC_WIN <= g_ss_workmem + WORKMEM_SIZE) { g_l4_rdx_kind = 3; return LREC_WIN; }
    if (g_ss_ready) {
        int k = pin_index_of((uintptr_t)p);
        if (k >= 0) { size_t avail = g_ss.pins[k].addr + g_ss.pins[k].len - p; g_l4_rdx_kind = 4; return (uint32_t)(avail < LREC_WIN ? avail : LREC_WIN); }
    }
    return 0;
}
static void l4_pre(int slot, uint64_t rdx, uint64_t crsp) {
    if (!g_l4 || (!g_ctx_flags && !g_l4_late)) return;   /* v0.33.0: + the late delivery (no step context: UE runs as through the proxy) */
    if (GetCurrentThreadId() != g_l4_tid) { g_l4_other_thread++; return; }
    if (g_l4_depth++ != 0) return;              /* nested call: the outer call's revert covers it */
    g_l4_top_slot = slot;
    g_l4_top_fwd = g_l4_late || (slot >= 0 && slot < LISTENER_NSLOTS && g_lp_fwd[slot]);   /* v0.33.0: a late delivery always reaches UE */
    if (!g_l4_top_fwd) return;
    if (!g_l4_buf) { g_l4_top_fwd = 0; if (g_l4_nobuf++ == 0) LOG("L4 WARN no revert buffer: calls are NOT reverted\n"); return; }
    uint64_t t = qpc();
    int ri = g_l4_pass_resim;
    g_l4_calls[ri]++;
    memcpy(g_l4_buf, (void *)VA(A_CORE_BSS_LO), CORE_BSS_LEN);
    g_l4_lst_before = *(volatile uintptr_t *)VA(A_LISTENER_PTR);
    g_l4_freed0 = g_pins_freed_n;
    g_l4f_n = 0;   /* fix R2: frees made during this call */
    if (g_l4 == 1) {   /* the out-param windows are only needed for the revert (v0.21.0-S1: the guard diffs core .bss only) */
        g_l4_rdx = rdx; g_l4_rdx_len = l4_rdx_window(rdx);
        if (g_l4_rdx_len) memcpy(g_l4_rdx_before, (void *)rdx, g_l4_rdx_len);
        g_l4_crsp = crsp; g_l4_stk = stk_ok(crsp);
        if (g_l4_stk) memcpy(g_l4_stk_before, (void *)crsp, STKWIN);
    } else { g_l4_rdx = rdx; g_l4_rdx_len = 0; g_l4_rdx_kind = 0; g_l4_crsp = crsp; g_l4_stk = 0; }
    g_l4_t_pre = qpc_us(qpc() - t);
    g_l4_snap_ok = 0;
    /* recheck fix: once a pinned block was freed (g_pins_freed), snap_take would memcpy the freed pin */
    if (g_ss_ready && !g_pins_freed && (g_cfg.l4_miss == 2 || (g_cfg.l4_miss == 1 && !ri))) {
        uint64_t tm = qpc();
        g_l4_snap_ok = snap_take(&g_ss, &g_l4_snap);
        g_l4_miss_us += qpc_us(qpc() - tm);
    }
    g_l4_call_t0 = qpc();   /* v0.31.0 */
    g_l4_in_call = 1;
}
typedef struct { int slot; } L4Ctx;
static void l4_span(void *u, size_t off, size_t len) {
    uintptr_t a = VA(A_CORE_BSS_LO) + off, lst = VA(A_LISTENER_PTR);
    if (a >= lst && a + len <= lst + 8) return;   /* the listener pointer (kept, counted separately) */
    l4w_note(((L4Ctx *)u)->slot, a & ~(uintptr_t)15, ((a + len + 15) & ~(uintptr_t)15) - (a & ~(uintptr_t)15));
}
typedef struct { int slot, ri, n; uint64_t bytes; } MissCtx;
static void l4_miss_cb(void *u, int region, size_t off, size_t len, uintptr_t addr) {
    MissCtx *m = (MissCtx *)u;
    const char *rn = region >= 0 ? g_ss.reg[region].name : region == -1 ? "arena" : "pins";
    if (region >= 0 && !strcmp(rn, "core_bss")) return;   /* reverted whole (only the kept listener pointer can remain, and it is masked) */
    if (region == -2) {   /* pins: map the blob offset back to an address */
        size_t acc = 0; int i = 0;
        for (; i < g_ss.npin && acc + g_ss.pins[i].len <= off; i++) acc += g_ss.pins[i].len;
        addr = i < g_ss.npin ? g_ss.pins[i].addr + (off - acc) : 0;
    }
    m->n++; m->bytes += len;
    int heap = region == -1 || region == -2;   /* fix C2: pins and arena (heap fight state) are counted and always listed */
    if (region == -2) g_l4_miss_pins++; else if (region == -1) g_l4_miss_arena++;
    if (heap ? g_l4_miss_heap_lines++ < 60 : g_l4_miss_lines++ < 60) {
        char d[96]; describe_va(addr, d, sizeof d);
        int c = CL_OTHER;
        if (!strcmp(rn, "workmem")) c = CL_REPLAY;
        else if (region >= 0 && reg_in_img(rn)) { int k = class_of_va(addr - g_base + IMG_PREF); c = k < 0 ? CL_OTHER : k; }
        LOG("L4MISS seq=%llu fr=%u pass=%s slot=+0x%03x at=%s len=%zu region=%s class=%c (UE wrote fight state outside the L4 windows; not reverted)\n",
            (unsigned long long)g_cur_seq, core_frame(), m->ri ? "resim" : "live", m->slot * 8, d, len, rn, k_cl_char[c]);
    }
}
/* v0.20.0 fix R2: real-heap and arena frees the step thread makes during a forwarded call. UE code runs
 * outside the step context there (the proxy clears the tid), so they are immediate. After the revert, a
 * reverted word whose old value points into one of these blocks would be a dangling pointer brought back:
 * UE's value is kept instead and L4DANGLE is logged (a stop item for the C0 reader). Step thread only; the
 * frees of other threads during the window are a known limit (review C5). */
#define L4F_MAX 4096   /* C1B3: 1024 -> 4096 (C1B-R3-SPEC.md §2.3.3: the largest observed is 2,749 frees in one +0x0c0 call) */
static uintptr_t g_l4f_lo[L4F_MAX], g_l4f_hi[L4F_MAX];
static volatile LONG g_l4f_n;
static uint64_t g_l4_dangle, g_l4_dangle_over, g_l4f_total; static int g_l4_dangle_lines;
void l4_note_heap_free(uintptr_t p, size_t sz) {
    if (!g_l4_in_call || GetCurrentThreadId() != g_l4_tid) return;
    lipsync_note_free(p, sz, g_l4_pass_resim);   /* LIPSYNC: the frees of a rollback's resims (pk_apply never puts back a pointer into one of them) */
    LONG k;
    if (g_mode == MODE_RBONLINE && !RS_OLD(RS_OLD_L4CNT)) { k = g_l4f_n; g_l4f_n = k + 1; }   /* v0.31.0: one writer (the L4 thread: checked above) */
    else k = InterlockedIncrement(&g_l4f_n) - 1;
    if (k < L4F_MAX) { g_l4f_lo[k] = p; g_l4f_hi[k] = p + (sz ? sz : 16); }
}
/* v0.31.0 (OPTA-M2-SPEC.md §20.3): whether a free's size is still read: L4 keeps [p, p + size) of a call's first L4F_MAX frees, the lip-sync
 * free list of a resim keeps LF_CAP. Past both (the +0x058 START call's ~1.18 M frees, S-B on v0.30.0) UE's GetAllocationSize is not called.
 * The L4 thread only (the counters it reads have that one writer); every other thread / mode: 1 (v0.30.0's path) */
static uint64_t g_l4_size_skips;
int l4_free_wants_size(void) {
    if (g_mode != MODE_RBONLINE || RS_OLD(RS_OLD_GETSIZE) || !g_l4_in_call || GetCurrentThreadId() != g_l4_tid) return 1;
    if (g_l4f_n < L4F_MAX || lipsync_free_wants(g_l4_pass_resim)) return 1;
    g_l4_size_skips++;
    return 0;
}
typedef struct { int slot; const char *win; uintptr_t base; } DangCtx;
/* C1B3 fix (round-3 review, RPB use-after-free): while armed (rb.c do_rpb), every core .bss word L4 keeps (UE's value, because the
 * old value pointed into a block freed during the call) is recorded with that block's range. do_rpb's scratch restore brings the present
 * back AFTER the pass: modes_l4kept_reapply() then puts UE's value back wherever the present still points into that block. */
#define L4K_MAX 1024
typedef struct { uintptr_t at; uint64_t ue; uintptr_t lo, hi; } L4Kept;
static L4Kept g_l4k[L4K_MAX]; static int g_l4k_on, g_l4k_n, g_l4k_over;
void modes_l4kept_arm(int on) { g_l4k_on = on; g_l4k_n = 0; g_l4k_over = 0; }
int modes_l4kept_reapply(int *kept, int *overflow) {
    int fixed = 0;
    for (int i = 0; i < g_l4k_n; i++) {
        volatile uint64_t *w = (volatile uint64_t *)g_l4k[i].at;
        uint64_t v = *w;
        if (v >= g_l4k[i].lo && v < g_l4k[i].hi) {
            *w = g_l4k[i].ue; fixed++;
            if (fixed <= 8) {
                char d[96]; describe_va(g_l4k[i].at, d, sizeof d);
                LOG("RBRPBDANGLE at=%s present=0x%llx ue=0x%llx freed=[0x%llx,0x%llx) (the scratch restore brought back a pointer into a block UE freed in the RPB's call: UE's value put back)\n",
                    d, (unsigned long long)v, (unsigned long long)g_l4k[i].ue, (unsigned long long)g_l4k[i].lo, (unsigned long long)g_l4k[i].hi);
            }
        }
    }
    if (kept) *kept = g_l4k_n;
    if (overflow) *overflow = g_l4k_over;
    return fixed;
}
static int l4_dangle_cb(void *u, size_t off, uint64_t before, uint64_t live) {
    DangCtx *c = (DangCtx *)u;
    LONG n = g_l4f_n; if (n > L4F_MAX) n = L4F_MAX;
    for (LONG i = 0; i < n; i++)
        if (before >= g_l4f_lo[i] && before < g_l4f_hi[i]) {
            g_l4_dangle++;
            if (g_l4k_on && c->base == VA(A_CORE_BSS_LO)) {   /* C1B3 fix: an RPB pass: remember the kept core .bss word */
                if (g_l4k_n < L4K_MAX) { g_l4k[g_l4k_n].at = c->base + off; g_l4k[g_l4k_n].ue = live; g_l4k[g_l4k_n].lo = g_l4f_lo[i]; g_l4k[g_l4k_n].hi = g_l4f_hi[i]; g_l4k_n++; }
                else g_l4k_over++;
            }
            if (g_l4_dangle_lines++ < 40) {
                char d[96]; describe_va(c->base + off, d, sizeof d);
                LOG("L4DANGLE seq=%llu fr=%u pass=%s slot=+0x%03x win=%s at=%s before=0x%llx ue=0x%llx freed=[0x%llx,0x%llx) (the revert would restore a pointer into a block freed during the call; UE's value kept)\n",
                    (unsigned long long)g_cur_seq, core_frame(), g_l4_pass_resim ? "resim" : "live", c->slot * 8, c->win, d, (unsigned long long)before,
                    (unsigned long long)live, (unsigned long long)g_l4f_lo[i], (unsigned long long)g_l4f_hi[i]);
            }
            return 1;
        }
    return 0;
}
/* the L4MISS watchdog check after a forwarded call (l4=1 after the revert; l4=2 after the guard's diff) */
static void l4_miss_check(int slot, int ri, int pin_freed) {
    if (g_l4_snap_ok && pin_freed) { g_l4_snap_ok = 0; if (g_l4_miss_skipped++ < 5) LOG("L4MISS check skipped seq=%llu slot=+0x%03x: a pinned block was freed during the call\n", (unsigned long long)g_cur_seq, slot * 8); }
    if (g_l4_snap_ok) {
        uint64_t tm = qpc();
        MissCtx m = { slot, ri, 0, 0 }; int nr = 0;
        snap_diff_live(&g_ss, &g_l4_snap, l4_miss_cb, &m, 64, &nr);
        g_l4_miss_checked++;
        if (m.n) { g_l4_miss_calls++; g_l4_miss_ranges += (uint64_t)m.n; g_l4_miss_bytes += m.bytes; g_l4_frame_miss++; }
        double mu = qpc_us(qpc() - tm);
        g_l4_miss_us += mu; if (mu > g_l4_miss_us_max) g_l4_miss_us_max = mu;
        g_l4_snap_ok = 0;
    }
}
/* v0.21.0-S1 L4 guard (l4=2): the core .bss diff of L4, but NOTHING is reverted and the result is UE's. Every changed
 * span (per 64-byte chunk, trimmed, split where the kind changes) counts as one write: in P, in the FX scratch e9b0,
 * or other. The other writes are UE writes into fight state outside the known presentation/FX windows: logged as
 * L4GUARD lines (budget L4G_BUDGET). The listener pointer (a new listener installed by UE) is counted apart. */
#define L4G_BUDGET 200
static uint64_t g_l4g_wp, g_l4g_wfx, g_l4g_wo, g_l4g_lst; static int g_l4g_lines;
static void l4g_post(int slot) {
    uint64_t t = qpc();
    int ri = g_l4_pass_resim;
    const uint8_t *now = (const uint8_t *)VA(A_CORE_BSS_LO), *was = g_l4_buf;
    uintptr_t lst = VA(A_LISTENER_PTR);
    g_l4f_total += (uint64_t)g_l4f_n;
    for (size_t off = 0; off < CORE_BSS_LEN; off += 64) {
        size_t cl = CORE_BSS_LEN - off < 64 ? CORE_BSS_LEN - off : 64;
        if (!memcmp(now + off, was + off, cl)) continue;
        size_t i0 = off, i1 = off + cl;
        while (i0 < i1 && now[i0] == was[i0]) i0++;
        while (i1 > i0 && now[i1 - 1] == was[i1 - 1]) i1--;
        uintptr_t a0 = VA(A_CORE_BSS_LO) + i0, a1 = VA(A_CORE_BSS_LO) + i1;
        if (a0 >= lst && a1 <= lst + 8) { g_l4g_lst++; continue; }
        uint64_t p = a0 - g_base + IMG_PREF, pe = a1 - g_base + IMG_PREF;
        while (p < pe) {
            uint64_t q; int k = pres_va_kind(p, pe, &q);
            if (k == PRES_K_P) g_l4g_wp++;
            else if (k == PRES_K_FX) g_l4g_wfx++;
            else {
                g_l4g_wo++;
                if (g_l4g_lines < L4G_BUDGET) {
                    g_l4g_lines++;
                    char d[96]; describe_va(VA(p), d, sizeof d);
                    int c = class_of_va(p);
                    LOG("L4GUARD slot=+0x%03x at=%s len=%llu class=%c seq=%llu fr=%u\n", slot * 8, d, (unsigned long long)(q - p),
                        k_cl_char[c < 0 ? CL_OTHER : c], (unsigned long long)g_cur_seq, core_frame());
                }
            }
            p = q;
        }
    }
    double us = g_l4_t_pre + qpc_us(qpc() - t);
    g_l4_us += us; if (us > g_l4_us_max) g_l4_us_max = us;
    l4_miss_check(slot, ri, g_pins_freed_n != g_l4_freed0);
}
static void l4_post(int slot, uint64_t *res) {
    if (!g_l4 || (!g_ctx_flags && !g_l4_late) || GetCurrentThreadId() != g_l4_tid || g_l4_depth <= 0) return;   /* v0.33.0: + the late delivery */
    if (--g_l4_depth != 0 || !g_l4_top_fwd) return;
    g_l4_in_call = 0;
    if (g_l4 == 2) { l4g_post(slot); return; }   /* v0.21.0-S1 guard: log only, UE's result and writes stand */
    uint64_t t = qpc();
    int ri = g_l4_pass_resim;
    uintptr_t lst_now = *(volatile uintptr_t *)VA(A_LISTENER_PTR);
    L4Ctx c = { slot };
    LONG nf = g_l4f_n;
    g_l4f_total += (uint64_t)nf;
    if (nf > L4F_MAX) {
        g_l4_dangle_over++;
        double cus = qpc_us(t - g_l4_call_t0);   /* v0.31.0: the forwarded call's own wall time (UE's work + every free through our door) */
        if (g_mode == MODE_RBONLINE) { g_l4_big_calls++; if (cus > g_l4_big_us_max) g_l4_big_us_max = cus; g_l4_big_us += cus; }
        if (g_l4_dangle_lines++ < 40) {
            if (g_mode == MODE_RBONLINE) LOG("L4DANGLE overflow seq=%llu slot=+0x%03x frees=%ld (only the first %d are checked) (the revert check is partial) call_us=%.0f size_skips=%llu\n",
                                             (unsigned long long)g_cur_seq, slot * 8, (long)nf, L4F_MAX, cus, (unsigned long long)g_l4_size_skips);
            else LOG("L4DANGLE overflow seq=%llu slot=+0x%03x frees=%ld (only the first %d are checked) (the revert check is partial)\n", (unsigned long long)g_cur_seq, slot * 8, (long)nf, L4F_MAX);
        }
    }
    l4_keep_cb kcb = nf ? l4_dangle_cb : NULL;
    size_t kept = 0;
    DangCtx dc = { slot, "core_bss", VA(A_CORE_BSS_LO) };
    lipsync_l4_keep_begin(slot, ri, g_l4_buf, VA(A_CORE_BSS_LO), CORE_BSS_LEN);   /* LIPSYNC: P after UE's call, before the unchanged revert */
    size_t nb = l4_revert_ex((uint8_t *)VA(A_CORE_BSS_LO), g_l4_buf, CORE_BSS_LEN, l4_span, &c, kcb, &dc, &kept);
    if (lst_now != g_l4_lst_before) { *(volatile uintptr_t *)VA(A_LISTENER_PTR) = lst_now; g_l4_lst_kept++; nb = nb > 8 ? nb - 8 : 0; }
    int win = 0;
    int pin_freed = g_pins_freed_n != g_l4_freed0;   /* UE freed a pinned block during the call: never touch pins now */
    if (pin_freed && g_l4_rdx_kind == 4) g_l4_rdx_len = 0;
    if (g_l4_rdx_len && memcmp((void *)g_l4_rdx, g_l4_rdx_before, g_l4_rdx_len)) {
        l4w_note(slot, (uintptr_t)g_l4_rdx, g_l4_rdx_len);
        DangCtx dr = { slot, "rdx", (uintptr_t)g_l4_rdx };
        l4_revert_ex((uint8_t *)g_l4_rdx, g_l4_rdx_before, g_l4_rdx_len, NULL, NULL, kcb, &dr, &kept); g_l4_win_rev++; win = 1;
    }
    if (g_l4_stk && memcmp((void *)g_l4_crsp, g_l4_stk_before, STKWIN)) {
        DangCtx ds = { slot, "stack", (uintptr_t)g_l4_crsp };
        l4_revert_ex((uint8_t *)g_l4_crsp, g_l4_stk_before, STKWIN, NULL, NULL, kcb, &ds, &kept); g_l4_stk_rev++; win = 1;
    }
    lipsync_l4_keep_end();   /* LIPSYNC: UE's P writes of a live pass stand (byte-exact; NONP ranges stay reverted) */
    if (nb || win) { g_l4_rev_calls[ri]++; g_l4_rev_bytes[ri] += nb; g_l4_frame_rev++; }
    res[0] = 0; res[1] = 0;   /* canonical result (a muted call returns rax = 0, xmm0 = 0) */
    double us = g_l4_t_pre + qpc_us(qpc() - t);
    g_l4_us += us; if (us > g_l4_us_max) g_l4_us_max = us;
    l4_miss_check(slot, ri, pin_freed);
}
/* v0.33.0 (OPTA-M2-SPEC.md §22.1): the late presentation deliveries (rb.c pres_flush, the step thread, between passes: no step context, the real
 * listener in place). Each call is made as the proxy makes a forwarded live call: UE's real function with (listener, &payload), inside L4's guard
 * (the core .bss copy before, the revert after with L4W lines, the kept listener pointer, the frees made during the call checked by L4DANGLE, a core
 * entry inside it counted and logged by L4REENTRY), so UE's presentation work can never change the simulation. guard = 0 only under the
 * harness control h_rb_pres_nol4 (XFXW). The listener calls of a normal pass are untouched.
 * v0.35.0 (OPTA-M2-SPEC.md §25): + r8 / r9. UE's function gets (listener, &q[i].pay, the confirmed call's r8, r9): the registers UE would have seen (none of
 * +0x008 / +0x010 / +0x028 / +0x120 / +0x128 reads them, V; passed anyway); rdx = the plugin's queue copy (0x48 bytes, UE reads at most 0x41: l4_rdx_window
 * gives 0 for a plugin .bss address), crsp 0 = no stack window. L4 is unchanged. */
int modes_lsn_deliver(int n, const RbLate *q, int guard) {
    void **lst = (void **)VA(A_LISTENER_PTR);
    void *real = *lst;
    if (!real || real == (void *)&g_proxy || g_ctx_flags || g_l4_depth) return 0;
    void **rvt = *(void ***)real;
    int done = 0;
    for (int i = 0; i < n; i++) {
        int slot = q[i].slot;
        if (slot <= 0 || slot >= LISTENER_NSLOTS) continue;
        void *fn = rvt[slot];
        if (!fn) continue;
        uint64_t res[2] = { 0, 0 };
        if (guard) { g_l4_late = 1; g_l4_tid = GetCurrentThreadId(); g_l4_depth = 0; g_l4_in_call = 0; g_l4_pass_resim = 0; l4_pre(slot, (uint64_t)(uintptr_t)q[i].pay, 0); }
        ((int64_t (*)(void *, const void *, uint64_t, uint64_t))fn)(real, q[i].pay, q[i].r8, q[i].r9);
        if (guard) { l4_post(slot, res); g_l4_late = 0; g_l4_depth = 0; g_l4_in_call = 0; }
        done++;
    }
    return done;
}
/* v0.36.0 (OPTA-M2-SPEC.md §26, DESIGN.md §2.3): a +0x030 stop the plugin makes inside a forwarding pass, right before the core's +0x028 is forwarded (rb.c
 * snd_fwd, from rb_lsn_pre in lsn_pre). Made exactly like a forwarded call: UE's function of this pass's listener (lp_family.self / fn), the step's tid
 * already cleared by the thunk, the FP guard around it, L4's guard around it (g_l4_depth == 0 here: we are in the +0x028's pre hook, before its
 * l4_pre). rdx = a plugin .bss copy (l4_rdx_window gives 0 for it), crsp 0, r8 = r9 = 0 (UE's +0x030 reads [0,8) only). Not a core call: not counted in
 * the pass's calls / fwd, the RBLEDGER or lp_family.counts (RBPRES2 inject= and L4STAT calls_live count it). Returns 0 (nothing called) outside a step,
 * nested in a call, or when the pass does not forward the slot. */
int modes_lsn_inject(int slot, const uint8_t *pay8) {
    if (slot <= 0 || slot >= LISTENER_NSLOTS || !g_ctx_flags || g_l4_depth) return 0;
    void *fn = lp_family.fn[slot], *self = lp_family.self[slot];
    if (!fn || !self || !g_lp_fwd[slot]) return 0;
    static uint8_t buf[RB_LATE_PAY];
    memset(buf, 0, sizeof buf); memcpy(buf, pay8, 8);
    uint64_t res[2] = { 0, 0 };
    fp_lsn_pre(); l4_pre(slot, (uint64_t)(uintptr_t)buf, 0);
    ((int64_t (*)(void *, const void *, uint64_t, uint64_t))fn)(self, buf, 0, 0);
    l4_post(slot, res); fp_lsn_post(slot);
    return 1;
}
void l4_note_ifc(int off, const char *what) {
    if (!g_l4_in_call) return;
    g_l4_reentry++;
    int ls = g_l4_top_slot, is = off / 8, first = 0;
    if (ls >= 0 && ls < LISTENER_NSLOTS && is >= 0 && is < IFC_NSLOTS) first = g_l4_re_pair[ls][is]++ == 0;
    if (first || g_l4_re_lines++ < 40)   /* fix C1: the first call of every pair is always logged */
        LOG("L4REENTRY ifc_slot=+0x%02x entry=%s during_listener_slot=+0x%03x same_thread=%d seq=%llu fr=%u (UE called the core inside a notification; %s)\n",
            off, what, g_l4_top_slot * 8, GetCurrentThreadId() == g_l4_tid, (unsigned long long)g_cur_seq, core_frame(),
            g_l4 == 2 ? "L4 guard: logged only, its core .bss writes show as L4GUARD lines" : "L4 reverts what it did");
}
static int ifc_pre(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp) {
    (void)rdx; (void)res; (void)crsp; l4_note_ifc(slot * 8, "census_thunk"); c1a_ifc_pre(slot);
    /* C2 (C2-SPEC.md §2.5): NETUESKIP. UE's replay skip is IBattleCore +0x88, called from UpdateBattle when +0x80 says yes and UE's OWN pad has
     * bit 0x10 (audit/c1a/RECON.md, V): each PC would call it from its own pad. In an armed rbnet battle it is dropped on both peers (not
     * counted, never forwarded); the result is 0 */
    int keep = 0;
#ifdef SC6RB_HARNESS
    keep = g_cfg.h_c2_ueskip_keep;   /* harness control NUESKIPX: the call reaches the core */
#endif
    if (g_mode == MODE_RBNET && slot == 0x88 / 8 && ns_battle_on() && !keep) {
        static int lines; static uint64_t n;
        ifc_family.counts[*ifc_family.bank * FWD_MAXSLOTS + (uint32_t)slot]--;
        res[0] = 0; res[1] = 0; n++;
        if (lines++ < 20) LOG("NETUESKIP suppressed f=%u n=%llu (UE's own skip request: dropped in netplay)\n", c1a_next_f() - 1, (unsigned long long)n);
        return 1;
    }
    return 0;
}
/* v0.22.1-C1a (C1A-AUDIT C3): installed in record/replay only: the TT800 state after an IBattleCore +0xc8 call (c1a_run.c) */
static int ifc_post(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp) { (void)rdx; (void)res; (void)crsp; c1a_ifc_post(slot); return 0; }
static void l4_report(const char *why) {
    if (!g_l4) return;
    uint64_t c = g_l4_calls[0] + g_l4_calls[1];
    if (g_l4 == 2) {   /* v0.21.0-S1 guard: writes counted per changed span (see l4g_post) */
        LOG("L4GUARDSTAT why=%s calls=%llu writes_in_p=%llu writes_in_fx=%llu writes_other=%llu calls_live=%llu calls_resim=%llu listener_changes=%llu lines=%d budget=%d us_avg=%.1f us_max=%.1f miss_checked=%llu miss_calls=%llu miss_ranges=%llu reentry=%llu other_thread=%llu\n",
            why, (unsigned long long)c, (unsigned long long)g_l4g_wp, (unsigned long long)g_l4g_wfx, (unsigned long long)g_l4g_wo,
            (unsigned long long)g_l4_calls[0], (unsigned long long)g_l4_calls[1], (unsigned long long)g_l4g_lst, g_l4g_lines, L4G_BUDGET,
            c ? g_l4_us / (double)c : 0.0, g_l4_us_max, (unsigned long long)g_l4_miss_checked, (unsigned long long)g_l4_miss_calls,
            (unsigned long long)g_l4_miss_ranges, (unsigned long long)g_l4_reentry, (unsigned long long)g_l4_other_thread);
        for (int a = 0; a < LISTENER_NSLOTS; a++)
            for (int b = 0; b < IFC_NSLOTS; b++)
                if (g_l4_re_pair[a][b]) LOG("L4REPAIR why=%s listener_slot=+0x%03x ifc_slot=+0x%03x n=%u\n", why, a * 8, b * 8, g_l4_re_pair[a][b]);
        return;
    }
    LOG("L4STAT why=%s calls_live=%llu calls_resim=%llu reverted_live=%llu reverted_resim=%llu bytes_live=%llu bytes_resim=%llu rdx_window_reverts=%llu stack_window_reverts=%llu listener_kept=%llu us_avg=%.1f us_max=%.1f miss_checked=%llu miss_calls=%llu miss_ranges=%llu miss_bytes=%llu miss_us_avg=%.1f miss_us_max=%.1f miss_skipped=%llu reentry=%llu other_thread=%llu depth_leak=%llu nobuf=%llu\n",
        why, (unsigned long long)g_l4_calls[0], (unsigned long long)g_l4_calls[1], (unsigned long long)g_l4_rev_calls[0], (unsigned long long)g_l4_rev_calls[1],
        (unsigned long long)g_l4_rev_bytes[0], (unsigned long long)g_l4_rev_bytes[1], (unsigned long long)g_l4_win_rev, (unsigned long long)g_l4_stk_rev,
        (unsigned long long)g_l4_lst_kept, c ? g_l4_us / (double)c : 0.0, g_l4_us_max, (unsigned long long)g_l4_miss_checked, (unsigned long long)g_l4_miss_calls,
        (unsigned long long)g_l4_miss_ranges, (unsigned long long)g_l4_miss_bytes, g_l4_miss_checked ? g_l4_miss_us / (double)g_l4_miss_checked : 0.0, g_l4_miss_us_max,
        (unsigned long long)g_l4_miss_skipped, (unsigned long long)g_l4_reentry, (unsigned long long)g_l4_other_thread, (unsigned long long)g_l4_depth_leak, (unsigned long long)g_l4_nobuf);
    /* v0.20.0 review fixes: R2 dangling-pointer guard, C2 heap misses, R1 pinned frees inside calls, C1 re-entry pairs */
    LOG("L4STAT2 why=%s dangle=%llu dangle_overflow=%llu frees_in_call=%llu miss_pins=%llu miss_arena=%llu pinfree_incall=%llu\n",
        why, (unsigned long long)g_l4_dangle, (unsigned long long)g_l4_dangle_over, (unsigned long long)g_l4f_total,
        (unsigned long long)g_l4_miss_pins, (unsigned long long)g_l4_miss_arena, (unsigned long long)g_pinfree_incall);
    for (int a = 0; a < LISTENER_NSLOTS; a++)
        for (int b = 0; b < IFC_NSLOTS; b++)
            if (g_l4_re_pair[a][b]) LOG("L4REPAIR why=%s listener_slot=+0x%03x ifc_slot=+0x%03x n=%u\n", why, a * 8, b * 8, g_l4_re_pair[a][b]);
    static int last_n = -1;
    if (g_nl4w == last_n && strcmp(why, "shutdown")) return;
    last_n = g_nl4w;
    for (int k = 0; k < g_nl4w && k < 120; k++) {
        char d[96]; describe_va(g_l4w[k].addr, d, sizeof d);
        { NT_TIB *tib = (NT_TIB *)NtCurrentTeb();   /* reports run on the step thread: name its stack */
          if (g_l4w[k].addr >= (uintptr_t)tib->StackLimit && g_l4w[k].addr < (uintptr_t)tib->StackBase) snprintf(d, sizeof d, "stack(step thread)"); }
        uint64_t rel; const char *fn = field_name(g_l4w[k].addr, &rel);
        int in_img = g_l4w[k].addr >= VA(A_CORE_BSS_LO) && g_l4w[k].addr < VA(A_CORE_BSS_HI);
        int cl = in_img ? class_of_va(g_l4w[k].addr - g_base + IMG_PREF) : -1;
        LOG("L4W slot=+0x%03x at=%s len=%u hits=%u field=%s+0x%llx class=%c (UE write reverted by L4)\n", g_l4w[k].slot * 8, d, g_l4w[k].len, g_l4w[k].hits,
            fn, (unsigned long long)rel, k_cl_char[cl < 0 ? CL_OTHER : cl]);
    }
}
/* v0.21.0-S1 setup barrier: the seq of the last live step that called UE's listener slot +0x0c0 (UE's battle-start
 * sway/cloth writes, P-fields.md §1.2) and the first step after the last Setup/Init. With r7_setup_barrier=1 no resim
 * starts at or before max(the two) (official online has a round-start control packet: rollback cannot cross it). */
#define LSN_SLOT_BATTLE_START (0x0c0 / 8)
static uint64_t g_bar_lsn_seq, g_bar_setup_seq, g_bar_lsn_calls, g_bar_setups, g_r7_bar_clamps;
static uint64_t bar_seq(void) { return g_bar_lsn_seq > g_bar_setup_seq ? g_bar_lsn_seq : g_bar_setup_seq; }
static int lsn_pre(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp) {
    /* C1B3: (C1B-R3-SPEC.md §2.3.1) the +0x0c0 counter is bumped only by a call that really reaches UE: rb_lsn_pre decides first (a muted
     * state-class call never reaches UE and is never counted). h_old_c0count=1 (harness only) keeps the v0.24 order (the negative control). */
    if (slot == LSN_SLOT_BATTLE_START && g_ctx_flags && !g_run_muted && g_lp_fwd[slot] && rb_t_old_c0count()) { g_bar_lsn_seq = g_cur_seq; g_bar_lsn_calls++; }
    if (g_ctx_flags && rb_lsn_pre(slot, rdx, crsp)) { res[0] = 0; res[1] = 0; return 1; }   /* v0.23.0-C1b: a muted state-class call (never reaches UE); v0.33.0: + rdx; v0.34.2: + crsp */
    if (slot == LSN_SLOT_BATTLE_START && g_ctx_flags && !g_run_muted && g_lp_fwd[slot] && !rb_t_old_c0count()) { g_bar_lsn_seq = g_cur_seq; g_bar_lsn_calls++; }
    int skip = lsn_pre_v19(slot, rdx, res, crsp);
    if (!skip) l4_pre(slot, rdx, crsp);
    return skip;
}
static int lsn_post(int slot, uint64_t rdx, uint64_t *res, uint64_t crsp) {
    l4_post(slot, res);   /* first: the v0.19 record logic then sees the canonical (reverted) state */
    return lsn_post_v19(slot, rdx, res, crsp);
}

static void lrec_begin(LRec *r, int mode) {
    g_lrec_cur = r; g_lrec_mode = r ? mode : 0; g_lrec_idx = 0; g_lrec_diverged = -1; g_lrec_depth = 0;
    if (r && mode == 1) { r->n = 0; r->overflow = 0; r->pool_used = 0; r->pool_overflow = 0; }
}
static void lrec_end(void) { g_lrec_cur = NULL; g_lrec_mode = 0; }

/* ------------------------------------------------------------------ foreign patches */
typedef struct { FPatch *fp; } FpCtx;
static void fp_cb(void *u, int region, size_t off, size_t len, uintptr_t addr) {
    FPatch *fp = ((FpCtx *)u)->fp;
    if (region == -2) {   /* pins: map the blob offset back to an address */
        size_t acc = 0; int i = 0;
        for (; i < g_ss.npin && acc + g_ss.pins[i].len <= off; i++) acc += g_ss.pins[i].len;
        if (i >= g_ss.npin) { fp->overflow++; return; }
        addr = g_ss.pins[i].addr + (off - acc);
        if (len > g_ss.pins[i].len - (off - acc)) len = g_ss.pins[i].len - (off - acc);
    }
    if (region != -9 && lipsync_fp_skip()) {   /* LIPSYNC: fp_cb P-span skip (region -9 = a non-P piece of this span) */
        for (uintptr_t a = addr; len; ) { size_t r; int p = lipsync_fp_run(a, len, &r); if (!p) fp_cb(u, -9, 0, r, a); a += r; len -= r; }   /* LIPSYNC: */
        return;   /* LIPSYNC: */
    }   /* LIPSYNC: */
    if (fp->n >= FP_MAXR || fp->used + len > FP_BYTES || len > 0xffff) { fp->overflow++; return; }
    fp->addr[fp->n] = addr; fp->len[fp->n] = (uint16_t)len;
    memcpy(fp->bytes + fp->used, (const void *)addr, len);
    fp->used += (uint32_t)len; fp->n++;
}
/* diff live memory against ref (state right after the previous step): what was written
 * between steps. Returns the number of differing bytes. */
size_t fp_capture(FPatch *fp, const Snap *ref) {
    fp->n = 0; fp->used = 0; fp->overflow = 0;
    FpCtx c = { fp };
    int nr = 0;
    return snap_diff_live(&g_ss, ref, fp_cb, &c, FP_MAXR + 8, &nr);
}
static int fp_covers(const void *fpv, uintptr_t a, size_t len) {
    const FPatch *fp = (const FPatch *)fpv;
    for (int i = 0; i < fp->n; i++) if (a >= fp->addr[i] && a + len <= fp->addr[i] + fp->len[i] + 16) return 1;
    return 0;
}
void fp_apply(const FPatch *fp) {
    uint32_t o = 0;
    for (int i = 0; i < fp->n; i++) { memcpy((void *)fp->addr[i], fp->bytes + o, fp->len[i]); o += fp->len[i]; }
}


/* ------------------------------------------------------------------ round 13: statics and FP environment
 * STATICS. Our snapshot covers the core .bss and the .data singletons, but the exe's .data section is
 * 0x14406e000..0x144877d90. On the first roll7 frames (static_learn_frames) we copy that section before
 * the restore, compare it after the FIRST resim step (frame k0, core code only, muted, arena), and keep
 * an 8-byte granule when (a) the resim changed it, (b) it is outside every snapshot range, and (c) it
 * is equal again once the live state is back (so another thread did not change it meanwhile). A
 * granule seen in 2 frames becomes a pin (image memory: never freed). Allocator and engine statistics
 * are not written in resims (allocations go to our arena), so they are not picked.
 * FP ENVIRONMENT. MXCSR and the x87 control word are captured at every live step entry and installed
 * at the entry of every resim of that frame; after each listener call they are put back to their
 * pre-call value (UE code could leave FTZ/DAZ or rounding changed for the rest of the live step). */
#define SDATA_LO 0x14406e000ULL
#define SDATA_HI 0x144877d90ULL
#define SWR_MAX 1024
typedef struct { uintptr_t a; uint32_t len, hits; uint32_t first_seq, pinned; } SwR;
static SwR g_sw[SWR_MAX];
static int g_nsw, g_sw_pins_pending;
static uint8_t *g_sw_buf;
static uintptr_t g_sw_tmp[4096]; static int g_nsw_tmp;
static uint64_t g_sw_frames, g_sw_new, g_sw_noise;
static double g_sw_us;
static int sw_learning(void) { return g_cfg.static_learn && g_sw_frames < (uint64_t)g_cfg.static_learn; }
static void sw_before(void) {
    if (!sw_learning()) return;
    size_t n = (size_t)(SDATA_HI - SDATA_LO);
    if (!g_sw_buf) g_sw_buf = (uint8_t *)VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_sw_buf || !mem_readable((void *)VA(SDATA_LO), 4096)) return;
    uint64_t t = qpc();
    memcpy(g_sw_buf, (void *)VA(SDATA_LO), n);
    g_nsw_tmp = 0;
    g_sw_us += qpc_us(qpc() - t);
}
static void sw_after_first(void) {   /* right after the first resim step */
    if (!sw_learning() || !g_sw_buf) return;
    uint64_t t = qpc();
    const uint8_t *now = (const uint8_t *)VA(SDATA_LO);
    size_t n = (size_t)(SDATA_HI - SDATA_LO);
    for (size_t pg = 0; pg < n; pg += 4096) {
        size_t m = n - pg < 4096 ? n - pg : 4096;
        if (!memcmp(now + pg, g_sw_buf + pg, m)) continue;
        for (size_t o = pg; o + 8 <= pg + m; o += 8) {
            if (!memcmp(now + o, g_sw_buf + o, 8)) continue;
            uintptr_t a = VA(SDATA_LO) + o;
            if (covered_cb(a, 8) || pin_index_of(a) >= 0) continue;
            if (g_nsw_tmp < 4096) g_sw_tmp[g_nsw_tmp++] = a;
        }
    }
    g_sw_us += qpc_us(qpc() - t);
}
static void sw_after_all(uint64_t seq) {   /* after the live state is back */
    if (!sw_learning() || !g_sw_buf) return;
    g_sw_frames++;
    for (int i = 0; i < g_nsw_tmp; i++) {
        uintptr_t a = g_sw_tmp[i];
        size_t o = a - VA(SDATA_LO);
        if (memcmp((void *)a, g_sw_buf + o, 8)) { g_sw_noise++; continue; }   /* changed again: another writer */
        int k = 0;
        for (; k < g_nsw; k++) if (a >= g_sw[k].a && a < g_sw[k].a + g_sw[k].len) break;
        if (k == g_nsw) {
            for (k = 0; k < g_nsw; k++) if (a == g_sw[k].a + g_sw[k].len && !g_sw[k].pinned) { g_sw[k].len += 8; break; }
            if (k == g_nsw) {
                if (g_nsw >= SWR_MAX) continue;
                g_sw[g_nsw].a = a; g_sw[g_nsw].len = 8; g_sw[g_nsw].hits = 0; g_sw[g_nsw].first_seq = (uint32_t)seq; g_sw[g_nsw].pinned = 0; g_nsw++;
            }
        }
        g_sw[k].hits++;
        if (g_sw[k].hits == 2 && !g_sw[k].pinned) {
            g_sw_pins_pending = 1; g_sw_new++;
            if (g_sw_new <= 64) LOG("STATICW new va=0x%llx len=%u seq=%llu (written by core code in a resim, outside the snapshot)\n",
                                    (unsigned long long)(a - g_base + IMG_PREF), g_sw[k].len, (unsigned long long)seq);
        }
    }
    if (g_sw_frames == (uint64_t)g_cfg.static_learn) LOG("STATICW learning done frames=%llu ranges=%d noise=%llu us_total=%.0f\n",
        (unsigned long long)g_sw_frames, g_nsw, (unsigned long long)g_sw_noise, g_sw_us);
}
static void sw_add_pins(SPin *pins, int *npins, int max) {
    int added = 0; uint64_t bytes = 0;
    for (int k = 0; k < g_nsw && *npins < max; k++) {
        if (g_sw[k].hits < 2) continue;
        g_sw[k].pinned = 1;
        pins[*npins].addr = g_sw[k].a; pins[*npins].len = g_sw[k].len; (*npins)++; added++; bytes += g_sw[k].len;
    }
    if (added) LOG("STATICW pins=%d bytes=%llu\n", added, (unsigned long long)bytes);
}
static void sw_report(const char *why) {
    if (!g_cfg.static_learn) return;
    int pinned = 0; for (int k = 0; k < g_nsw; k++) if (g_sw[k].hits >= 2) pinned++;
    LOG("STATICW report why=%s frames=%llu ranges=%d pinned=%d noise=%llu us_avg=%.0f\n", why, (unsigned long long)g_sw_frames, g_nsw, pinned,
        (unsigned long long)g_sw_noise, g_sw_frames ? g_sw_us / (double)g_sw_frames : 0.0);
    for (int k = 0, shown = 0; k < g_nsw && shown < 40; k++) {
        if (g_sw[k].hits < 2) continue;
        shown++;
        LOG("STATICWR va=0x%llx len=%u hits=%u first_seq=%u\n", (unsigned long long)(g_sw[k].a - g_base + IMG_PREF), g_sw[k].len, g_sw[k].hits, g_sw[k].first_seq);
    }
}

/* FP environment */
static uint32_t g_fp_mx[RING_MAX]; static uint16_t g_fp_cw[RING_MAX]; static uint64_t g_fp_seq[RING_MAX];
static uint32_t g_fp_pre_mx; static uint16_t g_fp_pre_cw;
static uint64_t g_fp_lsn_changes, g_fp_entry_fixes, g_fp_lines;
static inline uint16_t fp_getcw(void) { uint16_t cw; __asm__ volatile("fnstcw %0" : "=m"(cw)); return cw; }
static inline void fp_setcw(uint16_t cw) { __asm__ volatile("fldcw %0" :: "m"(cw)); }
static int g_fp_canon_active;   /* v0.20: the current step pass runs on the canonical MXCSR / x87 CW */
static void fp_lsn_pre(void) { if (g_cfg.fp_guard || g_fp_canon_active) { g_fp_pre_mx = _mm_getcsr(); g_fp_pre_cw = fp_getcw(); } }
static void fp_lsn_post(int slot) {
    if (!g_cfg.fp_guard && !g_fp_canon_active) return;
    uint32_t mx = _mm_getcsr(); uint16_t cw = fp_getcw();
    if ((mx & ~0x3fu) != (g_fp_pre_mx & ~0x3fu) || cw != g_fp_pre_cw) {   /* ignore the sticky exception flags */
        g_fp_lsn_changes++;
        if (g_fp_lines++ < 20) LOG("FPENV listener slot=+0x%03x changed mxcsr %08x->%08x x87cw %04x->%04x (restored)\n", slot * 8, g_fp_pre_mx, mx, g_fp_pre_cw, cw);
        _mm_setcsr((mx & 0x3fu) | (g_fp_pre_mx & ~0x3fu)); fp_setcw(g_fp_pre_cw);
    }
}

/* ------------------------------------------------------------------ running one step */
/* Stack scrub (round 9: always on). The core reads uninitialized stack (round 8: the W lane of
 * FUN_1402f54c0's pose buffer reaches PLAYER+0x4529c). Zeroing the stack below SP before every step
 * pass (live, replay, muted, every resim), and below the listener thunk after every listener call
 * (UE ran, was muted or was replayed), makes those reads see the same bytes in every pass. Both
 * netplay peers would do exactly the same. Sizes: stack_scrub_kb, lsn_scrub_kb. */
volatile int g_pending_scrub = -1, g_pending_depth = -1;
int g_depth_rt;
static int g_scrub;
static uint64_t g_scrubs, g_lscrubs;
static double g_scrub_us, g_scrub_us_max, g_lscrub_us, g_lscrub_us_max;
__attribute__((noinline)) static void scrub_below(size_t n) {
    uint8_t *buf = (uint8_t *)__builtin_alloca(n);
    __asm__ volatile("" : : "r"(buf) : "memory");   /* the buffer escapes: the memset below cannot be dropped */
    memset(buf, 0, n);
    __asm__ volatile("" : : "r"(buf) : "memory");
}
static void scrub_stack(void) {
    uint64_t t = qpc();
    scrub_below((size_t)(g_bigscrub ? 512 : g_cfg.stack_scrub_kb) * 1024);
    double us = qpc_us(qpc() - t);
    g_scrub_us += us; if (us > g_scrub_us_max) g_scrub_us_max = us;
}
static void lsn_after(void) {
    if (g_ctx_flags) fp_lsn_post(g_fp_slot);
    if (g_ctx_flags && g_cfg.ws_lsn_attrib && g_wsl_depth > 0 && --g_wsl_depth == 0 && g_ws_active) ws_lsn_post();
    if (!g_ctx_flags || g_cfg.lsn_scrub_kb <= 0) return;
    uint64_t t = qpc();
    scrub_below((size_t)(g_bigscrub ? 256 : g_cfg.lsn_scrub_kb) * 1024);
    double us = qpc_us(qpc() - t);
    g_lscrubs++; g_lscrub_us += us; if (us > g_lscrub_us_max) g_lscrub_us_max = us;
}
static void scrub_report(const char *why) {
    LOG("SCRUBSTAT why=%s step_scrubs=%llu step_kb=%d step_us_avg=%.2f step_us_max=%.1f lsn_scrubs=%llu lsn_kb=%d lsn_us_avg=%.2f lsn_us_max=%.1f\n", why,
        (unsigned long long)g_scrubs, g_cfg.stack_scrub_kb, g_scrubs ? g_scrub_us / (double)g_scrubs : 0.0, g_scrub_us_max,
        (unsigned long long)g_lscrubs, g_cfg.lsn_scrub_kb, g_lscrubs ? g_lscrub_us / (double)g_lscrubs : 0.0, g_lscrub_us_max);
}
static void lsn_after_fwd(void) { lsn_after(); }
void zero_below_caller(size_t n);   /* fwd_thunks.S */
/* v0.20 L3 canonical float environment: the MXCSR / x87 CW UE had set, once per battle and on change */
static uint64_t g_fpenv_battle = ~0ULL; static uint32_t g_fpenv_mx; static uint16_t g_fpenv_cw; static int g_fpenv_lines;
static void fpenv_note_ue(uint32_t mx, uint16_t cw, uint64_t seq) {
    uint32_t mk = mx & ~0x3fu;
    int first = g_fpenv_battle != g_battle_id;
    if (!first && mk == g_fpenv_mx && cw == g_fpenv_cw) return;
    g_fpenv_battle = g_battle_id; g_fpenv_mx = mk; g_fpenv_cw = cw;
    if (!first && g_fpenv_lines >= 50) return;
    g_fpenv_lines++;
    LOG("FPENV ue battle=%llu seq=%llu why=%s mxcsr=%08x x87cw=%04x canonical_mxcsr=%08x canonical_x87cw=%04x equal=%d vzeroall=%d (the values UE left at a live step entry; every step pass runs on the canonical ones)\n",
        (unsigned long long)g_battle_id, (unsigned long long)seq, first ? "battle" : "change", mk, cw, g_cfg.l3_mxcsr, g_cfg.l3_x87cw,
        mk == (g_cfg.l3_mxcsr & ~0x3fu) && cw == (uint16_t)g_cfg.l3_x87cw, (int)g_l3_vzero);
}
/* v0.20: the C0b pass context, kept out of run_step's own frame (noinline, state in globals) so the
 * registers run_step holds across the step call stay as in v0.19 (the L1 plant-3 harness reads them) */
__attribute__((noinline)) static void run_step_c0b_enter(int muted, uint64_t seq, uint32_t mx0, uint32_t cw0) {
    g_l4_tid = GetCurrentThreadId(); g_l4_depth = 0; g_l4_in_call = 0; g_l4_pass_resim = muted != 0;
    g_fp_canon_active = g_l3_on && g_cfg.l3_fpenv;
    if (g_fp_canon_active) {
        /* L3: every step pass (live and resim, both peers) runs on the same MXCSR / x87 CW */
        if (!muted) fpenv_note_ue(mx0, (uint16_t)cw0, seq);
        _mm_setcsr(g_cfg.l3_mxcsr); fp_setcw((uint16_t)g_cfg.l3_x87cw);
    }
}
__attribute__((noinline)) static void run_step_c0b_leave(uint64_t seq) {
    g_fp_canon_active = 0;
    if (g_l4_depth != 0) { if (g_l4_depth_leak++ < 5) LOG("L4 WARN listener depth %d after the step (seq=%llu)\n", g_l4_depth, (unsigned long long)seq); g_l4_depth = 0; }
    g_l4_in_call = 0;
}
static void run_step(StepIn *in, int muted, uint64_t seq, uint32_t flags) {
    if (g_q1_noroute) flags &= ~(uint32_t)CTX_ROUTE;   /* v0.29.2 (§16.9) rbreplay rb_arm_f: frames < F allocate on the heap, as rbonline's passive steps */
    void **lst = (void **)VA(A_LISTENER_PTR);
    void *real = *lst;
    int proxied = 0;
    if (g_cfg.proxy && real && real != (void *)&g_proxy) {
        lp_configure(real, muted);
        *lst = &g_proxy;
        proxied = 1;
    } else g_listener_nonproxy++;
    g_cur_seq = seq;
    g_ctx_flags = flags;
    g_ctx_tid = GetCurrentThreadId();
    g_last_action = muted ? "step:muted" : "step:live";
    g_run_muted = muted;
    if (g_scrub) { scrub_stack(); g_scrubs++; }
    uint32_t fp_mx0 = _mm_getcsr(); uint16_t fp_cw0 = fp_getcw();
    run_step_c0b_enter(muted, seq, fp_mx0, fp_cw0);
    if (!g_fp_canon_active && g_cfg.fp_guard && seq) {   /* v0.20: the canonical environment (L3) replaces this copy */
        int ri = (int)(seq % RING_MAX);
        if (!muted) { g_fp_mx[ri] = fp_mx0; g_fp_cw[ri] = fp_cw0; g_fp_seq[ri] = seq; }
        else if (g_fp_seq[ri] == seq && ((g_fp_mx[ri] & ~0x3fu) != (fp_mx0 & ~0x3fu) || g_fp_cw[ri] != fp_cw0)) {
            g_fp_entry_fixes++;
            if (g_fp_lines++ < 20) LOG("FPENV resim entry seq=%llu mxcsr %08x->%08x x87cw %04x->%04x (set to the live step's)\n", (unsigned long long)seq, fp_mx0, g_fp_mx[ri], fp_cw0, g_fp_cw[ri]);
            _mm_setcsr(g_fp_mx[ri]); fp_setcw(g_fp_cw[ri]);
        }
    }
    if (g_vm_body_on) vm_step_begin();   /* v0.32.0 review fix B1: this thread's VM dispatches are traced (src/vm.c); v0.34.0: or counted for the D-I census */
    if (g_l3_on) {
        /* v0.19 L3: canonical call context. Commit + zero the region with an alloca scrub, then the
         * wrapper zeroes it again from its own frame and calls the step with every register zero
         * except the arguments (l1_stub.S). The depth covers FUN_1402edb90's measured frame + 64 KB. */
        size_t zb = l3_zero_bytes();
        scrub_below(zb + 8192);
        if (g_mode == MODE_RBONLINE || (g_mode == MODE_RBREPLAY && g_cfg.rb_l3stock == 1)) {   /* optA M2 (D-B): L3-stock, every pass; v0.29.2: + rbreplay rb_l3stock=1 */
#ifdef SC6RB_HARNESS
            InterlockedIncrement(&rbo_h_l3s_calls);
#endif
            rbo_l3s_call_step((void *)o_step, g_iface, in, zb, &rbo_l3s_ctx);
        } else
        l3_call_step((void *)o_step, g_iface, in, zb);
    } else {
        if (g_scrub && g_cfg.clean_zero) zero_below_caller(16384);   /* last thing before the step: scrub/QPC frames gone */
        o_step(g_iface, in);
    }
    if (g_vm_body_on) vm_step_end();
    if (g_fp_canon_active || (g_cfg.fp_guard && muted)) { _mm_setcsr(fp_mx0); fp_setcw(fp_cw0); }   /* leave the thread as we found it (UE's values) */
    run_step_c0b_leave(seq);
    g_run_muted = 0;
    g_ctx_tid = 0;
    g_ctx_flags = 0;
    g_last_action = "step:after";
    if (proxied) {
        if (*lst == (void *)&g_proxy) *lst = real;
        else g_listener_changed++;
    }
}
void run_fi(FrameIn *f, int muted, uint64_t seq, uint32_t flags) { StepIn in; fi_make(&in, f); run_step(&in, muted, seq, flags); }

/* ------------------------------------------------------------------ reports */
static int g_reports;   /* detailed mismatch reports this battle */

/* names of known core fields (GATE-A + round 1); VAs at the preferred base */
typedef struct { uint64_t lo, hi; const char *name; } FieldName;
static const FieldName k_core_fields[] = {
    { 0x1440f4a84, 0x1440f4a88, "camera_throw_gate" },   /* v0.20.2 region "camgate" */
    { 0x14470d0c0, 0x14470d0c8, "frame_flags/frame_counter" },
    { 0x14470d0dc, 0x14470d0e0, "camera_yaw_turns(director 0x14470ea0c/360)" },
    { 0x14470d100, 0x14470d120, "third_input_block_copy" },
    { 0x14470d188, 0x14470d190, "listener_ptr" },
    { 0x14470d190, 0x14470d198, "resource_ptr(ifc+0x28)" },
    { 0x14470d198, 0x14470d200, "camera_output(0x1402dc0c0 from camera director)" },
    { 0x14470d200, 0x14470ddf0, "globals_block(SaveState 0xbf0)" },
    { 0x14470de90, 0x14470dea0, "chara_table" },
    { 0x14470e180, 0x14470e1c0, "camera_matrix_e180(0x14031e950)" },
    { 0x14470e890, 0x14470e9b0, "effect/scrape block(0x14470e890 cleared in Setup)" },
    { 0x14470e9b0, 0x14470e9e0, "UE-fed block e9b0 (foreign writer)" },
    { 0x14470e9e0, 0x14470e9f0, "cmd_list(ifc+0x58)" },
    { 0x14470ec60, 0x14470ece0, "camera_layers[16]" },
    { 0x14470ece0, 0x14470ed00, "camera_shake_state" },
    { 0x14470ed1c, 0x14470ed28, "camera_active_layer/blend" },
    { 0x14470e9f0, 0x14470ed50, "GT_CameraDirectorClass" },
    { 0x14470ed50, 0x14470ee90, "camera_obj_ed50" },
    { 0x14470ee90, 0x14470f180, "camera_obj_ee90" },
    { 0x14470f180, 0x14470f198, "camera_ptrs" },
    { 0x14470f1c0, 0x14470f1d0, "emitter_list" },
    { 0x14470f1d0, 0x14470f310, "rule_block(ifc+0x48)" },
    { 0x14470f320, 0x14470f388, "resources(ifc+0x28)" },
    { 0x144715400, 0x1447154c0, "player_controllers(2x0x60)" },
    { 0x1447155c0, 0x1447156f0, "init_objects_155c0/15658" },
    { 0x144843ed0, 0x144843f10, "battle_mode/workmem_owner" },
    { 0x1448462d0, 0x144846328, "timing_block" },
    { 0x144846330, 0x144846470, "globals_846330" },
    { 0x144855700, 0x144855710, "step_input_words" },
    { 0x144855710, 0x144856728, "core_allocator" },
    { 0x14485eb30, 0x14485eb98, "RNG(TT800)" },
    { 0x144100b30, 0x144100b38, "NullListener" },
    { 0x144100b38, 0x144100e60, "battle_mode_singletons" },
};
static const FieldName k_player_fields[] = {   /* first match wins */
    { 0x29300, 0x29320, "fx_vec@0x29310(0x14033423b)" },
    /* v0.21.0-S1: the X masks and the P ranges (pres.c), named so DIFF/L4GUARD readers see the class */
    { 0x3590, 0x3598, "X:std_map_head" },
    { 0x2b270, 0x2b278, "X:lip_data_ptr" },
    /* C1B3: (C1B-R3-SPEC.md §2.4.2) KEEP -> P:, POSE -> PP: (P0 offsets: the bank rows 62..96 at 0x3610; P1's bank is at 0x3620, a naming
     * slip of 16 bytes, names only) */
    { 0x3610 + 62 * 0x40, 0x3610 + 97 * 0x40, "PP:bank0_face_rows" },
    { 0x3610 + 0xc000 + 62 * 0x40, 0x3610 + 0xc000 + 97 * 0x40, "PP:bank1_face_rows" },
    { 0x3610 + 0x18000 + 62 * 0x40, 0x3610 + 0x18000 + 97 * 0x40, "PP:bank2_face_rows" },
    { 0x29120, 0x29310, "PP:face_a" },   /* v0.24.0 F2: the wind vector 0x29310..0x2931f (fx_vec above) is G now */
    { 0x29320, 0x295d0, "PP:face_c" },
    { 0x295d0, 0x29670, "P:face_w" },
    { 0x29670, 0x2aef8, "PP:face_d" },
    { 0x2aef8, 0x2b3c8, "P:face_layers" },
    { 0x95ec0, 0x95f60, "P:lipsync_ctl" },
    { 0x97268, 0x973e8, "P:voice_ctl" },
    { 0x2150, 0x2158, "input_word(facing-relative)" },
    { 0x3500, 0x3520, "time_scale" },
    { 0x3590, 0x35a0, "std::map@0x3590" },
    { 0x35a0, 0x4de0, "CMatrixBankImpl<768>" },
    { 0x27760, 0x27f60, "CMatrixBankImpl<32>" },
    { 0x2b500, 0x2b510, "std::list@0x2b500" },
    { 0x43d68, 0x44078, "CCharacterRelationTable" },
    { 0x44078, 0x44088, "list@0x44078" },
    { 0x444f0, 0x45228, "check_block(0x444f0)" },
    { 0x455c8, 0x45600, "CHeaderHitOffset" },
};
static const char *field_name(uintptr_t va, uint64_t *rel) {
    uint64_t p = va - g_base + IMG_PREF;
    uint64_t p0 = A_PLAYER0;
    if (p >= p0 && p < p0 + 2 * PLAYER_STRIDE) {
        uint64_t o = (p - p0) % PLAYER_STRIDE;
        for (size_t i = 0; i < sizeof k_player_fields / sizeof k_player_fields[0]; i++)
            if (o >= k_player_fields[i].lo && o < k_player_fields[i].hi) { *rel = o - k_player_fields[i].lo; return k_player_fields[i].name; }
        *rel = o;
        return "PLAYER(unnamed)";
    }
    for (size_t i = 0; i < sizeof k_core_fields / sizeof k_core_fields[0]; i++)
        if (p >= k_core_fields[i].lo && p < k_core_fields[i].hi) { *rel = p - k_core_fields[i].lo; return k_core_fields[i].name; }
    *rel = 0;
    return "-";
}

static int class_of_va(uint64_t p);
typedef struct { const char *ctx; uint64_t seq; uint64_t k; int n; const Snap *ref; int values; int only_cls; const void *ignore; int limit; } DiffCtx;
static int fp_covers(const void *fpv, uintptr_t a, size_t len);
static void hexs(char *o, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) snprintf(o + i * 2, 3, "%02x", p[i]); o[n * 2] = 0; }
static void diff_cb(void *u, int region, size_t off, size_t len, uintptr_t addr) {
    DiffCtx *c = (DiffCtx *)u;
    char d[96];
    const char *rn;
    const uint8_t *live = NULL, *ref = NULL;
    uintptr_t va = addr;
    if (region >= 0) {
        rn = g_ss.reg[region].name; describe_va(addr, d, sizeof d);
        live = (const uint8_t *)addr; ref = c->ref->buf[region] + off;
    } else if (region == -1) {
        rn = "arena"; snprintf(d, sizeof d, "arena+0x%zx", off);
        live = g_arena.base + off; ref = off < c->ref->alen ? c->ref->abuf + off : NULL;
    } else {
        rn = "pins";
        size_t acc = 0; int i = 0;
        for (; i < g_ss.npin && acc + g_ss.pins[i].len <= off; i++) acc += g_ss.pins[i].len;
        if (i < g_ss.npin) { va = g_ss.pins[i].addr + (off - acc); describe_va(va, d, sizeof d); cam_heap_name(va, d, sizeof d); live = (const uint8_t *)va; ref = c->ref->pbuf + off; }
        else snprintf(d, sizeof d, "pin?");
    }
    if (c->n >= c->limit) return;
    if (c->only_cls >= 0) {
        int cls = region == -1 || region == -2 ? CL_OTHER : class_of_va(va - g_base + IMG_PREF);
        if (cls < 0) cls = !strcmp(rn, "workmem") ? CL_REPLAY : CL_OTHER;
        if (cls != c->only_cls) return;
    }
    if (c->ignore && fp_covers(c->ignore, va, len)) return;   /* the next frame's between-step write, not a resim difference */
    uint64_t rel = 0;
    const char *fname = region == -1 ? "-" : field_name(va, &rel);
    /* v0.19: gameplay reports print the full range (up to 256 bytes; trunc=1 beyond), others 16 bytes */
    static char lh[2 * 256 + 1], rh[2 * 256 + 1]; char fv[64] = "";
    lh[0] = rh[0] = 0;
    size_t cap = c->only_cls == CL_GAME ? 256 : 16;
    const char *tr = c->only_cls == CL_GAME && len > cap ? " trunc=1" : "";
    if (c->values && live && ref) {
        size_t n = len < cap ? len : cap;
        hexs(lh, live, n); hexs(rh, ref, n);
        if (len >= 4 && !((va) & 3)) {
            float a, b; memcpy(&a, live, 4); memcpy(&b, ref, 4);
            snprintf(fv, sizeof fv, " f32=%g/%g", (double)a, (double)b);
        }
    }
    LOG("DIFF ctx=%s seq=%llu k=%llu region=%s off=0x%zx len=%zu at=%s field=%s+0x%llx%s%s%s%s%s%s\n", c->ctx, (unsigned long long)c->seq,
        (unsigned long long)c->k, rn, off, len, d, fname, (unsigned long long)rel,
        lh[0] ? " now=" : "", lh, rh[0] ? " ref=" : "", rh, fv, tr);
    c->n++;
}

/* v0.21.0-S1: what a DIFF line was diffed against. The unified roll7 path diffs resim k < s against the NEXT frame's pre-snapshot,
 * which under r7_commit is the COMMITTED post-state of frame k, while the check hashes compare with frame k's LIVE post-state.
 * When that commit left a residue (resid has the class), the DIFF of that class is against the committed state and can show
 * nothing (gameplay_ranges_logged=0) or other ranges than the check saw. k = s and the other paths diff against the live post-state. */
static int g_diff_ref = -1;   /* -1 live post-state; >= 0 the committed post-state of frame k, with this residue class mask */
static void class_str(int mask, char out[NCL + 1]);
static void diff_ref_tag(char *o, size_t cap) {
    if (g_diff_ref < 0) { snprintf(o, cap, " ref=live"); return; }
    char rs[NCL + 1]; class_str(g_diff_ref, rs);
    snprintf(o, cap, " ref=committed resid=%s", rs);
}
static size_t report_diff(const char *ctx, uint64_t seq, uint64_t k, const Snap *ref) {
    if (g_reports >= g_cfg.max_reports) {
        int nr = 0;
        return snap_diff_live(&g_ss, ref, NULL, NULL, 0, &nr);
    }
    g_reports++;
    DiffCtx c = { ctx, seq, k, 0, ref, g_reports <= 8, -1, NULL, g_cfg.max_diff_ranges };
    int nr = 0;
    size_t nb = snap_diff_live(&g_ss, ref, diff_cb, &c, 1 << 20, &nr);
    char rt[48]; diff_ref_tag(rt, sizeof rt);
    LOG("DIFFSUM ctx=%s seq=%llu k=%llu bytes=%zu ranges=%d%s\n", ctx, (unsigned long long)seq, (unsigned long long)k, nb, nr, rt);
    return nb;
}

/* gameplay-only detailed reports (own budget), with values; ignore = bytes a later between-step
 * write explains (roll7 reference artifact) */
static int g_greports;
static void report_gameplay(const char *ctx, uint64_t seq, uint64_t k, const Snap *ref, const void *ignore) {
    if (g_greports >= g_cfg.greport_budget) return;   /* v0.19: budget per path segment (reset by seg_reset) */
    g_greports++;
    char cx[48]; snprintf(cx, sizeof cx, "%s_G", ctx);
    DiffCtx c = { cx, seq, k, 0, ref, 1, CL_GAME, ignore, 32 };
    int nr = 0;
    snap_diff_live(&g_ss, ref, diff_cb, &c, 1 << 20, &nr);
    char rt[48]; diff_ref_tag(rt, sizeof rt);
    LOG("DIFFSUM ctx=%s seq=%llu k=%llu gameplay_ranges_logged=%d%s\n", cx, (unsigned long long)seq, (unsigned long long)k, c.n, rt);
}

/* ------------------------------------------------------------------ state classes (round 3)
 * Every snapshotted byte belongs to one class; comparisons are reported per class so a
 * gameplay mismatch can never hide behind camera/effects drift. VAs at the preferred base. */

static int class_of_va(uint64_t p) {   /* p = preferred-base VA */
    if (p >= A_PLAYER0 && p < A_PLAYER0 + 2 * PLAYER_STRIDE) {
        uint64_t o = (p - A_PLAYER0) % PLAYER_STRIDE;
        /* fx_vec written only by effects code; v0.21.0-S1: the presentation class P (pres.c) hashes as F.
         * C1B3: the hash class is frozen at the v0.24 P table (k_pres_hashfx_v024): KEEP / POSE do not move any tier-2 hash */
        return (o >= 0x29300 && o < 0x29320) || pres_hashfx_index(o) >= 0 ? CL_FX : CL_GAME;
    }
    for (size_t i = 0; i < sizeof k_cl_ranges / sizeof k_cl_ranges[0]; i++)
        if (p >= k_cl_ranges[i].lo && p < k_cl_ranges[i].hi) return k_cl_ranges[i].cls;
    return -1;
}

/* class segments of the fixed regions, rebuilt with the snapshot set */
typedef struct { int region; size_t off, len; int cls; } Seg;
static Seg g_seg[2048];
static int g_nseg;
void seg_build(void) {
    g_nseg = 0;
    for (int r = 0; r < g_ss.nreg && g_nseg < 2040; r++) {
        const char *nm = g_ss.reg[r].name;
        int def = !strcmp(nm, "workmem") ? CL_REPLAY : CL_OTHER;
        int in_img = reg_in_img(nm);
        if (!in_img) { g_seg[g_nseg].region = r; g_seg[g_nseg].off = 0; g_seg[g_nseg].len = g_ss.reg[r].len; g_seg[g_nseg].cls = def; g_nseg++; continue; }
        uint64_t base = g_ss.reg[r].addr - g_base + IMG_PREF;
        size_t len = g_ss.reg[r].len, off = 0;
        while (off < len && g_nseg < 2040) {
            /* walk in 4-byte steps to find class boundaries (fast enough at build time: ~1.3 MB / 4) */
            int c = class_of_va(base + off); if (c < 0) c = def;
            size_t e = off + 4;
            while (e < len) { int c2 = class_of_va(base + e); if (c2 < 0) c2 = def; if (c2 != c) break; e += 4; }
            if (e > len) e = len;
            g_seg[g_nseg].region = r; g_seg[g_nseg].off = off; g_seg[g_nseg].len = e - off; g_seg[g_nseg].cls = c; g_nseg++;
            off = e;
        }
    }
}

/* per-class hashes of live memory (sn == NULL) or of a snapshot */
static void class_hashes(const Snap *sn, uint64_t out[NCL]) {
    for (int c = 0; c < NCL; c++) out[c] = 0x5C60 + (uint64_t)c;
    for (int i = 0; i < g_nseg; i++) {
        Seg *s = &g_seg[i];
        uintptr_t live = g_ss.reg[s->region].addr + s->off;
        const uint8_t *view = sn ? sn->buf[s->region] + s->off : (const uint8_t *)live;
        out[s->cls] = snap_hash_range(&g_ss, view, live, s->len, out[s->cls]);
    }
    if (g_ss.arena) {
        if (sn) out[CL_OTHER] = snap_hash_range(&g_ss, sn->abuf, (uintptr_t)g_ss.arena->base, sn->alen, out[CL_OTHER] ^ 0xA);
        else {
            spin_lock((Spin *)g_ss.arena_lock);
            out[CL_OTHER] = snap_hash_range(&g_ss, g_ss.arena->base, (uintptr_t)g_ss.arena->base, arena_used(g_ss.arena), out[CL_OTHER] ^ 0xA);
            spin_unlock((Spin *)g_ss.arena_lock);
        }
    }
    if (g_ss.npin) {
        if (sn) out[CL_OTHER] = hash_bytes(sn->pbuf, sn->plen, out[CL_OTHER] ^ 0xB);
        else {
            uint8_t *o = g_ss.pin_scratch;
            for (int i = 0; i < g_ss.npin; i++) { memcpy(o, (const void *)g_ss.pins[i].addr, g_ss.pins[i].len); o += g_ss.pins[i].len; }
            out[CL_OTHER] = hash_bytes(g_ss.pin_scratch, g_ss.pin_bytes, out[CL_OTHER] ^ 0xB);
        }
    }
}
static int class_diff(const uint64_t a[NCL], const uint64_t b[NCL]) { int m = 0; for (int c = 0; c < NCL; c++) if (a[c] != b[c]) m |= 1 << c; return m; }
/* v0.22.0-C1a accessors (c1a_run.c): the snapshot set and its class segments */
const SnapSet *modes_snapset(void) { return &g_ss; }
SnapSet *modes_ssw(void) { return &g_ss; }                                   /* v0.23.0-C1b accessors (rb.c) */
Spin *modes_pin_lock(void) { return &g_pin_lock; }
uint64_t modes_lsn_c0_calls(void) { return g_bar_lsn_calls; }
int modes_cam_fail(void) { return g_cam_fail; }
int modes_ring_size(void) { int need = g_cfg.roll_depth + 2; if (g_cfg.fake_delay + 2 > need) need = g_cfg.fake_delay + 2; if (need < 8) need = 8; if (need > RING_MAX) need = RING_MAX; return need; }
int modes_ss_ready(void) { return g_ss_ready; }
void modes_seg_iter(modes_seg_cb cb, void *u) { for (int i = 0; i < g_nseg; i++) cb(u, g_seg[i].region, g_seg[i].off, g_seg[i].len, k_cl_char[g_seg[i].cls]); }
static void class_str(int mask, char out[NCL + 1]) { for (int c = 0; c < NCL; c++) out[c] = (mask & (1 << c)) ? k_cl_char[c] : '-'; out[NCL] = 0; }

/* ------------------------------------------------------------------ camera policy (round 3)
 * Camera modes are driven by core state every step: the director (0x14470e9f0) runs inside the
 * step; active layer 0x14470ed1c, 16 layer pointers 0x14470ec60, blend timer ed20/ed24, shake
 * state ecf4/ecf8. The camera stays in every snapshot (resims are exact). camera=exclude decides
 * what is committed after a loopback rollback: the live camera is kept (no jump) unless the
 * resimulated camera MODE differs (active layer, layer pointers, shake state), in which case the
 * resimulated camera is adopted with camera_resync=1 (a cinematic started or cancelled by the
 * rollback, at the right frame), and always when layer pointers differ (they could point at
 * objects the rollback freed). camera=include commits the resimulated camera as is. CAMSYNC lines
 * log every mode difference (roll7: check only, the live result is always kept). */
static uint8_t g_cam_save[0x7a8 + 0x60 + 0x40 + 4 + 64];
static uint8_t g_cam_resim[sizeof g_cam_save];
static int g_cam_saved;
static uint64_t g_camsync_n, g_camsync_mismatch, g_camsync_adopt;
volatile int g_pending_cam = -1;
static void cam_copy_out(uint8_t *dst) {
    size_t o = 0;
    for (int i = 0; i < NCAMR; i++) { size_t n = (size_t)(k_cam_ranges[i].hi - k_cam_ranges[i].lo); memcpy(dst + o, (void *)VA(k_cam_ranges[i].lo), n); o += n; }
}
static void cam_copy_in(const uint8_t *src) {
    size_t o = 0;
    for (int i = 0; i < NCAMR; i++) { size_t n = (size_t)(k_cam_ranges[i].hi - k_cam_ranges[i].lo); memcpy((void *)VA(k_cam_ranges[i].lo), src + o, n); o += n; }
}
static void cam_save(void) { cam_copy_out(g_cam_save); g_cam_saved = 1; }
/* offset of a camera VA inside the saved buffer */
static size_t cam_off(uint64_t va) {
    size_t o = 0;
    for (int i = 0; i < NCAMR; i++) { if (va >= k_cam_ranges[i].lo && va < k_cam_ranges[i].hi) return o + (size_t)(va - k_cam_ranges[i].lo); o += (size_t)(k_cam_ranges[i].hi - k_cam_ranges[i].lo); }
    return 0;
}
typedef struct { int32_t layer; uint64_t ptrs[16]; int32_t shake; int32_t shake_t; float blend_t; } CamMode;
static void cam_mode_of(const uint8_t *buf, CamMode *m) {
    memcpy(&m->layer, buf + cam_off(0x14470ed1c), 4);
    memcpy(m->ptrs, buf + cam_off(0x14470ec60), sizeof m->ptrs);
    memcpy(&m->shake, buf + cam_off(0x14470ecf4), 4);
    memcpy(&m->shake_t, buf + cam_off(0x14470ecf8), 4);
    memcpy(&m->blend_t, buf + cam_off(0x14470ed24), 4);
}
/* after test runs (sync1x/sync1) or a resim: put the live camera back */
static void cam_put_back(const char *ctx, uint64_t seq, int is_rollback, int allow_adopt) {
    if (!g_cam_saved) return;
    g_cam_saved = 0;
    if (!is_rollback) { cam_copy_in(g_cam_save); return; }
    if (!g_cfg.camera_exclude && allow_adopt) return;   /* camera=include: the resimulated camera is committed as is */
    cam_copy_out(g_cam_resim);
    CamMode live, resim;
    cam_mode_of(g_cam_save, &live); cam_mode_of(g_cam_resim, &resim);
    int ptrs_differ = memcmp(live.ptrs, resim.ptrs, sizeof live.ptrs) != 0;
    int differ = live.layer != resim.layer || live.shake != resim.shake || ptrs_differ;
    g_camsync_n++;
    if (differ) g_camsync_mismatch++;
    int adopt = allow_adopt && differ && (g_cfg.camera_resync || ptrs_differ);
    if (differ && g_camsync_mismatch <= 400)
        LOG("CAMSYNC ctx=%s seq=%llu live_layer=%d resim_layer=%d live_shake=%d/%d resim_shake=%d/%d live_blend=%g resim_blend=%g layer_ptrs_differ=%d action=%s\n",
            ctx, (unsigned long long)seq, live.layer, resim.layer, live.shake, live.shake_t, resim.shake, resim.shake_t,
            (double)live.blend_t, (double)resim.blend_t, ptrs_differ, adopt ? "adopt_resim_camera" : "keep_live_camera");
    if (adopt) { g_camsync_adopt++; return; }   /* keep the resimulated camera */
    cam_copy_in(g_cam_save);
}

/* ------------------------------------------------------------------ v0.20.1 camera keep-live under r7_commit
 * C0 session 2 (sc6rb_20261004_035456_14436.log): with r7_commit the resim result is kept, and the resim
 * camera can differ from the live one (resim-vs-live C mismatches exist in every session, L4 on or off;
 * at the onsets the resim camera sat at one value for every k while the live camera moved). Once
 * committed, the broken camera persisted until Setup re-initialized the director. The camera is
 * per-player presentation: after the resims the live camera (the core camera ranges, the camera layer
 * buffers and the camera function slots) is put back, unless the resimulated camera MODE differs
 * (active layer, layer pointers, shake state; camera_resync rule) or a differing qword holds a pointer
 * into the image (vtable, function) or the arena: then the resimulated camera is adopted as a whole.
 * v0.20.2: OFF by default (r7_cam_keep=0). Gameplay reads the camera (FUN_140312510 sets PLAYER+0x124 from the
 * dominant layer and the camera eye), so keeping the live camera mixes the old camera with the new gameplay; the
 * camera is rolled back with the game instead (whole camera function block + gate pinned). With r7_cam_keep=1 the
 * kept set covers the whole block (when pinned whole) and the camera gate. */
#define CK_MAXB 64
typedef struct { uintptr_t addr; uint32_t len; uint8_t kind; } CkBlk;   /* kind 0 core range (and the v0.20.2 camera gate), 1 layer buffer, 2 function block or slot */
static CkBlk g_ck_blk[CK_MAXB]; static int g_ck_n;
static uint8_t *g_ck_live, *g_ck_resim; static size_t g_ck_cap, g_ck_len;
static int g_ck_saved, g_ck_lines;
static char g_ck_act = '-';   /* per R7 line: - equal or off, K live camera put back, A resim camera adopted, x not saved */
static uint64_t g_ck_frames, g_ck_differ, g_ck_kept, g_ck_adopt_mode, g_ck_adopt_img, g_ck_adopt_arena, g_ck_bytes[3], g_ck_skipped;
static int ck_on(void) { return g_cfg.r7_commit && g_cfg.r7_cam_keep; }
static void ck_add(uintptr_t a, uint32_t len, uint8_t kind) {
    if (g_ck_n >= CK_MAXB || !len) return;
    if (kind) {   /* heap blocks only while a pin covers them (snapshotted, not freed) */
        int pi = pin_index_of(a);
        if (pi < 0 || a + len > g_ss.pins[pi].addr + g_ss.pins[pi].len) { g_ck_skipped++; return; }
    }
    g_ck_blk[g_ck_n].addr = a; g_ck_blk[g_ck_n].len = len; g_ck_blk[g_ck_n].kind = kind; g_ck_n++;
}
static int ck_build(void) {
    g_ck_n = 0;
    for (int i = 0; i < NCAMR; i++) ck_add(VA(k_cam_ranges[i].lo), (uint32_t)(k_cam_ranges[i].hi - k_cam_ranges[i].lo), 0);   /* same layout as cam_off() */
    ck_add(VA(0x14470d0dcULL), 4, 0);   /* camera yaw in turns (class C, outside k_cam_ranges) */
    for (int i = 0; i < g_ncam_bufs; i++) ck_add(g_cam_bufs[i], g_cam_buf_sz, 1);
    for (int b = 0; b < g_ncamf; b++) {   /* v0.20.2: the whole block when it is pinned whole, else the 4 slot heads; v0.21.1: nothing when not pinned (2) */
        if (g_camf_full[b] == 1) ck_add(g_camf_base[b], CAMF_SIZE, 2);
        else if (g_camf_full[b] == 0) for (int q = 0; q < 4; q++) ck_add(g_camf_base[b] + k_slot_off[q], k_slot_len[q], 2);
    }
    if (mem_readable((void *)VA(A_CAM_GATE), 4)) ck_add(VA(A_CAM_GATE), 4, 0);   /* v0.20.2: the throw camera gate (region "camgate") */
    size_t n = 0;
    for (int i = 0; i < g_ck_n; i++) n += g_ck_blk[i].len;
    if (n > g_ck_cap) {
        if (g_ck_live) VirtualFree(g_ck_live, 0, MEM_RELEASE);
        if (g_ck_resim) VirtualFree(g_ck_resim, 0, MEM_RELEASE);
        size_t cap = (n + 0xffff) & ~(size_t)0xffff;
        g_ck_live = (uint8_t *)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_ck_resim = (uint8_t *)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_ck_cap = g_ck_live && g_ck_resim ? cap : 0;
    }
    g_ck_len = n;
    return g_ck_cap >= n && n > 0;
}
static void ck_copy_out(uint8_t *dst) {
    size_t o = 0;
    for (int i = 0; i < g_ck_n; i++) { memcpy(dst + o, (const void *)g_ck_blk[i].addr, g_ck_blk[i].len); o += g_ck_blk[i].len; }
}
/* after the live pass of a commit frame: the live camera */
static void ck_save(void) {
    g_ck_saved = 0;
    if (!ck_on()) { g_ck_act = '-'; return; }
    if (!ck_build()) { g_ck_act = 'x'; return; }
    ck_copy_out(g_ck_live);
    g_ck_saved = 1; g_ck_act = '-';
}
static int ck_is_ptr(uint64_t v) {
    if (IN_IMAGE(v)) return 1;
    if (g_arena.base && v >= (uint64_t)(uintptr_t)g_arena.base && v < (uint64_t)(uintptr_t)g_arena.base + g_arena.cap) return 2;
    return 0;
}
/* after the resims of a commit frame: put the live camera back, or adopt the resimulated one.
 * Returns 1 when live bytes were written back (the caller re-hashes the classes for the residue). */
static int ck_apply(uint64_t s) {
    if (!g_ck_saved) return 0;
    g_ck_saved = 0;
    g_ck_frames++;
    ck_copy_out(g_ck_resim);
    if (!memcmp(g_ck_live, g_ck_resim, g_ck_len)) return 0;
    g_ck_differ++;
    CamMode live, resim;
    cam_mode_of(g_ck_live, &live); cam_mode_of(g_ck_resim, &resim);
    int ptrs_differ = memcmp(live.ptrs, resim.ptrs, sizeof live.ptrs) != 0;
    int mode = live.layer != resim.layer || live.shake != resim.shake || ptrs_differ;
    int why = mode && (g_cfg.camera_resync || ptrs_differ) ? 1 : 0;   /* 1 mode, 2 image pointer, 3 arena pointer */
    size_t db[3] = { 0, 0, 0 }, o = 0;
    for (int i = 0; i < g_ck_n; i++) {
        const CkBlk *b = &g_ck_blk[i];
        const uint8_t *L = g_ck_live + o, *R = g_ck_resim + o;
        for (uint32_t k = 0; k < b->len; k++) if (L[k] != R[k]) db[b->kind]++;
        for (uintptr_t a = (b->addr + 7) & ~(uintptr_t)7; !why && a + 8 <= b->addr + b->len; a += 8) {
            uint64_t lv, rv; memcpy(&lv, L + (a - b->addr), 8); memcpy(&rv, R + (a - b->addr), 8);
            if (lv == rv) continue;
            int pl = ck_is_ptr(lv), pr = ck_is_ptr(rv);
            if (pl == 1 || pr == 1) why = 2; else if (pl == 2 || pr == 2) why = 3;
        }
        o += b->len;
    }
    if (why) {
        if (why == 1) g_ck_adopt_mode++; else if (why == 2) g_ck_adopt_img++; else g_ck_adopt_arena++;
        g_ck_act = 'A';
        if (g_ck_lines++ < 60)
            LOG("CAMKEEP seq=%llu fr=%u action=adopt_resim_camera why=%s live_layer=%d resim_layer=%d live_shake=%d resim_shake=%d layer_ptrs_differ=%d bytes core=%zu layer=%zu func=%zu\n",
                (unsigned long long)s, core_frame(), why == 1 ? "mode" : why == 2 ? "image_pointer" : "arena_pointer", live.layer, resim.layer, live.shake, resim.shake,
                ptrs_differ, db[0], db[1], db[2]);
        return 0;
    }
    o = 0;
    for (int i = 0; i < g_ck_n; i++) { memcpy((void *)g_ck_blk[i].addr, g_ck_live + o, g_ck_blk[i].len); o += g_ck_blk[i].len; }
    g_ck_kept++; for (int k = 0; k < 3; k++) g_ck_bytes[k] += db[k];
    g_ck_act = 'K';
    if (g_ck_lines++ < 60)
        LOG("CAMKEEP seq=%llu fr=%u action=keep_live_camera bytes core=%zu layer=%zu func=%zu\n", (unsigned long long)s, core_frame(), db[0], db[1], db[2]);
    return 1;
}
static void ck_report(const char *why) {
    if (!g_cfg.r7_commit) return;
    LOG("R7CAMKEEP why=%s on=%d frames=%llu differ=%llu kept_live=%llu adopt_mode=%llu adopt_image_ptr=%llu adopt_arena_ptr=%llu bytes_core=%llu bytes_layer=%llu bytes_func=%llu blocks=%d block_bytes=%zu skipped_block_frames=%llu\n",
        why, ck_on(), (unsigned long long)g_ck_frames, (unsigned long long)g_ck_differ, (unsigned long long)g_ck_kept, (unsigned long long)g_ck_adopt_mode,
        (unsigned long long)g_ck_adopt_img, (unsigned long long)g_ck_adopt_arena, (unsigned long long)g_ck_bytes[0], (unsigned long long)g_ck_bytes[1],
        (unsigned long long)g_ck_bytes[2], g_ck_n, g_ck_len, (unsigned long long)g_ck_skipped);
}

/* ------------------------------------------------------------------ v0.21.0-S1 presentation keep-live (pres_keep=1, r7_commit=1)
 * The v0.20.1 camera keep-live generalized to the P range table (pres.c), both players. P is snapshotted and restored at
 * k0 like everything else (resims are exact), so a resim that runs muted (UE not called) leaves P without UE's lip-sync
 * writes, and the step advances the lip-sync time once per pass. After the live pass the P ranges are saved; after the
 * resims they are put back (lip-sync plays on the committed lineage), range by range, unless a differing 8-byte word holds
 * an image or arena pointer in either version (the arena is rolled back: a live arena pointer may point at freed or
 * reused memory): then that range keeps the resim bytes (PRESKEEP adopt line). The camera itself stays rolled back.
 * v0.21.1 (pres_ptr_rule=arena, the default): only an ARENA pointer keeps the resim bytes. An image pointer (the vtable of
 * an inline object such as the lip-sync controller UE constructs at PLAYER+0x95ed0 at voice starts: live = vtable,
 * resim = 0) keeps the live bytes; the S1 session lost the controller at every voice start to the old rule.
 * pres_ptr_rule=image_arena is the v0.21.0 rule (the negative control). */
#define PK_MAXB 16
static PresBlk g_pk_blk[PK_MAXB]; static int g_pk_n;
static uint8_t *g_pk_live;
static int g_pk_saved, g_pk_lines;
static char g_pk_act = '-';   /* per R7 line: - equal or off, K live P put back, A a range kept the resim P (pointer), x not saved */
static uint64_t g_pk_frames, g_pk_differ, g_pk_kept, g_pk_adopt, g_pk_bytes;
static int pk_on(void) { return g_cfg.pres_keep && (g_cfg.r7_commit || is_rbcore_mode(g_mode)); }   /* v0.23.0-C1b: rbreplay commits by construction */
static int pk_build(void) {
    if (g_pk_n) return g_pk_live != NULL;
    size_t n = 0;
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < k_pres_np && g_pk_n < PK_MAXB; i++) {
            g_pk_blk[g_pk_n].addr = VA(A_PLAYER0) + (uintptr_t)c * PLAYER_STRIDE + k_pres_p[i].lo;
            g_pk_blk[g_pk_n].len = k_pres_p[i].hi - k_pres_p[i].lo;
            n += g_pk_blk[g_pk_n].len; g_pk_n++;
        }
    g_pk_live = (uint8_t *)VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return g_pk_live != NULL;
}
void pk_save(void) {
    g_pk_saved = 0;
    if (!pk_on()) { g_pk_act = '-'; return; }
    if (!pk_build()) { g_pk_act = 'x'; return; }
    size_t o = 0;
    for (int i = 0; i < g_pk_n; i++) { memcpy(g_pk_live + o, (const void *)g_pk_blk[i].addr, g_pk_blk[i].len); o += g_pk_blk[i].len; }
    g_pk_saved = 1; g_pk_act = '-';
}
static int pk_is_ptr(void *u, uint64_t v) { (void)u; return pres_ptr_filter(ck_is_ptr(v), g_cfg.pres_ptr_rule); }   /* v0.21.1: the rule */
static int g_pkd_adopted[PK_MAXB];
static void pk_adopt(void *u, int block, size_t off, uint64_t lv, uint64_t rv, int kind) {
    uint64_t s = *(const uint64_t *)u;
    if (block >= 0 && block < PK_MAXB) g_pkd_adopted[block] = 1;
    if (g_pk_lines++ >= 60) return;
    char d[96]; describe_va(g_pk_blk[block].addr + off, d, sizeof d);
    LOG("PRESKEEP seq=%llu fr=%u action=adopt_resim range=%s at=%s live=0x%llx resim=0x%llx why=%s (this P range keeps the resim bytes)\n",
        (unsigned long long)s, core_frame(), k_pres_p[block % k_pres_np].name, d, (unsigned long long)lv, (unsigned long long)rv, kind == 1 ? "image_pointer" : "arena_pointer");
}
/* v0.24.0-C1b2 F2.2 (C1B-R2-SPEC.md §2.3): PRESKEEPD, budget 60 per battle. At a pk_apply that puts a differing block back ('K'), the first 8
 * differing 16-byte granules of each kept block (PLAYER-relative offset, hex, : length). A granule in face_a / face_b outside the known
 * lip-sync / face offsets is a FINDING to investigate (the PK-CARRY family: a field step s writes and step s+1 reads, carried by pres_keep). */
static int g_pkd_lines;
static uint8_t g_pkd_diff[PK_MAXB];
static void pkd_note(uint64_t s) {
    if (lipsync_pkd_note(s, g_pk_live, g_pkd_diff, g_pk_n)) return;   /* LIPSYNC: with lip-sync on: by-design granules counted, per-block budgets */
    size_t o = 0;
    for (int i = 0; i < g_pk_n; i++) {
        const uint8_t *L = g_pk_live + o, *M = (const uint8_t *)g_pk_blk[i].addr;
        uint32_t len = g_pk_blk[i].len;
        o += len;
        if (!g_pkd_diff[i] || g_pkd_lines >= 60) continue;
        int c = i / k_pres_np; uint32_t lo = k_pres_p[i % k_pres_np].lo, hi = lo + len;
        char gs[400]; size_t go = 0; int n = 0;
        for (uint32_t g = lo & ~15u; g < hi; g += 16) {
            uint32_t a = g < lo ? lo : g, b = g + 16 > hi ? hi : g + 16;
            if (!memcmp(L + (a - lo), M + (a - lo), b - a)) continue;
            n++;
            if (n <= 8) { int k = snprintf(gs + go, sizeof gs - go, "%s%x:%u", go ? "," : "", a, b - a); if (k > 0 && (size_t)k < sizeof gs - go) go += (size_t)k; }
        }
        if (!n) continue;
        g_pkd_lines++;
        LOG("PRESKEEPD s=%llu p=%d blk=%s n=%d g=%s\n", (unsigned long long)s, c, k_pres_p[i % k_pres_np].name, n, go ? gs : "-");
    }
}
/* after the resims of a commit frame. Returns 1 when live bytes were written back (the caller re-hashes the classes). */
int pk_apply(uint64_t s) {
    if (!g_pk_saved) return 0;
    g_pk_saved = 0;
    g_pk_frames++;
    PresKeep r;
    {   /* v0.24.0 F2.2: which blocks differ (before pres_keep_apply puts them back) */
        size_t o = 0;
        for (int i = 0; i < g_pk_n; i++) { g_pkd_diff[i] = (uint8_t)(memcmp(g_pk_live + o, (const void *)g_pk_blk[i].addr, g_pk_blk[i].len) != 0); o += g_pk_blk[i].len; g_pkd_adopted[i] = 0; }
        if (g_pkd_lines < 60) {   /* the granules are read from memory before the apply; a block that is adopted is not "kept" (dropped below) */
            for (int i = 0; i < g_pk_n; i++) if (g_pkd_diff[i]) { pkd_note(s); break; }
        }
    }
    lipsync_pk_fix(g_pk_live, s);   /* LIPSYNC: a live P word pointing into a block freed in the resims keeps the resim value */
    char a = pres_keep_apply(g_pk_blk, g_pk_n, g_pk_live, pk_is_ptr, NULL, pk_adopt, &s, &r);
    g_pk_act = a;
    if (r.differ) g_pk_differ++;
    if (r.kept) { g_pk_kept++; g_pk_bytes += r.bytes; }
    if (r.adopted) g_pk_adopt++;
    return r.kept > 0;
}
static void pk_report(const char *why) {
    LOG("R7PRESKEEP why=%s on=%d frames=%llu differ=%llu kept=%llu adopt_ptr=%llu bytes=%llu ranges=%d range_bytes=%zu adopt_lines=%d ptr_rule=%s\n", why, pk_on(),
        (unsigned long long)g_pk_frames, (unsigned long long)g_pk_differ, (unsigned long long)g_pk_kept, (unsigned long long)g_pk_adopt,
        (unsigned long long)g_pk_bytes, 2 * k_pres_np, 2 * pres_p_bytes(), g_pk_lines, pres_rule_name(g_cfg.pres_ptr_rule));
}

/* ------------------------------------------------------------------ external heap probe (round 3)
 * For the first extw_frames sync1x frames: find heap objects referenced from core state (pointer
 * values in core .bss/.data singletons that point outside the image, arena and pins), hash a
 * 256-byte window of each after every run, and report the ones the step changes. Those are
 * state outside the snapshot (e.g. camera layer objects). */
#define EXTW_MAX 4096
#define EXTW_WIN 256
static uintptr_t g_extw_p[EXTW_MAX], g_extw_ref[EXTW_MAX];
static uint64_t g_extw_h[EXTW_MAX];
static uint32_t g_extw_hits[EXTW_MAX], g_extw_live[EXTW_MAX], g_extw_test[EXTW_MAX];
static int g_extw_n, g_extw_frames, g_extw_done, g_extw_capped;
static int extw_candidate(uintptr_t v) {
    if (v < 0x10000 || v > 0x7ffffffeffffULL || (v & 7)) return 0;
    if (IN_IMAGE(v)) return 0;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();            /* round 6: game-thread stack addresses are scratch pointers */
    if (v >= (uintptr_t)tib->StackLimit && v < (uintptr_t)tib->StackBase) return 0;
    if (g_arena_ok && arena_owns(&g_arena, (void *)v)) return 0;
    for (int i = 0; i < g_ss.nreg; i++) if (v >= g_ss.reg[i].addr && v < g_ss.reg[i].addr + g_ss.reg[i].len) return 0;
    if (pin_index_of(v) >= 0) return 0;               /* pinned ranges are part of the snapshot */
    return 1;
}
static void extw_add(uintptr_t v, uintptr_t ref) {
    if (!extw_candidate(v)) return;
    if (g_extw_n >= EXTW_MAX) { g_extw_capped++; return; }
    for (int k = 0; k < g_extw_n; k++) if (g_extw_p[k] == v) return;
    if (!mem_readable((void *)v, EXTW_WIN)) return;
    g_extw_p[g_extw_n] = v; g_extw_ref[g_extw_n] = ref; g_extw_hits[g_extw_n] = g_extw_live[g_extw_n] = g_extw_test[g_extw_n] = 0;
    g_extw_h[g_extw_n] = hash_bytes((void *)v, EXTW_WIN, 3);
    g_extw_n++;
}
static void extw_begin(void) {
    g_extw_n = 0; g_extw_frames = 0; g_extw_capped = 0;
    uintptr_t ranges[2][2] = { { VA(A_CORE_BSS_LO), VA(A_CORE_BSS_HI) }, { VA(A_DATA_SINGLE_LO), VA(A_DATA_SINGLE_HI) } };
    for (int r = 0; r < 2; r++)
        for (uintptr_t a = ranges[r][0]; a + 8 <= ranges[r][1]; a += 8) extw_add(*(uintptr_t *)a, a);
    /* second level: pointers held inside pinned blocks (camera layer buffers first) */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_ss.npin; i++) {
            int cam = is_cam_buffer(g_ss.pins[i].addr);
            if (g_ss.pins[i].len < 32 || (pass == 0) != cam) continue;
            for (uintptr_t a = g_ss.pins[i].addr; a + 8 <= g_ss.pins[i].addr + g_ss.pins[i].len; a += 8) extw_add(*(uintptr_t *)a, a);
        }
    LOG("EXTW begin candidates=%d capped=%d (heap objects referenced from core state or pinned blocks, outside every snapshot range)\n", g_extw_n, g_extw_capped);
}
/* live = after the live pass (UE listener code ran), else after a test pass (only the core ran) */
static void extw_pass(int live) {
    for (int k = 0; k < g_extw_n; k++) {
        if (!g_extw_p[k]) continue;
        if (!mem_readable((void *)g_extw_p[k], EXTW_WIN)) { g_extw_p[k] = 0; continue; }
        uint64_t h = hash_bytes((void *)g_extw_p[k], EXTW_WIN, 3);
        if (h != g_extw_h[k]) { g_extw_h[k] = h; g_extw_hits[k]++; if (live) g_extw_live[k]++; else g_extw_test[k]++; }
    }
}
/* owner of a referencing address: a fixed region field, a camera layer buffer, or a tracked block */
static void extw_owner(uintptr_t ref, char *out, size_t cap) {
    int pi = pin_index_of(ref);
    if (pi < 0) { char d[96]; describe_va(ref, d, sizeof d); uint64_t rel; const char *fn = field_name(ref, &rel); snprintf(out, cap, "%s(%s+0x%llx)", d, fn, (unsigned long long)rel); return; }
    uintptr_t base = g_ss.pins[pi].addr;
    int cb = -1;
    for (int i = 0; i < g_ncam_bufs; i++) if (g_cam_bufs[i] == base) cb = i;
    if (cb >= 0) { snprintf(out, cap, "camera_layer_buffer[%d]+0x%llx", cb, (unsigned long long)(ref - base)); return; }
    uint32_t caller = 0; int kind = 0;
    if (itrack_contains(base, &caller, &kind)) snprintf(out, cap, "tracked_block(caller=0x%x,size=%u,%s)+0x%llx", caller, g_ss.pins[pi].len, (kind & 1) ? "crt" : "fmalloc", (unsigned long long)(ref - base));
    else snprintf(out, cap, "pinned_block(size=%u)+0x%llx", g_ss.pins[pi].len, (unsigned long long)(ref - base));
}


/* Round 14 bisection: every static range the step writes (B0 write-set union of this session), minus
 * the core .bss / singletons (already snapshotted), minus the classified must-never-restore ranges,
 * minus any word also written BETWEEN steps (UE / other threads). Image memory, never freed. */
#define WSX_MAX 2048
static WsxRange g_wsx[WSX_MAX];
static int g_nwsx;
void ws_collect_hook(void) {
    if (!g_cfg.ws_statics) return;
    static WsxRange tmp[WSX_MAX];
    int nf = 0, nb = 0, nu = 0, np = 0;
    int n = ws_export_statics(tmp, WSX_MAX, &nf, &nb, &nu, &np);
    int added = 0;
    for (int i = 0; i < n; i++) {
        int k = 0;
        for (; k < g_nwsx; k++) if (tmp[i].addr < g_wsx[k].addr + g_wsx[k].len && g_wsx[k].addr < tmp[i].addr + tmp[i].len) break;
        if (k < g_nwsx) {   /* overlap: grow to the union */
            uintptr_t lo = tmp[i].addr < g_wsx[k].addr ? tmp[i].addr : g_wsx[k].addr;
            uintptr_t hi = tmp[i].addr + tmp[i].len > g_wsx[k].addr + g_wsx[k].len ? tmp[i].addr + tmp[i].len : g_wsx[k].addr + g_wsx[k].len;
            g_wsx[k].addr = lo; g_wsx[k].len = (uint32_t)(hi - lo);
            continue;
        }
        if (g_nwsx < WSX_MAX) { g_wsx[g_nwsx++] = tmp[i]; added++; }
    }
    uint64_t bytes = 0; for (int k = 0; k < g_nwsx; k++) bytes += g_wsx[k].len;
    LOG("WSX statics from the write-set: ranges=%d (new %d) bytes=%llu excluded_foreign_words=%d excluded_classified_words=%d\n", g_nwsx, added,
        (unsigned long long)bytes, nf, nb);
    LOG("WSX core=%d ranges bytes=%llu ue_excluded=%d words ptr_excluded=%d words foreign_excluded=%d words classified_excluded=%d words lsn_attrib=%d\n",
        g_nwsx, (unsigned long long)bytes, nu, np, nf, nb, g_cfg.ws_lsn_attrib);
    for (int k = 0; k < g_nwsx && k < 400; k++)
        LOG("WSXR va=0x%llx len=%u hits=%u\n", (unsigned long long)(g_wsx[k].addr - g_base + IMG_PREF), g_wsx[k].len, g_wsx[k].hits);
    if (added && g_ss_ready) g_repin_request = 1;
}
static int g_wsx_disabled;
/* kill switch: a restored static that now holds a pointer-like value means it is not core state as
 * assumed (registry, list, sentinel). Stop restoring statics for the rest of the battle. */
static int wsx_check(uint64_t seq) {
    if (!g_cfg.ws_statics || g_wsx_disabled || !g_nwsx) return 0;
    for (int k = 0; k < g_nwsx; k++)
        for (uint32_t o = 0; o + 8 <= g_wsx[k].len; o += 8) {
            uint64_t v = *(const uint64_t *)(g_wsx[k].addr + o);
            if (IN_IMAGE(v) || (v >= 0x100000000ULL && v < 0x7fffffffffffULL)) {
                g_wsx_disabled = 1;
                LOG("WSX disabled why=pointer_value va=0x%llx value=0x%llx seq=%llu (statics no longer restored this battle)\n",
                    (unsigned long long)(g_wsx[k].addr + o - g_base + IMG_PREF), (unsigned long long)v, (unsigned long long)seq);
                log_flush();
                return 1;
            }
        }
    return 0;
}
static void wsx_add_pins(SPin *pins, int *npins, int max) {
    if (!g_cfg.ws_statics || !g_nwsx || g_wsx_disabled) return;
    int added = 0; uint64_t bytes = 0;
    for (int k = 0; k < g_nwsx && *npins < max; k++) {
        if (!mem_readable((void *)g_wsx[k].addr, g_wsx[k].len)) continue;
        pins[*npins].addr = g_wsx[k].addr; pins[*npins].len = g_wsx[k].len; (*npins)++; added++; bytes += g_wsx[k].len;
    }
    LOG("WSX pins=%d bytes=%llu\n", added, (unsigned long long)bytes);
}

/* ------------------------------------------------------------------ round 15: ANIM probe and pins
 * Motion objects: the pointers PLAYER{0,1}+0x44040..+0x44068 (the motion pointers the game's own
 * SaveState relocates) and the motion pointer of each motion layer (PLAYER+0x95fa0+0x800+i*0xc0, +0x8).
 * Every access is through ReadProcessMemory (fails instead of faulting) after a VirtualQuery check:
 * committed, readable, not guard. Each object is hashed in 4 KB chunks (up to 64 KB) before and after
 * the live step and after the first resim step. A chunk a RESIM step changed is decode/blend state the
 * core writes outside the snapshot; with anim_pins on (F5) such chunks are pinned if their region is
 * read-write. Kill switch: a pinned chunk whose region is no longer committed read-write disables
 * anim_pins for the battle. */
#define AN_MAXO 24
#define AN_CH 16
typedef struct { uintptr_t p, base; size_t win; uint64_t h0[AN_CH], h1[AN_CH], h2[AN_CH]; int ok; uint32_t live_w, resim_w; } AnObj;
static AnObj g_an[AN_MAXO];
static int g_nan, g_anim_pins, g_anim_disabled;
volatile int g_pending_anim = -1, g_pending_bigscrub = -1, g_pending_passes = -1;
static int g_x_passes;
typedef struct { uintptr_t a; int rw; } AnPin;
static AnPin g_anpin[64]; static int g_nanpin;
static uint64_t g_an_frames, g_an_live_frames, g_an_resim_frames, g_an_lines;
static uint8_t g_an_buf[4096];
static int an_region(uintptr_t p, uintptr_t *alloc_base, size_t *avail, int *rw) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !mbi.Protect) return 0;
    *alloc_base = (uintptr_t)mbi.AllocationBase;
    *avail = (uintptr_t)mbi.BaseAddress + mbi.RegionSize - p;
    *rw = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
    return 1;
}
static int an_hash_chunk(uintptr_t a, size_t n, uint64_t *h) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)a, g_an_buf, n, &got) || got != n) return 0;
    *h = hash_bytes(g_an_buf, n, 11);
    return 1;
}
static void an_collect(void) {
    g_nan = 0;
    for (int c = 0; c < 2; c++) {
        uintptr_t pl = VA(0x1447156f0ULL) + (uintptr_t)c * 0x973f0;
        uintptr_t cand[6 + 8]; int nc = 0;
        for (int i = 0; i < 6; i++) cand[nc++] = *(uintptr_t *)(pl + 0x44040 + i * 8);
        for (int i = 0; i < 8; i++) cand[nc++] = *(uintptr_t *)(pl + 0x95fa0 + 0x800 + i * 0xc0 + 8);
        for (int i = 0; i < nc && g_nan < AN_MAXO; i++) {
            uintptr_t p = cand[i];
            if (p < 0x10000 || p > 0x7ffffffeffffULL || (p & 7) || IN_IMAGE(p)) continue;
            int dup = 0; for (int k = 0; k < g_nan; k++) if (g_an[k].p == p) dup = 1;
            if (dup) continue;
            uintptr_t ab; size_t av; int rw;
            if (!an_region(p, &ab, &av, &rw)) continue;
            AnObj *o = &g_an[g_nan++]; memset(o, 0, sizeof *o);
            o->p = p; o->base = ab; o->win = av < (size_t)AN_CH * 4096 ? (av & ~(size_t)4095) : (size_t)AN_CH * 4096;
            if (!o->win) { g_nan--; continue; }
        }
    }
}
static void an_hash_all(int which) {   /* 0: before live, 1: after live, 2: after the first resim step */
    for (int k = 0; k < g_nan; k++) {
        AnObj *o = &g_an[k];
        uintptr_t ab; size_t av; int rw;
        o->ok = an_region(o->p, &ab, &av, &rw) && ab == o->base && av >= o->win;
        if (!o->ok) continue;
        uint64_t *h = which == 0 ? o->h0 : which == 1 ? o->h1 : o->h2;
        for (size_t c = 0; c < o->win / 4096; c++) if (!an_hash_chunk(o->p + c * 4096, 4096, &h[c])) { o->ok = 0; break; }
    }
}
static void an_compare(uint64_t seq) {
    int live_any = 0, resim_any = 0;
    for (int k = 0; k < g_nan; k++) {
        AnObj *o = &g_an[k];
        if (!o->ok) continue;
        for (size_t c = 0; c < o->win / 4096; c++) {
            int lw = o->h1[c] != o->h0[c], rw2 = o->h2[c] != o->h1[c];
            if (lw) { live_any = 1; o->live_w++; }
            if (rw2) {
                resim_any = 1; o->resim_w++;
                uintptr_t a = o->p + c * 4096, ab; size_t av; int rw;
                int writable = an_region(a, &ab, &av, &rw) && rw && (a + av - ab) >= 256 * 1024;
                int known = 0; for (int i = 0; i < g_nanpin; i++) if (g_anpin[i].a == a) known = 1;
                if (!known && g_nanpin < 64) { g_anpin[g_nanpin].a = a; g_anpin[g_nanpin].rw = writable; g_nanpin++; if (g_anim_pins && writable) g_repin_request = 1; }
            }
            if ((lw || rw2) && g_an_lines < 200) {
                g_an_lines++;
                LOG("ANIMW seq=%llu fr=%u obj=%d ptr=%p chunk=+0x%zx live_step_changed=%d resim_step_changed=%d\n", (unsigned long long)seq, core_frame(), k,
                    (void *)o->p, c * 4096, lw, rw2);
            }
        }
    }
    g_an_frames++; g_an_live_frames += live_any; g_an_resim_frames += resim_any;
}
static void an_drop_pins(void) {
    static SPin keep[40960];
    int n = 0;
    spin_lock(&g_pin_lock);
    for (int i = 0; i < g_ss.npin && n < 40960; i++) {
        int anim = 0;
        for (int j = 0; j < g_nanpin; j++) if (g_ss.pins[i].addr == g_anpin[j].a && g_ss.pins[i].len == 4096) anim = 1;
        if (!anim && (!g_ss.stable || (g_ss.pstate[i] == SPIN_ALIVE && !g_ss.phidden[i]))) keep[n++] = g_ss.pins[i];
    }
    if (g_ss.stable) { int a, v; snapset_merge_pins(&g_ss, keep, n, &a, &v); }   /* v0.23.0-C1b: the stable layout is never re-sorted */
    else snapset_set_pins(&g_ss, keep, n);
    g_npins = g_ss.npin;
    spin_unlock(&g_pin_lock);
    LOG("ANIM pins dropped kept=%d\n", n);
}
static int an_check_pins(uint64_t seq) {   /* kill switch */
    if (!g_anim_pins || g_anim_disabled) return 0;
    for (int i = 0; i < g_nanpin; i++) {
        if (!g_anpin[i].rw) continue;
        uintptr_t ab; size_t av; int rw;
        if (!an_region(g_anpin[i].a, &ab, &av, &rw) || !rw || av < 4096) {
            g_anim_disabled = 1;
            LOG("ANIM disabled why=region_invalid chunk=%p seq=%llu (anim pins off for this battle)\n", (void *)g_anpin[i].a, (unsigned long long)seq);
            log_flush();
            return 1;
        }
    }
    return 0;
}
static void an_add_pins(SPin *pins, int *npins, int max) {
    if (!g_anim_pins || g_anim_disabled) return;
    int n = 0;
    for (int i = 0; i < g_nanpin && *npins < max; i++) if (g_anpin[i].rw) { pins[*npins].addr = g_anpin[i].a; pins[*npins].len = 4096; (*npins)++; n++; }
    LOG("ANIM pins applied chunks=%d\n", n);
}
static void an_report(const char *why) {
    if (!g_cfg.anim_probe) return;
    LOG("ANIM report why=%s objects=%d frames=%llu frames_live_step_wrote=%llu frames_resim_step_wrote=%llu resim_chunks=%d anim_pins=%d disabled=%d\n", why, g_nan,
        (unsigned long long)g_an_frames, (unsigned long long)g_an_live_frames, (unsigned long long)g_an_resim_frames, g_nanpin, g_anim_pins, g_anim_disabled);
    for (int k = 0; k < g_nan; k++)
        LOG("ANIMO obj=%d ptr=%p alloc_base=%p window=%zu live_writes=%u resim_writes=%u\n", k, (void *)g_an[k].p, (void *)g_an[k].base, g_an[k].win, g_an[k].live_w, g_an[k].resim_w);
}
/* POSE: at the first gameplay mismatch of an episode, live (ring) vs resim values of the yaw rows */
static uint64_t g_pose_last, g_pose_lines;
static void pose_dump(uint64_t s, uint64_t k, const Snap *ref) {
    if (g_pose_lines >= 40 || (g_pose_last && s - g_pose_last <= 3)) { g_pose_last = s; return; }
    g_pose_last = s; g_pose_lines++;
    static const uint32_t offs[] = { 0x965c0, 0x965d0, 0x965e0, 0x965f0, 0x96600, 0x96610, 0x3c40, 0x3c50, 0x3c60, 0x3c70, 0x94 };
    uintptr_t lo = g_ss.reg[0].addr;
    for (int c = 0; c < 2; c++) {
        uintptr_t pl = VA(0x1447156f0ULL) + (uintptr_t)c * 0x973f0;
        char line[1400]; size_t o = 0;
        o += (size_t)snprintf(line + o, sizeof line - o, "POSE seq=%llu k=%llu PLAYER%d", (unsigned long long)s, (unsigned long long)k, c);
        for (size_t i = 0; i < sizeof offs / sizeof offs[0]; i++) {
            uintptr_t a = pl + offs[i];
            size_t n = offs[i] == 0x94 ? 4 : 16;
            const uint8_t *rv = ref->buf[0] + (a - lo);
            o += (size_t)snprintf(line + o, sizeof line - o, " +0x%x:", offs[i]);
            for (size_t b = 0; b < n; b += 4) {
                uint32_t nv, r; memcpy(&nv, (void *)(a + b), 4); memcpy(&r, rv + b, 4);
                o += (size_t)snprintf(line + o, sizeof line - o, nv == r ? "%08x" : "%08x/%08x", nv, r);
                if (b + 4 < n) o += (size_t)snprintf(line + o, sizeof line - o, ",");
            }
        }
        LOG("%s (resim/live where they differ)\n", line);
    }
}
/* Round 12's XR probe (hashing raw foreign candidates around the resims) crashed once on a stale
 * pointer and is removed. Its null result was also weak: it compared object state after the whole
 * resim block, which ends at the live frame again, so deterministic rewrites look unchanged. */
static void extw_report(void) {
    int shown = 0, changed = 0, test_changed = 0;
    for (int k = 0; k < g_extw_n; k++) { if (g_extw_hits[k]) changed++; if (g_extw_test[k]) test_changed++; }
    LOG("EXTW report frames=%d candidates=%d capped=%d changed=%d changed_by_core_in_test_passes=%d\n", g_extw_frames, g_extw_n, g_extw_capped, changed, test_changed);
    /* round 7: report only. Round 6 crashed in ntdll (HeapValidate on a pointer from another heap);
     * no Heap* call or allocator size query is ever made on a pointer we did not allocate-track. */
    for (int k = 0; k < g_extw_n; k++) {
        if (!g_extw_hits[k] || !g_extw_p[k] || !mem_readable((void *)g_extw_p[k], 8)) continue;
        char own[160]; extw_owner(g_extw_ref[k], own, sizeof own);
        uintptr_t vt = *(uintptr_t *)g_extw_p[k];
        if (shown < 120)
            LOG("EXTWO obj=%p owner=%s changes_live=%u changes_test=%u vt_rva=0x%llx typed=%d\n", (void *)g_extw_p[k], own,
                g_extw_live[k], g_extw_test[k], (unsigned long long)(IN_IMAGE(vt) ? RVA(vt) : 0), typed_slot_of(vt) >= 0);
        shown++;
    }
    g_extw_done = 1;
    g_extw_n = 0;
}

/* ------------------------------------------------------------------ mode control */
static int needs_routing(int m) { return m == MODE_SYNC1 || m == MODE_ROLL7 || m == MODE_LOOPBACK || m == MODE_SYNC1X || is_c1a(m); }   /* v0.22.0: + record/replay */
static int g_lb_on;
static uint64_t g_lb_steps;
static uint64_t g_lb_start, g_lb_last_conf;
static int g_lbp = 2;
static uint64_t g_last_real[3];
static int g_battle_init_routed;
static int g_first_step_logged;

static void set_mode(int m, const char *why) {
    int old = g_mode;
    if (needs_routing(m) && !(g_arena_ok && g_mem_hooked)) {
        LOG("MODE refuse to=%s why=arena_or_memhook_unavailable\n", g_mode_names[m]);
        m = MODE_OBSERVE;
    }
    ring_clear("mode_switch");
    g_lb_on = 0; g_lb_steps = 0;
    if (g_extw_frames > 0 && !g_extw_done) extw_report();
    g_extw_done = 0; g_extw_frames = 0;
    g_reports = 0;
    if (old == MODE_OBSERVE && g_ws_active) ws_abort("mode_switch");
    hws_report("mode_switch");
    g_mode = m;
    g_ss_ready = 0;
    if (needs_routing(m)) {
        if (!ss_build(why)) { LOG("MODE ss_build_failed -> observe\n"); g_mode = MODE_OBSERVE; }
        else if (!g_battle_init_routed && !g_cfg.track_init) LOG("WARN mode=%s entered mid-battle: Init allocations are on the real heap and not tracked (container pins only)\n", g_mode_names[m]);
    }
    LOG("MODE from=%s to=%s why=%s seq=%llu fr=%u\n", g_mode_names[old], g_mode_names[g_mode], why, (unsigned long long)g_seq, core_frame());
    g_title_dirty = 1;
    seg_reset("mode");
    mem_stats_log("mode_switch");
    log_flush();
}

static void fallback(const char *why) {
    LOG("FALLBACK why=%s route_broken=%d defer_broken=%d mode=%s\n", why, g_route_broken, g_defer_broken, g_mode_names[g_mode]);
    set_mode(MODE_OBSERVE, why);
}

volatile int g_title_dirty = 1;
/* v0.20: "[sc6rb <version> <tag> mode=... L4=... depth=... commit=... fwd=... L3=...]" for the window title */
void modes_title(char *out, size_t cap) {
    char dep[16];
    if (g_cfg.r7_random_depth) snprintf(dep, sizeof dep, "rand0-%d", g_cfg.roll_depth); else snprintf(dep, sizeof dep, "%d", g_depth_rt > 0 ? g_depth_rt : g_cfg.roll_depth);
    /* v0.20.2: cam=full when a scan pinned the whole camera function block, cam=slots with cam_pin_full=0, then cam=keep with
     * the keep-live on. v0.21.1: cam=FAIL while cam_pin_full=1 and the block is not pinned (rollback off); the silent fallback
     * to the slot pins (which showed cam=slots) is gone. */
    const char *cp = !g_cfg.cam_pin_full ? " cam=slots" : g_cam_fail ? " cam=FAIL" : g_cam_pinned_full ? " cam=full" : "";
    if (is_rb_mode(g_cfg.mode) && g_cfg.lipsync && !g_q1_rb) {   /* LIPSYNC: title tag (the line below is the unchanged lipsync=0 path) */   /* v0.29.4 FACES: + !g_q1_rb (the hybrid's branch appends lips=) */
        rb_title(out, cap, g_l4 == 2 ? "guard" : g_l4 ? "on" : "off", g_l3_on ? "on" : "off", cp); lipsync_title(out, cap, strstr(out, " rb=") && !strstr(out, " rb=REFUSED")); return;   /* LIPSYNC: */
    }   /* LIPSYNC: */
    if (is_rbcore_mode(g_cfg.mode) && g_q1_rb) {   /* v0.29.2 (§16.9): the hybrid's tokens (only with a q1 key in the ini: every other title keeps its bytes) */
        rb_title(out, cap, g_l4 == 2 ? "guard" : g_l4 ? "on" : "off", !g_l3_on ? "off" : g_cfg.rb_l3stock == 1 ? "stock" : "on", cp);
        size_t o = strlen(out);
        if (strstr(out, " rb=REFUSED")) return;
        if (!g_cfg.q1_roots) snprintf(out + o, cap > o ? cap - o : 0, " q1=off");
        else if (g_q1_fail_bat[0]) snprintf(out + o, cap > o ? cap - o : 0, " q1=FAIL:%s", g_q1_fail_bat);
        else if (!g_q1_armed || !g_q1_walked_bat) snprintf(out + o, cap > o ? cap - o : 0, " q1=pre");
        else snprintf(out + o, cap > o ? cap - o : 0, " q1=ok");
        lipsync_title(out, cap, strstr(out, " rb=") && !strstr(out, " rb=REFUSED"));   /* v0.29.4 FACES: " lips=on|FAIL|REFUSED" after q1= (nothing with lipsync absent / 0) */
        return;
    }
    if (is_rbcore_mode(g_cfg.mode)) { rb_title(out, cap, g_l4 == 2 ? "guard" : g_l4 ? "on" : "off", g_l3_on ? "on" : "off", cp); return; }   /* v0.23.0-C1b (also refused) */
    if (is_c1a(g_mode)) {   /* v0.22.0-C1a: "[sc6rb <version> <tag> mode=record|replay L4=.. L3=.. cam=full rec=<frames> | play=<state>]" */
        char sfx[64]; c1a_title(sfx, sizeof sfx);
        snprintf(out, cap, "[sc6rb %s %s mode=%s L4=%s L3=%s%s%s]", SC6RB_VERSION, g_cfg.title_tag, g_mode_names[g_mode], g_l4 == 2 ? "guard" : g_l4 ? "on" : "off",
                 g_l3_on ? "on" : "off", cp, sfx);
        lipsync_title(out, cap, g_first_step_logged);   /* LIPSYNC: title tag (lips=REFUSED) */
        return;
    }
    /* v0.21.0-S1: L4=on|off|guard, and pres=keep with the presentation keep-live on (pres_keep=1 and r7_commit=1) */
    snprintf(out, cap, "[sc6rb %s %s mode=%s L4=%s depth=%s commit=%s fwd=%s L3=%s%s%s%s]", SC6RB_VERSION, g_cfg.title_tag, g_mode_names[g_mode],
             g_l4 == 2 ? "guard" : g_l4 ? "on" : "off", dep,
             g_cfg.r7_commit ? "on" : "off", g_cfg.r7_forward ? "on" : "off", g_l3_on ? "on" : "off", cp, ck_on() ? " cam=keep" : "", pk_on() ? " pres=keep" : "");
    lipsync_title(out, cap, g_first_step_logged);   /* LIPSYNC: title tag (lips=REFUSED) */
}
DWORD modes_step_tid(void);
void modes_init(void) {
    g_mode = g_cfg.mode;
    if (is_rbcore_mode(g_cfg.mode) && !rb_cfg_check()) g_mode = MODE_OFF;   /* v0.23.0-C1b: CFG C1B REFUSE -> mode off, title rb=REFUSED:<key> */
    if (g_cfg.q1_present && !is_rbcore_mode(g_cfg.mode)) {   /* v0.29.2 (§16.9): the hybrid keys are rbreplay-only (rb modes refuse them in rb_cfg_check) */
        static const char *const qk[5] = { "rb_l3stock", "rb_arm_f", "rb_init_pins", "q1_roots", "q1_cov" };
        for (int i = 0; i < 5; i++) if (g_cfg.q1_present & (1 << i)) LOG("CFG Q1 REFUSE key=%s value=set need=mode=rbreplay (mode=%s: the plugin runs mode=off)\n", qk[i], g_mode_names[g_cfg.mode]);
        g_mode = MODE_OFF;
    }
    if (g_mode == MODE_RBREPLAY) q1_setup();   /* v0.29.2 */
    g_lbp = g_cfg.lb_player;
    g_l3_on = g_cfg.l3_ini != 0;
    g_l4 = g_cfg.l4;   /* 0 off, 1 on (revert), 2 guard (log only; v0.21.0-S1) */
    if (g_l4) {
        g_l4_buf = (uint8_t *)VirtualAlloc(NULL, CORE_BSS_LEN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        /* netplay-faithful: no listener recording to replay, nothing learned at runtime */
        if (g_cfg.lsn_replay) { LOG("L4 forces listener_replay=0 (a netplay resim has no live recording)\n"); g_cfg.lsn_replay = 0; }
        if (g_cfg.lsnw) { LOG("L4 forces listener_write_learning=0 (L4 reverts all of core .bss instead of a learned list)\n"); g_cfg.lsnw = 0; }
        if (g_l4 == 1)
            LOG("L4 on: buffer=%p bytes=%zu queries=null_in_every_pass miss_watchdog=%s\n", (void *)g_l4_buf, CORE_BSS_LEN,
                g_cfg.l4_miss == 2 ? "every_pass" : g_cfg.l4_miss == 1 ? "live_passes" : "off");
        else
            LOG("L4 guard: buffer=%p bytes=%zu queries=vanilla_live_null_resim miss_watchdog=%s (log only: UE's core .bss writes outside P and the FX scratch e9b0 are logged as L4GUARD lines, budget %d; nothing is reverted)\n",
                (void *)g_l4_buf, CORE_BSS_LEN, g_cfg.l4_miss == 2 ? "every_pass" : g_cfg.l4_miss == 1 ? "live_passes" : "off", L4G_BUDGET);
    }
    if (g_cfg.pres_keep && !g_cfg.r7_commit) LOG("PRESKEEP pres_keep=1 has no effect without r7_commit=1 (without commit the live result is kept whole)\n");
    if (g_cfg.r7_random_depth || g_cfg.r7_commit || g_cfg.r7_forward)
        LOG("C0B roll7 random_depth=%d seed=%u depth_range=0..%d commit=%d forward=%d cam_keep=%d pres_keep=%d setup_barrier=%d (one configuration per launch; hotkeys are not needed)\n",
            g_cfg.r7_random_depth, g_cfg.r7_random_seed, g_cfg.roll_depth, g_cfg.r7_commit, g_cfg.r7_forward, ck_on(), pk_on(), g_cfg.r7_setup_barrier);
    lp_install_family();
    c1a_init();   /* v0.22.0-C1a: record / replay (loads replay_file) */
    rb_init();    /* v0.23.0-C1b */
}

/* ------------------------------------------------------------------ stats */
typedef struct { uint64_t n; double sum, mx; double hist[8]; } TStat;
static double g_step_us[600]; static int g_step_us_n;
static void time_report(const char *why) {
    if (!g_step_us_n) return;
    double v[600]; int n = g_step_us_n;
    memcpy(v, g_step_us, sizeof(double) * n);
    for (int i = 1; i < n; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    LOG("TIME why=%s mode=%s n=%d p50=%.1f p90=%.1f p99=%.1f max=%.1f us\n", why, g_mode_names[g_mode], n, v[n / 2], v[n * 9 / 10],
        v[n * 99 / 100], v[n - 1]);
    g_step_us_n = 0;
}
static void time_add(double us) { if (g_step_us_n < 600) g_step_us[g_step_us_n++] = us; }

void modes_periodic_report(const char *why) {
    time_report(why);
    { static int last_nlw = -1; if (g_nlw != last_nlw) { last_nlw = g_nlw; lsnw_report(); } }
    if (g_cfg.census && g_mode == MODE_OBSERVE) census_report(why);
    lp_report(why);
    ifc_report(why);
    mem_stats_log(why);
    watch_report(why);
    scrub_report(why);
    l4_report(why);
    r7_c0b_report(why);
    sw_report(why);
    an_report(why);
    l1_report(why);
    LOG("FPENV why=%s listener_changes=%llu resim_entry_fixes=%llu fp_guard=%d\n", why, (unsigned long long)g_fp_lsn_changes, (unsigned long long)g_fp_entry_fixes, g_cfg.fp_guard);
}

/* ------------------------------------------------------------------ observe */
/* ---- heap write-set (B0, round 2): which blocks allocated inside Init/Setup does the step
 * (or something between steps) write? Hash every tracked block before and after each step
 * during the write-set window. */
static SPin *g_hws_blk; static int g_hws_n, g_hws_on;
static uint64_t *g_hws_h; static uint32_t *g_hws_step, *g_hws_for;
static void hws_free(void) {
    free(g_hws_blk); free(g_hws_h); free(g_hws_step); free(g_hws_for);
    g_hws_blk = NULL; g_hws_h = NULL; g_hws_step = g_hws_for = NULL; g_hws_n = 0; g_hws_on = 0;
}
static void hws_begin(void) {
    hws_free();
    g_hws_blk = (SPin *)malloc(sizeof(SPin) * 24576);
    if (!g_hws_blk) return;
    uint64_t b = 0;
    g_hws_n = itrack_list(g_hws_blk, 24576, &b);
    g_hws_h = (uint64_t *)calloc((size_t)g_hws_n + 1, 8);
    g_hws_step = (uint32_t *)calloc((size_t)g_hws_n + 1, 4);
    g_hws_for = (uint32_t *)calloc((size_t)g_hws_n + 1, 4);
    if (!g_hws_h || !g_hws_step || !g_hws_for) { hws_free(); return; }
    for (int i = 0; i < g_hws_n; i++) g_hws_h[i] = hash_bytes((void *)g_hws_blk[i].addr, g_hws_blk[i].len, 1);
    g_hws_on = 1;
    LOG("HWS begin blocks=%d bytes=%llu\n", g_hws_n, (unsigned long long)b);
}
static void hws_pass(uint32_t *cnt) {
    if (!g_hws_on) return;
    for (int i = 0; i < g_hws_n; i++) {
        if (!g_hws_blk[i].addr) continue;
        if (!itrack_contains(g_hws_blk[i].addr, NULL, NULL)) { g_hws_blk[i].addr = 0; continue; }   /* freed */
        uint64_t h = hash_bytes((void *)g_hws_blk[i].addr, g_hws_blk[i].len, 1);
        if (h != g_hws_h[i]) { g_hws_h[i] = h; cnt[i]++; }
    }
}
static void hws_report(const char *why) {
    if (!g_hws_on) return;
    int written = 0, fwritten = 0;
    uint64_t wbytes = 0;
    for (int i = 0; i < g_hws_n; i++) { if (g_hws_step[i]) { written++; wbytes += g_hws_blk[i].len; } if (g_hws_for[i]) fwritten++; }
    LOG("HWS report why=%s blocks=%d step_written=%d step_written_bytes=%llu foreign_written=%d\n", why, g_hws_n, written,
        (unsigned long long)wbytes, fwritten);
    int shown = 0;
    for (int pass = 0; pass < 2 && shown < 60; pass++)
        for (int i = 0; i < g_hws_n && shown < 60; i++) {
            if (!(pass == 0 ? g_hws_step[i] : (!g_hws_step[i] && g_hws_for[i]))) continue;
            uint32_t caller = 0; int kind = 0;
            itrack_contains(g_hws_blk[i].addr, &caller, &kind);
            LOG("HWSB addr=%p size=%u caller=0x%x kind=%s step_hits=%u foreign_hits=%u\n", (void *)g_hws_blk[i].addr, g_hws_blk[i].len,
                caller, kind ? "crt" : "fmalloc", g_hws_step[i], g_hws_for[i]);
            shown++;
        }
    hws_free();
}

static void do_observe(StepIn *gin, FrameIn *fi, uint64_t seq) {
    uint64_t wsf = ws_pre();
    hws_pass(g_hws_for);
    census_step_begin();
    uint64_t r0 = g_x.rand_calls, l0 = lp_total(0);
    uint32_t fr0 = core_frame();
    uint64_t t0 = qpc();
    run_step(gin, 0, seq, CTX_STEP | (g_cfg.census ? CTX_CENSUS : 0));
    uint64_t t1 = qpc();
    census_step_end(seq);
    hws_pass(g_hws_step);
    uint64_t wss = ws_post();
    if (g_hws_on && !g_ws_active) hws_report("done");
    double us = qpc_us(t1 - t0);
    time_add(us);
    if (g_cfg.step_log) {
        uint32_t pin0 = 0, pin1 = 0;
        uintptr_t c0 = *(uintptr_t *)VA(A_CHARA_TBL), c1 = *(uintptr_t *)(VA(A_CHARA_TBL) + 8);
        if (c0 == VA(A_PLAYER0)) pin0 = *(uint32_t *)(c0 + PLAYER_INPUT_OFF);
        if (c1 == VA(A_PLAYER0) + PLAYER_STRIDE) pin1 = *(uint32_t *)(c1 + PLAYER_INPUT_OFF);
        char xs[64] = "0";
        if (extra_nonzero(fi)) for (int i = 0; i < 0x18; i++) snprintf(xs + i * 2, 3, "%02x", fi->extra[i]);
        LOG("S seq=%llu fr=%u fr1=%u tid=%lu n=%d us=%.1f p1=%llx p2=%llx f=%x x=%s pin0=%x pin1=%x na=%u nb=%llu nfs=%u nfp=%u bfp=%llu nr=%u naa=%u naf=%u nca=%u ncf=%u lc=%llu rnd=%llu wsS=%llu wsF=%llu\n",
            (unsigned long long)seq, fr0, core_frame(), GetCurrentThreadId(), g_tick_steps, us,
            (unsigned long long)fi->p1, (unsigned long long)fi->p2, fi->flags, xs, pin0, pin1,
            g_sc.n_alloc, (unsigned long long)g_sc.bytes_alloc, g_sc.n_free_step, g_sc.n_free_pre,
            (unsigned long long)g_sc.bytes_free_pre, g_sc.n_realloc, g_sc.n_arena_alloc, g_sc.n_arena_free,
            g_sc.n_crt_alloc, g_sc.n_crt_free,
            (unsigned long long)(lp_total(0) - l0), (unsigned long long)(g_x.rand_calls - r0),
            (unsigned long long)wss, (unsigned long long)wsf);
    }
}

/* ------------------------------------------------------------------ sync1 */
/* RB (CTX_STEP | CTX_ROUTE | CTX_DEFER): rb.h */
static uint64_t g_y1_n, g_y1_bad;
static void do_sync1(StepIn *gin, FrameIn *fi, uint64_t seq) {
    uint64_t t0 = qpc();
    if (!snap_take(&g_ss, &g_s0)) { fallback("snap_alloc"); run_step(gin, 0, seq, CTX_STEP); return; }
    uint64_t t1 = qpc();
    run_fi(fi, 1, seq, RB);
    uint64_t t2 = qpc();
    if (g_route_broken || g_defer_broken) {
        snap_restore(&g_ss, &g_s0); def_drop_from(seq);
        fallback("sync1_muted");
        run_step(gin, 0, seq, CTX_STEP);
        return;
    }
    snap_take(&g_ss, &g_p);
    uint64_t c1[NCL]; class_hashes(&g_p, c1);
    uint64_t t3 = qpc();
    snap_restore(&g_ss, &g_s0);
    def_drop_from(seq);
    uint64_t t4 = qpc();
    run_fi(fi, 0, seq, RB);
    uint64_t t5 = qpc();
    uint64_t c2[NCL]; class_hashes(NULL, c2);
    uint64_t t6 = qpc();
    int mask = class_diff(c1, c2), ok = mask == 0;
    char ms[NCL + 1]; class_str(mask, ms);
    size_t nb = 0;
    g_y1_n++;
    if (!ok) { g_y1_bad++; nb = report_diff("sync1", seq, seq, &g_p); }
    def_confirm_before(seq + 1);
    time_add(qpc_us(t5 - t4));
    LOG("Y1 seq=%llu fr=%u ok=%d cls=%s diffbytes=%zu us_snap=%.0f us_muted=%.0f us_post=%.0f us_restore=%.0f us_live=%.0f us_hash=%.0f us_total=%.0f\n",
        (unsigned long long)seq, core_frame(), ok, ms, nb,
        qpc_us(t1 - t0), qpc_us(t2 - t1), qpc_us(t3 - t2), qpc_us(t4 - t3), qpc_us(t5 - t4), qpc_us(t6 - t5), qpc_us(t6 - t0));
    if (g_route_broken || g_defer_broken) fallback("sync1_live");
}

/* ------------------------------------------------------------------ sync1x (round 2 discriminator, per class since round 3)
 * Per frame, from the same pre-state S0 and the same inputs:
 *   L  = live step (listener recorded)                  -> committed at the end
 *   R  = muted step replaying L's listener calls (returns + out-param bytes)
 *   M  = muted step (notifications return 0)
 *   M2 = muted step again
 * Each comparison is reported per state class (G gameplay, C camera, F effects, R replay buffers,
 * O other): "-" equal, letter = differs. */
/* v0.19: listener slot IDs of every live pass (last 16 frames), for the R7 line (k*-1, k*-4) and L1W */
#define LSL_N 16
typedef struct { uint64_t seq; int n; uint8_t slot[16]; } LslEnt;
static LslEnt g_lsl[LSL_N];
static void lsl_note(uint64_t seq, const LRec *r) {
    LslEnt *e = &g_lsl[seq % LSL_N];
    e->seq = seq; e->n = 0;
    if (r) for (int i = 0; i < r->n && e->n < 16; i++) e->slot[e->n++] = r->c[i].slot;
}
static const char *lsl_str(uint64_t seq, char *o, size_t cap) {
    LslEnt *e = &g_lsl[seq % LSL_N];
    if (!seq || e->seq != seq) { snprintf(o, cap, "?"); return o; }
    if (!e->n) { snprintf(o, cap, "-"); return o; }
    size_t k = 0;
    for (int i = 0; i < e->n && k + 8 < cap; i++) k += (size_t)snprintf(o + k, cap - k, "%s%x", i ? "." : "", (unsigned)e->slot[i] * 8u);
    return o;
}
/* " lsn_km1=... lsn_km4=..." for a bad frame k* */
static const char *lsl_bad(int ok, uint64_t kstar, char *o, size_t cap) {
    if (ok) { o[0] = 0; return o; }
    char a[96], b[96], c[96];   /* v0.19.0 R8: a third buffer (km4 used to overwrite a + 48 while a still held lsn_k) */
    snprintf(o, cap, " lsn_k=%s lsn_km1=%s lsn_km4=%s", lsl_str(kstar, a, sizeof a), lsl_str(kstar - 1, b, sizeof b), lsl_str(kstar >= 4 ? kstar - 4 : 0, c, sizeof c));
    return o;
}
/* v0.19: a new path segment (mode, roll7 path, depth, L3, battle): gameplay report and L1 budgets reset */
static int g_r7_path;
#define CAMSPLIT_BUDGET 200   /* v0.20.2 CAMSPLIT lines per session (see camsplit_frame_end) */
static int g_csp_prev, g_csp_have, g_csp_lines;
static uint64_t g_csp_onsets, g_csp_k;
static void seg_reset(const char *why) {
    static const char *pn[4] = { "ring_old", "sync", "fresh", "unified" };
    g_greports = 0; g_pose_lines = 0;
    g_csp_prev = 0; g_csp_have = 0;   /* v0.20.2: a new segment (battle, mode, depth, L3, path) has no previous R7 frame */
    int p = g_r7_path >= 0 && g_r7_path < 4 ? g_r7_path : g_cfg.r7_path_ini;
    const char *path = g_mode == MODE_ROLL7 ? pn[p & 3] : g_mode == MODE_SYNC1X ? "sync1x" : g_mode_names[g_mode];
    l1_segment(why, path, g_depth_rt);
}

static uint64_t g_x_n, g_x_lr, g_x_lm, g_x_mm;
static uint64_t g_xc_lr[NCL], g_xc_lm[NCL], g_xc_mm[NCL];
static int do_sync1x(StepIn *gin, FrameIn *fi, uint64_t seq) {
    if (!g_lrec_x && !(g_lrec_x = lrec_alloc())) { fallback("lrec_alloc"); run_step(gin, 0, seq, CTX_STEP); return -1; }
    int probe = !g_extw_done && g_cfg.extw_frames > 0;
    if (probe && g_extw_frames == 0) extw_begin();
    uint64_t t0 = qpc();
    if (!snap_take(&g_ss, &g_s0)) { fallback("snap_alloc"); run_step(gin, 0, seq, CTX_STEP); return -1; }
    /* L: live, recorded (during the probe window also learning UE's writes into core state) */
    /* round 6: learn on every sync1x live pass (round 5's 30-frame window saw 0 listener calls) */
    if (g_cfg.lsnw && !g_lsnw_buf) g_lsnw_buf = (uint8_t *)VirtualAlloc(NULL, (size_t)(A_CORE_BSS_HI - A_CORE_BSS_LO), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_lsnw_on = g_cfg.lsnw && g_lsnw_buf;
    const char *l1path = g_mode == MODE_ROLL7 ? "sync" : "sync1x";
    lrec_begin(g_lrec_x, 1);
    l1_pass_begin(L1P_LIVE, seq, seq);
    run_fi(fi, 0, seq, RB);
    l1_pass_end();
    lrec_end();
    lsl_note(seq, g_lrec_x);
    g_lsnw_on = 0;
    if (probe) extw_pass(1);
    def_save();                             /* the live run's deferred frees are the committed ones */
    snap_take(&g_ss, &g_p);
    uint64_t cl[NCL]; class_hashes(&g_p, cl);
    uint64_t t1 = qpc();
    if (g_route_broken || g_defer_broken) { fallback("sync1x_live"); return -1; }
    /* R: muted with replay */
    snap_restore(&g_ss, &g_s0); def_drop_from(seq);
    lrec_begin(g_lrec_x, 2);
    l1_iter_begin(seq, seq);
    l1_pass_begin(L1P_REPLAY, seq, seq);
    run_fi(fi, 1, seq, RB);
    l1_pass_end();
    int diverged = g_lrec_diverged, replayed = g_lrec_idx;
    lrec_end();
    if (probe) extw_pass(0);
    uint64_t cr[NCL]; class_hashes(NULL, cr);
    int m_lr = class_diff(cl, cr);
    l1_check(seq, seq, m_lr & 1, l1path);
    size_t nb_r = 0;
    if (m_lr) nb_r = report_diff("x_replay_vs_live", seq, seq, &g_p);
    if (!g_x_passes) g_x_passes = g_cfg.sync1x_passes;
    int passes = g_x_passes;
    if (m_lr & 1) { if (passes == 2) pose_dump(seq, seq, &g_p); report_gameplay("x_replay_vs_live", seq, seq, &g_p, NULL); }
    int m_lm = 0, m_mm = 0;
    size_t nb_m = 0, nb_mm = 0;
    if (passes == 4) {
    /* M: muted (round 6: queries answered from L's record, so test passes never call UE) */
    snap_restore(&g_ss, &g_s0); def_drop_from(seq);
    lrec_begin(g_lrec_x, 3);
    run_fi(fi, 1, seq, RB);
    lrec_end();
    if (probe) extw_pass(0);
    uint64_t cm[NCL]; class_hashes(NULL, cm);
    snap_take(&g_ss, &g_pm);
    m_lm = class_diff(cl, cm);
    if (m_lm) nb_m = report_diff("x_muted_vs_live", seq, seq, &g_p);
    /* M2: muted again */
    snap_restore(&g_ss, &g_s0); def_drop_from(seq);
    lrec_begin(g_lrec_x, 3);
    run_fi(fi, 1, seq, RB);
    lrec_end();
    if (probe) extw_pass(0);
    uint64_t cm2[NCL]; class_hashes(NULL, cm2);
    m_mm = class_diff(cm, cm2);
    if (m_mm) nb_mm = report_diff("x_muted_vs_muted", seq, seq, &g_pm);
    if (m_mm & 1) report_gameplay("x_muted_vs_muted", seq, seq, &g_pm, NULL);
    }   /* passes == 4 */
    /* commit the live result */
    snap_restore(&g_ss, &g_p);
    def_restore();
    def_confirm_before(seq + 1);
    uint64_t t2 = qpc();
    g_x_n++; g_x_lr += m_lr != 0; g_x_lm += m_lm != 0; g_x_mm += m_mm != 0;
    for (int c = 0; c < NCL; c++) { g_xc_lr[c] += (m_lr >> c) & 1; g_xc_lm[c] += (m_lm >> c) & 1; g_xc_mm[c] += (m_mm >> c) & 1; }
    char s_lr[NCL + 1], s_lm[NCL + 1], s_mm[NCL + 1];
    class_str(m_lr, s_lr); class_str(m_lm, s_lm); class_str(m_mm, s_mm);
    LOG("X1 seq=%llu fr=%u passes=%d live_eq_replay=%d live_eq_muted=%d muted_eq_muted=%d cls_lr=%s cls_lm=%s cls_mm=%s gameplay_mm=%d gameplay_lr=%d calls=%d replayed=%d diverged_at=%d outparam_calls=%llu stk_calls=%llu bytes_r=%zu bytes_m=%zu bytes_mm=%zu cam=%s us_live=%.0f us_total=%.0f\n",
        (unsigned long long)seq, core_frame(), passes, m_lr == 0, m_lm == 0, m_mm == 0, s_lr, s_lm, s_mm, (m_mm & 1) != 0, (m_lr & 1) != 0,
        g_lrec_x->n, replayed, diverged, (unsigned long long)g_lrec_outparam_calls, (unsigned long long)g_stk_calls, nb_r, nb_m, nb_mm,
        g_cfg.camera_exclude ? "exclude" : "include", qpc_us(t1 - t0), qpc_us(t2 - t0));
    time_add(qpc_us(t1 - t0));
    { char ls[96]; l1_frame_end(seq, l1path, lsl_str(seq, ls, sizeof ls)); }
    if (probe && ++g_extw_frames >= g_cfg.extw_frames) { extw_report(); lsnw_report(); }
    if (g_route_broken || g_defer_broken) fallback("sync1x");
    return m_lr;
}

/* ------------------------------------------------------------------ roll7
 * The live step records its listener calls; resims replay them (listener_replay=1), and bytes
 * written into core state between steps are re-applied before the same frame is resimulated
 * (foreign_replay=1). Since round 3 every resim step is compared per state class and the resim
 * runs all 7 steps (the first mismatch is reported in detail). */
static uint64_t g_r7_n, g_r7_bad, g_r7_foreign, g_r7_cls[NCL], g_r7_game_frames;
/* Round 17 A/B: roll7_path. ring = the normal roll7 (restore the ring snapshot of k0, resim from the
 * roll7 loop). sync = at depth 1, run exactly sync1x with passes=2 (same snapshot point g_s0, same
 * function and call path for the live and the replay pass). fresh = the ring path, but at depth 1 it
 * restores a snapshot taken right before the live step (g_s0-style) instead of the ring's. */
static int g_r7_path = -1;   /* 0 ring_old, 1 sync, 2 fresh, 3 unified (round 18 default); v0.19 F5: sync -> unified -> ring_old */
static const char *k_r7_path[4] = { "ring_old", "sync", "fresh", "unified" };
static Snap g_r7fresh;
static void do_roll7_sync(StepIn *gin, FrameIn *fi, uint64_t s) {
    int save = g_x_passes; g_x_passes = 2;
    int m = do_sync1x(gin, fi, s);
    g_x_passes = save;
    if (m < 0) return;
    g_r7_n++; if (m) g_r7_bad++;
    for (int c = 0; c < NCL; c++) g_r7_cls[c] += (m >> c) & 1;
    if (m & 1) g_r7_game_frames++;
    char ms[NCL + 1]; class_str(m, ms);
    char lb[200];
    LOG("R7 seq=%llu fr=%u depth=1 path=sync scrub=%d L3=%d ok=%d checked=1 badk=%d cls=%s gameplay=%d%s (sync1x passes=2 sequence)\n", (unsigned long long)s, core_frame(),
        g_scrub ? g_cfg.stack_scrub_kb : 0, g_l3_on, m == 0, m ? 0 : -1, ms, (m & 1) != 0, lsl_bad(m == 0, s, lb, sizeof lb));
}

/* Round 18: roll7 "unified". Every step, live and resim, runs through exactly the primitives
 * do_sync1x uses, in the same order and from the same input location:
 *   pre-snapshot right before the live step (sync1x's g_s0 point) -> live pass (record) -> def_save ->
 *   post-snapshot -> reference hash FROM the post-snapshot (class_hashes(&P), as sync1x) -> restore
 *   pre(k0) -> per k: re-apply the between-step writes of k (k > k0), copy frame k's input into the
 *   SAME FrameIn the live step used (*fi), replay pass -> hash live memory -> restore the live input
 *   and the post-snapshot -> def_restore. No camera save/put-back, static learning or anim probe
 *   between the passes. Ring entries keep per-frame pre-snapshots, records, inputs and hashes. */
static int g_r7post_ok;
static uint64_t g_r7post_seq;
/* v0.20.0 C0b: random depth, commit and forwarded resims (GATE-C-DESIGN §8 C0b, review R1).
 *   r7_random_depth=1: the depth of every frame is r7_depth_draw(seed, battle, battle step, roll_depth),
 *     0..roll_depth (0 = no resim that frame); it goes through g_depth_rt. Deferred frees are confirmed
 *     at the maximum depth, so any later frame may reach back roll_depth frames.
 *   r7_commit=1: the resim result of frame s is KEPT (no g_r7post restore, no def_restore): the next live
 *     pass runs on the resim lineage. The checks still compare every resim k against frame k's live pass.
 *     g_r7post is re-taken from the committed state (the reference for the next between-step capture).
 *     The committed state's difference from the live result is the residue (R7RESID, per class).
 *   r7_forward=1: resim passes forward notifications to UE (L4 reverts their writes). */
static uint64_t g_r7_dn[32], g_r7_dchk[32], g_r7_dbad[32], g_r7_commits, g_r7_resid_frames[NCL], g_r7_resid_bytes[NCL];
static int g_r7_resid_lines;
typedef struct { uint64_t bytes[NCL]; } ResidCtx;
static int g_r7_resid_o_lines;
static void resid_cb(void *u, int region, size_t off, size_t len, uintptr_t addr) {
    ResidCtx *r = (ResidCtx *)u;
    int c = CL_OTHER;
    if (region >= 0) {
        const char *nm = g_ss.reg[region].name;
        if (reg_in_img(nm)) { int k = class_of_va(addr - g_base + IMG_PREF); c = k < 0 ? CL_OTHER : k; }
        else if (!strcmp(nm, "workmem")) c = CL_REPLAY;
    }
    char d[96]; int camh = 0;
    if (region == -2) {   /* pins: map the blob offset back to an address; v0.20.1: camera-owned heap bytes count as C */
        size_t acc = 0; int i = 0;
        for (; i < g_ss.npin && acc + g_ss.pins[i].len <= off; i++) acc += g_ss.pins[i].len;
        addr = i < g_ss.npin ? g_ss.pins[i].addr + (off - acc) : 0;
        if (addr && (camh = cam_heap_name(addr, d, sizeof d)) != 0) c = CL_CAM;
    }
    r->bytes[c] += len;
    if (c == CL_OTHER && g_r7_resid_o_lines < 60) {   /* fix C2: localize class O commit residue */
        g_r7_resid_o_lines++;
        if (!camh) describe_va(addr, d, sizeof d);
        LOG("R7RESIDO seq=%llu at=%s len=%zu region=%s (class O commit residue: committed resim state vs the live result)\n",
            (unsigned long long)g_cur_seq, d, len, region >= 0 ? g_ss.reg[region].name : region == -1 ? "arena" : "pins");
    }
}
static void r7_resid_note(uint64_t s, int mask) {   /* before g_r7post is re-taken: memory = committed, g_r7post = live */
    ResidCtx r; memset(&r, 0, sizeof r);
    int nr = 0;
    for (int c = 0; c < NCL; c++) if ((mask >> c) & 1) g_r7_resid_frames[c]++;
    snap_diff_live(&g_ss, &g_r7post, resid_cb, &r, 4096, &nr);
    for (int c = 0; c < NCL; c++) g_r7_resid_bytes[c] += r.bytes[c];
    if (g_r7_resid_lines++ < 30) {
        char ms[NCL + 1]; class_str(mask, ms);
        LOG("R7RESID seq=%llu fr=%u cls=%s bytes G=%llu C=%llu F=%llu R=%llu O=%llu ranges=%d (committed resim state vs the live result)\n", (unsigned long long)s, core_frame(), ms,
            (unsigned long long)r.bytes[CL_GAME], (unsigned long long)r.bytes[CL_CAM], (unsigned long long)r.bytes[CL_FX], (unsigned long long)r.bytes[CL_REPLAY],
            (unsigned long long)r.bytes[CL_OTHER], nr);
    }
}
static void r7_c0b_report(const char *why) {
    if (!g_cfg.r7_random_depth && !g_cfg.r7_commit && !g_cfg.r7_forward) return;
    char b[640]; size_t o = 0;
    for (int d = 0; d <= g_cfg.roll_depth && d < 32 && o + 40 < sizeof b; d++)
        o += (size_t)snprintf(b + o, sizeof b - o, " d%d=%llu/%llu/%llu", d, (unsigned long long)g_r7_dn[d], (unsigned long long)g_r7_dchk[d], (unsigned long long)g_r7_dbad[d]);
    LOG("R7DEPTH why=%s random=%d seed=%u frames/checked/gameplay_bad:%s\n", why, g_cfg.r7_random_depth, g_cfg.r7_random_seed, b);
    LOG("R7COMMIT why=%s commit=%d forward=%d commits=%llu resid_frames G=%llu C=%llu F=%llu R=%llu O=%llu resid_bytes G=%llu C=%llu F=%llu R=%llu O=%llu\n", why,
        g_cfg.r7_commit, g_cfg.r7_forward, (unsigned long long)g_r7_commits,
        (unsigned long long)g_r7_resid_frames[CL_GAME], (unsigned long long)g_r7_resid_frames[CL_CAM], (unsigned long long)g_r7_resid_frames[CL_FX],
        (unsigned long long)g_r7_resid_frames[CL_REPLAY], (unsigned long long)g_r7_resid_frames[CL_OTHER],
        (unsigned long long)g_r7_resid_bytes[CL_GAME], (unsigned long long)g_r7_resid_bytes[CL_CAM], (unsigned long long)g_r7_resid_bytes[CL_FX],
        (unsigned long long)g_r7_resid_bytes[CL_REPLAY], (unsigned long long)g_r7_resid_bytes[CL_OTHER]);
    ck_report(why);
    pk_report(why);   /* v0.21.0-S1 */
    LOG("R7BARRIER clamps=%llu last_barrier_seq=%llu why=%s on=%d setup_seq=%llu lsn_c0_seq=%llu lsn_c0_calls=%llu setup_events=%llu (a resim never starts at or before the barrier; k0 clamped forward)\n",
        (unsigned long long)g_r7_bar_clamps, (unsigned long long)bar_seq(), why, g_cfg.r7_setup_barrier, (unsigned long long)g_bar_setup_seq,
        (unsigned long long)g_bar_lsn_seq, (unsigned long long)g_bar_lsn_calls, (unsigned long long)g_bar_setups);
    LOG("R7CAMSPLIT why=%s onsets=%llu logged=%d budget=%d cam_pin_full=%d full_pinned=%d cam_fail=%d (camera-split onsets on the unified path)\n", why,
        (unsigned long long)g_csp_onsets, g_csp_lines, CAMSPLIT_BUDGET, g_cfg.cam_pin_full, g_cam_pinned_full, g_cam_fail);
}
static int g_r7_resid_cur;
__attribute__((noinline)) static int r7_pick_depth(void) {
    if (g_cfg.r7_random_depth) { g_depth_rt = r7_depth_draw(g_cfg.r7_random_seed, g_battle_id, g_battle_steps, g_cfg.roll_depth); return g_depth_rt; }
    return g_depth_rt > 0 && g_depth_rt <= g_cfg.roll_depth ? g_depth_rt : g_cfg.roll_depth;
}
/* after the resims of frame s: commit (keep the resim lineage) or put the live result back */
__attribute__((noinline)) static int r7_after_resims(uint64_t s, const uint64_t live_hcls[NCL]) {
    if (g_cfg.r7_commit && !(g_route_broken || g_defer_broken)) {
        /* v0.20.1: the live camera first, so the residue counts what is really committed; v0.21.0-S1: then the live P */
        int wb = ck_apply(s);
        wb |= pk_apply(s);
        if (wb) { uint64_t hc[NCL]; class_hashes(NULL, hc); g_r7_resid_cur = class_diff(hc, live_hcls); }
        if (g_r7_resid_cur) r7_resid_note(s, g_r7_resid_cur);
        if (!snap_take(&g_ss, &g_r7post)) return 0;
        g_r7post_seq = s;
        g_r7_commits++;
    } else {
        snap_restore(&g_ss, &g_r7post);   /* commit the live result */
        def_restore();
    }
    return 1;
}
/* v0.20.2 CAMSPLIT (unified path): one line per camera-split ONSET, i.e. a checked R7 frame whose check has class C
 * while the previous CHECKED R7 frame's did not (depth-0 frames run no check and leave the state as it was). Own
 * budget (CAMSPLIT_BUDGET lines per session), never the DIFF report budget (max_reports). k = the first k of the
 * frame whose class mask has C; the ranges come from a class-C-only diff of that k against its reference (camera-
 * owned pins count as C, as in R7RESID; image bytes by class_of_va). slots = the first qword of each block slot in
 * live memory when the line is written, as an RVA (0 = not an image pointer or no block known). */
#define CAMSPLIT_MAXR 8
typedef struct { int n; uint32_t len[CAMSPLIT_MAXR]; char name[CAMSPLIT_MAXR][112]; } CamSplit;
static CamSplit g_csp;   /* state variables: next to seg_reset */
static void csp_add(CamSplit *x, const char *nm, size_t len) {
    if (x->n < CAMSPLIT_MAXR) { snprintf(x->name[x->n], sizeof x->name[0], "%s", nm); x->len[x->n] = (uint32_t)len; }
    x->n++;
}
static void camsplit_cb(void *u, int region, size_t off, size_t len, uintptr_t addr) {
    CamSplit *x = (CamSplit *)u;
    char d[112];
    if (region == -2) {   /* pins: one piece per pin (a merged range can cross into the next pin's bytes) */
        size_t i = 0;
        while (i < len) {
            size_t acc = 0; int p = 0;
            for (; p < g_ss.npin && acc + g_ss.pins[p].len <= off + i; p++) acc += g_ss.pins[p].len;
            if (p >= g_ss.npin) return;
            size_t po = off + i - acc, run = g_ss.pins[p].len - po;
            if (run > len - i) run = len - i;
            if (cam_heap_name(g_ss.pins[p].addr + po, d, sizeof d)) csp_add(x, d, run);
            i += run;
        }
        return;
    }
    if (region < 0 || !reg_in_img(g_ss.reg[region].name)) return;   /* arena, WorkMemory, extras, ini regions: never class C */
    size_t i = 0;   /* image regions: split the range at class boundaries (4-byte granules) */
    while (i < len) {
        uintptr_t a = addr + i;
        int c = class_of_va(a - g_base + IMG_PREF);
        size_t j = i + (4 - (a & 3));
        while (j < len && class_of_va(addr + j - g_base + IMG_PREF) == c) j += 4;
        if (j > len) j = len;
        if (c == CL_CAM) { describe_va(a, d, sizeof d); csp_add(x, d, j - i); }
        i = j;
    }
}
/* in the resim loop, at the first k whose mask has C: the class-C diff, only when this frame can be an onset */
static void camsplit_note_k(uint64_t k, const Snap *ref) {
    if (g_csp_have || g_csp_prev || g_csp_lines >= CAMSPLIT_BUDGET) return;
    g_csp_have = 1; g_csp_k = k;
    memset(&g_csp, 0, sizeof g_csp);
    if (ref) { int nr = 0; snap_diff_live(&g_ss, ref, camsplit_cb, &g_csp, 1 << 20, &nr); }
}
static void camsplit_frame_end(uint64_t s, int checked, int umask, uint64_t k0, int D) {
    if (!checked) { g_csp_have = 0; return; }
    int c = (umask >> CL_CAM) & 1;
    if (c && !g_csp_prev) {
        g_csp_onsets++;
        if (g_csp_have && g_csp_lines < CAMSPLIT_BUDGET) {
            g_csp_lines++;
            char sl[200]; size_t o = 0;
            uintptr_t b0 = g_ncamf ? g_camf_base[0] : 0;
            for (int q = 0; q < 5; q++) {
                uint64_t rva = 0;
                if (b0 && mem_readable((void *)(b0 + k_cam_slots[q].off), 8)) { uintptr_t vt = *(uintptr_t *)(b0 + k_cam_slots[q].off); if (IN_IMAGE(vt)) rva = RVA(vt); }
                o += (size_t)snprintf(sl + o, sizeof sl - o, "%s%x:%#llx", q ? "," : "", k_cam_slots[q].off, (unsigned long long)rva);
            }
            char rs[CAMSPLIT_MAXR * 130]; size_t ro = 0; rs[0] = 0;
            for (int i = 0; i < g_csp.n && i < CAMSPLIT_MAXR; i++)
                ro += (size_t)snprintf(rs + ro, sizeof rs - ro, " r%d=%s:%u", i + 1, g_csp.name[i], g_csp.len[i]);
            LOG("CAMSPLIT seq=%llu fr=%u k=%llu k0=%llu depth=%d slots=%s ranges=%d%s\n", (unsigned long long)s, core_frame(), (unsigned long long)g_csp_k,
                (unsigned long long)k0, D, sl, g_csp.n, rs);
        }
    }
    g_csp_prev = c; g_csp_have = 0;
}
static void do_roll7_unified(StepIn *gin, FrameIn *fi, uint64_t s) {
    uint64_t t0 = qpc();
    RingEnt *e = &g_ring[s % (uint64_t)g_R];
    if (!e->lrec && !(e->lrec = lrec_alloc())) { fallback("lrec_alloc"); run_step(gin, 0, s, CTX_STEP); return; }
    g_l4_frame_rev = 0; g_l4_frame_miss = 0;
    if (g_pins_freed) pins_process_freed();   /* fix R1: a free since the tick's processing: drop the pin and the ring now */
    /* between-step writes since the previous post-state (the live one, or with r7_commit the committed one) */
    e->fp.n = 0; e->fp.used = 0; e->fp.overflow = 0;
    size_t foreign = 0;
    if (g_r7post_ok && g_r7post.valid && g_r7post_seq == s - 1 && g_cfg.foreign_replay) foreign = fp_capture(&e->fp, &g_r7post);
    if (foreign) g_r7_foreign++;
    /* pre-snapshot at the g_s0 point */
    e->seq = s; e->valid = 0; e->resid = 0;
    if (!snap_take(&g_ss, &e->snap)) { fallback("snap_alloc"); run_step(gin, 0, s, CTX_STEP); return; }
    e->valid = 1; e->is_post = 0;
    e->fi = *fi;
    if (g_cfg.lsnw && !g_lsnw_buf) g_lsnw_buf = (uint8_t *)VirtualAlloc(NULL, (size_t)(A_CORE_BSS_HI - A_CORE_BSS_LO), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_lsnw_on = g_cfg.lsnw && g_lsnw_buf;
    uint64_t t1 = qpc();
    if (g_cfg.anim_probe) { if (!g_nan || (s % 600) == 0) an_collect(); an_hash_all(0); }
    lrec_begin(e->lrec, 1);
    l1_pass_begin(L1P_LIVE, s, s);
    run_fi(fi, 0, s, RB);
    l1_pass_end();
    lrec_end();
    lsl_note(s, e->lrec);
    g_lsnw_on = 0;
    if (g_cfg.anim_probe) an_hash_all(1);
    if (an_check_pins(s)) { an_drop_pins(); ring_clear("anim_kill"); seg_build(); return; }
    uint64_t t2 = qpc();
    if (g_route_broken || g_defer_broken) { fallback("roll7_live"); return; }
    if (g_pins_freed) {   /* fix R1: a pinned block was freed during the live pass: no snapshot (it would read the freed block),
                           * no resim and no restore this frame; the state stays the live result */
        pinfree_incall(s, "live", s);
        def_save();
        g_r7post_ok = 0; g_r7post.valid = 0; e->valid = 0;
        time_add(qpc_us(t2 - t1));
        return;
    }
    def_save();
    ck_save();   /* v0.20.1: the live camera, put back after the resims when r7_commit keeps their result */
    pk_save();   /* v0.21.0-S1: the live presentation class P (pres_keep) */
    if (!snap_take(&g_ss, &g_r7post)) { fallback("snap_alloc"); return; }
    g_r7post_ok = 1; g_r7post_seq = s;
    class_hashes(&g_r7post, e->hcls);
    uint64_t t3 = qpc();
    int D = r7_pick_depth();
    uint64_t k0 = s + 1 >= (uint64_t)D ? s + 1 - (uint64_t)D : 0;
    int bar = 0;
    if (g_cfg.r7_setup_barrier && D >= 1 && k0 <= bar_seq()) {   /* v0.21.0-S1: never resim across the barrier; k0 moves forward */
        uint64_t nk0 = bar_seq() + 1;
        bar = 1; g_r7_bar_clamps++;
        D = nk0 > s ? 0 : (int)(s + 1 - nk0);
        k0 = s + 1 - (uint64_t)D;
    }
    int have = D >= 1;
    for (uint64_t k = k0; have && k <= s; k++) if (!ring_at(k)) have = 0;
    int ok = 1, div = 0, umask = 0; uint64_t badk = 0;
    uint64_t t4 = t3, t5 = t3, t6 = t3;
    int fwdk = 0, pf_abort = 0; uint64_t fwc0 = g_l4_calls[1];   /* fix R3: forwarded resim passes / calls this frame */
    g_r7_resid_cur = 0;
    if (D >= 0 && D < 32) g_r7_dn[D]++;
    if (have && s + 1 >= (uint64_t)D) {
        FrameIn live_fi = *fi;
        snap_restore(&g_ss, &ring_at(k0)->snap);
        def_drop_from(k0);
        t4 = qpc();
        l1_iter_begin(s, k0);
        for (uint64_t k = k0; k <= s; k++) {
            RingEnt *ek = ring_at(k);
            g_resim_fwd = g_cfg.r7_forward && k == k0;   /* fix R3: only the oldest resim (the frame netplay confirms) is forwarded, once per frame */
            if (g_resim_fwd) fwdk++;
            if (k > k0 && !g_cfg.r7_unified_plant && ek->fp.n) fp_apply(&ek->fp);
            *fi = ek->fi;                       /* the live step's input location, as in sync1x */
            if (g_cfg.lsn_replay) lrec_begin(ek->lrec, 2);
            l1_pass_begin(L1P_RESIM, s, k);
            run_fi(fi, 1, k, RB);
            l1_pass_end();
            if (k == k0 && g_cfg.anim_probe) { an_hash_all(2); an_compare(s); }
            if (g_cfg.lsn_replay) { if (g_lrec_diverged >= 0) div++; lrec_end(); }
            if (g_pins_freed) { pf_abort = 1; pinfree_incall(s, "resim", k); break; }   /* fix R1: never hash or restore the freed pin */
            uint64_t hc[NCL]; class_hashes(NULL, hc);
            int m = class_diff(hc, ek->hcls);
            if (k == s) g_r7_resid_cur = m;
            l1_check(s, k, m & 1, "unified");
            if (m) {
                const Snap *ref = k == s ? &g_r7post : (ring_at(k + 1) ? &ring_at(k + 1)->snap : NULL);
                g_diff_ref = k == s || !g_cfg.r7_commit ? -1 : ek->resid;   /* v0.21.0-S1: see diff_ref_tag */
                if (m & (1 << CL_CAM)) camsplit_note_k(k, ref);   /* v0.20.2: before report_diff (same memory state) */
                if (ok) { badk = k; if (ref) report_diff("roll7", s, k, ref); }
                if ((m & 1) && ref) { pose_dump(s, k, ref); report_gameplay("roll7", s, k, ref, NULL); }
                ok = 0; umask |= m;
                g_diff_ref = -1;
            }
            if (g_route_broken || g_defer_broken) { ok = 0; badk = k; break; }
        }
        g_resim_fwd = 0;
        t5 = qpc();
        *fi = live_fi;
        if (pf_abort) {   /* fix R1: back to the live result in regions and the arena; pins untouched; the frame is not checked */
            snap_restore_nopins(&g_ss, &g_r7post);
            def_restore();
            g_r7post_ok = 0; g_r7post.valid = 0;
            have = 0; ok = 1; umask = 0; g_r7_resid_cur = 0;
            if (g_ck_saved) { g_ck_saved = 0; g_ck_act = 'x'; }   /* v0.20.1: pins may hold a freed block: no camera write-back */
            if (g_pk_saved) { g_pk_saved = 0; g_pk_act = 'x'; }   /* v0.21.0-S1: the regions (and P with them) are the live result again */
            t6 = qpc();
        } else {
            if (!r7_after_resims(s, e->hcls)) { fallback("snap_alloc"); return; }
            e->resid = g_cfg.r7_commit ? g_r7_resid_cur : 0;   /* v0.21.0-S1: what frame s+1's pre-snapshot differs by from s's live result */
            t6 = qpc();
            g_r7_n++;
            if (!ok) g_r7_bad++;
            for (int c = 0; c < NCL; c++) g_r7_cls[c] += (umask >> c) & 1;
            if (umask & 1) g_r7_game_frames++;
            if (D >= 0 && D < 32) { g_r7_dchk[D]++; if (umask & 1) g_r7_dbad[D]++; }
        }
    } else {
        snap_restore(&g_ss, &g_r7post);
        def_restore();
        g_ck_saved = 0;   /* no resim: the live result (and its camera) is already in place */
        g_pk_saved = 0;   /* v0.21.0-S1: and its P */
    }
    {   int Dc = g_cfg.r7_random_depth ? g_cfg.roll_depth : D;   /* random depth: confirm at the deepest depth any later frame may use */
        def_confirm_before(s + 2 > (uint64_t)Dc ? s + 2 - (uint64_t)Dc : 0); }
    time_add(qpc_us(t2 - t1));
    int checked = have && s + 1 >= (uint64_t)D;
    char ms[NCL + 1]; class_str(umask, ms);
    char rs[NCL + 1]; class_str(g_r7_resid_cur, rs);
    char lb[200];
    LOG("R7 seq=%llu fr=%u depth=%d path=unified scrub=%d L3=%d ok=%d checked=%d badk=%lld cls=%s gameplay=%d foreign=%zu fp_ranges=%d fp_overflow=%d lsn_calls=%d replay_div=%d%s us_live=%.0f us_post=%.0f us_resim=%.0f us_back=%.0f us_total=%.0f rnd=%d commit=%d fwd=%d l4=%d resid=%s l4rev=%llu l4miss=%llu fwdk=%d fwd_calls=%llu pinfree=%d ck=%c pk=%c bar=%d\n",
        (unsigned long long)s, core_frame(), D, g_scrub ? g_cfg.stack_scrub_kb : 0, g_l3_on, ok, checked, ok ? -1LL : (long long)(s - badk), ms,
        (umask & 1) != 0, foreign, e->fp.n, e->fp.overflow, e->lrec->n, div, lsl_bad(ok, badk, lb, sizeof lb), qpc_us(t2 - t1), qpc_us(t3 - t2), qpc_us(t5 - t4), qpc_us(t6 - t5), qpc_us(t6 - t0),
        g_cfg.r7_random_depth, g_cfg.r7_commit, g_cfg.r7_forward, g_l4, g_cfg.r7_commit && checked ? rs : "na", (unsigned long long)g_l4_frame_rev, (unsigned long long)g_l4_frame_miss,
        fwdk, (unsigned long long)(g_l4_calls[1] - fwc0), pf_abort, g_ck_act, g_pk_act, bar);
    camsplit_frame_end(s, checked, umask, k0, D);   /* v0.20.2 */
    { char ls[96]; l1_frame_end(s, "unified", lsl_str(s, ls, sizeof ls)); }
    if (g_route_broken || g_defer_broken) fallback("roll7_resim");
}
static void do_roll7(StepIn *gin, FrameIn *fi, uint64_t s) {
    if (g_r7_path < 0) g_r7_path = g_cfg.r7_path_ini;
    if (g_r7_path == 3) { do_roll7_unified(gin, fi, s); return; }
    uint64_t t0 = qpc();
    {
        int D1 = g_depth_rt > 0 && g_depth_rt <= g_cfg.roll_depth ? g_depth_rt : g_cfg.roll_depth;
        if (g_r7_path == 1 && D1 == 1) { do_roll7_sync(gin, fi, s); return; }
    }
    RingEnt *e = &g_ring[s % (uint64_t)g_R];
    size_t foreign = 0;
    if (!e->lrec && !(e->lrec = lrec_alloc())) { fallback("lrec_alloc"); run_step(gin, 0, s, CTX_STEP); return; }
    e->fp.n = 0; e->fp.used = 0; e->fp.overflow = 0;
    if (e->valid && e->seq == s && e->is_post) {
        if (g_cfg.check_foreign) {
            foreign = fp_capture(&e->fp, &e->snap);
            if (foreign) {
                g_r7_foreign++;
                if (g_reports < g_cfg.max_reports) report_diff("foreign", s, s, &e->snap);
                if (!g_cfg.r7_plant_stale && !snap_take(&g_ss, &e->snap)) { fallback("snap_alloc"); run_step(gin, 0, s, CTX_STEP); return; }
            }
        }
    } else {
        e->seq = s;
        if (!snap_take(&g_ss, &e->snap)) { fallback("snap_alloc"); run_step(gin, 0, s, CTX_STEP); return; }
        e->valid = 1;
    }
    e->is_post = 0;
    e->fi = *fi;
    uint64_t t1 = qpc();
    /* round 7: learn UE's core writes on roll7 live steps too (round 6: throw/CE writes only seen here) */
    if (g_cfg.lsnw && !g_lsnw_buf) g_lsnw_buf = (uint8_t *)VirtualAlloc(NULL, (size_t)(A_CORE_BSS_HI - A_CORE_BSS_LO), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_lsnw_on = g_cfg.lsnw && g_lsnw_buf;
    if (g_cfg.anim_probe) { if (!g_nan || (s % 600) == 0) an_collect(); an_hash_all(0); }
    int fresh = 0;
    {
        int D1 = g_depth_rt > 0 && g_depth_rt <= g_cfg.roll_depth ? g_depth_rt : g_cfg.roll_depth;
        if (g_r7_path == 2 && D1 == 1) fresh = snap_take(&g_ss, &g_r7fresh);   /* exactly where sync1x takes g_s0 */
    }
    lrec_begin(e->lrec, 1);
    l1_pass_begin(L1P_LIVE, s, s);
    run_fi(fi, 0, s, RB);
    l1_pass_end();
    lsl_note(s, e->lrec);
    if (g_cfg.anim_probe) an_hash_all(1);
    if (an_check_pins(s)) {   /* the live step freed a pinned motion region: drop the pins before any snapshot reads them */
        an_drop_pins();
        ring_clear("anim_kill"); seg_build();
        lrec_end();
        return;
    }
    lrec_end();
    g_lsnw_on = 0;
    uint64_t t2 = qpc();
    class_hashes(NULL, e->hcls);
    RingEnt *n = &g_ring[(s + 1) % (uint64_t)g_R];
    n->seq = s + 1; n->valid = 0;
    if (!snap_take(&g_ss, &n->snap)) { fallback("snap_alloc"); return; }
    n->valid = 1; n->is_post = 1;
    uint64_t t3 = qpc();
    if (g_route_broken || g_defer_broken) { fallback("roll7_live"); return; }
    int D = g_depth_rt > 0 && g_depth_rt <= g_cfg.roll_depth ? g_depth_rt : g_cfg.roll_depth;
    uint64_t k0 = s + 1 >= (uint64_t)D ? s + 1 - (uint64_t)D : 0;
    int have = 1;
    for (uint64_t k = k0; k <= s; k++) if (!ring_at(k)) { have = 0; break; }
    int ok = 1, div = 0, umask = 0; uint64_t badk = 0;
    uint64_t t4 = t3, t5 = t3, t6 = t3;
    double us_hash = 0;
    if (have && s + 1 >= (uint64_t)D) {
        sw_before();
        cam_save();
        def_save();
        snap_restore(&g_ss, fresh ? &g_r7fresh : &ring_at(k0)->snap);   /* includes k0's foreign writes */
        def_drop_from(k0);
        t4 = qpc();
        l1_iter_begin(s, k0);
        for (uint64_t k = k0; k <= s; k++) {
            RingEnt *ek = ring_at(k);
            if (k > k0 && g_cfg.foreign_replay && ek->fp.n) fp_apply(&ek->fp);
            if (g_cfg.lsn_replay) lrec_begin(ek->lrec, 2);
            l1_pass_begin(L1P_RESIM, s, k);
            run_fi(&ek->fi, 1, k, RB);
            l1_pass_end();
            if (k == k0) { sw_after_first(); if (g_cfg.anim_probe) { an_hash_all(2); an_compare(s); } }
            if (g_cfg.lsn_replay) { if (g_lrec_diverged >= 0) div++; lrec_end(); }
            uint64_t th = qpc();
            uint64_t hc[NCL]; class_hashes(NULL, hc);
            us_hash += qpc_us(qpc() - th);
            int m = class_diff(hc, ek->hcls);
            l1_check(s, k, m & 1, fresh ? "fresh" : "ring_old");
            if (m) {
                RingEnt *ref = ring_at(k + 1);
                if (ok) {
                    badk = k;
                    if (ref) report_diff("roll7", s, k, &ref->snap);
                }
                if ((m & 1) && ref) { pose_dump(s, k, &ref->snap); report_gameplay("roll7", s, k, &ref->snap, &ref->fp); }
                ok = 0; umask |= m;
            }
            if (g_route_broken || g_defer_broken) { ok = 0; badk = k; break; }
        }
        t5 = qpc();
        cam_put_back("roll7", s, 1, 0);   /* CAMSYNC check only: roll7 always keeps the live result */
        snap_restore(&g_ss, &n->snap);   /* back to the live timeline */
        def_restore();
        sw_after_all(s);
        if (g_sw_pins_pending) { g_sw_pins_pending = 0; g_repin_request = 1; }
        t6 = qpc();
        g_r7_n++;
        if (!ok) g_r7_bad++;
        for (int c = 0; c < NCL; c++) g_r7_cls[c] += (umask >> c) & 1;
        if (umask & 1) g_r7_game_frames++;
    }
    def_confirm_before(s + 2 > (uint64_t)D ? s + 2 - (uint64_t)D : 0);
    time_add(qpc_us(t2 - t1));
    char ms[NCL + 1]; class_str(umask, ms);
    char lb[200];
    LOG("R7 seq=%llu fr=%u depth=%d path=%s scrub=%d L3=%d anim=%d big=%d ok=%d checked=%d badk=%lld cls=%s gameplay=%d foreign=%zu fp_ranges=%d fp_overflow=%d lsn_calls=%d replay_div=%d%s cam=%s us_pre=%.0f us_live=%.0f us_post=%.0f us_restore=%.0f us_resim=%.0f us_hash=%.0f us_back=%.0f us_total=%.0f\n",
        (unsigned long long)s, core_frame(), D, fresh ? "fresh" : "ring_old", g_scrub ? g_cfg.stack_scrub_kb : 0, g_l3_on, g_anim_pins && !g_anim_disabled, g_bigscrub, ok, have && s + 1 >= (uint64_t)D, ok ? -1LL : (long long)(s - badk), ms, (umask & 1) != 0, foreign,
        e->fp.n, e->fp.overflow, e->lrec->n, div, lsl_bad(ok, badk, lb, sizeof lb), g_cfg.camera_exclude ? "exclude" : "include",
        qpc_us(t1 - t0), qpc_us(t2 - t1), qpc_us(t3 - t2), qpc_us(t4 - t3), qpc_us(t5 - t4), us_hash, qpc_us(t6 - t5), qpc_us(t6 - t0));
    { char ls[96]; l1_frame_end(s, fresh ? "fresh" : "ring_old", lsl_str(s, ls, sizeof ls)); }
    if (g_route_broken || g_defer_broken) fallback("roll7_resim");
}

/* ------------------------------------------------------------------ loopback */
static FrameIn g_rec[RING_MAX];
static uint64_t g_rec_seq[RING_MAX];
static uint64_t g_lb_rb, g_lb_frames; static double g_lb_cost, g_lb_maxcost; static int g_lb_maxdepth;
static uint64_t g_lb_logged, g_lb_fp_frames;
static FrameIn *rec_at(uint64_t seq) { int i = (int)(seq % (uint64_t)g_R); return g_rec_seq[i] == seq ? &g_rec[i] : NULL; }

static void do_loopback(StepIn *gin, FrameIn *fi, uint64_t s) {
    int pl = g_lbp, D = g_cfg.fake_delay;
    int ri = (int)(s % (uint64_t)g_R);
    g_rec[ri] = *fi; g_rec_seq[ri] = s;
    static uint64_t restarts;
    if (g_lb_on && g_ring_cleared) {
        if (restarts++ < 50) LOG("LB restart seq=%llu (ring cleared; frames already predicted stay uncorrected)\n", (unsigned long long)s);
        g_lb_on = 0;
    }
    g_ring_cleared = 0;
    if (!g_lb_on) {
        int restart = g_lb_steps > 0;
        g_lb_on = 1; g_lb_start = s;
        g_lb_last_conf = g_last_real[pl];
        if (!restart) { g_lb_rb = g_lb_frames = g_lb_steps = 0; g_lb_cost = g_lb_maxcost = 0; g_lb_maxdepth = 0; }
        if (!restart || restarts <= 50) LOG("LB start seq=%llu player=%d delay=%d\n", (unsigned long long)s, pl, D);
        g_lbpost.valid = 0;
    }
    /* bytes written into core state since our last step (UE-fed blocks, manager setters) */
    FPatch *fps = &g_ring[s % (uint64_t)g_R].fp;
    fps->n = 0; fps->used = 0; fps->overflow = 0;
    if (g_cfg.foreign_replay && g_lbpost.valid && g_lbpost_seq == s - 1 && fp_capture(fps, &g_lbpost)) g_lb_fp_frames++;
    if (s >= g_lb_start + (uint64_t)D) {
        uint64_t c = s - (uint64_t)D;
        FrameIn *rc = rec_at(c);
        RingEnt *ec = ring_at(c);
        if (rc && ec) {
            uint64_t real = word_of(rc, pl), used = word_of(&ec->fi, pl);
            g_lb_last_conf = real;
            if (real != used) {
                uint64_t t0 = qpc();
                cam_save();
                snap_restore(&g_ss, &ec->snap);
                def_drop_from(c);
                set_word(&ec->fi, pl, real);
                run_fi(&ec->fi, 1, c, RB);
                uint64_t prev = real;
                int ok = 1;
                for (uint64_t k = c + 1; k < s; k++) {
                    RingEnt *ek = &g_ring[k % (uint64_t)g_R];
                    FrameIn *rk = rec_at(k);
                    if (!rk || ek->seq != k) { ok = 0; break; }
                    if (g_cfg.foreign_replay && ek->fp.n) fp_apply(&ek->fp);
                    ek->fi = *rk;
                    uint64_t w = predict(prev, real);
                    set_word(&ek->fi, pl, w);
                    prev = w;
                    ek->is_post = 0;
                    if (!snap_take(&g_ss, &ek->snap)) { ok = 0; break; }
                    ek->valid = 1;
                    run_fi(&ek->fi, 1, k, RB);
                }
                if (ok && g_cfg.foreign_replay && fps->n) fp_apply(fps);   /* this frame's foreign writes, lost by the restore */
                cam_put_back("loopback", s, 1, ok);
                double cost = qpc_us(qpc() - t0);
                int depth = (int)(s - c);
                g_lb_rb++; g_lb_frames += (uint64_t)depth; g_lb_cost += cost;
                if (cost > g_lb_maxcost) g_lb_maxcost = cost;
                if (depth > g_lb_maxdepth) g_lb_maxdepth = depth;
                if (g_lb_logged++ < 2000)
                    LOG("LB rb seq=%llu fr=%u c=%llu depth=%d us=%.0f real=%llx used=%llx ok=%d\n", (unsigned long long)s, core_frame(),
                        (unsigned long long)c, depth, cost, (unsigned long long)real, (unsigned long long)used, ok);
                if (!ok) { fallback("loopback_gap"); run_step(gin, 0, s, CTX_STEP); return; }
                if (g_route_broken || g_defer_broken) { fallback("loopback_resim"); run_step(gin, 0, s, CTX_STEP); return; }
            }
        } else {
            LOG("LB gap seq=%llu c=%llu rec=%d ring=%d\n", (unsigned long long)s, (unsigned long long)c, rc != NULL, ec != NULL);
            fallback("loopback_ring_gap");
            run_step(gin, 0, s, CTX_STEP);
            return;
        }
    }
    RingEnt *e = &g_ring[s % (uint64_t)g_R];
    RingEnt *ep = ring_at(s - 1);
    uint64_t prev_used = ep ? word_of(&ep->fi, pl) : g_last_real[pl];
    e->fi = *fi;
    set_word(&e->fi, pl, predict(prev_used, g_lb_last_conf));
    e->seq = s; e->is_post = 0;
    if (!snap_take(&g_ss, &e->snap)) { fallback("snap_alloc"); run_step(gin, 0, s, CTX_STEP); return; }
    e->valid = 1;
    uint64_t t0 = qpc();
    run_fi(&e->fi, 0, s, RB);
    time_add(qpc_us(qpc() - t0));
    if (g_cfg.foreign_replay) { if (snap_take(&g_ss, &g_lbpost)) g_lbpost_seq = s; else g_lbpost.valid = 0; }
    g_lb_steps++;
    def_confirm_before(s + 1 > (uint64_t)D ? s + 1 - (uint64_t)D : 0);
    if (g_lb_steps % 60 == 0) {
        LOG("LBSUM seq=%llu steps=%llu rollbacks=%llu resim_frames=%llu avg_us=%.0f max_us=%.0f max_depth=%d player=%d delay=%d foreign_frames=%llu\n",
            (unsigned long long)s, (unsigned long long)g_lb_steps, (unsigned long long)g_lb_rb, (unsigned long long)g_lb_frames,
            g_lb_rb ? g_lb_cost / (double)g_lb_rb : 0.0, g_lb_maxcost, g_lb_maxdepth, pl, D, (unsigned long long)g_lb_fp_frames);
        g_lb_rb = g_lb_frames = 0; g_lb_cost = g_lb_maxcost = 0; g_lb_maxdepth = 0; g_lb_fp_frames = 0;
    }
    if (g_route_broken || g_defer_broken) fallback("loopback_live");
}

/* ------------------------------------------------------------------ the hook */
static uint64_t g_passthrough, g_outside_tick, g_other_tid;
static DWORD g_step_tid0;

/* Round 14: a stage fingerprint per battle. The stage index itself was not located; the active stage
 * hit table (0x144844dd0 or 0x144845e80 by 0x14470dedc, blob at obj+0, entry count at blob+0x2c) differs
 * per stage, so its entry count and a hash of the blob's first 256 bytes identify the stage. */
static void stage_fingerprint(void) {
    int slot = *(volatile uint8_t *)VA(0x14470dedcULL) ? 1 : 0;
    uintptr_t obj = VA(slot ? 0x144845e80ULL : 0x144844dd0ULL);
    uintptr_t blob = *(uintptr_t *)obj;
    int n = -1; uint64_t h = 0;
    if (blob && !(blob & 7) && mem_readable((void *)blob, 256)) { n = *(int16_t *)(blob + 0x2c); h = hash_bytes((void *)blob, 256, 5); }
    LOG("STAGE battle=%llu slot=%d entries=%d blob_hash=%016llx (fingerprint: same values = same stage)\n", (unsigned long long)g_battle_id, slot, n, (unsigned long long)h);
}
static void first_step_sanity(void) {
    uintptr_t c0 = *(uintptr_t *)VA(A_CHARA_TBL), c1 = *(uintptr_t *)(VA(A_CHARA_TBL) + 8);
    uintptr_t lst = *(uintptr_t *)VA(A_LISTENER_PTR);
    uintptr_t wm = *(uintptr_t *)VA(A_WORKMEM_PTR), wmraw = *(uintptr_t *)VA(A_WORKMEM_RAW);
    LOG("SANITY chara0=%s chara1=%s expected0_rva=0x%llx expected1_rva=0x%llx listener_rva=0x%llx listener_is_null=%d listener_vt_rva=0x%llx workmem=%p workmem_raw=%p wm_in_arena=%d frame=%u tid=%lu\n",
        c0 == VA(A_PLAYER0) ? "ok" : "MISMATCH", c1 == VA(A_PLAYER0) + PLAYER_STRIDE ? "ok" : "MISMATCH",
        (unsigned long long)(A_PLAYER0 - IMG_PREF), (unsigned long long)(A_PLAYER0 + PLAYER_STRIDE - IMG_PREF),
        (unsigned long long)(IN_IMAGE(lst) ? RVA(lst) : 0), lst == VA(A_NULL_LISTENER),
        (unsigned long long)(lst && mem_readable((void *)lst, 8) && IN_IMAGE(*(uintptr_t *)lst) ? RVA(*(uintptr_t *)lst) : 0),
        (void *)wm, (void *)wmraw, wm && g_arena_ok && arena_owns(&g_arena, (void *)wm), core_frame(), GetCurrentThreadId());
    /* v0.31.0 (§20.3): rbonline skips this read-only scan (apply = 0: it pins nothing); the ARM scan logged the same containers a moment before
     * (S-B on v0.30.0: a second full scan at r=0's first armed step, 21 ms on PC 1, ~110 ms on PC 2) */
    if (g_cfg.scan_containers && !(g_mode == MODE_RBONLINE && !RS_OLD(RS_OLD_FIRSTSCAN))) do_pin_scan("battle_first_step", 0);
    if (g_cfg.track_init) itrack_report("battle_first_step");
}

/* ------------------------------------------------------------------ v0.22.0-C1a: record / replay (GATE-C-DESIGN §8 C1a)
 * One straight live pass per step: the same run_step as roll7's live pass (L3 canonical context, the L4 listener as configured, null
 * queries with l4=1, routing to the arena with deferred frees, the snapshot set with the camera block pinned as in roll7), then the
 * deferred frees are confirmed at once (no resim ever reaches back). No ring, no resim, no commit. c1a_run.c records the input and
 * the checksums, and in replay hands back the recorded input. Every step of the battle goes through here: the game's own short replay
 * (no replay pause) and CAMPIN FAIL (no passthrough) included; a step outside the manager Tick (never seen in a log so far) is the
 * vanilla call with the same recording / injection (passthrough=1, RECON R6). */
static uint64_t g_c1a_steps, g_c1a_inj, g_c1a_pt;
static void c1a_from_fi(C1aIn *c, const FrameIn *f) { c->p1 = f->p1; c->p2 = f->p2; memcpy(c->x, f->extra, 0x18); c->ff = f->flags; }
static void fi_from_c1a(FrameIn *f, const C1aIn *c) { f->p1 = c->p1; f->p2 = c->p2; memcpy(f->extra, c->x, 0x18); f->flags = (uint8_t)(c->ff & 7); }
static void do_c1a(void *iface, StepIn *gin, FrameIn *fi, uint64_t s, int passthrough) {
    C1aIn ue, use;
    c1a_from_fi(&ue, fi);
    int inj = c1a_step_begin(s, &ue, &use, passthrough);
    FrameIn run = *fi;
    if (inj) { fi_from_c1a(&run, &use); g_c1a_inj++; }
    g_c1a_steps++;
    if (passthrough) {
        StepIn in2 = *gin;
        if (inj) fi_make(&in2, &run);
        g_c1a_pt++;
        g_passthrough_active = 1; o_step(iface, &in2); g_passthrough_active = 0;
    } else {
        uint64_t t0 = qpc();
        if (g_cfg.rb_state_slots_set) rb_straight_mark(c1a_next_f() - 1);   /* v0.23.0-C1b: RBSTATEEV pass=straight */
        run_fi(&run, 0, s, RB);
        if (g_cfg.rb_state_slots_set) rb_straight_mark(0);
        time_add(qpc_us(qpc() - t0));
        def_confirm_before(s + 1);   /* this step's deferred frees are final (no resim) */
    }
    c1a_step_end(s);
    if (!passthrough && (g_route_broken || g_defer_broken)) fallback("c1a");
}

static uint64_t g_replay_steps;
DWORD modes_step_tid(void) { return g_step_tid0; }
volatile int g_hk_entry_valid; volatile uint64_t g_hk_entry_qpc;
void hk_step(void *iface, StepIn *in) {
    if (g_l4_in_call) l4_note_ifc(IFC_STEP, "step");
    g_hk_entry_qpc = qpc(); g_hk_entry_valid = 1;   /* v0.23.0-C1b: RBCOST us_core */
    g_iface = iface;
    uint64_t seq = ++g_seq;
    if (g_inert || g_mode == MODE_OFF || g_online || !g_in_tick) {
        if (!g_in_tick && !g_inert) { if (g_outside_tick++ < 20) LOG("STEP outside_tick seq=%llu\n", (unsigned long long)seq); }
        g_passthrough++;
        if (!g_inert && !g_online && is_rbcore_mode(g_mode)) rb_passthrough_step();   /* v0.23.0-C1b: RBABORT cause=passthrough_step */
        if (!g_inert && !g_online && is_c1a(g_mode)) { FrameIn pf; fi_capture(&pf, in); do_c1a(iface, in, &pf, seq, 1); return; }   /* v0.22.0-C1a (R6) */
        g_passthrough_active = 1;
        o_step(iface, in);
        g_passthrough_active = 0;
        return;
    }
    g_tick_steps++;
    g_battle_steps++;
    DWORD tid = GetCurrentThreadId();
    if (!g_step_tid0) g_step_tid0 = tid;
    else if (tid != g_step_tid0 && g_other_tid++ < 20) LOG("STEP other_thread tid=%lu first=%lu\n", tid, g_step_tid0);

    if (!g_first_step_logged) { g_first_step_logged = 1; first_step_sanity(); stage_fingerprint(); threads_census("battle_first_step"); khit_census(); if (is_rbcore_mode(g_mode)) sway_rows(); g_title_dirty = 1; if (!g_depth_rt) { g_depth_rt = g_cfg.roll_depth; } g_scrub = g_cfg.stack_scrub; }
    watch_arm();
    if (g_pending_lbp > 0) {
        g_lbp = g_pending_lbp; g_pending_lbp = -1;
        LOG("LBPLAYER player=%d\n", g_lbp);
        if (g_mode == MODE_LOOPBACK) { ring_clear("lb_player"); g_lb_on = 0; }
    }
    if (g_pending_cam >= 0) {
        g_cfg.camera_exclude = g_pending_cam; g_pending_cam = -1;
        LOG("CAMERA policy=%s seq=%llu fr=%u\n", g_cfg.camera_exclude ? "exclude" : "include", (unsigned long long)seq, core_frame());
        if (needs_routing(g_mode)) { ring_clear("camera_policy"); g_ss_ready = 0; g_lb_on = 0; }
        log_flush();
    }
    if (g_pending_scrub >= 0) {
        g_pending_scrub = -1;   /* round 9: the scrub is always on; F7 is a no-op */
        LOG("SCRUB F7 ignored: the stack scrub is always on (stack_scrub_kb=%d lsn_scrub_kb=%d) seq=%llu\n", g_cfg.stack_scrub_kb, g_cfg.lsn_scrub_kb, (unsigned long long)seq);
    }
    if (g_pending_depth > 0) {
        g_depth_rt = g_pending_depth; g_pending_depth = -1;
        /* a deeper resim must never reach frames whose deferred frees were already committed at the old depth */
        if (needs_routing(g_mode)) { ring_clear("roll_depth"); g_ss_ready = 0; g_lb_on = 0; }
        LOG("DEPTH roll_depth=%d seq=%llu fr=%u mode=%s\n", g_depth_rt, (unsigned long long)seq, core_frame(), g_mode_names[g_mode]); log_flush();
        seg_reset("depth");
    }
    if (g_pending_l3 >= 0) {
        g_pending_l3 = -1; g_l3_on = !g_l3_on;
        LOG("L3 canonical_context=%d seq=%llu fr=%u mode=%s zero_bytes=%zu (all registers zero except the arguments at every step call)\n", g_l3_on,
            (unsigned long long)seq, core_frame(), g_mode_names[g_mode], l3_zero_bytes());
        seg_reset("l3");
        log_flush();
    }
    if (g_pending_mode >= 0) { int m = g_pending_mode; g_pending_mode = -1; set_mode(m, "hotkey"); }
    if (g_mark_pending) { g_mark_pending = 0; LOG("MARK seq=%llu fr=%u mode=%s\n", (unsigned long long)seq, core_frame(), g_mode_names[g_mode]); log_flush(); }
    /* Round 11: the game's own short replay (between rounds). The short-replay controller 0x14037d670
     * sets 0x1448463f8 when it LoadStates a saved slot and replays recorded keys, and clears it at the
     * end. Test modes pass through while it is set; the ring is rebuilt afterwards. */
    if (g_cfg.replay_pause) {
        static int paused;
        int rp = *(volatile int *)VA(0x1448463f8ULL) != 0;
        if (rp != paused) {
            paused = rp;
            LOG("REPLAY %s seq=%llu fr=%u mode=%s\n", rp ? "pause" : "resume", (unsigned long long)seq, core_frame(), g_mode_names[g_mode]);
            if (is_rbcore_mode(g_mode) && rb_on() && g_ss_ready) {   /* v0.23.0-C1b: no rebuild (it would invalidate the ring); the same full pin
                                                                         * scan, merged into the stable layout (rb.c sets the edge barrier itself) */
                do_pin_scan(rp ? "replay_start" : "replay_end", 1); seg_build();
            } else if (needs_routing(g_mode)) { ring_clear(rp ? "replay_start" : "replay_end"); g_ss_ready = 0; g_lb_on = 0; }
            log_flush();
        }
        /* v0.22.0-C1a: no pause in record/replay: the replay steps are recorded and injected like every other step (RECON R6) */
        if (rp && !is_c1a(g_mode)) { g_replay_steps++; g_passthrough++; g_passthrough_active = 1; o_step(iface, in); g_passthrough_active = 0; return; }
    }
    if (g_cfg.ifc_census) ifc_check(seq);
    if (is_rbcore_mode(g_mode)) rb_step_entry_pins();   /* v0.23.0-C1b: the quarantine's visibility for this step (pins_process_freed's moment) */
    pins_process_freed();
    if (needs_routing(g_mode) && !(g_arena_ok && g_mem_hooked)) fallback("arena_or_memhook_unavailable");
    if (needs_routing(g_mode) && (g_route_broken || g_defer_broken)) fallback("broken_flag");
    if (needs_routing(g_mode) && g_ss_ready && *(uintptr_t *)VA(A_WORKMEM_PTR) != g_ss_workmem) {
        LOG("SS workmem_changed old=%p new=%p\n", (void *)g_ss_workmem, (void *)*(uintptr_t *)VA(A_WORKMEM_PTR));
        if (!ss_build("workmem_changed")) fallback("ss_build");
    }
    if (needs_routing(g_mode) && !g_ss_ready && !ss_build("lazy")) fallback("ss_build");
    /* core code allocated new blocks outside our routed steps (e.g. a training-dummy controller
     * swap): re-pin, at most every 30 steps */
    {
        static uint64_t last_adds, last_refresh;
        static uint64_t last_gs;
        int gs_miss = 0;
        if (g_pending_anim >= 0) {
            g_pending_anim = -1; g_anim_pins = !g_anim_pins;
            int rwn = 0; for (int i = 0; i < g_nanpin; i++) rwn += g_anpin[i].rw;
            LOG("ANIM pins=%s seq=%llu fr=%u resim_written_chunks=%d writable=%d disabled=%d\n", g_anim_pins ? "on" : "off", (unsigned long long)seq, core_frame(), g_nanpin, rwn, g_anim_disabled);
            log_flush();
            if (needs_routing(g_mode) && g_ss_ready) g_repin_request = 1;
        }
        if (g_pending_passes >= 0) {
            g_pending_passes = -1;
            if (g_mode == MODE_ROLL7) {
                if (g_r7_path < 0) g_r7_path = g_cfg.r7_path_ini;
                g_r7_path = g_r7_path == 1 ? 3 : g_r7_path == 3 ? 0 : 1;   /* v0.19: sync -> unified -> ring_old -> sync */
                LOG("ROLL7 path=%s seq=%llu fr=%u depth=%d%s\n", k_r7_path[g_r7_path], (unsigned long long)seq, core_frame(), g_depth_rt,
                    g_r7_path == 1 && g_depth_rt != 1 ? " (sync runs at depth 1 only; ring path at this depth)" : "");
                if (g_ss_ready) ring_clear("roll7_path");
                g_r7post_ok = 0;
                seg_reset("roll7_path");
            } else {
                if (!g_x_passes) g_x_passes = g_cfg.sync1x_passes;
                g_x_passes = g_x_passes == 4 ? 2 : 4;
                LOG("SYNC1X passes=%d seq=%llu fr=%u mode=%s\n", g_x_passes, (unsigned long long)seq, core_frame(), g_mode_names[g_mode]);
            }
            log_flush();
        }
        if (g_pending_bigscrub >= 0) {
            g_pending_bigscrub = -1; g_bigscrub = !g_bigscrub;
            LOG("SCRUB big=%d stack_kb=%d lsn_kb=%d seq=%llu fr=%u\n", g_bigscrub, g_bigscrub ? 512 : g_cfg.stack_scrub_kb, g_bigscrub ? 256 : g_cfg.lsn_scrub_kb, (unsigned long long)seq, core_frame());
            log_flush();
        }
        if (needs_routing(g_mode) && g_ss_ready && an_check_pins(seq)) { an_drop_pins(); ring_clear("anim_kill"); seg_build(); }
        if (needs_routing(g_mode) && g_ss_ready && wsx_check(seq)) g_repin_request = 1;
        /* v0.21.1: while CAMPIN FAIL is active (rollback off) the re-pin triggers wait: the passed-through steps' own allocations
         * are core-site allocations outside a routed step and would re-pin every 30 steps (a scan costs ~17 ms in the game).
         * Only the periodic rescan below runs; it re-pins everything (tracked blocks, game-state pins) */
        int rp_ok = !g_cam_fail || is_c1a(g_mode);   /* v0.22.0-C1a: no passthrough there, so the re-pin triggers stay on */
        if (needs_routing(g_mode) && g_ss_ready && rp_ok && g_cfg.gs_pins && seq - last_gs >= 60 && (gs_miss = gs_check())) { last_gs = seq; g_gs_repins++; }
        if (needs_routing(g_mode) && g_ss_ready && rp_ok && (g_repin_request || gs_miss || (g_it_adds != last_adds && seq - last_refresh >= 30))) {
            last_adds = g_it_adds; last_refresh = seq;
            ring_clear(gs_miss ? "khit_list_changed" : g_repin_request ? "extw_autopin" : "new_tracked_blocks");
            g_repin_request = 0;
            do_pin_scan("new_tracked_blocks", 1);
            seg_build();
        }
        if (needs_routing(g_mode) && g_ss_ready && g_cam_fail && g_cam_fail_steps && g_cam_fail_steps % CAMF_RESCAN == 0) {
            last_adds = g_it_adds; last_refresh = seq; last_gs = seq; g_repin_request = 0;
            ring_clear("campin_rescan");
            do_pin_scan("campin_rescan", 1);
            seg_build();
        }
    }
    /* v0.21.1: cam_pin_full=1 and the camera function block is not pinned whole (CAMPIN FAIL): rollback is OFF. Steps pass
     * straight through, as in the replay pause; every CAMF_RESCAN passed steps the pins are rescanned (above). When a scan
     * pins the block (CAMPIN ok, from that rescan or any other scan) rollback resumes on a cleared ring. */
    if (needs_routing(g_mode) && g_cam_fail && is_c1a(g_mode)) {   /* v0.22.0-C1a: no rollback to turn off; the step runs and is recorded */
        static uint64_t warned;
        if (warned != g_battle_id) { warned = g_battle_id; LOG("C1A campin_fail battle=%llu seq=%llu: the camera function block is not pinned whole; record/replay go on (no rollback in C1a; the block is missing from the snapshot set, the dumps and the pin normalization)\n", (unsigned long long)g_battle_id, (unsigned long long)seq); }
    } else if (needs_routing(g_mode) && g_cam_fail) {
        if (g_cam_fail_steps == 0) { ring_clear("campin_fail"); g_lb_on = 0; log_flush(); }
        g_cam_fail_steps++; g_cam_fail_total++; g_passthrough++;
        g_passthrough_active = 1; o_step(iface, in); g_passthrough_active = 0;
        if (g_battle_steps % (uint64_t)g_cfg.flush_every == 0) log_flush();
        if (g_battle_steps % 600 == 0) { modes_periodic_report("periodic"); log_flush(); }
        return;
    }
    if (needs_routing(g_mode) && g_pindeny_bat && !is_c1a(g_mode)) {   /* v0.24.0-C1b2 S1BFIX G2: rollback off for this battle */
        g_passthrough++;
        g_passthrough_active = 1; o_step(iface, in); g_passthrough_active = 0;
        if (g_battle_steps % (uint64_t)g_cfg.flush_every == 0) log_flush();
        return;
    }
    if (g_cam_ok_pending) { g_cam_ok_pending = 0; if (needs_routing(g_mode)) { ring_clear("campin_ok"); g_lb_on = 0; } log_flush(); }
    if (g_mode == MODE_OBSERVE && !g_ws_active && g_cfg.ws_frames > 0 && g_battle_steps == 1)
    { ws_begin(g_cfg.ws_frames, *(uintptr_t *)VA(A_WORKMEM_PTR)); if (g_cfg.track_init) hws_begin(); }

    if (g_q1_rb && g_mode == MODE_RBREPLAY && g_cfg.rb_arm_f > 0 && !g_q1_armed && rb_on() && c1a_next_f() >= (uint32_t)g_cfg.rb_arm_f) {   /* v0.29.2 (§16.9) */
        q1_arm_begin("rbo_arm");
        int ok = ss_build("rbo_arm");
        g_q1_arming = 0;
        if (!ok) fallback("ss_build");   /* as the lazy build: only an unreadable WorkMemory fails it in rbreplay (a walk failure aborts rb instead) */
        log_flush();
    }
    FrameIn fi;
    fi_capture(&fi, in);
    switch (g_mode) {
    case MODE_SYNC1: do_sync1(in, &fi, seq); break;
    case MODE_SYNC1X: (void)do_sync1x(in, &fi, seq); break;
    case MODE_ROLL7: do_roll7(in, &fi, seq); break;
    case MODE_LOOPBACK: do_loopback(in, &fi, seq); break;
    case MODE_RECORD: case MODE_REPLAY: do_c1a(iface, in, &fi, seq, 0); break;   /* v0.22.0-C1a */
    case MODE_RBREPLAY: case MODE_RBLIVE: case MODE_RBNET: rb_step(iface, in, &fi, seq); break;   /* v0.23.0-C1b; v0.24.0 + rblive; C2 + rbnet */
    default: do_observe(in, &fi, seq); break;
    }
    g_last_real[1] = fi.p1; g_last_real[2] = fi.p2;
    {   static uint64_t lsk_logged;   /* v0.23.0-C1b: today's silent pin skip in snap_restore, named once (outside rb: behavior kept) */
        if (g_ss.layout_skips && !lsk_logged) { lsk_logged = 1; LOG("SNAP LAYOUT_MISMATCH restores_with_pins_skipped=%llu mode=%s seq=%llu (a snapshot's pin layout differed from the set; its pins were not restored, as in every version before v0.23.0)\n", (unsigned long long)g_ss.layout_skips, g_mode_names[g_mode], (unsigned long long)seq); }
    }
    if (g_battle_steps % (uint64_t)g_cfg.flush_every == 0) log_flush();
    if (g_battle_steps % 600 == 0) { modes_periodic_report("periodic"); log_flush(); }
}

/* ------------------------------------------------------------------ lifecycle */
void modes_on_init(int before, uint32_t seed, int routed) {
    if (before) l4_note_ifc(IFC_INIT, "init");
    if (before) {
        LOG("BATTLE init seed=%u battle=%llu mode=%s online_flag=%d\n", seed, (unsigned long long)(g_battle_id + 1), g_mode_names[g_mode], g_online);
        if (is_rbcore_mode(g_cfg.mode)) rb_battle_begin();   /* v0.23.0-C1b: RBCLEAR cause=init (before the C0 clear) */
        ring_clear("init");
        if (g_ws_active) ws_abort("init");
        hws_report("init");
        census_battle_reset();
        g_battle_id++;
        g_bar_setup_seq = g_seq + 1; g_bar_setups++;   /* v0.21.0-S1 barrier: the first step after Init */
        g_battle_steps = 0; g_first_step_logged = 0; g_reports = 0; g_lb_on = 0; g_lb_steps = 0; g_greports = 0;
        g_pkd_lines = 0; g_pindeny_bat = 0; g_pinpool_n = 0; g_pinpool_lines = 0;   /* v0.24.0-C1b2: per-battle budgets (PRESKEEPD, PINDENY, RBPINPOOL) */
        seg_reset("battle");
        memset(g_ifc_logged, 0, sizeof g_ifc_logged);
        memset(g_last_real, 0, sizeof g_last_real);
        g_ss_ready = 0;
        spin_lock(&g_pin_lock); snapset_clear_pins(&g_ss); g_npins = 0; spin_unlock(&g_pin_lock);
        g_route_broken = 0; g_defer_broken = 0;
        c1a_battle_begin(g_battle_id);   /* v0.22.0-C1a */
        if (g_q1_rb) { q1_battle_reset(); qc_reset(); }   /* v0.29.2 */
    } else {
        g_x.lcg = seed ^ 0x5c6b5c6bu;
        if (g_cfg.r7_random_depth) LOG("R7RAND battle=%llu seed=%u depth=splitmix64(seed,battle,battle_step) mod %d\n", (unsigned long long)g_battle_id, g_cfg.r7_random_seed, g_cfg.roll_depth + 1);
        g_x.rand_calls = 0;
        g_battle_init_routed = routed;
        mem_stats_log("after_init");
        log_flush();
    }
}

void modes_on_setup(int before) {
    if (before) l4_note_ifc(IFC_SETUP, "setup");
    if (before) {
        LOG("BATTLE setup battle=%llu steps=%llu fr=%u\n", (unsigned long long)g_battle_id, (unsigned long long)g_battle_steps, core_frame());
        g_bar_setup_seq = g_seq + 1; g_bar_setups++;   /* v0.21.0-S1 barrier: the first step after Setup (round start) */
        g_wsx_disabled = 0; g_anim_disabled = 0; g_nan = 0; g_nanpin = 0;
        c1a_note_setup();   /* v0.22.0-C1a: listed as slot +0x60 in the next ifc= */
        if (is_rbcore_mode(g_cfg.mode)) rb_battle_setup();   /* v0.23.0-C1b */
        ring_clear("setup");
        g_ss_ready = 0;
    } else {
        mem_stats_log("after_setup");
        log_flush();
    }
}

void modes_on_shutdown(int before) {
    if (before) l4_note_ifc(IFC_SHUTDOWN, "shutdown");
    if (before) {
        LOG("BATTLE shutdown battle=%llu steps=%llu fr=%u\n", (unsigned long long)g_battle_id, (unsigned long long)g_battle_steps, core_frame());
        if (is_rbcore_mode(g_cfg.mode)) rb_battle_end();   /* v0.23.0-C1b: the tail, RBEND, RBCOST, RBLEDGER, the quarantine (before the C1a END) */
        c1a_battle_end();   /* v0.22.0-C1a: END line, files closed */
        if (is_c1a(g_mode) || g_c1a_steps)
            LOG("STATS c1a steps=%llu injected=%llu passthrough=%llu (record/replay straight live passes, whole session)\n", (unsigned long long)g_c1a_steps,
                (unsigned long long)g_c1a_inj, (unsigned long long)g_c1a_pt);
        randx_report("shutdown");   /* v0.22.0-C1a (record/replay only) */
        ring_clear("shutdown");
        spin_lock(&g_pin_lock); snapset_clear_pins(&g_ss); g_npins = 0; spin_unlock(&g_pin_lock);
        g_ss_ready = 0;
        if (g_ws_active) ws_abort("shutdown");
        hws_report("shutdown");
        if (g_cfg.track_init) itrack_report("shutdown");
        modes_periodic_report("shutdown");
        LOG("STATS passthrough=%llu outside_tick=%llu other_thread=%llu sync1=%llu/%llu bad roll7=%llu/%llu bad foreign=%llu\n",
            (unsigned long long)g_passthrough, (unsigned long long)g_outside_tick, (unsigned long long)g_other_tid,
            (unsigned long long)g_y1_bad, (unsigned long long)g_y1_n, (unsigned long long)g_r7_bad, (unsigned long long)g_r7_n,
            (unsigned long long)g_r7_foreign);
        LOG("STATS sync1x frames=%llu live_ne_replay=%llu live_ne_muted=%llu muted_ne_muted=%llu lsn_replayed=%llu lsn_divergences=%llu outparam_calls=%llu\n",
            (unsigned long long)g_x_n, (unsigned long long)g_x_lr, (unsigned long long)g_x_lm, (unsigned long long)g_x_mm,
            (unsigned long long)g_lrec_replayed, (unsigned long long)g_lrec_divergences, (unsigned long long)g_lrec_outparam_calls);
        LOG("STATS learned_listener_windows=%d learned_writes_replayed=%llu\n", g_nlw, (unsigned long long)g_lw_replayed);
        LOG("STATS r10 game_state_repins=%llu replay_passthrough_steps=%llu\n", (unsigned long long)g_gs_repins, (unsigned long long)g_replay_steps);
        LOG("CAMPINSTAT why=shutdown cam_pin_full=%d fail_active=%d fail_scans=%llu ok=%llu passthrough_steps=%llu full_pinned=%d (v0.21.1: steps passed straight through while CAMPIN FAIL was active)\n",
            g_cfg.cam_pin_full, g_cam_fail, (unsigned long long)g_cam_fail_scans, (unsigned long long)g_cam_fail_oks, (unsigned long long)g_cam_fail_total, g_cam_pinned_full);
        LOG("BETWEEN policy=%s roll7_frames_with_between_writes=%llu (core-state bytes written between steps are recorded per frame and re-applied in resims)\n",
            g_cfg.foreign_replay ? "record" : "off", (unsigned long long)g_r7_foreign);
        LOG("STATS r8 stack_outparam_calls=%llu stack_outparam_bytes=%llu scrubs=%llu stack_scrub=%d roll_depth=%d\n", (unsigned long long)g_stk_calls,
            (unsigned long long)g_stk_bytes, (unsigned long long)g_scrubs, g_scrub, g_depth_rt);
        for (int c = 0; c < NCL; c++)
            LOG("STATSCLS class=%c sync1x_live_ne_replay=%llu sync1x_live_ne_muted=%llu sync1x_muted_ne_muted=%llu roll7_frames_differing=%llu\n",
                k_cl_char[c], (unsigned long long)g_xc_lr[c], (unsigned long long)g_xc_lm[c], (unsigned long long)g_xc_mm[c],
                (unsigned long long)g_r7_cls[c]);
        LOG("STATS roll7_gameplay_frames=%llu camsync_checks=%llu camsync_mismatch=%llu camsync_adopt=%llu camera=%s\n",
            (unsigned long long)g_r7_game_frames, (unsigned long long)g_camsync_n, (unsigned long long)g_camsync_mismatch,
            (unsigned long long)g_camsync_adopt, g_cfg.camera_exclude ? "exclude" : "include");
        log_flush();
    } else {
        mem_stats_log("after_shutdown");
        log_flush();
    }
}

/* ------------------------------------------------------------------ optA M2 (OPTA-M2-SPEC.md §5.4; sc6rb/M2-INTERFACES.md §3.6, §9): mode=rbonline's
 * entry points into this file. ADDITIVE: no existing mode reaches them, and rbonline reaches nothing else of this file except run_step (through
 * rb.c). rbonline installs none of this file's hooks at process start (no hk_step, no hk_init / hk_setup / hk_shutdown, no MinHook Tick, no
 * ifc census thunks, no watch DRs, no hotkeys); its battle starts PASSIVE, and the rollback machinery below is set up at ARM (round r's
 * InputSync 1 -> 2) and torn down at END, on the game thread, from src/rbo_core.c. */
#ifdef SC6RB_HARNESS
int g_h_rs_old;   /* v0.31.0: h_rbo_rs_old (common.h RS_OLD_*: v0.30.0's round-edge rules, one piece at a time) */
void (*rbo_h_edge_mid_fn)(void);   /* v0.32.0 (review m2) harness only: the RSM2 model's "another thread tracks a block during the replay_end scan" */
#endif
int modes_rbo_init(void) {   /* process start: the allocation part of modes_init (no game write) */
#ifdef SC6RB_HARNESS
    g_h_rs_old = cfg_geti("h_rbo_rs_old", 0); g_h_rs_pinf_plant = cfg_geti("h_rbo_rs_pinf_plant", 0);
    {   extern int g_h_it_wide; g_h_it_wide = cfg_geti("h_rbo_it_wide", 0); }
    if (g_h_rs_old || g_h_rs_pinf_plant) LOG("RB CFG harness control h_rbo_rs_old=%d h_rbo_rs_pinf_plant=%d (v0.30.0's round-edge rules: 1 rescan2, 2 census, 4 first-step scan, 8 getsize, 16 memo, 32 filters, 64 l4 counter; v0.31.0's: 128 the replay-edge baseline after the scan)\n",
                                             g_h_rs_old, g_h_rs_pinf_plant);
#endif
    g_scan_memo_on = !RS_OLD(RS_OLD_MEMO);   /* v0.31.0 (§20.3): the scan's probe memo (rbonline only: scan.c) */
    g_lbp = g_cfg.lb_player;
    g_l3_on = 1;                 /* every pass through the L3 wrapper; run_step picks rbonline's L3-stock context */
    g_l4 = 1;                    /* the canonical listener (L4 revert, null queries in every pass), as rbnet */
    g_l4_buf = (uint8_t *)VirtualAlloc(NULL, CORE_BSS_LEN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    int ok = rb_cfg_check() && g_l4_buf;
    q1_setup();   /* v0.29.2: the walk and the late-pin rule (rbonline: always) */
    lp_install_family();
    c1a_init();
    rb_init();
    LOG("MODES rbonline init ok=%d l4_buf=%p l3=1 l4=1 (the armed path's tables; nothing installed until ARM)\n", ok, (void *)g_l4_buf);
    return ok;
}
/* ARM: the per-battle resets of modes_on_init(before/after) that the passive battle start skipped, then the snapshot set (the C1b machinery's
 * per-battle state starts at the first armed step: f = 1 is round r's first step) */
/* v0.31.0 (§20.3): the round-edge counters of this battle (rbo.c end_lines: RB EDGE) */
void modes_rbo_edge(char *out, size_t cap) {
    snprintf(out, cap, "l4_big_calls=%llu l4_big_us_max=%.0f l4_big_us_sum=%.0f size_skips=%llu pinf_skips=%llu itf_skips=%llu",
             (unsigned long long)g_l4_big_calls, g_l4_big_us_max, g_l4_big_us, (unsigned long long)g_l4_size_skips, (unsigned long long)rb_pinf_skips(),
             (unsigned long long)itrack_filter_skips());
}
int modes_rbo_arm(void *orig_step) {
    o_step = (StepFn)orig_step;
    g_l4_big_calls = 0; g_l4_big_us = 0; g_l4_big_us_max = 0; g_l4_size_skips = 0;   /* v0.31.0 (RB EDGE) */
    rb_battle_begin();
    ring_clear("init");
    census_battle_reset();
    g_battle_id++;
    g_bar_setup_seq = g_seq + 1; g_bar_setups++;
    g_battle_steps = 0; g_first_step_logged = 0; g_reports = 0; g_lb_on = 0; g_lb_steps = 0; g_greports = 0;
    g_pkd_lines = 0; g_pindeny_bat = 0; g_pinpool_n = 0; g_pinpool_lines = 0;
    seg_reset("battle");
    memset(g_ifc_logged, 0, sizeof g_ifc_logged);
    memset(g_last_real, 0, sizeof g_last_real);
    g_ss_ready = 0;
    spin_lock(&g_pin_lock); snapset_clear_pins(&g_ss); g_npins = 0; spin_unlock(&g_pin_lock);
    g_route_broken = 0; g_defer_broken = 0;
    c1a_battle_begin(g_battle_id);
    g_x.lcg = 0x5c6b5c6bu; g_x.rand_calls = 0;   /* our snapshotted extras (no rand hook in rbonline: §9 row M8) */
    g_battle_init_routed = 0;
    LOG("BATTLE rbo_arm battle=%llu mode=%s seq=%llu fr=%u (the battle started passive: Init/Setup ran untracked; v0.29.2: the Q1 root walk + the late pins, OPTA-M2-SPEC.md §16)\n",
        (unsigned long long)g_battle_id, g_mode_names[g_mode], (unsigned long long)g_seq, core_frame());
    q1_battle_reset();
    q1_arm_begin("rbo_arm");   /* v0.29.2: the itrack generation (rbo_core_arm cleared itrack: every later entry is late), the walk's gate */
    int ok = ss_build("rbo_arm");
    g_q1_arming = 0;
    if (!ok) g_q1_armed = 0;
    log_flush();
    return ok;
}
/* the armed step: hk_step's rollback-core path, minus everything rbonline never installs (hotkeys, watch DRs, the ifc census, mode switches,
 * fallback to observe: a failure here aborts the rb battle, which rbo turns into a DRAIN) */
static uint64_t g_rbo_last_adds, g_rbo_last_refresh, g_rbo_last_gs; static int g_rbo_rp;
void modes_rbo_step(void *iface, StepIn *in) {
    if (g_l4_in_call) l4_note_ifc(IFC_STEP, "step");
    g_hk_entry_qpc = qpc(); g_hk_entry_valid = 1;
    g_iface = iface;
    uint64_t seq = ++g_seq;
    g_tick_steps++;
    g_battle_steps++;
    DWORD tid = GetCurrentThreadId();
    if (!g_step_tid0) g_step_tid0 = tid;
    else if (tid != g_step_tid0 && g_other_tid++ < 20) LOG("STEP other_thread tid=%lu first=%lu\n", tid, g_step_tid0);
    if (!g_first_step_logged) {
        g_first_step_logged = 1; first_step_sanity(); stage_fingerprint(); khit_census(); sway_rows(); g_rbo_rp = 0;
        if (!g_depth_rt) g_depth_rt = g_cfg.roll_depth;
        g_scrub = g_cfg.stack_scrub;
        g_rbo_last_adds = g_it_adds; g_rbo_last_refresh = seq; g_rbo_last_gs = seq;
    }
    if (g_cfg.replay_pause) {   /* the game's own short replay: recorded and run like every other step (C1a), the pins rescanned at its edges */
        int rp = *(volatile int *)VA(0x1448463f8ULL) != 0;
        if (rp != g_rbo_rp) {
            g_rbo_rp = rp;
            LOG("REPLAY %s seq=%llu fr=%u mode=%s\n", rp ? "pause" : "resume", (unsigned long long)seq, core_frame(), g_mode_names[g_mode]);
            if (rb_on() && g_ss_ready) {
                uint64_t adds0 = g_it_adds;   /* v0.32.0 (review m2): the baseline is taken BEFORE the scan, as every other rescan takes it: a block
                                               * tracked by another thread during the scan is counted for the next new_tracked_blocks rescan */
                do_pin_scan(rp ? "replay_start" : "replay_end", 1); seg_build();
#ifdef SC6RB_HARNESS
                if (!rp && rbo_h_edge_mid_fn) rbo_h_edge_mid_fn();   /* v0.32.0 RSM2 / XRSM2: a block tracked after the scan read itrack, before the baseline */
#endif
                /* v0.31.0 (OPTA-M2-SPEC.md §20.3): this apply scan already took every tracked block (the late pins: all of them, from the itrack
                 * table now) and every container; the new-tracked-block trigger below would rescan the same table at this same step (V, S-B on
                 * v0.30.0: SCAN why=new_tracked_blocks right after every replay_end, RBPINS extend appended=0). Its baseline moves here, as a
                 * new_tracked_blocks rescan moves it: the next one comes 30 steps on, as after any rescan. g_repin_request / gs_miss still fire */
                if (!RS_OLD(RS_OLD_RESCAN2)) { g_rbo_last_adds = RS_OLD(RS_OLD_M2BASE) ? g_it_adds : adds0; g_rbo_last_refresh = seq; }
            }
        }
    }
    rb_step_entry_pins();
    pins_process_freed();
    if (!(g_arena_ok && g_mem_hooked)) rb_abort_ext("arena_or_memhook_unavailable");
    if (g_ss_ready && *(uintptr_t *)VA(A_WORKMEM_PTR) != g_ss_workmem) {
        LOG("SS workmem_changed old=%p new=%p\n", (void *)g_ss_workmem, (void *)*(uintptr_t *)VA(A_WORKMEM_PTR));
        if (!ss_build("workmem_changed")) rb_abort_ext("ss_build");
    }
    if (!g_ss_ready && !ss_build("lazy")) rb_abort_ext("ss_build");
    {   /* the re-pin triggers (as hk_step): game-state pins every 60 steps, new tracked blocks every 30 */
        int gs_miss = 0;
        if (g_ss_ready && an_check_pins(seq)) { an_drop_pins(); ring_clear("anim_kill"); seg_build(); }
        if (g_ss_ready && wsx_check(seq)) g_repin_request = 1;
        if (g_ss_ready && g_cfg.gs_pins && seq - g_rbo_last_gs >= 60 && (gs_miss = gs_check())) { g_rbo_last_gs = seq; g_gs_repins++; }
        if (g_ss_ready && (g_repin_request || gs_miss || (g_it_adds != g_rbo_last_adds && seq - g_rbo_last_refresh >= 30))) {
            g_rbo_last_adds = g_it_adds; g_rbo_last_refresh = seq;
            ring_clear(gs_miss ? "khit_list_changed" : g_repin_request ? "extw_autopin" : "new_tracked_blocks");
            g_repin_request = 0;
            do_pin_scan("new_tracked_blocks", 1);
            seg_build();
        }
    }
    if (g_cam_ok_pending) { g_cam_ok_pending = 0; ring_clear("campin_ok"); }
    FrameIn fi;
    fi_capture(&fi, in);
    rb_step(iface, in, &fi, seq);
    g_last_real[1] = fi.p1; g_last_real[2] = fi.p2;
    if (g_battle_steps % (uint64_t)g_cfg.flush_every == 0) log_flush();
    if (g_battle_steps % 600 == 0) { modes_periodic_report("periodic"); log_flush(); }
}
/* v0.29.1 (review M2): the degraded arm (this PC agreed on ARM but its core cannot run, or the peer armed alone): every remaining step of the
 * battle runs as ONE live pass through run_step, exactly the pass the armed core runs live (the listener proxy with the canonical queries, the
 * scrubs, the L3-stock wrapper with its pre-zero), without the rollback core (no snapshot, no recording, no CHECK). The allocator cells are
 * the originals (rbo_core.c), so the pass's allocations go to UE as in a stock step.
 * v0.29.5 RBO (F1 / m7, design_v0295/rbo/DESIGN.md §3): v0.29.1-v0.29.4 ran it with flags 0, so g_ctx_flags = 0 switched off the canonical
 * listener path the armed core runs in every pass: L4 (UE's core .bss writes reverted, the canonical result), the FP restore and the 32 KiB
 * scrub after each listener call, rb's state-class rule. UE's +0x0c0 round-start writes (24 class-G PLAYER ranges in 35 of 37 real logs) then
 * stood on the degraded PC only. Now the pass runs with CTX_STEP: exactly the armed core's confirmed live pass (state-class calls forwarded,
 * rb's pass type cleared, as pass=live_confirmed), still without arena routing or deferral (CTX_ROUTE / CTX_DEFER: the cells stay at the
 * originals after a failed ARM). WorkMemory is the current one for L4's rdx window, as on the armed peer. h_rbo_plain_old=1 (harness only):
 * the v0.29.4 flags 0 (control XASYMUE). */
#ifdef SC6RB_HARNESS
static int g_h_plain_old = -1;
#endif
void modes_rbo_step_plain(void *orig_step, void *iface, StepIn *in) {
    if (!o_step) o_step = (StepFn)orig_step;
    g_iface = iface;
    g_scrub = g_cfg.stack_scrub;
    uint64_t seq = ++g_seq;
    FrameIn fi;
    fi_capture(&fi, in);
    uint32_t flags = CTX_STEP;   /* v0.29.5 RBO (F1) */
#ifdef SC6RB_HARNESS
    if (g_h_plain_old < 0) { g_h_plain_old = cfg_geti("h_rbo_plain_old", 0) != 0; if (g_h_plain_old) LOG("MODES harness control h_rbo_plain_old=1 (XASYMUE: the degraded pass runs with flags 0, the v0.29.4 pass)\n"); }
    if (g_h_plain_old) flags = 0;
#endif
    g_ss_workmem = *(volatile uintptr_t *)VA(A_WORKMEM_PTR);   /* v0.29.5 RBO (F1): L4's rdx window treats the current WorkMemory as fight state */
    rb_straight_mark(0);   /* v0.29.5 RBO (F1): no rb pass: a state-class call is forwarded, as in the armed core's confirmed live pass */
    run_fi(&fi, 0, seq, flags);
}
/* END: modes_on_shutdown(before)'s rb part (the tail, RBEND, the quarantine) and the C1a END; the pins go (the core keeps running UE's steps
 * in state 5 with the original step, outside every context) */
void modes_rbo_end(void) {
    LOG("BATTLE rbo_end battle=%llu steps=%llu fr=%u\n", (unsigned long long)g_battle_id, (unsigned long long)g_battle_steps, core_frame());
    rb_battle_end();
    c1a_battle_end();
    ring_clear("shutdown");
    spin_lock(&g_pin_lock); snapset_clear_pins(&g_ss); g_npins = 0; spin_unlock(&g_pin_lock);
    g_ss_ready = 0;
    g_q1_armed = 0;   /* v0.29.2: the walk's gate closes with the battle */
    modes_periodic_report("rbo_end");
    log_flush();
}

/* ------------------------------------------------------------------ v0.29.2-optA-M2: Q1COV (OPTA-M2-SPEC.md §16.9; rbreplay q1_cov=1 only)
 * The set tracked at the ARM frame plus every block tracked later (from its first live pass), to the end of the battle. Each block is hashed
 * at every step entry (a change since the last live pass = written between steps: foreign), before the live pass when the step rolled back,
 * and after the live pass (a change = written by the step). Coverage = what restores the block at the latest pin scan: a snapshot region, the
 * arena, or the pins covering it (their source: E1..E6 the root walk, cam, cont the container scan, gs the game-state pins, late Q1b, pin any
 * other). A block written by the live passes and covered by nothing is a NONE line. The kind names are right here (HWSB keeps its label). */
/* v0.29.4 Q1 (Q1COV v2): + uncov (live-pass writes of a non-zone frame while the block was not covered whole at that moment: such a block is
 * NOT covered even when a later scan covers it, why=write), the written span [wlo, whi) from a shadow copy taken at phase 0 / 1 of every block
 * not covered whole (at most 64 KiB each, 4 MiB in all; beyond it wr=?), wunk (a write seen without a shadow) */
typedef struct { uintptr_t a; uint32_t len, caller, first_f, step_hits, foreign_hits; uint8_t kind, alive, tag, wunk; uint64_t h;
                 uint32_t uncov, wlo, whi; int32_t sh; } QcBlk;
typedef struct { uintptr_t a; uint32_t len; uint8_t tag, used; } QcSrc;
#define QC_CAP 49152
#define QC_MAP (1u << 17)
#define QS_CAP (1u << 17)
#define QSH_CAP (4u << 20)   /* v0.29.4 Q1: the shadow pool */
#define QSH_BLK (64u << 10)  /* v0.29.4 Q1: the largest block that gets a shadow */
static QcBlk *g_qc; static int32_t *g_qc_map; static QcSrc *g_qs;
static int g_qc_n, g_qc_drop; static uint32_t g_qc_f0, g_qc_last_f; static uint64_t g_qc_adds;
static uint8_t *g_qsh; static uint32_t g_qsh_used; static int g_qsh_full;   /* v0.29.4 Q1 */
/* v0.29.5 Q1R (m10): Q1COV's own cost, per battle (the Q1COV COST line): step-phase calls, their time, the bytes hashed and shadow-copied */
static uint64_t g_qcc_phases, g_qcc_hashed, g_qcc_shadow_bytes; static double g_qcc_us, g_qcc_us_max, g_qcc_shadow_us;
static FoundCont g_qc_cen[512]; static int g_qc_ncen;                       /* v0.29.4 Q1: the latest apply scan's census (owner names) */
static uintptr_t g_qc_e5done[Q1_E5_TAB]; static int g_qc_ne5;               /* v0.29.4 Q1: the E5 tables E5CHK already logged this battle */
static inline uint32_t qc_hash(uintptr_t p) { return (uint32_t)(((p >> 3) * 0x9E3779B97F4A7C15ULL) >> 47) & (QC_MAP - 1); }
static int qc_alloc(void) {
    if (g_qc) return 1;
    QC_H_COUNT();   /* v0.29.5 Q1R (m10) */
    g_qc = (QcBlk *)VirtualAlloc(NULL, sizeof(QcBlk) * QC_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_qc_map = (int32_t *)VirtualAlloc(NULL, sizeof(int32_t) * QC_MAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_qs = (QcSrc *)VirtualAlloc(NULL, sizeof(QcSrc) * QS_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_qc || !g_qc_map || !g_qs) { LOG("Q1COV WARN allocation failed: the coverage diagnostic is off\n"); g_qc_on = 0; return 0; }
    g_qsh = (uint8_t *)VirtualAlloc(NULL, QSH_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   /* v0.29.4 Q1: NULL = no written spans (wr=?) */
    if (!g_qsh) LOG("Q1COV WARN shadow allocation failed: NONE lines carry wr=?\n");
    return 1;
}
static void qc_reset(void) {
    g_qc_active = 0; g_qc_n = 0; g_qc_drop = 0; g_qc_f0 = g_qc_last_f = 0;
    if (g_qc_map) memset(g_qc_map, 0xff, sizeof(int32_t) * QC_MAP);
    if (g_qs) memset(g_qs, 0, sizeof(QcSrc) * QS_CAP);
    g_qsh_used = 0; g_qsh_full = 0; g_qc_ncen = 0; g_qc_ne5 = 0;   /* v0.29.4 Q1 */
    g_qcc_phases = g_qcc_hashed = g_qcc_shadow_bytes = 0; g_qcc_us = g_qcc_us_max = g_qcc_shadow_us = 0;   /* v0.29.5 Q1R (m10) */
}
static void qc_note_census(const FoundCont *f, int n) {   /* v0.29.4 Q1 */
    if (n > 512) n = 512;
    if (n > 0) memcpy(g_qc_cen, f, sizeof(FoundCont) * (size_t)n);
    g_qc_ncen = n < 0 ? 0 : n;
}
static int qc_find(uintptr_t a) {   /* the newest block at a, or -1 */
    for (uint32_t i = qc_hash(a), k = 0; k < QC_MAP; k++, i = (i + 1) & (QC_MAP - 1)) {
        int32_t v = g_qc_map[i];
        if (v < 0) return -1;
        if (g_qc[v].a == a) return v;
    }
    return -1;
}
static void qc_map_set(uintptr_t a, int idx) {
    for (uint32_t i = qc_hash(a), k = 0; k < QC_MAP; k++, i = (i + 1) & (QC_MAP - 1)) {
        int32_t v = g_qc_map[i];
        if (v < 0 || g_qc[v].a == a) { g_qc_map[i] = idx; return; }
    }
}
static void qc_add(const ItInfo *it, uint32_t f) {
    int k = qc_find(it->p);
    if (k >= 0 && g_qc[k].alive) return;
    if (g_qc_n >= QC_CAP) { g_qc_drop++; return; }
    QcBlk *b = &g_qc[g_qc_n];
    memset(b, 0, sizeof *b);
    b->a = it->p; b->len = it->size; b->caller = it->caller; b->kind = it->kind; b->first_f = f; b->alive = 1; b->tag = QT_NONE;
    b->sh = -1; b->wlo = 0xffffffffu; b->whi = 0;   /* v0.29.4 Q1 */
    b->h = hash_bytes((const void *)b->a, b->len, 0x51c0);
    qc_map_set(b->a, g_qc_n);
    g_qc_n++;
}
static void qc_refresh(uint32_t f) {   /* blocks tracked since the last look */
    static ItInfo it[24576];
    if (g_it_adds == g_qc_adds) return;
    g_qc_adds = g_it_adds;
    int n = itrack_list_ex(it, 24576, 0, NULL);
    for (int i = 0; i < n; i++) qc_add(&it[i], f);
}
static void qc_begin(uint32_t f) {
    if (!qc_alloc()) return;
    QC_H_COUNT();   /* v0.29.5 Q1R (m10) */
    qc_reset();
    g_qc_active = 1; g_qc_f0 = f; g_qc_last_f = f;
    g_qc_adds = g_it_adds + 1;   /* force the first listing */
    qc_refresh(f);
    LOG("Q1COV begin b=%llu f0=%u blocks=%d (every tracked block, hashed around every live pass to the end of the battle)\n", (unsigned long long)rb_battle_no(), f, g_qc_n);
}
static uint32_t qs_hash(uintptr_t a, uint32_t len) { return (uint32_t)(((a >> 3) * 0x9E3779B97F4A7C15ULL ^ (uint64_t)len * 0xC2B2AE3D27D4EB4FULL) >> 47) & (QS_CAP - 1); }
static void qc_note_pins(const SPin *pins, int n) {   /* the source of every pin of this scan (the first source seen for an (addr, len) stays) */
    if (!g_qs) return;
    for (int i = 0; i < n && i < 40960; i++) {
        for (uint32_t j = qs_hash(pins[i].addr, pins[i].len), k = 0; k < QS_CAP; k++, j = (j + 1) & (QS_CAP - 1)) {
            QcSrc *q = &g_qs[j];
            if (!q->used) { q->used = 1; q->a = pins[i].addr; q->len = pins[i].len; q->tag = g_ptag[i] ? g_ptag[i] : QT_PIN; break; }
            if (q->a == pins[i].addr && q->len == pins[i].len) break;
        }
    }
}
static int qc_src_tag(uintptr_t a, uint32_t len) {
    if (g_qs) for (uint32_t j = qs_hash(a, len), k = 0; k < QS_CAP; k++, j = (j + 1) & (QS_CAP - 1)) {
        QcSrc *q = &g_qs[j];
        if (!q->used) break;
        if (q->a == a && q->len == len) return q->tag;
    }
    return QT_PIN;
}
static int qc_cover(uintptr_t a, uint32_t len) {
    for (int i = 0; i < g_ss.nreg; i++) if (a >= g_ss.reg[i].addr && a + len <= g_ss.reg[i].addr + g_ss.reg[i].len) return QT_REGION;
    if (g_arena_ok && arena_owns(&g_arena, (void *)a)) return QT_ARENA;
    uintptr_t v = a, end = a + len; int tag = QT_NONE;
    while (v < end) {
        int k = snapset_lookup(&g_ss, v);
        if (k < 0) return QT_NONE;
        const SPin *p = &g_ss.pins[k];
        if (tag == QT_NONE) tag = qc_src_tag(p->addr, p->len);
        uintptr_t pe = p->addr + p->len;
        if (pe <= v) return QT_NONE;
        v = pe;
    }
    return tag;
}
static void qc_eval_all(void) {
    for (int i = 0; i < g_qc_n; i++) if (g_qc[i].alive) g_qc[i].tag = (uint8_t)qc_cover(g_qc[i].a, g_qc[i].len);
}
/* rb.c, rbreplay with rb on: phase 0 = the step's entry, 1 = before its live pass (only after a rollback / RPB of the step), 2 = after it */
/* v0.29.4 Q1: a shadow of a block that is not covered whole (phase 0 / 1), for the written span of its next live pass */
static void qc_shadow(QcBlk *b) {
    if (b->tag != QT_NONE || b->len > QSH_BLK || !g_qsh) return;
    if (b->sh < 0) {
        uint32_t need = (b->len + 15u) & ~15u;
        if (g_qsh_used + need > QSH_CAP) { g_qsh_full = 1; return; }
        b->sh = (int32_t)g_qsh_used; g_qsh_used += need;
    }
#ifdef SC6RB_HARNESS
    uint64_t t0 = qpc();   /* v0.29.5 Q1R (m10): the shadow's own time, harness builds only (two qpc per copied block) */
#endif
    memcpy(g_qsh + b->sh, (const void *)b->a, b->len);
    g_qcc_shadow_bytes += b->len;   /* v0.29.5 Q1R (m10) */
#ifdef SC6RB_HARNESS
    g_qcc_shadow_us += qpc_us(qpc() - t0);
#endif
}
static void qc_span(QcBlk *b) {   /* the bytes the live pass changed: [lo, hi) merged into [wlo, whi) */
    const uint8_t *s = g_qsh + b->sh, *c = (const uint8_t *)b->a;
    uint32_t lo = 0, hi = b->len;
    while (lo < b->len && s[lo] == c[lo]) lo++;
    if (lo == b->len) return;
    while (hi > lo && s[hi - 1] == c[hi - 1]) hi--;
    if (lo < b->wlo) b->wlo = lo;
    if (hi > b->whi) b->whi = hi;
}
void modes_q1cov_step(uint32_t f, int phase) {
    if (!g_qc_on || !g_qc_active) return;
    QC_H_COUNT();   /* v0.29.5 Q1R (m10) */
    uint64_t qt0 = qpc();   /* v0.29.5 Q1R (m10): the Q1COV COST line */
    if (phase == 0) qc_refresh(f);
    int zone = phase == 2 ? rb_zone_of(f) : 0;   /* v0.29.4 Q1: a zone frame is never a rollback target (lockstep) */
    for (int i = 0; i < g_qc_n; i++) {
        QcBlk *b = &g_qc[i];
        if (!b->alive) continue;
        if (!itrack_contains(b->a, NULL, NULL)) { b->alive = 0; continue; }   /* freed (a confirmed free, or UE's own) */
        uint64_t h = hash_bytes((const void *)b->a, b->len, 0x51c0);
        g_qcc_hashed += b->len;   /* v0.29.5 Q1R (m10) */
        if (h != b->h) {
            b->h = h;
            if (phase == 0) b->foreign_hits++;
            else if (phase == 2) {
                b->step_hits++;
                if (b->tag == QT_NONE) {   /* v0.29.4 Q1: written by a live pass while not covered whole (the latest scan's coverage) */
                    if (!zone) b->uncov++;
                    if (b->sh >= 0) qc_span(b); else b->wunk = 1;
                }
            }
        }
        if (phase != 2) qc_shadow(b);   /* v0.29.4 Q1 */
    }
    if (phase == 2) g_qc_last_f = f;
    {   double us = qpc_us(qpc() - qt0); g_qcc_us += us; if (us > g_qcc_us_max) g_qcc_us_max = us; g_qcc_phases++; }   /* v0.29.5 Q1R (m10) */
}
static const char *qc_kind(int k) { return k == 0 ? "fmalloc" : k == 1 ? "crt" : k == 2 ? "fmalloc_out" : "crt_out"; }
/* v0.29.4 Q1: the NONE lines' details: the bytes of the block inside regions, the arena or pins; the owner; the allocation site */
static uint64_t qc_pinned_bytes(uintptr_t a, uint32_t len) {
    uintptr_t e = a + len;
    for (int i = 0; i < g_ss.nreg; i++) if (a >= g_ss.reg[i].addr && e <= g_ss.reg[i].addr + g_ss.reg[i].len) return len;
    if (g_arena_ok && arena_owns(&g_arena, (void *)a)) return len;
    const SPin *p = g_ss.stable ? g_ss.spins : g_ss.pins; int n = g_ss.stable ? g_ss.nspin : g_ss.npin;
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (p[m].addr < a) lo = m + 1; else hi = m - 1; }
    int k = hi - 16 < 0 ? 0 : hi - 16;
    uint64_t got = 0; uintptr_t cur = a;   /* pins are sorted by address: one sweep, overlaps counted once */
    for (; k < n && p[k].addr < e; k++) {
        uintptr_t s = p[k].addr > cur ? p[k].addr : cur, t = p[k].addr + p[k].len < e ? p[k].addr + p[k].len : e;
        if (t > s) { got += t - s; cur = t; }
    }
    return got;
}
static const char *qc_site(uint32_t c) {
    switch (c) {
    case 0x38f08a: return "vtb";       case 0x2ddfea: return "list_node"; case 0x37f6a1: return "E3";      case 0x2de0ad: return "E1";
    case 0x38c4f0: return "E4";        case 0x37f81b: case 0x38201e: return "E6";
    case 0x3345b4: case 0x334516: case 0x334a1e: case 0x2fc720: return "E2";
    case 0x37c65d: return "workmem";   case 0x2e25f9: return "E8";
    }
    return "-";
}
static void qc_owner(uintptr_t a, char *out, size_t cap) {
    snprintf(out, cap, "-");
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 3; k++) {
            uint64_t off = Q1_E5_OBJ0 + (uint64_t)k * Q1_E5_STRIDE;
            uintptr_t obj = VA(A_PLAYER0) + (uintptr_t)p * PLAYER_STRIDE + (uintptr_t)off;
            if (mem_readable((void *)(obj + 0x260), 8) && *(uintptr_t *)(obj + 0x260) == a) { snprintf(out, cap, "E5:PLAYER%d+0x%llx", p, (unsigned long long)off); return; }
        }
    for (int s = 0; s < 2; s++) if (*(uintptr_t *)(VA(Q1_A_CTL0) + (uintptr_t)s * Q1_CTL_STRIDE + 0x50) == a) { snprintf(out, cap, "E8:controller%d+0x50", s); return; }
    if (*(uintptr_t *)VA(Q1_A_EMIT_SENT) == a) { snprintf(out, cap, "E1:head"); return; }
    for (int p = 0; p < 2; p++) {
        uintptr_t pl = VA(A_PLAYER0) + (uintptr_t)p * PLAYER_STRIDE;
        if (*(uintptr_t *)(pl + Q1_E4_OFF) == a) { snprintf(out, cap, "E4:PLAYER%d+0x95750", p); return; }
        if (*(uintptr_t *)(pl + Q1_E6_OFF) == a) { snprintf(out, cap, "E6:PLAYER%d+0x3590", p); return; }
    }
    static uintptr_t nodes[8192];
    for (int i = 0; i < g_qc_ncen; i++) {
        const FoundCont *c = &g_qc_cen[i];
        char d[96]; describe_va(c->obj, d, sizeof d);
        if (!mem_readable((void *)c->obj, 16)) continue;
        uintptr_t h = *(uintptr_t *)c->obj; uint64_t n = *(uint64_t *)(c->obj + 8);
        if (h == a) { snprintf(out, cap, "cont:%s:head", d); return; }
        int nn = 0;
        if (!(c->kind == 1 ? check_list(h, n, nodes, 8192, &nn) : check_tree(h, n, nodes, 8192, &nn))) continue;
        for (int j = 0; j < nn; j++) if (nodes[j] == a) { snprintf(out, cap, "cont:%s:node", d); return; }
    }
}
void modes_q1cov_report(void) {
    if (!g_qc_on || !g_qc_active) return;
    QC_H_COUNT();   /* v0.29.5 Q1R (m10) */
    for (int i = 0; i < g_qc_n; i++) if (g_qc[i].alive && itrack_contains(g_qc[i].a, NULL, NULL)) g_qc[i].tag = (uint8_t)qc_cover(g_qc[i].a, g_qc[i].len);
    uint32_t by[QT_N]; memset(by, 0, sizeof by);
    int written = 0, covered = 0, none = 0, fpin = 0, partial = 0, uncovn = 0; uint64_t none_bytes = 0;
    for (int i = 0; i < g_qc_n; i++) {
        const QcBlk *b = &g_qc[i];
        int pinned = b->tag != QT_NONE && b->tag != QT_REGION && b->tag != QT_ARENA;
        if (b->foreign_hits && pinned) fpin++;
        if (!b->step_hits) continue;
        written++;
        /* v0.29.4 Q1: covered = covered whole at the end AND never written by a non-zone live pass while not covered whole */
        if (b->tag == QT_NONE) { none++; none_bytes += b->len; if (qc_pinned_bytes(b->a, b->len)) partial++; }
        else if (b->uncov && !g_qc_hold) { none++; none_bytes += b->len; uncovn++; }   /* g_qc_hold: the harness control (v0.29.3 counted it covered) */
        else { covered++; by[b->tag]++; }
    }
    char bs[256]; size_t o = 0; bs[0] = 0;
    for (int t = QT_E1; t < QT_N; t++) { int k = snprintf(bs + o, sizeof bs - o, "%s%s:%u", o ? "," : "", k_qt[t], by[t]); if (k > 0 && o + (size_t)k < sizeof bs) o += (size_t)k; }
    LOG("Q1COV b=%llu f0=%u end_f=%u blocks=%d written=%d covered=%d by=%s none=%d none_bytes=%llu foreign_pinned=%d partial=%d uncov=%d\n", (unsigned long long)rb_battle_no(), g_qc_f0,
        g_qc_last_f, g_qc_n, written, covered, bs, none, (unsigned long long)none_bytes, fpin, partial, uncovn);   /* v0.29.4 Q1: + partial= uncov= */
    int shown = 0;
    for (int i = 0; i < g_qc_n && shown < 60; i++) {
        const QcBlk *b = &g_qc[i];
        if (!b->step_hits || (b->tag != QT_NONE && (!b->uncov || g_qc_hold))) continue;
        shown++;
        char wr[48], ow[160];   /* v0.29.4 Q1: pinned= wr= why= owner= site= */
        if (b->wlo < b->whi) snprintf(wr, sizeof wr, "0x%x-0x%x", b->wlo, b->whi);
        else snprintf(wr, sizeof wr, "%s", b->wunk || (b->sh < 0 && b->len <= QSH_BLK) || b->len > QSH_BLK ? "?" : "-");
        qc_owner(b->a, ow, sizeof ow);
        LOG("Q1COV NONE addr=0x%llx size=%u caller=0x%x kind=%s step_hits=%u foreign_hits=%u first_f=%u pinned=%llu/%u wr=%s why=%s owner=%s site=%s\n",
            (unsigned long long)b->a, b->len, b->caller, qc_kind(b->kind), b->step_hits, b->foreign_hits, b->first_f,
            (unsigned long long)qc_pinned_bytes(b->a, b->len), b->len, wr, b->tag == QT_NONE ? "end" : "write", ow, qc_site(b->caller));
    }
    shown = 0;
    for (int i = 0; i < g_qc_n && shown < 20; i++) {
        const QcBlk *b = &g_qc[i];
        if (!b->foreign_hits || b->tag == QT_NONE || b->tag == QT_REGION || b->tag == QT_ARENA) continue;
        shown++;
        LOG("Q1COV FOREIGN addr=0x%llx size=%u caller=0x%x kind=%s by=%s step_hits=%u foreign_hits=%u first_f=%u (a pinned block written between steps: the S1-B signature)\n",
            (unsigned long long)b->a, b->len, b->caller, qc_kind(b->kind), k_qt[b->tag], b->step_hits, b->foreign_hits, b->first_f);
    }
    if (g_qc_drop) LOG("Q1COV WARN dropped=%d (more than %d blocks: the counts above are a lower bound)\n", g_qc_drop, QC_CAP);
    if (g_qsh_full) LOG("Q1COV WARN shadow_full used=%u (the 4 MiB shadow pool is full: later uncovered blocks carry wr=?)\n", g_qsh_used);   /* v0.29.4 Q1 */
    {   /* v0.29.5 Q1R (m10): what Q1COV itself cost this battle (the hybrid's step time includes it: rb.c counts it as core, not diag) */
        int nsh = 0; for (int i = 0; i < g_qc_n; i++) if (g_qc[i].sh >= 0) nsh++;
        char hs[48] = "";
#ifdef SC6RB_HARNESS
        snprintf(hs, sizeof hs, " us_shadow=%.0f", g_qcc_shadow_us);   /* harness builds only: the shadow copies' own time */
#endif
        LOG("Q1COV COST b=%llu phases=%llu us=%.0f us_per_phase=%.1f us_max=%.0f hashed=%llu shadow_bytes=%llu shadow_blocks=%d pool=%u/%u%s\n", (unsigned long long)rb_battle_no(),
            (unsigned long long)g_qcc_phases, g_qcc_us, g_qcc_phases ? g_qcc_us / (double)g_qcc_phases : 0.0, g_qcc_us_max, (unsigned long long)g_qcc_hashed,
            (unsigned long long)g_qcc_shadow_bytes, nsh, g_qsh_used, QSH_CAP, hs);
    }
    g_qc_active = 0;
}
/* v0.29.4 Q1: after every walk: the Q1 CONT lines (E7's unknown containers: an empty one once per battle, one with heap nodes every time: it
 * fails the walk) and, in the hybrid with q1_cov=1, Q1COV E5CHK once per E5 table and battle (the asset formula's size against the size the
 * CRT hook recorded for the tracked block; 0 = the block is not tracked). */
static void q1_log_extra(const Q1Res *r, const Q1Env *env) {
    /* v0.29.5 Q1R (m4): what the scan could not record (E7 fails closed on it: fail=E7:overflow / E7:unchecked); nothing logged when all is 0 */
    const ScanQ1Stat *s = env ? env->scan : NULL;
    const char *ign = NULL;   /* the harness control's verdict text (never in the plugin build: h_old is 0 there) */
#ifdef SC6RB_HARNESS
    if (env && (env->h_old & Q1R_OLD_E7)) ign = "ignored (harness h_q1_old_rules=16: v0.29.4's E7)";
#endif
    if (env && (env->ncensus_total > env->ncensus || (s && (s->pins_dropped || s->nodes_cut))))
        LOG("Q1 CONT overflow containers=%d recorded=%d pins_dropped=%d nodes_cut=%d rule=%s\n", env->ncensus_total, env->ncensus, s ? s->pins_dropped : 0,
            s ? s->nodes_cut : 0, ign ? ign : "fail (more containers, link pins or nodes than the scan records: E7 fails closed)");
    if (s && s->unchecked) {
        static const char *const k_uwhy[] = { "-", "ring", "tree", "oversize", "budget" };
        for (int i = 0; i < s->unchecked && i < SCANQ1_U; i++) {
            char d[96]; describe_va(s->u[i].obj, d, sizeof d);
            LOG("Q1 CONT unchecked obj=%s head=0x%llx kind=%s size=%llu why=%s rule=%s\n", d, (unsigned long long)s->u[i].head, s->u[i].kind == 1 ? "list" : "tree",
                (unsigned long long)s->u[i].size, k_uwhy[s->u[i].why >= 1 && s->u[i].why <= 4 ? s->u[i].why : 0],
                ign ? ign : "fail (an MSVC container shape the scan cannot walk exactly: E7 fails closed)");
        }
        if (s->unchecked > SCANQ1_U) LOG("Q1 CONT unchecked more=%d (only the first %d are named)\n", s->unchecked - SCANQ1_U, SCANQ1_U);
    }
    for (uint32_t i = 0; i < r->unk_n && i < Q1_UNK_MAX; i++) {
        int heap = r->unk[i].size > 0 && r->unk[i].nodes_out > 0, seen = 0;
        if (!heap) {
            for (int k = 0; k < g_q1_nunk_seen; k++) if (g_q1_unk_seen[k] == r->unk[i].obj) seen = 1;
            if (seen) continue;
            if (g_q1_nunk_seen < 16) g_q1_unk_seen[g_q1_nunk_seen++] = r->unk[i].obj;
        }
        char d[96]; describe_va(r->unk[i].obj, d, sizeof d);
        LOG("Q1 CONT unknown obj=%s kind=%s size=%llu nodes_out=%d rule=%s\n", d, r->unk[i].kind == 1 ? "list" : "tree", (unsigned long long)r->unk[i].size,
            r->unk[i].nodes_out, heap ? "fail (heap nodes outside the snapshot: E7 fails closed)" : "links (the container scan's link pins)");
    }
    if (r->unk_n > Q1_UNK_MAX) LOG("Q1 CONT unknown more=%u (only the first %d are named)\n", r->unk_n - Q1_UNK_MAX, Q1_UNK_MAX);
    if (!g_qc_on || !g_qc_active) return;
    QC_H_COUNT();   /* v0.29.5 Q1R (m10) */
    for (uint32_t i = 0; i < r->e5_n && i < Q1_E5_TAB; i++) {
        int done = 0;
        for (int k = 0; k < g_qc_ne5; k++) if (g_qc_e5done[k] == r->e5_a[i]) done = 1;
        if (done || g_qc_ne5 >= Q1_E5_TAB) continue;
        g_qc_e5done[g_qc_ne5++] = r->e5_a[i];
        int k = qc_find(r->e5_a[i]);
        uint32_t ts = k >= 0 && g_qc[k].alive ? g_qc[k].len : 0;
        int p = r->e5_slot[i] / 3, s = r->e5_slot[i] % 3;
        LOG("Q1COV E5CHK addr=0x%llx slot=PLAYER%d+0x%llx walk_size=%u track_size=%u eq=%d\n", (unsigned long long)r->e5_a[i], p,
            (unsigned long long)(Q1_E5_OBJ0 + (uint64_t)s * Q1_E5_STRIDE), r->e5_len[i], ts, ts == r->e5_len[i]);
    }
}
