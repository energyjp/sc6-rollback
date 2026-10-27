/* v0.23.0-C1b: the rollback session core for mode=rbreplay (v0.24.0-C1b2: and mode=rblive) (C1B-SPEC.md §2): a ghost replay of a C1a recording through the emulated
 * network (emu.c). The remote side's record (P1 word + the 24-byte third block) arrives late; frames run on predictions and are rolled
 * back when the real record arrives. Everything is keyed by the C1a battle step f (the .rec F number).
 * This header also declares the modes.c internals rb.c uses (one place, so rb.c reuses the C0/C1a primitives instead of copies). */
#pragma once
#include "common.h"
#include "snap.h"
#include "c1a.h"

/* ------------------------------------------------------------------ modes.c internals (shared with rb.c) */
typedef struct { uint64_t p1, p2; uint8_t extra[0x18]; uint8_t flags; } FrameIn; /* flags: 1 p1, 2 p2, 4 extra */
#define FP_MAXR 64
#define FP_BYTES 4096
typedef struct { int n, overflow; uint32_t used; uintptr_t addr[FP_MAXR]; uint16_t len[FP_MAXR]; uint8_t bytes[FP_BYTES]; } FPatch;
#define RB (CTX_STEP | CTX_ROUTE | CTX_DEFER)

void     run_fi(FrameIn *f, int muted, uint64_t seq, uint32_t flags);
size_t   fp_capture(FPatch *fp, const Snap *ref);
void     fp_apply(const FPatch *fp);
void     pk_save(void);
int      pk_apply(uint64_t s);
void     do_pin_scan(const char *why, int apply);
void     seg_build(void);
SnapSet *modes_ssw(void);                /* the live snapshot set (writable) */
Spin    *modes_pin_lock(void);
uint64_t modes_lsn_c0_calls(void);       /* UE listener +0x0c0 calls forwarded from live passes (the setup barrier counter) */
int      modes_cam_fail(void);           /* CAMPIN FAIL is active */
int      modes_ring_size(void);          /* the C0 ring size formula (g_R) */
int      modes_pinpool_count(void);      /* v0.24.0-C1b2 S1BFIX G3: distinct pinned blocks RBPINPOOL named this battle */
void     modes_l4kept_arm(int on);       /* C1B3 fix: record the core .bss words L4 keeps (L4DANGLE) during an RPB pass */
int      modes_l4kept_reapply(int *kept, int *overflow);   /* C1B3 fix: after the RPB's scratch restore: UE's value back where the present points into the freed block */
extern volatile uint64_t g_lp_extra_fwd; /* listener slots (bit = offset / 8) a MUTED pass still forwards (rb: the state class) */
extern volatile int g_hk_entry_valid; extern volatile uint64_t g_hk_entry_qpc;   /* hk_step entry time (RBCOST us_core) */

