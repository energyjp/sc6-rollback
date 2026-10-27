/* Option A M2: the bridge from rbonline to the C1b rollback core (src/rb.c, snap.c, arena.c, l4.c, pres.c, mem.c, modes.c run_step)
 * (OPTA-M2-SPEC.md §5.4, §5.8, §6; sc6rb/M2-INTERFACES.md §3.6, §9). Part of the rollback core.
 *
 * rbo_step_armed is where the armed IFC step lands (rbo_thunks.S tgt, ARMED and DRAINED): the step of armed frame f (f = 1 at round r's
 * first step) binds its fetch (src/rbo_fetch.c), scans the game's own ring for newly confirmed remote frames, then runs the core's step
 * path (modes.c modes_rbo_step -> rb.c rb_step): rb composes the live table (local words + confirmed remote words), rolls back to the
 * earliest frame whose confirmed word differs from the one it ran on, re-simulates (<= max_rb frames, inside this game frame) and runs the
 * live pass. EVERY pass goes through run_step -> rbo_l3s_call_step with rbo_l3s_ctx (L3-stock, D-B) and the 256 KiB stack pre-zero, with
 * the C1b machinery unchanged: snapshots, the camera block pin, L4, +0x0c0 delivered late, +0x058 as state, POSE/KEEP, the KHit census.
 *
 * THE PASSIVE-TRANSPARENCY DECISIONS (M2-INTERFACES.md §9; the rollback core's record, every row decided as "installed at process start and
 * transparent while passive" or "installed / enabled only at ARM"). Evidence labels: V = shown (where), H = not yet shown.
 *   P1-P4, P6-P10 the 9 vtable slots: process start; rbo_thunks.S = M1's proven HOOK macro (tools/rbo_static.py R7: textually identical
 *      after the documented substitutions; R8: a missing XRSTOR64 / a jump to orig are caught). tgt = orig while passive. V (core scratch
 *      smoke: the NOBS-CTX probe through all 9 rbo slots = the direct original, 2 teeth caught; RBO-CTX is the test harness's proof).
 *   P5 the enqueue: process start; M1's retarget to nobs_enq_hook, unchanged body. V (as M1).
 *   F  the FetchInput call 0x1403fe6e1: process start; rel32 -> `jmp [rip+0]; dq cell` (cell = FetchInput while passive). V (scratch smoke:
 *      FetchInput's entry context through the call path = direct).
 *   bodies (rbo_body.c): gptr reads only, no vector register, no CRT, no allocation, no LOG (R2-R4 + 4 planted violations caught, R8);
 *      the state machine they call (rbo.c) writes no game byte before ARMED (rbo_h_game_writes == 0 in the scratch ZERO smoke: every fake
 *      region and both byte streams identical to no-plugin, with a PROBE entry in the battle). V in the fake game; RBO-ZERO is the harness's.
 *   M1 the allocator hooks (GMalloc vtable +0x10/+0x18/+0x20/+0x30, the CRT heap IAT). v0.29.1 (review B1; the v0.29.0 "restore after
 *      Shutdown or TAINT" rule left C wrappers in the vtable for the rest of the process after every real armed battle: 65 of 66 live C1b
 *      battles kept arena blocks past Shutdown). Now FRONT DOORS (src/rbo_thunks.S rbo_md_door*: `jmp [cell]`), patched into the 8 slots
 *      ONCE, at the first ARM of the process (game thread, IsPaused pre of round r's 1 -> 2, before the first armed step), never removed.
 *      The cells are this file's: the C hooks (mem.c hk_*) from a successful ARM to END; the originals at every other time, except that the
 *      pointer entries (realloc / free / getsize) go through a range door (two compares on the pointer argument: status flags only) while
 *      the arena still holds live blocks, so an arena block UE frees in a later battle still reaches the arena. A failed ARM never points a
 *      cell at a hook. Never installed while no battle has armed (UE's originals, RBO-ZERO). V: RBO-ALLOC (the R-STACK probe through all 8
 *      entries between battles and in a passive battle after an armed one = the direct original; today's C wrapper detected; leaked
 *      blocks freed later reach the arena; the next modded battle arms again).
 *   M2 the arena reservation (64 MB): process start (a VirtualAlloc, not a game write).
 *   M3/M4 the listener proxy (A_LISTENER_PTR swap), L4 revert, scrubs, L3-stock + the 256 KiB pre-zero: inside run_step, i.e. per armed pass
 *      only (rbonline reaches run_step only from rbo_step_armed). Never while PASSIVE.
 *   M5 the snapshot set (pin scans, camera block, gs pins, KHit census): built AT ARM (modes_rbo_arm -> ss_build, reads only); restores write
 *      only inside armed rollbacks. V (scratch smoke: 347 rollbacks, the final core state = a no-plugin run's).
 *   M6 InputLag pin, M7 stall lift / reset wall / drain repair: rbo.c / rbo_fetch.c, armed only (counted writes).
 *   M8 CRT rand: NOT installed in rbonline (Q5 decision: no IAT patch; if the core draws ucrt rand inside Step in a human-vs-human battle, a
 *      resim would draw again and CHECK reports the divergence -> DESYNC -> drain; S-B settles it. H).
 *   M9 Init/Setup allocation tracking: impossible while PASSIVE (Q1 decision for this build: option c, accept and measure; the snapshot set
 *      at ARM has no tracked-Init pins, so a battle object allocated in Init and written by Step outside every other pin is not restored by a
 *      rollback -> divergence -> CHECK -> DESYNC -> drain. H; the live C1b sessions counted 1,521 such pins / 331,355 bytes offline).
 *   M10 L1 probes, the IFC Setup hook, ifc_census thunks, MinHook, the lever, ns_start, rand, the watch DRs, the generic VEH: never (main.c
 *      returns before them; tools/rbo_static.py R6, planted counterpart caught).
 *   M11 (v0.29.4 FACES, design_v0294/faces/DESIGN.md §3) lip-sync (lipsync=1; absent / 0 = off as before): the KEEP blocks are a
 *      plugin VirtualAlloc at process start (rb_cfg_check, like M2); the module runs only from ARM (rb_battle_begin in modes_rbo_arm) to END
 *      (rb_battle_end in modes_rbo_end), inside the armed passes' L4 post path and the rb restore sites. No hook, patch, slot or thread of
 *      its own; nothing while PASSIVE (every entry returns at its gate). V: harness RBO cases LIPS LIPSU LIPSZ (passive bytes = REF).
 *   Stack: ARM (ss_build) and END (the rb tail) run inside hook bodies on M1's 256 KiB private stacks. V (scratch smoke high-water marks:
 *      IsPaused 9 KiB with ARM, Tick 12.5 KiB with END; the real game's pin scans use static arrays: H that it stays far below 256 KiB). */
