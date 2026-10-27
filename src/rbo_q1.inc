/* rbo_q1.inc — the Q1 root walk (OPTA-M2-SPEC.md §16.1, §16.3; v0.29.2-optA-M2). Part of the rollback core. #included from src/modes.c only.
 *
 * What it is: a typed walk over a FIXED table of static root paths (src/rbo_q1_table.h). Each path starts in memory the snapshot set
 * already restores (core .bss, the `single` region, the PLAYER structs, WorkMemory) and ends at heap blocks the battle core's step
 * writes: E1 the wind emitters' ref-count blocks, E2 the wind manager's objects, E3 the 29 key-record vectors, E4 KN_EnshutsuHeader
 * (presentation), E6 the PLAYER std::map. The walk checks each block's type, links and size against code constants (checked at process
 * start by q1_sig_check) and object fields, and returns the ranges to pin. The caller (modes.c do_pin_scan) pins them.
 *
 * SELF-CONTAINED ON PURPOSE (tools/q1_static.py QS3 compiles this file alone through a wrapper TU and lists its undefined symbols):
 *   - reads only, every read preceded by a VirtualQuery check (q1_rd); every range it returns is also writable (q1_wr: RW or RWX);
 *   - no allocator of any kind, no size query (Heap*, _msize, GetAllocationSize, the GMalloc vtable), no CRT call, no game-code call,
 *     no function pointer built from VA(); no write outside its own static arrays and the caller's out arrays;
 *   - undefined symbols: VirtualQuery, check_list / check_tree (src/scan.c), arena_owns (src/arena.c), g_base / g_image_size (VA()),
 *     and under SC6RB_HARNESS nothing more (the harness symbols live in modes.c). No LOG here: the caller formats every line.
 * Every gameplay entry (E1 E2 E3 E6) fails closed (the caller refuses the snapshot set); E4 (presentation) is skipped on any failure.
 * v0.29.4 Q1 (Q1, OPTA-M2-SPEC.md §17): three more entries, appended after E6 so the existing skip-mask bits and the
 * walk order of E1..E6 stay as they were:
 *   E5  the vtb voice tables (gameplay, fails closed): PLAYER[p] + 0x95788 + 0x268 k owns a sequencer object whose [+0x260] is a CRT block the
 *       step writes (the round-robin cursor, 0x14038f204) and whose state feeds the TT800 retry loop of the random pick (0x14038f216). The S-M2a
 *       L2 drift (DECISIONS item 5: L2P EQUAL = a coverage gap). Sized by the game's own formula from the asset header, after 7 cross-checks.
 *   E7  the container census (gameplay, fails closed): every std::list / std::map the container scan found (Q1Env.census) must be a known row:
 *       whole pins for the two lists whose heap parts the step writes (HeaderGaugeResource, StageWindParam), the scan's link pins for the
 *       read-only ones; an unknown container with heap nodes outside the snapshot fails closed (unknown_class), an empty one keeps its links.
 *   E8  the controllers' CPU direct-command objects (controller + 0x50, HgCpuDirectStand 0x68): never written by the step on the S-M2a recording
 *       (Q1COV, L1 and L2P: V), so a failure skips the entry like E4; pinned so the tier-1 `ctl` pointer units normalize as in a track_init
 *       recording (DECISIONS item 5: L1's `ctl` differs from the recording on every frame only through these two units, V).
 * v0.29.5 Q1R (design_v0295/q1r/DESIGN.md; review m4, m5): E7 also fails closed on what the container scan could not record (fail=E7:overflow:
 *   more than 512 containers, link pins past the scan's pin array, nodes past its node buffer; fail=E7:unchecked: an MSVC-shaped container head
 *   whose walk broke or ran past LIST_LIMIT, src/scan.c q1r_note). E5 checks the game's own rules (no alignment rule on the asset, off >= 0x18,
 *   no n / off bounds besides the 2 MiB size bound) plus one pin-coverage rule: the pick's cursor writes must lie inside the pinned table. */
#include "common.h"
#include "scan.h"
#include "arena.h"

enum { Q1E_E1 = 0, Q1E_E2, Q1E_E3, Q1E_E4, Q1E_E6, Q1E_E5, Q1E_E7, Q1E_E8, Q1E_N };   /* v0.29.4 Q1: + E5 E7 E8 (appended: the old bits stay) */
enum { Q1W_OK = 0, Q1W_ROOT, Q1W_OWNER, Q1W_LINK, Q1W_CYCLE, Q1W_COUNT, Q1W_VTABLE, Q1W_SIZE, Q1W_UNKNOWN_CLASS, Q1W_UNREADABLE, Q1W_UNWRITABLE,
       Q1W_BOUND, Q1W_OVERFLOW, Q1W_UNCHECKED, Q1W_N };   /* v0.29.5 Q1R (m4): + overflow, unchecked (E7; appended) */
static const char *const k_q1_why[Q1W_N] = { "-", "root", "owner", "link", "cycle", "count", "vtable", "size", "unknown_class", "unreadable",
                                             "unwritable", "bound", "overflow", "unchecked" };
static const char *const k_q1_ent[Q1E_N] = { "E1", "E2", "E3", "E4", "E6", "E5", "E7", "E8" };
static int q1_skippable(int i) { return i == Q1E_E4 || i == Q1E_E8; }   /* v0.29.4 Q1: a failure skips the entry (not a gameplay failure) */

