/* Option A M2: mode=rbonline (OPTA-M2-SPEC.md; the module split, ownership and every cross-module contract: sc6rb/M2-INTERFACES.md).
 *
 * Rollback inside the game's own online matches, used only when both players run it. Per battle:
 *   PASSIVE -> PROBE -> PAIRED -> ARMED(round r, at its InputSync 1->2) -> DRAINED -> END
 * PASSIVE is M1 (netobserve) exactly: R-STACK pre-thunks (src/rbo_thunks.S, M1's macro + a target cell), zero game writes, zero game
 * calls. PROBE / PAIRED add only side-channel traffic (src/p2p.c, our own thread). Nothing is written into game memory before ARMED.
 *
 * Part of the rollback core. FROZEN for the other modules (the interface pass, v0.29.0): every declaration below keeps its meaning; the
 * core may APPEND declarations (mark them "core:"). The transport layer never includes this header; the test harness reads it. */
#pragma once
#include "common.h"
#include "nobs.h"
#include "p2p.h"

/* ---------------------------------------------------------------- the per-battle state machine */
typedef enum { RBO_PASSIVE = 0, RBO_PROBE, RBO_PAIRED, RBO_ARMED, RBO_DRAINED, RBO_END, RBO_NSTATE } RboState;
extern const char *const k_rbo_state[RBO_NSTATE];   /* "passive", "probe", "paired", "armed", "drained", "end" (rbo.c) */

/* ---------------------------------------------------------------- game addresses M2 adds (preferred-base VAs; through VA()) */
#define RBO_A_FETCH        0x1403fcd10ULL   /* FetchInput FUN_1403fcd10(mgr, int p, int remaining, char t1468, char t146c) */
#define RBO_A_FETCH_CALL   0x1403fe6e1ULL   /* its ONLY reference (T-M2-8, V): `call rel32` (e8 2a e6 ff ff) in UpdateBattle */
#define RBO_FETCH_SIG      "48897424204154415641574883ec20"   /* FetchInput prologue (V, the installed exe) */
#define RBO_FETCH_CALL_SIG "e82ae6ffff"
#define RBO_A_FETCH_PRED   0x1403f2220ULL   /* FUN_1403f2220(mgr, p): the 0xffffc3f0 mask predicate (OPTA-M0.md §1.6) */
/* v0.29.1 (review m4): the two calls stock FetchInput makes before the predicate (dec_tick1.c:591-598), called the same way by the armed
 * FetchInput; prologues VERIFIED read-only on the installed exe (2026-10-13) and checked at install with the FetchInput signature */
#define RBO_A_SESS_ACTIVE  0x1403f2590ULL   /* bool FUN_1403f2590(void): CheckOnlineSessionActive (timers ignored when true) */
#define RBO_SESS_ACTIVE_SIG "40574883ec30488d4c2420"
#define RBO_A_SETUP_OF     0x1403f00b0ULL   /* FUN_1403f00b0(mgr): the setup object; its +0x220 byte also makes stock ignore the timers */
#define RBO_SETUP_OF_SIG   "4883ec28488b8138140000"
#define RBO_FETCH_PRED_SIG "48895c2410574883ec20"
#define RBO_A_RINGREAD     0x1403f0720ULL   /* FUN_1403f0720(obj, p, round, frame): value iff (round, frame) tags match */
#define RBO_A_UE_LISTENER  0x140427940ULL   /* the mgr+0x1210 subscriber that can zero a pressed half (T-M2-5; tripwire) */
#define RBO_A_SESS_HOLDER  NOBS_A_SESS_HOLDER   /* [holder+0] session, [holder+8] refblock (alive iff int at +8 > 0) */
#define RBO_SESS_OPP_ID    NOBS_SESS_OPP_ID     /* session+0xd0: the opponent's address id (v0.29.7: a CommSys rand() handle, not a SteamID; see the end) */
#define RBO_STEAMID_HI     0x01100001u      /* universe 1 (public), account type 1 (individual), instance 1 (desktop) */
#define RBO_O_LAGMIN       NOBS_O_LAGMIN    /* +0x43e4 InputLagMin (pinned to d at ARM, restored at drain and END) */
#define RBO_O_LAGMAX       NOBS_O_LAGMAX    /* +0x43e8 InputLagMax */
#define RBO_O_RESULT       NOBS_O_RESULT    /* +0x4425 SSM result (the stall lift writes 2 -> 0, never 1/3/4/5) */