#include "rbo_int.inc"
#include "rb.h"
#include "modes.h"
#include "mem.h"
#include "c1a_run.h"
#include "l1.h"
#include "lipsync.h"   /* v0.29.4 FACES */
#include <stdlib.h>

RboCtx rbo_l3s_ctx = {
    .xmm_lo = { 0, 0, 0, 0, 0, 0, RBO_L3S_XMM6, RBO_L3S_XMM7, RBO_L3S_XMM8, RBO_L3S_XMM9, 0, 0, 0, RBO_L3S_XMM13, 0, 0 },
    .mxcsr = RBO_L3S_MXCSR, .fcw = RBO_L3S_FCW,
};

static int g_core_ok;
static void **g_iat[4];
static uint32_t g_last_f, g_f_mismatch, g_rb_lines, g_mask_f, g_c3_f, g_unexp_seen;
/* v0.29.1 (review B1): the allocator front doors (src/rbo_thunks.S) and their cells */
extern void *volatile rbo_md_cell[8];
extern void *rbo_md_orig[8], *rbo_md_hk[8];
extern uintptr_t rbo_md_alo, rbo_md_ahi;
extern void rbo_md_door0(void), rbo_md_door1(void), rbo_md_door2(void), rbo_md_door3(void), rbo_md_door4(void), rbo_md_door5(void), rbo_md_door6(void),
            rbo_md_door7(void), rbo_md_range1(void), rbo_md_range2(void), rbo_md_range3(void), rbo_md_range5(void), rbo_md_range6(void);
static int g_doors, g_doors_crt, g_doors_armed, g_modes_armed;
static int g_h_no_c3trip;                               /* harness plants (cfg_geti, read once; 0 in the plugin build) */
#ifdef SC6RB_HARNESS
static int g_h_abort_at, g_h_arm_fail, g_h_cwrap;
static int g_h_cf_old;   /* v0.33.0 (n2): XCHAINLAT */
#endif
int rbo_core_doors(void) { return g_doors; }
static char g_q1f[32];   /* v0.29.2: the Q1 failure of this battle's ARM ("" = none) */
const char *rbo_core_q1_fail(void) { return g_q1f[0] ? g_q1f : NULL; }
/* the cells: armed = the C hooks; otherwise the originals, or the range doors for the pointer entries while the arena holds live blocks */
static void doors_set(int armed, int range) {
    if (!g_doors) return;
    void *rng[8] = { NULL, (void *)rbo_md_range1, (void *)rbo_md_range2, (void *)rbo_md_range3, NULL, (void *)rbo_md_range5, (void *)rbo_md_range6, NULL };
    for (int i = 0; i < 8; i++) {
        if (i >= 4 && !g_doors_crt) continue;
        void *v = armed ? rbo_md_hk[i] : (range && rng[i]) ? rng[i] : rbo_md_orig[i];
#ifdef SC6RB_HARNESS
        if (!armed && g_h_cwrap) v = rbo_md_hk[i];   /* XALLOC: the C hooks stay in place after the battle (the v0.29.0 TAINT state) */
#endif
        InterlockedExchangePointer((void *volatile *)&rbo_md_cell[i], v);
    }
    g_doors_armed = armed;
}
#define COST_CAP 65536
static float *g_cost; static uint32_t g_cost_n;

