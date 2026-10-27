/* UE allocator hooks (GMalloc vtable), arena routing, deferred frees and the
 * allocation census.
 *
 * We patch the GMalloc object's vtable slots (Malloc +0x10, Realloc +0x18,
 * Free +0x20, GetAllocationSize +0x30) instead of FMemory::Malloc/Free code:
 * ~300 call sites in the exe inline FMemory::Free as GMalloc->Free, and
 * 0x1404a61c0 (named FMemory::Malloc in GATE-A) is only an operator-new thunk
 * (xor edx,edx; jmp 0x140d4f760). The vtable catches every path. */
#include "mem.h"
#include "rb.h"

typedef void *(*MallocFn)(void *self, size_t n, uint32_t al);
typedef void *(*ReallocFn)(void *self, void *p, size_t n, uint32_t al);
typedef void  (*FreeFn)(void *self, void *p);
typedef uint8_t (*GetSizeFn)(void *self, void *p, size_t *out);

static int patch_ptr(void **slot, void *val);
static void itrack_remove(uintptr_t p);
static uint32_t core_caller(void);
static MallocFn o_malloc;
static ReallocFn o_realloc;
static FreeFn o_free;
static GetSizeFn o_getsize;
void *g_gm_obj;
int g_mem_hooked;

Arena g_arena;
Spin g_arena_lock;
int g_arena_ok;
volatile int g_route_broken, g_defer_broken;

static DeferList g_def, g_def_saved;
static Spin g_def_lock;
/* v0.23.1 (review F3 ii): outside frees that took an entry out of the deferred list while a def_save copy is held (an RPB pass, the
 * roll7 non-commit resims). def_restore would put those entries back and the block would be freed twice (now, and at its confirmation):
 * def_restore removes them again. More than DEF_HITS_CAP in one window: g_defer_broken (rb aborts, the other modes fall back). */
#define DEF_HITS_CAP 256
static int g_def_saved_on, g_def_nhits, g_def_hits_over; static uintptr_t g_def_hits[DEF_HITS_CAP];
static void def_hit_note(uintptr_t key) {   /* g_def_lock held */
    if (!g_def_saved_on) return;
    if (g_def_nhits < DEF_HITS_CAP) g_def_hits[g_def_nhits++] = key; else g_def_hits_over = 1;
}

static volatile LONG64 g_arena_out_frees, g_arena_in_frees, g_arena_allocs, g_arena_bad_frees, g_def_outside_hits;
static volatile LONG64 g_init_allocs, g_init_bytes;
static volatile LONG64 g_crt_in_allocs, g_crt_in_frees, g_init_route_fail;

/* ------------------------------------------------------------------ census tables */
StepCensus g_sc;
typedef struct { uintptr_t p; uint32_t size; uint32_t caller; uint64_t seq; } TrackEnt;
#define TRACK_BITS 17
#define TRACK_CAP (1u << TRACK_BITS)
static TrackEnt *g_track;
static volatile uint32_t g_track_n;
static volatile uintptr_t g_track_lo = ~(uintptr_t)0, g_track_hi;
static Spin g_track_lock;
static volatile int g_track_full;
static uint64_t g_live_bytes, g_peak_live_bytes;
static uint64_t g_life_same, g_life_le8, g_life_le60, g_life_gt60, g_life_out;

typedef struct { uint32_t key; uint32_t n; uint64_t bytes; uint32_t mn, mx; } CallerEnt;
#define CALLERS 512
static CallerEnt g_c_alloc[CALLERS], g_c_prefree[CALLERS], g_c_outfree[CALLERS], g_c_crt[CALLERS];
static uint64_t g_cen_steps, g_cen_allocs, g_cen_bytes, g_cen_max_allocs, g_cen_max_bytes, g_cen_prefrees, g_cen_reallocs;

static inline uint32_t hash_ptr(uintptr_t p) { return (uint32_t)(((p >> 4) * 0x9E3779B97F4A7C15ULL) >> (64 - TRACK_BITS)); }

static void caller_add(CallerEnt *t, uint32_t key, uint64_t bytes) {
    uint32_t h = (key * 2654435761u) & (CALLERS - 1);
    for (int i = 0; i < CALLERS; i++) {
        CallerEnt *e = &t[(h + i) & (CALLERS - 1)];
        if (e->n && e->key != key) continue;
        if (!e->n) { e->key = key; e->mn = 0xffffffffu; }
        e->n++; e->bytes += bytes;
        if (bytes < e->mn) e->mn = (uint32_t)bytes;
        if (bytes > e->mx) e->mx = (uint32_t)(bytes > 0xffffffffu ? 0xffffffffu : bytes);
        return;
    }
}

static void track_put(uintptr_t p, uint32_t size, uint32_t caller, uint64_t seq) {
    if (!g_track || g_track_n > TRACK_CAP * 3 / 4) { g_track_full = 1; return; }
    uint32_t i = hash_ptr(p);
    while (g_track[i].p && g_track[i].p != p) i = (i + 1) & (TRACK_CAP - 1);
    if (!g_track[i].p) g_track_n++;
    if (p < g_track_lo) g_track_lo = p;
    if (p > g_track_hi) g_track_hi = p;
    g_track[i].p = p; g_track[i].size = size; g_track[i].caller = caller; g_track[i].seq = seq;
    g_live_bytes += size;
    if (g_live_bytes > g_peak_live_bytes) g_peak_live_bytes = g_live_bytes;
}

static int track_take(uintptr_t p, TrackEnt *out) {
    uint32_t i = hash_ptr(p);
    while (g_track[i].p) {
        if (g_track[i].p == p) {
            *out = g_track[i];
            /* backward-shift delete */
            uint32_t j = i;
            for (;;) {
                j = (j + 1) & (TRACK_CAP - 1);
                if (!g_track[j].p) break;
                uint32_t k = hash_ptr(g_track[j].p);
                if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) { g_track[i] = g_track[j]; i = j; }
            }
            g_track[i].p = 0;
            g_track_n--;
            g_live_bytes -= out->size;
            return 1;
        }
        i = (i + 1) & (TRACK_CAP - 1);
    }
    return 0;
}

static uint32_t caller_rva(void *ret) {
    uintptr_t r = (uintptr_t)ret;
    return IN_IMAGE(r) ? (uint32_t)RVA(r) : 0xffffffffu;
}

/* first return address inside the battle core code range (0x1402c0000-0x1403a0000) */
__attribute__((noinline)) static uint32_t core_caller(void) {
    void *fr[10];
    USHORT n = RtlCaptureStackBackTrace(2, 10, fr, NULL);
    uintptr_t lo = VA(0x1402c0000ULL), hi = VA(0x1403a0000ULL);
    for (USHORT i = 0; i < n; i++)
        if ((uintptr_t)fr[i] >= lo && (uintptr_t)fr[i] < hi) return (uint32_t)RVA(fr[i]);
    return n ? caller_rva(fr[0]) : 0xffffffffu;
}

