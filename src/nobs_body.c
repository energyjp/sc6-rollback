/* Option A M1: the hook bodies and every game-memory read of mode=netobserve (OPTA-M0.md §2, §5, the M1 review).
 *
 * Built as its own object with -fno-builtin -mgeneral-regs-only -fno-stack-protector (build.sh): the compiler cannot emit a CRT
 * call (memcpy/memset/__chkstk) or any vector instruction here, so a body leaves no trace in xmm/ymm/zmm/k state even before the
 * thunk's XRSTOR64 (tools/nobs_static.py checks the object's imports and disassembly). The only external calls are VirtualQuery and
 * QueryPerformanceCounter (kernel32) and the o_enq original.
 *
 * Every game address is a gptr (const volatile) and is read only through g8/g16/g32/g64; a store through it is a compile error under
 * -Werror=cast-qual. The pointer casts in this file live only inside the two helpers G() (address -> read pointer) and Q() (address
 * -> the argument VirtualQuery wants; never dereferenced); tools/nobs_static.py's tokenizer enforces that. Bodies never log and never
 * allocate: they fill fixed records directly inside a preallocated lock-free ring cell. */
#include "nobs.h"
#include <intrin.h>

typedef const volatile uint8_t *gptr;
static inline gptr G(uintptr_t a) { return (gptr)a; }                       /* cast helper 1 (reads only) */
static inline const void *Q(uintptr_t a) { return (const void *)a; }        /* cast helper 2 (VirtualQuery argument only) */
static inline uint8_t  g8 (gptr p) { return p[0]; }
static inline uint32_t g32(gptr p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t g64(gptr p) { return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---------------------------------------------------------------- shared state (declared in nobs.h) */
NobsCell *nobs_ring;
volatile LONG64 nobs_ring_enq, nobs_ring_drops;
LONG64 nobs_ring_deq;
volatile LONG nobs_game_tid;
volatile LONG nobs_off, nobs_partial, nobs_offline_rec;
volatile uint32_t nobs_bat, nobs_tick;
uint8_t *nobs_xarea[NOBS_H_N];
uint32_t nobs_xoff_avx;
uintptr_t nobs_pstk_lo[NOBS_H_N], nobs_pstk_hi[NOBS_H_N];
volatile LONG nobs_unexp_thread[NOBS_H_N];
NobsEnqFn nobs_o_enq;
#ifdef SC6RB_HARNESS
void (*nobs_h_plant_write_fn)(uint64_t addr);
void (*nobs_h_plant_send_fn)(uint64_t obj);
void (*nobs_h_body_clobber_fn)(void);
void (*nobs_h_body_enter_fn)(int hook);
void (*nobs_h_body_exit_fn)(int hook);
#endif

_Static_assert(sizeof(NobsCtx) == NOBS_CTX_STRIDE, "NobsCtx size mismatch with nobs_thunks.S");
_Static_assert(offsetof(NobsCtx, rsp) == 0x20 && offsetof(NobsCtx, rflags) == 0x80 && offsetof(NobsCtx, inuse) == NOBS_CTX_INUSE, "ctx offsets");
_Static_assert(sizeof(NobsRec) <= 384, "NobsRec too big");

/* ---------------------------------------------------------------- VirtualQuery-based readability (no CRT) */
static int readable(uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a || a + n > 0x7ffffffeffffULL) return 0;
    uintptr_t end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(Q(a), &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (rend <= a) return 0;
        a = rend;
    }
    return 1;
}
/* a one-entry cache per call site: VirtualQuery only when the pointer changes */
typedef struct { uintptr_t a; size_t n; int ok; } RdCache;
static int readable_c(RdCache *c, uintptr_t a, size_t n) {
    if (c->a != a || c->n != n) { c->a = a; c->n = n; c->ok = readable(a, n); }
    return c->ok;
}

static inline uint64_t qpc_now(void) { LARGE_INTEGER li; QueryPerformanceCounter(&li); return (uint64_t)li.QuadPart; }
static inline DWORD tid_now(void) { return __readgsdword(0x48); }   /* TEB ClientId.UniqueThread */

/* ---------------------------------------------------------------- the lock-free MPSC ring (producer side) */
static NobsRec *rec_begin(uint32_t type, LONG64 *pos_out) {
    if (!nobs_ring) return 0;
    for (;;) {
        LONG64 pos = nobs_ring_enq;
        NobsCell *c = &nobs_ring[pos & (NOBS_RING_CAP - 1)];
        LONG64 dif = c->seq - pos;
        if (dif == 0) {
            if (_InterlockedCompareExchange64(&nobs_ring_enq, pos + 1, pos) == pos) {
                for (unsigned i = 0; i < sizeof c->u.q / sizeof c->u.q[0]; i++) c->u.q[i] = 0;
                NobsRec *r = &c->u.rec;
                r->type = type; r->bat = nobs_bat; r->tick = nobs_tick; r->qpc = qpc_now();
                *pos_out = pos;
                return r;
            }
        } else if (dif < 0) {
            _InterlockedIncrement64(&nobs_ring_drops);
            return 0;
        }
    }
}
static void rec_end(LONG64 pos) {
    NobsCell *c = &nobs_ring[pos & (NOBS_RING_CAP - 1)];
    _ReadWriteBarrier();
    _InterlockedExchange64(&c->seq, pos + 1);
}

/* ---------------------------------------------------------------- game-thread view (only the game thread writes these) */
static uintptr_t g_obj_a __attribute__((unused));   /* the validated input object address (harness plants) */
static gptr g_mgr, g_obj;
static int g_mgr_ok, g_obj_ok, g_cls, g_rec, g_last_cls = -1;
static uint64_t g_obj_vt;
static RdCache c_mgr, c_obj, c_setup, c_a14a8, c_a1498, c_a14b8, c_pad, c_rule, c_sync;
static uint32_t g_tick_last_time, g_tick_last_round, g_step_k;
static int32_t g_tick_1468, g_tick_146c;
static NobsX128 g_tx[10]; static uint32_t g_t_mxcsr, g_t_fcw, g_tregs_tick; static int g_tregs_valid;

static int sync_obj_ok(uintptr_t a) {   /* an InputSync method's `this`: readable and of the InputSync class */
    if (!readable_c(&c_sync, a, NOBS_OBJ_SIZE)) return 0;
    return g64(G(a)) == VA(NOBS_A_INPUTSYNC_VT);
}

static void revalidate(uint64_t mgr) {
    g_mgr = G(mgr);
    g_mgr_ok = readable_c(&c_mgr, mgr, NOBS_MGR_READ);
    g_obj_ok = 0; g_cls = 0;
    if (!g_mgr_ok) return;
    uint64_t obj = g64(g_mgr + NOBS_M_INPUTOBJ);
    if (!obj) return;
    g_obj_a = obj; g_obj = G(obj);
    if (!readable_c(&c_obj, obj, NOBS_OBJ_SIZE)) { g_cls = 3; return; }
    g_obj_vt = g64(g_obj);                            /* the vtable, re-read every tick (m2) */
    g_cls = g_obj_vt == VA(NOBS_A_INPUTSYNC_VT) ? 2 : g_obj_vt == VA(NOBS_A_INPUTLOG_VT) ? 1 : 3;
    g_obj_ok = g_cls == 1 || g_cls == 2;
}

/* XSAVE legacy area: FCW +0 (u16), MXCSR +24, xmm[i] +160+16*i; XSAVE header +512: XSTATE_BV (u64). A component whose XSTATE_BV
 * bit is 0 was in its initial configuration (XINUSE = 0): XRSTOR64 then loads the init values (xmm = 0, FCW = 0x037f) and the
 * area bytes may be stale from an earlier save, so the readers substitute the init values. MXCSR is written whenever RFBM[1] or
 * RFBM[2] is set (always, with our mask). */
static void xmm_from(const uint8_t *area, int i, NobsX128 *out) {
    if (!(rd64(area + 512) & 2)) { out->lo = 0; out->hi = 0; return; }
    out->lo = rd64(area + 160 + 16 * i); out->hi = rd64(area + 168 + 16 * i);
}
static uint32_t fcw_from(const uint8_t *area) { return (rd64(area + 512) & 1) ? ((uint32_t)area[0] | (uint32_t)area[1] << 8) : 0x037fu; }

static void fill_tick(NobsRec *r) {
    r->u.t.obj_class = (uint32_t)g_cls;
    r->u.t.obj_vt = g_obj_vt;
    r->u.t.obj_vt_ok = (uint32_t)g_obj_ok;
    r->frame = g32(G(VA(NOBS_A_CORE_FRAME)));
    if (g_mgr_ok) {
        r->u.t.phase = g8(g_mgr + NOBS_M_PHASE1480);
        r->u.t.m12f8 = g32(g_mgr + NOBS_M_MASK12F8);
        r->u.t.mgrstate = g8(g_mgr + NOBS_M_STATEBYTE);
        r->u.t.last_round = g32(g_mgr + NOBS_M_LASTROUND);
        r->u.t.last_time = g32(g_mgr + NOBS_M_LASTTIME);
        /* setup+0x150 only in manager state 2, like the Tick (dec_gb_tick.c:28-33), and through VirtualQuery (m1) */
        if (r->u.t.mgrstate == 2) {
            uint64_t setup = g64(g_mgr + NOBS_M_SETUPCACHE);
            if (setup && readable_c(&c_setup, setup + NOBS_SETUP_150, 4)) { r->u.t.setup150 = g32(G(setup + NOBS_SETUP_150)); r->u.t.setup150_ok = 1; }
        }
    }
    if (g_obj_ok) {
        r->gr = g32(g_obj + NOBS_O_GAMEROUND); r->gt = g32(g_obj + NOBS_O_GAMETIME);
        r->u.t.bonline = g8(g_obj + NOBS_O_BONLINE);
        r->u.t.remote = g32(g_obj + NOBS_O_REMOTEFLAGS);
        r->u.t.local = g32(g_obj + NOBS_O_LOCALFLAGS);
        r->u.t.onlinesync = g8(g_obj + NOBS_O_ONLINESYNC);
        r->u.t.state = g8(g_obj + NOBS_O_STATE);
        r->u.t.result = g8(g_obj + NOBS_O_RESULT);
        r->u.t.spectator = r->u.t.bonline && r->u.t.remote == 0;
        r->u.t.lag = g32(g_obj + NOBS_O_INPUTLAG);
        r->u.t.lagmin = g32(g_obj + NOBS_O_LAGMIN);
        r->u.t.lagmax = g32(g_obj + NOBS_O_LAGMAX);
        r->u.t.rtt_bits = g32(g_obj + NOBS_O_RTT);
        r->u.t.sendtime = g32(g_obj + NOBS_O_SENDTIME);
        r->u.t.synctime = g32(g_obj + NOBS_O_SYNCTIME);
        r->u.t.updatetime = g32(g_obj + NOBS_O_UPDATETIME);
        r->u.t.timeout_cnt = g32(g_obj + NOBS_O_TIMEOUT_CNT);
        r->u.t.synctimeout = g32(g_obj + NOBS_O_SYNCTIMEOUT);
    }
}

static void body_tick(NobsCtx *ctx) {
    uint32_t tk = ++nobs_tick;
    /* the Tick-entry vector context for REGSEQ (B1): xmm1, xmm7..xmm15, MXCSR, FCW */
    const uint8_t *xa = nobs_xarea[NOBS_H_TICK];
    xmm_from(xa, 1, &g_tx[0]);
    for (int i = 7; i <= 15; i++) xmm_from(xa, i, &g_tx[i - 6]);
    g_t_mxcsr = rd32(xa + 24); g_t_fcw = fcw_from(xa);
    g_tregs_tick = tk; g_tregs_valid = 1;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_nobs_plant_xmm9) nobs_xarea[NOBS_H_TICK][160 + 9 * 16] ^= 1;   /* plant: the Tick thunk hands Tick a dirty xmm9 */
#endif
    revalidate(ctx->rcx);
    g_rec = g_cls == 2 || (g_cls == 1 && nobs_offline_rec);
    if (g_mgr_ok) {   /* the UpdateBattle inputs for this tick's fetch-frame arithmetic (read before UpdateBattle rewrites them) */
        g_tick_last_time = g32(g_mgr + NOBS_M_LASTTIME);
        g_tick_last_round = g32(g_mgr + NOBS_M_LASTROUND);
        g_tick_1468 = (int32_t)g32(g_mgr + NOBS_M_TIMER1468);   /* FetchInput's param_4 / param_5 = these > 0 (pre-decrement) */
        g_tick_146c = (int32_t)g32(g_mgr + NOBS_M_TIMER146C);
    }
    g_step_k = 0;
    if (g_rec || g_cls != g_last_cls) {   /* every tick of a recorded battle; otherwise only on a class change (one INPOBJ) */
        g_last_cls = g_cls;
        LONG64 pos; NobsRec *r = rec_begin(REC_TICK, &pos);
        if (r) { fill_tick(r); rec_end(pos); }
    }
#ifdef SC6RB_HARNESS
    if (g_rec && g_cfg.h_nobs_plant_write && (int)tk == g_cfg.h_nobs_plant_write && nobs_h_plant_write_fn) nobs_h_plant_write_fn(g_obj_a + NOBS_O_RESULT);
    if (g_rec && g_cfg.h_nobs_plant_send && (int)tk == g_cfg.h_nobs_plant_send && nobs_h_plant_send_fn) nobs_h_plant_send_fn(g_obj_a);
    if (g_rec && g_cfg.h_nobs_t1_cost && (int)tk == g_cfg.h_nobs_t1_cost) {
        LONG64 pos; NobsRec *r = rec_begin(REC_UNEXP, &pos);
        if (r) { r->u.u.kind = UNEXP_COST_PLANT; r->u.u.hook = NOBS_H_TICK; rec_end(pos); }
    }
#endif
}

