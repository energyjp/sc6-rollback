/* Option A M1: mode=netobserve. Observe-only online battle recorder (OPTA-M0.md §2, as amended by §5 and the M1 review).
 *
 * Contract (verified by tools/nobs_static.py, tools/static_check.py and the harness NOBS-* cases):
 *   N1 zero writes to game data   N2 zero packets / no game send   N3 no call into game code except the originals (o_enq)
 *   N4 the exact patch set below, installed once at process start
 *   R-STACK: every game-thread hook is a naked asm pre-thunk (src/nobs_thunks.S). It takes a per-hook lock first (lock bts, no
 *            register touched), saves every GPR into a per-hook ctx and the full extended state with XSAVE64 (mask = XCR0 without
 *            AMX) into a 64-byte-aligned per-hook area, switches to a private stack, runs a short pre-only C body, XRSTOR64s,
 *            restores every GPR, rsp and RFLAGS, and jmps to the original. The original sees identical general-purpose and vector
 *            registers, identical rsp and an untouched game stack below rsp. The status flags are NOT preserved (the lock's bts
 *            sets CF; the review VERIFIED that none of the 10 entry points reads a status flag before writing it); DF is untouched.
 *
 * Translation units:
 *   nobs_body.c     hook bodies + every game-memory read (gptr only). Compiled -fno-builtin -mgeneral-regs-only: no CRT call,
 *                   no vector instruction. Never logs, never allocates.
 *   nobs.c          the writer thread (formatting, analysis, WriteFile, title), runtime init/shutdown, the record-only VEH.
 *                   Reads game memory only through nobs_body.c functions.
 *   nobs_install.c  the ONLY TU that patches (vtable slots + the P5 rel32 retarget through a +-2 GiB trampoline). */
#pragma once
#include "common.h"

/* ---------------------------------------------------------------- game addresses (preferred-base VAs; go through VA()) */
#define NOBS_A_TICK        0x1403fbf30ULL   /* ALuxBattleManager::Tick (P1); its only reference is the slot 0x14327ae38 */
#define NOBS_A_TICK_SLOT   0x14327ae38ULL
#define NOBS_A_IFC_VT      0x14327b610ULL   /* IBattleCore interface vtable (P2/P3/P4) */
#define NOBS_IFC_INIT      0x00
#define NOBS_IFC_SHUTDOWN  0x18
#define NOBS_IFC_STEP      0x68
#define NOBS_A_INIT_THUNK  0x1403c3650ULL
#define NOBS_A_SHUT_THUNK  0x1403bcee0ULL
#define NOBS_A_STEP_THUNK  0x1403d2a20ULL
#define NOBS_A_SYNC_VT     0x143283578ULL   /* ALuxBattleFrameInputSync vtable (P6-P10) */
#define NOBS_SYNC_RRESET   0x618            /* FUN_1403f7d20 per-round reset (P10; edx = the new round) */
#define NOBS_SYNC_ISPAUSED 0x628            /* FUN_1403e9ee0 IsPaused (P8) */
#define NOBS_SYNC_SETSTATE 0x690            /* FUN_1403fc920 SetOnlineState (P9; dl = the new state) */
#define NOBS_SYNC_SSM      0x698            /* FUN_1403fdfa0 SyncStateMachine (P7) */
#define NOBS_SYNC_SENDCTL  0x6d0            /* FUN_1403f84e0 SendControl (P6; dl = code, r8d = round or -1) */
#define NOBS_A_RRESET      0x1403f7d20ULL
#define NOBS_A_ISPAUSED    0x1403e9ee0ULL
#define NOBS_A_SETSTATE    0x1403fc920ULL
#define NOBS_A_SSM         0x1403fdfa0ULL
#define NOBS_A_SENDCTL     0x1403f84e0ULL
#define NOBS_A_ENQ         0x1403f4be0ULL   /* FUN_1403f4be0 enqueue (P5); retarget the rel32 call at 0x1403eed2f */
#define NOBS_A_ENQ_CALL    0x1403eed2fULL
#define NOBS_A_INPUTSYNC_VT 0x143283578ULL  /* the online input-object class */
#define NOBS_A_INPUTLOG_VT 0x143282f18ULL   /* offline */
#define NOBS_A_CORE_FRAME  0x14470d0c4ULL

