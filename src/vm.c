/* v0.32.0 review fix (REVIEW_BUILD.md B1; OPTA-M2-SPEC.md §21.13): the move-script VM diagnostics of FINDINGS3 §3.1, on the facts of src/vm.h.
 *
 *   D-G  vm_bytecode=1: the bytecode of chosen slots (vm_bytecode_slots, default 0xbd,0x30da) of both players once per battle at the first
 *        confirmed frame, and of every (player, slot) a D-F window traced at the window's end. A pure read between steps on the step thread
 *        (bank [P+0x455c0] -> the record of the slot lookup 0x1402fc400 -> bank + i32 [record+0x38]); every read is bounded by VirtualQuery
 *        (committed, readable): up to 0x400 bytes, fewer when the region ends. No patch.
 *   D-F  vm_trace=1: every native dispatch of every step pass is recorded (src/vm_thunks.S: the pre thunk at 0x1402e6508 records funcIdx,
 *        argc, args[0..15] as the native sees them, the player, [P+0x1c58] lane, [P+0x1c68] slot and the nesting depth; the post thunk at
 *        0x1402e6510 the native's ax). The records of a pass travel with its frame (vm_pass_end) and are written at the frame's confirmation
 *        only (vm_confirm: the confirmed pass, as D-B's A lines), so a 0 ms and a 150 ms run's traces line up frame by frame. A window opens
 *        when either player's var 0x40 becomes 0x0f with var 0x41 == 2 (the confirmed state after the frame's pass), for the first
 *        vm_trace_clashes clashes per battle; it covers vm_trace_pre frames before the arming frame through vm_trace_frames after it.
 *        Lines go to <stem>_vm.txt next to the .rec / .play (VMARM, VMDIFF, VMTF, VMT, VMWARN, VMEND, VMBC*, VMSTAT).
 *   D-H  vm_fill=<value> | rand: args[argc..15] = the value before every dispatch (every thread; argc >= 16: nothing written, counted).
 *   fix  vm_fix=1: the same with 0 (the candidate fix; default OFF everywhere this round; DECISIONS_BUILD item 3).
 * Installed only when vm_trace / vm_fill / vm_fix is on and the mode is record / replay / rbreplay; the CALLCOND block's 137 signature bytes
 * must equal the image (else: VM INSTALL FAIL, nothing patched, every feature off for the launch). Nothing of this runs in rbonline or
 * netobserve (main.c returns before vm_install).
 *
 * v0.32.0 finish review fixes (REVIEW_FINISH.md; OPTA-M2-SPEC.md §21.14):
 *   F4  the battle's first tier-1 difference from the recording in a component other than ctl (vm_t1_diff, from src/c1a_run.c cmp_frame) opens
 *       a window too: VMDIFF, the ring's frames before it (vm_trace_diff_pre, default 48; the ring keeps the latest pass of the last 64 frames)
 *       and vm_trace_frames after it, so a fill-induced divergence before the clash names the dispatches that ran before it.
 *   F5  the thunks time themselves (XSAVE64 / XRSTOR64 and the stack switch included: src/vm_thunks.S C_T0 / C_TCYC) and run_step's bracket
 *       times the whole step: VMSTAT carries hook_us / step_us per pass and per confirmed frame (the TSC rate from QPC since the install),
 *       win_over (records lost inside an emitted frame, with a VMWARN line) and gate=ok|FAIL(...).
 *   n6  a D-G read that failed for a transient reason (bank_null, *_unreadable) is retried, up to 3 tries per (player, slot) and battle (at
 *       each window end); only a definitive result (ok, none, out_of_range, bad_subbank) ends it.
 *   n7  the side file is flushed at each window end; nothing is patched when the process enforces a user shadow stack (CET). */
#include "common.h"
#include "vm.h"
#include "argcfix.h"   /* v0.34.0 (OPTA-M2-SPEC.md §23.2, D-I): argc_pred, the census's C copy of the sub-0x25 / 0x91 predicate */
#include <stddef.h>
#include <stdarg.h>
#include <x86intrin.h>

extern uint8_t vm_tpl_begin[], vm_tpl_pre[], vm_tpl_post[], vm_tpl_ctl[], vm_tpl_end[];
#ifdef SC6RB_HARNESS
extern uint8_t vm_tpl_begin_mut[], vm_tpl_pre_mut[], vm_tpl_post_mut[], vm_tpl_ctl_mut[], vm_tpl_end_mut[];
#endif
void c1a_out_path_ext(char *p, size_t cap, const char *suffix);   /* src/c1a_run.c: <log_dir>\c1a_<ts>_b<battle><suffix> */

typedef struct { uint64_t ret; uint32_t flags, tid; uint16_t fillv, pad16[3]; uint64_t n16, pstk, prefn, postfn, cmpaddr, next, nother, nfill, t0, tcyc; } VmCtl;
_Static_assert(offsetof(VmCtl, flags) == 0x08 && offsetof(VmCtl, tid) == 0x0c && offsetof(VmCtl, fillv) == 0x10 && offsetof(VmCtl, n16) == 0x18 &&
               offsetof(VmCtl, pstk) == 0x20 && offsetof(VmCtl, prefn) == 0x28 && offsetof(VmCtl, postfn) == 0x30 && offsetof(VmCtl, cmpaddr) == 0x38 &&
               offsetof(VmCtl, next) == 0x40 && offsetof(VmCtl, nother) == 0x48 && offsetof(VmCtl, nfill) == 0x50 && offsetof(VmCtl, t0) == 0x58 &&
               offsetof(VmCtl, tcyc) == 0x60 && sizeof(VmCtl) <= 0x80, "VmCtl = src/vm_thunks.S C_*");
#ifdef SC6RB_HARNESS
_Static_assert(sizeof(VmCtl) == sizeof(VmCtlView) && offsetof(VmCtlView, tcyc) == 0x60, "VmCtlView = VmCtl");
#endif
_Static_assert(sizeof(VmRec) == 48, "VmRec");

volatile int g_vm_on, g_vm_trace_on, g_vm_body_on;   /* v0.34.0: g_vm_body_on = trace OR census (the pre-thunk C body runs; run_step brackets on it) */
volatile int g_vm_census_on;                         /* v0.34.0 (OPTA-M2-SPEC.md §23.2, D-I) */
static VmCtl *g_ctl;
static uint8_t *g_page;
static int g_bc_on;