void census_step_begin(void) { memset(&g_sc, 0, sizeof g_sc); }

void census_step_end(uint64_t seq) {
    (void)seq;
    g_cen_steps++;
    g_cen_allocs += g_sc.n_alloc;
    g_cen_bytes += g_sc.bytes_alloc;
    g_cen_prefrees += g_sc.n_free_pre;
    g_cen_reallocs += g_sc.n_realloc;
    if (g_sc.n_alloc > g_cen_max_allocs) g_cen_max_allocs = g_sc.n_alloc;
    if (g_sc.bytes_alloc > g_cen_max_bytes) g_cen_max_bytes = g_sc.bytes_alloc;
}

void census_battle_reset(void) {
    spin_lock(&g_track_lock);
    if (g_track) memset(g_track, 0, sizeof(TrackEnt) * TRACK_CAP);
    g_track_n = 0; g_track_full = 0; g_live_bytes = 0; g_peak_live_bytes = 0;
    g_track_lo = ~(uintptr_t)0; g_track_hi = 0;
    g_life_same = g_life_le8 = g_life_le60 = g_life_gt60 = g_life_out = 0;
    memset(g_c_alloc, 0, sizeof g_c_alloc); memset(g_c_prefree, 0, sizeof g_c_prefree); memset(g_c_outfree, 0, sizeof g_c_outfree); memset(g_c_crt, 0, sizeof g_c_crt);
    g_cen_steps = g_cen_allocs = g_cen_bytes = g_cen_max_allocs = g_cen_max_bytes = g_cen_prefrees = g_cen_reallocs = 0;
    spin_unlock(&g_track_lock);
}

static void dump_callers(const char *kind, CallerEnt *t) {
    for (int i = 0; i < CALLERS; i++)
        if (t[i].n) LOG("CEN %s caller=0x%x n=%u bytes=%llu min=%u max=%u\n", kind, t[i].key, t[i].n,
                        (unsigned long long)t[i].bytes, t[i].mn, t[i].mx);
}

void census_report(const char *why) {
    spin_lock(&g_track_lock);
    uint64_t live_blocks = g_track_n;
    LOG("CEN summary why=%s steps=%llu allocs=%llu bytes=%llu max_allocs_step=%llu max_bytes_step=%llu prefrees=%llu reallocs=%llu "
        "peak_live=%llu live_now=%llu live_blocks=%llu life_same=%llu life_le8=%llu life_le60=%llu life_gt60=%llu freed_outside=%llu track_full=%d\n",
        why, (unsigned long long)g_cen_steps, (unsigned long long)g_cen_allocs, (unsigned long long)g_cen_bytes,
        (unsigned long long)g_cen_max_allocs, (unsigned long long)g_cen_max_bytes, (unsigned long long)g_cen_prefrees,
        (unsigned long long)g_cen_reallocs, (unsigned long long)g_peak_live_bytes, (unsigned long long)g_live_bytes,
        (unsigned long long)live_blocks, (unsigned long long)g_life_same, (unsigned long long)g_life_le8,
        (unsigned long long)g_life_le60, (unsigned long long)g_life_gt60, (unsigned long long)g_life_out, g_track_full);
    dump_callers("alloc", g_c_alloc);
    dump_callers("prefree", g_c_prefree);
    dump_callers("outfree", g_c_outfree);
    dump_callers("crtalloc", g_c_crt);
    spin_unlock(&g_track_lock);
}

void mem_stats_log(const char *why) {
    char err[128] = "ok";
    int ok = 1;
    if (g_arena_ok) { spin_lock(&g_arena_lock); ok = arena_check(&g_arena, err, sizeof err); spin_unlock(&g_arena_lock); }
    LOG("MEM why=%s arena_ok=%d used=%llu peak=%llu live_blocks=%llu live_bytes=%llu allocs=%lld in_frees=%lld out_frees=%lld bad_frees=%lld fails=%llu "
        "init_allocs=%lld init_bytes=%lld init_route_fail=%lld crt_in_allocs=%lld crt_in_frees=%lld deferred=%d def_outside_hits=%lld route_broken=%d defer_broken=%d check=%s\n",
        why, g_arena_ok, (unsigned long long)arena_used(&g_arena), (unsigned long long)g_arena.peak_bump,
        g_arena_ok ? (unsigned long long)g_arena.h->live_blocks : 0ULL, g_arena_ok ? (unsigned long long)g_arena.h->live_bytes : 0ULL,
        (long long)g_arena_allocs, (long long)g_arena_in_frees, (long long)g_arena_out_frees, (long long)g_arena_bad_frees,
        (unsigned long long)g_arena.fail_count, (long long)g_init_allocs, (long long)g_init_bytes, (long long)g_init_route_fail,
        (long long)g_crt_in_allocs, (long long)g_crt_in_frees, def_count(),
        (long long)g_def_outside_hits, g_route_broken, g_defer_broken, ok ? "ok" : err);
}

/* ------------------------------------------------------------------ arena */
/* v0.22.0-C1a (GATE-C-DESIGN §8 C1a): with fixed=1 the arena is reserved at ARENA_FIXED_BASE first, so arena pointers have the same
 * raw value in every launch; any address otherwise (the C1a hashes normalize arena pointers to offsets either way). The address is
 * 2 TB below the top of the 128 TB user space: far from the exe and DLL images (0x7ff0'00000000 and up) and from the heaps
 * (typically 0x1'00000000..0x3ff'00000000). Logged as ARENA base=... fixed=1|0. */
#define ARENA_FIXED_BASE 0x00007e0000000000ULL
int g_arena_fixed;
int arena_setup(int mb, int fixed) {
    size_t cap = (size_t)mb << 20;
    void *mem = NULL; DWORD ferr = 0;
    if (fixed) { mem = VirtualAlloc((void *)(uintptr_t)ARENA_FIXED_BASE, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); if (!mem) ferr = GetLastError(); }
    g_arena_fixed = mem != NULL;
    if (!mem) mem = VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) { LOG("ARENA alloc_failed mb=%d err=%lu\n", mb, GetLastError()); return 0; }
    g_arena_ok = arena_init(&g_arena, mem, cap);
    defer_init(&g_def, 65536);
    defer_init(&g_def_saved, 65536);
    g_track = (TrackEnt *)VirtualAlloc(NULL, sizeof(TrackEnt) * TRACK_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    LOG("ARENA base=%p cap=%zu ok=%d fixed=%d requested_fixed=%d preferred=0x%llx fixed_err=%lu\n", mem, cap, g_arena_ok, g_arena_fixed, fixed,
        (unsigned long long)ARENA_FIXED_BASE, (unsigned long)ferr);
    return g_arena_ok;
}