static uint32_t ring_val(uint32_t p, uint32_t round, uint32_t f, uint32_t *tag, uint32_t *frame, uint32_t *valid) {
    gptr c = g_obj + NOBS_O_RING + (uint64_t)p * 0x2000 + (uint64_t)(f & 0x1ff) * 0x10;
    uint32_t t = g32(c), fr = g32(c + 4), v = g32(c + 8);
    if (tag) { *tag = t; *frame = fr; *valid = g8(c + 12); }
    return (t == round && fr == f) ? v : 0;   /* FUN_1403f0720: value iff round and frame tags match (the valid byte is not read) */
}

static void body_ifc_step(NobsCtx *ctx) {
    /* B1: pair the first Step entry of this tick with its Tick entry */
    if (g_tregs_valid && g_tregs_tick == nobs_tick) {
        g_tregs_valid = 0;
        if (g_rec) {
            LONG64 pos; NobsRec *r = rec_begin(REC_REGS, &pos);
            if (r) {
                const uint8_t *xs = nobs_xarea[NOBS_H_IFC_STEP];
                for (int i = 0; i < 10; i++) { r->u.r.tx[i] = g_tx[i]; xmm_from(xs, 6 + i, &r->u.r.sx[i]); }
                r->u.r.t_mxcsr = g_t_mxcsr; r->u.r.t_fcw = g_t_fcw;
                r->u.r.s_mxcsr = rd32(xs + 24); r->u.r.s_fcw = fcw_from(xs);
                rec_end(pos);
            }
        }
    }
    if (!g_rec || !g_obj_ok || !g_mgr_ok) return;
    uint32_t k = g_step_k++;
    LONG64 pos; NobsRec *r = rec_begin(REC_STEP, &pos);
    if (!r) return;
    uint32_t gr = g32(g_obj + NOBS_O_GAMEROUND), gt = g32(g_obj + NOBS_O_GAMETIME);
    r->gr = gr; r->gt = gt; r->frame = g32(G(VA(NOBS_A_CORE_FRAME)));
    r->u.s.phase = g8(g_mgr + NOBS_M_PHASE1480); r->u.s.m12f8 = g32(g_mgr + NOBS_M_MASK12F8);
    r->u.s.result = g8(g_obj + NOBS_O_RESULT);
    /* UpdateBattle (dec_tick1.c:46-62): N = GameTime - min(lastTime, GameTime) when the round is unchanged, else GameTime;
     * sub-step k fetches frame GameTime - N + k; a call with k >= N is a 0x1462 repeat (no fetch) */
    uint32_t n = gr == g_tick_last_round ? gt - (g_tick_last_time < gt ? g_tick_last_time : gt) : gt;
    r->u.s.k = k; r->u.s.n = n; r->u.s.repeat = k >= n;
    uint32_t f = gt - n + k;
    r->u.s.fetch_f = f;
    uint32_t lf = g32(g_obj + NOBS_O_LOCALFLAGS);
    uint32_t local = (lf & 1) ? 0 : 1;
    r->u.s.local = local;
    /* the StepIn words through in[] (UpdateBattle's alStack_b0 on the game stack: in[p] = &[mgr+0x14a8][p], in[2] = mgr+0x14c8) */
    uint64_t in = ctx->rdx;
    uint64_t sb = __readgsqword(0x08), sl = __readgsqword(0x10);   /* TEB StackBase / StackLimit of the game thread */
    uint64_t a14a8 = g64(g_mgr + NOBS_M_WORD14A8), a1498 = g64(g_mgr + NOBS_M_PREV1498), a14b8 = g64(g_mgr + NOBS_M_WORD14B8);
    int ok14a8 = a14a8 && readable_c(&c_a14a8, a14a8, 16), ok1498 = a1498 && readable_c(&c_a1498, a1498, 8), ok14b8 = a14b8 && readable_c(&c_a14b8, a14b8, 16);
    if (in >= sl && in + 0x18 <= sb) {
        for (uint32_t p = 0; p < 2; p++) {
            uint64_t wp = g64(G(in + 8 * p));
            if (ok14a8 && wp == a14a8 + 8 * p) r->u.s.w[p] = g64(G(wp));
        }
    }
    for (uint32_t p = 0; p < 2; p++) {
        if (ok14a8) r->u.s.w14a8[p] = g64(G(a14a8 + 8 * p));
        if (ok14b8) r->u.s.w14b8[p] = g64(G(a14b8 + 8 * p));
        if (ok1498) r->u.s.prev1498[p] = g32(G(a1498 + 4 * p));
        ring_val(p, gr, f, &r->u.s.ring[p][0], &r->u.s.ring[p][1], &r->u.s.ring[p][3]);
        r->u.s.ring[p][2] = g32(g_obj + NOBS_O_RING + (uint64_t)p * 0x2000 + (uint64_t)(f & 0x1ff) * 0x10 + 8);
    }
    for (int i = 0; i < 0x18; i++) r->u.s.third[i] = g8(g_mgr + NOBS_M_THIRD14C8 + i);
    /* SLACK: valid remote ring entries beyond the fetch frame; T3: the local ring value at f+j */
    uint32_t remote = 1 - local, ahead = 0;
    for (uint32_t j = 1; j <= 32; j++) {
        uint32_t t, fr, v; ring_val(remote, gr, f + j, &t, &fr, &v);
        if (t != gr || fr != f + j || !v) break;
        ahead++;
    }
    r->u.s.ahead = ahead;
    for (uint32_t j = 0; j < 16; j++) r->u.s.loc_ahead[j] = ring_val(local, gr, f + j, 0, 0, 0);
    uint64_t pad = g64(g_mgr + NOBS_M_PADBLK);
    if (pad && readable_c(&c_pad, pad + 0x3e0 + (uint64_t)local * 0x90, 4)) { r->u.s.pad_raw = g32(G(pad + 0x3e0 + (uint64_t)local * 0x90)); r->u.s.pad_ok = 1; }
    r->u.s.t1_on = (uint32_t)g_cfg.netobs_t1;
    r->u.s.t1468 = g_tick_1468; r->u.s.t146c = g_tick_146c;
    rec_end(pos);
}

