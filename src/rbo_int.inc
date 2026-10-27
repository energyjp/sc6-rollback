/* Option A M2 (rbonline): the rollback core's INTERNAL declarations, shared by src/rbo.c, src/rbo_body.c, src/rbo_fetch.c, src/rbo_core.c and
 * src/rbo_install.c. Part of the rollback core. Not an interface for the other modules (they use src/rbo.h). No float types here: rbo_body.c is
 * compiled with -mgeneral-regs-only. */
#pragma once
#include "rbo.h"
#include "rbo_logic.inc"

/* ------------------------------------------------------------------ what a passive body read (rbo_body.c fills it; rbo.c decides) */
typedef struct {
    uintptr_t mgr, obj;
    int mgr_ok, obj_ok, cls;            /* cls: 2 = InputSync (online), 1 = InputLog (offline), 3 = other / unreadable, 0 = none */
    uint32_t bonline, onlinesync, state, result, remote, local, players;
    uint32_t round, gt, synctime, sendtime, pausetime, lag, lagmin, lagmax;
    uint32_t phase, m12f8, mgrstate;
    uint32_t rtt_bits;                  /* v0.30.0: +0x4408, the game's RTT (a float, ms) as raw bits (the time-sync report's log field) */
} RboObs;

/* ------------------------------------------------------------------ rbo_body.c: passive game reads for rbo.c (gptr, VirtualQuery checked) */
int  rbo_read_obj(uintptr_t obj, RboObs *o);         /* an InputSync method's `this`: 1 = readable and of the InputSync class (o filled) */
int  rbo_read_opp_id(uint64_t *id);                  /* the session's opponent id (never logged): 1 = read, 0 = no live session */
/* v0.29.7 (OPTA-M2-SPEC.md §18.10): the opponent's SteamID candidates, in source order (rbo.h RBO_PSRC_*), each id once (a later source's
 * equal id is counted in n_src but not repeated). ids / srcs: cap entries. n_src[RBO_PSRC_N]: what each source found (index = source).
 * old_rule = 1: only RBO_PSRC_RAW (v0.29.6's read; the harness control). Returns the number of candidates (0 = no live session). Reads only,
 * bounded; never logged. The individual-SteamID type check is the caller's. */
int  rbo_read_opp_cands(uint64_t *ids, uint8_t *srcs, int cap, uint32_t *n_src, int old_rule);
void rbo_read_content(uint64_t *digest, uint32_t n[2], uint32_t *walk_ok);   /* the content digest (move-record trees + KHit lists; v0.29.9: base-independent) */
/* v0.29.9 (OPTA-M2-SPEC.md §18.12): the HELLO digest hashes per KHit node the list, the class as a vtable RVA and the AUTHORED fields only (the
 * same per-node values as the v0.29.8 'auth' sub-digest, with the Sphere's authored local center +0x40.. in place of its live copy +0x30..),
 * in list order, then each list's count; the tree part and walk_ok as before. rbo_cdiag.digest_v9 = that value; rbo_cdiag.digest_v8 = what
 * v0.29.7 / v0.29.8 sent for the same read (a diagnostic: RB CONTENT v8= and the dump's H line); pl[p].ka[q] = list q's part of the HELLO
 * digest (cd_auth per node from a fresh basis, then the count). */
#ifdef SC6RB_HARNESS
extern int rbo_h_content_old;                        /* h_rbo_content_old=1: the HELLO carries digest_v8 (a v0.29.8 build; XCDBASE, PROTO5) */
#endif
/* v0.29.8 (OPTA-M2-SPEC.md §18.11): the content digest's diagnosis (v0.29.8 left the digest as v0.29.7's; v0.29.9 replaced it, above, and
 * keeps all of this); every call also fills rbo_cdiag.pl[] with sub-digests that never go on the wire, and, when rbo_cd_capture is set
 * (hello_build only) and the previous capture was written, the raw capture behind them: game data only (the move-record keys in walk order,
 * each KHit node's class and bytes +0x00..+0x9f). rbo.c's writer thread turns them into the RB CONTENT lines and the per-battle dump file
 * rbcontent_<ts>_<pid>_b<n>.txt, then clears busy. Per list q: kvt = the vtable RVAs, kspan = the hashed span (+0x7c..+0x97, as the digest),
 * kreb = that span with every 8-aligned qword (+0x80 / +0x88 / +0x90) that is an image address replaced by its RVA, kabs = how many were;
 * auth = the authored hitbox fields the deserializer writes from the move data (common header, local points, radius, bone indices), all
 * lists, in walk order. */