static void *arena_route(size_t n, uint32_t al) {
    spin_lock(&g_arena_lock);
    void *p = arena_alloc(&g_arena, n, al);
    spin_unlock(&g_arena_lock);
    if (p) InterlockedIncrement64(&g_arena_allocs);
    return p;
}

static void arena_release(void *p, int inside) {
    spin_lock(&g_arena_lock);
    int ok = arena_free(&g_arena, p);
    spin_unlock(&g_arena_lock);
    if (!ok) InterlockedIncrement64(&g_arena_bad_frees);
    else if (inside) InterlockedIncrement64(&g_arena_in_frees);
    else InterlockedIncrement64(&g_arena_out_frees);
}

/* CRT heap originals (exe IAT: api-ms-win-crt-heap-l1-1-0.dll) */
typedef void *(*CMallocFn)(size_t);
typedef void  (*CFreeFn)(void *);
typedef void *(*CReallocFn)(void *, size_t);
typedef void *(*CCallocFn)(size_t, size_t);
static CMallocFn o_cmalloc;
static CFreeFn o_cfree;
static CReallocFn o_crealloc;
static CCallocFn o_ccalloc;


/* ------------------------------------------------------------------ deferral */
int def_count(void) { return g_def.n; }

void def_drop_from(uint64_t seq) {
    spin_lock(&g_def_lock);
    defer_drop_from(&g_def, seq);
    spin_unlock(&g_def_lock);
}

static int confirm(uint64_t before, int all) {
    static uintptr_t out[4096];
    int total = 0;
    for (;;) {
        spin_lock(&g_def_lock);
        int k = defer_take_before(&g_def, all ? ~0ULL : before, out, 4096);
        spin_unlock(&g_def_lock);
        for (int i = 0; i < k; i++) {
            uintptr_t p = out[i] & ~(uintptr_t)1;
            pins_note_free(p);
            itrack_remove(p);
            if (out[i] & 1) o_cfree((void *)p);   /* CRT heap block */
            else o_free(g_gm_obj, (void *)p);
        }
        total += k;
        if (k < 4096) break;
    }
    return total;
}
int def_confirm_before(uint64_t seq) { return g_def.n ? confirm(seq, 0) : 0; }
int def_confirm_all(void) { return g_def.n ? confirm(0, 1) : 0; }
void def_save(void) { spin_lock(&g_def_lock); defer_copy(&g_def_saved, &g_def); g_def_saved_on = 1; g_def_nhits = 0; g_def_hits_over = 0; spin_unlock(&g_def_lock); }
void def_restore(void) {
    spin_lock(&g_def_lock);
    defer_copy(&g_def, &g_def_saved);
    for (int i = 0; i < g_def_nhits; i++) defer_remove(&g_def, g_def_hits[i]);   /* v0.23.1: freed by their owner meanwhile */
    if (g_def_hits_over) g_defer_broken = 1;
    g_def_saved_on = 0; g_def_nhits = 0; g_def_hits_over = 0;
    spin_unlock(&g_def_lock);
}

/* ------------------------------------------------------------------ Init/Setup block tracking
 * Init runs before the battle manager's first Tick, so the online/offline class is unknown
 * there (round 1: routed=0 for every Init). Instead of routing, we only remember every block
 * allocated inside BattleCore::Init/Setup (pure bookkeeping, also safe online). Rollback modes
 * add the live tracked blocks to the snapshot as pins, so battle-lifetime heap objects the
 * step writes are restored. */
typedef struct { uintptr_t p; uint32_t size; uint32_t caller; uint8_t kind; uint32_t gen; } ITrack;   /* v0.29.2: + gen (the itrack generation stamp) */
#define ITRACK_BITS 15
#define ITRACK_CAP (1u << ITRACK_BITS)
static ITrack *g_it;
static volatile uint32_t g_it_n;
static uint64_t g_it_bytes;
static volatile uintptr_t g_it_lo = ~(uintptr_t)0, g_it_hi;
static Spin g_it_lock;
static volatile LONG64 g_it_dropped;
volatile uint64_t g_it_adds;
static volatile uint32_t g_it_gen;   /* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.4): the generation an insertion carries (itrack_gen_bump at ARM) */
static inline uint32_t it_hash(uintptr_t p) { return (uint32_t)(((p >> 4) * 0x9E3779B97F4A7C15ULL) >> (64 - ITRACK_BITS)); }

/* v0.31.0 (OPTA-M2-SPEC.md §20.3): the itrack filter. One bit per hash of an inserted block start, set under g_it_lock before the entry is
 * visible and cleared only with the table (itrack_clear): a superset of the table's keys at every moment a free can ask. A block cannot be
 * inserted (allocated) while it is being freed, so a free whose bit is clear has no entry: itrack_remove returns without the lock. rbonline only
 * (every other mode keeps v0.30.0's path); the harness control h_rbo_rs_old bit 32 turns it off. */
#define ITF_BITS 18
static uint64_t g_itf[(1u << ITF_BITS) / 64];
static uint64_t g_itf_skips;
#ifdef SC6RB_HARNESS
int g_h_it_wide;   /* v0.31.0: h_rbo_it_wide=1: the range check never rejects (in the game the few late blocks come from the same FMalloc as UE's
                    * blocks, so [lo, hi] spans them: H; the harness's heap keeps them apart and the range check alone answered every free) */
#endif
static inline uint32_t itf_h(uintptr_t p) { return (uint32_t)(((uint64_t)(p >> 4) * 0xC2B2AE3D27D4EB4FULL) >> (64 - ITF_BITS)); }
uint64_t itrack_filter_skips(void) { return g_itf_skips; }
static void itrack_put2(void *ptr, size_t n, int kind, uint32_t caller) {
    uintptr_t p = (uintptr_t)ptr;
    if (!g_it) return;
    uint32_t c = caller ? caller : core_caller();
    spin_lock(&g_it_lock);
    if (g_it_n < ITRACK_CAP * 3 / 4 && n < 0x10000000) {
        { uint32_t h = itf_h(p); g_itf[h >> 6] |= 1ULL << (h & 63); }   /* v0.31.0: before the entry exists */
        uint32_t i = it_hash(p);
        while (g_it[i].p && g_it[i].p != p) i = (i + 1) & (ITRACK_CAP - 1);
        if (!g_it[i].p) { g_it_n++; g_it_bytes += n; g_it_adds++; }
        g_it[i].p = p; g_it[i].size = (uint32_t)n; g_it[i].caller = c; g_it[i].kind = (uint8_t)kind; g_it[i].gen = g_it_gen;
        if (p < g_it_lo) g_it_lo = p;
        if (p + n > g_it_hi) g_it_hi = p + n;
#ifdef SC6RB_HARNESS
        if (g_h_it_wide) { g_it_lo = 0x10000; g_it_hi = 0x7ffffffeffffULL; }   /* v0.31.0 harness model (h_rbo_it_wide): the late blocks span UE's heap */
#endif
    } else InterlockedIncrement64(&g_it_dropped);
    spin_unlock(&g_it_lock);
}