static void body_ifc_init(NobsCtx *ctx) {
    uint32_t b = ++nobs_bat;
    LONG64 pos; NobsRec *r = rec_begin(REC_LIFE, &pos);
    if (!r) return;
    r->bat = b; r->u.life.ev = LIFE_INIT;
    uint64_t rule = ctx->rdx;   /* the slot is called (iface, rule); the init thunk moves rdx to rcx */
    if (rule && readable_c(&c_rule, rule, 0x14)) r->u.life.seed = g32(G(rule + 0x10));
    rec_end(pos);
}
static void body_ifc_shutdown(NobsCtx *ctx) {
    (void)ctx;
    LONG64 pos; NobsRec *r = rec_begin(REC_LIFE, &pos);
    if (r) { r->u.life.ev = LIFE_SHUTDOWN; rec_end(pos); }
}
static void body_sendctl(NobsCtx *ctx) {
    if (!sync_obj_ok(ctx->rcx)) return;
    gptr o = G(ctx->rcx);
    LONG64 pos; NobsRec *r = rec_begin(REC_SENDCTL, &pos);
    if (!r) return;
    int32_t round = (int32_t)(uint32_t)ctx->r8;
    r->gr = g32(o + NOBS_O_GAMEROUND); r->gt = g32(o + NOBS_O_GAMETIME);
    r->u.sc.code = (uint32_t)(ctx->rdx & 0xff);
    r->u.sc.round = round < 0 ? r->gr : (uint32_t)round;
    r->u.sc.state = g8(o + NOBS_O_STATE);
    rec_end(pos);
}
static void body_ispaused(NobsCtx *ctx) {
    /* IsPaused pre: the result SyncStateMachine just wrote (+0x4425, consumed and zeroed here by the original) and the return
     * value, which is a pure function of the pre-state: PauseTime(+0x3a8) != 0 || result == 1 (dec_fi3.c:376-404) */
    if (!sync_obj_ok(ctx->rcx)) return;
    gptr o = G(ctx->rcx);
    LONG64 pos; NobsRec *r = rec_begin(REC_SSM, &pos);
    if (!r) return;
    r->gr = g32(o + NOBS_O_GAMEROUND); r->gt = g32(o + NOBS_O_GAMETIME);
    r->u.ssm.state = g8(o + NOBS_O_STATE);
    r->u.ssm.result = g8(o + NOBS_O_RESULT);
    r->u.ssm.pausetime = g32(o + NOBS_O_PAUSETIME);
    r->u.ssm.ispaused = r->u.ssm.pausetime != 0 || r->u.ssm.result == 1;
    r->u.ssm.synctime = g32(o + NOBS_O_SYNCTIME);
    r->u.ssm.timeout_cnt = g32(o + NOBS_O_TIMEOUT_CNT);
    r->u.ssm.synctimeout = g32(o + NOBS_O_SYNCTIMEOUT);
    r->u.ssm.lag = g32(o + NOBS_O_INPUTLAG);
    r->u.ssm.rtt_bits = g32(o + NOBS_O_RTT);
    r->u.ssm.sendtime = g32(o + NOBS_O_SENDTIME);
    rec_end(pos);
}
static void body_setstate(NobsCtx *ctx) {
    if (!sync_obj_ok(ctx->rcx)) return;
    gptr o = G(ctx->rcx);
    LONG64 pos; NobsRec *r = rec_begin(REC_LIFE, &pos);
    if (!r) return;
    r->gr = g32(o + NOBS_O_GAMEROUND); r->gt = g32(o + NOBS_O_GAMETIME);
    r->u.life.ev = LIFE_SETSTATE; r->u.life.round = r->gr;
    r->u.life.from = g8(o + NOBS_O_STATE); r->u.life.to = (uint32_t)(ctx->rdx & 0xff);
    rec_end(pos);
}
static void body_rreset(NobsCtx *ctx) {
    if (!sync_obj_ok(ctx->rcx)) return;
    gptr o = G(ctx->rcx);
    LONG64 pos; NobsRec *r = rec_begin(REC_LIFE, &pos);
    if (!r) return;
    r->gr = g32(o + NOBS_O_GAMEROUND); r->gt = g32(o + NOBS_O_GAMETIME);
    r->u.life.ev = LIFE_RESET;
    r->u.life.from = r->gr;                          /* the round being left */
    r->u.life.round = (uint32_t)ctx->rdx;            /* edx = the new round (passed through to FUN_1403f7c60) */
    r->u.life.sendtime = g32(o + NOBS_O_SENDTIME); r->u.life.synctime = g32(o + NOBS_O_SYNCTIME); r->u.life.updatetime = g32(o + NOBS_O_UPDATETIME);
    rec_end(pos);
}