/* ------------------------------------------------------------------ c1a_run.c additions (per pass / per confirmed frame) */
#define C1A_DUMP_SLOTS 2
/* v0.24.0-C1b2 F1: the tier-1 ai / ctl components under the OTHER pointer rule, and the units the two rules normalize differently */
#define C1A_T1U_MAX 8
typedef struct {
    uint64_t c_ctl, c_ai, t1;    /* the other rule's ctl, ai and t1 (t1 with those two substituted) */
    uint32_t ndis;               /* 8-byte units (ai + ctl) the two rules normalize differently */
    int nu; uint32_t uoff[C1A_T1U_MAX]; uint8_t ukm[C1A_T1U_MAX], uko[C1A_T1U_MAX];   /* the first ones: offset from 0x14470f390 (ctl from +0x6070), kind map / old */
} C1aT1Alt;
typedef struct {
    C1aFrame fr;                 /* f, fr, in (the record the pass used), rp, pt, t1, c[], ifc, q */
    int has_t2, nt2; C1aCell t2[C1A_MAXCELL];
    int dump;                    /* -1 none, else the capture slot (c1a_dump_take) */
    C1aT1Alt alt;                /* v0.24.0-C1b2 F1 */
    int mm;                      /* v0.24.0-C1b2 F1.7: -1 none, else the first-mismatch dump slot this pass captured */
    char ps;                     /* v0.24.0-C1b2: the pass type of the frame's last pass (L C R S T; '-' straight modes), set by rb.c */
    uint64_t t15;                /* C2 (C2-SPEC.md §5.1): the KHit world-center tripwire after the pass (F line t15=; never in tier-1) */
    uint64_t t1x;                /* optA M2 (rbonline only): tier-1 minus the D6 exclusions (the third block's core copy), the cross-PC CHECK hash */
    int has_ab;                  /* v0.32.0 (D-B, c1a_clash_bytes=1): ab holds the raw clash / aif spans of both players right after this pass */
    uint8_t ab[2][C1A_AB_LEN];
    uint16_t vm_cv[2][2];        /* v0.32.0 review fix B1 (src/vm.c): var 0x40 / 0x41 of both players right after this pass (the D-F arm on confirmed state) */
} C1aPass;
uint32_t c1a_next_f(void);                                   /* the f the next c1a_step_begin gives */
uint32_t c1a_rec_n(void);                                    /* the recording's frame count (0 = none) */
const C1aFrame *c1a_rec_frame(uint32_t f);                   /* 1..n, NULL outside */
int      c1a_play_ok(void);                                  /* setup accepted, injecting */
int      c1a_play_refused(void);                             /* SETUP_MISMATCH / NO_SETUP_DUMP (not NO_FILE / BAD_FILE) */
void     c1a_cur_ifc(char *ifc, size_t cap, uint32_t *q);    /* the UE->core calls c1a_step_begin collected for this step */
/* t1 (+ t2 when f % 32 == 0, + a dump capture when f % c1a_dump_every == 0) of the state right after a pass of frame f; us = diag time */
void     c1a_pass_compute(uint32_t f, const C1aIn *used, const char *ifc, uint32_t q, C1aPass *out, double *us);
void     c1a_pass_drop(C1aPass *p);                          /* releases its dump capture slot */
/* the confirmed frame f: the F line (+ " sfx" after q=), the comparison with the recording, T2 (+ comparison), the D line and dump file */
void     c1a_confirm(C1aPass *p, const char *sfx);
void     c1a_step_rt(void);                                  /* real-time per step: the U line, the personality watch, title / flush pacing */
int      c1a_t2_comparable(void);                            /* the recording's t2rule equals this run's */
uint32_t c1a_t2_points(void);
uint32_t c1a_t1mm(void);
/* v0.24.0-C1b2 */
uint32_t c1a_t1norm(void);                                   /* C1A T1NORM frames (not mismatches) */
uint32_t c1a_t1norm_rescue(void);                           /* v0.24.0-C1b2 fix: the T1NORM frames of branch why=rescue */
uint32_t c1a_t1norm_oldrule(void);                          /* v0.24.0-C1b2 fix: the T1NORM frames of branch why=oldrule */
uint32_t c1a_mm_written(void);                               /* first-mismatch dumps written this battle */
int      c1a_demo_battle(void);                              /* this battle's Init ran with UE's demo seed (C1A_DEMO_SEED) */
void     c1a_set_cur_in(const C1aIn *in);                    /* rblive: the record a straight pass ran (the F line of c1a_step_end) */
uint32_t c1a_t1map_bad(void);                                /* C1A T1MAP_BAD units at this battle's first step */
uint32_t c1a_rec_frames(void);                               /* frames recorded / confirmed this battle */
uint64_t c1a_battle_no(void);                                /* C2 fix r1: the C1a battle number (the _b<n> of the .rec; counts the demo battle) */
uint64_t rb_battle_no(void);                                 /* C2 fix r1: the rb battle number (RBEND / RBKC b=) */