/* ---------------------------------------------------------------- the C1b knobs rbonline runs with (one fixed set: rbnet's, C2-proven) */
void rbo_core_force(char *csv, size_t cap) {
    Config *c = &g_cfg;
    size_t o = 0; csv[0] = 0;
#define F(field, val, name) do { if ((long long)(c->field) != (long long)(val)) { c->field = (val); o += (size_t)snprintf(csv + o, o < cap ? cap - o : 0, "%s%s", o ? "," : "", name); } } while (0)
    F(l4, 1, "l4"); F(l4_miss, 0, "l4_miss"); F(l3_ini, 1, "l3"); F(l3_fpenv, 1, "l3_fpenv"); F(l3_mxcsr, 0x1f80u, "l3_mxcsr"); F(l3_x87cw, 0x027fu, "l3_x87cw");
    F(l3_vzeroall, 1, "l3_vzeroall"); F(l3_zero_kb, c->rbonline_l3_zero_kb, "l3_zero_kb"); F(l1, 0, "l1");
    F(cam_pin_full, 1, "cam_pin_full"); F(camera_exclude, 0, "camera"); F(camera_resync, 1, "camera_resync"); F(ws_statics, 0, "ws_statics");
    F(query_forward, 0, "query_policy"); F(lsn_replay, 0, "listener_replay"); F(lsnw, 0, "listener_write_learning"); F(ai_rand_det, 1, "ai_rand");
    F(r7_cam_keep, 0, "r7_cam_keep"); F(pres_keep, 1, "pres_keep"); F(pres_ptr_rule, 0, "pres_ptr_rule"); F(r7_setup_barrier, 1, "r7_setup_barrier");
    F(r7_commit, 0, "r7_commit"); F(r7_forward, 0, "r7_forward"); F(r7_random_depth, 0, "r7_random_depth");
    F(net_delay, c->rbonline_d < 4 ? c->rbonline_d : 4, "net_delay"); F(net_max_rollback, c->rbonline_max_rb, "net_max_rollback"); F(net_remote_side, 1, "net_remote_side");
    F(rb_state_mask, (1ULL << (0x58 / 8)) | (1ULL << (0xc0 / 8)), "rb_state_slots"); F(rb_state_slots_set, 1, "rb_state_slots_set");
    snprintf(c->rb_state_slots_s, sizeof c->rb_state_slots_s, "0x58,0xc0");
    F(rb_mt_tail, 1, "rb_mt_tail"); F(rb_pinguard, 1, "rb_pinguard"); F(rb_quarantine, 1, "rb_quarantine"); F(rb_state_quiet, 0, "rb_state_quiet");
    F(rb_rollback, 1, "rb_rollback"); F(rb_skip_restore, 0, "rb_skip_restore"); F(rb_tick_limit, 1, "rb_tick_limit"); F(rb_state_gate, 1, "rb_state_gate");
    F(rb_test_layout_at, 0, "rb_test_layout_at");
    F(c1a_t2_heap_tag, 2, "c1a_t2_heap"); F(c1a_t2_follow, 0, "c1a_t2_follow"); F(c1a_t2_heap_bad, 0, "c1a_t2_heap_bad");
    snprintf(c->c1a_t2_heap_s, sizeof c->c1a_t2_heap_s, "map");
    F(rb_zone, 1, "rb_zone"); F(rb_zone_fight_mode, 2, "rb_zone_fight_mode"); F(rb_zone_entry_gap, 21, "rb_zone_entry_gap"); F(rb_zone_exit_margin, 30, "rb_zone_exit_margin");
    F(c1a_t1_rule, 0, "c1a_t1_rule"); F(c1a_mm_dumps, 0, "c1a_mm_dumps"); F(live_cap, 216000, "live_cap"); F(c1a_dump_every, 0, "c1a_dump_every");
    F(c1a_dump_at_n, 0, "c1a_dump_at"); F(c1a_t2mm_dumps, 0, "c1a_t2mm_dumps");
    F(stack_scrub_kb, 64, "stack_scrub_kb"); F(lsn_scrub_kb, 32, "lsn_scrub_kb"); F(stack_scrub, 1, "stack_scrub"); F(clean_zero, 1, "clean_zero");
    F(proxy, 1, "listener_proxy"); F(pin_containers, 1, "pin_containers"); F(scan_containers, 1, "scan_containers"); F(gs_pins, 1, "game_state_pins");
    F(replay_pause, 1, "replay_pause"); F(foreign_replay, 1, "foreign_replay"); F(between_record, 1, "between_policy"); F(fp_guard, 1, "fp_guard");
    F(track_init, 0, "track_init"); F(init_routing, 0, "init_routing"); F(crt_hooks, 1, "crt_hooks"); F(roll_depth, 7, "roll_depth"); F(fake_delay, 7, "fake_delay");
    F(arena_mb, 64, "arena_mb");
    if (c->lipsync != 1 || lipsync_h_force_off()) F(lipsync, 0, "lipsync");   /* v0.29.4 FACES: lipsync=1 stays (armed battles only, DESIGN.md §3); any other value -> 0 */
    F(ifc_census, 0, "ifc_census"); F(khit_census, 1, "khit_census"); F(static_learn, 0, "static_learn_frames");
    F(anim_probe, 0, "anim_probe"); F(stack_outparams, 1, "stack_outparams"); F(extw_autopin, 1, "extw_autopin"); F(check_foreign, 1, "check_foreign");
    F(ws_lsn_attrib, 1, "ws_lsn_attrib"); F(census, 1, "census");
    F(key_cycle, 0, "hotkey_cycle"); F(key_mark, 0, "hotkey_mark"); F(key_lbplayer, 0, "hotkey_loopback_player"); F(key_camera, 0, "hotkey_camera");
    F(key_l3, 0, "hotkey_l3"); F(key_scrub, 0, "hotkey_scrub"); F(key_depth, 0, "hotkey_depth"); F(key_passes, 0, "hotkey_passes"); F(key_anim, 0, "hotkey_anim");
    F(key_quit, 0, "hotkey_quit"); F(c1a_inject_set, 0, "c1a_setup_inject");
    if (c->replay_file[0]) { c->replay_file[0] = 0; o += (size_t)snprintf(csv + o, o < cap ? cap - o : 0, "%sreplay_file", o ? "," : ""); }
#undef F
    if (!o) snprintf(csv, cap, "-");
}

