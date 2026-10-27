/* Option A M2: the rbonline patch set, installed once at process start (sc6rb/M2-INTERFACES.md §3.2, §6, §9). Part of the rollback core.
 * The ONLY rbonline unit that patches the game at process start. Order: the config gate, the M1 runtime (rings, private stacks, XSAVE
 * areas, the record-only VEH, M1's writer), the rbo runtime (event ring + writer), the armed path's allocations (rbo_core_init: the arena
 * reservation and the C1b tables; no game write), the side channel's idle I/O thread (p2p_init); then the signatures (every one checked
 * before anything is written), the wiring (rbo_hooktab: orig, M1's private stack, M1's XSAVE area, tgt = orig), and the enabling:
 *   P1-P4, P6-P10: the 9 vtable slots -> src/rbo_thunks.S (M1's R-STACK thunk + a target cell),
 *   P5: the enqueue call at 0x1403eed2f -> a +-2 GiB trampoline -> nobs_enq_hook (M1's body, unchanged),
 *   F:  the FetchInput call at 0x1403fe6e1 -> a +-2 GiB trampoline `jmp qword ptr [rip+0]; dq cell` with cell = FetchInput.
 * Nothing else is ever patched at process start (§9 rows M1-M13: the allocator hooks only at ARM; L1/L3 probes, MinHook, the IFC Setup
 * hook, ifc_census thunks, the lever, rand, ns_start, seed/setup injection, lip-sync, the generic VEH: never in rbonline). */
#include "overlay.h"   /* v0.31.0 (§20.1) */
#include "argcfix.h"   /* v0.34.0 (OPTA-M2-SPEC.md §23.1) */
#include "rbo_int.inc"
#include "rb.h"   /* v0.29.2: modes_q1_sig / modes_q1_rows */

volatile uint64_t *rbo_fetch_cell;   /* the trampoline's 8-byte target (NULL until installed) */
void *rbo_o_step;                    /* the IFC Step slot's original (the step thunk 0x1403d2a20) */
static void *g_fetch_tramp __attribute__((unused));   /* the FetchInput call path's entry (the jmp at +2; RBO-CTX probes it) */