#define NOBS_A_COMMSYS     0x1443ff350ULL   /* CommSystem static; its channel getter (0x142e07340) runs on CommSystem+0x20 and */
#define NOBS_COMMSYS_CHBEG 0x230            /*   indexes the vector [+0x230 begin, +0x238 end) (max 9 channels) */
#define NOBS_COMMSYS_CHEND 0x238
#define NOBS_A_SEL5_EL0    0x1440e3c70ULL   /* selector-5 element-0 static object {vt, id}; id at +8 */
#define NOBS_A_SESS_HOLDER 0x1443ff320ULL   /* static holder object pointer: [holder+0] = session interface, [holder+8] = refblock */
#define NOBS_SESS_OPP_ID   0xd0             /* the opponent address object is (session-0x20)+0xe8 = session+0xc8; its id at +8 */

/* input-object (mgr+0x478) field offsets (T-M1-3, VERIFIED from the decompiles) */
#define NOBS_O_LOCALFLAGS  0x39c
#define NOBS_O_PLAYERS     0x398
#define NOBS_O_GAMEROUND   0x3a0
#define NOBS_O_GAMETIME    0x3a4
#define NOBS_O_PAUSETIME   0x3a8
#define NOBS_O_UPDATETIME  0x3ac
#define NOBS_O_RING        0x3c0            /* + p*0x2000 + (f&0x1ff)*0x10 = {tag(round), frame, value, valid} */
#define NOBS_O_RTT         0x4408
#define NOBS_O_INPUTLAG    0x440c
#define NOBS_O_SENDTIME    0x4410
#define NOBS_O_SYNCTIME    0x4414
#define NOBS_O_LAGMIN      0x43e4
#define NOBS_O_LAGMAX      0x43e8
#define NOBS_O_SYNCTIMEOUT 0x43f0
#define NOBS_O_TIMEOUT_CNT 0x441c
#define NOBS_O_REMOTEFLAGS 0x4400
#define NOBS_O_BONLINE     0x4404
#define NOBS_O_ONLINESYNC  0x4405
#define NOBS_O_STATE       0x4424
#define NOBS_O_RESULT      0x4425
#define NOBS_OBJ_SIZE      0x44d0

/* manager (mgr) field offsets */
#define NOBS_M_INPUTOBJ    0x478
#define NOBS_M_PADBLK      0x450            /* [mgr+0x450] + 0x3e0 + idx*0x90 = the raw pad */
#define NOBS_M_SETUPCACHE  (0x287 * 8)      /* mgr[0x287]: the cached setup object (read only in manager state 2, like the Tick) */
#define NOBS_M_MASK12F8    0x12f8
#define NOBS_M_IFACE       0x1450
#define NOBS_M_TIMER1468   0x1468           /* int: > 0 at UpdateBattle entry -> FetchInput zeroes the pressed half (dec_tick1.c:36-39, :615-617) */
#define NOBS_M_TIMER146C   0x146c           /* int: > 0 at UpdateBattle entry -> FetchInput zeroes the held half (dec_tick1.c:40-43, :610-613) */
#define NOBS_M_STATEBYTE   0x1461
#define NOBS_M_PHASE1480   0x1480
#define NOBS_M_LASTROUND   0x1488
#define NOBS_M_LASTTIME    0x148c
#define NOBS_M_PREV1498    0x1498           /* u32[2] (raw fetched) -- a TArray data POINTER in the real game, see nobs_body.c */
#define NOBS_M_WORD14A8    0x14a8           /* u64[2] step words (pointer in the real game) */
#define NOBS_M_WORD14B8    0x14b8           /* u64[2] previous words (pointer) */
#define NOBS_M_THIRD14C8   0x14c8           /* 0x18 bytes, inline */
#define NOBS_MGR_READ      0x14e0           /* the manager span the bodies read (validated once per pointer change) */
#define NOBS_SETUP_150     0x150

/* ---------------------------------------------------------------- the thunk context (mirrored by src/nobs_thunks.S) */
#define NOBS_CTX_STRIDE    0xc0
#define NOBS_CTX_INUSE     0x88
#define NOBS_TAB_STRIDE    32               /* nobs_hooktab[id] = { orig +0, pstk +8, xarea +16, pad +24 } */
typedef struct {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;   /* 0x00..0x38 (rsp = the game's rsp at thunk entry) */
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;     /* 0x40..0x78 */
    uint64_t rflags;                                   /* 0x80 (status flags already reflect the lock's bts) */
    uint32_t inuse, pad0;                              /* 0x88 */
    uint64_t _pad[6];                                  /* 0x90..0xbf */
} NobsCtx;

/* hook ids (must match the HOOK lines in nobs_thunks.S) */
enum { NOBS_H_TICK = 0, NOBS_H_IFC_INIT, NOBS_H_IFC_SHUTDOWN, NOBS_H_IFC_STEP,
       NOBS_H_SENDCTL, NOBS_H_SSM, NOBS_H_ISPAUSED, NOBS_H_SETSTATE, NOBS_H_RRESET, NOBS_H_N };

