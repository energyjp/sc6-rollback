/* v0.23.0-C1b: the emulated network for the ghost replay (C1B-SPEC.md §2.3). Pure C: no game state, no Windows, no log.
 * Shared by the plugin (rb.c) and the unit test; tools/emu_preview.py implements the same rules.
 *
 *   PRNG splitmix64 (initial state = seed). For packets p = 1 .. N+R-1 in order:
 *     u1 = next(); u2 = next(); j = (int64)(u1 % (2J+1)) - J; lost = (u2 % 10000) < loss*100; d = max(0, L + j)
 *     arrive(p) = p - D + (d + T - 1) / T   (T = 16667 us; a lost packet never arrives)
 *   Packet p carries the remote records of frames max(1, p-R+1) .. p.
 *   known_raw(f) = min over p in [f, f+R-1] of arrive(p); known(f) = min(known_raw(f), f + Lmax), Lmax = max_rb - 1 (one clamp
 *   when the min applies). Lateness(f) = known(f) - f.
 *   sched = FNV-1a 64 over u32le(f) || u32le(known(f)) for f = 1..N.
 *   Frontier F(s) = the largest f such that known(g) <= s for every g <= f (the contiguous frontier; it may be >= s). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define EMU_T_US     16667
#define EMU_HIST_LO  (-8)          /* lateness histogram covers [EMU_HIST_LO, EMU_HIST_LO + EMU_HIST_N) */
#define EMU_HIST_N   48

typedef struct {
    uint32_t N;                    /* the recording's frame count */
    uint64_t seed;
    int lat_ms, jit_ms, loss_pct;  /* L, J (ms), loss (integer percent) */
    int D, R, max_rb;              /* net_delay, emu_redundancy, net_max_rollback */
} EmuCfg;

typedef struct {
    uint32_t N;
    int32_t *known;                /* [1..N] (index 0 unused) */
    int32_t *pmax;                 /* prefix max of known (the frontier search) */
    uint64_t sched;
    uint32_t clamps;
    uint32_t hist[EMU_HIST_N];     /* lateness counts, index = lateness - EMU_HIST_LO (clamped to the ends) */
    int32_t *raw_late;             /* [1..N] raw lateness before the clamp, INT32_MAX = never arrives (RBCLAMP lines) */
} EmuSched;

uint64_t emu_splitmix_next(uint64_t *state);
uint64_t emu_fnv1a(const void *p, size_t n, uint64_t h);   /* h = 0xcbf29ce484222325 to start */
#define EMU_FNV_BASIS 0xcbf29ce484222325ULL
/* 1 = ok, 0 = out of memory or N == 0. es is filled (free with emu_free). */
int      emu_build(const EmuCfg *c, EmuSched *es);
void     emu_free(EmuSched *es);
/* F(s) for s >= 0 (0 when known(1) > s); never more than N */
uint32_t emu_frontier(const EmuSched *es, int64_t s);
/* "l:n,l:n" of the nonzero histogram buckets (ascending lateness); returns the length */
size_t   emu_hist_str(const EmuSched *es, char *out, size_t cap);

/* the remote-record prediction (§2.3): held_c | ((held_c & ~held(prev)) << 32), held_c = (uint32_t)base */
static inline uint64_t emu_predict(uint64_t prev_used, uint64_t base) {
    uint32_t held = (uint32_t)base, prev = (uint32_t)prev_used;
    return (uint64_t)held | ((uint64_t)(held & ~prev) << 32);
}

/* the rbcfg hash (§2.9): FNV-1a 64 over the parsed rb/net/emu/c1a_t2_heap/rb_mt_tail values, in this order, each as u64 LE */
typedef struct {
    int64_t remote_side, net_delay, max_rb, lat_ms, jit_ms, loss_pct, redundancy;
    uint64_t seed;
    uint64_t state_slots_mask;     /* bit i = listener slot offset 8*i is state class */
    int64_t mt_tail, pinguard, quarantine, t2_heap_tag, rollback, skip_restore, tick_limit, state_gate, test_layout_at;
    int64_t state_quiet;           /* v0.23.1: hashed only when nonzero (the v0.23.0 rbcfg values stay as they were) */
    int64_t zone, zone_mode, zone_gap, zone_margin;   /* v0.24.0-C1b2: hashed only when zone != 0 */
    int64_t live, live_cap;        /* v0.24.0-C1b2: hashed only when live != 0 (mode=rblive) */
} RbCfgVals;
uint64_t rb_cfg_hash(const RbCfgVals *v);
/* "0x58,0x60" -> mask (bit = offset / 8); 0 = parse error: not a multiple of 8, outside 0x8..0x130, or 0x138/0x140 (*bad_tok gets it) */
uint64_t rb_parse_state_slots(const char *s, char *bad_tok, size_t cap);
/* the mask as "58[,60..]" (hex, no 0x) */
size_t   rb_state_slots_str(uint64_t mask, char *out, size_t cap);

/* the pin quarantine list (C1B-SPEC §2.5): rbq_take moves every entry with q <= H (all = every entry) to out (order kept), at most max;
 * returns the count. An entry with q is released only when the release horizon H has reached q. */
typedef struct { uintptr_t addr; uint32_t len; int slot, crt; uint32_t q; uint32_t rkey; } RbQEnt;   /* rkey: rb.c slot re-resolution (v0.23.1) */
int rbq_take(RbQEnt *list, int *n, uint32_t H, int all, RbQEnt *out, int max);