/* ---------------------------------------------------------------- the dispatcher the thunks call (rcx = id, rdx = ctx) */
void nobs_dispatch(int hook, NobsCtx *ctx) {
#ifdef SC6RB_HARNESS
    if (nobs_h_body_clobber_fn) nobs_h_body_clobber_fn();   /* NOBS-CTX: the worst-case body dirties every register it can */
#endif
    if (nobs_off) return;
    DWORD tid = tid_now();
    if (hook == NOBS_H_TICK && !nobs_game_tid) _InterlockedCompareExchange(&nobs_game_tid, (LONG)tid, 0);
    if (nobs_game_tid && tid != (DWORD)nobs_game_tid) {   /* an unexpected thread: skip the body (the thunk still restores everything) */
        if (_InterlockedIncrement(&nobs_unexp_thread[hook]) == 1) {
            LONG64 pos; NobsRec *r = rec_begin(REC_UNEXP, &pos);
            if (r) { r->u.u.kind = UNEXP_THREAD; r->u.u.hook = (uint32_t)hook; r->u.u.tid = tid; rec_end(pos); }
        }
        return;
    }
    uint64_t t0 = g_cfg.netobs_cost ? qpc_now() : 0;
#ifdef SC6RB_HARNESS
    if (nobs_h_body_enter_fn) nobs_h_body_enter_fn(hook);
#endif
    switch (hook) {
    case NOBS_H_TICK:         body_tick(ctx); break;
    case NOBS_H_IFC_INIT:     body_ifc_init(ctx); break;
    case NOBS_H_IFC_SHUTDOWN: body_ifc_shutdown(ctx); break;
    case NOBS_H_IFC_STEP:     body_ifc_step(ctx); break;
    case NOBS_H_SENDCTL:      body_sendctl(ctx); break;
    case NOBS_H_SSM:          break;   /* SSM pre has nothing new: its result is read at IsPaused pre, the next hook */
    case NOBS_H_ISPAUSED:     body_ispaused(ctx); break;
    case NOBS_H_SETSTATE:     body_setstate(ctx); break;
    case NOBS_H_RRESET:       body_rreset(ctx); break;
    default: break;
    }
#ifdef SC6RB_HARNESS
    if (nobs_h_body_exit_fn) nobs_h_body_exit_fn(hook);
#endif
    if (g_cfg.netobs_cost) {
        uint64_t dt = qpc_now() - t0;
        LONG64 pos; NobsRec *r = rec_begin(REC_COST, &pos);
        if (r) { r->u.cost.hook = (uint32_t)hook; r->u.cost.ticks = dt; rec_end(pos); }
    }
}