/* ---------------------------------------------------------------- patch helpers (as nobs_install.c) */
static int rp_write(void *dst, const void *src, size_t n) {
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(dst, src, n);
    DWORD tmp; VirtualProtect(dst, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 1;
}
static int rp_patch_ptr(void *slot, void *val) { return rp_write(slot, &val, sizeof val); }
/* a page within +-2 GiB of anchor (M1's fixed bound: the x64 user-address limit 0x7ffffffeffff; NOBS-NEAR) */
static void *alloc_near(uintptr_t anchor) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (int dir = 0; dir < 2; dir++)
        for (uintptr_t d = gran; d < 0x60000000ULL; d += gran) {
            uintptr_t a = dir ? anchor + d : anchor - d;
            a &= ~(gran - 1);
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
static int hexbyte2(const char *h) { int v = 0; sscanf(h, "%2x", &v); return v; }
static int bytes_ok(uint64_t va, const char *hex) {
    const uint8_t *p = (const uint8_t *)VA(va);
    size_t n = strlen(hex) / 2;
    if (!mem_readable(p, n)) return 0;
    for (size_t i = 0; i < n; i++) if (p[i] != hexbyte2(hex + i * 2)) return 0;
    return 1;
}
static void note_patch(const char *name, uint64_t va, const char *kind, int sig_ok, int status) {
    rbo_log("RB PATCH name=%s va=game+0x%llx kind=%s sig=%s status=%d", name, (unsigned long long)(va - IMG_PREF), kind, sig_ok ? "ok" : "BAD", status);
}

/* the M1 patch set (nobs_install.c's table: the same slots, originals and prologues) */
typedef struct { int hook; const char *name; uint64_t slot, orig; const char *prologue; int core; } RPatch;
static const RPatch k_patch[] = {
    { NOBS_H_TICK,         "tick",         NOBS_A_TICK_SLOT,                    NOBS_A_TICK,       "40534883ec30488bd9",       1 },
    { NOBS_H_IFC_INIT,     "ifc_init",     NOBS_A_IFC_VT + NOBS_IFC_INIT,       NOBS_A_INIT_THUNK, "488bcae9586bf1ff",         1 },
    { NOBS_H_IFC_SHUTDOWN, "ifc_shutdown", NOBS_A_IFC_VT + NOBS_IFC_SHUTDOWN,   NOBS_A_SHUT_THUNK, "e93bd6f1ff",               1 },
    { NOBS_H_IFC_STEP,     "ifc_step",     NOBS_A_IFC_VT + NOBS_IFC_STEP,       NOBS_A_STEP_THUNK, "488bcae93892f0ff",         1 },
    { NOBS_H_SENDCTL,      "sendctl",      NOBS_A_SYNC_VT + NOBS_SYNC_SENDCTL,  NOBS_A_SENDCTL,    "488bc453574881ec88000000", 0 },
    { NOBS_H_SSM,          "ssm",          NOBS_A_SYNC_VT + NOBS_SYNC_SSM,      NOBS_A_SSM,        "40574883ec300fb6812444",   0 },
    { NOBS_H_ISPAUSED,     "ispaused",     NOBS_A_SYNC_VT + NOBS_SYNC_ISPAUSED, NOBS_A_ISPAUSED,   "40534883ec3080b904440000", 1 },
    { NOBS_H_SETSTATE,     "setstate",     NOBS_A_SYNC_VT + NOBS_SYNC_SETSTATE, NOBS_A_SETSTATE,   "48895c2408574883ec30",     1 },
    { NOBS_H_RRESET,       "rreset",       NOBS_A_SYNC_VT + NOBS_SYNC_RRESET,   NOBS_A_RRESET,     "40534883ec20488bd9",       1 },
};
#define NPATCH ((int)(sizeof k_patch / sizeof k_patch[0]))
static void *thunk_of(int hook) {
    switch (hook) {
    case NOBS_H_TICK: return (void *)rbo_th_tick;
    case NOBS_H_IFC_INIT: return (void *)rbo_th_ifc_init;
    case NOBS_H_IFC_SHUTDOWN: return (void *)rbo_th_ifc_shutdown;
    case NOBS_H_IFC_STEP: return (void *)rbo_th_ifc_step;
    case NOBS_H_SENDCTL: return (void *)rbo_th_sendctl;
    case NOBS_H_SSM: return (void *)rbo_th_ssm;
    case NOBS_H_ISPAUSED: return (void *)rbo_th_ispaused;
    case NOBS_H_SETSTATE: return (void *)rbo_th_setstate;
    case NOBS_H_RRESET: return (void *)rbo_th_rreset;
    default: return NULL;
    }
}
#ifdef SC6RB_HARNESS
void *rbo_h_thunk_of(int hook) { return thunk_of(hook); }   /* RBO-CTX */
void *rbo_h_fetch_tramp(void) { return g_fetch_tramp; }     /* RBO-CTX: the FetchInput call path's entry (the jmp at +2) */
#endif

/* ---------------------------------------------------------------- P5: the enqueue call -> nobs_enq_hook (M1's retarget, rbonline's copy) */
static int retarget_enq(int *sig_ok) {
    const uint8_t *cs = (const uint8_t *)VA(NOBS_A_ENQ_CALL);
    *sig_ok = 0;
    if (!mem_readable(cs, 5) || cs[0] != 0xe8) return 0;
    int32_t cur = (int32_t)(cs[1] | cs[2] << 8 | cs[3] << 16 | (uint32_t)cs[4] << 24);
    if ((uint64_t)((intptr_t)cs + 5 + cur) != VA(NOBS_A_ENQ) || !bytes_ok(NOBS_A_ENQ, "48895c241048896c2418")) return 0;
    *sig_ok = 1;
    nobs_o_enq = (NobsEnqFn)VA(NOBS_A_ENQ);   /* the original the hook forwards to (the only cast of a game address to a function here) */
    void *target = (void *)nobs_enq_hook;
    intptr_t direct = (intptr_t)target - ((intptr_t)cs + 5);
    if (direct <= -0x7ff00000LL || direct >= 0x7ff00000LL) {
        void *tr = alloc_near((uintptr_t)cs);
        if (!tr) return 0;
        uint8_t jmp[12] = { 0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xe0 };   /* mov rax, imm64 ; jmp rax (M1's P5 trampoline: rax is volatile at a call) */
        memcpy(jmp + 2, &target, 8);
        memcpy(tr, jmp, sizeof jmp);
        FlushInstructionCache(GetCurrentProcess(), tr, sizeof jmp);
        target = tr;
    }
    int32_t rel = (int32_t)((intptr_t)target - ((intptr_t)cs + 5));
    uint8_t ins[5] = { 0xe8, (uint8_t)rel, (uint8_t)(rel >> 8), (uint8_t)(rel >> 16), (uint8_t)(rel >> 24) };
    return rp_write((void *)cs, ins, 5);
}

/* ---------------------------------------------------------------- F: the FetchInput call (T-M2-8: its only reference) -> the front door.
 * The trampoline: at an 8-aligned base T, bytes T+0..1 = cc cc (never executed), T+2..7 = ff 25 00 00 00 00 (jmp qword ptr [rip+0]: the
 * next 8 bytes), T+8..15 = the cell (8-aligned: one atomic store retargets it). The call lands at T+2 with the return address
 * 0x1403fe6e6 on the stack exactly as the original call left it; the jmp touches no register, no flag and no stack byte. */
static int fetch_sig(void) {
    const uint8_t *cs = (const uint8_t *)VA(RBO_A_FETCH_CALL);
    if (!bytes_ok(RBO_A_FETCH_CALL, RBO_FETCH_CALL_SIG)) return 0;
    int32_t cur = (int32_t)(cs[1] | cs[2] << 8 | cs[3] << 16 | (uint32_t)cs[4] << 24);
    /* v0.29.1 (review m4): + the three functions the armed FetchInput calls as stock does (prologues VERIFIED on the installed exe) */
    return (uint64_t)((intptr_t)cs + 5 + cur) == VA(RBO_A_FETCH) && bytes_ok(RBO_A_FETCH, RBO_FETCH_SIG) &&
           bytes_ok(RBO_A_SESS_ACTIVE, RBO_SESS_ACTIVE_SIG) && bytes_ok(RBO_A_SETUP_OF, RBO_SETUP_OF_SIG) && bytes_ok(RBO_A_FETCH_PRED, RBO_FETCH_PRED_SIG);
}
static int retarget_fetch(void) {
    uint8_t *cs = (uint8_t *)VA(RBO_A_FETCH_CALL);
    uint8_t *t = (uint8_t *)alloc_near((uintptr_t)cs);
    if (!t) return 0;
    uint64_t cell = VA(RBO_A_FETCH);
    uint8_t tr[16] = { 0xcc, 0xcc, 0xff, 0x25, 0, 0, 0, 0 };
    memcpy(tr + 8, &cell, 8);
    memcpy(t, tr, sizeof tr);
    FlushInstructionCache(GetCurrentProcess(), t, sizeof tr);
    g_fetch_tramp = t + 2;
    rbo_fetch_cell = (volatile uint64_t *)(t + 8);
    int32_t rel = (int32_t)((intptr_t)(t + 2) - ((intptr_t)cs + 5));
    uint8_t ins[5] = { 0xe8, (uint8_t)rel, (uint8_t)(rel >> 8), (uint8_t)(rel >> 16), (uint8_t)(rel >> 24) };
    return rp_write(cs, ins, 5);
}

/* ---------------------------------------------------------------- the config gate (M2-INTERFACES.md §6) */
static int g_refused;
static void refuse_i(const char *k, long long v, const char *need) { LOG("RB CFG REFUSE key=%s value=%lld need=%s\n", k, v, need); g_refused = 1; }
static void cfg_gate(void) {
    const Config *c = &g_cfg;
    if (c->rbonline_d < 2 || c->rbonline_d > 15) refuse_i("rbonline_d", c->rbonline_d, "2..15");
    /* the spec range is 1..15; the C1b core is proven (and sized: PEND_N, the ring) for 1..7 (rb.c net_max_rollback) */
    if (c->rbonline_max_rb < 1 || c->rbonline_max_rb > 7) refuse_i("rbonline_max_rb", c->rbonline_max_rb, "1..7(the_C1b_core's_range)");
    if (c->rbonline_ranked != 0 && c->rbonline_ranked != 1) refuse_i("rbonline_ranked", c->rbonline_ranked, "0|1");
    if (c->rbonline_check_every < 10 || c->rbonline_check_every > 600) refuse_i("rbonline_check_every", c->rbonline_check_every, "10..600");
    if (c->rbonline_arm_round < 0 || c->rbonline_arm_round > 15) refuse_i("rbonline_arm_round", c->rbonline_arm_round, "0..15");
    if (c->rbonline_channel <= 65535u || c->rbonline_channel == 0x484Fu || c->rbonline_channel > 0x7fffffffu)
        refuse_i("rbonline_channel", (long long)c->rbonline_channel, ">65535,!=0x484F,<=0x7fffffff");
    if (c->rbonline_probe_max < 1 || c->rbonline_probe_max > 100) refuse_i("rbonline_probe_max", c->rbonline_probe_max, "1..100");
    if (c->rbonline_probe_gap_ms < 1 || c->rbonline_probe_gap_ms > 60000) refuse_i("rbonline_probe_gap_ms", c->rbonline_probe_gap_ms, "1..60000");
    if (c->rbonline_resend_ms < 1 || c->rbonline_resend_ms > 10000) refuse_i("rbonline_resend_ms", c->rbonline_resend_ms, "1..10000");
    if (c->rbonline_peer_timeout_ms < 100 || c->rbonline_peer_timeout_ms > 120000) refuse_i("rbonline_peer_timeout_ms", c->rbonline_peer_timeout_ms, "100..120000");
    if (c->rbonline_poll_ms < 1 || c->rbonline_poll_ms > 100) refuse_i("rbonline_poll_ms", c->rbonline_poll_ms, "1..100");
    if (c->rbonline_arm_margin_ms < 0 || c->rbonline_arm_margin_ms > 60000) refuse_i("rbonline_arm_margin_ms", c->rbonline_arm_margin_ms, "0..60000");
    if (c->rbonline_l3_zero_kb < 64 || c->rbonline_l3_zero_kb > 1024) refuse_i("rbonline_l3_zero_kb", c->rbonline_l3_zero_kb, "64..1024");
    if (c->rbonline_dump != 0 && c->rbonline_dump != 1) refuse_i("rbonline_dump", c->rbonline_dump, "0|1");
    if (c->rbonline_unranked_ack != 0 && c->rbonline_unranked_ack != 1) refuse_i("rbonline_unranked_ack", c->rbonline_unranked_ack, "0|1");   /* v0.29.1 (M3) */
    /* v0.30.0 (OPTA-M2-SPEC.md §19.2): the zone lag's lookahead must leave a KO zone's entry (z = k + rb_zone_entry_gap 21) known before the switch
     * frame: the live frame is at most max_rb - 1 past the confirmed k, so lead <= 21 - max_rb - 1 */
    {   int zl_max = 21 - c->rbonline_max_rb - 1;
        if (c->rbonline_zone_lag_lead < 0 || c->rbonline_zone_lag_lead > zl_max) { char nd[32]; snprintf(nd, sizeof nd, "0..%d", zl_max); refuse_i("rbonline_zone_lag_lead", c->rbonline_zone_lag_lead, nd); } }
    if (c->rbonline_tsync != 0 && c->rbonline_tsync != 1) refuse_i("rbonline_tsync", c->rbonline_tsync, "0|1");
    {   int dad = cfg_geti("rbonline_d_adapt", 1);   /* v0.33.0 (OPTA-M2-SPEC.md §22.2): compared in HELLO (both PCs must use the same rule) */
        if (dad != 0 && dad != 1) refuse_i("rbonline_d_adapt", dad, "0|1"); }
    {   int ak = cfg_geti("argc_fix", 1);   /* v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix (advertised in HELLO, compared) */
        if (ak != 0 && ak != 1) refuse_i("argc_fix", ak, "0|1"); }
    /* v0.31.0 (§20.1, §20.2): presentation keys are clamped and said so, never refused (a bad badge position must not turn rollback off) */
    {   Config *w = &g_cfg;
        int b0 = w->rbonline_badge, x0 = w->rbonline_badge_x, y0 = w->rbonline_badge_y, s0 = w->rbonline_badge_scale, k0 = w->rbonline_badge_key, n0 = w->rbonline_sound;
        w->rbonline_badge = w->rbonline_badge != 0; w->rbonline_sound = w->rbonline_sound != 0;
        if (w->rbonline_badge_x < 0) w->rbonline_badge_x = 0;
        if (w->rbonline_badge_x > 100) w->rbonline_badge_x = 100;
        if (w->rbonline_badge_y < 0) w->rbonline_badge_y = 0;
        if (w->rbonline_badge_y > 100) w->rbonline_badge_y = 100;
        if (w->rbonline_badge_scale < 0) w->rbonline_badge_scale = 0;
        if (w->rbonline_badge_scale > 16) w->rbonline_badge_scale = 16;
        if (w->rbonline_badge_key < 0 || w->rbonline_badge_key > 255) w->rbonline_badge_key = 0;
        if (b0 != w->rbonline_badge || x0 != w->rbonline_badge_x || y0 != w->rbonline_badge_y || s0 != w->rbonline_badge_scale || k0 != w->rbonline_badge_key || n0 != w->rbonline_sound)
            LOG("CFG rbonline badge CLAMPED badge=%d x=%d y=%d scale=%d key=0x%x sound=%d (ini badge=%d x=%d y=%d scale=%d key=%d sound=%d)\n", w->rbonline_badge, w->rbonline_badge_x,
                w->rbonline_badge_y, w->rbonline_badge_scale, w->rbonline_badge_key, w->rbonline_sound, b0, x0, y0, s0, k0, n0); }
}

/* ---------------------------------------------------------------- install */
static void install_fail(const char *gate) {
    rbo_set_off(gate);
    nobs_gate_fail(gate, 1);
    rbo_badge_tick();   /* v0.31.0: the badge says ROLLBACK PLUGIN OFF: <gate> at once (M1's writer may not run) */
}
void rbo_install(void) {
    LOG("RB INSTALL begin version=%s pid=%lu\n", SC6RB_VERSION, GetCurrentProcessId());
    cfg_gate();
    rbo_d = g_cfg.rbonline_d; rbo_max_rb = g_cfg.rbonline_max_rb;
    {   char forced[512]; rbo_core_force(forced, sizeof forced);   /* the C1b knobs rbonline runs with (one fixed set; the ini cannot change them) */
        LOG("RB CFG d=%d max_rb=%d channel=0x%08x ranked=%d check_every=%d arm_round=%d probe_max=%d probe_gap_ms=%d resend_ms=%d peer_timeout_ms=%d l3_zero_kb=%d "
            "arm_margin_ms=%d poll_ms=%d dump=%d unranked_ack=%d forced=%s\n", g_cfg.rbonline_d, g_cfg.rbonline_max_rb, g_cfg.rbonline_channel, g_cfg.rbonline_ranked, g_cfg.rbonline_check_every,
            g_cfg.rbonline_arm_round, g_cfg.rbonline_probe_max, g_cfg.rbonline_probe_gap_ms, g_cfg.rbonline_resend_ms, g_cfg.rbonline_peer_timeout_ms,
            g_cfg.rbonline_l3_zero_kb, g_cfg.rbonline_arm_margin_ms, g_cfg.rbonline_poll_ms, g_cfg.rbonline_dump, g_cfg.rbonline_unranked_ack, forced); }
    LOG("RB CFG v0.30.0 zone_lag_lead=%d tsync=%d (the zones run at the game's own InputLag, d=%d on fight frames; the frame-advantage time sync %s)\n",
        g_cfg.rbonline_zone_lag_lead, g_cfg.rbonline_tsync, g_cfg.rbonline_d, g_cfg.rbonline_tsync ? "waits on this PC" : "reports only");
    LOG("RB CFG v0.33.0 d_adapt=%d (the delay of an armed battle: %s; RBM_PROTO %d)\n", cfg_geti("rbonline_d_adapt", 1),
        cfg_geti("rbonline_d_adapt", 1) ? "max(rbonline_d, clamp(ceil(maxRTT * 0.03) + 1 - max_rb, 2, 4)) from the two HELLOs' RTTs, agreed at ARM" : "rbonline_d (v0.32.0's fixed delay)", RBM_PROTO);
    if (!g_cfg.rbonline_ranked && !g_cfg.rbonline_unranked_ack)
        LOG("RB CFG ranked gate: no battle probes (rbonline_unranked_ack=0 and rbonline_ranked=0; the plugin cannot see whether a match is Ranked)\n");
    if (g_refused) { InterlockedExchange(&rbo_probe_off, 1); LOG("RB CFG REFUSED: PROBE is off for this process (every battle stays M1-passive)\n"); }
    argc_rbo_install();   /* v0.34.0 (OPTA-M2-SPEC.md §23.1): the four EvaluateIf sites checked (read only); nothing is written at install */
    {   /* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.6): the Q1 root table's signature gate (reads the image only). A mismatch turns PROBE off for the
         * whole process (every battle stays M1-passive with the stock netcode: RB STATE ... why=q1sig, title rb=OFF:q1sig). The ini's hybrid keys
         * are rbreplay's: rbonline's walk and late-pin rule are by mode (roots=1 init_pins=0, every pass L3-stock). */
        int sig = modes_q1_sig();
        LOG("CFG Q1 mode=rbonline roots=1 init_pins=0 l3stock=1 arm_f=0 cov=0 table=%d sig=%s%s\n", modes_q1_rows(), sig ? "ok" : "BAD",
            g_cfg.q1_present ? " (the ini's rb_l3stock / rb_arm_f / rb_init_pins / q1_roots / q1_cov keys are rbreplay's: ignored here)" : "");
        if (!sig) { InterlockedExchange(&rbo_q1sig, 1); InterlockedExchange(&rbo_probe_off, 1); LOG("RB CFG Q1 signature mismatch: PROBE is off for this process (every battle stays M1-passive, why=q1sig)\n"); }
    }
#ifdef SC6RB_HARNESS
    if (g_cfg.h_rbo_ctx_xmm9_zero) { rbo_l3s_ctx.xmm_lo[9] = 0; LOG("RB CFG harness plant h_rbo_ctx_xmm9_zero=1: this peer's L3-stock xmm9 = 0 (XCTX)\n"); }
#endif
    nobs_title_ext_fn = rbo_title_token;   /* before M1's writer starts (it pumps the title) */
    /* v0.31.0 (OPTA-M2-SPEC.md §20.1, §20.2): the badge text and the sound cue are M1's writer thread's (~20 Hz); the first text now, the hook from a
     * thread of its own (never in DllMain: the throw-away D3D11 device it needs is made there) */
    nobs_tick_ext_fn = rbo_badge_tick;
    rbo_badge_tick();
#ifndef SC6RB_HARNESS
    {   OverlayCfg oc; oc.on = g_cfg.rbonline_badge; oc.x_pct = g_cfg.rbonline_badge_x; oc.y_pct = g_cfg.rbonline_badge_y; oc.scale = g_cfg.rbonline_badge_scale;
        oc.key = g_cfg.rbonline_badge_key;
        overlay_start(&oc); }
#endif
    if (!nobs_runtime_init()) { install_fail("runtime_init"); LOG("RB INSTALL version=%s patchset_ok=0 fetch=BAD p2p=BAD (nobs_runtime_init failed: nothing patched)\n", SC6RB_VERSION); log_flush(); return; }
    if (!rbo_runtime_init()) { install_fail("rbo_runtime"); LOG("RB INSTALL version=%s patchset_ok=0 fetch=BAD p2p=BAD (the rbo runtime failed: nothing patched)\n", SC6RB_VERSION); log_flush(); return; }
    int core_ok = rbo_core_init();
    if (!core_ok) { InterlockedExchange(&rbo_probe_off, 1); rbo_unexpected("core_init", "the_armed_path_cannot_run:_PROBE_off"); }

    /* the side channel (idle I/O thread; no Steam call until a PROBE) */
    P2pCfg pc; memset(&pc, 0, sizeof pc);
    pc.channel = (int)g_cfg.rbonline_channel; pc.probe_max = g_cfg.rbonline_probe_max; pc.probe_gap_ms = g_cfg.rbonline_probe_gap_ms;
    pc.resend_ms = g_cfg.rbonline_resend_ms; pc.peer_timeout_ms = g_cfg.rbonline_peer_timeout_ms; pc.poll_ms = g_cfg.rbonline_poll_ms;
#ifdef SC6RB_HARNESS
    pc.h_accept_any_id = g_cfg.h_p2p_accept_any_id; pc.h_no_guard = g_cfg.h_p2p_no_guard; pc.h_no_resend = g_cfg.h_p2p_no_resend; pc.h_corrupt_every = g_cfg.h_p2p_corrupt_every;
    pc.h_no_self_check = cfg_geti("h_p2p_no_self_check", 0);   /* v0.29.1: XSELF */
    pc.h_no_sess_check = cfg_geti("h_p2p_no_sess_check", 0);   /* v0.29.7: XPEERNOSESS */
    g_p2p_api_name = "fake";    /* a fallback only: RB P2P api= now comes from p2p_api_name() at the event (TR-9). Tests never load steam_api64.dll (p2p.h exposed no
                                 * name; see the requests in M2-INTERFACES.md) */
#else
    g_p2p_api_name = "steam";
#endif
    int p2p_ok = !g_refused && p2p_init(&pc);
    if (!p2p_ok) InterlockedExchange(&rbo_probe_off, 1);

    /* phase 1: every signature */
    int sig[NPATCH], core_bad = 0, any_bad = 0;
    for (int i = 0; i < NPATCH; i++) {
        uint64_t slotv = 0; int slot_ok = mem_readable((const void *)VA(k_patch[i].slot), 8);
        if (slot_ok) slotv = *(uint64_t *)VA(k_patch[i].slot);
        sig[i] = slot_ok && slotv == VA(k_patch[i].orig) && bytes_ok(k_patch[i].orig, k_patch[i].prologue);
        if (!sig[i]) { any_bad = 1; if (k_patch[i].core) core_bad = 1; }
    }
    int fetch_ok = fetch_sig();
    if (core_bad) {
        for (int i = 0; i < NPATCH; i++) note_patch(k_patch[i].name, k_patch[i].orig, "vtbl", sig[i], 0);
        install_fail("signature");
        rbo_log("RB PATCHSET n=0 names=- ok=0");
        rbo_log("RB INSTALL version=%s patchset_ok=0 fetch=%s p2p=%s (a core signature failed: nothing patched; the game runs untouched)", SC6RB_VERSION, fetch_ok ? "ok" : "BAD", p2p_ok ? "ok" : "BAD");
        log_flush();
        return;
    }
    /* phase 2: wiring (orig, M1's private stack, M1's XSAVE area, tgt = orig) */
    for (int i = 0; i < NPATCH; i++) {
        if (!sig[i]) continue;
        int h = k_patch[i].hook;
        rbo_hooktab[h * 4 + 0] = VA(k_patch[i].orig);
        rbo_hooktab[h * 4 + 1] = (uint64_t)(uintptr_t)nobs_pstack_top(h);
        rbo_hooktab[h * 4 + 2] = (uint64_t)(uintptr_t)nobs_xarea[h];
        rbo_hooktab[h * 4 + 3] = VA(k_patch[i].orig);
    }
    rbo_o_step = (void *)VA(NOBS_A_STEP_THUNK);
    /* phase 3: enable */
    int n = 0; char names[256] = ""; size_t no = 0;
    for (int i = 0; i < NPATCH; i++) {
        int st = sig[i] && rp_patch_ptr((void *)VA(k_patch[i].slot), thunk_of(k_patch[i].hook));
        note_patch(k_patch[i].name, k_patch[i].orig, "vtbl", sig[i], st);
        if (st) { n++; no += (size_t)snprintf(names + no, sizeof names - no, "%s%s", no ? "," : "", k_patch[i].name); }
        else { any_bad = 1; nobs_gate_fail(k_patch[i].name, 0); }
    }
    int enq_sig = 0, enq_ok = retarget_enq(&enq_sig);
    note_patch("enq", NOBS_A_ENQ, "rel32", enq_sig, enq_ok);
    if (enq_ok) { n++; no += (size_t)snprintf(names + no, sizeof names - no, "%s%s", no ? "," : "", "enq"); } else { any_bad = 1; nobs_gate_fail("enq", 0); }
    int fetch_st = fetch_ok && retarget_fetch();
    note_patch("fetch", RBO_A_FETCH, "rel32", fetch_ok, fetch_st);
    if (fetch_st) { n++; no += (size_t)snprintf(names + no, sizeof names - no, "%s%s", no ? "," : "", "fetch"); }
    else { any_bad = 1; InterlockedExchange(&rbo_probe_off, 1); rbo_unexpected("fetch", "FetchInput_front_door_not_installed:_PROBE_off"); }
    int ok = !any_bad && n == NPATCH + 2;
    rbo_log("RB PATCHSET n=%d names=%s ok=%d", n, names[0] ? names : "-", ok);
    rbo_log("RB INSTALL version=%s patchset_ok=%d fetch=%s p2p=%s probe=%s core=%s", SC6RB_VERSION, ok, fetch_st ? "ok" : "BAD", p2p_ok ? "ok" : "BAD",
            rbo_probe_off ? "off" : "on", core_ok ? "ok" : "BAD");
    {   char tok[96]; rbo_title_token(tok, sizeof tok, "?"); rbo_log("RB TITLE %s", tok); }
    log_flush();
}