/* round 4: every block whose allocation call site is in the battle core's code range is tracked,
 * whenever it is allocated (observe-time step allocations, HgCpuDirect* dummy controllers created by
 * 0x1402e26a0, emitters 0x140334960, camera layer buffers...). The return address of the hook is
 * the core call site because FMemory::Malloc and 0x1404a61c0 reach the vtable slot by tail jumps. */
volatile int g_track_core = 1;
/* S1BFIX: G1 (C1B-R2-SPEC.md §2.4, Y4/Y5): UE's TaskGraph small-task allocator is linked inside the core's code range. Its 8 KiB bundles
 * (32 x 0x100 slots, a free list through each slot's first qword) were tracked as core blocks, pinned whole and restored by rollbacks: the
 * restore wrote old vtables over UE's free-list links and the next pop returned a vtable address (the S1-B crashes, VERIFIED for crash 2).
 * The two function bounds (lip/fnbounds.out) hold the 5 inlined FMemory::Malloc bundle sites (RVAs 0x39a215 0x39a3a5 0x39a535 0x39a6c5
 * 0x39a865) and the per-thread cache operator new sites: an allocation returning there is NEVER tracked, in every mode. */
static const uint64_t k_s1b_deny[2][2] = { { 0x14039a160ULL, 0x14039a948ULL }, { 0x14039af40ULL, 0x14039b412ULL } };   /* S1BFIX: */
int mem_deny_rva(uint32_t rva, int gate) {   /* S1BFIX: gate 1 = G1 (tracking), 2 = G2 (pin gate); the harness controls h_deny_off 1 / 2 */
    if (g_cfg.h_deny_off == 1 || (gate == 1 && g_cfg.h_deny_off == 2)) return 0;
    uint64_t va = IMG_PREF + rva;
    for (int i = 0; i < 2; i++) if (va >= k_s1b_deny[i][0] && va < k_s1b_deny[i][1]) return 1;
    return 0;
}
static volatile LONG64 g_it_denied;   /* S1BFIX: allocations G1 kept out of the tracking (ITRACK denied=) */
static uint32_t g_itx_seen[16]; static int g_itx_n; static Spin g_itx_lock;
static __attribute__((noinline)) int deny_site(uintptr_t ra, size_t n) {   /* S1BFIX: 1 = never track (ITRACKX once per caller) */
    if (ra < g_base || ra >= g_base + g_image_size) return 0;
    uint32_t rva = (uint32_t)RVA(ra);
    if (!mem_deny_rva(rva, 1)) return 0;
    InterlockedIncrement64(&g_it_denied);
    int log_it = 0;
    spin_lock(&g_itx_lock);
    int seen = 0; for (int i = 0; i < g_itx_n; i++) if (g_itx_seen[i] == rva) seen = 1;
    if (!seen && g_itx_n < 16) { g_itx_seen[g_itx_n++] = rva; log_it = 1; }
    spin_unlock(&g_itx_lock);
    if (log_it) LOG("ITRACKX caller=0x%x size=%zu note=UE_TaskGraph_small_task_allocator_never_tracked\n", rva, n);
    return 1;
}
static inline int core_site(uintptr_t ra) { return ra >= VA(0x1402c0000ULL) && ra < VA(0x1403a0000ULL); }

static void itrack_remove(uintptr_t p) {
    if (!g_it_n || p < g_it_lo || p > g_it_hi) return;
    if (g_mode == MODE_RBONLINE && !RS_OLD(RS_OLD_FILTERS)) {   /* v0.31.0: the filter's negative answer (above), without the lock */
        uint32_t h = itf_h(p);
        if (!(g_itf[h >> 6] & (1ULL << (h & 63)))) { g_itf_skips++; return; }
    }
    spin_lock(&g_it_lock);
    uint32_t i = it_hash(p);
    while (g_it[i].p) {
        if (g_it[i].p == p) {
            g_it_n--; g_it_bytes -= g_it[i].size;
            uint32_t j = i;
            for (;;) {
                j = (j + 1) & (ITRACK_CAP - 1);
                if (!g_it[j].p) break;
                uint32_t k = it_hash(g_it[j].p);
                if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) { g_it[i] = g_it[j]; i = j; }
            }
            g_it[i].p = 0;
            break;
        }
        i = (i + 1) & (ITRACK_CAP - 1);
    }
    spin_unlock(&g_it_lock);
}

int itrack_contains(uintptr_t p, uint32_t *caller, int *kind) {
    if (!g_it_n || p < g_it_lo || p > g_it_hi) return 0;
    int found = 0;
    spin_lock(&g_it_lock);
    uint32_t i = it_hash(p);
    while (g_it[i].p) {
        if (g_it[i].p == p) { found = 1; if (caller) *caller = g_it[i].caller; if (kind) *kind = g_it[i].kind; break; }
        i = (i + 1) & (ITRACK_CAP - 1);
    }
    spin_unlock(&g_it_lock);
    return found;
}

