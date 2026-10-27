/* v0.19.0 "localizer" (Gate B step 2).
 *
 * L1: probes at the STEP3-static.md §6 sites of FUN_1402edb90 (PLAYER0 only: rcx == PLAYER0+0x95fa0
 * at entry), at the Step entry and at the caller's return site. Mechanism: MinHook detours whose
 * target is a per-site copy of the l1_stub.S template. The stub never writes the game stack (TLS
 * slots + a private stack), takes no exception and needs no debug register, so the capture does not
 * change the residue it measures; a 5-byte patch per site was checked statically (tools/l1_sites.py:
 * no branch target inside any patched window, no call/jump/ret in it). Hardware execute breakpoints
 * were rejected: each hit pushes a ~1.5 KB exception frame onto the game stack, exactly over the
 * uninitialized region under test (the entry site would overwrite the hold flags at E-0x1b8), and
 * there are only 4 of them for 9 sites. Single-stepping (L2) was not built.
 *
 * Every record is indexed by pass (live / resim k / sync replay), call ordinal and layer. The handler
 * only copies into preallocated buffers (no allocation, no lock, no logging); every read outside the
 * image and the current thread's committed stack is checked with VirtualQuery first. A fault inside
 * the handler is caught by the VEH (l1_veh): logged, L1 disabled, the game resumes at the stub's
 * restore path with its state intact. Formatting happens after the pass, on the normal stack.
 *
 * L3: every step pass goes through l3_call_step (l1_stub.S) when on: all registers zero except the
 * arguments, and the stack below the call zeroed to a depth that covers FUN_1402edb90's frame as
 * measured by L1 (STEP rsp - (E - 0x39d8)) plus 64 KB. */
#include "common.h"
#include "l1.h"
#include "MinHook.h"

typedef struct { uint64_t r[16]; uint64_t fl; } Gpr;      /* rax rcx rdx rbx rsp rbp rsi rdi r8..r15, rflags */
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
static const char *k_reg[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };

typedef struct { const char *name; uint64_t va; const char *sig; } Site;
static const Site k_site[L1S_N] = {   /* keep in sync with tools/static_check.py (L1 sites) */
    { "STEP",  0x1402dbc60ULL, "4c8bdc49895b10" },
    { "ENTRY", 0x1402edb90ULL, "48895c2418" },
    { "SAMP1", 0x1402edf0aULL, "4c8be0c78570370000ffffffff" },
    { "SAMP2", 0x1402ee172ULL, "4c8be049d1ec" },
    { "SAMP3", 0x1402ee62fULL, "498b570848d1e8" },
    { "MASK",  0x1402efae9ULL, "f30f107d6c" },
    { "LOOPX", 0x1402efe88ULL, "f3440f10157fa9b903" },
    { "HOLD",  0x1402f095bULL, "4438a521370000" },
    { "EXIT",  0x140305d9cULL, "f30f108354650900" },
};

#define POFF     0x95fa0ULL   /* rcx = PLAYER + 0x95fa0 */
#define FRAME    0x38d8ULL    /* rbp = E - 0x38d8 (lea rbp,[rsp-0x38a0] after 7 pushes; VERIFIED disassembly) */
#define FRAME_LO 0x39d8ULL    /* rsp after the prologue = E - 0x39d8 (mov eax,0x39a0; sub rsp,rax) */
#define NBLK     58           /* 256-byte block hashes of [E - 0x3a00, E) */
#define PL_LO    0x96370ULL   /* raw PLAYER chunk: bone-parent ints .. parent table */
#define PL_HI    0x96c30ULL

typedef struct { uint64_t xmm[10][2]; uint32_t mxcsr, pad; } PStep;
typedef struct { uint64_t motion; int32_t idx; uint32_t flags; uint8_t par[9]; uint8_t ok; uint8_t pad[6]; } Clip;
typedef struct {
    uint64_t xmm[10][2];
    uint8_t resid[0x48];          /* [E-0x1b8, E-0x170): hold flags rbp+0x3720..0x372f, gap, local_198 rbp+0x3740..0x3763 */
    uint8_t samp[9][0x30];        /* rbp+0xdf0 + b*0x30, bones 23..31 (residue at entry) */
    uint64_t blk[NBLK];
    uint8_t pl[PL_HI - PL_LO];
    uint32_t wtype; uint8_t b1704, pad[3]; uint64_t p78, p96f60, hpose, hbank;
    Clip clip[5];                 /* layers 0..3, fallback */
    uint8_t glob[0x40]; uint32_t g19c; uint32_t okbits;
    uint8_t extra[4][64];
} PEntry;
typedef struct { uint8_t s[3][0x30]; } PSamp;             /* bones 25, 26, 30 */
typedef struct { uint8_t s[9][0x30]; } PMask;             /* bones 23..31 */
typedef struct { uint8_t flags[0x10]; } PLoopx;
typedef struct { uint8_t flags[0x10]; uint8_t l198[0x28]; uint8_t bank[3][0x40]; uint8_t pose[3][0x40]; } PHold;
typedef struct { uint8_t pose[3][0x40]; uint8_t bank[3][0x40]; } PExit;
static const uint32_t k_wrow[3] = { 0x640, 0x680, 0x780 };   /* WEAPON3 (25), WEAPON4 (26), WEAPON8 (30) */
static const char *k_wname[3] = { "W3", "W4", "W8" };

typedef struct { uint8_t site, layer, ord, pad; uint32_t len; Gpr g; } Rec;
#define L1_BUFSZ (24 * 1024)
typedef struct { uint64_t seq, k; int kind, nrec, over; uint32_t used; uint8_t buf[L1_BUFSZ]; } PassBuf;
#define NLIVE 16
#define NRES 8

static PassBuf *g_live, *g_res, *g_cur;
static uint64_t g_k0, g_iter_s;
volatile uint32_t g_l1_tid_on;                 /* read by the stub: capture only on this thread */
static uint64_t g_pstack_top;                  /* read by the stub */
static uint8_t *g_pstack;
static uint8_t *g_stubs;
static size_t g_stub_sz, g_restore_off;
static void *g_tramp[L1S_N];
static uintptr_t g_site_abs[L1S_N];
static int g_l1_ok, g_l1_disabled;
static int g_cur_p0, g_nent;
static uint64_t g_E, g_bank, g_step_rsp_cur;
static volatile int g_in_handler, g_h_site;
static Gpr *volatile g_h_regs;
static uint8_t *volatile g_h_xs;
static uint64_t g_hits[L1S_N], g_overflows, g_unreadable, g_depth_max, g_filtered;
volatile int g_l3_on;
volatile uint8_t g_l3_vzero;   /* v0.20: the L3 wrapper runs vzeroall (set only when CPUID + XGETBV report AVX) */
#ifdef SC6RB_HARNESS
volatile int g_l1_test_fault;
#endif

