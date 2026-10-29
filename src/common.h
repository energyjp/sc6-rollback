/* sc6rb: Gate B test plugin for a SoulCalibur VI rollback retrofit.
 * Shared declarations. All game addresses are VAs at the preferred base
 * 0x140000000 and must go through VA() (the exe has ASLR). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <intrin.h>

#define SC6RB_VERSION "0.36.1-optA-M2"   /* 0.36.1 = the public release of 0.36.0: with no log_dir key the logs go to sc6rb_logs next to the plugin, and the desync badge text and one log line changed, as in 0.34.1 (protocol, rules and every gameplay path unchanged from 0.36.0); 0.36.0 = late sound stops under UE's latest-only tracking (the sound mirror, option b) and the voice +0x038 late with the lip-sync guard checked before late calls (OPTA-M2-SPEC.md §26: UE's sound handler tracks only the latest playback of (owner, id) and a +0x030 stops only that one; a per-key mirror of what UE tracks delivers the missed +0x030 stops of older playbacks late, holds back a late +0x028 when UE tracks a newer one, sends a sync stop before a late start that would untrack an older playing one, injects a stop inside the live pass before a forwarded start that would untrack the plugin's own late playback, and handles missed stop-alls; the voice +0x038 (13 bytes) is a late slot with the count gate, the near window, the supersede and the +0x040 stop guard; pres_flush runs the pending LIPSYNCGUARD check before UE gets late calls; the default rb_pres_slots is unchanged); 0.35.0 = late delivery of the Reversal Edge spark and sound (OPTA-M2-SPEC.md §25: listener slots +0x008 / +0x028 join rb_pres_slots: a call of the confirmed pass that UE never got is delivered once, late, inside L4's guard, with every byte UE reads at rdx (0x48-byte copy) and the confirmed r8 / r9; the near window (3 frames), the stop guard and the own stop (+0x010, watched); review fix M1: the sound +0x028 is guarded the same way by its stop slot +0x030 (watched: the stop guard, own entries, the own stop); +0x038 stays out); 0.34.2 = the listener-call trace for the spark test (OPTA-M2-SPEC.md §24: rb_lsn_trace, off by default, logging only; a private diagnostic build of 0.34.0; the public 0.34.1 is a separate, scrubbed line); 0.34.1 = the first public release (0.34.0 with the same three changes as 0.36.1); 0.34.0 = the EvaluateIf argc fix and the D-I census (OPTA-M2-SPEC.md §23: SC6's own off-by-one in EvaluateIf sub-ops 0x25 / 0x91 (they read one argument past argc: a per-launch stack-cookie word that can fail a Reversal Edge direction on one PC only) is fixed in memory by four jcc bytes (src/argcfix.c, argc_fix): in rbreplay for the whole process, in rbonline only in an armed battle whose two PCs both advertise it in HELLO (RBM_HF_ARGC, RBM_PROTO 10, a disagreement refuses to pair why=argc_off), applied at the agreed ARM and restored at END; the D-I census (vm_census, record / replay / rbreplay only) counts every sub-0x25 / 0x91 evaluation with the stock and the fixed return); 0.33.0 = missed effects, the at-limit badge and the ping-adaptive delay (OPTA-M2-SPEC.md §22: the presentation notifications of listener slots +0x120 / +0x128 that a muted resim pass made and UE never got are delivered once from the confirmed pass, inside L4's guard, with the +0x120 value channel's orphans repaired (rb_pres_slots, rb_pres_repair); the delay of an armed battle is computed at pairing from the two HELLOs' RTTs and agreed at ARM (rbonline_d_adapt; RBM_PROTO 9; a disagreement refuses to arm, why=d_disagree); the badge says ROLLBACK ON - at limit (waiting on the other PC) when >= 10 % of the last 120 armed fight ticks stopped at the max_rb bound; the light dump keeps 2048 frames (n2); the XBCNR0 plant replays its beacon 3 times (n3)); 0.32.0 = the CHECK chain and the clash diagnostics (OPTA-M2-SPEC.md §21: the cross-PC CHECK compares a rolling hash over every confirmed frame of its window, so a one-frame divergence between two CHECKs is caught, with the window's per-frame hashes named on a mismatch (RBM_PROTO 8); c1a_clash_bytes: the raw clash / aif bytes of both players as an A line in the .rec / .play on every change; the replay-edge baseline before the scan; v0.31.0 recordings replay (PLUGIN_COMPAT); round A finish (OPTA-M2-SPEC.md §21.12-21.14): the chain carried to every confirmed frame (zones, END) and the move-script VM diagnostics (src/vm.c: the D-F trace with clash and first-difference windows, the D-G bytecode dump, the D-H argument-tail fill and the zero-tail fix, all off by default, record / replay / rbreplay only)); 0.31.0 = the status badge, the sound cue and the round-start freeze (OPTA-M2-SPEC.md §20: an in-game badge drawn on the back buffer (IDXGISwapChain::Present, ClearView rects of a built-in 8x8 font, no pipeline state touched; rbonline_badge*), a short synthesized cue when a battle arms or falls back (rbonline_sound), and the armed round edge: one rescan per replay edge, no census at the rescans, no first-step diagnostic scan, the scan's probe memo, the step thread's pin / itrack filters and no GetAllocationSize past L4's first 4096 frees; RBM_PROTO 7); 0.30.0 = the feel of two-PC rollback (OPTA-M2-SPEC.md §19: S-B step 2 on v0.29.9 held 432/432 checks but hitched between rounds, armed one round late and ran one PC ahead; the zones run at the game's own InputLag with d pinned only on fight frames (rbonline_zone_lag_lead), ARM at the game's first round r=0 (the intro = the first reset, the beacon ordinal, the reverse view at r=0), the frame-advantage time sync RBM_TSYNC (rbonline_tsync); RBM_PROTO 6); 0.29.9 = the base-independent content digest (OPTA-M2-SPEC.md §18.12: the HELLO digest hashes per KHit node the class as a vtable RVA and the authored fields only, never an absolute address, a world buffer, the per-state flag or a never-written byte; RBM_PROTO 5; RB CONTENT ka= v8=, the dump's digest_v9 / digest_v8; the C1a assets hash on the same rule); 0.29.8 = the content-digest diagnosis (OPTA-M2-SPEC.md §18.11: S-B step 2 refused why=mismatch, content_eq=0; RB CONTENT sub-digests per player + a per-battle content dump; the digest and every verdict unchanged); 0.29.7 =the opponent SteamID (OPTA-M2-SPEC.md §18.10: S-B step 1 logged why=badid; session+0xd0 is a CommSys rand() handle, the SteamID is read from the CommSys peer of the opponent destination and accepted only if it is an individual SteamID, not this PC's own and Steam has a P2P session with it; RB PEERID); 0.29.6 = three small fixes before S-B (OPTA-M2-SPEC.md §18.9: review MINOR 4 a degraded peer's beacon drains the armed PC why=asym, MINOR 6 a 1-node std::map is a tree in the Q1 modes, MINOR 7 RBM_PROTO 4 and the canonical CHECK's ctl / ai divergence cases); 0.29.5 = the pre-S-B cleanup (OPTA-M2-SPEC.md §18: Q1R m4 E7 fails closed on what the scan could not record, m5 E5 = the game's loader / pick rules + the cursor-coverage check, m9 / m10 harness; RBO m6 canonical CHECK pointers, F1 / m7 the degraded arm runs the armed core's canonical pass, m8, the shipped-ini cases); 0.29.4 = Q1 container coverage + faces in the M2 path (OPTA-M2-SPEC.md §17: the walk's E5 vtb voice tables, E7 container census, E8 direct-command objects, the scan's empty-map rule, Q1COV v2; lip-sync in the S-M2a hybrid's title and in rbonline from ARM to END); 0.29.3 = the Q1 review fixes (OPTA-M2-SPEC.md §16.14: the relocated signature rows B1, the hybrid's pre-F lockstep M2, fail-closed gate / cap m3 / m4, q1sig named m7); 0.29.2 = Q1 (the Init/Setup blocks: the typed root walk at ARM, OPTA-M2-SPEC.md §16); 0.29.1 = the M2 review fixes (OPTA-M2-SPEC.md §15.7); Option A M2 (OPTA-M2-SPEC.md, sc6rb/M2-INTERFACES.md): + mode=rbonline (rollback inside the game's own online matches); 0.29.0 = the interface pass (stubs); 0.28.1-optA-M1 = M1 (netobserve, _v0281_snapshot_src) */