/* v0.34.0 (OPTA-M2-SPEC.md §23.2, D-I): the census of EvaluateIf sub-ops 0x25 / 0x91. For each evaluation, a C copy of the predicate (argc_pred)
 * gives the stock return (reading args[argc], the uninitialized tail) and the fixed return (stopping at args[argc-1]). A summary per battle; a detail
 * line only when the tail word is in class 1-4 (0x1000..0x4000: the only classes that can suppress a registration) or the two returns differ. The
 * raw 16-bit tail is never logged (it is part of the process's stack cookie): only its class nibble and the two verdicts. The evaluated player of
 * sub 0x25 is the running player (rcx = r13); of 0x91 the opponent ([P + 0x973e8]). With the patch applied in the same launch, "differ" counts the
 * frames where the fix changed this PC's result. */
static struct {
    uint64_t n25, n91, class14, differ, no_fields; uint32_t detail;
} CEN;

/* ------------------------------------------------------------------ the records (step thread only) */
#define VM_NB   64          /* frames whose latest pass is kept until their confirmation (rb's pending frames are far fewer) */
#define VM_MAXD 64          /* nesting depth (ExecuteBankSlotScript nests the interpreter) */
#define VM_BC_TRIES 3       /* finish review fix n6: D-G tries per (player, slot) and battle */
typedef struct { uint32_t f, n, over, maxd; int pending, valid, emitted; char ps; VmRec *r; } VmBuf;
static VmBuf g_scr, g_ring[VM_NB];
static uint32_t g_cap;
static struct { uint64_t rsp; uint32_t idx; } g_dstk[VM_MAXD];
static int g_dep;
static uint64_t g_unmatched, g_deep_over, g_cyc_pass, g_tcyc0, g_step_t0, g_tsc0;
static LARGE_INTEGER g_qpc0;

static int player_of(uintptr_t P) {
    uintptr_t p0 = (uintptr_t)VA(A_PLAYER0);
    if (P == p0) return 0;
    if (P == p0 + PLAYER_STRIDE) return 1;
    return 2;
}
static void vm_census_eval(uint64_t P, uint64_t argc, const uint16_t *args, uint16_t sub);   /* v0.34.0 (§23.2), defined after the side file */
/* the pre thunk's C body (private stack, the game's registers saved): rsp_n = the native's rsp (its return slot) */
void vm_pre_c(uint64_t P, uint64_t argc, const uint16_t *args, uint64_t fi, uint64_t rsp_n) {
    uint64_t t0 = __rdtsc();
    while (g_dep > 0 && g_dstk[g_dep - 1].rsp <= rsp_n) { g_dep--; g_unmatched++; }   /* a dispatch at the same or a deeper level never came back here */
    uint32_t idx = UINT32_MAX;
    if (g_scr.n < g_cap) {
        VmRec *r = &g_scr.r[g_scr.n];
        int p = player_of((uintptr_t)P);
        r->fi = (uint8_t)fi; r->argc = (uint8_t)(argc > 255 ? 255 : argc); r->depth = (uint8_t)(g_dep > 255 ? 255 : g_dep); r->p = (uint8_t)p;
        if (p < 2) { r->lane = *(volatile int32_t *)(uintptr_t)(P + VM_P_LANE); r->slot = *(volatile uint16_t *)(uintptr_t)(P + VM_P_SLOT); }
        else { r->lane = -1; r->slot = 0xffff; }
        for (int i = 0; i < 16; i++) r->a[i] = args[i];   /* the interpreter's own frame (always mapped) */
        r->ret = 0; r->has_ret = 0;
        idx = g_scr.n++;
        if ((uint32_t)g_dep > g_scr.maxd) g_scr.maxd = (uint32_t)g_dep;
    } else g_scr.over++;
    if (g_dep < VM_MAXD) { g_dstk[g_dep].rsp = rsp_n; g_dstk[g_dep].idx = idx; g_dep++; } else g_deep_over++;
    if (g_vm_census_on && fi <= 1 && (args[0] == 0x25 || args[0] == 0x91)) vm_census_eval(P, argc, args, args[0]);   /* v0.34.0 (§23.2, D-I) */
    g_cyc_pass += __rdtsc() - t0;
}
/* the post thunk's C body: rsp_i = the interpreter's rsp after the native returned (= the native's rsp + 8) */
void vm_post_c(uint64_t rax, uint64_t rsp_i) {
    uint64_t t0 = __rdtsc(), key = rsp_i - 8;
    while (g_dep > 0 && g_dstk[g_dep - 1].rsp < key) { g_dep--; g_unmatched++; }
    if (g_dep > 0 && g_dstk[g_dep - 1].rsp == key) {
        g_dep--;
        uint32_t idx = g_dstk[g_dep].idx;
        if (idx < g_scr.n) { g_scr.r[idx].ret = (uint16_t)rax; g_scr.r[idx].has_ret = 1; }
    } else g_unmatched++;
    g_cyc_pass += __rdtsc() - t0;
}

/* ------------------------------------------------------------------ the per-battle state and the side file */
static struct {
    uint64_t bat, passes, disp, disp_max, over_passes, cyc, cyc_max, hcyc, hcyc_max, scyc, scyc_max, frames, recs, missing, win_over, nconf;
    uint64_t unmatched0, nother0, nfill0, n160, deep0;
    uint32_t nclash, cl_w1, e_w1, diff_f, diff_w1, nwarn; int cl_set, e_set, diff_done, prev[2];
    uint32_t nbc, nbc_ok; struct { uint8_t p, done, tries, pad; uint16_t slot; } bc[256];
    uint32_t nseen; struct { uint8_t p; uint16_t slot; } seen[256];
    int bc_defaults_done, in_battle;
} S;
static HANDLE g_vf = INVALID_HANDLE_VALUE; static char *g_vb; static size_t g_vn; static int g_vf_tried;
#define VB_CAP (1u << 20)
static void vf_flush(void) {
    if (g_vf != INVALID_HANDLE_VALUE && g_vn) { DWORD w; WriteFile(g_vf, g_vb, (DWORD)g_vn, &w, NULL); }
    g_vn = 0;
}
static void vf_put(const char *s, size_t n) {
    if (!g_vb) return;
    if (!g_vf_tried) {
        g_vf_tried = 1;
        char p[MAX_PATH]; c1a_out_path_ext(p, sizeof p, "_vm.txt");
        g_vf = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        const char *nm = strrchr(p, '\\');
        if (g_vf == INVALID_HANDLE_VALUE) LOG("VM WARN cannot create %s err=%lu (no VM side file this battle)\n", p, GetLastError());
        else LOG("VM FILE b=%llu %s\n", (unsigned long long)S.bat, nm ? nm + 1 : p);
    }
    if (g_vf == INVALID_HANDLE_VALUE) return;
    if (g_vn + n > VB_CAP) vf_flush();
    if (n > VB_CAP) { DWORD w; WriteFile(g_vf, s, (DWORD)n, &w, NULL); return; }
    memcpy(g_vb + g_vn, s, n); g_vn += n;
}
__attribute__((format(printf, 1, 2))) static void vf_printf(const char *fmt, ...) {
    char b[1200]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n > 0) vf_put(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}