#define RBO_CD_KEYS  2048                            /* move keys captured per player (S-B's largest tree: 869) */
#define RBO_CD_NODES 768                             /* KHit nodes captured per player */
#define RBO_CD_NB    0xa0                            /* bytes captured per node (+0x00..+0x9f: a Sphere's 0x80 + the next 0x20) */
typedef struct {
    uint32_t read, tree_sz, tree_n, tree_ok, list_ok[3], kn[3], kabs[3];
    uint64_t tree_h, kvt[3], kspan[3], kreb[3], auth;
    uint64_t ka[3];                                  /* v0.29.9: list q's part of the HELLO digest (authored fields per node, then the count) */
} RboCdPl;
typedef struct { uint8_t q, rsv; uint16_t idx; uint32_t vt_rva; uint64_t node; uint8_t b[RBO_CD_NB]; } RboCdNode;
typedef struct {
    RboCdPl pl[2];
    uint32_t toggle, toggle_ok;                      /* the KHitArea double-buffer toggle 0x14470dec4 at the read */
    volatile LONG busy;                              /* 1 = a capture waits for the writer (set here, cleared by rbo.c's writer) */
    uint32_t captured;                               /* the last call captured */
    uint64_t digest_v9, digest_v8;                   /* v0.29.9: the last read's HELLO digest and v0.29.8's digest of the same read */
    uint32_t nkeys[2], nnodes[2], trunc_keys[2], trunc_nodes[2];
    uint32_t keys[2][RBO_CD_KEYS];
    RboCdNode nodes[2][RBO_CD_NODES];
} RboCdiag;
extern RboCdiag rbo_cdiag;
extern int rbo_cd_capture;                           /* set by hello_build around its rbo_read_content call (game thread) */
extern RboCtxAcc rbo_ctx_acc;                        /* the Step-entry context measurement (P4 body, while rbo_ctx_on) */
extern volatile int rbo_ctx_on;

/* ------------------------------------------------------------------ rbo.c: the state machine, called from the bodies (game thread) */
extern volatile LONG rbo_off;                        /* the install failed: bodies return at once (M1 still observes) */
extern volatile LONG rbo_st;                         /* RboState */
extern volatile LONG rbo_probe_off;                  /* config refused / fetch signature bad: no PROBE in this process (v0.29.1: no TAINT any more) */
extern int rbo_remote;                               /* the remote player index (0 / 1) of the armed battle */
void rbo_on_init(uint64_t rule);                     /* P2 IFC Init pre (after M1's body incremented nobs_bat) */
void rbo_on_shutdown(void);                          /* P3 IFC Shutdown pre */
void rbo_on_tick(const RboObs *o);                   /* P1 Tick pre */
void rbo_on_step(NobsCtx *ctx);                      /* P4 IFC Step pre: tripwire + the step target */
void rbo_on_ispaused(const RboObs *o);               /* P8 IsPaused pre: ARM takes effect; the stall lift */
void rbo_on_setstate(const RboObs *o, uint32_t to);  /* P9 SetOnlineState pre */
void rbo_on_rreset(const RboObs *o, uint32_t round); /* P10 per-round reset pre */
void rbo_on_any_body(int hook);                      /* every game-thread body: the post-Shutdown mem restore check */

/* ------------------------------------------------------------------ rbo.c: events -> the rbo writer thread (RB lines) */
void rbo_ev_text(const char *line);                  /* armed / install code: a preformatted RB line (the writer adds nothing) */
void rbo_ev_state(int from, int to, int why);        /* RB STATE (passive-safe: no CRT, no log lock) */
void rbo_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* armed / install: format here, write on the rbo writer */
void rbo_unexpected(const char *kind, const char *detail);   /* RB UNEXPECTED (armed / install code only) */