extern uint8_t l1_tpl_begin[], l1_tpl_restore[], l1_tpl_end[];

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
    return h;
}
static void cpy(void *d, const void *s, size_t n) { uint8_t *o = (uint8_t *)d; const uint8_t *i = (const uint8_t *)s; while (n--) *o++ = *i++; }

/* reads: image and the current thread's committed stack are direct; anything else is VirtualQuery-checked */
static int in_stack(uintptr_t a, size_t n) {
    NT_TIB *t = (NT_TIB *)NtCurrentTeb();
    return a >= (uintptr_t)t->StackLimit && a + n <= (uintptr_t)t->StackBase && a + n >= a;
}
static int safe_rd(void *dst, uintptr_t src, size_t n) {
    if ((IN_IMAGE(src) && IN_IMAGE(src + n - 1)) || in_stack(src, n) || mem_readable((const void *)src, n)) { cpy(dst, (const void *)src, n); return 1; }
    uint8_t *o = (uint8_t *)dst; for (size_t i = 0; i < n; i++) o[i] = 0;
    g_unreadable++;
    return 0;
}

static void *rec_new(int site, int layer, const Gpr *g, uint32_t len) {
    PassBuf *pb = g_cur;
    if (!pb) return NULL;
    uint32_t need = (uint32_t)((sizeof(Rec) + len + 7) & ~7u);
    if (pb->used + need > L1_BUFSZ) { pb->over++; g_overflows++; return NULL; }
    Rec *r = (Rec *)(pb->buf + pb->used);
    r->site = (uint8_t)site; r->layer = (uint8_t)layer; r->ord = (uint8_t)(g_nent > 0 ? g_nent - 1 : 0); r->len = len;
    cpy(&r->g, g, sizeof *g);
    pb->used += need; pb->nrec++;
    return r + 1;
}

static void clip_of(Clip *c, uintptr_t layer_or_fb, int fallback) {
    uint64_t mot = 0; int32_t idx = 0;
    safe_rd(&mot, layer_or_fb + 8, 8); safe_rd(&idx, layer_or_fb + 0x10, 4);
    c->motion = mot; c->idx = idx; c->ok = 0; (void)fallback;
    if (!mot || idx < 0 || idx > 100000) return;
    uint32_t eo = 0;
    if (!safe_rd(&eo, (uintptr_t)mot + 8 + (uintptr_t)idx * 4, 4)) return;
    uintptr_t ent = (uintptr_t)mot + eo;
    if (!safe_rd(&c->flags, ent + 4, 4)) return;
    if (!safe_rd(c->par, ent + 0x12, 9)) return;
    c->ok = 1;
}

/* called by the stub on the private stack; must not allocate, lock or log */
void l1_handler(uint32_t site, Gpr *g, uint8_t *xs) {
    g_h_regs = g; g_h_xs = xs; g_h_site = (int)site; g_in_handler = 1;
    if (site >= L1S_N || !g_cur) { g_in_handler = 0; return; }
    g_hits[site]++;
    uintptr_t p0 = VA(A_PLAYER0), par = p0 + POFF;
    if (site == L1S_STEP) {
        g_step_rsp_cur = g->r[RSP];
        PStep *p = (PStep *)rec_new(site, 0xff, g, sizeof(PStep));
        if (p) { cpy(p->xmm, xs + 0xa0 + 6 * 16, sizeof p->xmm); cpy(&p->mxcsr, xs + 24, 4); }
    } else if (site == L1S_ENTRY) {
        if (g->r[RCX] != par) { g_cur_p0 = 0; g_filtered++; g_in_handler = 0; return; }
#ifdef SC6RB_HARNESS
        if (g_l1_test_fault == 1) { g_l1_test_fault = 2; volatile int *bad = (volatile int *)(uintptr_t)0x18; (void)*bad; }
#endif
        g_cur_p0 = 1; g_E = g->r[RSP]; g_bank = g->r[RDX]; g_nent++;
        if (g_step_rsp_cur > g_E && g_step_rsp_cur - g_E < (64u << 20)) {
            uint64_t d = g_step_rsp_cur - (g_E - FRAME_LO);
            if (d > g_depth_max) g_depth_max = d;
        }
        PEntry *p = (PEntry *)rec_new(site, 0xff, g, sizeof(PEntry));
        if (!p) { g_in_handler = 0; return; }
        uint32_t ok = 0;
        cpy(p->xmm, xs + 0xa0 + 6 * 16, sizeof p->xmm);
        ok |= (uint32_t)safe_rd(p->resid, g_E - 0x1b8, sizeof p->resid) << 0;
        ok |= (uint32_t)safe_rd(p->samp, g_E - 0x2ae8 + 23 * 0x30, sizeof p->samp) << 1;
        uint8_t blk[256];
        for (int i = 0; i < NBLK; i++) { safe_rd(blk, g_E - 0x3a00 + (uint64_t)i * 256, 256); p->blk[i] = fnv(blk, 256); }
        ok |= (uint32_t)safe_rd(p->pl, p0 + PL_LO, sizeof p->pl) << 2;
        safe_rd(&p->wtype, p0 + 0x2b4a4, 4); safe_rd(&p->b1704, p0 + 0x1704, 1);
        safe_rd(&p->p78, p0 + 0x78, 8); safe_rd(&p->p96f60, p0 + 0x96f60, 8);
        { uint8_t pose[0x800]; safe_rd(pose, par, sizeof pose); p->hpose = fnv(pose, sizeof pose); }
        { static uint8_t bank[0x1840]; ok |= (uint32_t)safe_rd(bank, g_bank, sizeof bank) << 3; p->hbank = fnv(bank, sizeof bank); }
        for (int n = 0; n < 4; n++) clip_of(&p->clip[n], par + 0x800 + (uintptr_t)n * 0xc0, 0);
        clip_of(&p->clip[4], p0 + 0x96b60, 1);
        safe_rd(p->glob, VA(0x14470de50ULL), 0x40); safe_rd(&p->g19c, VA(0x14470d19cULL), 4);
        for (int i = 0; i < g_cfg.l1_nextra && i < 4; i++) safe_rd(p->extra[i], VA(g_cfg.l1_extra_va[i]), g_cfg.l1_extra_len[i] > 64 ? 64 : g_cfg.l1_extra_len[i]);
        p->okbits = ok;
    } else if (site == L1S_EXIT) {
        if (!g_cur_p0 || g->r[RSP] != g_E + 8) { g_in_handler = 0; return; }
        PExit *p = (PExit *)rec_new(site, 0xff, g, sizeof(PExit));
        if (p) for (int i = 0; i < 3; i++) { safe_rd(p->pose[i], par + k_wrow[i], 0x40); safe_rd(p->bank[i], g_bank + k_wrow[i], 0x40); }
        g_cur_p0 = 0;
    } else {
        if (!g_cur_p0 || g->r[RBP] != g_E - FRAME) { g_in_handler = 0; return; }
        uint64_t lp = g->r[R15] - (par + 0x800);
        int layer = lp < 4 * 0xc0 && lp % 0xc0 == 0 ? (int)(lp / 0xc0) : 0xff;
        uintptr_t rbp = g->r[RBP];
        if (site == L1S_SAMP1 || site == L1S_SAMP2 || site == L1S_SAMP3) {
            PSamp *p = (PSamp *)rec_new(site, layer, g, sizeof(PSamp));
            if (p) { safe_rd(p->s[0], rbp + 0x12a0, 0x30); safe_rd(p->s[1], rbp + 0x12d0, 0x30); safe_rd(p->s[2], rbp + 0x1390, 0x30); }
        } else if (site == L1S_MASK) {
            PMask *p = (PMask *)rec_new(site, layer, g, sizeof(PMask));
            if (p) safe_rd(p->s, rbp + 0xdf0 + 23 * 0x30, sizeof p->s);
        } else if (site == L1S_LOOPX) {
            PLoopx *p = (PLoopx *)rec_new(site, layer, g, sizeof(PLoopx));
            if (p) safe_rd(p->flags, rbp + 0x3720, 0x10);
        } else if (site == L1S_HOLD) {
            PHold *p = (PHold *)rec_new(site, layer, g, sizeof(PHold));
            if (p) {
                safe_rd(p->flags, rbp + 0x3720, 0x10); safe_rd(p->l198, rbp + 0x3740, 0x28);
                for (int i = 0; i < 3; i++) { safe_rd(p->bank[i], g->r[R14] + k_wrow[i], 0x40); safe_rd(p->pose[i], g->r[RBX] + k_wrow[i], 0x40); }
            }
        }
    }
    g_in_handler = 0;
}