/* ---------------------------------------------------------------- addresses */
#define IMG_PREF 0x140000000ULL
extern uintptr_t g_base;
#define VA(x)     (g_base + ((uintptr_t)(x) - (uintptr_t)IMG_PREF))
#define RVA(p)    ((uintptr_t)(p) - g_base)
#define IN_IMAGE(p) ((uintptr_t)(p) >= g_base && (uintptr_t)(p) < g_base + g_image_size)
extern size_t g_image_size;

/* IBattleCore interface vtable (TSharedPtr object at mgr+0x1450) */
#define A_IFC_VT          0x14327b610ULL   /* v0.20.0 fix C1: 37 slots used, +0x00..+0x120 (+0xe0..+0x120 = the handler factories, static analysis 1.2) */
#define IFC_NSLOTS        37
#define IFC_INIT          0x00             /* thunk 0x1403c3650: mov rcx,rdx; jmp Init 0x1402da1b0 */
#define IFC_SHUTDOWN      0x18             /* thunk 0x1403bcee0: jmp Shutdown 0x1402da520 */
#define IFC_SETUP         0x60             /* thunk 0x1403cfe80: jmp Setup 0x1402db3a0 (allocs WorkMemory) */
#define IFC_STEP          0x68             /* thunk 0x1403d2a20: mov rcx,rdx; jmp Step 0x1402dbc60 */
#define A_THUNK_INIT      0x1403c3650ULL
#define A_THUNK_SHUTDOWN  0x1403bcee0ULL
#define A_THUNK_SETUP     0x1403cfe80ULL
#define A_THUNK_STEP      0x1403d2a20ULL
#define A_STEP            0x1402dbc60ULL
#define A_MGR_TICK        0x1403fbf30ULL   /* ALuxBattleManager::Tick (calls UpdateBattle in state 2) */
#define A_UPDATEBATTLE    0x1403fe520ULL