int itrack_init(void) {
    if (!g_it) g_it = (ITrack *)VirtualAlloc(NULL, sizeof(ITrack) * ITRACK_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return g_it != NULL;
}

int itrack_list(SPin *out, int max, uint64_t *bytes_out) {
    int n = 0;
    uint64_t b = 0;
    if (!g_it) return 0;
    spin_lock(&g_it_lock);
    for (uint32_t i = 0; i < ITRACK_CAP && n < max; i++)
        if (g_it[i].p) { out[n].addr = g_it[i].p; out[n].len = g_it[i].size; b += g_it[i].size; n++; }
    spin_unlock(&g_it_lock);
    if (bytes_out) *bytes_out = b;
    return n;
}

/* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.4, §16.9): the generation stamp. rbreplay with rb_init_pins=0 pins only the entries tracked at or after
 * its ARM frame (gen >= the value itrack_gen_bump returned there); Q1COV reads caller / kind / gen. Existing modes never call these. */
uint32_t itrack_gen_bump(void) { spin_lock(&g_it_lock); uint32_t g = ++g_it_gen; spin_unlock(&g_it_lock); return g; }
uint32_t itrack_gen(void) { return g_it_gen; }
int itrack_list_ex(ItInfo *out, int max, uint32_t gen_min, uint64_t *bytes_out) {
    int n = 0;
    uint64_t b = 0;
    if (!g_it) { if (bytes_out) *bytes_out = 0; return 0; }
    spin_lock(&g_it_lock);
    for (uint32_t i = 0; i < ITRACK_CAP && n < max; i++)
        if (g_it[i].p && g_it[i].gen >= gen_min) {
            out[n].p = g_it[i].p; out[n].size = g_it[i].size; out[n].caller = g_it[i].caller; out[n].kind = g_it[i].kind; out[n].gen = g_it[i].gen;
            b += g_it[i].size; n++;
        }
    spin_unlock(&g_it_lock);
    if (bytes_out) *bytes_out = b;
    return n;
}
void itrack_clear(void) {
    if (!g_it) return;
    spin_lock(&g_it_lock);
    memset(g_it, 0, sizeof(ITrack) * ITRACK_CAP);
    g_it_n = 0; g_it_bytes = 0; g_it_lo = ~(uintptr_t)0; g_it_hi = 0;
    memset(g_itf, 0, sizeof g_itf); g_itf_skips = 0;   /* v0.31.0: with the table, never alone */
    spin_unlock(&g_it_lock);
}

void itrack_report(const char *why) {
    if (!g_it) return;
    /* aggregate by allocation call site */
    typedef struct { uint32_t caller; uint32_t n; uint64_t bytes; uint8_t kind; } Agg;
    static Agg agg[256];
    int na = 0;
    spin_lock(&g_it_lock);
    for (uint32_t i = 0; i < ITRACK_CAP; i++) {
        if (!g_it[i].p) continue;
        int k = 0;
        for (; k < na; k++) if (agg[k].caller == g_it[i].caller && agg[k].kind == g_it[i].kind) break;
        if (k == na) { if (na >= 256) continue; agg[na].caller = g_it[i].caller; agg[na].n = 0; agg[na].bytes = 0; agg[na].kind = g_it[i].kind; na++; }
        agg[k].n++; agg[k].bytes += g_it[i].size;
    }
    uint32_t n = g_it_n; uint64_t b = g_it_bytes;
    spin_unlock(&g_it_lock);
    LOG("ITRACK why=%s blocks=%u bytes=%llu dropped=%lld callsites=%d denied=%lld\n", why, n, (unsigned long long)b, (long long)g_it_dropped, na, (long long)g_it_denied);   /* S1BFIX: + denied */
    for (int k = 0; k < na; k++)
        LOG("ITRACKC caller=0x%x kind=%s n=%u bytes=%llu\n", agg[k].caller, (agg[k].kind & 1) ? "crt" : "fmalloc", agg[k].n, (unsigned long long)agg[k].bytes);
}

/* ------------------------------------------------------------------ hooks */
static void *hk_malloc(void *self, size_t n, uint32_t al) {
    if (in_ctx()) {
        uint32_t f = g_ctx_flags;
        if ((f & CTX_ROUTE) && g_arena_ok && !g_route_broken) {
            void *p = arena_route(n, al);
            if (p) {
                if (f & CTX_INIT) { InterlockedIncrement64(&g_init_allocs); InterlockedAdd64(&g_init_bytes, (LONG64)n); }
                g_sc.n_arena_alloc++;
                if (f & CTX_CENSUS) { g_sc.n_alloc++; g_sc.bytes_alloc += n; caller_add(g_c_alloc, core_caller(), n); }
                return p;
            }
            if (f & CTX_INIT) InterlockedIncrement64(&g_init_route_fail);
            else g_route_broken = 1;
        }
        void *p = o_malloc(self, n, al);
        if (p && ((f & CTX_TRACK) || (g_track_core && core_site((uintptr_t)__builtin_return_address(0)))) && !deny_site((uintptr_t)__builtin_return_address(0), n))   /* S1BFIX: */
            itrack_put2(p, n, 0, (uint32_t)RVA(__builtin_return_address(0)));
        if ((f & CTX_CENSUS) && p) {
            uint32_t c = core_caller();
            g_sc.n_alloc++; g_sc.bytes_alloc += n;
            spin_lock(&g_track_lock);
            caller_add(g_c_alloc, c, n);
            track_put((uintptr_t)p, (uint32_t)n, c, g_cur_seq);
            spin_unlock(&g_track_lock);
        }
        return p;
    }
    void *p = o_malloc(self, n, al);
    if (p && g_track_core && core_site((uintptr_t)__builtin_return_address(0)) && !deny_site((uintptr_t)__builtin_return_address(0), n))   /* S1BFIX: */
        itrack_put2(p, n, 2, (uint32_t)RVA(__builtin_return_address(0)));
    return p;
}

static void census_free_in(void *p, void *ret) {
    TrackEnt t;
    spin_lock(&g_track_lock);
    if (g_track_n && track_take((uintptr_t)p, &t)) {
        g_sc.n_free_step++;
        uint64_t age = g_cur_seq - t.seq;
        if (age == 0) g_life_same++; else if (age <= 8) g_life_le8++; else if (age <= 60) g_life_le60++; else g_life_gt60++;
    } else {
        size_t sz = 0;
        if (o_getsize && !o_getsize(g_gm_obj, p, &sz)) sz = 0;
        g_sc.n_free_pre++; g_sc.bytes_free_pre += sz;
        caller_add(g_c_prefree, caller_rva(ret), sz);
    }
    spin_unlock(&g_track_lock);
}

static void hk_free(void *self, void *p) {
    if (!p) { o_free(self, p); return; }
    if (g_arena_ok && arena_owns(&g_arena, p)) {
        int inside = in_ctx();
        if (inside) g_sc.n_arena_free++;
        else if (g_l4_in_call) l4_note_heap_free((uintptr_t)p, arena_usize(&g_arena, p));   /* fix R2 */
        arena_release(p, inside);
        return;
    }
    if (in_ctx()) {
        uint32_t f = g_ctx_flags;
        if (f & CTX_DEFER) {
            spin_lock(&g_def_lock);
            int r = defer_add(&g_def, (uintptr_t)p, g_cur_seq);
            spin_unlock(&g_def_lock);
            if (r == 1 && g_rb_mem_on) rb_def_note((uintptr_t)p);   /* v0.23.1 (F10): a pinned block's pin is hidden for later frames */
            if (r) { g_sc.n_defer++; return; }
            g_defer_broken = 1;   /* table full: freeing now would be unsafe for rollback; caller falls back */
            /* keep the block alive (leak) rather than free it under a possible rollback */
            return;
        }
        if (f & CTX_CENSUS) census_free_in(p, __builtin_return_address(0));
        itrack_remove((uintptr_t)p);
        o_free(self, p);
        return;
    }
    if (g_track_n && (uintptr_t)p >= g_track_lo && (uintptr_t)p <= g_track_hi) {
        TrackEnt t;
        spin_lock(&g_track_lock);
        if (g_track_n && track_take((uintptr_t)p, &t)) { g_life_out++; caller_add(g_c_outfree, caller_rva(__builtin_return_address(0)), t.size); }
        spin_unlock(&g_track_lock);
    }
    int dhit = 0;
    if (g_def.n) {
        spin_lock(&g_def_lock);
        int hit = defer_remove(&g_def, (uintptr_t)p);
        if (hit) def_hit_note((uintptr_t)p);   /* v0.23.1 */
        spin_unlock(&g_def_lock);
        if (hit) { InterlockedIncrement64(&g_def_outside_hits); dhit = 1; }
    }
    if (g_rb_mem_on) {   /* v0.23.0-C1b (rbreplay, rb on): a pinned block is quarantined (1) or marked dead and freed now (2); no pins_note_free */
        int q = rb_mem_free((uintptr_t)p, 0, dhit);
        if (q == 1) {
            itrack_remove((uintptr_t)p);
            if (g_l4_in_call) { size_t sz = 0; if (o_getsize && l4_free_wants_size() && !o_getsize(g_gm_obj, p, &sz)) sz = 0; l4_note_heap_free((uintptr_t)p, sz); }
            return;
        }
    } else if (g_npins) pins_note_free((uintptr_t)p);
    itrack_remove((uintptr_t)p);
    if (g_l4_in_call) { size_t sz = 0; if (o_getsize && l4_free_wants_size() && !o_getsize(g_gm_obj, p, &sz)) sz = 0; l4_note_heap_free((uintptr_t)p, sz); }   /* fix R2 */
    o_free(self, p);
}

static void *hk_realloc(void *self, void *p, size_t n, uint32_t al) {
    int inside = in_ctx();
    uint32_t f = inside ? g_ctx_flags : 0;
    if (inside) g_sc.n_realloc++;
    if (p && g_arena_ok && arena_owns(&g_arena, p)) {
        if (n == 0) { arena_release(p, inside); return NULL; }
        size_t old = arena_usize(&g_arena, p);
        void *q = NULL;
        if ((f & CTX_ROUTE) && !g_route_broken) q = arena_route(n, al);
        if (!q && (f & CTX_ROUTE) && !(f & CTX_INIT)) g_route_broken = 1;
        if (!q) q = o_malloc(self, n, al);
        if (q) { memcpy(q, p, old < n ? old : n); arena_release(p, inside); }
        return q;
    }
    if ((f & CTX_ROUTE) && g_arena_ok && !g_route_broken) {
        if (!p) return hk_malloc(self, n, al);
        if (n == 0) { hk_free(self, p); return NULL; }
        size_t old = 0;
        if (o_getsize && o_getsize(self, p, &old) && old) {
            void *q = arena_route(n, al);
            if (q) {
                memcpy(q, p, old < n ? old : n);
                hk_free(self, p);   /* defers inside a rollback step */
                return q;
            }
        }
        if (!(f & CTX_INIT)) g_route_broken = 1;
        LOG("WARN realloc_unroutable p=%p n=%zu old=%zu\n", p, n, old);
    }
    if (p) { itrack_remove((uintptr_t)p); if (g_rb_mem_on) rb_mem_realloc((uintptr_t)p); else if (g_npins) pins_note_free((uintptr_t)p); }   /* v0.23.0-C1b */
    size_t l4sz = 0;
    if (p && g_l4_in_call && o_getsize && !o_getsize(g_gm_obj, p, &l4sz)) l4sz = 0;
    void *q = o_realloc(self, p, n, al);
    if (p && q != p && g_l4_in_call) l4_note_heap_free((uintptr_t)p, l4sz);   /* fix R2: the old block was freed */
    if (q && g_track_core && core_site((uintptr_t)__builtin_return_address(0)) && !deny_site((uintptr_t)__builtin_return_address(0), n))   /* S1BFIX: */
        itrack_put2(q, n, 2, (uint32_t)RVA(__builtin_return_address(0)));
    return q;
}

/* ------------------------------------------------------------------ CRT heap hooks
 * The core's own allocator (0x144855710, used for WorkMemory/KeyRecordBuf) sits on
 * CRT malloc (0x14037c630), and SaveState/LoadState (inside the step closure) call
 * CRT free. Same policy as the GMalloc hooks; deferred CRT blocks are tagged bit0=1. */
static void *hk_cmalloc(size_t n) {
    if (in_ctx()) {
        uint32_t f = g_ctx_flags;
        InterlockedIncrement64(&g_crt_in_allocs);
        g_sc.n_crt_alloc++;
        if ((f & CTX_ROUTE) && g_arena_ok && !g_route_broken) {
            void *p = arena_route(n, 16);
            if (p) { if (f & CTX_INIT) { InterlockedIncrement64(&g_init_allocs); InterlockedAdd64(&g_init_bytes, (LONG64)n); } return p; }
            if (f & CTX_INIT) InterlockedIncrement64(&g_init_route_fail); else g_route_broken = 1;
        }
        void *p = o_cmalloc(n);
        if (p && ((f & CTX_TRACK) || (g_track_core && core_site((uintptr_t)__builtin_return_address(0)))) && !deny_site((uintptr_t)__builtin_return_address(0), n))   /* S1BFIX: */
            itrack_put2(p, n, 1, (uint32_t)RVA(__builtin_return_address(0)));
        if ((f & CTX_CENSUS) && p) caller_add(g_c_crt, core_caller(), n);
        return p;
    }
    void *p = o_cmalloc(n);
    if (p && g_track_core && core_site((uintptr_t)__builtin_return_address(0)) && !deny_site((uintptr_t)__builtin_return_address(0), n))   /* S1BFIX: */
        itrack_put2(p, n, 3, (uint32_t)RVA(__builtin_return_address(0)));
    return p;
}

static void *hk_ccalloc(size_t a, size_t b) {
    if (in_ctx() && (g_ctx_flags & CTX_ROUTE) && g_arena_ok && !g_route_broken && b && a <= (SIZE_MAX / b)) {
        void *p = hk_cmalloc(a * b);
        if (p) memset(p, 0, a * b);
        return p;
    }
    if (in_ctx()) g_sc.n_crt_alloc++;
    return o_ccalloc(a, b);
}

static void hk_cfree(void *p) {
    if (!p) return;
    if (g_arena_ok && arena_owns(&g_arena, p)) { int inside = in_ctx(); if (inside) g_sc.n_arena_free++; arena_release(p, inside); return; }
    if (in_ctx()) {
        uint32_t f = g_ctx_flags;
        InterlockedIncrement64(&g_crt_in_frees);
        g_sc.n_crt_free++;
        if (f & CTX_DEFER) {
            spin_lock(&g_def_lock);
            int r = defer_add(&g_def, (uintptr_t)p | 1, g_cur_seq);
            spin_unlock(&g_def_lock);
            if (r == 1 && g_rb_mem_on) rb_def_note((uintptr_t)p);   /* v0.23.1 (F10) */
            if (r) { g_sc.n_defer++; return; }
            g_defer_broken = 1;
            return;
        }
        itrack_remove((uintptr_t)p);
        o_cfree(p);
        return;
    }
    int dhit = 0;
    if (g_def.n) {
        spin_lock(&g_def_lock);
        int hit = defer_remove(&g_def, (uintptr_t)p | 1);
        if (hit) def_hit_note((uintptr_t)p | 1);   /* v0.23.1 */
        spin_unlock(&g_def_lock);
        if (hit) { InterlockedIncrement64(&g_def_outside_hits); dhit = 1; }
    }
    if (g_rb_mem_on) {   /* v0.23.0-C1b: as hk_free */
        int q = rb_mem_free((uintptr_t)p, 1, dhit);
        if (q == 1) { itrack_remove((uintptr_t)p); if (g_l4_in_call) l4_note_heap_free((uintptr_t)p, 16); return; }
    } else if (g_npins) pins_note_free((uintptr_t)p);
    itrack_remove((uintptr_t)p);
    if (g_l4_in_call) l4_note_heap_free((uintptr_t)p, 16);   /* fix R2 */
    o_cfree(p);
}

static void *hk_crealloc(void *p, size_t n) {
    int inside = in_ctx();
    uint32_t f = inside ? g_ctx_flags : 0;
    if (p && g_arena_ok && arena_owns(&g_arena, p)) {
        if (n == 0) { hk_cfree(p); return NULL; }
        size_t old = arena_usize(&g_arena, p);
        void *q = NULL;
        if ((f & CTX_ROUTE) && !g_route_broken) q = arena_route(n, 16);
        if (!q && (f & CTX_ROUTE) && !(f & CTX_INIT)) g_route_broken = 1;
        if (!q) q = o_cmalloc(n);
        if (q) { memcpy(q, p, old < n ? old : n); arena_release(p, inside); }
        return q;
    }
    if ((f & CTX_ROUTE) && g_arena_ok && !g_route_broken) {
        if (!p) return hk_cmalloc(n);
        if (n == 0) { hk_cfree(p); return NULL; }
        size_t old = _msize(p);
        void *q = arena_route(n, 16);
        if (q && old != (size_t)-1) { memcpy(q, p, old < n ? old : n); hk_cfree(p); return q; }
        if (q) arena_release(q, 1);
        if (!(f & CTX_INIT)) g_route_broken = 1;
    }
    if (p) { itrack_remove((uintptr_t)p); if (g_rb_mem_on) rb_mem_realloc((uintptr_t)p); else if (g_npins) pins_note_free((uintptr_t)p); }   /* v0.23.0-C1b */
    void *q = o_crealloc(p, n);
    if (p && q != p && g_l4_in_call) l4_note_heap_free((uintptr_t)p, 16);   /* fix R2 */
    if (q && g_track_core && core_site((uintptr_t)__builtin_return_address(0))) itrack_put2(q, n, 3, (uint32_t)RVA(__builtin_return_address(0)));
    return q;
}

int mem_install_crt(void **iat_malloc, void **iat_free, void **iat_realloc, void **iat_calloc) {
    o_cmalloc = (CMallocFn)*iat_malloc;
    o_cfree = (CFreeFn)*iat_free;
    o_crealloc = (CReallocFn)*iat_realloc;
    o_ccalloc = (CCallocFn)*iat_calloc;
    return patch_ptr(iat_malloc, (void *)hk_cmalloc) && patch_ptr(iat_free, (void *)hk_cfree) &&
           patch_ptr(iat_realloc, (void *)hk_crealloc) && patch_ptr(iat_calloc, (void *)hk_ccalloc);
}

static uint8_t hk_getsize(void *self, void *p, size_t *out) {
    if (p && g_arena_ok && arena_owns(&g_arena, p)) {
        size_t u = arena_usize(&g_arena, p);
        if (out) *out = u;
        return u != 0;
    }
    return o_getsize ? o_getsize(self, p, out) : 0;
}

static int patch_ptr(void **slot, void *val) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer(slot, val);
    DWORD tmp;
    VirtualProtect(slot, sizeof(void *), old, &tmp);
    return 1;
}