/* ---------------------------------------------------------------- P5: the enqueue hook (CommSys thread), a plain C hook.
 * The packet object (T-M1-2): +0x14 mode (1 = read), +0x18 cursor, +0x20 buffer {data +0, int len +8, int cap +0xc}. The copy
 * constructor 0x142dff940 sets cursor = data, so the payload starts at data; cur_off records cursor-data (0 expected). */
void nobs_enq_hook(void *q, uint64_t player, void *pkt) {
#ifdef SC6RB_HARNESS
    if (nobs_h_body_enter_fn) nobs_h_body_enter_fn(NOBS_H_N);
#endif
    if (!nobs_off && pkt) {
        uintptr_t pk = (uintptr_t)pkt;
        LONG64 pos; NobsRec *r = rec_begin(REC_PKT, &pos);
        if (r) {
            r->u.p.dir = 0; r->u.p.player = (uint32_t)(player & 0xff);
            if (readable(pk + 0x14, 0x14)) {
                r->u.p.mode = g32(G(pk + 0x14));
                uint64_t cur = g64(G(pk + 0x18)), buf = g64(G(pk + 0x20));
                if (buf && readable(buf, 0x10)) {
                    uint64_t data = g64(G(buf));
                    int32_t len = (int32_t)g32(G(buf + 8));
                    r->u.p.cap = g32(G(buf + 12));
                    r->u.p.len = (uint32_t)(len < 0 ? 0 : len);
                    r->u.p.cur_off = (int64_t)(cur - data);
                    int n = len < 0 ? 0 : len > g_cfg.netobs_pkt_bytes ? g_cfg.netobs_pkt_bytes : len;
                    if (data && n > 0 && readable(data, (size_t)n)) {
                        for (int i = 0; i < n; i++) r->u.p.bytes[i] = g8(G(data + (uint64_t)i));
                        r->u.p.nbytes = (uint32_t)n;
                    }
                }
            }
            rec_end(pos);
        }
    }
#ifdef SC6RB_HARNESS
    if (nobs_h_body_exit_fn) nobs_h_body_exit_fn(NOBS_H_N);
#endif
    nobs_o_enq(q, player, pkt);   /* the one allowed game call: the original enqueue */
}