/* ---------------------------------------------------------------- the record (ring element and .nobs on-disk record), 384 bytes */
enum { REC_TICK = 1, REC_STEP, REC_PKT, REC_LIFE, REC_SSM, REC_SENDCTL, REC_REGS, REC_COST, REC_UNEXP };
enum { LIFE_INIT = 0, LIFE_SHUTDOWN, LIFE_RESET, LIFE_SETSTATE };
typedef struct { uint64_t lo, hi; } NobsX128;
typedef struct {
    uint32_t type, bat;
    uint64_t qpc;
    uint32_t tick, frame;            /* manager tick counter, core frame [0x14470d0c4] */
    uint32_t gr, gt;                 /* GameRound, GameTime */
    union {
        struct {                     /* REC_TICK */
            uint32_t obj_class, bonline, remote, local, onlinesync, state, result, phase, m12f8, mgrstate;
            uint32_t lag, lagmin, lagmax, rtt_bits, sendtime, synctime, updatetime, timeout_cnt, synctimeout;
            uint32_t setup150, setup150_ok, spectator, last_round, last_time, obj_vt_ok;
            uint64_t obj_vt;
        } t;
        struct {                     /* REC_STEP */
            uint32_t phase, m12f8, k, n, fetch_f, repeat, local, result, t1_on, ahead;
            uint64_t w[2];                    /* StepIn words read through the in[] pointers */
            uint64_t w14a8[2], w14b8[2];
            uint32_t prev1498[2];
            uint32_t ring[2][4];              /* {tag, frame, value, valid} at the fetch frame, per player */
            uint32_t pad_raw, pad_ok;         /* the local raw pad [mgr+0x450]+0x3e0+idx*0x90 */
            uint32_t loc_ahead[16];           /* the local ring value at fetch+k (tag-checked; 0 = missing) for T3 lead */
            int32_t t1468, t146c;             /* mgr+0x1468 / +0x146c read at Tick pre (UpdateBattle's pre-decrement values) */
            uint8_t third[0x18];
        } s;
        struct {                     /* REC_PKT */
            uint32_t dir, len, mode, nbytes, player, cap;
            int64_t cur_off;                  /* cursor (pkt+0x18) minus the buffer data pointer: the payload starts at data */
            uint8_t bytes[64];
        } p;
        struct { uint32_t ev, from, to, round, sendtime, synctime, updatetime, seed; } life;
        struct { uint32_t state, result, pausetime, ispaused, synctime, timeout_cnt, synctimeout, lag, rtt_bits, sendtime; } ssm;
        struct { uint32_t code, round, state; } sc;
        struct {                     /* REC_REGS: the Tick-entry and first-Step-entry vector context of one tick */
            NobsX128 tx[10];                  /* Tick entry: [0] = xmm1, [1..9] = xmm7..xmm15 */
            NobsX128 sx[10];                  /* Step entry: xmm6..xmm15 */
            uint32_t t_mxcsr, t_fcw, s_mxcsr, s_fcw;
        } r;
        struct { uint32_t hook; uint64_t ticks; } cost;   /* REC_COST: QPC ticks spent in the body */
        struct { uint32_t kind, hook, tid; uint64_t detail; } u;   /* REC_UNEXP (detected on the game thread) */
    } u;
} NobsRec;

enum { UNEXP_THREAD = 1, UNEXP_COST_PLANT };

/* ---------------------------------------------------------------- shared runtime state (defined in nobs_body.c) */
#define NOBS_RING_CAP (1u << 14)
typedef struct { volatile LONG64 seq; LONG64 pad_; union { NobsRec rec; uint64_t q[48]; } u; } NobsCell;   /* q[] zeroes the record without a cast */
extern NobsCell *nobs_ring;                 /* allocated by nobs_runtime_init */
extern volatile LONG64 nobs_ring_enq, nobs_ring_drops;
extern LONG64 nobs_ring_deq;                /* writer-owned */
extern volatile LONG nobs_game_tid;         /* the game thread id (the first Tick) */
extern volatile LONG nobs_off, nobs_partial, nobs_offline_rec;
extern volatile uint32_t nobs_bat, nobs_tick;
extern uint8_t *nobs_xarea[NOBS_H_N];       /* the 64-byte-aligned XSAVE areas (also in nobs_hooktab[id].xarea) */
extern uint32_t nobs_xoff_avx;              /* CPUID 0xD.2 EBX: the AVX (ymm upper) component offset in the standard format */
extern uintptr_t nobs_pstk_lo[NOBS_H_N], nobs_pstk_hi[NOBS_H_N];
extern volatile LONG nobs_unexp_thread[NOBS_H_N];