/* ---------------------------------------------------------------- L3-stock (D-B): the measured Step-entry context (V, OPTA-SA §2,
 * sc6rb_20261012_214846_26256.log NOBS REGS b=2: 13,359 of 13,359 Step entries; 128-bit values, dwords 1..3 zero) */
#define RBO_L3S_XMM7       0x47a8c000u      /* 86400.0f; also xmm9 (the Gate B register) */
#define RBO_L3S_XMM8       0x7fffffffu
#define RBO_L3S_XMM9       0x47a8c000u
#define RBO_L3S_XMM6       0x3c888888u      /* 1/60 (the frame dt; also xmm13). Varies on hitch frames in stock; L3-stock uses 1/60 */
#define RBO_L3S_XMM13      0x3c888888u
#define RBO_L3S_MXCSR      0x1f80u
#define RBO_L3S_FCW        0x027fu
typedef struct {                            /* what rbo_l3s_call_step loads before calling Step (every other xmm, every upper half: 0) */
    uint32_t xmm_lo[16];                    /* dword 0 of xmm0..xmm15 (index = register number; xmm0-5 = 0 in L3-stock) */
    uint32_t mxcsr, fcw;
} RboCtx;
extern RboCtx rbo_l3s_ctx;                  /* the L3-stock values above (harness control h_rbo_ctx_xmm9_zero sets xmm_lo[9] = 0) */
/* rbo_l3s.S: zero [rsp - zero_bytes, rsp), load ctx (xmm6-15 dword 0 + MXCSR + FCW; all other vector state 0, vzeroall first when
 * AVX is usable), zero every GPR except the arguments, call fn(iface, in), restore the caller's nonvolatile state. */
void rbo_l3s_call_step(void *fn, void *iface, void *in, size_t zero_bytes, const RboCtx *ctx);

/* ---------------------------------------------------------------- the rbonline thunks (rbo_thunks.S) and their wiring
 * rbo_hooktab[id] = { orig +0, pstk +8, xarea +16, tgt +24 } (stride 32, ids = NOBS_H_*). Each thunk is M1's R-STACK thunk with ONE
 * difference: it calls rbo_dispatch (not nobs_dispatch) and its final jump (and its contended path) goes through tgt, not orig.
 * PASSIVE: tgt == orig (so the thunk is M1's). Only the core changes tgt, only for NOBS_H_IFC_STEP, only on the game thread inside
 * that hook's own body (ARM -> rbo_step_armed; END -> orig). */
extern uint64_t rbo_hooktab[NOBS_H_N * 4];
extern void rbo_th_tick(void), rbo_th_ifc_init(void), rbo_th_ifc_shutdown(void), rbo_th_ifc_step(void),
            rbo_th_sendctl(void), rbo_th_ssm(void), rbo_th_ispaused(void), rbo_th_setstate(void), rbo_th_rreset(void);
void rbo_dispatch(int hook, NobsCtx *ctx);  /* rbo_body.c: nobs_dispatch(hook, ctx) (M1's body, unchanged) then the rbo body */

/* the FetchInput front door: the call at RBO_A_FETCH_CALL is retargeted (rel32) to a near trampoline `jmp qword ptr [rip+0]; dq tgt`
 * (14 bytes, touches no register, flag or stack byte). *rbo_fetch_cell = VA(RBO_A_FETCH) while passive, rbo_fetch_armed from ARM to END. */
extern volatile uint64_t *rbo_fetch_cell;
void rbo_fetch_armed(void *mgr, int p, int remaining, char t1468, char t146c);   /* rbo_fetch.c: the stock rule with raw = ring or prediction */
void rbo_step_armed(void *iface, void *in);                                      /* rbo_core.c: the armed IFC step (game stack) */

/* ---------------------------------------------------------------- lifecycle (rbo.c / rbo_install.c) */
void rbo_install(void);                     /* plugin_init, mode=rbonline: config gate, nobs_runtime_init, the patch set, p2p_init */
void rbo_shutdown(int process_exit);        /* DllMain detach / harness detach: p2p_shutdown, then nobs_shutdown, the summaries */
void rbo_title_token(char *out, size_t cap, const char *regs);   /* nobs_title_ext_fn (writer thread): "rb=<state>[...] regs=<regs>" */
RboState rbo_state(void);                   /* any thread (a snapshot) */
uint32_t rbo_battle(void);                  /* the battle index (= nobs_bat) */

