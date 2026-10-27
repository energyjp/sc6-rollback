/* LIPSYNC: v0.25.0-C1b3 PART B (C1B-R3-SPEC.md §3; C1B-R2-SPEC.md §3; REVERT-LIPSYNC.md). Lip-sync in the rollback modes, behind ONE ini
 * key: lipsync=0|1 (code default 0; absent = 0). Every entry point returns at its first statement while the key is 0, so the lipsync=0 build
 * behaves exactly like the pre-lip-sync PART A build (proof: tools/lipsync_ident.py).
 *
 * What it does with lipsync=1 in rbreplay / rblive. The P table is pres.c k_pres_p = KEEP since v0.25 (C1B-R3-SPEC.md §2.4, §3.2): face_w,
 * face_layers, lipsync_ctl, voice_ctl; 8 ranges / 3,872 bytes for both players. The world-space face rows of the bone bank and face_a / face_c /
 * face_d are POSE: they roll back like G, so the resim face is computed from the resim head (the round-2 face stretch, second source).
 *   - §3.2 keep: inside L4 (l4=1), UE's writes into KEEP made by a forwarded listener call of a LIVE pass stand (copied after UE's call, put
 *     back byte-exact after the unchanged revert). NONP: a KEEP range whose call also wrote within 64 bytes outside KEEP is reverted whole.
 *     Writes of muted passes (confirmed resims, RPB) stay reverted (counted resim_p_reverted).
 *   - §3.3 KEEP never rolls back: every snap_restore_ex in rb.c is wrapped by lipsync_before_restore / lipsync_after_restore (the KEEP bytes
 *     copied right before that restore are written back right after it). fp_cb skips KEEP spans (their content is already live).
 *   - LIPSYNCGUARD: at the start of the first pass after each hooked restore, KEEP is compared with the before-restore copy; a mismatch is
 *     LIPSYNCGUARD FAIL, then rb.c aborts the rollback (RBABORT cause=lipsync_guard; title lips=FAIL).
 *   - Watchdog (informational): the lpd bases [PLAYER+0x95ec0] / [PLAYER+0x95f50] changing mid-battle (LIPSYNC lpd_changed), and the
 *     pointer fields +0x95ed8 / +0x2b270 not 0, not an image address and not committed (LIPSYNC ptr_bad).
 *   Round-2 fix (2026-10-06), kept:
 *   - the core-written spans +0x292f0..+0x292f7, +0x29304..+0x29307, +0x29690..+0x296af roll back with the restore. Since v0.25 they lie in
 *     POSE (face_a / face_d), outside KEEP: build_segs asserts that k_core_w intersects KEEP in 0 bytes (CFG LIPSYNC core_bytes=0; a nonzero
 *     intersection with the plugin's table refuses lip-sync loudly: CFG LIPSYNC REFUSE why=core_in_keep);
 *   - pres_keep never puts back a live KEEP word that points into a block the step thread freed inside a forwarded call during the
 *     rollback's resims: it keeps the resim word (LIPSYNC pk_freed, a FINDING; harness LSDM / LSDMX / LSDM0);
 *   - PRESKEEPD with lip-sync on: every KEEP granule is by design (counted, LIPSYNCSTAT pkd_bydesign=); other granules are logged with a
 *     budget of 60 lines per block (only reachable with the harness's old tables).
 *   v0.25 (C1B-R3-SPEC.md §3.3): the round-1 lip-sync DIAGNOSTIC LIPSYNCEV (no fix; round-1 mouths are predicted NOT to move):
 *     ev=keep  one line per kept call and player (lipsync_l4_keep_end), the after-values;
 *     ev=fp    one line per frame and player when a between-step UE write touches KEEP (lipsync_fp_run);
 *     ev=frame the 30 frames after each event (per player, read at the start of each NEW frame's pass): the first and the last frame of the
 *              window and every frame whose fields (or mouth moving / still) changed, with mouth= (this frame) and mmax= (max since the
 *              previous line of the window);
 *     an ev=keep / ev=fp whose kind, slot and fields equal the player's previous one is counted (ev_same=), not logged;
 *     budget 300 lines per battle (harness-only h_lips_ev_budget), split so that rounds 1 and 2 both get lines: a segment starts at the
 *     battle's begin and at each NEW frame whose battle mode id (0x144846364, the zone reader's) becomes 1 (the round call); segments 0..2
 *     (intro, round 1, round 2) may use budget/3 lines each (harness control h_lips_skip_site=evseg: one shared budget). LIPSYNCSTAT
 *     + ev_lines= ev_dropped= ev_same=.
 * This module is plugin-only (it is not part of CORE_SRCS); config.c parses the key into Config without calling it.
 * v0.29.4 FACES (design_v0294/faces/DESIGN.md §3): mode=rbonline accepts lipsync=0|1 too. rb_cfg_check (process start) builds the KEEP blocks
 * and logs CFG LIPSYNC on=1 mode=rbonline ... gate=armed, but leaves the module OFF; lipsync_battle_begin turns it on (rbonline calls
 * rb_battle_begin only from modes_rbo_arm, i.e. at ARM) and lipsync_battle_end turns it off again after LIPSYNCSTAT (modes_rbo_end, at END).
 * So PASSIVE / PROBE / PAIRED battles never run any of it (every entry returns at its g_on gate), and the KEEP / POSE rule, the restore
 * hooks and the guard are the rbreplay ones, unchanged. The face family stays out of tier-1 and CHECK; nothing of it crosses the wire. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { LIPS_SITE_ROLLBACK = 0, LIPS_SITE_RPB = 1, LIPS_SITE_RPB_SCRATCH = 2, LIPS_SITE_FP = 3,
       LIPS_SITE_CORE = 4, LIPS_SITE_PK = 5, LIPS_SITE_PKD = 6,   /* fix: harness-only control names (h_lips_skip_site=core|pk|pkd) */
       LIPS_SITE_EVSEG = 7, LIPS_SITE_PKDV024 = 8,                 /* v0.25: h_lips_skip_site=evseg (one shared LIPSYNCEV budget) | pkd_v024 */
       LIPS_SITE_N };