/* battle core state */
#define A_FRAMECTR        0x14470d0c4ULL   /* u32, incremented by Step, zeroed by Init */
#define A_LISTENER_PTR    0x14470d188ULL   /* scbattle::EventListenerBase* */
#define A_NULL_LISTENER   0x144100b30ULL
#define A_NULL_LISTENER_VT 0x143e853c8ULL
#define LISTENER_NSLOTS   41               /* +0x000..+0x140 */
#define A_CHARA_TBL       0x14470de90ULL   /* PLAYER* [2] */
#define A_PLAYER0         0x1447156f0ULL
#define PLAYER_STRIDE     0x973f0ULL
#define PLAYER_INPUT_OFF  0x2150           /* Scuffle's P1 input word */
#define A_WORKMEM_PTR     0x144843f00ULL   /* CBattleGameClass(0x144843ef0)+0x10 (GATE-A said [0x144843ed0+0x10]: off by 0x20) */
#define A_WORKMEM_RAW     0x144843f08ULL   /* +0x18 raw allocation */
#define WORKMEM_SIZE      0x19a710ULL
#define A_RNG             0x14485eb30ULL
#define A_CORE_BSS_LO     0x14470d000ULL
#define A_CORE_BSS_HI     0x14485f000ULL
#define A_DATA_SINGLE_LO  0x144100a40ULL
#define A_DATA_SINGLE_HI  0x144100ee0ULL
#define A_IMG_DATA_LO     0x14406e000ULL   /* .data + .bss of the whole exe */
#define A_IMG_DATA_HI     0x144878000ULL

/* UE allocator */
#define A_GMALLOC         0x1441971c8ULL   /* FMalloc* GMalloc */
#define A_FMEM_MALLOC     0x140d4f760ULL   /* FMemory::Malloc(size, align) -> GMalloc vt+0x10 */
#define A_FMEM_FREE       0x140d46a00ULL   /* FMemory::Free(p) -> GMalloc vt+0x20 */
#define GM_SLOT_MALLOC    2                /* +0x10 */
#define GM_SLOT_REALLOC   3                /* +0x18 */
#define GM_SLOT_FREE      4                /* +0x20 */
#define GM_SLOT_GETSIZE   6                /* +0x30 GetAllocationSize(ptr, SIZE_T&) */

/* CRT rand imports (api-ms-win-crt-utility-l1-1-0.dll) */
#define A_IAT_RAND        0x14322d800ULL
#define A_IAT_SRAND       0x14322d818ULL

/* frame input objects (mgr+0x478) */
#define A_INPUTLOG_VT     0x143282f18ULL   /* ALuxBattleFrameInputLog (offline) */
#define A_INPUTSYNC_VT    0x143283578ULL   /* ALuxBattleFrameInputSync (online) */
#define MGR_INPUTOBJ      0x478
#define MGR_STATE         0x1461
#define INPUT_BONLINE     0x4404
#define A_DELAY_SITE      0x1403f074fULL   /* WAZAAAAA offline-delay patch site */

/* v0.20.2 camera function block (audit/camdet/CHECK.md A1/A5): one heap block of CAMF_SIZE bytes at [A_CAMF_HOLDER+8]
 * (allocated by FUN_1403234a0, 0x1403234b9 mov ecx,0x41e0). The game's own camera SaveState FUN_140324900 copies the
 * whole block (0x140324932 mov r8d,0x41e0) plus the throw-camera gate dword A_CAM_GATE (0x14032499d lea). */
#define A_CAMF_HOLDER     0x14470ee90ULL
#define CAMF_SIZE         0x41e0
#define A_CAM_GATE        0x1440f4a84ULL   /* .data, outside core .bss: set by the throw camera's Start (0x140327f47), read by its Update (0x140328bfc) */

/* known noise for hashes */
#define A_SCRAPE_CTR0     0x14470e984ULL
#define A_SCRAPE_CTR1     0x14470e98cULL

/* ---------------------------------------------------------------- modes */
/* v0.22.0-C1a: record / replay (straight live passes; c1a_run.c). v0.23.0-C1b: rbreplay (the ghost replay through the rollback core, rb.c) */
enum { MODE_OFF = 0, MODE_OBSERVE, MODE_SYNC1, MODE_ROLL7, MODE_LOOPBACK, MODE_SYNC1X, MODE_RECORD, MODE_REPLAY, MODE_RBREPLAY, MODE_RBLIVE, MODE_RBNET, MODE_NETOBSERVE, MODE_RBONLINE, MODE_COUNT };   /* C2: + rbnet; optA M1: + netobserve; optA M2: + rbonline */
/* v0.24.0-C1b2: mode=rblive (C1B-R2-SPEC.md §2.5) = live play through the rollback core of rbreplay; c1a records (the .rec) */
static inline int is_rb_mode(int m) { return m == MODE_RBREPLAY || m == MODE_RBLIVE; }
/* C2 (C2-SPEC.md D14): the rollback-core modes (rbreplay, rblive, rbnet) at the PART A core sites; is_rb_mode keeps its meaning (PART B's lip-sync gate) */
static inline int is_rbcore_mode(int m) { return m == MODE_RBREPLAY || m == MODE_RBLIVE || m == MODE_RBNET || m == MODE_RBONLINE; }   /* optA M2: + rbonline
                                 * (its armed path runs the same core; the generic installers never run in rbonline: main.c branches first) */