int mem_install(void) {
    if (g_mem_hooked) return 1;
    void *gm = *(void **)VA(A_GMALLOC);
    if (!gm || !mem_readable(gm, 8)) { LOG("MEMHOOK fail gmalloc=%p\n", gm); return 0; }
    void **vt = *(void ***)gm;
    if (!IN_IMAGE(vt)) { LOG("MEMHOOK fail vtable %p not in image\n", (void *)vt); return 0; }
    for (int i = 2; i <= 6; i++)
        if (!IN_IMAGE(vt[i])) { LOG("MEMHOOK fail slot %d -> %p not in image\n", i, vt[i]); return 0; }
    g_gm_obj = gm;
    o_malloc = (MallocFn)vt[GM_SLOT_MALLOC];
    o_realloc = (ReallocFn)vt[GM_SLOT_REALLOC];
    o_free = (FreeFn)vt[GM_SLOT_FREE];
    o_getsize = (GetSizeFn)vt[GM_SLOT_GETSIZE];
    LOG("MEMHOOK gmalloc=%p vt_rva=0x%llx malloc=0x%llx realloc=0x%llx free=0x%llx getsize=0x%llx\n", gm,
        (unsigned long long)RVA(vt), (unsigned long long)RVA(o_malloc), (unsigned long long)RVA(o_realloc),
        (unsigned long long)RVA(o_free), (unsigned long long)RVA(o_getsize));
    int ok = patch_ptr(&vt[GM_SLOT_MALLOC], (void *)hk_malloc) && patch_ptr(&vt[GM_SLOT_REALLOC], (void *)hk_realloc) &&
             patch_ptr(&vt[GM_SLOT_FREE], (void *)hk_free) && patch_ptr(&vt[GM_SLOT_GETSIZE], (void *)hk_getsize);
    g_mem_hooked = ok;
    if (!ok) LOG("MEMHOOK patch failed err=%lu\n", GetLastError());
    return ok;
}

