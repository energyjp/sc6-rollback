/* v0.22.0-C1a runtime (plugin only): mode=record / mode=replay (GATE-C-DESIGN §8 C1a; audit/c1a/RECON.md).
 * modes.c runs the step (straight live passes, L3, L4, routing, the camera block pinned as in roll7) and calls these hooks;
 * main.c calls c1a_seed_for_init from the Init hook. */
#pragma once
#include "common.h"
#include "c1a.h"
#include "snap.h"

void c1a_init(void);                                   /* modes_init: mode, timestamp, refusal set, the replay file */
int  c1a_active(void);                                 /* 1 record, 2 replay, 0 neither */
/* main.c hk_init: 1 = Init must run with *used instead of UE's seed (replay with a loaded recording; v0.22.1: never the menu demo
 * Init, UE seed C1A_DEMO_SEED) */
int  c1a_seed_for_init(uint32_t ue_seed, uint32_t *used);
void c1a_battle_begin(uint64_t battle);                /* modes_on_init(before) */
void c1a_after_init(uint32_t seed_orig, uint32_t seed_used, int overridden);   /* main.c after modes_on_init(after): the LCG reset */
void c1a_crt_note(int is_srand);                       /* v0.22.1 main.c hk_rand / hk_srand: a CRT call that did not take the LCG (C5) */
void c1a_ifc_pre(int slot);                            /* v0.22.1 modes.c census thunk hooks: TT800 around IBattleCore +0xc8 (C3) */
void c1a_ifc_post(int slot);
void c1a_note_setup(void);                             /* modes_on_setup(before): counted as slot +0x60 in the next ifc= */
void c1a_battle_end(void);                             /* modes_on_shutdown(before): END line, close the files */
/* per step: begin before the step pass (ifc, the first-step setup hash, the input to use), end after it (checksums, files) */
int  c1a_step_begin(uint64_t seq, const C1aIn *ue, C1aIn *use, int passthrough);   /* 1 = *use is the recorded input */
void c1a_step_end(uint64_t seq);
void c1a_title(char *out, size_t cap);                 /* " rec=<n>" | " play=<state>[ t1mm=<n>]" */
void c1a_title2(char *out, size_t cap, int with_t1mm);   /* v0.23.0-C1b: rbreplay shows its own t1mm (confirmed frames) */
void c1a_flush_crash(void);
void randx_report(const char *why);                    /* main.c: RANDX summary (record/replay only) */

/* modes.c accessors (the snapshot set, the class map) */
const SnapSet *modes_snapset(void);
int  modes_ss_ready(void);
typedef void (*modes_seg_cb)(void *u, int region, size_t off, size_t len, char cls);
void modes_seg_iter(modes_seg_cb cb, void *u);         /* the class segments of the snapshot regions (G C F R O) */