/* the TSC rate since the install (QPC as the clock): cycles per microsecond; 0 when not measurable */
static double tsc_mhz(void) {
    LARGE_INTEGER q, fq; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&fq);
    if (!g_tsc0 || !fq.QuadPart) return 0.0;
    double us = (double)(q.QuadPart - g_qpc0.QuadPart) * 1e6 / (double)fq.QuadPart;
    return us > 1000.0 ? (double)(__rdtsc() - g_tsc0) / us : 0.0;
}

/* ------------------------------------------------------------------ D-I: the census (OPTA-M2-SPEC.md §23.2) */
static int argc_evaluated(uint64_t P, uint16_t sub, ArgcIn *in, int *pl) {
    uintptr_t ev = (uintptr_t)P;
    if (sub == 0x91) ev = (uintptr_t)*(volatile uint64_t *)((uintptr_t)P + ARGC_P_OPP);   /* the opponent (r11 = [P + 0x973e8]) */
    *pl = player_of(ev);
    if (!mem_readable((const void *)(ev + ARGC_P_C1), 2) || !mem_readable((const void *)(ev + ARGC_P_C3), 2)) return 0;
    in->c1 = *(volatile uint16_t *)(ev + ARGC_P_C1); in->c2 = *(volatile uint16_t *)(ev + ARGC_P_C2);
    in->c3 = *(volatile uint16_t *)(ev + ARGC_P_C3); in->c4 = *(volatile uint16_t *)(ev + ARGC_P_C4);
    return 1;
}
static void vf_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vm_census_eval(uint64_t P, uint64_t argc, const uint16_t *args, uint16_t sub) {
    ArgcIn in; int pl;
    if (!argc_evaluated(P, sub, &in, &pl)) { CEN.no_fields++; return; }
    int ac = (int)(argc > 16 ? 16 : argc);   /* the native sees at most 16 args; argc > 16 would read the cookie, never seen (VS3) */
    int rs = argc_pred(&in, args, ac, 0), rf = argc_pred(&in, args, ac, 1);
    uint16_t tail = (argc >= 1 && argc < 16) ? args[argc] : 0;   /* args[argc]: the uninitialized tail (the stock read); never logged raw */
    int cls = (argc >= 1 && argc < 16) ? ((tail >> 12) & 0xf) : 0;   /* the class nibble only */
    int class14 = cls >= 1 && cls <= 4;
    if (sub == 0x25) CEN.n25++; else CEN.n91++;
    if (class14) CEN.class14++;
    if (rs != rf) CEN.differ++;
    if ((class14 || rs != rf) && CEN.detail < (uint32_t)g_cfg.vm_census_cap) {
        CEN.detail++;
        uintptr_t p0 = (uintptr_t)VA(A_PLAYER0);
        int32_t lane = (pl < 2) ? *(volatile int32_t *)(p0 + (uintptr_t)pl * PLAYER_STRIDE + VM_P_LANE) : -1;
        uint16_t slot = (pl < 2) ? *(volatile uint16_t *)(p0 + (uintptr_t)pl * PLAYER_STRIDE + VM_P_SLOT) : 0xffff;
        char ab[96]; size_t ao = 0; ab[0] = 0;   /* args[0..argc-1]: the script's own words (NOT the cookie tail at args[argc]) */
        for (int i = 0; i < ac && ao + 6 < sizeof ab; i++) { int k = snprintf(ab + ao, sizeof ab - ao, "%s%04x", i ? "," : "", args[i]); if (k > 0) ao += (size_t)k; }
        vf_printf("VMCEN f=%u p=%d lane=%d slot=0x%04x sub=0x%02x argc=%d class=%d ret_stock=%d ret_fixed=%d fields=%04x,%04x,%04x,%04x args=%s\n",
                  core_frame(), pl, lane, slot, sub, ac, cls, rs, rf, in.c1, in.c2, in.c3, in.c4, ab);
    }
}

/* ------------------------------------------------------------------ D-G: the bytecode of a slot (a guarded read between steps) */
static size_t readable_len(uintptr_t a, size_t want) {
    size_t got = 0;
    while (got < want) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((const void *)(a + got), &mi, sizeof mi)) break;
        DWORD pr = mi.Protect;
        if (mi.State != MEM_COMMIT || (pr & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) break;
        got += (size_t)((uintptr_t)mi.BaseAddress + mi.RegionSize - (a + got));
    }
    return got < want ? got : want;
}
static void hexs(char *o, const uint8_t *b, size_t n) { static const char hx[] = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { o[2 * i] = hx[b[i] >> 4]; o[2 * i + 1] = hx[b[i] & 15]; } o[2 * n] = 0; }
static void bc_dump(int p, uint16_t slot, const char *why) {
    int e = -1;
    for (uint32_t i = 0; i < S.nbc && e < 0; i++) if (S.bc[i].p == p && S.bc[i].slot == slot) e = (int)i;
    if (e < 0) { if (S.nbc >= 256) return; e = (int)S.nbc++; S.bc[e].p = (uint8_t)p; S.bc[e].slot = slot; S.bc[e].done = 0; S.bc[e].tries = 0; }
    if (S.bc[e].done || S.bc[e].tries >= VM_BC_TRIES) return;
    S.bc[e].tries++;
    uintptr_t P = (uintptr_t)VA(A_PLAYER0) + (uintptr_t)p * PLAYER_STRIDE;
    uintptr_t bank = (uintptr_t)*(volatile uint64_t *)(P + VM_P_BANK);
    const char *res = NULL; int definitive = 1;
    uint32_t b = ((uint32_t)slot >> 12) & 0xf, idx = slot & 0x7ffu, base = 0, count = 0; uintptr_t rec = 0; int32_t off = 0; size_t n = 0;
    if (!bank) { res = "bank_null"; definitive = 0; }
    else if (readable_len(bank, 0x30) < 0x30) { res = "bank_unreadable"; definitive = 0; }
    else if (b >= 4) res = "bad_subbank";
    else {
        base = *(volatile uint16_t *)(bank + 0x1c + 4 * b); count = *(volatile uint16_t *)(bank + 0x1e + 4 * b);
        if (idx >= count) res = "out_of_range";
        else {
            rec = bank + ((uintptr_t)(base + idx) * 9 + 6) * 8;
            if (readable_len(rec, VM_REC_LEN) < VM_REC_LEN) { res = "rec_unreadable"; definitive = 0; }
            else {
                off = *(volatile int32_t *)(rec + 0x38);
                if (off == -1) res = "none";
                else { n = readable_len(bank + (uintptr_t)(intptr_t)off, 0x400); if (n) res = "ok"; else { res = "bc_unreadable"; definitive = 0; } }
            }
        }
    }
#ifdef SC6RB_HARNESS
    if (g_cfg.h_vm_bc_once) definitive = 1;   /* control XBCRETRY: v0.32.0-B1's rule (the pair is done after its first read, whatever it gave) */
#endif
    if (definitive) S.bc[e].done = 1;
    if (n) S.nbc_ok++;
    vf_printf("VMBC b=%llu p=%d slot=0x%04x why=%s try=%u result=%s sub=%u base=%u count=%u idx=%u rec_off=0x%llx bc_off=0x%x n=0x%zx\n", (unsigned long long)S.bat, p, slot, why,
              S.bc[e].tries, res, b, base, count, idx, rec ? (unsigned long long)(rec - bank) : 0ULL, (uint32_t)off, n);
    LOG("VM BC b=%llu p=%d slot=0x%04x why=%s try=%u result=%s n=0x%zx\n", (unsigned long long)S.bat, p, slot, why, S.bc[e].tries, res, n);
    if (rec && readable_len(rec, VM_REC_LEN) == VM_REC_LEN) { char h[2 * VM_REC_LEN + 1]; hexs(h, (const uint8_t *)rec, VM_REC_LEN); vf_printf("VMBCR b=%llu p=%d slot=0x%04x rec=%s\n", (unsigned long long)S.bat, p, slot, h); }
    for (size_t at = 0; at < n; at += 64) {
        size_t k = n - at < 64 ? n - at : 64; char h[129]; hexs(h, (const uint8_t *)(bank + (uintptr_t)(intptr_t)off + at), k);
        vf_printf("VMBCB b=%llu p=%d slot=0x%04x at=0x%03zx %s\n", (unsigned long long)S.bat, p, slot, at, h);
    }
}
static void bc_defaults(const char *why) { for (int p = 0; p < 2; p++) for (int i = 0; i < g_cfg.vm_bc_n; i++) bc_dump(p, g_cfg.vm_bc_slots[i], why); }