extern const char *g_mode_names[MODE_COUNT];
int mode_from_name(const char *s);

/* ---------------------------------------------------------------- config */
#define MAX_EXTRA 32
typedef struct {
    int mode;
    int cycle[MODE_COUNT]; int ncycle;
    int key_cycle, key_mark, key_lbplayer;
    char log_dir[MAX_PATH];
    int flush_every;
    int step_log;
    int ws_frames;
    int census;
    int proxy;
    int arena_mb;
    int init_routing;
    int pin_containers;
    int scan_containers;
    int roll_depth;
    int fake_delay;
    int lb_player;
    int ai_rand_det;
    int max_reports;
    int max_diff_ranges;
    int ifc_census;
    int nmask; uintptr_t mask_va[MAX_EXTRA]; size_t mask_len[MAX_EXTRA];
    int nregion; uintptr_t region_va[MAX_EXTRA]; size_t region_len[MAX_EXTRA];
    int nallow_vt; uintptr_t allow_vt[MAX_EXTRA];
    int check_foreign;
    int crt_hooks;
    int lsnw;
    int extw_autopin;
    int camera_exclude;
    int camera_resync;
    int key_camera;
    int extw_frames;
    int track_init;
    int lsn_replay;
    int foreign_replay;
    int stack_scrub;            /* derived: stack_scrub_kb > 0 */
    int stack_scrub_kb, lsn_scrub_kb;
    int gs_pins;
    int replay_pause;
    int xr;                     /* round 12 XR probe: REMOVED after the round 12 crash (always 0) */
    int static_learn;           /* round 13: learn core-written statics in the exe's .data from roll7 resims, then pin them */
    int fp_guard;
    int ws_statics;
    int ws_lsn_attrib;
    int anim_probe;             /* round 15: hash motion objects around live/resim steps (read-only, ReadProcessMemory) */
    int key_anim;               /* anim_pins on/off (no default key since round 16) */
    int sync1x_passes;          /* round 16: 4 = L,R,M,M2 (default); 2 = L,R then commit L (mirrors roll7 depth 1) */
    int key_passes;             /* F5: sync1x passes 4 <-> 2; in roll7: path ring -> sync -> fresh -> ring */
    int r7_path_ini;            /* round 18: roll7_path=unified|ring_old|sync|fresh (initial) */
    int r7_unified_plant;       /* harness negative control: unified path without between-step re-application */
    int r7_plant_stale;         /* harness negative control: keep the stale ring snapshot even when between-step writes were found */          /* round 14b: words written inside listener calls (UE) never become WSX pins */             /* round 14: snapshot the statics the step writes (B0 write-set union) */
    int clean_zero;             /* round 14: frameless zeroing right before each step and after each listener call */               /* round 13: same MXCSR/x87 control word for live and resim passes */
    int between_record;         /* round 12: between_policy=record|off (alias of foreign_replay) */           /* round 11: pass-through while the game's own short replay plays */                /* round 10: pin the heap state the game's own SaveState saves */
    int query_forward;          /* muted passes without a record: 1 = call UE's query, 0 = NullListener answers */
    int stack_outparams;
    int key_scrub, key_depth;
    /* v0.19.0 localizer */
    int l1, l3_ini, key_l3, l3_zero_kb, l1_cmp_every, l1_full_budget, l1_cmp_budget, greport_budget;
    int l1_nextra; uintptr_t l1_extra_va[4]; size_t l1_extra_len[4];
    char watch[256];
    /* v0.20.0 C0b (GATE-C-DESIGN §8 C0b, Appendix B): one configuration per launch, no hotkeys */
    int l4;                     /* L4 canonical listener: 1 = revert UE's writes in every forwarded listener call, null queries in every pass;
                                 * v0.21.0-S1: 2 = guard (log UE's core .bss writes outside P and the FX scratch e9b0, never revert; live
                                 * queries vanilla, resims muted/null); 0 = off */
    int l4_miss;                /* L4MISS watchdog: 0 off, 1 live passes, 2 every pass */
    int r7_random_depth;        /* roll7 depth 0..roll_depth per frame from a seeded PRNG */
    uint32_t r7_random_seed;
    int r7_commit;              /* keep the resim lineage (skip the g_r7post restore) */
    int r7_forward;             /* resims call UE's notifications too (with the L4 revert) */
    int r7_cam_keep;            /* v0.20.1: with r7_commit, the live camera is put back after the resims (camera_resync rule); v0.20.2 default 0 */
    int cam_pin_full;           /* v0.20.2: pin the whole camera function block (1, default) or its 4 slot heads (0, the v0.20.1 negative control);
                                 * v0.21.1: 1 pins the block whenever the typed-vtable scan finds its base (no adjacency gate); a block that cannot
                                 * be pinned is a loud failure (CAMPIN FAIL, title cam=FAIL, rollback off until a rescan pins it), never the slots */
    int l3_fpenv;               /* L3 also loads a canonical MXCSR / x87 control word for every step pass */
    uint32_t l3_mxcsr, l3_x87cw;
    int l3_vzeroall;            /* vzeroall in the L3 wrapper (only if the CPU and OS support AVX) */
    char title_tag[48];         /* shown in the window title (one configuration per launch) */
    /* v0.21.0-S1 (audit/direction/CHECK.md §6 S1, P-fields.md §2) */
    int pres_keep;              /* with r7_commit=1: the live presentation class P is put back after the resims (default 0) */
    int r7_setup_barrier;       /* a resim never starts at or before the last Setup/Init or UE listener slot +0x0c0 call (default 0) */
    int khit_census;            /* log the KHit capsules' bone ints once per player at each battle's first step (default 0) */
    /* v0.21.1 (S1 fixes) */
    int pres_ptr_rule;          /* pres_keep: which differing pointer keeps a P range's resim bytes: PRES_RULE_ARENA (0, default "arena":
                                 * arena pointers only; image pointers such as inline-object vtables keep the live bytes) or
                                 * PRES_RULE_IMAGE_ARENA (1, "image_arena": the v0.21.0 behavior, the negative control) */
    int pres_ptr_rule_bad;      /* the ini value was not "arena" or "image_arena" (the default was used; logged) */
    /* v0.22.0-C1a (GATE-C-DESIGN §8 C1a) */
    char replay_file[MAX_PATH]; /* mode=replay: absolute path of the .rec to inject */
    int c1a_dump_every;         /* record/replay: pointer-normalized dump of the image regions every N battle steps (default 600; 0 = off) */
    uint32_t c1a_refuse;        /* setup categories whose difference refuses the injection (bit mask of C1A_S_*; default exe..assets) */
    int c1a_refuse_bad;         /* an unknown category name in c1a_refuse (ignored; logged) */
    int arena_fixed;            /* reserve the arena at 0x7e0000000000 first (default 1 in record/replay, 0 otherwise) */
    /* v0.22.1-C1a (C1A-AUDIT.md C1-C2) */
    uint32_t c1a_inject;        /* c1a_setup_inject: setup categories whose CpuPersonality-derived bytes the replay may take from the recording's f0
                                 * dump (mask of C1A_S_RULES / C1A_S_AI; default both; 0 = none, the v0.22.0 behavior) */
    int c1a_inject_bad;         /* an unknown name in c1a_setup_inject (ignored; logged) */
    /* v0.23.0-C1b (C1B-SPEC.md §3.1): raw parsed values; mode=rbreplay validates them (rb.c rb_cfg_check, CFG C1B REFUSE) */
    int net_remote_side, net_delay, net_max_rollback;
    int emu_latency_ms, emu_jitter_ms, emu_loss_pct, emu_redundancy;
    long long emu_seed;
    char rb_state_slots_s[128]; uint64_t rb_state_mask; char rb_state_bad[32]; int rb_state_slots_set;
    int rb_mt_tail, rb_pinguard, rb_quarantine, c1a_t2_heap_tag, c1a_t2_heap_bad;
    int rb_rollback, rb_skip_restore, rb_tick_limit, rb_state_gate, rb_test_layout_at;
    int rb_state_quiet;         /* v0.23.1 (review F1/F7): 0 = off; N = a state-class call in a predicted live pass is forwarded live with a
                                 * barrier when the remote record was constant over the last N known frames (rb.c) */
    /* v0.24.0-C1b2 PART A (C1B-R2-SPEC.md §2.9, §4.1): raw parsed values; the rb modes validate them (rb.c rb_cfg_check) */
    int live_cap;               /* rblive: frames the emulator schedule and the live table hold (600..432000) */
    int c1a_t1_rule;            /* 0 = ptrmap (default, the F1 pointer map), 1 = heaptag (the v0.23 rule: a control, CTRL); -1 = bad value */
    int c1a_mm_dumps;           /* rbreplay: first-mismatch dumps per battle (0..16) */
    /* C1B4: (round 4, item 1) diagnostic dumps, outside every setup / simcfg / rbcfg hash (a recording replays with or without them):
     * c1a_dump_at = up to C1A_DUMP_AT_MAX explicit frames dumped in every C1a mode (rb modes: the frame's confirmed pass); c1a_t2mm_dumps = mode=replay:
     * a dump at the first tier-2 mismatch of each cell, at most N per battle (other modes: logged as ignored) */
#define C1A_DUMP_AT_MAX 32
    uint32_t c1a_dump_at[C1A_DUMP_AT_MAX]; int c1a_dump_at_n, c1a_dump_at_bad; char c1a_dump_at_s[400];
    int c1a_t2mm_dumps;
    int h_dump_first_pass;      /* C1B4 harness-only control (DARBX): an rb-mode dump keeps its frame's FIRST pass (0 in the plugin build) */
    /* v0.32.0 (OPTA-M2-SPEC.md §21.6, D-B): c1a_clash_bytes=1 = the raw clash / aif spans of both players into the .rec / .play as an A line
     * whenever they change (the frame's confirmed pass; outside every setup / simcfg / rbcfg hash). h_ab_at_confirm (harness only, the control
     * XGAB): the bytes are read at the confirmation instead of right after the pass (in the rb modes: a later frame's state) */
    int c1a_clash_bytes, h_ab_at_confirm;
    /* v0.32.0 review fix (REVIEW_BUILD.md B1; OPTA-M2-SPEC.md §21.13, src/vm.c): the move-script VM diagnostics, all OFF by default, record / replay /
     * rbreplay only (never in rbonline), outside every setup / simcfg / rbcfg hash. vm_trace (D-F), vm_trace_frames / _pre / _clashes / _cap,
     * vm_fill = off | rand | <value> (D-H), vm_fix (the zero tail; wins over vm_fill), vm_bytecode + vm_bytecode_slots (D-G). h_vm_* harness only. */
    int vm_trace, vm_trace_frames, vm_trace_pre, vm_trace_clashes, vm_trace_cap;
    int vm_fill_on, vm_fill_rand, vm_fill_bad; uint32_t vm_fill_val; char vm_fill_s[32];
    int vm_fix, vm_bytecode, vm_bc_n, vm_bc_bad; uint16_t vm_bc_slots[8];
    int vm_census, vm_census_cap;   /* v0.34.0 (OPTA-M2-SPEC.md §23.2, D-I): the EvaluateIf sub-0x25 / 0x91 census (stock vs fixed return); detail lines per battle capped at vm_census_cap */
    int h_vm_pushres, h_vm_sig_bad, h_vm_every_pass;
    /* v0.32.0 finish review fixes (REVIEW_FINISH.md F4 / n6 / n7; OPTA-M2-SPEC.md §21.14): vm_trace_diff_pre (the frames before the battle's first
     * tier-1 difference from the recording that its window writes; -1 = no difference window); h_vm_nodiff / h_vm_bc_once / h_vm_cet harness only */
    int vm_trace_diff_pre;
    int h_vm_nodiff, h_vm_bc_once, h_vm_cet;
    int rb_zone, rb_zone_fight_mode, rb_zone_entry_gap, rb_zone_exit_margin;   /* §2.6 lockstep zones */
    /* C1B3: (C1B-R3-SPEC.md §2.6.2, §4.1) c1a_t2_heap = raw | tag | map | follow. c1a_t2_heap_tag holds the RESOLVED rule (0 raw, 1 tag, 2 map;
     * the rbcfg value, so every v0.24 configuration keeps its rbcfg); follow (replay modes) is resolved from the recording's t2rule at its
     * load (c1a_run.c), before that it reads 1 */
    int c1a_t2_follow; char c1a_t2_heap_s[16];
    /* C1B3: harness-only switches (read only under SC6RB_HARNESS; 0 in the plugin build): one fix off = its negative control */
    int h_old_xpred, h_old_c0count, h_old_t1align, h_modset, h_content_gate_off, h_c0_refuse_off; char h_modset_root[MAX_PATH];
    int c1a_inject_set;         /* the c1a_setup_inject key is present in the ini (rblive: must be absent or none) */
    int h_pres_old, h_deny_off; /* harness-only switches (read only under SC6RB_HARNESS; 0 in the plugin build) */
    int h_barrier_at;           /* harness-only: a forced mid-fight repin barrier at this frame (GLIVE-AB) */
    int lipsync; char lipsync_s[16]; int h_lips_no_after; char h_lips_skip_site[16];   /* LIPSYNC: PART B key (0 off = absent, 1 on, -1 not a number) + harness-only controls */
    int h_lips_ev_budget;   /* LIPSYNC: v0.25 harness-only: the LIPSYNCEV lines per battle (harness default 300 = the plugin's fixed budget; 0 = none) */
    /* C2 (C2-SPEC.md §6.1): mode=rbnet keys (raw parsed values; rb_cfg_check validates them in rbnet: CFG C2 REFUSE) */
    int net_role;               /* 1 host, 2 guest, 0 absent, -1 bad */
    char net_peer[64];          /* "a.b.c.d:port" (never logged) */
    int net_port, net_bind;     /* net_bind: 0 auto, 1 loopback, 2 any, -1 bad */
    int net_peer_kind;          /* 0 udp, 1 script, -1 bad */
    char net_script_rec[MAX_PATH]; int net_script_from;
    int net_local_src;          /* 0 pad, 1 rec, -1 bad */
    char net_rec_file[MAX_PATH];
    int lock_delay;             /* 0 auto, 4..12 fixed, -1 bad */
    int net_lever;              /* 0 clamp, 1 one, -1 bad */
    int net_timeout_s, pause_cap_s;
    int key_quit;               /* hotkey_quit (rbnet only) */
    int present_net_remote_side, present_replay_file, present_emu;   /* presence tracking: rbnet refuses an explicit key */
    /* C2 harness-only switches (read only under SC6RB_HARNESS; 0 / empty in the plugin build) */
    char h_netem[16]; int h_netem_seed; int h_c2_corrupt, h_c2_check_xor_at, h_c2_flood_at, h_c2_noconv, h_c2_nolever, h_c2_nopauseflag, h_t2map_ver, h_c2_fbar_after, h_c2_ueskip_keep, h_c2_noseed, h_c2_setup_gate_off, h_c2_peerleft_off, h_c2_rs_burst, h_c2_noabort_tx; uint32_t h_c2_seed;   /* fix r1: + the two controls */
    char h_c2_hs[16], h_c2_su[16];
    /* Option A M1 (OPTA-M0.md §2.5): mode=netobserve keys */
    int netobs_trace, netobs_trace_mb, netobs_pkt_bytes, netobs_pkt_lines, netobs_d2_lines, netobs_chan4, netobs_offline, netobs_cost, netobs_t1;
    /* netobserve harness-only controls (read only under SC6RB_HARNESS; 0 in the plugin build) */
    int h_nobs_plant_write, h_nobs_plant_send, h_nobs_plant_sig, h_nobs_plant_xmm9, h_nobs_t1_cost;
    /* Option A M2 (OPTA-M2-SPEC.md, sc6rb/M2-INTERFACES.md §6): mode=rbonline keys. Raw parsed values; rbo_install validates them
     * (RB CFG REFUSE key=... -> PROBE off for the process: the battle stays M1-passive). The netobs_* keys above also configure the
     * M1 observer layer rbonline runs on. */
    int rbonline_d, rbonline_max_rb, rbonline_ranked, rbonline_check_every, rbonline_arm_round;
    uint32_t rbonline_channel;
    int rbonline_probe_max, rbonline_probe_gap_ms, rbonline_resend_ms, rbonline_peer_timeout_ms, rbonline_poll_ms, rbonline_arm_margin_ms;
    int rbonline_l3_zero_kb, rbonline_dump;
    int rbonline_unranked_ack;   /* v0.29.1 (review M3): 1 = the user never queues Ranked with this plugin installed (no ranked detector exists); default 0 = PROBE off */
    int rbonline_zone_lag_lead;  /* v0.30.0 (OPTA-M2-SPEC.md §19.2): the zone lag's lookahead (frames); 0 = v0.29.9's InputLag pin for the whole armed battle */
    int rbonline_tsync;          /* v0.30.0 (OPTA-M2-SPEC.md §19.4): 1 = this PC's time-sync waits on (reports are sent either way) */
    /* v0.31.0 (OPTA-M2-SPEC.md §20.1, §20.2): presentation only (never compared in HELLO, never in a hash): the in-game badge (on, the text's center x
     * and baseline y in percent of the frame, the glyph pixel size (0 = from the frame height), the show/hide key) and the sound cue */
    int rbonline_badge, rbonline_badge_x, rbonline_badge_y, rbonline_badge_scale, rbonline_badge_key, rbonline_sound;
    /* rbonline harness-only controls and plants (read only under SC6RB_HARNESS; 0 in the plugin build). h_rbo_* = the core's,
     * h_p2p_* = the transport's (the core copies them into P2pCfg). M2-INTERFACES.md §8 says which case uses which. */
    int h_rbo_fetch_no_c3f0, h_rbo_lift_unbounded, h_rbo_ctx_xmm9_zero, h_rbo_drain_at, h_rbo_crash_at, h_rbo_no_wall,
        h_rbo_lag_norestore, h_rbo_hello_ctx_xor, h_rbo_leak_arena, h_rbo_no_restore;
    int h_p2p_accept_any_id, h_p2p_no_guard, h_p2p_no_resend, h_p2p_corrupt_every;
    /* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.9): the rbreplay hybrid keys (rbreplay-only; every default = today's behavior, byte-identical; outside
     * simcfg and rbcfg). rbonline ignores the ini values (its walk and late-pin rule are by mode); other modes refuse them when present (CFG Q1 REFUSE). */
    int rb_l3stock;             /* 1 = every pass through rbo_l3s_call_step with rbo_l3s_ctx (rbonline's L3-stock wrapper); needs l3=1 (default 0) */
    int rb_arm_f;               /* ARM at frame F: frames < F run without arena routing and lockstep (v0.29.3: every one, M2); at F the snapshot set is rebuilt (0 = C1b) */
    int rb_init_pins;           /* 1 = C1b (every tracked block is pinned); 0 = only blocks tracked at or after the ARM frame (rbonline's rule, §16.4) */
    int q1_roots;               /* rbreplay: the Q1 root walk at every pin scan from ARM on (default 0) */
    int q1_cov;                 /* rbreplay: the Q1COV coverage diagnostic (default 0) */
    int q1_present;             /* bit mask of the five keys above present in the ini (1 rb_l3stock, 2 rb_arm_f, 4 rb_init_pins, 8 q1_roots, 16 q1_cov) */
} Config;
extern Config g_cfg;
void config_load(const char *ini_path);
void config_log(void);
uint64_t cfg_simcfg_hash(const Config *c);   /* v0.23.0-C1b: the C1a simcfg setup hash (its v0.22 fields) */
uint64_t cfg_rbcfg_hash(const Config *c);    /* v0.23.0-C1b: the rb/net/emu/c1a_t2_heap/rb_mt_tail hash (C1B-SPEC.md §2.9) */
size_t   cfg_controls_str(const Config *c, char *out, size_t cap);
/* optA M2: any [sc6rb] key of the loaded ini, for code that keeps its keys out of Config (a module reads its own keys without editing
 * config.c). Valid after config_load. */
