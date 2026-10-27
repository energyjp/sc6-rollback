/* v0.23.0-C1b: the emulated network (see emu.h). */
#include "emu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

uint64_t emu_splitmix_next(uint64_t *state) {
    *state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t emu_fnv1a(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
    return h;
}

void emu_free(EmuSched *es) {
    free(es->known); free(es->pmax); free(es->raw_late);
    memset(es, 0, sizeof *es);
}

#define EMU_INF ((int64_t)1 << 60)
int emu_build(const EmuCfg *c, EmuSched *es) {
    memset(es, 0, sizeof *es);
    if (!c->N || c->R < 1) return 0;
    uint32_t N = c->N, R = (uint32_t)c->R;
    size_t np = (size_t)N + R + 1;
    int64_t *arr = (int64_t *)malloc(sizeof(int64_t) * np);
    es->known = (int32_t *)calloc((size_t)N + 1, sizeof(int32_t));
    es->pmax = (int32_t *)calloc((size_t)N + 1, sizeof(int32_t));
    es->raw_late = (int32_t *)calloc((size_t)N + 1, sizeof(int32_t));
    if (!arr || !es->known || !es->pmax || !es->raw_late) { free(arr); emu_free(es); return 0; }
    es->N = N;
    int64_t L = (int64_t)c->lat_ms * 1000, J = (int64_t)c->jit_ms * 1000;
    uint64_t st = c->seed;
    for (uint32_t p = 1; p + 1 <= N + R; p++) {   /* p = 1 .. N+R-1 */
        uint64_t u1 = emu_splitmix_next(&st), u2 = emu_splitmix_next(&st);
        int64_t j = (int64_t)(u1 % (uint64_t)(2 * J + 1)) - J;
        int lost = (u2 % 10000) < (uint64_t)c->loss_pct * 100;
        int64_t d = L + j; if (d < 0) d = 0;
        arr[p] = lost ? EMU_INF : (int64_t)p - c->D + (d + EMU_T_US - 1) / EMU_T_US;
    }
    int64_t Lmax = c->max_rb - 1;
    uint64_t h = EMU_FNV_BASIS;
    int32_t pm = INT32_MIN;
    for (uint32_t f = 1; f <= N; f++) {
        int64_t kr = EMU_INF;
        for (uint32_t p = f; p <= f + R - 1; p++) if (arr[p] < kr) kr = arr[p];
        int64_t k = kr;
        if (kr > (int64_t)f + Lmax) { k = (int64_t)f + Lmax; es->clamps++; }
        es->raw_late[f] = kr >= EMU_INF ? INT32_MAX : (int32_t)(kr - (int64_t)f);
        es->known[f] = (int32_t)k;
        if ((int32_t)k > pm) pm = (int32_t)k;
        es->pmax[f] = pm;
        int64_t late = k - (int64_t)f, hi = late - EMU_HIST_LO;
        if (hi < 0) hi = 0;
        if (hi >= EMU_HIST_N) hi = EMU_HIST_N - 1;
        es->hist[hi]++;
        uint32_t w[2] = { f, (uint32_t)(int32_t)k };
        uint8_t b[8];
        for (int i = 0; i < 4; i++) { b[i] = (uint8_t)(w[0] >> (8 * i)); b[4 + i] = (uint8_t)(w[1] >> (8 * i)); }
        h = emu_fnv1a(b, 8, h);
    }
    es->sched = h;
    free(arr);
    return 1;
}

uint32_t emu_frontier(const EmuSched *es, int64_t s) {
    if (!es->N || (int64_t)es->pmax[1] > s) return 0;
    uint32_t lo = 1, hi = es->N;   /* pmax is nondecreasing: the last f with pmax[f] <= s */
    while (lo < hi) {
        uint32_t m = lo + (hi - lo + 1) / 2;
        if ((int64_t)es->pmax[m] <= s) lo = m; else hi = m - 1;
    }
    return lo;
}

size_t emu_hist_str(const EmuSched *es, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < EMU_HIST_N; i++) {
        if (!es->hist[i]) continue;
        int k = snprintf(out + o, cap > o ? cap - o : 0, "%s%d:%u", o ? "," : "", i + EMU_HIST_LO, es->hist[i]);
        if (k < 0 || o + (size_t)k >= cap) break;
        o += (size_t)k;
    }
    if (!o && cap > 1) { out[0] = '-'; out[1] = 0; o = 1; }
    return o;
}