/* ------------------------------------------------------------------ rbo.c: armed actions and drains (game thread) */
void rbo_drain(int why);                             /* ARMED -> DRAINED (no-op otherwise) */
int  rbo_drained(void);                              /* DRAINED: no prediction, no lift, FetchInput confirmed-only */
void rbo_end(int why);                               /* any -> END */
uint32_t rbo_gw32(uintptr_t a, uint32_t v);          /* an armed game write (counted; returns the old value) */
uint8_t  rbo_gw8(uintptr_t a, uint8_t v);
uint64_t rbo_gw64(uintptr_t a, uint64_t v);
extern int rbo_d, rbo_max_rb;                        /* the agreed values of the armed battle */
extern uint32_t rbo_armed_round;
extern uintptr_t rbo_obj, rbo_mgr;                   /* the armed battle's InputSync object and manager */

/* ------------------------------------------------------------------ rbo_fetch.c: the per-frame record of the armed timeline */
typedef struct {
    uint32_t f;                          /* the rb frame this slot holds (the slot is g_fr[f % RBO_FR_CAP]) */
    uint32_t round, ff;                  /* GameRound, the fetch frame (GameTime - remaining - 1) */
    uint32_t raw[2];                     /* the raw each player's fetch used (remote: the prediction when pred) */
    uint32_t craw;                       /* the remote's confirmed raw (valid when conf) */
    uint64_t word[2];                    /* the step word FetchInput produced (what the live pass ran) */
    uint32_t m12f8; uint8_t c3[2];       /* the masks sampled at the live fetch (reused in resims and for the truth word) */
    uint8_t tm[2];                       /* v0.29.1 (review m4): the stock timer flags of player p's fetch (RBO_TM_*; 0 online) */
    uint8_t pred, repeat, valid, fetched, conf, forced;   /* pred: the remote raw was a prediction; repeat: a 0x1462 step (no fetch);
                                                           * fetched: bit p = player p fetched; conf: the remote raw is confirmed;
                                                           * forced: 1 = committed unconfirmed at a round-reset wall, 2 = lost (v0.29.1,
                                                           * review B2: the core aborted and the frame's ring slot was reset by a newer
                                                           * round before its truth arrived: never verifiable) */
} RboFrame;
#define RBO_FR_CAP 16384                 /* rb frames kept (the ring of the armed timeline; > max_rb + the confirmation lag) */
RboFrame *rbo_frame(uint32_t f);         /* NULL outside the kept window */
uint32_t rbo_fetch_frontier(void);       /* the largest rb frame f such that every frame <= f has its remote raw confirmed */
int  rbo_fetch_init(void);               /* process start: the frame table */
void rbo_fetch_arm(uintptr_t mgr, uintptr_t obj, int remote);   /* ARM: reset the timeline, prev raws from 0x1498 */
void rbo_fetch_step_begin(uint32_t f);   /* rbo_core: the step of rb frame f begins (binds the last fetch to f) */
int  rbo_fetch_confirm_scan(uint32_t upto);   /* the newly confirmed remote frames up to rb frame upto (reads the ring); returns how many */
void rbo_fetch_truth(uint32_t f, uint64_t *local_w, uint64_t *remote_w, int *known);
uint64_t rbo_fetch_pred_word(uint32_t f);   /* the remote word frame f runs on while unconfirmed */
int  rbo_fetch_tripwire(const uint64_t stepin[2], int have[2]);   /* P4: 0 = the Step-entry words equal what FetchInput produced */
int  rbo_fetch_repair(void);             /* the drain repair of 0x1498/0x14a8/0x14b8[remote]; 1 = done */
uint32_t rbo_fetch_force_confirm(void);  /* the round-reset wall: commit every unconfirmed frame as predicted; returns how many */
uint32_t rbo_fetch_mispredicts(void);
uint32_t rbo_fetch_unconfirmed(void);    /* live frames not yet confirmed */
extern volatile int rbo_fetch_mode;      /* 0 predict (ARMED), 1 confirmed-only (DRAINED) */
extern uint64_t rbo_fetch_calls, rbo_fetch_pred, rbo_fetch_miss_drained;
/* v0.29.1: what the armed FetchInput saw that stock online never does (rbo_core.c drains at the next step and clears it):
 * bit 0 = CheckOnlineSessionActive (FUN_1403f2590) returned 0 (review m4), bit 1 = the stock timers applied (not ignored and > 0),
 * bit 2 = a player index outside 0..1 (review m3 e: nothing written for it) */