/* ------------------------------------------------------------------ D-F: the frame's lines */
static void seen_add(int p, uint16_t slot) {
    if (p > 1) return;
    for (uint32_t i = 0; i < S.nseen; i++) if (S.seen[i].p == p && S.seen[i].slot == slot) return;
    if (S.nseen < 256) { S.seen[S.nseen].p = (uint8_t)p; S.seen[S.nseen].slot = slot; S.nseen++; }
}
/* finish review fix F5: a VMT line is formatted by hand (no vsnprintf per dispatch: a window frame can hold thousands of lines) */
static char *put_s(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static char *put_u(char *o, uint64_t v) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) *o++ = t[--n]; return o; }
static char *put_i(char *o, int32_t v) { if (v < 0) { *o++ = '-'; return put_u(o, (uint64_t)(-(int64_t)v)); } return put_u(o, (uint64_t)v); }
static char *put_h(char *o, uint32_t v, int digits) { static const char hx[] = "0123456789abcdef"; for (int i = digits - 1; i >= 0; i--) *o++ = hx[(v >> (4 * i)) & 15]; return o; }
static void emit_buf(uint32_t f, const VmBuf *b, char ps) {
    if (!b) { vf_printf("VMTF b=%llu f=%u missing=1\n", (unsigned long long)S.bat, f); S.missing++; return; }
    vf_printf("VMTF b=%llu f=%u pass=%c n=%u over=%u maxd=%u\n", (unsigned long long)S.bat, f, ps ? ps : '-', b->n, b->over, b->maxd);
    S.frames++;
    if (b->over) {   /* finish review fix F5: records lost inside an emitted frame make that frame's trace incomplete (gate=FAIL) */
        S.win_over += b->over;
        if (S.nwarn++ < 20) {
            vf_printf("VMWARN b=%llu f=%u over=%u cap=%u (records lost in an emitted frame: this frame's trace is incomplete; raise vm_trace_cap)\n", (unsigned long long)S.bat, f, b->over, g_cap);
            LOG("VM WARN b=%llu f=%u over=%u cap=%u (records lost in an emitted frame: this frame's trace is incomplete; raise vm_trace_cap)\n", (unsigned long long)S.bat, f, b->over, g_cap);
        }
    }
    char ln[256];
    for (uint32_t i = 0; i < b->n; i++) {
        const VmRec *r = &b->r[i];
        char *o = ln;
        o = put_s(o, "VMT f="); o = put_u(o, f); o = put_s(o, " pass="); *o++ = ps ? ps : '-';
        o = put_s(o, " i="); o = put_u(o, i); o = put_s(o, " d="); o = put_u(o, r->depth); o = put_s(o, " p="); o = put_u(o, r->p);
        o = put_s(o, " lane="); o = put_i(o, r->lane); o = put_s(o, " slot=0x"); o = put_h(o, r->slot, 4); o = put_s(o, " fi=0x"); o = put_h(o, r->fi, 2);
        o = put_s(o, " argc="); o = put_u(o, r->argc); o = put_s(o, " a=");
        for (int k = 0; k < 16; k++) {
            if (k) *o++ = (r->argc < 16 && k == r->argc) ? '|' : ',';
            else if (r->argc == 0) *o++ = '|';
            o = put_h(o, r->a[k], 4);
        }
        o = put_s(o, " ret="); if (r->has_ret) o = put_h(o, r->ret, 4); else *o++ = '-';
        if (r->fi <= 1) { o = put_s(o, " sub="); o = put_h(o, r->a[0], 4); }
        *o++ = '\n';
        vf_put(ln, (size_t)(o - ln));
        seen_add(r->p, r->slot);
        S.recs++;
    }
}
static VmBuf *ring_of(uint32_t f) { VmBuf *b = &g_ring[f % VM_NB]; return b->valid && b->f == f ? b : NULL; }
static void emit_frame(uint32_t k) { VmBuf *e = ring_of(k); if (e && e->emitted) return; emit_buf(k, e, e ? e->ps : 0); if (e) e->emitted = 1; }
static int every_pass(void) {
#ifdef SC6RB_HARNESS
    return g_cfg.h_vm_every_pass != 0;   /* control XGVM: every pass is written as it ends, not the confirmed one */
#else
    return 0;
#endif
}
/* a window: the confirmed frames lo..upto now (the ones not written yet), and every frame up to hi as it is confirmed */
static void win_open(uint32_t lo, uint32_t upto, uint32_t hi) {
    if (!every_pass()) for (uint32_t k = lo; k <= upto; k++) emit_frame(k);
    if (!S.e_set || hi > S.e_w1) S.e_w1 = hi;
    S.e_set = 1;
}
static void win_end(uint32_t f, const char *why) {   /* the window's end: D-G of every (player, slot) it traced, and the chosen slots again (n6) */
    vf_printf("VMEND b=%llu f=%u why=%s clash=%u slots=%u\n", (unsigned long long)S.bat, f, why, S.nclash, S.nseen);
    if (g_bc_on) { for (uint32_t i = 0; i < S.nseen; i++) bc_dump(S.seen[i].p, S.seen[i].slot, "traced"); bc_defaults("retry"); }
    vf_flush();   /* finish review fix n7: a crash after the window keeps its lines */
}