/* ---------------------------------------------------------------- game reads requested by the writer thread (M3) */
void nobs_read_chan4(NobsChan4 *o) {
    o->ok = 0; o->n = 0; o->ch4_vt = 0; o->ch4_slot0 = 0;
    uintptr_t cs = VA(NOBS_A_COMMSYS);
    if (!readable(cs + NOBS_COMMSYS_CHBEG, 16)) return;
    uint64_t b = g64(G(cs + NOBS_COMMSYS_CHBEG)), e = g64(G(cs + NOBS_COMMSYS_CHEND));
    o->ok = 1;
    if (!b || e < b || e - b > 64 * 8) return;
    o->n = (int)((e - b) / 8);
    if (o->n > 4 && readable(b + 4 * 8, 8)) {
        uint64_t ch = g64(G(b + 4 * 8));
        if (ch && readable(ch, 8)) {
            o->ch4_vt = g64(G(ch));
            if (o->ch4_vt && readable(o->ch4_vt, 8)) o->ch4_slot0 = g64(G(o->ch4_vt));
        }
    }
}
void nobs_read_sel5(NobsSel5 *o) {
    o->el0_ok = 0; o->el0_minus1 = 0; o->sess_ok = 0; o->opp_ok = 0; o->el0_ne_opp = 0; o->sess_vt = 0;
    uintptr_t el0 = VA(NOBS_A_SEL5_EL0);
    uint64_t el0_id = 0;
    if (readable(el0, 16)) { el0_id = g64(G(el0 + 8)); o->el0_ok = 1; o->el0_minus1 = el0_id == ~0ULL; }
    uintptr_t hp = VA(NOBS_A_SESS_HOLDER);
    if (!readable(hp, 8)) return;
    uint64_t holder = g64(G(hp));
    if (!holder || !readable(holder, 16)) return;
    uint64_t sess = g64(G(holder)), ref = g64(G(holder + 8));
    if (!sess || !ref || !readable(ref, 12) || (int32_t)g32(G(ref + 8)) <= 0) return;   /* the weak pointer must be alive */
    if (!readable(sess, NOBS_SESS_OPP_ID + 8)) return;
    o->sess_ok = 1; o->sess_vt = g64(G(sess));
    uint64_t opp_id = g64(G(sess + NOBS_SESS_OPP_ID));
    o->opp_ok = 1;
    o->el0_ne_opp = o->el0_ok && el0_id != opp_id;   /* booleans only: no id is ever logged */
}
