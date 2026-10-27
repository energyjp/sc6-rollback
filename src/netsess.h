/* C2 (C2-SPEC.md §2.3-§2.10): the netplay session of mode=rbnet, on the game thread. It owns the handshake (HELLO), the per-battle seed,
 * SETUP / READY barrier, the input tables (the owner's local records with the zone-edge fill / merge, the peer's records and the contiguous
 * frontier F), the CHECK exchange (tier-1 decides DESYNC; t15 / ued are findings), time sync and the stall lever's allowed-step count,
 * pause, timeouts, QUIT / ABORT, the desync dumps and the title tokens. The rollback core (rb.c) calls it per step; main.c per tick, at
 * Init / Setup / Shutdown and from the Advance hook. The net thread (net.c) never touches game memory. */
#pragma once
#include "common.h"
#include "c1a.h"

/* startup (plugin_init after modes_init, mode=rbnet accepted): the socket, the net thread, the modset2 thread; logs NETCFG / NETUP */
void ns_start(void);
int  ns_active(void);                 /* mode=rbnet accepted and started */
/* main.c hk_tick: entry (before o_tick: drain the rx ring once, process, timers, pause edge) and exit (after o_tick: send) */
void ns_tick_begin(void *mgr);
void ns_tick_end(void);
/* main.c hk_init: the seed of a non-demo Init (1 = override with *used); then the barrier arm after Init */
int  ns_seed_for_init(uint32_t ue_seed, uint32_t *used);
void ns_on_init(uint32_t ue_seed);
void ns_on_setup(void);
void ns_on_shutdown(void);
/* main.c: the Advance hook, after the original returned: the stall lever (C2-SPEC.md §2.7) */
void ns_advance_after(void *inputlog);
/* main.c poll_hotkeys: F3 (and the harness's hook) */
void ns_quit_local(const char *why);

/* ---------------------------------------------------------------- rb.c (the rollback core) */
int  ns_first_step(void);             /* rb_step f == 1: 1 = netplay runs this battle (released, setup re-verified); 0 = refused / off (logged) */
int  ns_battle_on(void);              /* the battle is armed (not refused, not aborted) */
int  ns_want_abort(char *cause, size_t cap);   /* a pending abort for the core (rb_step entry): 1 = abort with this cause */
void ns_capture(uint32_t s, const C1aIn *ue);  /* step s: the owner sample (fill / merge into the local table) */
int  ns_local(uint32_t f, C1aIn *out);         /* the local side of frame f (p1 + x for the host, p2 for the guest; ff = 7): 1 = known */
int  ns_remote(uint32_t f, C1aIn *out);        /* the peer's side of frame f: 1 = received (or neutral after an abort) */
uint32_t ns_frontier(void);                    /* F: the largest f with the peer's records 1..f received */
uint32_t ns_local_next(void);                  /* the next unfilled local target (local records exist for frames < it) */
int  ns_remote_side(void);                     /* 2 for the host (the guest drives P2), 1 for the guest */
/* rb confirm_frame: the confirmed frame f's tier-1, t15, ued and the 25 tier-1 components (CHECK, the component ring, the DESYNC compare) */
void ns_confirmed(uint32_t f, uint64_t t1, uint64_t t15, uint32_t ued, const uint64_t comps[C1A_NCOMP]);
void ns_step_done(uint32_t s, uint32_t C, int depth);   /* after each step (lastFrame s, the confirmed frame C, the rollback depth or -1) */
void ns_battle_end(const char *why, uint32_t s, uint32_t C);   /* rb_finish / rb abort: NETEND */
void ns_zone_event(int idx, uint32_t z, uint32_t e);    /* rb zone_add / exit: ZONE messages */
void ns_fight_barrier(uint32_t f, long long unconfirmed);   /* rb barrier outside a zone with unconfirmed frames: NETBARRIER fight */
void ns_mispredict_behind_barrier(uint32_t f);          /* a confirmation showed a misprediction behind a fight barrier: ABORT barrier_mispredict */
void ns_core_abort(const char *cause);                  /* the core aborted on its own (depth, layout, ...): ABORT to the peer */
void ns_ctltype(uint32_t f, int side, uint32_t t324, uint32_t ctl30);   /* entry.c per-step assert */
void ns_title(char *out, size_t cap);                   /* " net=<N> ent=<E> role=<host|guest>[ kd=<n>][ ud=<n>]" */
int  ns_lipsync_off(void);                              /* rbnet: lip-sync stays off (D14) */
uint64_t ns_simcfg_net(void);
uint64_t ns_stalls_total(void);                         /* RBEND stalls= */
void ns_header_keys(char *out, size_t cap);             /* the rbnet .rec header keys (C2-SPEC.md §6.4) */
const char *ns_setup_blob_hex(void);                    /* the host's SETUP blob (hex) for the "N 0 setup=" line; NULL = none */
/* harness only */
void ns_h_pause(int on);
const char *ns_last_abort(void);   /* the last ABORT reason of this process ("" = none): the c2peer exit code */
