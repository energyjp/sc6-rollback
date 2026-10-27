#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "snap.h"

/* ------------------------------------------------------------------ hash */
#define P1 0x9E3779B185EBCA87ULL
#define P2 0xC2B2AE3D27D4EB4FULL
#define P3 0x165667B19E3779F9ULL
#define P4 0x85EBCA77C2B2AE63ULL
#define P5 0x27D4EB2F165667C5ULL
static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t round1(uint64_t acc, uint64_t in) { acc += in * P2; acc = rotl64(acc, 31); return acc * P1; }
static inline uint64_t avalanche(uint64_t h) {
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32; return h;
}

uint64_t hash_bytes(const void *data, size_t n, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data, *end = p + n;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t *lim = end - 32;
        do {
            v1 = round1(v1, rd64(p)); v2 = round1(v2, rd64(p + 8));
            v3 = round1(v3, rd64(p + 16)); v4 = round1(v4, rd64(p + 24));
            p += 32;
        } while (p <= lim);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = (h ^ round1(0, v1)) * P1 + P4;
        h = (h ^ round1(0, v2)) * P1 + P4;
        h = (h ^ round1(0, v3)) * P1 + P4;
        h = (h ^ round1(0, v4)) * P1 + P4;
    } else {
        h = seed + P5;
    }
    h += (uint64_t)n;
    while (p + 8 <= end) { h ^= round1(0, rd64(p)); h = rotl64(h, 27) * P1 + P4; p += 8; }
    while (p < end) { h ^= (*p) * P5; h = rotl64(h, 11) * P1; p++; }
    return avalanche(h);
}

/* ------------------------------------------------------------------ set */
void snapset_init(SnapSet *s) { memset(s, 0, sizeof *s); }

int snapset_add_region(SnapSet *s, uintptr_t addr, size_t len, const char *name) {
    if (s->nreg >= SNAP_MAXREG || !addr || !len) return 0;
    SRegion *r = &s->reg[s->nreg++];
    r->addr = addr; r->len = len;
    strncpy(r->name, name ? name : "?", sizeof r->name - 1);
    return 1;
}

int snapset_add_mask(SnapSet *s, uintptr_t addr, size_t len) {
    if (s->nmask >= SNAP_MAXMASK || !len) return 0;
    s->mask[s->nmask].addr = addr; s->mask[s->nmask].len = len; s->nmask++;
    return 1;
}

/* by address; at equal addresses the longer pin first, so a covering pin always precedes the pins it covers */
static int pin_cmp(const void *a, const void *b) {
    const SPin *x = (const SPin *)a, *y = (const SPin *)b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    return x->len > y->len ? -1 : x->len < y->len;
}

int snapset_set_pins(SnapSet *s, const SPin *pins, int n) {
    if (n > s->pincap) {
        SPin *np = (SPin *)realloc(s->pins, sizeof(SPin) * (size_t)n);
        if (!np) return 0;
        s->pins = np; s->pincap = n;
    }
    memcpy(s->pins, pins, sizeof(SPin) * (size_t)n);
    qsort(s->pins, (size_t)n, sizeof(SPin), pin_cmp);
    /* v0.20.2: drop every pin that lies wholly inside ONE other pin (exact duplicates included). The scans add
     * pins independently (a camera layer buffer was pinned twice in session 2b; the whole camera function block
     * covers its slot pins and any container pin inside it). Overlapping bytes would be copied, restored and
     * hashed twice: harmless for restore (both copies hold the same bytes) but they double-count in diffs and
     * make blob offsets ambiguous. The kept pin with the furthest end so far starts at or before the current
     * one (sorted), so "end <= cov_end" means that single pin covers it. Partial overlaps are kept as they are. */
    int w = 0, dropped = 0; size_t dbytes = 0; uintptr_t cov_end = 0; int have = 0;
    for (int i = 0; i < n; i++) {
        SPin p = s->pins[i];
        uintptr_t end = p.addr + p.len;
        if (have && end <= cov_end) { dropped++; dbytes += p.len; continue; }
        s->pins[w++] = p;
        if (!have || end > cov_end) { cov_end = end; have = 1; }
    }
    s->npin = w;
    s->pins_dropped = dropped; s->pins_dropped_bytes = dbytes;
    n = w;
    s->pin_bytes = 0;
    for (int i = 0; i < n; i++) s->pin_bytes += s->pins[i].len;
    if (s->pin_bytes > s->pin_scratch_cap) {
        free(s->pin_scratch);
        s->pin_scratch = (uint8_t *)malloc(s->pin_bytes);
        s->pin_scratch_cap = s->pin_scratch ? s->pin_bytes : 0;
        if (!s->pin_scratch) { s->npin = 0; s->pin_bytes = 0; return 0; }
    }
    return 1;
}