/* ------------------------------------------------------------------ the pass / confirm hooks (src/c1a_run.c, src/modes.c) */
void vm_step_begin(void) {
    if (!g_ctl || !(g_ctl->flags & 2)) return;
    g_scr.n = 0; g_scr.over = 0; g_scr.maxd = 0; g_scr.pending = 0; g_dep = 0; g_cyc_pass = 0;
    g_tcyc0 = g_ctl->tcyc;
    g_step_t0 = __rdtsc();
    g_ctl->tid = GetCurrentThreadId();
}
void vm_step_end(void) {
    if (!g_ctl || !(g_ctl->flags & 2)) return;
    g_ctl->tid = 0;
    uint64_t sc = __rdtsc() - g_step_t0, hc = g_ctl->tcyc - g_tcyc0;
    g_scr.pending = 1;
    uint64_t d = (uint64_t)g_scr.n + g_scr.over;
    S.passes++; S.disp += d; if (d > S.disp_max) S.disp_max = d; if (g_scr.over) S.over_passes++;
    S.cyc += g_cyc_pass; if (g_cyc_pass > S.cyc_max) S.cyc_max = g_cyc_pass;
    S.hcyc += hc; if (hc > S.hcyc_max) S.hcyc_max = hc;
    S.scyc += sc; if (sc > S.scyc_max) S.scyc_max = sc;
}
void vm_pass_end(uint32_t f, uint16_t cv[2][2]) {
    if (!g_vm_on) return;
    for (int p = 0; p < 2; p++) {
        uintptr_t P = (uintptr_t)VA(A_PLAYER0) + (uintptr_t)p * PLAYER_STRIDE;
        cv[p][0] = *(volatile uint16_t *)(P + 0x19fc); cv[p][1] = *(volatile uint16_t *)(P + 0x19fe);
    }
    if (!g_vm_trace_on) return;
    VmBuf *slot = &g_ring[f % VM_NB];
    if (g_scr.pending) {   /* the latest pass's records become frame f's (a pointer swap) */
        VmBuf t = *slot; *slot = g_scr; slot->f = f; slot->valid = 1; slot->pending = 0; slot->ps = 0; slot->emitted = 0;
        g_scr = t; g_scr.n = 0; g_scr.over = 0; g_scr.maxd = 0; g_scr.pending = 0; g_scr.valid = 0; g_scr.emitted = 0;
    } else { slot->f = f; slot->valid = 1; slot->n = 0; slot->over = 0; slot->maxd = 0; slot->ps = 0; slot->emitted = 0; }   /* a compute with no traced pass behind it */
    if (every_pass() && S.e_set && f <= S.e_w1) emit_buf(f, slot, '*');
}
void vm_confirm(uint32_t f, char ps, const uint16_t cv[2][2]) {
    if (!g_vm_on || !S.in_battle) return;
    S.nconf++;
    if (g_bc_on && !S.bc_defaults_done) { S.bc_defaults_done = 1; bc_defaults("battle"); }   /* D-G: the chosen slots of both players, once per battle */
    if (!g_vm_trace_on) return;
    VmBuf *b = ring_of(f);
    if (b) b->ps = ps;
    int c[2]; for (int p = 0; p < 2; p++) c[p] = cv[p][0] == 0x0f && cv[p][1] == 2;
    int cl_open = S.cl_set && f <= S.cl_w1;
    if (!cl_open && S.nclash < (uint32_t)g_cfg.vm_trace_clashes && ((c[0] && !S.prev[0]) || (c[1] && !S.prev[1]))) {
        S.nclash++; S.cl_set = 1; S.cl_w1 = f + (uint32_t)g_cfg.vm_trace_frames;
        uint32_t w0 = f > (uint32_t)g_cfg.vm_trace_pre ? f - (uint32_t)g_cfg.vm_trace_pre : 1;
        vf_printf("VMARM b=%llu f=%u clash=%u p0=%04x/%04x p1=%04x/%04x window=%u..%u\n", (unsigned long long)S.bat, f, S.nclash, cv[0][0], cv[0][1], cv[1][0], cv[1][1], w0, S.cl_w1);
        LOG("VM ARM b=%llu f=%u clash=%u p0=%04x/%04x p1=%04x/%04x window=%u..%u\n", (unsigned long long)S.bat, f, S.nclash, cv[0][0], cv[0][1], cv[1][0], cv[1][1], w0, S.cl_w1);
        if (f > 1) win_open(w0, f - 1, S.cl_w1); else win_open(1, 0, S.cl_w1);   /* the frames before the arming frame (confirmed, kept) */
    }
    S.prev[0] = c[0]; S.prev[1] = c[1];
    if (S.e_set && f <= S.e_w1 && !every_pass()) emit_frame(f);
    if (S.cl_set && f == S.cl_w1) win_end(f, "clash");
    if (S.diff_done && f == S.diff_w1) win_end(f, "diff");
}
/* finish review fix F4 (src/c1a_run.c cmp_frame, after vm_confirm of the same frame): the battle's first tier-1 difference from the recording in
 * a component other than ctl. The ring's frames before it (the latest pass of each, as confirmed) and this frame are written now, and the next
 * vm_trace_frames frames as they are confirmed. */
void vm_t1_diff(uint32_t f, const char *comps) {
    if (!g_vm_trace_on || !S.in_battle || S.diff_done || g_cfg.vm_trace_diff_pre < 0) return;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_vm_nodiff) return;   /* control XDIFF: v0.32.0-B1 (only the clash windows) */