/* the CRT heap IAT (as main.c install_crt_heap: the exe's api-ms-win-crt-heap-l1-1-0.dll slots, V at the expected addresses) */
static void **find_iat(const char *dll, const char *fn) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(g_base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(g_base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(g_base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData);
            if (!strcmp((const char *)ibn->Name, fn)) return (void **)&iat->u1.Function;
        }
    }
    return NULL;
}

int rbo_core_init(void) {
    int fr = rbo_fetch_init();
    int ar = arena_setup(g_cfg.arena_mb, g_cfg.arena_fixed);
    itrack_init();
    int md = modes_rbo_init();
    g_cost = (float *)VirtualAlloc(NULL, sizeof(float) * COST_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const char *dll = "api-ms-win-crt-heap-l1-1-0.dll";
    g_iat[0] = find_iat(dll, "malloc"); g_iat[1] = find_iat(dll, "free"); g_iat[2] = find_iat(dll, "realloc"); g_iat[3] = find_iat(dll, "calloc");
    int iat_ok = g_iat[0] == (void **)VA(0x14322d2f8ULL) && g_iat[1] == (void **)VA(0x14322d2f0ULL) && g_iat[2] == (void **)VA(0x14322d2e0ULL) &&
                 g_iat[3] == (void **)VA(0x14322d2e8ULL);
    g_core_ok = fr && ar && md && g_cost;
    LOG("RB CORE init frames=%d arena=%d modes=%d crt_iat=%d ok=%d (the armed path's tables; no game write until ARM)\n", fr, ar, md, iat_ok, g_core_ok);
    if (!iat_ok) g_iat[0] = NULL;
#ifdef SC6RB_HARNESS
    g_h_abort_at = cfg_geti("h_rbo_abort_at", 0); g_h_arm_fail = cfg_geti("h_rbo_arm_fail", 0); g_h_no_c3trip = cfg_geti("h_rbo_no_c3_tripwire", 0);
    g_h_cwrap = cfg_geti("h_rbo_doors_cwrap", 0);
    g_h_cf_old = cfg_geti("h_rbo_cf_old", 0);   /* v0.33.0 (n2): XCHAINLAT */
    if (g_h_cf_old) LOG("RB CFG harness control h_rbo_cf_old=1 (XCHAINLAT: the light dump keeps only what v0.32.0's 64-frame ring held)\n");
    if (g_h_abort_at || g_h_arm_fail || g_h_no_c3trip || g_h_cwrap)
        LOG("RB CFG harness plants h_rbo_abort_at=%d h_rbo_arm_fail=%d h_rbo_no_c3_tripwire=%d h_rbo_doors_cwrap=%d\n", g_h_abort_at, g_h_arm_fail, g_h_no_c3trip, g_h_cwrap);
#endif
    return g_core_ok;
}

static void cf_reset(void);   /* v0.33.0 (n2), below */
int rbo_core_arm(uintptr_t mgr, uintptr_t obj, int remote) {
    (void)mgr; (void)obj;
    g_q1f[0] = 0;
    if (!g_core_ok) return 0;
#ifdef SC6RB_HARNESS
    if (g_h_arm_fail) { LOG("RB CORE arm harness plant h_rbo_arm_fail: the core arm fails here (M2: an asymmetric arm)\n"); return 0; }
#endif
    g_cfg.net_remote_side = remote + 1;
    cf_reset();   /* v0.33.0 (n2): the light dump's ring holds this battle's frames only (2048 slots outlive a battle's zone gaps) */
    g_last_f = 0; g_f_mismatch = 0; g_cost_n = 0; g_rb_lines = 0; g_mask_f = 0; g_c3_f = 0; g_unexp_seen = 0;
    /* §9 row M1 (v0.29.1, review B1): the allocator front doors, once per process; their cells stay at the originals until this arm succeeds */
    if (!g_doors) {
        void *door[8] = { (void *)rbo_md_door0, (void *)rbo_md_door1, (void *)rbo_md_door2, (void *)rbo_md_door3,
                          (void *)rbo_md_door4, (void *)rbo_md_door5, (void *)rbo_md_door6, (void *)rbo_md_door7 };
        void **iat[4] = { g_iat[0], g_iat[1], g_iat[2], g_iat[3] };
        int crt = g_iat[0] && g_cfg.crt_hooks;
        rbo_md_alo = (uintptr_t)g_arena.base; rbo_md_ahi = (uintptr_t)g_arena.base + g_arena.cap;
        if (!mem_doors_install(door, crt ? iat : NULL, rbo_md_orig, rbo_md_hk, rbo_md_cell)) { LOG("RB CORE arm mem_doors_install failed (cells at the originals)\n"); return 0; }
        g_doors = 1; g_doors_crt = crt;
    }
    itrack_clear();   /* entries of an earlier armed battle: their frees bypassed the hooks since (the doors passed them through) */
    LOG("RB CORE arm doors=%d crt=%d remote=p%d\n", g_doors, g_doors_crt, remote + 1);
    /* M5: the battle's core state and the snapshot set (v0.29.2: + the Q1 root walk and the late pins, OPTA-M2-SPEC.md §16) */
    g_modes_armed = 1;   /* modes_rbo_end at END even if the snapshot set fails below */
    g_q1f[0] = 0;
    if (!modes_rbo_arm(rbo_o_step)) {
        const char *q = modes_q1_fail();
        if (q) snprintf(g_q1f, sizeof g_q1f, "%s", q);
        LOG("RB CORE arm snapshot set failed%s%s (the cells stay at the originals)\n", q ? ": q1=" : "", q ? q : "");
        return 0;
    }
    doors_set(1, 0);   /* from here every allocation of every thread reaches the C hooks (arena routing inside the armed passes) */
    return 1;
}

/* v0.29.1 (review M2): the degraded arm. This PC agreed on ARM(r) (or learned that the peer armed at r) but cannot run the core: from here
 * to END its steps run the game's own step through the SAME L3-stock wrapper as the armed core's passes (the 256 KiB pre-zero and the
 * measured context), so both PCs run every remaining frame of the battle in the same context (spec D-C). No core, no FetchInput change, no
 * InputLag pin, no game write of ours (run_step swaps the listener pointer around the pass, as in every armed pass). */
void rbo_step_ctxonly(void *iface, void *in) {
    modes_rbo_step_plain(rbo_o_step, iface, (StepIn *)in);   /* run_step's live pass: the listener proxy (canonical queries), the scrubs, L3-stock */
}
/* v0.29.5 RBO (F1, design_v0295/rbo/DESIGN.md §3.1): the degraded pass now reverts UE's core writes (L4) like the armed peer's passes, so with
 * lipsync=1 the face family must be kept by lip-sync there too, as on the armed peer. lipsync_battle_begin normally runs at ARM inside
 * modes_rbo_arm (rb_battle_begin); an ARM that failed before it (local flags, no manager, the core init, the peer armed alone) never turned it
 * on: the degraded arm turns it on for the battle and off at END (LIPSYNCSTAT). lipsync absent / 0: nothing (faces freeze on both PCs, as on
 * any armed PC without lip-sync). */
static int g_degr_lips;
void rbo_core_degraded_begin(void) {
    g_degr_lips = 0;
    if (lipsync_on()) return;   /* modes_rbo_arm ran: modes_rbo_end (rb_battle_end) turns it off at END */
    lipsync_battle_begin(rbo_battle());
    g_degr_lips = lipsync_on();
}
void rbo_core_degraded_end(void) {
    if (!g_degr_lips) return;
    g_degr_lips = 0;
    lipsync_battle_end(rbo_battle());
}

void rbo_step_armed(void *iface, void *in) {
    uint32_t f = g_last_f + 1;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_rbo_crash_at && f == (uint32_t)g_cfg.h_rbo_crash_at) { volatile int *bad = (volatile int *)(uintptr_t)8; *bad = 1; }   /* CRASH: a fault in our step code */
    if (g_cfg.h_rbo_drain_at && f == (uint32_t)g_cfg.h_rbo_drain_at) rbo_drain(RBW_FAULT);                                                 /* DRAIN: a unilateral drain */
    if (g_h_abort_at && f == (uint32_t)g_h_abort_at) rb_abort_ext("test_abort");   /* v0.29.1 ABORT0 / XABORT0: a real rb_abort while armed */
#endif
    uint64_t t0 = qpc();
    rbo_fetch_step_begin(f);
    rbo_fetch_confirm_scan(f);
    {   /* the R-3 tripwire (spec §12 T-M2-3): mgr+0x12f8 (the UE input-disable mask; the phase-3 all-input write 0x1403fdafe) changed between two
         * fetches outside a zone: the cross-PC timing of that write is unproven, so the battle drains (the existing mask_in_fight check, armed) */
        RboFrame *a = rbo_frame(f), *b = rbo_frame(f - 1);
        if (a && b && a->fetched && b->fetched && a->m12f8 != b->m12f8 && !rb_zone_of(f) && rbo_state() == RBO_ARMED && !g_mask_f) {
            g_mask_f = f;
            rbo_log("RB UNEXPECTED kind=mask b=%u detail=m12f8_0x%x_to_0x%x_at_f%u_outside_a_zone", rbo_battle(), b->m12f8, a->m12f8, f);
            rbo_drain(RBW_MASK);
        }
        /* v0.29.1 (review m2): the c3f0 predicate (FUN_1403f2220: phase 3 of mgr+0x1480, the option byte, the player not local) sampled at the
         * live fetch is reused as the confirmed truth; its input (the UE phase byte) moves later under rollback and by a different amount on
         * each PC. A change of it between two fetches outside a zone drains, as the m12f8 tripwire above (in all live data it never fired:
         * phase 3 sits in the KO window, a zone) */
        for (int p = 0; p < 2 && !g_h_no_c3trip; p++)
            if (a && b && (a->fetched >> p & 1) && (b->fetched >> p & 1) && a->c3[p] != b->c3[p] && !rb_zone_of(f) && rbo_state() == RBO_ARMED && !g_c3_f) {
                g_c3_f = f;
                rbo_log("RB UNEXPECTED kind=c3 b=%u detail=p%d_c3f0_predicate_%u_to_%u_at_f%u_outside_a_zone", rbo_battle(), p + 1, b->c3[p], a->c3[p], f);
                rbo_drain(RBW_C3);
            }
    }
    {   /* v0.29.1 (review m4 / m3 e): what the armed FetchInput saw that stock online never does */
        uint32_t u = rbo_fetch_unexp & ~g_unexp_seen;
        if (u) {
            g_unexp_seen |= u;
            if (u & 1u) rbo_log("RB UNEXPECTED kind=online_session b=%u detail=CheckOnlineSessionActive_returned_0_at_f%u", rbo_battle(), f);
            if (u & 2u) rbo_log("RB UNEXPECTED kind=timers b=%u detail=the_stock_input_suppression_timers_applied_at_f%u_(the_word_is_stock's)", rbo_battle(), f);
            if (u & 4u) rbo_log("RB UNEXPECTED kind=player_index b=%u detail=FetchInput_asked_for_a_player_outside_0..1_at_f%u", rbo_battle(), f);
            if (rbo_state() == RBO_ARMED) rbo_drain((u & 3u) ? RBW_TIMERS : RBW_INTERNAL);
        }
    }
    if (c1a_next_f() != f && !g_f_mismatch++) { rbo_log("RB UNEXPECTED kind=frame_map b=%u detail=rbo_f_%u_c1a_f_%u", rbo_battle(), f, c1a_next_f()); rbo_drain(RBW_INTERNAL); }
    modes_rbo_step(iface, (StepIn *)in);
    g_last_f = f;
    if (g_cost && g_cost_n < COST_CAP) g_cost[g_cost_n++] = (float)qpc_us(qpc() - t0);
}