/* ---------------------------------------------------------------- functions */
void nobs_dispatch(int hook, NobsCtx *ctx);                 /* the C body entry the thunks call (rcx=id, rdx=ctx) */
void nobs_enq_hook(void *q, uint64_t player, void *pkt);    /* P5: the retargeted call target (network thread) */
typedef void (*NobsEnqFn)(void *q, uint64_t player, void *pkt);
extern NobsEnqFn nobs_o_enq;                                /* set by nobs_install.c (the cast lives there) */
/* game reads the writer asks for (each validates with VirtualQuery; booleans/addresses only, no ids) */
typedef struct { int ok, n; uint64_t ch4_vt, ch4_slot0; } NobsChan4;
typedef struct { int el0_ok, el0_minus1, sess_ok, opp_ok, el0_ne_opp; uint64_t sess_vt; } NobsSel5;
void nobs_read_chan4(NobsChan4 *out);
void nobs_read_sel5(NobsSel5 *out);
/* thunk wiring (nobs_install.c) */
extern void nobs_th_tick(void), nobs_th_ifc_init(void), nobs_th_ifc_shutdown(void), nobs_th_ifc_step(void),
            nobs_th_sendctl(void), nobs_th_ssm(void), nobs_th_ispaused(void), nobs_th_setstate(void), nobs_th_rreset(void);
extern uint64_t nobs_hooktab[NOBS_H_N * 4];
extern uint32_t nobs_xmask[2];
void *nobs_thunk_entry(int hook);
void  nobs_thunk_wire(int hook, uint64_t orig, void *pstack_top, void *xarea);
void  nobs_install(void);
/* nobs.c runtime */
int  nobs_runtime_init(void);               /* rings, private stacks, XSAVE areas, the VEH, the writer thread; 0 on failure */
void nobs_note_patch(const char *name, uint64_t va, const char *kind, int sig_ok, uint64_t want, uint64_t got, int status);
void nobs_patchset(int n, const char *names, int ok);
void nobs_ready(void);
void nobs_cfg_ignored(void);
void nobs_gate_fail(const char *what, int core);
void nobs_title_token(char *out, size_t cap);
/* Option A M2 (rbonline, sc6rb/M2-INTERFACES.md §3.4): a mode built on this observer may supply the title token instead ("rb=...");
 * regs = the REGSEQ verdict "ok" | "BAD" | "?". NULL (netobserve) = the M1 token above, unchanged. Set once, before the writer runs. */
typedef void (*NobsTitleExtFn)(char *out, size_t cap, const char *regs);
extern NobsTitleExtFn nobs_title_ext_fn;
typedef void (*NobsTickExtFn)(void);
extern NobsTickExtFn nobs_tick_ext_fn;   /* v0.31.0 (OPTA-M2-SPEC.md §20.1): rbonline's badge / cue tick, called by the writer thread every ~50 ms */
int nobs_regs_bad(void);                 /* v0.31.0: the title's regs=BAD (sticky for the session) */
void nobs_shutdown(int process_exit);       /* DllMain detach (process_exit = lpReserved != NULL) / harness detach */
const char *config_ini_path(void);          /* config.c */
void log_set_bounded(int on);               /* log.c: bounded lock waits (process exit) */
HANDLE log_handle(void);                    /* log.c: the session log file (the VEH writes one CRASH line straight to it) */
void nobs_self_range(uintptr_t *lo, uintptr_t *hi);   /* nobs_install.c: the plugin image range */
void *nobs_pstack_top(int hook);

#ifdef SC6RB_HARNESS
/* harness-only plants (function pointers the harness sets; never present in the plugin build) */
extern void (*nobs_h_plant_write_fn)(uint64_t addr);
extern void (*nobs_h_plant_send_fn)(uint64_t obj);
extern void (*nobs_h_body_clobber_fn)(void);
extern void (*nobs_h_body_enter_fn)(int hook);   /* NOBS-GUARD: around every body (game-thread hooks and P5): regions read-only */
extern void (*nobs_h_body_exit_fn)(int hook);
extern volatile int nobs_h_title_ms;        /* the title pump period (harness speeds it up) */
extern volatile int nobs_h_title_any;       /* NOBS-TITLEX plant: the v0.28.0 behavior (first visible process window, cached) */
HANDLE nobs_h_writer(void);                 /* NOBS-LOCK: the writer thread handle */
void  *nobs_h_alloc_near(uintptr_t anchor); /* NOBS-NEAR: the P5 trampoline allocator, for a high anchor */
void log_h_take_lock(void);                 /* NOBS-LOCK: take the log lock and never release it (log.c) */
#endif