/* v0.23.0-C1b: the real free of a quarantined block (rb.c pinq_release, step thread) */
void mem_free_now(uintptr_t p, int crt) {
    if (!p) return;
    if (crt) o_cfree((void *)p); else o_free(g_gm_obj, (void *)p);
}

/* optA M2 (rbonline, M2-INTERFACES.md §9 row M1): the allocator hooks are installed at ARM and taken out again after the battle's Shutdown
 * (rbo_core.c checks the arena is empty and no free is deferred first). A slot that no longer holds our hook is left alone (someone else
 * patched it after us) and counts as not restored. Existing modes never call these. */
static int unpatch_if(void **slot, void *ours, void *orig) {
    if (*slot != ours) return 0;
    return patch_ptr(slot, orig);
}
int mem_uninstall(void) {
    if (!g_mem_hooked || !g_gm_obj) return 1;
    void **vt = *(void ***)g_gm_obj;
    int ok = unpatch_if(&vt[GM_SLOT_MALLOC], (void *)hk_malloc, (void *)o_malloc);
    ok &= unpatch_if(&vt[GM_SLOT_REALLOC], (void *)hk_realloc, (void *)o_realloc);
    ok &= unpatch_if(&vt[GM_SLOT_FREE], (void *)hk_free, (void *)o_free);
    ok &= unpatch_if(&vt[GM_SLOT_GETSIZE], (void *)hk_getsize, (void *)o_getsize);
    if (ok) g_mem_hooked = 0;
    LOG("MEMHOOK uninstall ok=%d\n", ok);
    return ok;
}
int mem_uninstall_crt(void **iat_malloc, void **iat_free, void **iat_realloc, void **iat_calloc) {
    if (!o_cmalloc) return 1;
    int ok = unpatch_if(iat_malloc, (void *)hk_cmalloc, (void *)o_cmalloc);
    ok &= unpatch_if(iat_free, (void *)hk_cfree, (void *)o_cfree);
    ok &= unpatch_if(iat_realloc, (void *)hk_crealloc, (void *)o_crealloc);
    ok &= unpatch_if(iat_calloc, (void *)hk_ccalloc, (void *)o_ccalloc);
    LOG("CRTHEAP uninstall ok=%d\n", ok);
    return ok;
}