void rbo_core_tick(void) {   /* Tick pre while ARMED / DRAINED */
    g_tick_steps = 0;                    /* rb.c's tick bookkeeping (hk_tick's role: a new tick before its first step) */
    rbo_fetch_confirm_scan(g_last_f);    /* the frontier also moves on ticks without a step (a stall, a barrier): the drain run-out and the reset wall see it */
}
uint32_t rbo_core_last_f(void) { return g_last_f; }
uint32_t rbo_core_confirmed(void) { RboFrame *f = rbo_frame(rb_confirmed()); return f ? f->ff : 0; }
int rbo_core_zone(uint32_t f) { return rb_zone_of(f); }
int rbo_core_on(void) { return rb_is_on(); }

void rbo_core_end(const char *why) {
    LOG("RB CORE end why=%s frames=%u mispredicts=%u fetch_calls=%llu fetch_pred=%llu fetch_miss_drained=%llu\n", why, g_last_f, rbo_fetch_mispredicts(),
        (unsigned long long)rbo_fetch_calls, (unsigned long long)rbo_fetch_pred, (unsigned long long)rbo_fetch_miss_drained);
    if (g_modes_armed) { g_modes_armed = 0; modes_rbo_end(); }
    /* v0.29.1 (review B1): every deferred free is final now (no rollback after END), then the cells leave the C hooks */
    int nconf = def_count() ? def_confirm_all() : 0;
    uint64_t live = g_arena_ok && g_arena.h ? g_arena.h->live_blocks : 0;
    int was = g_doors_armed;
    doors_set(0, live != 0);
    rbo_log("RB MEM b=%u end doors=%d was_armed=%d cells=%s arena_live_blocks=%llu deferred_confirmed=%d (the doors pass through from here; a live arena "
            "block freed later still reaches the arena)", rbo_battle(), g_doors, was, !g_doors ? "-" : live ? "originals+range" : "originals",
            (unsigned long long)live, nconf);
}
/* the first rbo body after a battle's Shutdown (rbo.c): UE has freed what it frees at Shutdown; with the arena empty the pointer entries go
 * straight to the originals too (with blocks left they keep the range door: two compares, status flags only) */