int  cfg_geti(const char *key, int def);
void cfg_gets(const char *key, const char *def, char *out, unsigned cap);

/* ---------------------------------------------------------------- log */
void log_open(const char *dir);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)
void log_flush(void);
void log_close(void);   /* v0.31.0: test/overlay_test.c only */
void log_flush_crash(void);
double now_ms(void);
uint64_t qpc(void);
double qpc_us(uint64_t ticks);
extern char g_log_path[MAX_PATH];

/* ---------------------------------------------------------------- shared runtime state */
extern volatile const char *g_last_action;
extern volatile int g_mode;
extern volatile uint64_t g_seq;          /* live step sequence (monotonic per session) */
extern volatile int g_inert;
uint32_t core_frame(void);

/* step context: allocations/frees/rand on this thread are "inside" while g_ctx_tid matches.
 * The listener proxy clears g_ctx_tid while UE listener code runs. */
#define CTX_STEP    0x01
#define CTX_ROUTE   0x02   /* route allocations to the arena */
#define CTX_DEFER   0x04   /* defer frees of non-arena blocks */
#define CTX_CENSUS  0x08   /* allocation census (observe) */
#define CTX_INIT    0x10   /* inside BattleCore::Init/Setup */
#define CTX_TRACK   0x20   /* remember allocations (Init/Setup) as snapshot candidates */
extern volatile DWORD g_ctx_tid;
extern volatile uint32_t g_ctx_flags;
extern volatile uint64_t g_cur_seq;
extern volatile int g_run_muted;
extern volatile int g_passthrough_active;   /* round 12: a step we pass through untouched (replay pause, online, outside tick) */
/* watch.c (round 8): hardware write watches */
void watch_config(const char *spec);
void watch_arm(void);
void watch_disarm(const char *why);
int watch_veh(EXCEPTION_POINTERS *ep);
void watch_report(const char *why);
long long watch_phase_hits(int dr, int ph);
int watch_armed_threads(void);
static inline int in_ctx(void) { DWORD t = g_ctx_tid; return t && t == GetCurrentThreadId(); }