#ifdef SC6RB_HARNESS
/* harness-only observation points (never in the plugin build) */
extern volatile LONG rbo_h_game_writes;     /* rbonline's own game-memory writes so far (the core counts every one: 0 while PASSIVE) */
/* core: (appended, v0.29.x core) more harness-only points for RBO-CTX / RBO-GUARD */
extern void (*rbo_h_body_enter_fn)(int hook);   /* around the rbonline part of every game-thread body (M1's nobs_h_body_*_fn wrap only M1's) */
extern void (*rbo_h_body_exit_fn)(int hook);
void *rbo_h_thunk_of(int hook);                 /* the rbo_th_* entry of NOBS_H_<hook> */
void *rbo_h_fetch_tramp(void);                  /* the FetchInput call path's entry: where the retargeted `call` at 0x1403fe6e1 lands */
extern void rbo_th_step_mut_noxrstor(void), rbo_th_step_mut_pushres(void);   /* RBO-CTX teeth: the step thunk without XRSTOR64 / with game-stack residue */
#endif

/* core: (appended, v0.29.2-optA-M2, OPTA-M2-SPEC.md §16.11 "Contracts", frozen) the Q1 root walk's harness-only observation points.
 * Defined in src/modes.c (src/rbo_q1.inc is the walk itself). Never in the plugin build. */
#ifdef SC6RB_HARNESS
/* rbo_h_q1_walks: production walks run (do_pin_scan(apply=1) -> the walk), every mode. 0 after every passive battle (Q1ZERO, Q1UNMOD).
 * rbo_h_q1_violations: rbonline asked for a walk while not ARMED / DRAINED and no ARM in progress: nothing was read (must stay 0).
 * rbo_h_l3s_calls: passes that went through rbo_l3s_call_step (rbonline's L3-stock wrapper), any mode (IQ1*: > 0; IG4R: 0). */
extern volatile LONG rbo_h_q1_walks, rbo_h_q1_violations, rbo_h_l3s_calls;
/* rbo_h_q1_walk: the walk run directly (Q1WALK, in-process), bypassing the ARMED gate; never counted in rbo_h_q1_walks. It honors the
 * h_rbo_q1_skip key like a production walk. apply must be 0 (anything else: returns -1, nothing read); the snapshot set is never touched.
 * line (cap >= 64; truncated to cap) receives two '\n'-terminated lines:
 *   "Q1 WALK mode=<mode> b=0 f=0 why=test ok=0|1 e1=... e2=... e3=... e4=... e6=... arena_skipped= pins= bytes= us= fail=<E#>:<why>|-\n"
 *   "Q1 PINS n=<n> <addr hex>:<len hex>,<addr hex>:<len hex>,...\n"   (addresses 0x-prefixed, in walk order E1, E2, E3, E4, E6; every range
 *                                                                        the walk would pin, before any coverage / merge step)
 * Returns 1 = ok (every gameplay entry passed or was absent), 0 = a gameplay entry failed (fail= names it; the pin list is then the
 * pins collected before the failure, never applied anywhere), -1 = bad argument. The walk writes nothing outside its own static arrays
 * and calls no allocator (Q1WALK hashes the fake image and checks the fake allocators' counters around it). */
int rbo_h_q1_walk(int apply, char *line, size_t cap);
/* v0.29.3 (the M2 review fixes, OPTA-M2-SPEC.md §16.14; appended). rbo_h_q1_scan_fails: apply scans whose Q1 part counted as failed.
 * rbo_h_q1_pin_scan: one REAL do_pin_scan(apply=1) with the walk gate's state forced to `state`, rb_on() as the failure rule reads it forced to
 * `rbon`, the pin array's free slots before the walk forced to `room` (each -1 = the real value) and `old` = the v0.29.2 rules as controls
 * (bit 1: a gated walk counts as a success; bit 2: walk / late pins that do not fit are dropped silently). Returns 1 when the scan's Q1 part
 * counted as failed (the production q1_failed: m3 Q1GATE, m4 Q1CAP), else 0. */
extern volatile LONG rbo_h_q1_scan_fails;
int rbo_h_q1_pin_scan(int state, int rbon, int room, int old);
/* v0.29.5 Q1R (appended). rbo_h_qc_calls: Q1COV entries past their gate (the pool allocation, begin, every step phase, the report, E5CHK):
 * 0 in every rbonline process (Q1ARM60V); > 0 only with the harness key h_q1cov_rbo=1 (Q1COVRB, the control) or in the hybrid with q1_cov=1.
 * rbo_h_q1_old_rules: h_q1_old_rules set at run time for the in-process controls (16 = v0.29.4's E7, 32 = v0.29.4's E5); returns the old bits.
 * rbo_h_q1_walk(2, ...) appends "Q1 SCANQ1 containers= recorded= unchecked= oversize= budget= pins_dropped= nodes_cut= [<obj>:<head>:<list|tree>:<size>:<why>]..." */