void rbo_core_mem_after_shutdown(uint32_t b) {
    if (!g_doors || g_doors_armed) return;
    uint64_t live = g_arena_ok && g_arena.h ? g_arena.h->live_blocks : 0, bytes = g_arena_ok && g_arena.h ? g_arena.h->live_bytes : 0;
    doors_set(0, live != 0);
    rbo_log("RB MEM b=%u after_shutdown arena_live_blocks=%llu live_bytes=%llu cells=%s deferred=%d (front doors: a passive battle reaches UE's "
            "originals with the game's own context)", b, (unsigned long long)live, (unsigned long long)bytes, live ? "originals+range" : "originals", def_count());
}

/* v0.29.1 (review B2): the core aborted while armed (rb.c rb_abort). Never silent: a committed frame already known to be mispredicted is a
 * DESYNC now; otherwise the battle drains (why=abort) and every frame committed or run without a correcting core is verified when its truth
 * arrives (rb.c), while CHECK keeps running on the straight passes. */
void rbo_core_aborted(const char *cause, uint32_t f, uint32_t unconfirmed, uint32_t mm_f, uint64_t mm_used, uint64_t mm_truth, uint32_t unverified) {
    rbo_log("RB ABORT b=%u f=%u cause=%s unconfirmed=%u mispredicted_f=%u unverified=%u (no rollback any more: committed unconfirmed frames are verified "
            "when their truth arrives, CHECK keeps running)", rbo_battle(), f, cause, unconfirmed, mm_f, unverified);
    if (mm_f) rbo_desync_local(mm_f, mm_used, mm_truth, "committed_mispredict");
    else rbo_drain(RBW_ABORT);
}
void rbo_core_committed_mispredict(uint32_t f, uint64_t used, uint64_t truth) {
    rbo_log("RB UNEXPECTED kind=committed_mispredict b=%u detail=f%u_ran_remote_0x%016llx_truth_0x%016llx_(no_core_to_correct_it)", rbo_battle(), f,
            (unsigned long long)used, (unsigned long long)truth);
    rbo_desync_local(f, used, truth, "committed_mispredict");
}