/* our snapshotted extras (part of every snapshot) */
typedef struct {
    uint32_t lcg;        /* deterministic rand() state */
    uint32_t pad;
    uint64_t rand_calls; /* in-step rand calls (snapshotted so resims count the same) */
} SnapExtra;
extern SnapExtra g_x;

/* spinlock */
typedef volatile LONG Spin;
static inline void spin_lock(Spin *s) { while (InterlockedCompareExchange(s, 1, 0)) YieldProcessor(); }
static inline void spin_unlock(Spin *s) { InterlockedExchange(s, 0); }
static inline int spin_trylock(Spin *s, int spins) {
    for (int i = 0; i < spins; i++) { if (!InterlockedCompareExchange(s, 1, 0)) return 1; YieldProcessor(); }
    return 0;
}

/* memory readability probe (VirtualQuery based, no SEH needed) */
int mem_readable(const void *p, size_t n);

/* region naming for reports */
void describe_va(uintptr_t va, char *out, size_t cap);

/* v0.20.0: thread table (main.c) for the crash logger and the per-battle THREAD census */
void threads_census(const char *why);
DWORD modes_step_tid(void);
void l4_note_ifc(int off, const char *what);
/* v0.20.0 fix R2: set while a forwarded listener call runs on the step thread; mem.c reports the frees made
 * outside the step context during it (they are immediate) so the L4 revert never brings back a pointer to them */