/* optA M2 v0.29.1 (review B1): rbonline's allocator FRONT DOORS (src/rbo_thunks.S rbo_md_*; the cells are rbo_core.c's). Installed once, at
 * the first ARM of the process (game thread), and never removed: the slots then hold `jmp [cell]` doors, so whether a call reaches the C hooks
 * above (armed battles) or the original with the game's exact context (every other time) is a cell switch in OUR memory, never another
 * patch of the game's vtable / IAT. The hooks' originals (o_*) are captured here exactly as mem_install / mem_install_crt capture them.
 * idx: 0..3 GMalloc Malloc / Realloc / Free / GetAllocationSize, 4..7 CRT malloc / free / realloc / calloc. orig[] / hk[] / cell[] are filled
 * (cell = orig) BEFORE any slot points at a door. iat[] NULL = no CRT part (cells 4..7 stay unused). 1 = every slot patched. */
int mem_doors_install(void *const door[8], void **iat[4], void *orig[8], void *hk[8], void *volatile cell[8]) {
    if (g_mem_hooked) { LOG("MEMDOORS refused: the GMalloc hooks of another mode are installed\n"); return 0; }
    void *gm = *(void **)VA(A_GMALLOC);
    if (!gm || !mem_readable(gm, 8)) { LOG("MEMDOORS fail gmalloc=%p\n", gm); return 0; }
    void **vt = *(void ***)gm;
    if (!IN_IMAGE(vt)) { LOG("MEMDOORS fail vtable %p not in image\n", (void *)vt); return 0; }
    for (int i = 2; i <= 6; i++) if (!IN_IMAGE(vt[i])) { LOG("MEMDOORS fail slot %d -> %p not in image\n", i, vt[i]); return 0; }
    g_gm_obj = gm;
    o_malloc = (MallocFn)vt[GM_SLOT_MALLOC]; o_realloc = (ReallocFn)vt[GM_SLOT_REALLOC]; o_free = (FreeFn)vt[GM_SLOT_FREE]; o_getsize = (GetSizeFn)vt[GM_SLOT_GETSIZE];
    orig[0] = (void *)o_malloc; orig[1] = (void *)o_realloc; orig[2] = (void *)o_free; orig[3] = (void *)o_getsize;
    hk[0] = (void *)hk_malloc; hk[1] = (void *)hk_realloc; hk[2] = (void *)hk_free; hk[3] = (void *)hk_getsize;
    int crt = iat && iat[0] && iat[1] && iat[2] && iat[3];
    if (crt) {
        o_cmalloc = (CMallocFn)*iat[0]; o_cfree = (CFreeFn)*iat[1]; o_crealloc = (CReallocFn)*iat[2]; o_ccalloc = (CCallocFn)*iat[3];
        orig[4] = (void *)o_cmalloc; orig[5] = (void *)o_cfree; orig[6] = (void *)o_crealloc; orig[7] = (void *)o_ccalloc;
        hk[4] = (void *)hk_cmalloc; hk[5] = (void *)hk_cfree; hk[6] = (void *)hk_crealloc; hk[7] = (void *)hk_ccalloc;
    }
    for (int i = 0; i < 8; i++) cell[i] = orig[i];
    MemoryBarrier();
    int ok = patch_ptr(&vt[GM_SLOT_MALLOC], door[0]) && patch_ptr(&vt[GM_SLOT_REALLOC], door[1]) && patch_ptr(&vt[GM_SLOT_FREE], door[2]) &&
             patch_ptr(&vt[GM_SLOT_GETSIZE], door[3]);
    if (ok && crt) ok = patch_ptr(iat[0], door[4]) && patch_ptr(iat[1], door[5]) && patch_ptr(iat[2], door[6]) && patch_ptr(iat[3], door[7]);
    g_mem_hooked = ok;   /* modes_rbo_step's arena/memhook check */
    LOG("MEMDOORS gmalloc=%p vt_rva=0x%llx malloc=0x%llx realloc=0x%llx free=0x%llx getsize=0x%llx crt=%d ok=%d (front doors: cells = the originals until ARM)\n", gm,
        (unsigned long long)RVA(vt), (unsigned long long)RVA(o_malloc), (unsigned long long)RVA(o_realloc), (unsigned long long)RVA(o_free),
        (unsigned long long)RVA(o_getsize), crt, ok);
    return ok;
}