/* ------------------------------------------------------------------ the roots and bounds (§16.3; every VA is preferred-base, through VA()) */
#define Q1_A_EMIT_SENT    0x14470f1c0ULL   /* E1: std::list<shared_ptr<Iw_WindEmitter>> sentinel pointer (E1.root row) */
#define Q1_A_EMIT_SIZE    0x14470f1c8ULL   /*     its size (E1.count row) */
#define Q1_A_WIND_MGR     0x14470e038ULL   /* E2: the wind manager pointer (E2.mgr_store row) */
#define Q1_A_WIND_MGR_OBJ 0x144100b60ULL   /*     the only value it may hold (E2.mgr_lea row; inside the `single` region) */
#define Q1_A_WORKMEM      0x144843f00ULL   /* E3: WorkMemory (A_WORKMEM_PTR) */
#define Q1_E3_MGR_OFF     0xaa120ULL       /*     the key-record manager inside WorkMemory (E3.mgr row) */
#define Q1_A_PLAYER0      0x1447156f0ULL
#define Q1_PLAYER_STRIDE  0x973f0ULL
#define Q1_E4_OFF         0x95750ULL       /* E4: PLAYER + this = the KN_EnshutsuHeader pointer (E4.store row: the manager at +0x95720, +0x30) */
#define Q1_E6_OFF         0x3590ULL        /* E6: PLAYER + this = the std::map object (E6.store row: rbx = PLAYER+0x3570, +0x20) */
#define Q1_E1_MAX   64
#define Q1_E2_MAX   1024
#define Q1_E4_MAX   256
#define Q1_E6_MAX   4096
#define Q1_NODE     0x20                   /* std::list node (E1.node_size row) */
#define Q1_TNODE    0x38                   /* std::map tree node (E6.node_size row; the head comes from the same allocator, E6.head row) */
#define Q1_MAPOBJ   0x10                   /* the map object (E6.obj_size row) */
/* v0.29.4 Q1: E5 (the vtb voice tables) */
#define Q1_E5_OBJ0    0x95788ULL           /* PLAYER + this = sequencer object 0 (E5.own0 row; E5.own1 = +0x268, E5.own2 = +0x4d0) */
#define Q1_E5_STRIDE  0x268ULL
#define Q1_E5_MAGIC   0x00627476u          /* "vtb" + NUL (E5.magic row, compared as 4 bytes like the code's own compare) */
#define Q1_E5_VER     0x1002u              /* E5.ver row */
#define Q1_E5_EVT     0x84u                /* one event entry (E5.mul row) */
#define Q1_E5_MAX     0x200000u            /* a table larger than 2 MiB is not a vtb table (fails closed: size) */
/* v0.29.4 Q1: E7 (the census) and E8 (the controllers) */
#define Q1_E7_GAUGE_CTRL  0x18u            /* _Ref_count<HeaderGaugeResource> (E7.p_ctrl_size row) */
#define Q1_E7_GAUGE_OBJ   0x10u            /* the gauge object (E7.g_size row: 5 + 0xb) */
#define Q1_E7_WIND_CTRL   0x60u            /* _Ref_count_obj<StageWindParam> (E7.q_ctrl_size row) */
#define Q1_E7_MAX     4096                 /* nodes per whole-pinned list */
#define Q1_A_CTL0     0x144715400ULL       /* E8: the player controllers (E8.ctl_lea row), stride 0x60, the direct-command object at +0x50 (E8.store) */
#define Q1_CTL_STRIDE 0x60ULL

/* the vtables / sizes of the table (Q1VT rows) */
typedef struct { const char *entry, *rtti; uint64_t vt; uint32_t size; const char *size_sig; } Q1Vt;
static const Q1Vt k_q1vt[] = {
#define Q1SIG(name, va, hex)
#define Q1VT(entry, rtti, vt, size, size_sig) { entry, rtti, vt, size, size_sig },
#include "rbo_q1_table.h"
#undef Q1SIG
#undef Q1VT
};
#define Q1_NVT ((int)(sizeof k_q1vt / sizeof k_q1vt[0]))
typedef struct { const char *name; uint64_t va; const char *hex; } Q1Sig;
static const Q1Sig k_q1sig[] = {
#define Q1SIG(name, va, hex) { name, va, hex },
#define Q1VT(entry, rtti, vt, size, size_sig)
#include "rbo_q1_table.h"
#undef Q1SIG
#undef Q1VT
};
#define Q1_NSIG ((int)(sizeof k_q1sig / sizeof k_q1sig[0]))
/* v0.29.3 (review B1): the loader-relocated pointer rows (Q1PTR): [VA(va)] == VA(target) at the module's real base */
typedef struct { const char *name; uint64_t va, target; } Q1Ptr;
static const Q1Ptr k_q1ptr[] = {
#define Q1PTR(name, va, target) { name, va, target },
#include "rbo_q1_table.h"
#undef Q1PTR
};
#define Q1_NPTR ((int)(sizeof k_q1ptr / sizeof k_q1ptr[0]))

static int q1_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static const Q1Vt *q1_vt_of(const char *entry, uint64_t vt_pref) {   /* the row of entry with this vtable (preferred-base VA), or NULL */
    for (int i = 0; i < Q1_NVT; i++) if (k_q1vt[i].vt == vt_pref && q1_streq(k_q1vt[i].entry, entry)) return &k_q1vt[i];
    return NULL;
}
static uint64_t q1_vt_first(const char *entry) { for (int i = 0; i < Q1_NVT; i++) if (q1_streq(k_q1vt[i].entry, entry)) return k_q1vt[i].vt; return 0; }