extern volatile int g_l4_in_call;
void l4_note_heap_free(uintptr_t p, size_t sz);   /* L4REENTRY check at every IBattleCore entry */
/* v0.31.0 (OPTA-M2-SPEC.md §20.3): the round-edge work in rbonline. l4_free_wants_size() = 1 while a free's size is still read by someone
 * (L4's first L4F_MAX frees of a call, the lip-sync free list of a resim): mem.c asks UE for the size only then (rbonline; every other mode: 1) */
int l4_free_wants_size(void);
/* the harness controls h_rbo_rs_old (bits; v0.30.0's round-edge rules, one piece at a time): 1 the second rescan at a replay edge, 2 the CPU /
 * SWAY census at every rescan, 4 the first-step diagnostic scan, 8 UE's size of every free in an L4 call, 16 the scan's probe memo off, 32 the
 * pin / itrack filters off (every free takes both locks), 64 the atomic L4 free counter. 0 in the plugin build (RS_OLD() is a constant 0) */
#define RS_OLD_RESCAN2   1
#define RS_OLD_CENSUS    2
#define RS_OLD_FIRSTSCAN 4
#define RS_OLD_GETSIZE   8
#define RS_OLD_MEMO      16
#define RS_OLD_FILTERS   32
#define RS_OLD_L4CNT     64
#define RS_OLD_M2BASE    128   /* v0.32.0 (review m2): the replay-edge baseline taken after the scan (v0.31.0); XRSM2 */
#ifdef SC6RB_HARNESS
extern int g_h_rs_old;
#define RS_OLD(bit) ((g_h_rs_old & (bit)) != 0)
#else
#define RS_OLD(bit) 0
#endif