/* ------------------------------------------------------------------ rb.c */
extern volatile int g_rb_mem_on;         /* the quarantine/pin-death hooks in mem.c are live (rbreplay battle, rb on) */
int  rb_cfg_check(void);                 /* modes_init (mode=rbreplay): 1 = accepted; 0 = CFG C1B REFUSE (mode off) */
void rb_init(void);
int  rb_on(void);                        /* rb runs this battle (not refused, not aborted, not ended) */
void rb_step(void *iface, void *gin, FrameIn *fi, uint64_t seq);   /* hk_step dispatch for MODE_RBREPLAY */
void rb_passthrough_step(void);          /* hk_step: a step outside the manager Tick in rbreplay */
void rb_step_entry_pins(void);           /* hk_step, before the re-pin triggers: quarantined pins hidden for the step's frame */
int  rb_ring_clear(const char *why);     /* ring_clear in rbreplay with rb on: 1 = handled (no C0 ring clear) */
void rb_note_repin(const char *why);     /* a re-pin trigger ran do_pin_scan (stable merge): RBBARRIER cause=repin:<why> at s-1 */
void rb_note_rebuild(const char *why);   /* ss_build rebuilt the snapshot set (RBPINS rebuild) */
void rb_note_extend(int old_n, int new_n, int appended, int vanished);
void rb_battle_begin(void);              /* modes_on_init(before) */
void rb_battle_setup(void);              /* modes_on_setup(before) */
void rb_battle_end(void);                /* modes_on_shutdown(before) */
void rb_title(char *out, size_t cap, const char *l4, const char *l3, const char *cam);   /* the title in rbreplay */
void rb_straight_mark(uint32_t f);       /* mode=record / replay: the live pass of C1a frame f runs (0 = ended): RBSTATEEV pass=straight */
int  rb_ss_stable(void);                 /* ss_build: build the snapshot set in the stable layout (mode=rbreplay) */
uint32_t rb_layout_gen(void);
/* listener hooks (modes.c lsn_pre): 1 = mute this call (skip UE, rax/xmm0 = 0). v0.33.0: + rdx (the presentation slots' payload, §22.1) */
int  rb_lsn_pre(int slot, uint64_t rdx);
/* v0.33.0 (OPTA-M2-SPEC.md §22.1): modes.c delivers n presentation notifications (slot index, the 0x30-byte payload copy) to UE's real listener between
 * passes, each inside L4's guard when guard = 1 (the core .bss revert, the kept listener pointer, L4DANGLE, L4REENTRY). Returns the calls made. */
int  modes_lsn_deliver(int n, const uint8_t *slots, uint8_t (*pay)[0x30], int guard);
/* mem.c free paths (outside the step context, rb on): 0 = not ours (normal path), 1 = quarantined (do not free), 2 = pin marked dead
 * (free now, no pins_note_free) */
int  rb_mem_free(uintptr_t p, int crt, int deferred_hit);
void rb_pinf_rebuild(const SnapSet *ss);   /* v0.31.0 (rbonline): the step thread's pin filter after a pin-set merge (pin lock held; rb.c) */
uint64_t rb_pinf_skips(void);              /* v0.31.0: the frees it answered (RB EDGE) */
#ifdef SC6RB_HARNESS
extern int g_h_rs_pinf_plant;              /* v0.31.0 harness plant (XRSPINF) */
#endif
void rb_mem_realloc(uintptr_t p);        /* a realloc / crealloc of a pinned block (immediate path): dead + RBBARRIER pin_realloc */
void rb_pin_freed_confirm(uintptr_t p);  /* pins_note_free from a confirmed deferred free (rb on) */
void rb_def_note(uintptr_t p);           /* v0.23.1: mem.c deferred a free inside a pass (step thread): a pinned block's pin is hidden for later frames */
int  rb_test_tick_reruns(void);          /* harness: re-run operations in the current tick */
void rb_tick_info(uint64_t *tick, int *step_in_tick);   /* C1B3: the current tick id and the step's position in it (1 = first) */
int  rb_t_old_c0count(void);             /* C1B3: harness-only h_old_c0count (0 in the plugin build) */
void rb_header_keys(char *out, size_t cap);
void rb_header_keys2(char *out, size_t cap);   /* v0.24.0-C1b2: " zone=1 zone_mode= zone_gap= zone_margin=" (rb_zone=1) */
void rb_pindeny(uintptr_t addr, uint32_t len, uint32_t caller);   /* v0.24.0-C1b2 S1BFIX G2 (modes.c pin scans) */
/* C2 (netsess.c) */
int  rb_zone_of(uint32_t f);              /* frame f is a lockstep zone frame (the decided intervals) */
uint32_t rb_last_step(void);
uint32_t rb_confirmed(void);              /* C (the harness waits for the confirmation of a battle's last frame) */
/* optA M2 (rbonline): rb.c's armed-online source (the game's own ring through src/rbo_fetch.c) */
void rb_abort_ext(const char *cause);     /* an abort requested from outside rb.c (rbonline: the snapshot set could not be rebuilt) */
uint32_t rb_rbk(void);                    /* rollbacks this battle */
uint32_t rb_dmax(void);                   /* the deepest rollback this battle */
void rb_dhist_str(char *out, size_t cap); /* "d:n,..." */
void rbo_core_confirmed_frame(uint32_t f, const C1aPass *p);   /* src/rbo_core.c: a confirmed armed frame (outside a zone): the CHECK, the dump record */
void rbo_core_zone_frame(uint32_t f);   /* src/rbo_core.c (v0.32.0 review fix M1): a confirmed armed zone frame: the open CHECK chain is sent at its last frame */
void rbo_core_note_rollback(uint32_t s, uint32_t r, int depth, const char *cause);   /* src/rbo_core.c: RB ROLLBACK */
/* v0.29.1 (review B2): an abort of the armed core is never silent. rb.c reports it (instead of a bare drain) with what it knows: the first
 * committed frame whose run word is already known to differ from the truth (0 = none known), how many frames it committed unconfirmed
 * (each is verified later, when its truth arrives); and every frame that runs or was committed without a correcting core is verified. */