/* ------------------------------------------------------------------ install */
static int hexb(const char *h) { int v = 0; for (int i = 0; i < 2; i++) { char c = h[i]; v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0'); } return v; }
static void patch32(uint8_t *b, size_t n, uint32_t mark, uint32_t v) { for (size_t i = 0; i + 4 <= n; i++) if (!memcmp(b + i, &mark, 4)) memcpy(b + i, &v, 4); }
static void patch64(uint8_t *b, size_t n, uint64_t mark, uint64_t v) { for (size_t i = 0; i + 8 <= n; i++) if (!memcmp(b + i, &mark, 8)) memcpy(b + i, &v, 8); }

#ifdef SC6RB_HARNESS
void l1_harness_sites(const uintptr_t *abs_sites) { for (int i = 0; i < L1S_N; i++) g_site_abs[i] = abs_sites[i]; }
#endif

int l1_install(void) {
    if (!g_cfg.l1) { LOG("L1 off (ini l1=0)\n"); return 0; }
    unsigned a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (!(c & (1u << 27))) { LOG("L1 disabled why=no_osxsave\n"); return 0; }
    a = 0xd; c = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    (void)d;
    if (b == 0 || b > 0x4000 - 64) { LOG("L1 disabled why=xsave_size size=%u\n", b); return 0; }
    DWORD tls[3];
    for (int i = 0; i < 3; i++) { tls[i] = TlsAlloc(); if (tls[i] == TLS_OUT_OF_INDEXES || tls[i] >= 64) { LOG("L1 disabled why=tls_index idx=%lu\n", (unsigned long)tls[i]); return 0; } }
    uintptr_t addr[L1S_N];
    for (int i = 0; i < L1S_N; i++) {
        addr[i] = g_site_abs[i] ? g_site_abs[i] : VA(k_site[i].va);
        if (g_site_abs[i]) continue;   /* harness: its own fake sites */
        size_t n = strlen(k_site[i].sig) / 2;
        const uint8_t *p = (const uint8_t *)addr[i];
        int ok = mem_readable(p, n);
        for (size_t j = 0; ok && j < n; j++) if (p[j] != hexb(k_site[i].sig + 2 * j)) ok = 0;
        if (!ok) { LOG("L1 disabled why=site_signature site=%s va=0x%llx (no probe installed)\n", k_site[i].name, (unsigned long long)k_site[i].va); return 0; }
    }
    if (!mem_readable((void *)VA(A_PLAYER0), 2 * PLAYER_STRIDE)) { LOG("L1 disabled why=player_unreadable\n"); return 0; }
    g_live = (PassBuf *)VirtualAlloc(NULL, sizeof(PassBuf) * (NLIVE + NRES), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_pstack = (uint8_t *)VirtualAlloc(NULL, 512 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_stub_sz = (size_t)(l1_tpl_end - l1_tpl_begin);
    g_restore_off = (size_t)(l1_tpl_restore - l1_tpl_begin);
    g_stubs = (uint8_t *)VirtualAlloc(NULL, 0x200 * L1S_N, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);   /* MinHook requires an executable detour; RX after patching */
    if (!g_live || !g_pstack || !g_stubs || g_stub_sz > 0x200) { LOG("L1 disabled why=alloc stub_size=%zu\n", g_stub_sz); return 0; }
    g_res = g_live + NLIVE;
    for (int i = 0; i < NLIVE + NRES; i++) g_live[i].seq = ~0ULL;
    memset(g_pstack, 0xa5, 4096);   /* canary at the bottom of the private stack */
    g_pstack_top = ((uint64_t)(uintptr_t)(g_pstack + 512 * 1024) - 256) & ~63ULL;
    MH_STATUS s = MH_OK;
    int created = 0;
    for (int i = 0; i < L1S_N && s == MH_OK; i++) {
        uint8_t *st = g_stubs + 0x200 * i;
        memcpy(st, l1_tpl_begin, g_stub_sz);
        patch32(st, g_stub_sz, 0x7EAD0001u, 0x1480u + 8u * tls[0]);
        patch32(st, g_stub_sz, 0x7EAD0002u, 0x1480u + 8u * tls[1]);
        patch32(st, g_stub_sz, 0x7EAD0003u, 0x1480u + 8u * tls[2]);
        patch64(st, g_stub_sz, 0x7EAD7EAD00000004ULL, (uint64_t)(uintptr_t)&g_l1_tid_on);
        patch64(st, g_stub_sz, 0x7EAD7EAD00000005ULL, (uint64_t)(uintptr_t)&g_pstack_top);
        patch32(st, g_stub_sz, 0x7EAD0006u, (uint32_t)i);
        patch64(st, g_stub_sz, 0x7EAD7EAD00000007ULL, (uint64_t)(uintptr_t)l1_handler);
        s = MH_CreateHook((void *)addr[i], st, &g_tramp[i]);
        if (s == MH_OK) { patch64(st, g_stub_sz, 0x7EAD7EAD00000008ULL, (uint64_t)(uintptr_t)g_tramp[i]); created++; }
        else LOG("L1 create_hook site=%s status=%s\n", k_site[i].name, MH_StatusToString(s));
    }
    DWORD old;
    VirtualProtect(g_stubs, 0x200 * L1S_N, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_stubs, 0x200 * L1S_N);
    if (s == MH_OK) for (int i = 0; i < L1S_N && s == MH_OK; i++) s = MH_QueueEnableHook((void *)addr[i]);
    if (s == MH_OK) s = MH_ApplyQueued();
    if (s != MH_OK) {
        for (int i = 0; i < created; i++) MH_RemoveHook((void *)addr[i]);
        LOG("L1 disabled why=minhook status=%s (all probes removed)\n", MH_StatusToString(s));
        return 0;
    }
    g_l1_ok = 1;
    LOG("L1 installed sites=%d mechanism=minhook_detour_private_stack stub_bytes=%zu xsave_bytes=%u tls=%lu,%lu,%lu pstack=%p l3=%d l3_zero_kb=%d cmp_every=%d full_budget=%d cmp_budget=%d extra=%d\n",
        L1S_N, g_stub_sz, b, (unsigned long)tls[0], (unsigned long)tls[1], (unsigned long)tls[2], (void *)g_pstack, g_l3_on, g_cfg.l3_zero_kb,
        g_cfg.l1_cmp_every, g_cfg.l1_full_budget, g_cfg.l1_cmp_budget, g_cfg.l1_nextra);
    return 1;
}

int l1_active(void) { return g_l1_ok && !g_l1_disabled; }

int l1_veh(EXCEPTION_POINTERS *ep) {
    if (!g_in_handler || !g_l1_ok || GetCurrentThreadId() != g_l1_tid_on) return 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP || code == EXCEPTION_BREAKPOINT) return 0;
    int site = g_h_site;
    g_in_handler = 0; g_l1_disabled = 1; g_l1_tid_on = 0;
    uintptr_t a = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    LOG("L1 FAULT site=%s code=0x%08lx addr=%p: capture disabled for the session, game resumed at the probe's restore path\n",
        site >= 0 && site < L1S_N ? k_site[site].name : "?", code, (void *)a);
    CONTEXT *c = ep->ContextRecord;
    c->Rbx = (DWORD64)(uintptr_t)g_h_regs;
    c->R12 = (DWORD64)(uintptr_t)g_h_xs;
    c->Rsp = (DWORD64)(uintptr_t)g_h_xs - 0x40;
    c->Rip = (DWORD64)(uintptr_t)(g_stubs + 0x200 * (size_t)site + g_restore_off);
    return 1;
}

/* ------------------------------------------------------------------ passes */
static int g_seg_cmp, g_seg_full, g_seg_bad, g_seg_frames, g_seg_every;
static uint64_t g_seg_E;
static uint64_t g_walk_E, g_last_bad;   /* R3: global walk phase = start of the latest bad run (k); NOT reset per segment */
static char g_seg_path[16];

void l1_pass_begin(int kind, uint64_t seq, uint64_t k) {
    g_cur = NULL;
    if (!l1_active() || kind == L1P_NONE) return;
    PassBuf *pb = NULL;
    if (kind == L1P_LIVE) pb = &g_live[seq % NLIVE];
    else if (kind == L1P_REPLAY) pb = &g_res[0];
    else if (k >= g_k0 && k - g_k0 < NRES) pb = &g_res[k - g_k0];
    if (!pb) return;
    pb->seq = seq; pb->k = k; pb->kind = kind; pb->nrec = 0; pb->used = 0; pb->over = 0;
    g_cur = pb; g_cur_p0 = 0; g_nent = 0; g_step_rsp_cur = 0;
    g_l1_tid_on = GetCurrentThreadId();
}
void l1_pass_end(void) {
    g_l1_tid_on = 0; g_cur = NULL;
    if (g_l1_ok && !g_l1_disabled) {
        for (int i = 0; i < 4096; i += 64) if (g_pstack[i] != 0xa5) { g_l1_disabled = 1; LOG("L1 disabled why=private_stack_canary\n"); break; }
    }
}
void l1_iter_begin(uint64_t s, uint64_t k0) {
    g_k0 = k0; g_iter_s = s;
    if (g_res) for (int i = 0; i < NRES; i++) g_res[i].seq = ~0ULL;
}
static PassBuf *live_of(uint64_t k) { if (!g_live) return NULL; PassBuf *p = &g_live[k % NLIVE]; return p->seq == k && p->kind == L1P_LIVE ? p : NULL; }
static PassBuf *res_of(uint64_t k) {
    if (!g_res) return NULL;
    for (int i = 0; i < NRES; i++) if (g_res[i].seq == g_iter_s && g_res[i].k == k && g_res[i].kind != L1P_LIVE) return &g_res[i];
    return NULL;
}
static Rec *nth(PassBuf *pb, int site, int n) {
    uint32_t off = 0;
    for (int i = 0; i < pb->nrec; i++) {
        Rec *r = (Rec *)(pb->buf + off);
        if (r->site == site && r->ord == 0 && n-- == 0) return r;   /* R11: first PLAYER0 call only (records are paired by position) */
        off += (uint32_t)((sizeof(Rec) + r->len + 7) & ~7u);
    }
    return NULL;
}
static int count(PassBuf *pb, int site) { int n = 0; while (nth(pb, site, n)) n++; return n; }
static int calls(PassBuf *pb) {   /* PLAYER0 FUN_1402edb90 calls in the pass (all ordinals) */
    uint32_t off = 0; int n = 0;
    for (int i = 0; i < pb->nrec; i++) { Rec *r = (Rec *)(pb->buf + off); n += r->site == L1S_ENTRY; off += (uint32_t)((sizeof(Rec) + r->len + 7) & ~7u); }
    return n;
}
#define PAY(r, T) ((T *)((r) + 1))

/* ------------------------------------------------------------------ compare / classify */
typedef struct { char *b; size_t n, cap; } Sb;
static void sb(Sb *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb(Sb *s, const char *fmt, ...) {
    if (s->n + 1 >= s->cap) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(s->b + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (k > 0) s->n += (size_t)k < s->cap - s->n ? (size_t)k : s->cap - s->n - 1;
}
static const char *pl_name(size_t off, char *o, size_t cap) {   /* off relative to PLAYER */
    if (off >= 0x967a0 && off < 0x96aa0) { size_t l = (off - 0x967a0) / 0xc0, f = (off - 0x967a0) % 0xc0;
        snprintf(o, cap, "L%zu+0x%zx(%s)", l, f, f == 8 ? "motion" : f == 0x10 ? "index" : f == 0x14 ? "time" : f >= 0x18 && f < 0x20 ? "mask" : f >= 0x20 ? "weight" : "?"); }
    else if (off >= 0x96b60 && off < 0x96b80) snprintf(o, cap, "fallback+0x%zx", off - 0x96b60);
    else if (off >= 0x96c24 && off < 0x96c2d) snprintf(o, cap, "parent_table[%zu]", off - 0x96c24);
    else if (off >= 0x965e0 && off < 0x967a0) snprintf(o, cap, "pose_bone%zu+0x%zx", (off - 0x95fa0) / 0x40, (off - 0x95fa0) % 0x40);
    else if (off < 0x965e0) snprintf(o, cap, "bone_ints+0x%zx", off - 0x96370);
    else snprintf(o, cap, "PLAYER+0x%zx", off);
    return o;
}
/* R1: a differing residue qword counts as call context only if it TRACKS plugin context on both passes:
 * resid_a == reg_a and resid_b == reg_b (so the register differs too) for a STEP-site callee-saved GPR
 * or xmm6-15 half, or both values point into their own pass's plugin stack [STEP rsp, +1 MB). ENTRY
 * registers are the game's own arguments and are not used. Values below 0x10000 and qwords whose two
 * halves are both zero or normal floats in [-16, 16] (flag bytes, 1.0f lanes) are skipped. Denormal
 * halves do not count as floats: a user-space pointer's high half (0x00007ffx) is a denormal. */
static int smallf(uint32_t h) { uint32_t m = h & 0x7fffffffu, e = m >> 23; return m == 0 || (e != 0 && m <= 0x41800000u); }
static int ctx_plausible(uint64_t v) { return v >= 0x10000 && !(smallf((uint32_t)v) && smallf((uint32_t)(v >> 32))); }
static int ctx_match(PassBuf *A, PassBuf *B, uint64_t va, uint64_t vb, char *o, size_t cap) {
    if (va == vb || !ctx_plausible(va) || !ctx_plausible(vb)) return 0;
    Rec *ra = nth(A, L1S_STEP, 0), *rb = nth(B, L1S_STEP, 0);
    if (!ra || !rb) return 0;
    static const int cs[8] = { RBX, RBP, RSI, RDI, R12, R13, R14, R15 };
    for (int i = 0; i < 8; i++) if (ra->g.r[cs[i]] == va && rb->g.r[cs[i]] == vb) {
        snprintf(o, cap, "%s@step(a=%llx,b=%llx)", k_reg[cs[i]], (unsigned long long)va, (unsigned long long)vb); return 1; }
    const uint64_t (*xa)[2] = PAY(ra, PStep)->xmm, (*xb)[2] = PAY(rb, PStep)->xmm;
    for (int i = 0; i < 10; i++) for (int h = 0; h < 2; h++) if (xa[i][h] == va && xb[i][h] == vb) {
        snprintf(o, cap, "xmm%d.%d@step(a=%llx,b=%llx)", 6 + i, h, (unsigned long long)va, (unsigned long long)vb); return 1; }
    uint64_t sa = ra->g.r[RSP], sbp = rb->g.r[RSP];
    if (va >= sa && va - sa < 0x100000 && vb >= sbp && vb - sbp < 0x100000) {
        snprintf(o, cap, "plugin_stack@step(a=rsp+0x%llx,b=rsp+0x%llx)", (unsigned long long)(va - sa), (unsigned long long)(vb - sbp)); return 1; }
    return 0;
}
/* R2: lane mask (bit l = 16-byte lane l): resim bone X lane equals live bone Y lane where live X and Y differ */
static int xlanes(const uint8_t *bx, const uint8_t *ax, const uint8_t *ay) {
    int m = 0;
    for (int l = 0; l < 3; l++) if (memcmp(ax + 16 * l, ay + 16 * l, 16) && !memcmp(bx + 16 * l, ay + 16 * l, 16)) m |= 1 << l;
    return m;
}
/* 9 bits: [0..2] b25=a26, [3..5] b26=a25, [6..8] b26=a30 (W8 follows 26) */
static int xmask(const uint8_t *a25, const uint8_t *a26, const uint8_t *a30, const uint8_t *b25, const uint8_t *b26) {
    return xlanes(b25, a25, a26) | xlanes(b26, a26, a25) << 3 | xlanes(b26, a26, a30) << 6;
}
static void hex(Sb *s, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) sb(s, "%02x", p[i]); }
/* " xc_<site>[.L<layer>]=<b25=a26>,<b26=a25>,<b26=a30>": each group lists the 16-byte lanes (0,1,2) that hold, "-" = none */
static void xc_print(Sb *o, const char *site, int layer, int m) {
    sb(o, " xc_%s", site);
    if (layer != 0xff) sb(o, ".L%d", layer);
    for (int g = 0; g < 3; g++) {
        sb(o, "%s", g ? "," : "=");
        int w = (m >> (3 * g)) & 7;
        if (!w) sb(o, "-");
        for (int l = 0; l < 3; l++) if (w & (1 << l)) sb(o, "%d", l);
    }
}

static void cmp_passes(PassBuf *A, PassBuf *B, uint64_t s, uint64_t k, int bad, const char *why) {
    static char line[6000]; Sb o = { line, 0, sizeof line };
    sb(&o, "L1CMP seq=%llu k=%llu path=%s why=%s bad=%d L3=%d a=live b=%s", (unsigned long long)s, (unsigned long long)k, g_seg_path, why, bad, g_l3_on,
       B->kind == L1P_REPLAY ? "replay" : "resim");
    Rec *sa = nth(A, L1S_STEP, 0), *sbb = nth(B, L1S_STEP, 0), *ea = nth(A, L1S_ENTRY, 0), *eb = nth(B, L1S_ENTRY, 0);
    Rec *xa = nth(A, L1S_EXIT, 0), *xb = nth(B, L1S_EXIT, 0), *ha = nth(A, L1S_HOLD, 0), *hb = nth(B, L1S_HOLD, 0);
    if (!ea || !eb || !xa || !xb) { sb(&o, " verdict=incomplete recs=%d/%d entry=%d/%d exit=%d/%d", A->nrec, B->nrec, !!ea, !!eb, !!xa, !!xb); LOG("%s\n", line); return; }
    PEntry *pa = PAY(ea, PEntry), *pb = PAY(eb, PEntry);
    /* registers */
    char regs[200] = ""; size_t ro = 0;
    if (sa && sbb) for (int i = 0; i < 16; i++) if (sa->g.r[i] != sbb->g.r[i] && ro < sizeof regs - 8) ro += (size_t)snprintf(regs + ro, sizeof regs - ro, "%s%s", ro ? "," : "", k_reg[i]);
    if (sa && sbb && memcmp(PAY(sa, PStep)->xmm, PAY(sbb, PStep)->xmm, sizeof PAY(sa, PStep)->xmm) && ro < sizeof regs - 8) ro += (size_t)snprintf(regs + ro, sizeof regs - ro, "%sxmm6-15", ro ? "," : "");
    sb(&o, " step_regs=%s", ro ? regs : "-");
    ro = 0; regs[0] = 0;
    for (int i = 0; i < 16; i++) if (ea->g.r[i] != eb->g.r[i] && ro < sizeof regs - 8) ro += (size_t)snprintf(regs + ro, sizeof regs - ro, "%s%s", ro ? "," : "", k_reg[i]);
    sb(&o, " entry_regs=%s", ro ? regs : "-");
    if (ea->g.r[RSP] != eb->g.r[RSP]) sb(&o, " entry_rsp=%llx/%llx", (unsigned long long)ea->g.r[RSP], (unsigned long long)eb->g.r[RSP]);
    /* memory inputs at entry */
    int upstream = 0, addr = 0;
    char up[400] = ""; Sb u = { up, 0, sizeof up };   /* R4: every append is clamped by sb() */
    for (size_t i = 0; i < sizeof pa->pl; i++) if (pa->pl[i] != pb->pl[i]) {
        char nm[64]; pl_name(PL_LO + i, nm, sizeof nm);
        int isptr = ((PL_LO + i - 0x967a0) % 0xc0) / 8 == 1 && PL_LO + i >= 0x967a0 && PL_LO + i < 0x96aa0;
        if (isptr) addr = 1; else upstream = 1;
        if (u.n < sizeof up - 80) sb(&u, "%s%s:%02x/%02x", u.n ? "," : "", nm, pa->pl[i], pb->pl[i]);
        size_t skip = i; while (skip + 1 < sizeof pa->pl && pa->pl[skip + 1] != pb->pl[skip + 1]) skip++;
        i = skip;
    }
    if (pa->wtype != pb->wtype) { upstream = 1; sb(&u, "%swtype:%u/%u", u.n ? "," : "", pa->wtype, pb->wtype); }
    if (pa->b1704 != pb->b1704) { upstream = 1; sb(&u, "%s+0x1704", u.n ? "," : ""); }
    if (pa->p78 != pb->p78) { upstream = 1; sb(&u, "%s+0x78", u.n ? "," : ""); }
    if (memcmp(pa->glob, pb->glob, sizeof pa->glob) || pa->g19c != pb->g19c) { upstream = 1; sb(&u, "%score_globals", u.n ? "," : ""); }
    for (int i = 0; i < g_cfg.l1_nextra && i < 4; i++) if (memcmp(pa->extra[i], pb->extra[i], 64)) { upstream = 1; sb(&u, "%sextra%d(0x%llx)", u.n ? "," : "", i, (unsigned long long)g_cfg.l1_extra_va[i]); }
    for (int n = 0; n < 5; n++) {
        if (pa->clip[n].motion != pb->clip[n].motion) addr = 1;
        if (pa->clip[n].flags != pb->clip[n].flags || memcmp(pa->clip[n].par, pb->clip[n].par, 9)) { upstream = 1; sb(&u, "%sclip%d", u.n ? "," : "", n); }
    }
    if (pa->hpose != pb->hpose) { upstream = 1; sb(&u, "%spose_rows", u.n ? "," : ""); }
    if (pa->hbank != pb->hbank) { upstream = 1; sb(&u, "%sbank_rows", u.n ? "," : ""); }
    if (ea->g.r[RDX] != eb->g.r[RDX] || pa->p96f60 != pb->p96f60) addr = 1;
    sb(&o, " inputs=%s ptrs=%s", u.n ? up : "eq", addr ? "differ" : "eq");
    /* residue at entry */
    int rf = memcmp(pa->resid, pb->resid, 0x10) != 0, rl = memcmp(pa->resid + 0x20, pb->resid + 0x20, 0x24) != 0, nblk = 0;
    char rs[64] = ""; size_t so = 0;
    for (int b = 0; b < 9; b++) if (memcmp(pa->samp[b], pb->samp[b], 0x30) && so < sizeof rs - 6) so += (size_t)snprintf(rs + so, sizeof rs - so, "%s%d", so ? "." : "", 23 + b);
    for (int i = 0; i < NBLK; i++) nblk += pa->blk[i] != pb->blk[i];
    int resid = rf || rl || so;
    sb(&o, " resid_flags=%s resid_l198=%s resid_samp=%s resid_blocks=%d", rf ? "differ" : "eq", rl ? "differ" : "eq", so ? rs : "eq", nblk);
    if (rf) { sb(&o, " flags_a="); hex(&o, pa->resid, 0x10); sb(&o, " flags_b="); hex(&o, pb->resid, 0x10); }
    char rm[128] = ""; int match = 0;   /* R1: tracking match only (ctx_match) */
    for (int q = 0; q < 9 && !match; q++) { uint64_t v, w; memcpy(&v, pa->resid + 8 * q, 8); memcpy(&w, pb->resid + 8 * q, 8);
        if (ctx_match(A, B, v, w, rm, sizeof rm)) { match = 1; size_t l = strlen(rm); snprintf(rm + l, sizeof rm - l, "@resid+0x%x", 8 * q); } }
    for (int q = 0; q < 54 && !match; q++) { uint64_t v, w; memcpy(&v, (uint8_t *)pa->samp + 8 * q, 8); memcpy(&w, (uint8_t *)pb->samp + 8 * q, 8);
        if (ctx_match(A, B, v, w, rm, sizeof rm)) { match = 1; size_t l = strlen(rm); snprintf(rm + l, sizeof rm - l, "@samp%d+0x%x", 23 + q / 6, 8 * (q % 6)); } }
    sb(&o, " resid_reg=%s", match ? rm : "-");
    /* R2: exchange flags per lane, in record order: ENTRY residue, each SAMP record (sampler, layer), each MASK record */
    char xf[40] = "-"; int xchg = 0;
    {
        int m = xmask(pa->samp[2], pa->samp[3], pa->samp[7], pb->samp[2], pb->samp[3]);
        if (m) { xchg = 1; snprintf(xf, sizeof xf, "ENTRY"); xc_print(&o, "ENTRY", 0xff, m); }
        int cnt[L1S_N] = { 0 }; uint32_t off = 0;
        for (int i = 0; i < A->nrec; i++) {
            Rec *ra = (Rec *)(A->buf + off); off += (uint32_t)((sizeof(Rec) + ra->len + 7) & ~7u);
            int st = ra->site;
            if (ra->ord != 0 || st < L1S_SAMP1 || st > L1S_MASK) continue;
            Rec *rb = nth(B, st, cnt[st]++);
            if (!rb) continue;
            if (st == L1S_MASK) { PMask *qa = PAY(ra, PMask), *qb = PAY(rb, PMask); m = xmask(qa->s[2], qa->s[3], qa->s[7], qb->s[2], qb->s[3]); }
            else { PSamp *qa = PAY(ra, PSamp), *qb = PAY(rb, PSamp); m = xmask(qa->s[0], qa->s[1], qa->s[2], qb->s[0], qb->s[1]); }
            if (!m) continue;
            if (!xchg) { if (ra->layer == 0xff) snprintf(xf, sizeof xf, "%s", k_site[st].name); else snprintf(xf, sizeof xf, "%s.L%d", k_site[st].name, ra->layer); }
            xchg = 1; xc_print(&o, k_site[st].name, ra->layer, m);
        }
    }
    sb(&o, " xchg_first=%s", xf);
    /* per-layer records */
    int nm = count(A, L1S_MASK), nmb = count(B, L1S_MASK), mask_samp = 0;
    sb(&o, " layers=%d/%d calls=%d/%d", nm, nmb, calls(A), calls(B));
    for (int i = 0; i < nm && i < nmb; i++) {
        Rec *ma = nth(A, L1S_MASK, i), *mb = nth(B, L1S_MASK, i);
        PMask *qa = PAY(ma, PMask), *qb = PAY(mb, PMask);
        if (ma->g.r[R12] != mb->g.r[R12]) sb(&o, " mask%d=%llx/%llx", ma->layer, (unsigned long long)ma->g.r[R12], (unsigned long long)mb->g.r[R12]);
        char ds[48] = ""; size_t d = 0;
        for (int b = 0; b < 9; b++) if (memcmp(qa->s[b], qb->s[b], 0x30) && d < sizeof ds - 4) d += (size_t)snprintf(ds + d, sizeof ds - d, "%s%d", d ? "." : "", 23 + b);
        if (d) { mask_samp = 1; sb(&o, " samp%d=%s", ma->layer, ds); }
        if (memcmp(qa->s[2], qb->s[2], 0x30) && !memcmp(qb->s[2], qa->s[3], 0x30) && !memcmp(qb->s[3], qa->s[2], 0x30)) sb(&o, " locals_exchanged_L%d=1", ma->layer);   /* full 48-byte, both directions (xchg= also counts partial) */
    }
    for (int sidx = L1S_SAMP1; sidx <= L1S_SAMP3; sidx++) {
        int na = count(A, sidx), nb = count(B, sidx);
        if (na != nb) sb(&o, " n%s=%d/%d", k_site[sidx].name, na, nb);
        for (int i = 0; i < na && i < nb; i++) {
            Rec *ra = nth(A, sidx, i), *rb = nth(B, sidx, i);
            if (ra->g.r[RAX] != rb->g.r[RAX]) sb(&o, " %s_rax%d=%llx/%llx", k_site[sidx].name, ra->layer, (unsigned long long)ra->g.r[RAX], (unsigned long long)rb->g.r[RAX]);
        }
    }
    int nl = count(A, L1S_LOOPX), nlb = count(B, L1S_LOOPX), early = 0;
    sb(&o, " loopx_rdi=");
    for (int i = 0; i < nl || i < nlb; i++) {
        Rec *la = i < nl ? nth(A, L1S_LOOPX, i) : NULL, *lb = i < nlb ? nth(B, L1S_LOOPX, i) : NULL;
        uint64_t va = la ? la->g.r[RDI] : 99, vb = lb ? lb->g.r[RDI] : 99;
        if (va < 32 || vb < 32) early = 1;
        sb(&o, "%sL%d:%llu/%llu", i ? "," : "", la ? la->layer : lb->layer, (unsigned long long)va, (unsigned long long)vb);
    }
    int hf = 0, hl = 0, hbk = 0, hps = 0;
    if (ha && hb) {
        PHold *qa = PAY(ha, PHold), *qb = PAY(hb, PHold);
        hf = memcmp(qa->flags, qb->flags, 0x10) != 0; hl = memcmp(qa->l198, qb->l198, 0x24) != 0;
        for (int i = 0; i < 3; i++) { hbk |= (memcmp(qa->bank[i], qb->bank[i], 0x40) != 0) << i; hps |= (memcmp(qa->pose[i], qb->pose[i], 0x40) != 0) << i; }
        sb(&o, " hold_flags=");
        if (hf) { hex(&o, qa->flags + 1, 9); sb(&o, "/"); hex(&o, qb->flags + 1, 9); } else { sb(&o, "eq:"); hex(&o, qa->flags + 1, 9); }
        sb(&o, " hold_l198=%s hold_bank=%s%s%s hold_pose=%s%s%s", hl ? "differ" : "eq", hbk & 1 ? "W3" : "", hbk & 2 ? "W4" : "", hbk & 4 ? "W8" : "", hps & 1 ? "W3" : "", hps & 2 ? "W4" : "", hps & 4 ? "W8" : "");
    } else sb(&o, " hold=missing");
    PExit *qa = PAY(xa, PExit), *qb = PAY(xb, PExit);
    int xd = 0;
    for (int i = 0; i < 3; i++) xd |= (memcmp(qa->pose[i], qb->pose[i], 0x40) != 0) << i;
    int xx = memcmp(qa->pose[0], qb->pose[0], 0x40) && !memcmp(qb->pose[0], qa->pose[1], 0x40);
    sb(&o, " exit=%s%s%s%s exit_b_W3_eq_a_W4=%d", xd ? "" : "eq", xd & 1 ? "W3" : "", xd & 2 ? "W4" : "", xd & 4 ? "W8" : "", xx);
    if (xd & 1) { int own = !memcmp(qb->pose[0], pa->pl + (0x965e0 - PL_LO), 0x40); sb(&o, " b_W3_eq_own_entry=%d", own); }
    const char *v;
    if (upstream) v = "c_upstream_input";
    else if (addr) v = "d_address_or_pointer";
    else if (resid && match) v = "e_call_context";
    else if (resid && hf) v = early ? "a_hold_flag_unwritten" : "a_hold_flag";
    else if (resid && mask_samp) v = "a2_sample_buffer_residue";
    else if (resid) v = "stack_residue_unread";
    else if (hl) v = "b_clip_parent";
    else if (mask_samp || hf || hbk || hps || xd) v = "inside_function";
    else if (bad) v = "later_writer";
    else v = "equal";
    sb(&o, " xchg=%d verdict=%s", xchg, v);
    LOG("%s\n", line);
}

static void dump_pass(PassBuf *pb, const char *tag) {
    if (!pb) return;
    static const char *kn[4] = { "-", "live", "resim", "replay" };
    uint32_t off = 0;
    for (int i = 0; i < pb->nrec; i++) {
        Rec *r = (Rec *)(pb->buf + off);
        off += (uint32_t)((sizeof(Rec) + r->len + 7) & ~7u);
        char g[16 * 17 + 32]; size_t go = 0;
        for (int j = 0; j < 16; j++) go += (size_t)snprintf(g + go, sizeof g - go, "%s%llx", j ? "," : "", (unsigned long long)r->g.r[j]);
        snprintf(g + go, sizeof g - go, ",fl=%llx", (unsigned long long)r->g.fl);
        const uint8_t *p = (const uint8_t *)(r + 1);
        uint32_t parts = (r->len + 1023) / 1024; if (!parts) parts = 1;
        for (uint32_t q = 0; q < parts; q++) {
            static char hx[2 * 1024 + 1];
            uint32_t a = q * 1024, n = r->len - a < 1024 ? r->len - a : 1024;
            for (uint32_t j = 0; j < n; j++) snprintf(hx + 2 * j, 3, "%02x", p[a + j]);
            hx[2 * n] = 0;
            LOG("L1R %s seq=%llu k=%llu pass=%s site=%s ord=%u layer=%u part=%u/%u%s%s data=%s\n", tag, (unsigned long long)pb->seq, (unsigned long long)pb->k,
                kn[pb->kind & 3], k_site[r->site].name, r->ord, r->layer, q + 1, parts, q ? "" : " gpr=", q ? "" : g, hx);
        }
    }
}

void l1_segment(const char *why, const char *path, int depth) {
    if (!g_l1_ok) return;
    LOG("L1SEG why=%s path=%s depth=%d L3=%d prev_path=%s prev_frames=%d prev_bad=%d prev_E=%llu walk_E=%llu walk_phase=%llu cmp_lines=%d full_dumps=%d\n", why, path, depth, g_l3_on,
        g_seg_path[0] ? g_seg_path : "-", g_seg_frames, g_seg_bad, (unsigned long long)g_seg_E, (unsigned long long)g_walk_E, (unsigned long long)(g_walk_E % 31), g_seg_cmp, g_seg_full);
    snprintf(g_seg_path, sizeof g_seg_path, "%s", path);
    g_seg_E = 0; g_seg_cmp = g_seg_full = g_seg_bad = g_seg_frames = g_seg_every = 0;
}

void l1_check(uint64_t s, uint64_t k, int bad, const char *path) {
    if (!l1_active()) return;
    if (!g_seg_path[0] || strcmp(g_seg_path, path)) l1_segment("path_seen", path, 0);
    if (bad) {
        g_seg_bad++; if (!g_seg_E) g_seg_E = k;
        if (!g_last_bad || k > g_last_bad + 3) g_walk_E = k;   /* a new bad run starts: re-anchor the phase */
        if (k > g_last_bad) g_last_bad = k;
    }
    /* R3: predicted marker frames come from the global phase, so a clean segment (sync) after a bad one gets them too */
    int pred = g_walk_E && k > g_walk_E && ((k - g_walk_E) % 31 == 0);
    int predm1 = g_walk_E && k > g_walk_E && ((k - g_walk_E) % 31 == 30);
    int every = g_cfg.l1_cmp_every > 0 && k % (uint64_t)g_cfg.l1_cmp_every == 0 && !bad && !pred && !predm1 && g_seg_every < 20;   /* clean controls: own budget (20 per segment) */
    if (every) g_seg_every++;
    if (!(bad || pred || predm1 || every)) return;
    PassBuf *A = live_of(k), *B = res_of(k);
    if (!A || !B) { if (bad && g_seg_cmp < g_cfg.l1_cmp_budget) { g_seg_cmp++; LOG("L1CMP seq=%llu k=%llu path=%s bad=%d verdict=no_records live=%d res=%d\n", (unsigned long long)s, (unsigned long long)k, path, bad, !!A, !!B); } return; }
    if (g_seg_cmp < g_cfg.l1_cmp_budget) { g_seg_cmp++; cmp_passes(A, B, s, k, bad, bad ? "bad" : pred ? "predicted" : predm1 ? "predicted_k-1" : "every"); }
    if ((bad || pred) && g_seg_full < g_cfg.l1_full_budget) {
        g_seg_full++;
        char tag[32]; snprintf(tag, sizeof tag, "%s", bad ? "bad" : "pred");
        dump_pass(A, tag); dump_pass(B, tag);
        if (k > 0) dump_pass(live_of(k - 1), "km1");
    }
}

static void wxy(char *o, size_t cap, PassBuf *pb) {
    Rec *x = pb ? nth(pb, L1S_EXIT, 0) : NULL;
    if (!x) { snprintf(o, cap, "none"); return; }
    PExit *p = PAY(x, PExit); size_t n = 0;
    for (int i = 0; i < 3; i++) { uint32_t a, b; memcpy(&a, p->pose[i] + 0x30, 4); memcpy(&b, p->pose[i] + 0x34, 4); n += (size_t)snprintf(o + n, cap - n, "%s%s:%08x,%08x", i ? " " : "", k_wname[i], a, b); }
}
void l1_frame_end(uint64_t s, const char *path, const char *lsn) {
    if (!l1_active()) return;
    g_seg_frames++;
    PassBuf *A = live_of(s), *B = res_of(s);
    char a[96], b[96];
    wxy(a, sizeof a, A);
    Rec *xa = A ? nth(A, L1S_EXIT, 0) : NULL, *xb = B ? nth(B, L1S_EXIT, 0) : NULL;
    if (xa && xb && !memcmp(PAY(xa, PExit)->pose, PAY(xb, PExit)->pose, sizeof PAY(xa, PExit)->pose)) snprintf(b, sizeof b, "eq");
    else wxy(b, sizeof b, B);
    LOG("L1W seq=%llu path=%s L3=%d live=%s res=%s lsn=%s recs=%d/%d\n", (unsigned long long)s, path, g_l3_on, a, b, lsn ? lsn : "-", A ? A->nrec : 0, B ? B->nrec : 0);
}

size_t l3_zero_bytes(void) {
    size_t want = (size_t)(g_cfg.l3_zero_kb > 0 ? g_cfg.l3_zero_kb : 256) * 1024;
    if (g_depth_max + 64 * 1024 > want) want = (size_t)(g_depth_max + 64 * 1024);
    want = (want + 4095) & ~(size_t)4095;
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t lo = *(uintptr_t *)((uint8_t *)NtCurrentTeb() + 0x1478);   /* TEB DeallocationStack: bottom of the reserve */
    if (lo && sp > lo + 256 * 1024 && want > sp - lo - 256 * 1024) {
        static int warned;
        if (!warned++) LOG("L3 WARN zero size clamped to the stack reserve: want=%zu avail=%zu\n", want, (size_t)(sp - lo - 256 * 1024));
        want = (sp - lo - 256 * 1024) & ~(size_t)4095;
    }
    return want;
}

void l1_report(const char *why) {
    if (!g_l1_ok) return;
    LOG("L1STAT why=%s active=%d hits=STEP:%llu,ENTRY:%llu,SAMP1:%llu,SAMP2:%llu,SAMP3:%llu,MASK:%llu,LOOPX:%llu,HOLD:%llu,EXIT:%llu filtered_p1=%llu unreadable=%llu overflows=%llu edb90_depth_max=%llu L3=%d l3_zero_bytes=%zu\n",
        why, l1_active(), (unsigned long long)g_hits[0], (unsigned long long)g_hits[1], (unsigned long long)g_hits[2], (unsigned long long)g_hits[3],
        (unsigned long long)g_hits[4], (unsigned long long)g_hits[5], (unsigned long long)g_hits[6], (unsigned long long)g_hits[7], (unsigned long long)g_hits[8],
        (unsigned long long)g_filtered, (unsigned long long)g_unreadable, (unsigned long long)g_overflows, (unsigned long long)g_depth_max, g_l3_on, l3_zero_bytes());
}