#endif
    S.diff_done = 1; S.diff_f = f; S.diff_w1 = f + (uint32_t)g_cfg.vm_trace_frames;
    uint32_t lo = f > (uint32_t)g_cfg.vm_trace_diff_pre ? f - (uint32_t)g_cfg.vm_trace_diff_pre : 1;
    vf_printf("VMDIFF b=%llu f=%u comps=%s window=%u..%u (the battle's first tier-1 difference from the recording outside ctl)\n", (unsigned long long)S.bat, f, comps ? comps : "-", lo, S.diff_w1);
    LOG("VM DIFF b=%llu f=%u comps=%s window=%u..%u (the battle's first tier-1 difference from the recording outside ctl: the trace's frames before it are written)\n",
        (unsigned long long)S.bat, f, comps ? comps : "-", lo, S.diff_w1);
    win_open(lo, f, S.diff_w1);
    if (f == S.diff_w1) win_end(f, "diff");
}
void vm_battle_begin(uint64_t battle) {
    if (!g_vm_on) return;
    if (S.in_battle) vm_battle_end();
    memset(&S, 0, sizeof S);
    S.bat = battle; S.in_battle = 1;
    memset(&CEN, 0, sizeof CEN);   /* v0.34.0 (§23.2, D-I): the census counters per battle */
    S.unmatched0 = g_unmatched; S.deep0 = g_deep_over;
    if (g_ctl) { S.nother0 = g_ctl->nother; S.nfill0 = g_ctl->nfill; S.n160 = g_ctl->n16; }
    for (int i = 0; i < VM_NB; i++) g_ring[i].valid = 0;
    g_vf_tried = 0; g_vf = INVALID_HANDLE_VALUE; g_vn = 0;
}
void vm_battle_end(void) {
    if (!g_vm_on || !S.in_battle) return;
    double mhz = tsc_mhz();
    uint64_t unm = g_unmatched - S.unmatched0, deep = g_deep_over - S.deep0;
    char gate[96]; size_t go = 0; gate[0] = 0;
    if (g_vm_trace_on) {   /* finish review fix F5: the per-launch gate of session S-M2d (also: no new TICK near the clash) */
        const char *nm[5] = { "over_passes", "win_over", "missing", "unmatched", "deep_over" };
        uint64_t v[5] = { S.over_passes, S.win_over, S.missing, unm, deep };
        for (int i = 0; i < 5; i++) if (v[i]) { int k = snprintf(gate + go, sizeof gate - go, "%s%s", go ? "," : "FAIL(", nm[i]); if (k > 0 && (size_t)k < sizeof gate - go) go += (size_t)k; }
        if (go) snprintf(gate + go, sizeof gate - go, ")"); else snprintf(gate, sizeof gate, "ok");
    } else snprintf(gate, sizeof gate, "-(no trace)");
    double ps = S.passes ? (double)S.passes : 1.0, cf = S.nconf ? (double)S.nconf : 1.0, mu = mhz > 0 ? mhz : 1.0;
    char ln[1100];
    snprintf(ln, sizeof ln, "b=%llu trace=%d fill=%s fix=%d bytecode=%d passes=%llu dispatches=%llu per_pass_avg=%.1f per_pass_max=%llu cap=%u over_passes=%llu win_over=%llu "
             "handler_cycles_per_pass_avg=%.0f max=%llu hook_us_per_pass_avg=%.1f max=%.1f step_us_per_pass_avg=%.1f max=%.1f confirmed=%llu passes_per_frame=%.2f "
             "hook_us_per_frame_avg=%.1f step_us_per_frame_avg=%.1f tsc_mhz=%.0f clashes=%u diff_f=%u frames=%llu lines=%llu missing=%llu unmatched=%llu deep_over=%llu other=%llu "
             "filled=%llu argc16=%llu bc=%u bc_ok=%u gate=%s",
             (unsigned long long)S.bat, (int)g_vm_trace_on, g_cfg.vm_fix ? "0(fix)" : g_cfg.vm_fill_on ? g_cfg.vm_fill_s : "off", g_cfg.vm_fix, g_bc_on,
             (unsigned long long)S.passes, (unsigned long long)S.disp, (double)S.disp / ps, (unsigned long long)S.disp_max, g_cap,
             (unsigned long long)S.over_passes, (unsigned long long)S.win_over, (double)S.cyc / ps, (unsigned long long)S.cyc_max,
             (double)S.hcyc / ps / mu, (double)S.hcyc_max / mu, (double)S.scyc / ps / mu, (double)S.scyc_max / mu, (unsigned long long)S.nconf, S.nconf ? (double)S.passes / cf : 0.0,
             (double)S.hcyc / cf / mu, (double)S.scyc / cf / mu, mhz, S.nclash, S.diff_f, (unsigned long long)S.frames,
             (unsigned long long)S.recs, (unsigned long long)S.missing, (unsigned long long)unm, (unsigned long long)deep,
             (unsigned long long)(g_ctl ? g_ctl->nother - S.nother0 : 0), (unsigned long long)(g_ctl ? g_ctl->nfill - S.nfill0 : 0),
             (unsigned long long)(g_ctl ? g_ctl->n16 - S.n160 : 0), S.nbc, S.nbc_ok, gate);
    LOG("VM STAT %s\n", ln);
    if (g_vf_tried || S.nbc || S.frames) vf_printf("VMSTAT %s\n", ln);
    if (g_vm_census_on) {   /* v0.34.0 (OPTA-M2-SPEC.md §23.2, D-I): the census summary (patch = whether argc_fix applied in this launch; "differ" = the frames the fix changed) */
        int patch = g_argc_cap && !g_argc_broken && cfg_geti("argc_fix", 1) == 1;
        LOG("VM CENSUS b=%llu n25=%llu n91=%llu class1_4=%llu differ=%llu no_fields=%llu detail=%u cap=%d patch=%s (sub-ops 0x25 / 0x91 stock vs fixed return; the raw tail word is "
            "never logged: class nibble and verdicts only)\n", (unsigned long long)S.bat, (unsigned long long)CEN.n25, (unsigned long long)CEN.n91, (unsigned long long)CEN.class14,
            (unsigned long long)CEN.differ, (unsigned long long)CEN.no_fields, CEN.detail, g_cfg.vm_census_cap, patch ? "on" : "off");
        vf_printf("VMCENSUS b=%llu n25=%llu n91=%llu class1_4=%llu differ=%llu no_fields=%llu detail=%u cap=%d patch=%s\n", (unsigned long long)S.bat, (unsigned long long)CEN.n25,
                  (unsigned long long)CEN.n91, (unsigned long long)CEN.class14, (unsigned long long)CEN.differ, (unsigned long long)CEN.no_fields, CEN.detail, g_cfg.vm_census_cap, patch ? "on" : "off");
    }
    vf_flush();
    if (g_vf != INVALID_HANDLE_VALUE) { CloseHandle(g_vf); g_vf = INVALID_HANDLE_VALUE; }
    S.in_battle = 0;
}