extern volatile uint32_t rbo_fetch_unexp;
uint32_t rbo_fetch_lost(void);           /* v0.29.1 (B2): frames never verifiable (their ring slot was reset after an abort) */

/* ------------------------------------------------------------------ rbo_core.c: the bridge to the C1b core */
int  rbo_core_init(void);                /* process start: the armed path's allocations (arena, L4 buffer, rb/c1a tables); 1 = ok */
int  rbo_core_arm(uintptr_t mgr, uintptr_t obj, int remote);   /* ARM step (3): mem hooks, the snapshot set, the rb battle; 1 = ok */
void rbo_core_end(const char *why);      /* END: the rb battle's tail, RBEND, the quarantine; v0.29.1: the deferred frees, the doors' cells */
void rbo_core_mem_after_shutdown(uint32_t b);   /* v0.29.1 (B1): the first body after a Shutdown: RB MEM after_shutdown, the pointer cells */
int  rbo_core_doors(void);               /* v0.29.1 (B1): the allocator front doors are installed (since the process's first ARM) */
void rbo_step_ctxonly(void *iface, void *in);   /* v0.29.1 (M2): the degraded arm's step (run_step's live pass: L3-stock, no core) */
void rbo_core_degraded_begin(void);      /* v0.29.5 RBO (F1): the degraded arm: lip-sync on for the battle when modes_rbo_arm did not turn it on */
void rbo_core_degraded_end(void);        /* v0.29.5 RBO (F1): END of a degraded battle: lip-sync off (LIPSYNCSTAT) when the degraded arm turned it on */
void rbo_desync_local(uint32_t f, uint64_t mine, uint64_t theirs, const char *why);   /* rbo.c, v0.29.1 (B2): a DESYNC found on this PC */
void rbo_core_tick(void);                /* Tick pre while ARMED/DRAINED (plugin-side per-tick bookkeeping) */
uint32_t rbo_core_last_f(void);          /* the last rb frame stepped */
uint32_t rbo_core_confirmed(void);
int  rbo_core_zone(uint32_t f);
int  rbo_core_on(void);                  /* the rb core runs (not aborted) */
void rbo_core_summary(char *out, size_t cap);   /* "frames_armed= rollbacks= rb_hist= max_depth=" for RB SUMMARY */
void rbo_core_cost(char *out, size_t cap);      /* "p50= p99= max=" (us per armed step) */
extern void *rbo_o_step;                 /* the original IFC step thunk (the slot value at install) */

/* ------------------------------------------------------------------ cross-TU core plumbing */
int  rbo_runtime_init(void);             /* rbo.c: the event ring + the rbo writer thread (process start) */
void rbo_set_off(const char *gate);      /* rbo.c: the install failed (title rb=OFF:<gate>) */
void rbo_badge_tick(void);               /* v0.31.0 (OPTA-M2-SPEC.md §20.1, §20.2; rbo.c): the badge text + the sound cue (M1's writer thread, ~20 Hz) */
extern const char *g_p2p_api_name;       /* rbo.c: "steam" | "fake" (set by rbo_install) */
void rbo_check_local(uint32_t f, uint32_t round, uint32_t gt, uint64_t hash);   /* rbo.c: a confirmed armed frame's CHECK hash (rbo_core) */
void rbo_check_zone(uint32_t f);        /* rbo.c (v0.32.0 review fix M1): a confirmed armed zone frame (the zone-entry flush of the CHECK chain) */
void rbo_core_dump_frame(const char *path, uint32_t f);   /* rbo_core.c: the DESYNC light dump (tier-1 components of frame f) */
#ifdef SC6RB_HARNESS
void rbo_core_plant_leak(void);          /* rbo_core.c: h_rbo_leak_arena (one arena block survives Shutdown: TAINT) */
#endif
void rbo_core_force(char *csv, size_t cap);   /* rbo_core.c: set the C1b knobs rbonline runs with (g_cfg, in memory); csv = what was forced */
/* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.6) */
extern volatile LONG rbo_q1sig;          /* rbo.c: the Q1 table's signatures differ from this exe (rbo_install): PROBE off, why=q1sig, title rb=OFF:q1sig */
const char *rbo_core_q1_fail(void);      /* rbo_core.c: "<E#>:<why>" when this battle's ARM failed in the Q1 root walk, else NULL */