extern volatile LONG rbo_h_qc_calls;
int rbo_h_q1_old_rules(int bits);
#endif

/* core: (appended, v0.29.7-optA-M2, OPTA-M2-SPEC.md §18.10) where the opponent's SteamID lives. S-B step 1 (2026-10-21) logged why=badid:
 * session+0xd0 is NOT a SteamID. Static, read-only (V, the installed exe): session+0xc8 is the opponent's address object {vt 0x1432ee9a8,
 * u64 id} held inline (selector 5 at 0x142e6a5d3: lea rdx,[r14+0xe8], r14 = session-0x20; the copy 0x142dffcf0 takes [rdx+8]). That id is a
 * CommSys peer handle: the peer's SetAddress (vtbl+0x90 = 0x142e046c0) draws it from the CRT rand() (0x142e04727 `call [rand]`, unique per
 * process) and stores it at peer+0x2710, so (id >> 32) is 0 in every match. The game's own send (0x142e0b340 -> CommSystem vtbl+0x28 =
 * 0x142e089d0 -> 0x142e06f00 -> 0x142e01f80) finds the peer by that id in the CommSystem static's vector [+0x260, +0x268) of 16-byte
 * shared pointers {peer, ctrl}. A peer (0x27f0 bytes, vtable 0x143d060f8, 7 allocated at CommSystem init 0x142e09931) holds its address
 * object at +0x2708 (slot 0 0x142e07fc0 returns it; id at +0x2710) and the UE FInternetAddr at +0x26f8 (TSharedPtr object, set by 0x142e0ddb0:
 * CreateInternetAddr, then SetIp(ToString(true)) when the socket API is "SteamSockets"). FInternetAddrSteam (vtable 0x143ba78c0, 0x30 bytes,
 * constructed at 0x1429cab10): FUniqueNetIdSteam at +0x08, the SteamID u64 at +0x20, the Steam channel at +0x28 (its ToString 0x1429e30a0
 * prints [+0x20]:[+0x28]; GetPort / SetPort = +0x28). Candidate sources, in this order (rbo_read_opp_cands): */
#define RBO_A_COMMSYS      NOBS_A_COMMSYS   /* 0x1443ff350: the CommSystem static */
#define RBO_CS_PEERS_BEG   0x260            /* its peer vector: begin, end (16-byte {peer, ctrl} elements) */
#define RBO_CS_PEERS_END   0x268
#define RBO_A_PEER_VT      0x143d060f8ULL   /* the CommSys peer class (primary vtable) */
#define RBO_PEER_UADDR     0x26f8           /* TSharedPtr<FInternetAddr>: the object pointer (+0x2700 its controller) */
#define RBO_PEER_AOBJ      0x2708           /* the peer's address object {vt, id} */
#define RBO_PEER_AID       0x2710           /* its id (the rand() handle the session's opponent object carries) */
#define RBO_A_ADDROBJ_VT   0x1432ee9a8ULL   /* the address object class (selector lists, the session's opponent object, the peer's) */
#define RBO_A_ADDR_NONE    0x1440e3c58ULL   /* the static "no address" object's id (the game's lookup returns nothing for it) */
#define RBO_A_ADDR_EL0     0x1440e3c78ULL   /* selector 5's element 0 id (-1; NOBS SEL5) */
#define RBO_A_UADDR_STEAM_VT 0x143ba78c0ULL /* UE's FInternetAddrSteam */
#define RBO_UADDR_STEAM_ID 0x20             /* its SteamID (u64) */
#define RBO_PEER_SCAN      64               /* the peer vector is read only when it holds 1..64 elements (the game allocates 7) */
enum { RBO_PSRC_PEER = 1,                   /* the CommSys peer whose address id = the session's opponent id -> its FInternetAddrSteam's SteamID */
       RBO_PSRC_RAW = 2,                    /* session+0xd0 itself (v0.29.6's read; a rand() handle in the game, kept last: the checks make it harmless) */
       RBO_PSRC_N };
#define RBO_CAND_MAX       8                /* = P2P_CAND_MAX */