void rbo_core_aborted(const char *cause, uint32_t f, uint32_t unconfirmed, uint32_t mm_f, uint64_t mm_used, uint64_t mm_truth, uint32_t unverified);
void rbo_core_committed_mispredict(uint32_t f, uint64_t used, uint64_t truth);   /* a committed frame's run word != its truth (no core to correct it) */
int  rb_is_aborted(void);                 /* the rb battle of this mode is RB_ABORTED (rbonline: the drained FetchInput, the lost-frame rule) */
void rb_unverified_stats(uint32_t *pending, uint32_t *verified, uint32_t *lost, uint32_t *dropped);
int  rb_is_on(void);
double rb_p99_core(void);                  /* the largest per-depth p99 core cost (us) of this battle so far */
void rb_p99_by_depth(char *out, size_t cap);   /* "d:us,..." */
/* C2 (c1a_run.c) */
void c1a_net_setup(uint64_t *setup_x, uint64_t *setup_full, uint64_t trees[4]);   /* §2.4 SETUP hashes (x masks the CPU-setup spans and type words) */
int  c1a_live_views(C1aView v[C1A_NRANGE]);
void c1a_dump_frame_add(uint32_t g);      /* a full dump at g's confirmed pass (the DESYNC / KHITDIFF dumps) */
uint64_t modes_t15(void);                 /* modes.c: the KHit world-center tripwire hash (§5.1) */
void modes_l4_times(double *l4_us, double *l4miss_us);   /* modes.c: cumulative L4 / L4MISS microseconds (C2 §5.3 cost tail) */
/* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16): the Q1 root walk and the rbreplay hybrid (rb_arm_f, rb_l3stock, rb_init_pins, q1_roots, q1_cov) */
int  modes_q1_sig(void);                  /* modes.c: the signature gate, run once (logs Q1 SIG ok= rows= bad=): 1 = every row of src/rbo_q1_table.h matches */
int  modes_q1_rows(void);                 /* the table's rows (Q1SIG + Q1PTR + Q1VT; v0.29.3: + Q1PTR) */
const char *modes_q1_fail(void);          /* "<E#>:<why>" of the last failed walk, NULL = none (rbonline: RB ARM ... DEGRADED ... q1=) */
void modes_q1cov_step(uint32_t f, int phase);   /* rb.c (rbreplay q1_cov=1): 0 = step entry, 1 = before the live pass (after a rollback), 2 = after it */
void modes_q1cov_report(void);            /* rb.c rb_finish: the Q1COV lines */
void rb_q1_walk_failed(void);             /* modes.c (rbreplay): a walk failed a gameplay entry: RBABORT cause=q1_walk now, or at battle start */
