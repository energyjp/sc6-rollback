#pragma once
#include "common.h"
#include "arena.h"
#include "defer.h"
#include "snap.h"

extern Arena g_arena;
extern Spin g_arena_lock;
extern int g_arena_ok;
extern volatile int g_route_broken;     /* an allocation needed routing but the arena failed */
extern volatile int g_defer_broken;     /* deferred-free table overflowed */
extern void *g_gm_obj;                  /* GMalloc instance */
extern int g_mem_hooked;

int  mem_install(void);                 /* patch the GMalloc vtable (call on the game thread) */
int  arena_setup(int mb, int fixed);   /* v0.22.0-C1a: fixed=1 tries 0x7e0000000000 first (g_arena_fixed says whether it was granted) */
extern int g_arena_fixed;
int  mem_install_crt(void **iat_malloc, void **iat_free, void **iat_realloc, void **iat_calloc);
/* optA M2 (rbonline): put the originals back (only where the slot still holds our hook); 1 = every slot restored */
int  mem_uninstall(void);
int  mem_uninstall_crt(void **iat_malloc, void **iat_free, void **iat_realloc, void **iat_calloc);
/* optA M2 v0.29.1 (review B1): rbonline's allocator front doors (mem.c; the doors and cells are src/rbo_thunks.S's, rbo_core.c switches them) */
int  mem_doors_install(void *const door[8], void **iat[4], void *orig[8], void *hk[8], void *volatile cell[8]);
/* blocks allocated inside BattleCore::Init/Setup (snapshot candidates) */
int  itrack_init(void);
int  itrack_list(SPin *out, int max, uint64_t *bytes_out);
void itrack_clear(void);
uint64_t itrack_filter_skips(void);   /* v0.31.0 (§20.3): the frees the itrack filter answered since the last itrack_clear (RB EDGE) */
int  itrack_contains(uintptr_t p, uint32_t *caller, int *kind);
void itrack_report(const char *why);
int  mem_deny_rva(uint32_t rva, int gate);   /* S1BFIX: the UE TaskGraph small-task allocator ranges (gate 1 = G1 tracking, 2 = G2 pin gate) */
extern volatile uint64_t g_it_adds;   /* tracked-block insertions (pins are refreshed when it moves) */
/* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.4): the itrack generation stamp (every insertion carries the current generation) */
typedef struct { uintptr_t p; uint32_t size, caller; uint8_t kind; uint32_t gen; } ItInfo;   /* kind: 0 fmalloc, 1 crt (inside a step context), 2 / 3 outside */
uint32_t itrack_gen_bump(void);   /* a new generation (returned): entries tracked from now on carry it */
uint32_t itrack_gen(void);
int  itrack_list_ex(ItInfo *out, int max, uint32_t gen_min, uint64_t *bytes_out);   /* the live entries with gen >= gen_min */

/* deferred frees */
int  def_count(void);
void def_drop_from(uint64_t seq);
int  def_confirm_before(uint64_t seq);  /* really frees entries with seq < seq; returns count */
int  def_confirm_all(void);
void def_save(void);
void def_restore(void);
void mem_free_now(uintptr_t p, int crt);   /* v0.23.0-C1b: the real free of a quarantined block (rb.c) */

/* allocation census (observe mode, inside the step) */
typedef struct {
    uint32_t n_alloc, n_free_step, n_free_pre, n_realloc, n_arena_alloc, n_arena_free, n_defer, n_crt_alloc, n_crt_free;
    uint64_t bytes_alloc, bytes_free_pre;
} StepCensus;
extern StepCensus g_sc;
void census_step_begin(void);
void census_step_end(uint64_t seq);
void census_battle_reset(void);
void census_report(const char *why);
void mem_stats_log(const char *why);

/* callbacks implemented in modes.c */
void pins_note_free(uintptr_t p);       /* a pinned heap block was freed (outside our control) */
extern volatile int g_npins;