int  lipsync_on(void);                         /* lip-sync runs (lipsync=1 accepted by rb_cfg_check) */
void lipsync_set_active(int on);               /* rb.c rb_cfg_check: logs CFG LIPSYNC on=1 when on */
void lipsync_battle_begin(uint64_t b);         /* rb.c rb_battle_begin (after the battle counter moved) */
void lipsync_battle_end(uint64_t b);           /* rb.c rb_battle_end: LIPSYNCSTAT why=battle_end */
void lipsync_before_restore(int site);         /* rb.c: right before a snap_restore_ex */
void lipsync_after_restore(int site);          /* rb.c: right after it */
void lipsync_pass_start(uint32_t k, uint32_t s);   /* rb.c pass_run: the guard (first pass after a hooked restore), the watchdog, LIPSYNCEV frames */
int  lipsync_guard_tripped(void);              /* rb.c: 1 once after a LIPSYNCGUARD FAIL (the caller aborts the rollback) */
/* modes.c l4_post (l4=1): begin after UE's call and before the revert (pre = the pre-call copy of core .bss at bss_lo); end after it */
void lipsync_l4_keep_begin(int slot, int resim, const uint8_t *pre, uintptr_t bss_lo, size_t bss_len);
void lipsync_l4_keep_end(void);
int  lipsync_fp_skip(void);                    /* modes.c fp_cb: KEEP spans are skipped */
/* fix (C1b round 2 review): */
int  lipsync_free_wants(int resim);
int  lipsync_rbo_fail(void);         /* v0.31.0: the title's lips=FAIL (the badge shows it too) */   /* v0.31.0: whether lipsync_note_free would keep the size (modes.c l4_free_wants_size) */
void lipsync_note_free(uintptr_t p, size_t sz, int resim);   /* modes.c l4_note_heap_free: a step-thread free inside a forwarded call (resim = a muted pass) */
void lipsync_pk_fix(uint8_t *live, uint64_t s);   /* modes.c pk_apply, before pres_keep_apply: never put back a pointer into a block freed in the resims */
int  lipsync_pkd_note(uint64_t s, const uint8_t *live, const uint8_t *diff, int n);   /* modes.c pkd_note: PRESKEEPD with lip-sync on (1 = handled) */
int  lipsync_fp_run(uintptr_t a, size_t len, size_t *run);   /* 1: [a, a + *run) is KEEP (ev=fp noted); 0: not KEEP (*run >= 1) */
void lipsync_title(char *out, size_t cap, int fight);        /* modes.c: " lips=on|FAIL|REFUSED" appended while a fight runs */
/* v0.29.4 FACES (design_v0294/faces/DESIGN.md §3): rbonline accepts lipsync=1; lip-sync runs only from ARM (rb_battle_begin, called by
 * modes_rbo_arm) to END (rb_battle_end, called by modes_rbo_end) of each armed battle; PASSIVE / PROBE / PAIRED never reach it. */
void lipsync_rbo_title(char *out, size_t cap);   /* rbo.c rbo_title_token (M1's writer thread): " lips=on|FAIL" while an armed battle runs it */
int  lipsync_h_force_off(void);                  /* rbo_core.c rbo_core_force: harness control h_lips_rbo_force_off (the v0.29.3 forcing); 0 in the plugin */
/* v0.25: defined in rb.c (a LIPSYNC: insertion): the name of the pass running on the step thread (LIPSYNCEV pass=) */
const char *rb_lips_pass_name(void);