static int fcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
void rbo_core_cost(char *out, size_t cap) {
    uint32_t n = g_cost_n;
    if (!n || !g_cost) { snprintf(out, cap, "p50=0 p99=0 max=0 n=0"); return; }
    float *c = (float *)malloc(sizeof(float) * n);
    if (!c) { snprintf(out, cap, "p50=? p99=? max=? n=%u", n); return; }
    memcpy(c, g_cost, sizeof(float) * n); qsort(c, n, sizeof(float), fcmp);
    snprintf(out, cap, "p50=%.0f p99=%.0f max=%.0f n=%u", (double)c[(n - 1) / 2], (double)c[(uint32_t)(0.99 * (double)(n - 1) + 0.5)], (double)c[n - 1], n);
    free(c);
}
void rbo_core_summary(char *out, size_t cap) {
    char h[160]; rb_dhist_str(h, sizeof h);
    uint32_t pend = 0, ver = 0, lost = 0, drop = 0; rb_unverified_stats(&pend, &ver, &lost, &drop);
    snprintf(out, cap, "frames_armed=%u rollbacks=%u rb_hist=%s max_depth=%u mispredicts=%u unverified_left=%u verified=%u unverifiable=%u lost=%u",
             g_last_f, rb_rbk(), h, rb_dmax(), rbo_fetch_mispredicts(), pend + drop, ver, lost, rbo_fetch_lost());
}