/* ------------------------------------------------------------------ install */
static int hexb(const char *h) { int v = 0; for (int i = 0; i < 2; i++) { char c = h[i]; v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10); } return v; }
static void *alloc_near(uintptr_t anchor) {   /* as src/nobs_install.c: a page within +-2 GiB of the site (rel32) */
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (int dir = 0; dir < 2; dir++)
        for (uintptr_t d = gran; d < 0x60000000ULL; d += gran) {
            uintptr_t a = (dir ? anchor + d : anchor - d) & ~(gran - 1);
            if (a < 0x10000 || a > 0x7ffffffeffffULL) continue;
            void *p = VirtualAlloc((void *)a, gran, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) {
                intptr_t off = (intptr_t)p - (intptr_t)anchor;
                if (off > -0x7ff00000LL && off < 0x7ff00000LL) return p;
                VirtualFree(p, 0, MEM_RELEASE);
            }
        }
    return NULL;
}
static int patch32_all(uint8_t *b, size_t n, uint32_t marker, uint32_t val) {
    int k = 0;
    for (size_t i = 0; i + 4 <= n; i++) { uint32_t v; memcpy(&v, b + i, 4); if (v == marker) { memcpy(b + i, &val, 4); k++; i += 3; } }
    return k;
}
static int store8(uintptr_t site, const uint8_t nb[8]) {   /* one aligned 8-byte store (the site is 8-aligned: V, vm_static) */
    if (site & 7) return 0;
    DWORD old, tmp;
    if (!VirtualProtect((void *)site, 8, PAGE_EXECUTE_READWRITE, &old)) return 0;
    uint64_t v; memcpy(&v, nb, 8);
    InterlockedExchange64((volatile LONG64 *)site, (LONG64)v);
    VirtualProtect((void *)site, 8, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void *)site, 8);
    return 1;
}
/* finish review fix n7: the pre thunk rewrites the native's return slot (0x1402e650d -> 0x1402e6510); under an enforced user shadow stack the
 * native's ret would fault. The exe is not CET_COMPAT (V, REVIEW_FINISH), so this only refuses when a system policy forces one. Resolved at run
 * time (no new import). */