/* ------------------------------------------------------------------ memory checks (VirtualQuery; the region of every byte) */
static int q1_plaus(uintptr_t v) { return v >= 0x10000 && v < 0x7ffffffeffffULL && !(v & 7); }
static int q1_query(uintptr_t a, size_t n, int need_write) {
    if (!n || a < 0x10000 || a > 0x7ffffffeffffULL || a + n < a || a + n > 0x7fffffff0000ULL) return 0;
    uintptr_t end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        DWORD pr = mbi.Protect & 0xff;
        if (need_write) { if (pr != PAGE_READWRITE && pr != PAGE_EXECUTE_READWRITE) return 0; }
        else if (!(pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        uintptr_t nx = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (nx <= a) return 0;
        a = nx;
    }
    return 1;
}
static int q1_rd(uintptr_t a, size_t n) { return q1_query(a, n, 0); }
static int q1_wr(uintptr_t a, size_t n) { return q1_query(a, n, 1); }
static uint64_t q1_u64(uintptr_t a) { return *(volatile const uint64_t *)a; }   /* only after q1_rd(a, >= 8) */
/* v0.29.5 Q1R (m5): a u32 at any alignment (byte loads: the game reads the vtb asset header with plain unaligned-tolerant loads); only after q1_rd */
static uint32_t q1_u32u(uintptr_t a) {
    const volatile uint8_t *p = (const volatile uint8_t *)a;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ------------------------------------------------------------------ the result */
typedef struct {
    int st;                 /* 0 walked, 1 absent, 2 skipped (E4 failure, or the harness key), 3 failed (gameplay) */
    int why;                /* Q1W_* of the failure / skip */
    uint32_t a, b;          /* E1 nodes / ctrl; E2 objects / pinned; E3 vectors / -; E4 headers / nodes; E6 nodes / - */
    uint32_t grown;         /* E3: vectors whose capacity differs from the code's reserve (information, never a failure) */
    uint64_t bytes;         /* bytes this entry pins */
    int harness_skip;       /* skipped by h_rbo_q1_skip (harness builds only) */
} Q1Ent;
#define Q1_E5_TAB 6
#define Q1_UNK_MAX 8
typedef struct {
    Q1Ent e[Q1E_N];
    int ok;                 /* 1 = every gameplay entry walked or absent */
    int fail_ent, fail_why; /* the first failed gameplay entry (Q1E_*) and why (Q1W_*); -1 / 0 = none */
    uint32_t arena_skipped; /* arena-owned blocks the walk traversed but did not pin (the arena snapshot covers them) */
    uint32_t pins; uint64_t bytes;
    /* v0.29.4 Q1: the E5 tables found (the caller's E5CHK and Q1COV owner names), E7's unknown containers (the caller's Q1 CONT lines) */
    uint32_t e5_n; uintptr_t e5_a[Q1_E5_TAB]; uint32_t e5_len[Q1_E5_TAB]; uint8_t e5_slot[Q1_E5_TAB];   /* slot = p * 3 + k */
    uint32_t unk_n, unk_heap;   /* unknown containers (all), of them with heap nodes outside the snapshot */
    struct { uintptr_t obj; uint64_t size; int kind, nodes_out; } unk[Q1_UNK_MAX];
} Q1Res;
typedef struct {
    const Arena *arena;     /* NULL = no arena (nothing is arena-owned) */
    uint32_t skip_mask;     /* bit Q1E_* = do not walk that entry (h_rbo_q1_skip; 0 in the plugin) */
    const FoundCont *census; int ncensus;   /* v0.29.4 Q1: the container scan's finds of this pin scan (E7); NULL = no census (E7 absent) */
    /* v0.29.5 Q1R (m4, m5): ncensus_total = the scan's container count (ncensus = the recorded ones: at most 512); scan = what the scan could
     * not record (scan.h ScanQ1Stat; NULL = none known); h_old = harness-only controls (Q1R_OLD_*: v0.29.4's E7 / E5 rules), 0 in the plugin */
    int ncensus_total; const ScanQ1Stat *scan; int h_old;
} Q1Env;
#define Q1R_OLD_E7 1   /* v0.29.5 Q1R (harness only): E7 without the overflow / unchecked gate (v0.29.4) */
#define Q1R_OLD_E5 2   /* v0.29.5 Q1R (harness only): v0.29.4's E5 rules (A 8-aligned, 0x18 < off <= 0x10000, 1 <= n <= 0x4000, no write-span check) */
/* the out array (the caller's): every range to pin, with its entry */
typedef struct { SPin *pins; uint8_t *tag; int n, max, overflow; } Q1Out;

static int q1_arena(const Q1Env *env, uintptr_t p) { return env->arena && arena_owns(env->arena, (const void *)p); }
static int q1_put(Q1Out *o, uintptr_t a, uint32_t len, int ent) {
    if (o->n >= o->max) { o->overflow = 1; return 0; }
    o->pins[o->n].addr = a; o->pins[o->n].len = len; if (o->tag) o->tag[o->n] = (uint8_t)ent; o->n++;
    return 1;
}
/* a pin candidate: readable and writable, else the entry fails (unreadable / unwritable); out of room = bound */
static int q1_pin(Q1Out *o, uintptr_t a, uint32_t len, int ent, Q1Ent *e) {
    if (!q1_rd(a, len)) return Q1W_UNREADABLE;
    if (!q1_wr(a, len)) return Q1W_UNWRITABLE;
    if (!q1_put(o, a, len, ent)) return Q1W_BOUND;
    e->bytes += len;
    return Q1W_OK;
}

/* ------------------------------------------------------------------ E1: the wind emitters (gameplay) */
static int q1_e1(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask) {
    static uintptr_t nodes[Q1_E1_MAX];
    uintptr_t rs = VA(Q1_A_EMIT_SENT), rn = VA(Q1_A_EMIT_SIZE);
    if (!q1_rd(rs, 8) || !q1_rd(rn, 8)) return Q1W_UNREADABLE;
    uintptr_t S = (uintptr_t)q1_u64(rs); uint64_t N = q1_u64(rn);
    if (!S) { e->st = 1; return Q1W_OK; }
    if (!q1_plaus(S) || !q1_rd(S, Q1_NODE)) return Q1W_UNREADABLE;
    uint32_t k = 0;
    for (uintptr_t n = (uintptr_t)q1_u64(S); n != S; ) {
        if (k >= Q1_E1_MAX) return Q1W_BOUND;           /* a ring longer than the bound (a self-loop never returns to S): treated as a cycle */
        if (!q1_plaus(n) || !q1_rd(n, Q1_NODE)) return Q1W_UNREADABLE;
        uintptr_t p = (uintptr_t)q1_u64(n + 8);         /* n.prev.next == n */
        if (!q1_plaus(p) || !q1_rd(p, 8) || (uintptr_t)q1_u64(p) != n) return Q1W_LINK;
        nodes[k++] = n; e->a = k;   /* HQ-7: the count so far, also on a failure line */
        n = (uintptr_t)q1_u64(n);
    }
    if ((uint64_t)k != N) return Q1W_COUNT;
    uint64_t vt = VA(q1_vt_first("E1"));
    const Q1Vt *row = q1_vt_of("E1", q1_vt_first("E1"));
    int w;
    if (!q1_arena(env, S)) { if ((w = q1_pin(o, S, Q1_NODE, Q1E_E1, e)) != Q1W_OK) return w; } else (*ask)++;
    for (uint32_t i = 0; i < k; i++) {
        uintptr_t n = nodes[i];
        uintptr_t ctrl = (uintptr_t)q1_u64(n + 0x18), obj = (uintptr_t)q1_u64(n + 0x10);
        if (!q1_arena(env, n)) { if ((w = q1_pin(o, n, Q1_NODE, Q1E_E1, e)) != Q1W_OK) return w; } else (*ask)++;
        if (q1_plaus(ctrl) && q1_arena(env, ctrl)) { (*ask)++; continue; }   /* an arena ctrl block: traversed, never validated or pinned */
        if (!q1_plaus(ctrl) || !q1_rd(ctrl, row ? row->size : 0xc0)) return Q1W_UNREADABLE;
        if (obj != ctrl + 0x10) return Q1W_LINK;                              /* make_shared: the emitter lives at ctrl + 0x10 */
        if (q1_u64(ctrl) != vt) return Q1W_VTABLE;
        uint32_t uses = *(volatile const uint32_t *)(ctrl + 8);
        if (uses < 1 || uses > 0xffff) return Q1W_COUNT;
        if ((w = q1_pin(o, ctrl, row ? row->size : 0xc0, Q1E_E1, e)) != Q1W_OK) return w;
        e->b++;
    }
    return Q1W_OK;
}

/* ------------------------------------------------------------------ E2: the wind manager's objects (gameplay) */
static int q1_e2(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask) {
    static uintptr_t seen[Q1_E2_MAX];
    uintptr_t rm = VA(Q1_A_WIND_MGR);
    if (!q1_rd(rm, 8)) return Q1W_UNREADABLE;
    uintptr_t M = (uintptr_t)q1_u64(rm);
    if (!M) { e->st = 1; return Q1W_OK; }
    if (M != VA(Q1_A_WIND_MGR_OBJ)) return Q1W_ROOT;
    if (!q1_rd(M, 8)) return Q1W_UNREADABLE;
    uint32_t k = 0;
    for (uintptr_t ob = (uintptr_t)q1_u64(M); ob; ) {
        if (k >= Q1_E2_MAX) return Q1W_BOUND;
        for (uint32_t i = 0; i < k; i++) if (seen[i] == ob) return Q1W_CYCLE;
        if (!q1_plaus(ob) || !q1_rd(ob, 0x30)) return Q1W_UNREADABLE;
        if ((uintptr_t)q1_u64(ob + 0x28) != M) return Q1W_OWNER;
        uintptr_t pv = (uintptr_t)q1_u64(ob + 0x18);
        if (k == 0) { if (pv) return Q1W_LINK; }
        else if (!q1_plaus(pv) || !q1_rd(pv + 0x10, 8) || (uintptr_t)q1_u64(pv + 0x10) != ob) return Q1W_LINK;
        seen[k++] = ob; e->a = k;   /* HQ-7: the count so far, also on a failure line */
        if (q1_arena(env, ob)) (*ask)++;   /* an object allocated while armed: the arena snapshot covers it */
        else {
            const Q1Vt *row = q1_vt_of("E2", (uint64_t)(q1_u64(ob) - g_base + IMG_PREF));
            if (!row) return Q1W_UNKNOWN_CLASS;
            int w = q1_pin(o, ob, row->size, Q1E_E2, e);
            if (w != Q1W_OK) return w;
            e->b++;
        }
        ob = (uintptr_t)q1_u64(ob + 0x10);
    }
    return Q1W_OK;
}

/* ------------------------------------------------------------------ E3: the 29 key-record vectors (gameplay-capable) */
static int q1_e3(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask) {
    static const struct { uint32_t off, step, n; const char *res_sig; } grp[3] = {
        { 0x30, 0x28, 18, "E3.res_key" }, { 0x300, 0x28, 9, "E3.res_parity" }, { 0xf0538, 0x30, 2, "E3.res_ring" } };
    uintptr_t rw = VA(Q1_A_WORKMEM);
    if (!q1_rd(rw, 8)) return Q1W_UNREADABLE;
    uintptr_t WM = (uintptr_t)q1_u64(rw);
    if (!WM) { e->st = 1; return Q1W_OK; }
    uintptr_t R = WM + (uintptr_t)Q1_E3_MGR_OFF;
    if (!q1_rd(R + grp[0].off, 8)) return Q1W_UNREADABLE;
    if (!q1_u64(R + grp[0].off)) { e->st = 1; return Q1W_OK; }   /* not constructed yet */
    for (int g = 0; g < 3; g++) {
        const Q1Vt *row = NULL;
        for (int i = 0; i < Q1_NVT; i++) if (q1_streq(k_q1vt[i].entry, "E3") && q1_streq(k_q1vt[i].size_sig, grp[g].res_sig)) row = &k_q1vt[i];
        if (!row) return Q1W_VTABLE;
        for (uint32_t i = 0; i < grp[g].n; i++) {
            uintptr_t ob = R + grp[g].off + (uintptr_t)grp[g].step * i;
            if (!q1_rd(ob, 0x20)) return Q1W_UNREADABLE;
            if (q1_u64(ob) != VA(row->vt)) return Q1W_VTABLE;
            uintptr_t v = ob + 8;
            uint64_t b = q1_u64(v), en = q1_u64(v + 8), c = q1_u64(v + 0x10);
            if (!b) continue;                                     /* an empty vector */
            if (b > en || en > c || ((c - b) & 1) || c - b < 2 || c - b > 0x400000) return Q1W_SIZE;
            e->a++;
            if (c - b != row->size) e->grown++;                   /* the game grew it: information */
            if (q1_arena(env, (uintptr_t)b)) { (*ask)++; continue; }
            int w = q1_pin(o, (uintptr_t)b, (uint32_t)(c - b), Q1E_E3, e);
            if (w != Q1W_OK) return w;
        }
    }
    return Q1W_OK;
}

/* ------------------------------------------------------------------ E4: KN_EnshutsuHeader (presentation: any failure skips the entry) */
static int q1_list(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask, uintptr_t L, uint64_t N, uint32_t bound, int ent, uint32_t *nodes_out) {
    if (!q1_plaus(L) || !q1_rd(L, Q1_NODE)) return Q1W_UNREADABLE;
    uint32_t k = 0; int w;
    for (uintptr_t n = (uintptr_t)q1_u64(L); n != L; ) {
        if (k >= bound) return Q1W_BOUND;
        if (!q1_plaus(n) || !q1_rd(n, Q1_NODE)) return Q1W_UNREADABLE;
        uintptr_t p = (uintptr_t)q1_u64(n + 8);
        if (!q1_plaus(p) || !q1_rd(p, 8) || (uintptr_t)q1_u64(p) != n) return Q1W_LINK;
        k++;
        uintptr_t nx = (uintptr_t)q1_u64(n);
        if (!q1_arena(env, n)) { if ((w = q1_pin(o, n, Q1_NODE, ent, e)) != Q1W_OK) return w; } else (*ask)++;
        n = nx;
    }
    if ((uint64_t)k != N) return Q1W_COUNT;
    if (!q1_arena(env, L)) { if ((w = q1_pin(o, L, Q1_NODE, ent, e)) != Q1W_OK) return w; } else (*ask)++;
    *nodes_out += k;
    return Q1W_OK;
}
static int q1_e4(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask) {
    uint64_t vt = VA(q1_vt_first("E4"));
    const Q1Vt *row = q1_vt_of("E4", q1_vt_first("E4"));
    int any = 0;
    for (int p = 0; p < 2; p++) {
        uintptr_t slot = VA(Q1_A_PLAYER0) + (uintptr_t)p * (uintptr_t)Q1_PLAYER_STRIDE + (uintptr_t)Q1_E4_OFF;
        if (!q1_rd(slot, 8)) return Q1W_UNREADABLE;
        uintptr_t H = (uintptr_t)q1_u64(slot);
        if (!H) continue;
        any = 1;
        if (q1_arena(env, H)) { (*ask)++; continue; }
        if (!q1_plaus(H) || !q1_rd(H, row ? row->size : 0x80)) return Q1W_UNREADABLE;
        if (q1_u64(H) != vt) return Q1W_VTABLE;
        int w = q1_pin(o, H, row ? row->size : 0x80, Q1E_E4, e);
        if (w != Q1W_OK) return w;
        e->a++;
        if ((w = q1_list(env, o, e, ask, (uintptr_t)q1_u64(H + 0x70), q1_u64(H + 0x78), Q1_E4_MAX, Q1E_E4, &e->b)) != Q1W_OK) return w;
    }
    if (!any) e->st = 1;
    return Q1W_OK;
}

/* ------------------------------------------------------------------ E6: the PLAYER std::map (unknown class: gameplay rules) */
static int q1_e6(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask) {
    static uintptr_t nodes[Q1_E6_MAX];
    int any = 0;
    for (int p = 0; p < 2; p++) {
        uintptr_t slot = VA(Q1_A_PLAYER0) + (uintptr_t)p * (uintptr_t)Q1_PLAYER_STRIDE + (uintptr_t)Q1_E6_OFF;
        if (!q1_rd(slot, 8)) return Q1W_UNREADABLE;
        uintptr_t O = (uintptr_t)q1_u64(slot);
        if (!O) continue;
        any = 1;
        if (!q1_plaus(O) || !q1_rd(O, Q1_MAPOBJ)) return Q1W_UNREADABLE;
        uintptr_t h = (uintptr_t)q1_u64(O); uint64_t n = q1_u64(O + 8);
        if (n > Q1_E6_MAX) return Q1W_BOUND;
        if (!q1_plaus(h) || !q1_rd(h, Q1_TNODE)) return Q1W_UNREADABLE;
        int nn = 0;
        if (!check_tree(h, n, nodes, Q1_E6_MAX, &nn) || (uint64_t)nn != n) return Q1W_LINK;
        int w;
        if (!q1_arena(env, O)) { if ((w = q1_pin(o, O, Q1_MAPOBJ, Q1E_E6, e)) != Q1W_OK) return w; } else (*ask)++;
        if (!q1_arena(env, h)) { if ((w = q1_pin(o, h, Q1_TNODE, Q1E_E6, e)) != Q1W_OK) return w; } else (*ask)++;
        for (int i = 0; i < nn; i++) {
            if (q1_arena(env, nodes[i])) { (*ask)++; continue; }
            if ((w = q1_pin(o, nodes[i], Q1_TNODE, Q1E_E6, e)) != Q1W_OK) return w;
        }
        e->a += (uint32_t)nn;
    }
    if (!any) e->st = 1;
    return Q1W_OK;
}

/* ------------------------------------------------------------------ v0.29.4 Q1: E5, the vtb voice tables (gameplay).
 * PLAYER[p] + 0x95788 + 0x268 k (k = 0..2; E5.own0..2 rows) is a sequencer object: +0x240 the asset (UE data, read by every pick: E5.groups),
 * +0x248 the group table, +0x250 the event table, +0x258 active, +0x25c the last chosen variant, +0x260 the table (a CRT block, E5.store).
 * The loader 0x14038efa0 checks the asset ("vtb", version 0x1002), allocates n * 0x84 + off - 0x18 bytes (E5.hdr / mul / add), copies the asset
 * from +0x18 into it and points +0x248 at it and +0x250 at it + off - 0x18. The walk re-derives exactly that: 7 cross-checks, then one pin. */
static Q1Res *s_q1r;   /* the result being filled (q1_walk_run): E5's tables and E7's unknown containers go there */
static int q1_e5(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask) {
    int any = 0;
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 3; k++) {
            uintptr_t obj = VA(Q1_A_PLAYER0) + (uintptr_t)p * (uintptr_t)Q1_PLAYER_STRIDE + (uintptr_t)Q1_E5_OBJ0 + (uintptr_t)k * (uintptr_t)Q1_E5_STRIDE;
            if (!q1_rd(obj + 0x240, 0x28)) return Q1W_UNREADABLE;          /* core .bss */
            uintptr_t v = (uintptr_t)q1_u64(obj + 0x260);
            if (!v) continue;                                               /* no table in this slot */
            any = 1;
            uint32_t act = *(volatile const uint32_t *)(obj + 0x258);
            if (act > 1) return Q1W_LINK;
            if (!act) { e->b++; continue; }                                 /* inactive: the pick (0x14038f18a) and the ticker (E5.tick_act) skip it */
            if (!q1_plaus(v) || (uintptr_t)q1_u64(obj + 0x248) != v) return Q1W_LINK;   /* both allocators return >= 8-aligned blocks; the step writes through +0x248 */
            uintptr_t A = (uintptr_t)q1_u64(obj + 0x240);
            /* v0.29.5 Q1R (m5): the game's own rules (design_v0295/q1r/DESIGN.md §2): the loader 0x14038efa0 and the pick 0x14038f180 read the asset
             * header with plain loads, so A needs no alignment (v0.29.4 required 8: q1_plaus(A)); the header is read byte-wise here */
            if (A < 0x10000 || A > 0x7ffffffeffffULL || !q1_rd(A, 0x18)) return Q1W_UNREADABLE;
#ifdef SC6RB_HARNESS
            if ((env->h_old & Q1R_OLD_E5) && !q1_plaus(A)) return Q1W_UNREADABLE;   /* the control: v0.29.4's alignment rule */
#endif
            if (q1_u32u(A) != Q1_E5_MAGIC || q1_u32u(A + 8) != Q1_E5_VER) return Q1W_VTABLE;
            int32_t off = (int32_t)q1_u32u(A + 0x10), groups = (int32_t)q1_u32u(A + 0x14);
            uint32_t n = q1_u32u(A + 0xc);
            /* the loader bounds neither n nor off (size = n * 0x84 + off - 0x18, 0x14038f071..07b): n = 0 is a groups-only table, off = 0x18 a table
             * without groups. off < 0x18 puts the game's event table at table + (u32)(off - 0x18) (0x14038f0cb, zero-extended: ~4 GiB past it):
             * every pick would read outside the table, not a working asset: fail closed. v0.29.4: 0x18 < off <= 0x10000, 1 <= n <= 0x4000 */
            if (off < 0x18) return Q1W_SIZE;
#ifdef SC6RB_HARNESS
            if ((env->h_old & Q1R_OLD_E5) && (off <= 0x18 || off > 0x10000 || n < 1 || n > 0x4000)) return Q1W_SIZE;
#endif
            if ((uintptr_t)q1_u64(obj + 0x250) != v + (uintptr_t)(off - 0x18)) return Q1W_LINK;
            uint64_t size = (uint64_t)n * Q1_E5_EVT + (uint64_t)(off - 0x18);
            if (size > Q1_E5_MAX) return Q1W_SIZE;   /* safety, not a game rule: n is cross-checked by nothing else, and a pin past the block is a wrong pin */
            /* v0.29.5 Q1R (m5): the step's round-robin cursor writes land at table + 8 g + 2..+4 for 0 <= g < [A+0x14] (the pick's bound 0x14038f1a4,
             * the write 0x14038f204): the pin must cover them, or the step writes bytes no snapshot restores */
            int span_ok = groups <= 0 || (uint64_t)groups * 8u - 4u <= size;
#ifdef SC6RB_HARNESS
            if (env->h_old & Q1R_OLD_E5) span_ok = 1;
#endif
            if (!span_ok) return Q1W_SIZE;
            if (s_q1r && s_q1r->e5_n < Q1_E5_TAB) { s_q1r->e5_a[s_q1r->e5_n] = v; s_q1r->e5_len[s_q1r->e5_n] = (uint32_t)size; s_q1r->e5_slot[s_q1r->e5_n] = (uint8_t)(p * 3 + k); s_q1r->e5_n++; }
            e->a++;
            if (q1_arena(env, v)) { (*ask)++; continue; }                   /* a table loaded while armed: the arena snapshot covers it */
            if (!size) continue;                                            /* v0.29.5 Q1R (m5): a zero-byte table holds nothing the step writes (groups <= 0 above) */
            int w = q1_pin(o, v, (uint32_t)size, Q1E_E5, e);
            if (w != Q1W_OK) return w;
        }
    if (!any) e->st = 1;
    return Q1W_OK;
}

/* ------------------------------------------------------------------ v0.29.4 Q1: E7, the container census (gameplay).
 * Every container the scan of this pin scan found (Q1Env.census) is matched by its root against the known rows. whole: the list walked exactly
 * (check_list: every prev link and the count), every non-arena head / node / ref-count block / object pinned whole after its vtable and use
 * count; links: the scan's link pins restore all the step writes (Q1COV: never step-written); E1: E1's own list. An unknown container with
 * heap nodes outside the snapshot fails closed (unknown_class); an empty unknown one keeps the scan's link pins (an empty std container's
 * head links are its whole state). Unknown containers are returned to the caller (Q1 CONT lines). */
enum { Q1C_CORE = 0, Q1C_PLAYER, Q1C_WM };
enum { Q1R_E1 = 0, Q1R_LINKS, Q1R_GAUGE, Q1R_WIND };
typedef struct { uint8_t root, rule; uint64_t off; } Q1CenRow;
static const Q1CenRow k_q1_cen[] = {
    { Q1C_CORE,   Q1R_WIND,  0x14470e9e0ULL },   /* list<shared_ptr<scbattle::StageWindParam>> (E7.q_head_st, E7.q_push): whole */
    { Q1C_PLAYER, Q1R_GAUGE, 0x2b500ULL },       /* list<shared_ptr<PLAYER::HeaderGaugeResource>> (E7.p_push, E7.p_clear0/1): whole */
    { Q1C_CORE,   Q1R_E1,    0x14470f1c0ULL },   /* E1's list (E1 walks it) */
    { Q1C_CORE,   Q1R_LINKS, 0x14470f310ULL },   /* an empty std::map (E7.map_erase); its users are UE calls (IFC +0xb0 +0xc8 +0xd0) */
    { Q1C_PLAYER, Q1R_LINKS, 0x455d0ULL },       /* the 48-byte-node tree: never step-written (Q1COV of L1 and L2) */
    { Q1C_CORE,   Q1R_LINKS, 0x144844160ULL },   /* a tree: never step-written (Q1COV) */
    { Q1C_WM,     Q1R_LINKS, 0x10ULL }, { Q1C_WM, Q1R_LINKS, 0x20ULL }, { Q1C_WM, Q1R_LINKS, 0x30ULL }, { Q1C_WM, Q1R_LINKS, 0x40ULL },   /* WorkMemory: never step-written */
#ifdef SC6RB_HARNESS
    { Q1C_CORE,   Q1R_LINKS, 0x14470e230ULL },   /* harness builds only: the fake battle core's own list (test/harness.c Core.head), not a game root */
    { Q1C_CORE,   Q1R_LINKS, 0x144100e60ULL },   /* harness builds only: the Q1 model's std::map (test/harness_rbo_q1.inc Q1M_EMAP) */
#endif
};
static const Q1CenRow *q1_cen_row(uintptr_t obj) {
    uintptr_t wm = q1_rd(VA(Q1_A_WORKMEM), 8) ? (uintptr_t)q1_u64(VA(Q1_A_WORKMEM)) : 0;
    for (size_t i = 0; i < sizeof k_q1_cen / sizeof k_q1_cen[0]; i++) {
        const Q1CenRow *r = &k_q1_cen[i];
        if (r->root == Q1C_CORE && obj == VA(r->off)) return r;
        if (r->root == Q1C_WM && wm && obj == wm + (uintptr_t)r->off) return r;
        if (r->root == Q1C_PLAYER)
            for (int p = 0; p < 2; p++) if (obj == VA(Q1_A_PLAYER0) + (uintptr_t)p * (uintptr_t)Q1_PLAYER_STRIDE + (uintptr_t)r->off) return r;
    }
    return NULL;
}
static int q1_e7_list(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask, uintptr_t h, uint64_t n, int rule) {
    static uintptr_t nodes[Q1_E7_MAX];
    if (n > Q1_E7_MAX) return Q1W_BOUND;
    if (!q1_plaus(h) || !q1_rd(h, Q1_NODE)) return Q1W_UNREADABLE;
    int nn = 0;
    if (!check_list(h, n, nodes, Q1_E7_MAX, &nn) || (uint64_t)nn != n) return Q1W_LINK;
    uint64_t vtp = 0; uint32_t csz = 0;
    for (int i = 0; i < Q1_NVT; i++) {   /* the ref-count block's row: E7.p_ctrl_size (gauge) or E7.q_ctrl_size (wind) */
        if (!q1_streq(k_q1vt[i].entry, "E7")) continue;
        if (q1_streq(k_q1vt[i].size_sig, rule == Q1R_GAUGE ? "E7.p_ctrl_size" : "E7.q_ctrl_size")) { vtp = k_q1vt[i].vt; csz = k_q1vt[i].size; }
    }
    if (!vtp) return Q1W_VTABLE;
    int w;
    if (q1_arena(env, h)) (*ask)++;
    else { if ((w = q1_pin(o, h, Q1_NODE, Q1E_E7, e)) != Q1W_OK) return w; e->b++; }
    for (int i = 0; i < nn; i++) {
        uintptr_t nd = nodes[i];
        if (q1_arena(env, nd)) { (*ask)++; continue; }                     /* pushed while armed: node, ref count and object are arena blocks */
        if (!q1_rd(nd, Q1_NODE)) return Q1W_UNREADABLE;
        if ((w = q1_pin(o, nd, Q1_NODE, Q1E_E7, e)) != Q1W_OK) return w;
        e->b++;
        uintptr_t ob = (uintptr_t)q1_u64(nd + 0x10), ctrl = (uintptr_t)q1_u64(nd + 0x18);
        if (!ob && !ctrl) continue;                                          /* an empty shared_ptr */
        if (!q1_plaus(ctrl)) return Q1W_LINK;
        if (q1_arena(env, ctrl)) { (*ask)++; continue; }
        if (!q1_rd(ctrl, csz)) return Q1W_UNREADABLE;
        if (q1_u64(ctrl) != VA(vtp)) return Q1W_VTABLE;
        uint32_t uses = *(volatile const uint32_t *)(ctrl + 8);
        if (uses < 1 || uses > 0xffff) return Q1W_COUNT;
        if (rule == Q1R_WIND) { if (ob != ctrl + 0x10) return Q1W_LINK; }    /* make_shared: the object lives in the ref-count block */
        else if (ob != (uintptr_t)q1_u64(ctrl + 0x10)) return Q1W_LINK;       /* _Ref_count<T>::_Ptr (0x14037c0f5) */
        if ((w = q1_pin(o, ctrl, csz, Q1E_E7, e)) != Q1W_OK) return w;
        e->b++;
        if (rule == Q1R_GAUGE) {
            if (!q1_plaus(ob)) return Q1W_LINK;
            if (q1_arena(env, ob)) { (*ask)++; continue; }
            if ((w = q1_pin(o, ob, Q1_E7_GAUGE_OBJ, Q1E_E7, e)) != Q1W_OK) return w;
            e->b++;
        }
    }
    return Q1W_OK;
}
static int q1_e7(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask) {
    if (!env->census) { e->st = 1; return Q1W_OK; }   /* no scan this time (the harness door): absent */
    for (int i = 0; i < env->ncensus; i++) {
        const FoundCont *c = &env->census[i];
        const Q1CenRow *row = q1_cen_row(c->obj);
        if (!row) {
            if (s_q1r) {
                if (s_q1r->unk_n < Q1_UNK_MAX) { s_q1r->unk[s_q1r->unk_n].obj = c->obj; s_q1r->unk[s_q1r->unk_n].size = c->size; s_q1r->unk[s_q1r->unk_n].kind = c->kind; s_q1r->unk[s_q1r->unk_n].nodes_out = c->nodes_out; }
                s_q1r->unk_n++;
                if (c->size > 0 && c->nodes_out > 0) s_q1r->unk_heap++;
            }
            continue;
        }
        if (row->rule == Q1R_E1) continue;
        e->a++;
        if (row->rule == Q1R_LINKS) continue;
        if (!q1_rd(c->obj, 16)) return Q1W_UNREADABLE;
        int w = q1_e7_list(env, o, e, ask, (uintptr_t)q1_u64(c->obj), q1_u64(c->obj + 8), row->rule);
        if (w != Q1W_OK) return w;
    }
    /* v0.29.5 Q1R (m4): fail closed on what the scan could not record (v0.29.4 judged only the recorded containers: fail-open). overflow: more
     * containers than the census array holds, link pins past the scan's pin array, a container with nodes past the scan's node buffer;
     * unchecked: an MSVC-shaped container head whose walk broke or ran past LIST_LIMIT (src/scan.c q1r_note) */
    if (!(env->h_old & Q1R_OLD_E7)) {
        if (env->ncensus_total > env->ncensus || (env->scan && (env->scan->pins_dropped || env->scan->nodes_cut))) return Q1W_OVERFLOW;
        if (env->scan && env->scan->unchecked) return Q1W_UNCHECKED;
    }
    if (s_q1r && s_q1r->unk_heap) return Q1W_UNKNOWN_CLASS;   /* fail closed: a container nobody knows holds heap nodes the step may write */
    return Q1W_OK;
}

/* ------------------------------------------------------------------ v0.29.4 Q1: E8, the controllers' CPU direct-command objects (skipped on a
 * failure, like E4). Controller s (0x144715400 + 0x60 s, E8.ctl_lea) holds at +0x50 the object 0x1402e25a0 allocates (0x68, E8.size) and
 * constructs as HgCpuDirectStand (E8.vt). Another direct-command class (a training dummy's setting) in a slot: that slot is counted (other=)
 * and not pinned; the walk never guesses another class's size. */
static int q1_e8(const Q1Env *env, Q1Out *o, Q1Ent *e, uint32_t *ask) {
    const Q1Vt *row = q1_vt_of("E8", q1_vt_first("E8"));
    if (!row) return Q1W_VTABLE;
    int any = 0;
    for (int s = 0; s < 2; s++) {
        uintptr_t slot = VA(Q1_A_CTL0) + (uintptr_t)s * (uintptr_t)Q1_CTL_STRIDE + 0x50;
        if (!q1_rd(slot, 8)) return Q1W_UNREADABLE;
        uintptr_t c = (uintptr_t)q1_u64(slot);
        if (!c) continue;
        any = 1;
        if (q1_arena(env, c)) { (*ask)++; continue; }
        if (!q1_plaus(c) || !q1_rd(c, 8)) return Q1W_UNREADABLE;
        if (q1_u64(c) != VA(row->vt)) { e->b++; continue; }
        int w = q1_pin(o, c, row->size, Q1E_E8, e);
        if (w != Q1W_OK) return w;
        e->a++;
    }
    if (!any) e->st = 1;
    return Q1W_OK;
}

/* ------------------------------------------------------------------ the whole table. Returns r->ok. Pins of a failed (E4: skipped) entry are
 * taken back out of the out array; a failed gameplay entry makes the whole result unusable (the caller never applies a partial set). */
static int q1_walk_run(const Q1Env *env, Q1Res *r, Q1Out *o) {
    static int (*const fn[Q1E_N])(const Q1Env *, Q1Out *, Q1Ent *, uint32_t *) = { q1_e1, q1_e2, q1_e3, q1_e4, q1_e6, q1_e5, q1_e7, q1_e8 };   /* v0.29.4 Q1: + E5 E7 E8 */
    for (int i = 0; i < Q1E_N; i++) { r->e[i].st = 0; r->e[i].why = 0; r->e[i].a = r->e[i].b = r->e[i].grown = 0; r->e[i].bytes = 0; r->e[i].harness_skip = 0; }
    r->ok = 1; r->fail_ent = -1; r->fail_why = 0; r->arena_skipped = 0; r->pins = 0; r->bytes = 0;
    r->e5_n = 0; r->unk_n = 0; r->unk_heap = 0; s_q1r = r;   /* v0.29.4 Q1 */
    o->n = 0; o->overflow = 0;
    for (int i = 0; i < Q1E_N; i++) {
        Q1Ent *e = &r->e[i];
        if (env->skip_mask & (1u << i)) { e->st = 2; e->harness_skip = 1; continue; }
        int n0 = o->n; uint32_t ask = 0;
        int w = fn[i](env, o, e, &ask);
        if (w != Q1W_OK) {
            o->n = n0; e->bytes = 0;                      /* nothing of a failed / skipped entry stays in the out array */
            e->why = w;
            if (q1_skippable(i)) { e->st = 2; continue; } /* presentation (E4) / v0.29.4 Q1 E8: skip:<why>, the rest of the table continues */
            e->st = 3;
            if (r->ok) { r->ok = 0; r->fail_ent = i; r->fail_why = w; }
            continue;                                      /* the other entries are still walked (reads only), for a complete log line */
        }
        r->arena_skipped += ask;
    }
    for (int i = 0; i < o->n; i++) r->bytes += o->pins[i].len;
    r->pins = (uint32_t)o->n;
    s_q1r = NULL;   /* v0.29.4 Q1 */
    return r->ok;
}

/* ------------------------------------------------------------------ the signature gate (process start; §16.6). Every Q1SIG row's bytes at
 * its VA, every Q1PTR row's relocated pointer (v0.29.3), and every Q1VT size against the immediate of its size_sig row (`mov r32, imm32`:
 * bytes 1..4; E3 rows are u16 item counts, x2). bad: "name@0xva,..." (truncated to cap) or "-". Returns 1 = every row matches. */
static int q1_hexnib(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void q1_app(char *out, size_t cap, size_t *o, const char *s) { while (*s && *o + 1 < cap) out[(*o)++] = *s++; if (cap) out[*o < cap ? *o : cap - 1] = 0; }
static void q1_apphex(char *out, size_t cap, size_t *o, uint64_t v) {
    char b[19]; int n = 0; b[n++] = '0'; b[n++] = 'x';
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) { int d = (int)((v >> s) & 15); if (d || started || s == 0) { b[n++] = "0123456789abcdef"[d]; started = 1; } }
    b[n] = 0; q1_app(out, cap, o, b);
}
static int q1_sig_row_ok(const Q1Sig *s, uint8_t *imm, int *nbytes) {
    int n = 0;
    while (s->hex[2 * n] && s->hex[2 * n + 1]) n++;
    *nbytes = n;
    uintptr_t a = VA(s->va);
    if (!n || !q1_rd(a, (size_t)n)) return 0;
    for (int i = 0; i < n; i++) {
        int hi = q1_hexnib(s->hex[2 * i]), lo = q1_hexnib(s->hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        uint8_t want = (uint8_t)(hi << 4 | lo);
        if (*(volatile const uint8_t *)(a + (uintptr_t)i) != want) return 0;
        if (imm && i < 5) imm[i] = want;
    }
    return 1;
}
/* v0.29.3 (review B1): a Q1PTR row holds the loader-relocated target: [VA(va)] == VA(target) (VA = this process's base) */
static int q1_ptr_row_ok(const Q1Ptr *p) {
    uintptr_t a = VA(p->va);
    return q1_rd(a, 8) && q1_u64(a) == (uint64_t)VA(p->target);
}
static int q1_sig_check(char *bad, size_t cap, int *rows) {
    size_t o = 0; int ok = 1;
    if (cap) bad[0] = 0;
    *rows = Q1_NSIG + Q1_NPTR;
    for (int i = 0; i < Q1_NSIG; i++) {
        int nb = 0;
        if (q1_sig_row_ok(&k_q1sig[i], NULL, &nb)) continue;
        if (o) q1_app(bad, cap, &o, ",");
        q1_app(bad, cap, &o, k_q1sig[i].name); q1_app(bad, cap, &o, "@"); q1_apphex(bad, cap, &o, k_q1sig[i].va);
        ok = 0;
    }
    for (int i = 0; i < Q1_NPTR; i++) {
        if (q1_ptr_row_ok(&k_q1ptr[i])) continue;
        if (o) q1_app(bad, cap, &o, ",");
        q1_app(bad, cap, &o, k_q1ptr[i].name); q1_app(bad, cap, &o, "@"); q1_apphex(bad, cap, &o, k_q1ptr[i].va);
        ok = 0;
    }
    for (int i = 0; i < Q1_NVT; i++) {   /* the table's own consistency: each size equals the immediate of its size_sig row */
        int found = 0;
        for (int j = 0; j < Q1_NSIG; j++) {
            if (!q1_streq(k_q1sig[j].name, k_q1vt[i].size_sig)) continue;
            found = 1;
            int nb = 0; uint8_t imm[5] = { 0 };
            for (int q = 0; q < 5 && k_q1sig[j].hex[2 * q] && k_q1sig[j].hex[2 * q + 1]; q++)
                imm[q] = (uint8_t)(q1_hexnib(k_q1sig[j].hex[2 * q]) << 4 | q1_hexnib(k_q1sig[j].hex[2 * q + 1]));
            while (k_q1sig[j].hex[2 * nb]) nb++;
            uint32_t v = (uint32_t)imm[1] | (uint32_t)imm[2] << 8 | (uint32_t)imm[3] << 16 | (uint32_t)imm[4] << 24;
            if (q1_streq(k_q1vt[i].entry, "E3")) v *= 2;
            if (nb != 5 || (imm[0] & 0xf8) != 0xb8 || v != k_q1vt[i].size) found = 2;
        }
        if (found != 1) {
            if (o) q1_app(bad, cap, &o, ",");
            q1_app(bad, cap, &o, k_q1vt[i].size_sig); q1_app(bad, cap, &o, found ? "(size)" : "(missing)");
            ok = 0;
        }
    }
    if (!o) q1_app(bad, cap, &o, "-");
    return ok;
}