/* ---------------------------------------------------------------- the confirmed frames' tier-1 components (the DESYNC light dump) */
/* v0.33.0 (OPTA-M2-SPEC.md §22.4, n2 = REVIEW_FINISH m1): 64 -> 2048 frames. The dump is written when the PEER's CHECK arrives (check_compare), at real
 * latency many frames after this PC confirmed the window: a 64-frame ring had already overwritten the window's first frames (f - 59 .. at 60 frames per
 * window) by then. 2048 >= 2 x the largest rbonline_check_every (600) + 848 frames (~14 s) of a late peer CHECK. h_rbo_cf_old=1 (harness only, XCHAINLAT):
 * the dump skips every frame a 64-frame ring would have overwritten (v0.32.0). */
#define CF_N 2048
#define CF_N_OLD 64
static struct { uint32_t f; uint64_t c[C1A_NCOMP], t1, t1x; } g_cf[CF_N];
static uint32_t g_cf_last;   /* v0.33.0: the newest frame recorded (the XCHAINLAT control's overwrite rule) */
static void cf_reset(void) { memset(g_cf, 0, sizeof g_cf); g_cf_last = 0; }
void rbo_core_confirmed_frame(uint32_t f, const C1aPass *p) {
    int i = (int)(f % CF_N);
    g_cf[i].f = f; memcpy(g_cf[i].c, p->fr.c, sizeof g_cf[i].c); g_cf[i].t1 = p->fr.t1; g_cf[i].t1x = p->t1x;
    if (f > g_cf_last) g_cf_last = f;
    RboFrame *rf = rbo_frame(f);
    rbo_check_local(f, rf ? rf->round : 0, rf ? rf->ff : 0, p->t1x);
}
void rbo_core_zone_frame(uint32_t f) { rbo_check_zone(f); }   /* v0.32.0 review fix M1 */
void rbo_core_dump_frame(const char *path, uint32_t f) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    /* v0.32.0 (OPTA-M2-SPEC.md §21.1): every frame of the CHECK window this PC kept (v0.31.0: 8 back; v0.33.0: the ring holds 2048, so the whole window
     * survives a late peer CHECK), so a chain mismatch's first differing frame and component can be read from the two PCs' files */
    uint32_t back = (uint32_t)g_cfg.rbonline_check_every; if (back < 9) back = 9; if (back > CF_N) back = CF_N;
    size_t cap = 1u << 17; char *b = (char *)malloc(cap);
    if (!b) { CloseHandle(h); return; }
    int o = snprintf(b, cap, "RBO DESYNC LIGHT DUMP b=%u f=%u back=%u (tier-1 components of the confirmed frames this PC kept; compare the two PCs' files)\n", rbo_battle(), f, back);
    for (uint32_t k = 0; k < CF_N && o < (int)cap - 1200; k++) {
        int i = (int)((f + CF_N - k) % CF_N);
        if (!g_cf[i].f || g_cf[i].f > f || f - g_cf[i].f >= back) continue;
#ifdef SC6RB_HARNESS
        if (g_h_cf_old && g_cf_last >= g_cf[i].f + CF_N_OLD) continue;   /* XCHAINLAT: v0.32.0's 64-frame ring had overwritten it */
#endif
        o += snprintf(b + o, cap - (size_t)o, "f=%u t1=%016llx t1x=%016llx", g_cf[i].f, (unsigned long long)g_cf[i].t1, (unsigned long long)g_cf[i].t1x);
        for (int c = 0; c < C1A_NCOMP && o < (int)cap - 40; c++) o += snprintf(b + o, cap - (size_t)o, " %s=%016llx", k_c1a_comp[c], (unsigned long long)g_cf[i].c[c]);
        o += snprintf(b + o, cap - (size_t)o, "\n");
    }
    DWORD w; WriteFile(h, b, (DWORD)o, &w, NULL); CloseHandle(h);
    free(b);
}

#ifdef SC6RB_HARNESS
uint64_t rbo_h_arena_live(void) { return g_arena_ok && g_arena.h ? g_arena.h->live_blocks : 0; }   /* RBO-ALLOC */
void rbo_core_plant_leak(void) {   /* v0.29.0 TAINT plant (kept for the harness; v0.29.1 RBO-ALLOC leaks through the fake core's own allocations instead) */
    spin_lock(&g_arena_lock); void *p = arena_alloc(&g_arena, 64, 16); spin_unlock(&g_arena_lock);
    LOG("RB CORE harness plant h_rbo_leak_arena=1: one arena block left live (%p)\n", p);
}
#endif

/* rb.c: a rollback of the armed timeline (RB ROLLBACK, the first 50 per battle; the totals are in RB SUMMARY) */
void rbo_core_note_rollback(uint32_t s, uint32_t r, int depth, const char *cause) {
    if (g_rb_lines >= 50) return;
    g_rb_lines++;
    rbo_log("RB ROLLBACK b=%u f=%u depth=%d cause=%s r=%u", rbo_battle(), s, depth, cause, r);
}