static int shadow_stack_on(DWORD *flags) {
    typedef BOOL (WINAPI *Gpmp)(HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    *flags = 0;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    Gpmp gp = k32 ? (Gpmp)(void *)GetProcAddress(k32, "GetProcessMitigationPolicy") : NULL;
    DWORD v = 0;
    if (gp && gp(GetCurrentProcess(), ProcessUserShadowStackPolicy, &v, sizeof v)) *flags = v;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_vm_cet) *flags |= 1;   /* control VMCET: a planted enforced shadow stack */
#endif
    return (*flags & 1) != 0;
}
void vm_install(void) {
    int mode_ok = g_mode == MODE_RECORD || g_mode == MODE_REPLAY || g_mode == MODE_RBREPLAY;
    int fill = g_cfg.vm_fill_on || g_cfg.vm_fix, trace = g_cfg.vm_trace != 0, census = g_cfg.vm_census != 0;
    int body = trace || census;   /* v0.34.0 (§23.2): the pre-thunk C body runs for the census too (vm_pre_c); the trace's windows / records stay trace-only */
    g_bc_on = g_cfg.vm_bytecode && mode_ok;
    if (!body && !fill) {
        if (g_bc_on) { g_vm_on = 1; g_vb = (char *)VirtualAlloc(NULL, VB_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
        LOG("VM INSTALL none (no vm_trace / vm_fill / vm_fix / vm_census: the dispatch site is not patched) bytecode=%d%s\n", g_bc_on,
            g_cfg.vm_bytecode && !mode_ok ? " (vm_bytecode ignored: record / replay / rbreplay only)" : "");
        return;
    }
    if (!mode_ok) { LOG("VM INSTALL refused mode=%s (the VM hook runs only in record / replay / rbreplay: nothing patched, every VM key off)\n", g_mode_names[g_mode]); g_bc_on = 0; return; }
    /* the signature: the CALLCOND block's 137 bytes (the dispatch and the post-call cmp inside) */
    const char *sig = VM_SIG_CALLCOND; size_t sn = strlen(sig) / 2; const uint8_t *site = (const uint8_t *)VA(VM_A_CALLCOND);
    int bad = -1;
    if (!mem_readable(site, sn)) bad = 0;
    else for (size_t i = 0; i < sn && bad < 0; i++) if (site[i] != hexb(sig + 2 * i)) bad = (int)i;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_vm_sig_bad && bad < 0) bad = 100;   /* control VMSIG: a planted mismatch */
#endif
    if (bad >= 0) {
        LOG("VM INSTALL FAIL why=signature at=0x%llx first_diff=+%d (the CALLCOND block is not the verified one: the trace, the fill and the fix stay OFF for this launch; nothing patched)\n",
            (unsigned long long)VM_A_CALLCOND, bad);
        g_bc_on = 0; return;
    }
    DWORD ssp = 0;
    if (shadow_stack_on(&ssp)) {
        LOG("VM INSTALL FAIL why=shadow_stack policy=0x%lx (a user shadow stack is enforced on this process: the hook's return-slot rewrite would fault; the trace, the fill and the fix "
            "stay OFF for this launch; nothing patched)\n", (unsigned long)ssp);
        g_bc_on = 0; return;
    }
    DWORD tls[4];
    for (int i = 0; i < 4; i++) { tls[i] = TlsAlloc(); if (tls[i] == TLS_OUT_OF_INDEXES || tls[i] >= 64) { LOG("VM INSTALL FAIL why=tls_index idx=%lu (nothing patched)\n", (unsigned long)tls[i]); g_bc_on = 0; return; } }
    const uint8_t *tb = vm_tpl_begin, *tpre = vm_tpl_pre, *tpost = vm_tpl_post, *tctl = vm_tpl_ctl, *te = vm_tpl_end;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_vm_pushres) { tb = vm_tpl_begin_mut; tpre = vm_tpl_pre_mut; tpost = vm_tpl_post_mut; tctl = vm_tpl_ctl_mut; te = vm_tpl_end_mut; }
#endif
    size_t tn = (size_t)(te - tb);
    g_page = (uint8_t *)alloc_near((uintptr_t)VA(VM_A_DISPATCH));
    uint8_t *pstk = (uint8_t *)VirtualAlloc(NULL, 1u << 19, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_vb = (char *)VirtualAlloc(NULL, VB_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_page || !pstk || !g_vb || tn > 0x10000) { LOG("VM INSTALL FAIL why=alloc page=%d pstk=%d buf=%d (nothing patched)\n", g_page != NULL, pstk != NULL, g_vb != NULL); g_bc_on = 0; return; }
    if (trace) {
        g_cap = (uint32_t)g_cfg.vm_trace_cap;
        size_t per = (size_t)g_cap * sizeof(VmRec);
        uint8_t *rb = (uint8_t *)VirtualAlloc(NULL, per * (VM_NB + 1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!rb) { LOG("VM INSTALL FAIL why=alloc_records cap=%u (nothing patched)\n", g_cap); g_bc_on = 0; return; }
        g_scr.r = (VmRec *)rb; for (int i = 0; i < VM_NB; i++) g_ring[i].r = (VmRec *)(rb + per * (size_t)(i + 1));
    }
    memcpy(g_page, tb, tn);
    int k1 = patch32_all(g_page, tn, 0x7EAD0101u, 0x1480u + 8u * tls[0]), k2 = patch32_all(g_page, tn, 0x7EAD0102u, 0x1480u + 8u * tls[1]);
    int k3 = patch32_all(g_page, tn, 0x7EAD0103u, 0x1480u + 8u * tls[2]), k4 = patch32_all(g_page, tn, 0x7EAD0104u, 0x1480u + 8u * tls[3]);
    if (!k1 || !k2 || !k3 || !k4) { LOG("VM INSTALL FAIL why=markers %d/%d/%d/%d (nothing patched)\n", k1, k2, k3, k4); g_bc_on = 0; return; }
    g_ctl = (VmCtl *)(g_page + (tctl - tb));
    memset(g_ctl, 0, sizeof *g_ctl);
    g_ctl->ret = (uint64_t)VA(VM_A_POSTCALL); g_ctl->cmpaddr = (uint64_t)VA(VM_A_CMP_TARGET); g_ctl->next = (uint64_t)VA(VM_A_POSTNEXT);
    g_ctl->pstk = ((uint64_t)(uintptr_t)pstk + (1u << 19) - 64) & ~15ULL;
    g_ctl->prefn = (uint64_t)(uintptr_t)vm_pre_c; g_ctl->postfn = (uint64_t)(uintptr_t)vm_post_c;
    uint32_t fv = 0; char fdesc[48];
    if (g_cfg.vm_fix) { fv = 0; snprintf(fdesc, sizeof fdesc, "0x0000(fix)"); }
    else if (g_cfg.vm_fill_on && g_cfg.vm_fill_rand) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        fv = ((uint32_t)q.QuadPart ^ ((uint32_t)q.QuadPart >> 16) ^ (GetCurrentProcessId() * 2654435761u)) & 0xffffu; if (!fv) fv = 0x5a5a;
        snprintf(fdesc, sizeof fdesc, "0x%04x(rand)", fv);
    } else if (g_cfg.vm_fill_on) { fv = g_cfg.vm_fill_val & 0xffffu; snprintf(fdesc, sizeof fdesc, "0x%04x", fv); }
    else snprintf(fdesc, sizeof fdesc, "off");
    g_ctl->fillv = (uint16_t)fv;
    g_ctl->flags = (fill ? 1u : 0u) | (body ? 2u : 0u);   /* v0.34.0 (§23.2): bit 1 = the C body (trace OR census) */
    FlushInstructionCache(GetCurrentProcess(), g_page, tn);
    /* the patches: the post site first (trace only; every native return then passes the post thunk, which passes anything it did not record
     * straight to the stock cmp), then the dispatch */
    uintptr_t ds = (uintptr_t)VA(VM_A_DISPATCH), ps = (uintptr_t)VA(VM_A_POSTCALL);
    uintptr_t pre_at = (uintptr_t)(g_page + (tpre - tb)), post_at = (uintptr_t)(g_page + (tpost - tb));
    int ok_post = 1;
    if (trace) {
        int32_t r2 = (int32_t)((intptr_t)post_at - (intptr_t)(ps + 5));
        uint8_t nb[8] = { 0xe9, 0, 0, 0, 0, 0x66, 0x90, *(const uint8_t *)(ps + 7) }; memcpy(nb + 1, &r2, 4);
        ok_post = store8(ps, nb);
    }
    int32_t r1 = (int32_t)((intptr_t)pre_at - (intptr_t)(ds + 5));
    uint8_t na[8] = { 0xe8, 0, 0, 0, 0, 0x0f, 0x1f, 0x00 }; memcpy(na + 1, &r1, 4);
    int ok_pre = ok_post && store8(ds, na);
    if (!ok_pre || !ok_post) { LOG("VM INSTALL FAIL why=patch pre=%d post=%d\n", ok_pre, ok_post); g_ctl->flags = 0; g_bc_on = 0; return; }
    g_tsc0 = __rdtsc(); QueryPerformanceCounter(&g_qpc0);
    g_vm_on = 1; g_vm_trace_on = trace; g_vm_census_on = census; g_vm_body_on = body;   /* v0.34.0 (§23.2) */
    LOG("VM INSTALL ok trace=%d fill=%s census=%d bytecode=%d mode=%s site=0x%llx post=%s page_off=%+lld tls=%lu,%lu,%lu,%lu cap=%u window=-%d..+%d clashes=%d diff_pre=%d shadow_stack=0x%lx%s "
        "(v0.32.0 review fix B1: off by default, record / replay / rbreplay only, never in rbonline)\n", trace, fdesc, census, g_bc_on, g_mode_names[g_mode], (unsigned long long)VM_A_DISPATCH,
        trace ? "patched" : "stock", (long long)((intptr_t)g_page - (intptr_t)ds), (unsigned long)tls[0], (unsigned long)tls[1], (unsigned long)tls[2], (unsigned long)tls[3],
        g_cap, g_cfg.vm_trace_pre, g_cfg.vm_trace_frames, g_cfg.vm_trace_clashes, g_cfg.vm_trace_diff_pre, (unsigned long)ssp,
#ifdef SC6RB_HARNESS
        g_cfg.h_vm_pushres ? " HARNESS_CONTROL=h_vm_pushres(a push/pop on the game stack)" : g_cfg.h_vm_every_pass ? " HARNESS_CONTROL=h_vm_every_pass" :
        g_cfg.h_vm_nodiff ? " HARNESS_CONTROL=h_vm_nodiff" : g_cfg.h_vm_bc_once ? " HARNESS_CONTROL=h_vm_bc_once" : ""
#else
        ""
#endif
        );
}

#ifdef SC6RB_HARNESS
const VmCtlView *vm_h_ctl(void) { return (const VmCtlView *)g_ctl; }
const VmRec *vm_h_scratch(uint32_t *n) { *n = g_scr.n; return g_scr.r; }
uint64_t vm_h_unmatched(void) { return g_unmatched; }
void vm_h_cycles(uint64_t *body, uint64_t *hook, uint64_t *passes, uint64_t *disp) { *body = S.cyc; *hook = S.hcyc; *passes = S.passes; *disp = S.disp; }
void vm_h_census(uint64_t *n25, uint64_t *n91, uint64_t *class14, uint64_t *differ, uint32_t *detail) {   /* v0.34.0 (§23.2, D-I) */
    *n25 = CEN.n25; *n91 = CEN.n91; *class14 = CEN.class14; *differ = CEN.differ; *detail = CEN.detail;
}
#endif