void snapset_clear_pins(SnapSet *s) { s->npin = 0; s->pin_bytes = 0; if (s->stable) { s->gen++; s->nspin = 0; } }

int snapset_find_pin(const SnapSet *s, uintptr_t addr) {
    if (s->stable) return snapset_find_start(s, addr);
    int lo = 0, hi = s->npin - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (s->pins[m].addr == addr) return m;
        if (s->pins[m].addr < addr) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

int snapset_remove_pins_in(SnapSet *s, uintptr_t lo, uintptr_t hi) {
    if (s->stable) {   /* v0.23.0-C1b: never removed, marked dead (freed) */
        int k = 0;
        for (int i = 0; i < s->npin; i++)
            if (s->pstate[i] == SPIN_ALIVE && s->pins[i].addr + s->pins[i].len > lo && s->pins[i].addr < hi) { s->pstate[i] = SPIN_FREED; s->phidden[i] = 1; k++; }
        if (k) snapset_rebuild_visible(s);
        return k;
    }
    int w = 0, removed = 0;
    for (int i = 0; i < s->npin; i++) {
        SPin p = s->pins[i];
        if (p.addr + p.len > lo && p.addr < hi) { removed++; s->pin_bytes -= p.len; continue; }
        s->pins[w++] = p;
    }
    s->npin = w;
    return removed;
}

size_t snap_bytes(const SnapSet *s) {
    size_t t = 0;
    for (int i = 0; i < s->nreg; i++) t += s->reg[i].len;
    if (s->arena) t += arena_used(s->arena);
    return t + s->pin_bytes;
}

/* ------------------------------------------------------------------ buffers */
static int ensure(uint8_t **buf, size_t *cap, size_t need) {
    if (need <= *cap) return 1;
    size_t nc = (need + (1 << 20) - 1) & ~(size_t)((1 << 20) - 1);
    if (*cap && nc < *cap * 2) nc = (*cap * 2 + (1 << 20) - 1) & ~(size_t)((1 << 20) - 1);
    uint8_t *nb = (uint8_t *)VirtualAlloc(NULL, nc, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!nb) return 0;
    if (*buf) VirtualFree(*buf, 0, MEM_RELEASE);
    *buf = nb; *cap = nc;
    return 1;
}

static void lock_arena(SnapSet *s) { if (s->arena_lock) while (InterlockedCompareExchange(s->arena_lock, 1, 0)) YieldProcessor(); }
static void unlock_arena(SnapSet *s) { if (s->arena_lock) InterlockedExchange(s->arena_lock, 0); }

int snap_take(SnapSet *s, Snap *sn) {
    sn->valid = 0;
    for (int i = 0; i < s->nreg; i++) {
        if (!ensure(&sn->buf[i], &sn->cap[i], s->reg[i].len)) return 0;
        memcpy(sn->buf[i], (const void *)s->reg[i].addr, s->reg[i].len);
    }
    if (s->arena) {
        lock_arena(s);
        size_t used = arena_used(s->arena);
        if (!ensure(&sn->abuf, &sn->acap, used)) { unlock_arena(s); return 0; }
        memcpy(sn->abuf, s->arena->base, used);
        sn->alen = used;
        unlock_arena(s);
    } else sn->alen = 0;
    if (s->npin) {
        if (!ensure(&sn->pbuf, &sn->pcap, s->pin_bytes)) return 0;
        uint8_t *o = sn->pbuf;
        if (s->stable) {   /* v0.23.0-C1b: dead slots are not read (their block may be gone); their bytes stay zero */
            if (!ensure(&sn->pdead, &sn->pdcap, (size_t)s->npin)) return 0;
            for (int i = 0; i < s->npin; i++) {
                int dead = s->pstate[i] != SPIN_ALIVE;
                sn->pdead[i] = (uint8_t)dead;
                if (dead) memset(o, 0, s->pins[i].len); else memcpy(o, (const void *)s->pins[i].addr, s->pins[i].len);
                o += s->pins[i].len;
            }
        } else
        for (int i = 0; i < s->npin; i++) { memcpy(o, (const void *)s->pins[i].addr, s->pins[i].len); o += s->pins[i].len; }
    }
    sn->plen = s->pin_bytes;
    sn->npin = s->npin;
    sn->gen = s->gen;
    sn->valid = 1;
    return 1;
}

static void restore_stable_pins(SnapSet *s, const Snap *sn) {
    const uint8_t *o = sn->pbuf;
    for (int i = 0; i < sn->npin && i < s->npin; i++) {
        if (!sn->pdead[i] && s->pstate[i] == SPIN_ALIVE) memcpy((void *)s->pins[i].addr, o, s->pins[i].len);
        o += s->pins[i].len;
    }
}
void snap_restore(SnapSet *s, const Snap *sn) {
    if (!sn->valid) return;
    for (int i = 0; i < s->nreg; i++) memcpy((void *)s->reg[i].addr, sn->buf[i], s->reg[i].len);
    if (s->arena && sn->alen) {
        lock_arena(s);
        memcpy(s->arena->base, sn->abuf, sn->alen);
        unlock_arena(s);
    }
    if (s->stable) {   /* v0.23.0-C1b: the prefix rule; a non-prefix layout skips the pins and is counted (the caller logs it) */
        if (snap_layout_prefix(s, sn)) restore_stable_pins(s, sn); else if (sn->npin || s->npin) s->layout_skips++;
        return;
    }
    if (s->npin && sn->npin == s->npin && sn->plen == s->pin_bytes) {
        const uint8_t *o = sn->pbuf;
        for (int i = 0; i < s->npin; i++) { memcpy((void *)s->pins[i].addr, o, s->pins[i].len); o += s->pins[i].len; }
    } else if (s->npin || sn->npin) s->layout_skips++;   /* v0.23.0-C1b: today's silent skip, now counted (SNAP LAYOUT_MISMATCH once) */
}
int snap_layout_prefix(const SnapSet *s, const Snap *sn) {
    if (!s->stable) return sn->npin == s->npin && sn->plen == s->pin_bytes;
    return sn->gen == s->gen && sn->npin <= s->npin;
}
int snap_restore_ex(SnapSet *s, const Snap *sn) {
    if (!sn->valid) return 0;
    if (!s->stable) { snap_restore(s, sn); return 1; }
    if (!snap_layout_prefix(s, sn)) return 0;
    for (int i = 0; i < s->nreg; i++) memcpy((void *)s->reg[i].addr, sn->buf[i], s->reg[i].len);
    if (s->arena && sn->alen) {
        lock_arena(s);
        memcpy(s->arena->base, sn->abuf, sn->alen);
        unlock_arena(s);
    }
    restore_stable_pins(s, sn);
    return 1;
}

/* v0.20.0 fix R1: a pinned block was freed outside our control this frame: never write the pins */
void snap_restore_nopins(SnapSet *s, const Snap *sn) {
    if (!sn->valid) return;
    for (int i = 0; i < s->nreg; i++) memcpy((void *)s->reg[i].addr, sn->buf[i], s->reg[i].len);
    if (s->arena && sn->alen) {
        lock_arena(s);
        memcpy(s->arena->base, sn->abuf, sn->alen);
        unlock_arena(s);
    }
}

void snap_release(Snap *sn) {
    for (int i = 0; i < SNAP_MAXREG; i++) if (sn->buf[i]) VirtualFree(sn->buf[i], 0, MEM_RELEASE);
    if (sn->abuf) VirtualFree(sn->abuf, 0, MEM_RELEASE);
    if (sn->pbuf) VirtualFree(sn->pbuf, 0, MEM_RELEASE);
    if (sn->pdead) VirtualFree(sn->pdead, 0, MEM_RELEASE);
    memset(sn, 0, sizeof *sn);
}

/* ------------------------------------------------------------------ masked hash */
static uint64_t hash_view(SnapSet *s, const uint8_t *view, uintptr_t live_addr, size_t len, uint64_t h) {
    static uint8_t tmp[SNAP_CHUNK];
    for (size_t off = 0; off < len; off += SNAP_CHUNK) {
        size_t cl = len - off < SNAP_CHUNK ? len - off : SNAP_CHUNK;
        uintptr_t a0 = live_addr + off, a1 = a0 + cl;
        const uint8_t *src = view + off;
        int masked = 0;
        for (int m = 0; m < s->nmask; m++) {
            uintptr_t m0 = s->mask[m].addr, m1 = m0 + s->mask[m].len;
            if (m1 <= a0 || m0 >= a1) continue;
            if (!masked) { memcpy(tmp, src, cl); masked = 1; }
            uintptr_t z0 = m0 > a0 ? m0 : a0, z1 = m1 < a1 ? m1 : a1;
            memset(tmp + (z0 - a0), 0, z1 - z0);
        }
        h = hash_bytes(masked ? tmp : src, cl, h);
    }
    return h;
}

uint64_t snap_hash_live(SnapSet *s) {
    uint64_t h = 0x5C6;
    for (int i = 0; i < s->nreg; i++) h = hash_view(s, (const uint8_t *)s->reg[i].addr, s->reg[i].addr, s->reg[i].len, h);
    if (s->arena) {
        lock_arena(s);
        h = hash_view(s, s->arena->base, (uintptr_t)s->arena->base, arena_used(s->arena), h ^ 0xA);
        unlock_arena(s);
    }
    if (s->npin) {
        uint8_t *o = s->pin_scratch;
        for (int i = 0; i < s->npin; i++) {
            if (s->stable && s->pstate[i] != SPIN_ALIVE) memset(o, 0, s->pins[i].len); else memcpy(o, (const void *)s->pins[i].addr, s->pins[i].len);
            o += s->pins[i].len;
        }
        h = hash_bytes(s->pin_scratch, s->pin_bytes, h ^ 0xB);
    }
    return h;
}

uint64_t snap_hash_snap(SnapSet *s, const Snap *sn) {
    uint64_t h = 0x5C6;
    for (int i = 0; i < s->nreg; i++) h = hash_view(s, sn->buf[i], s->reg[i].addr, s->reg[i].len, h);
    if (s->arena) h = hash_view(s, sn->abuf, (uintptr_t)s->arena->base, sn->alen, h ^ 0xA);
    if (s->npin) h = hash_bytes(sn->pbuf, sn->plen, h ^ 0xB);
    return h;
}

/* ------------------------------------------------------------------ masked diff */
typedef struct { snap_diff_cb cb; void *user; int max, n; size_t bytes; int region; uintptr_t base;
                 int open; size_t r0, r1; } DiffState;

static void diff_emit(DiffState *d) {
    if (!d->open) return;
    d->open = 0;
    if (d->n < d->max && d->cb) d->cb(d->user, d->region, d->r0, d->r1 - d->r0, d->base + d->r0);
    d->n++;
}

static int is_masked(SnapSet *s, uintptr_t a) {
    for (int m = 0; m < s->nmask; m++)
        if (a >= s->mask[m].addr && a < s->mask[m].addr + s->mask[m].len) return 1;
    return 0;
}

static void diff_view(SnapSet *s, DiffState *d, const uint8_t *a, const uint8_t *b, size_t len) {
    for (size_t off = 0; off < len; off += SNAP_CHUNK) {
        size_t cl = len - off < SNAP_CHUNK ? len - off : SNAP_CHUNK;
        if (!memcmp(a + off, b + off, cl)) continue;
        for (size_t i = off; i < off + cl; i++) {
            if (a[i] == b[i] || is_masked(s, d->base + i)) continue;
            d->bytes++;
            if (d->open && i <= d->r1 + 16) { d->r1 = i + 1; continue; }
            diff_emit(d);
            d->open = 1; d->r0 = i; d->r1 = i + 1;
        }
    }
    diff_emit(d);
}

size_t snap_diff_live(SnapSet *s, const Snap *sn, snap_diff_cb cb, void *user, int max_ranges, int *nranges_out) {
    DiffState d; memset(&d, 0, sizeof d);
    d.cb = cb; d.user = user; d.max = max_ranges;
    for (int i = 0; i < s->nreg; i++) {
        d.region = i; d.base = s->reg[i].addr;
        diff_view(s, &d, (const uint8_t *)s->reg[i].addr, sn->buf[i], s->reg[i].len);
    }
    if (s->arena) {
        lock_arena(s);
        size_t used = arena_used(s->arena);
        size_t n = used < sn->alen ? used : sn->alen;
        d.region = -1; d.base = (uintptr_t)s->arena->base;
        diff_view(s, &d, s->arena->base, sn->abuf, n);
        if (used != sn->alen) {
            size_t lo = n, hi = used > sn->alen ? used : sn->alen;
            if (d.n < d.max && cb) cb(user, -1, lo, hi - lo, d.base + lo);
            d.n++; d.bytes += hi - lo;
        }
        unlock_arena(s);
    }
    if (s->stable && s->npin && sn->npin && snap_layout_prefix(s, sn)) {
        /* v0.23.0-C1b: the prefix slots; a slot dead now or when taken shows no difference (its block is never read) */
        uint8_t *o = s->pin_scratch; const uint8_t *q = sn->pbuf;
        for (int i = 0; i < sn->npin; i++) {
            if (s->pstate[i] == SPIN_ALIVE && !sn->pdead[i]) memcpy(o, (const void *)s->pins[i].addr, s->pins[i].len); else memcpy(o, q, s->pins[i].len);
            o += s->pins[i].len; q += s->pins[i].len;
        }
        d.region = -2; d.base = 0;
        diff_view(s, &d, s->pin_scratch, sn->pbuf, sn->plen);
    } else if (!s->stable && s->npin && sn->plen == s->pin_bytes) {
        uint8_t *o = s->pin_scratch;
        for (int i = 0; i < s->npin; i++) { memcpy(o, (const void *)s->pins[i].addr, s->pins[i].len); o += s->pins[i].len; }
        /* report pins by blob offset; the caller maps blob offsets back to pins */
        d.region = -2; d.base = 0;
        diff_view(s, &d, s->pin_scratch, sn->pbuf, s->pin_bytes);
    }
    if (nranges_out) *nranges_out = d.n;
    return d.bytes;
}

/* masked hash of one sub-range of a view (live memory or a snapshot buffer) */
uint64_t snap_hash_range(SnapSet *s, const uint8_t *view, uintptr_t live_addr, size_t len, uint64_t seed) {
    return hash_view(s, view, live_addr, len, seed);
}

/* ------------------------------------------------------------------ v0.23.0-C1b stable layout */
static int grow_slots(SnapSet *s, int need) {
    if (need <= s->pincap && need <= s->pstcap) return 1;
    int nc = need < 1024 ? 1024 : need + need / 2;
    if (nc > s->pincap) { SPin *np = (SPin *)realloc(s->pins, sizeof(SPin) * (size_t)nc); if (!np) return 0; s->pins = np; s->pincap = nc; }
    if (nc > s->pstcap) {
        uint8_t *a = (uint8_t *)realloc(s->pstate, (size_t)nc); if (!a) return 0; s->pstate = a;
        uint8_t *b = (uint8_t *)realloc(s->phidden, (size_t)nc); if (!b) return 0; s->phidden = b;
        int32_t *c = (int32_t *)realloc(s->pdq, sizeof(int32_t) * (size_t)nc); if (!c) return 0; s->pdq = c;
        s->pstcap = nc;
    }
    return 1;
}
typedef struct { SPin p; int slot; } VisPin;
static int vis_cmp(const void *a, const void *b) {
    const VisPin *x = (const VisPin *)a, *y = (const VisPin *)b;
    int c = pin_cmp(&x->p, &y->p);
    return c ? c : (x->slot < y->slot ? -1 : x->slot > y->slot);
}
void snapset_stable_reset(SnapSet *s, uint32_t gen) {
    s->stable = 1; s->npin = 0; s->pin_bytes = 0; s->nspin = 0; s->gen = gen;
}
void snapset_rebuild_visible(SnapSet *s) {
    if (!s->stable) return;
    if (s->npin > s->spcap) {
        int nc = s->npin + s->npin / 2 + 64;
        SPin *a = (SPin *)realloc(s->spins, sizeof(SPin) * (size_t)nc); if (!a) { s->nspin = 0; return; } s->spins = a;
        int *b = (int *)realloc(s->sslot, sizeof(int) * (size_t)nc); if (!b) { s->nspin = 0; return; } s->sslot = b;
        s->spcap = nc;
    }
    int n = 0;
    for (int i = 0; i < s->npin; i++) if (s->pstate[i] == SPIN_ALIVE && !(s->phidden[i] & 1)) n++;
    VisPin *v = (VisPin *)malloc(sizeof(VisPin) * (size_t)(n ? n : 1));
    if (!v) { s->nspin = 0; return; }
    n = 0;
    for (int i = 0; i < s->npin; i++) if (s->pstate[i] == SPIN_ALIVE && !(s->phidden[i] & 1)) { v[n].p = s->pins[i]; v[n].slot = i; n++; }
    qsort(v, (size_t)n, sizeof(VisPin), vis_cmp);   /* by (addr, longer first) like pin_cmp, then slot */
    for (int i = 0; i < n; i++) { s->spins[i] = v[i].p; s->sslot[i] = v[i].slot; }
    free(v);
    s->nspin = n;
}
int snapset_lookup(const SnapSet *s, uintptr_t v) {
    const SPin *p = s->stable ? s->spins : s->pins; int n = s->stable ? s->nspin : s->npin;
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (p[m].addr <= v) lo = m + 1; else hi = m - 1; }
    for (int k = hi; k >= 0 && k > hi - 16; k--) if (v >= p[k].addr && v < p[k].addr + p[k].len) return s->stable ? s->sslot[k] : k;
    return -1;
}
int snapset_find_start(const SnapSet *s, uintptr_t a) {
    const SPin *p = s->stable ? s->spins : s->pins; int n = s->stable ? s->nspin : s->npin;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (p[m].addr == a) { while (m > 0 && p[m - 1].addr == a) m--; return s->stable ? s->sslot[m] : m; }
        if (p[m].addr < a) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
void snapset_pin_kill(SnapSet *s, int slot, int state, int32_t dq) {
    if (!s->stable || slot < 0 || slot >= s->npin || s->pstate[slot] != SPIN_ALIVE) return;
    s->pstate[slot] = (uint8_t)state; s->pdq[slot] = dq; s->phidden[slot] = 1;
    snapset_rebuild_visible(s);
}
int snapset_kill_range(SnapSet *s, uintptr_t lo, uintptr_t hi, int state, int32_t dq) {
    if (!s->stable) return 0;
    int n = 0;
    for (int i = 0; i < s->npin; i++) {
        if (s->pstate[i] != SPIN_ALIVE) continue;
        uintptr_t a = s->pins[i].addr, e = a + s->pins[i].len;
        if (a == lo || (a >= lo && e <= hi)) { s->pstate[i] = (uint8_t)state; s->pdq[i] = dq; s->phidden[i] = 1; n++; }
    }
    if (n) snapset_rebuild_visible(s);
    return n;
}
int snapset_find_alive_start(const SnapSet *s, uintptr_t a) {
    if (!s->stable) return -1;
    for (int i = 0; i < s->npin; i++) if (s->pstate[i] == SPIN_ALIVE && s->pins[i].addr == a) return i;
    return -1;
}
void snapset_pin_hide(SnapSet *s, int slot) {
    if (!s->stable || slot < 0 || slot >= s->npin || (s->phidden[slot] & 1)) return;
    s->phidden[slot] |= 1;
    snapset_rebuild_visible(s);
}
int snapset_pin_flags(SnapSet *s, int slot, uint8_t flags) {
    if (!s->stable || slot < 0 || slot >= s->npin || s->phidden[slot] == flags) return 0;
    s->phidden[slot] = flags;
    return 1;
}
int snapset_merge_pins(SnapSet *s, const SPin *pins, int n, int *appended, int *vanished) {
    *appended = 0; *vanished = 0;
    SPin *c = (SPin *)malloc(sizeof(SPin) * (size_t)(n ? n : 1));
    uint8_t *used = (uint8_t *)calloc((size_t)(n ? n : 1), 1);
    if (!c || !used) { free(c); free(used); return 0; }
    if (n) memcpy(c, pins, sizeof(SPin) * (size_t)n);
    qsort(c, (size_t)n, sizeof(SPin), pin_cmp);
    int w = 0, dropped = 0; size_t dbytes = 0; uintptr_t cov_end = 0; int have = 0;   /* the snapset_set_pins dedupe */
    for (int i = 0; i < n; i++) {
        SPin p = c[i]; uintptr_t end = p.addr + p.len;
        if (have && end <= cov_end) { dropped++; dbytes += p.len; continue; }
        c[w++] = p;
        if (!have || end > cov_end) { cov_end = end; have = 1; }
    }
    n = w; s->pins_dropped = dropped; s->pins_dropped_bytes = dbytes;
    {   /* v0.23.1 (review F3/F5): a candidate that starts at an alive HIDDEN slot (quarantined, or a deferred free hidden by rb.c) or lies
         * wholly inside one is that slot: it is never appended a second time. Before, the rescan appended a second ALIVE slot for a
         * quarantined block; the release killed only the first, and every later snapshot read and every restore wrote the freed block. */
        int nh = 0;
        for (int i = 0; i < s->npin; i++) if (s->pstate[i] == SPIN_ALIVE && s->phidden[i]) nh++;
        if (nh) {
            SPin *hs = (SPin *)malloc(sizeof(SPin) * (size_t)nh);
            if (!hs) { free(c); free(used); return 0; }
            nh = 0;
            for (int i = 0; i < s->npin; i++) if (s->pstate[i] == SPIN_ALIVE && s->phidden[i]) hs[nh++] = s->pins[i];
            qsort(hs, (size_t)nh, sizeof(SPin), pin_cmp);
            for (int k = 0; k < n; k++) {
                int lo = 0, hi = nh - 1;   /* hs[hi] = the last hidden slot starting at or before the candidate */
                while (lo <= hi) { int m = (lo + hi) / 2; if (hs[m].addr <= c[k].addr) lo = m + 1; else hi = m - 1; }
                for (int j = hi; j >= 0 && j > hi - 16; j--)   /* the snapset_lookup window */
                    if (c[k].addr == hs[j].addr || c[k].addr + c[k].len <= hs[j].addr + hs[j].len) { used[k] = 1; break; }
            }
            free(hs);
        }
    }
    for (int i = 0; i < s->npin; i++) {
        if (s->pstate[i] != SPIN_ALIVE || s->phidden[i]) continue;   /* hidden or quarantined (bit 1) + alive: stays until its release */
        int lo = 0, hi = n - 1, found = -1;
        while (lo <= hi) { int m = (lo + hi) / 2; if (c[m].addr < s->pins[i].addr) lo = m + 1; else hi = m - 1; }
        for (int k = lo; k < n && c[k].addr == s->pins[i].addr; k++) if (c[k].len == s->pins[i].len && !used[k]) { found = k; break; }
        if (found >= 0) used[found] = 1;
        else { s->pstate[i] = SPIN_VANISHED; s->phidden[i] = 1; s->pdq[i] = 0; (*vanished)++; }
    }
    int add = 0; for (int k = 0; k < n; k++) add += !used[k];
    if (!grow_slots(s, s->npin + add)) { free(c); free(used); return 0; }
    size_t nb = s->pin_bytes;
    for (int k = 0; k < n; k++) if (!used[k]) nb += c[k].len;
    if (nb > s->pin_scratch_cap) {
        size_t cap = nb + (nb >> 2) + 4096;
        uint8_t *ns = (uint8_t *)malloc(cap);
        if (!ns) { free(c); free(used); return 0; }
        free(s->pin_scratch); s->pin_scratch = ns; s->pin_scratch_cap = cap;
    }
    for (int k = 0; k < n; k++) {
        if (used[k]) continue;
        int i = s->npin++;
        s->pins[i] = c[k]; s->pstate[i] = SPIN_ALIVE; s->phidden[i] = 0; s->pdq[i] = 0;
        s->pin_bytes += c[k].len; (*appended)++;
    }
    free(c); free(used);
    snapset_rebuild_visible(s);
    return 1;
}