uint64_t rb_cfg_hash(const RbCfgVals *v) {
    int64_t a[] = { v->remote_side, v->net_delay, v->max_rb, v->lat_ms, v->jit_ms, v->loss_pct, v->redundancy, (int64_t)v->seed,
                    (int64_t)v->state_slots_mask, v->mt_tail, v->pinguard, v->quarantine, v->t2_heap_tag, v->rollback, v->skip_restore,
                    v->tick_limit, v->state_gate, v->test_layout_at };
    uint64_t h = EMU_FNV_BASIS;
    for (size_t i = 0; i < sizeof a / sizeof a[0]; i++) {
        uint8_t b[8]; uint64_t x = (uint64_t)a[i];
        for (int k = 0; k < 8; k++) b[k] = (uint8_t)(x >> (8 * k));
        h = emu_fnv1a(b, 8, h);
    }
    if (v->state_quiet) {   /* v0.23.1: appended only when set, so every v0.23.0 configuration keeps its rbcfg value */
        uint8_t b[8]; uint64_t x = (uint64_t)v->state_quiet;
        for (int k = 0; k < 8; k++) b[k] = (uint8_t)(x >> (8 * k));
        h = emu_fnv1a(b, 8, h);
    }
    /* v0.24.0-C1b2 (C1B-R2-SPEC.md §2.9): the zone keys only when rb_zone != 0, live_cap only in rblive (a 'live' marker first) */
    int64_t z[] = { v->zone, v->zone_mode, v->zone_gap, v->zone_margin }, l[] = { 0x6576696cLL, v->live_cap };
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0 ? !v->zone : !v->live) continue;
        const int64_t *a = pass == 0 ? z : l; int n = pass == 0 ? 4 : 2;
        for (int i = 0; i < n; i++) {
            uint8_t b[8]; uint64_t x = (uint64_t)a[i];
            for (int k = 0; k < 8; k++) b[k] = (uint8_t)(x >> (8 * k));
            h = emu_fnv1a(b, 8, h);
        }
    }
    return h;
}

uint64_t rb_parse_state_slots(const char *s, char *bad_tok, size_t cap) {
    uint64_t m = 0; int n = 0;
    if (bad_tok && cap) bad_tok[0] = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        const char *t0 = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        char tok[32]; size_t tl = (size_t)(p - t0); if (tl >= sizeof tok) tl = sizeof tok - 1;
        memcpy(tok, t0, tl); tok[tl] = 0;
        char *e; unsigned long v = strtoul(tok, &e, 16);   /* "0x58" and "58" are both hex */
        if (*e || !tl || v % 8 || v < 0x8 || v > 0x130) { if (bad_tok && cap) snprintf(bad_tok, cap, "%s", tok); return 0; }
        m |= 1ULL << (v / 8); n++;
    }
    if (!n) { if (bad_tok && cap) snprintf(bad_tok, cap, "(empty)"); return 0; }
    return m;
}

size_t rb_state_slots_str(uint64_t mask, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < 64; i++) {
        if (!(mask >> i & 1)) continue;
        int k = snprintf(out + o, cap > o ? cap - o : 0, "%s%x", o ? "," : "", i * 8);
        if (k < 0 || o + (size_t)k >= cap) break;
        o += (size_t)k;
    }
    return o;
}

int rbq_take(RbQEnt *list, int *n, uint32_t H, int all, RbQEnt *out, int max) {
    int w = 0, k = 0;
    for (int i = 0; i < *n; i++) {
        if ((all || list[i].q <= H) && k < max) { out[k++] = list[i]; continue; }
        list[w++] = list[i];
    }
    *n = w;
    return k;
}
