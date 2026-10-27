/* v0.22.0-C1a pure helpers (see c1a.h). */
#include "c1a.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const char *const k_c1a_comp[C1A_NCOMP] = {
    "fc", "tt", "mrg", "mt", "sd", "md", "cin", "lcg", "ctl", "ai", "rcv",
    "pos0", "side0", "inp0", "st0", "gau0", "mv0", "aif0",
    "pos1", "side1", "inp1", "st1", "gau1", "mv1", "aif1",
};
const char *const k_c1a_setup[C1A_NSETUP] = {
    "exe", "plugin", "simcfg", "rules", "flags", "pl0", "pl1", "ctl", "ai", "stage", "assets", "catch", "uecam", "wind",
    "content",   /* C1B3: (C1B-R3-SPEC.md §2.1.1) */
};

/* ------------------------------------------------------------------ normalization */
static int pin_of(const C1aNorm *n, uintptr_t v) {
    int lo = 0, hi = n->npin - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (n->pins[m].addr <= v) lo = m + 1; else hi = m - 1; }
    /* hi = the last pin starting at or before v; partial overlaps are kept by the snapset, so look back a little */
    for (int k = hi; k >= 0 && k > hi - 16; k--) if (v >= n->pins[k].addr && v < n->pins[k].addr + n->pins[k].len) return k;
    return -1;
}

uint64_t c1a_norm_qword(const C1aNorm *n, uint64_t v, int *kind) {
    int k = C1A_K_RAW;
    uint64_t r = v;
    if (v < 0x10000 || v >= 0x800000000000ULL) goto done;   /* small values, negative numbers, kernel space: never a pointer */
    if (n->arena_hi > n->arena_lo && v >= n->arena_lo && v < n->arena_hi) { k = C1A_K_ARENA; r = C1A_TAG_ARENA | ((v - n->arena_lo) & 0x0fffffffffffffffULL); goto done; }
    if (n->npin) {
        int p = pin_of(n, (uintptr_t)v);
        if (p >= 0) { k = C1A_K_PIN; r = C1A_TAG_PIN | ((uint64_t)(n->pins[p].len & 0x0fffffffu) << 32) | ((v - n->pins[p].addr) & 0xffffffffULL); goto done; }
    }
    if (n->img_hi > n->img_lo && v >= n->img_lo && v < n->img_hi) { k = C1A_K_IMAGE; r = C1A_TAG_IMAGE | (v - n->img_lo); goto done; }
    for (int i = 0; i < n->nreg; i++)
        if (v >= n->regs[i].lo && v - n->regs[i].lo < n->regs[i].len) {
            k = C1A_K_REGION; r = C1A_TAG_REGION | ((uint64_t)(n->regs[i].idx & 0xff) << 40) | ((v - n->regs[i].lo) & 0xffffffffffULL); goto done;
        }
    if (n->stk_hi > n->stk_lo && v >= n->stk_lo && v < n->stk_hi) { k = C1A_K_STACK; r = C1A_TAG_STACK; goto done; }
    if (n->readable && n->readable((uintptr_t)v)) { k = C1A_K_HEAP; if (n->heap_mode) r = C1A_TAG_HEAP; }
done:
    if (kind) *kind = k;
    return r;
}

size_t c1a_norm_buf(const C1aNorm *n, uint8_t *buf, uintptr_t va, size_t len, uint8_t *kinds) {
    size_t changed = 0;
    if (kinds) memset(kinds, C1A_K_RAW, (len + 7) / 8);
    size_t o = (size_t)((8 - (va & 7)) & 7);   /* first 8-aligned VA inside the buffer */
    for (; o + 8 <= len; o += 8) {
        uint64_t v; memcpy(&v, buf + o, 8);
        if (v < 0x10000 || v >= 0x800000000000ULL) continue;   /* fast path */
        int k; uint64_t r = c1a_norm_qword(n, v, &k);
        if (kinds && !(va & 7)) kinds[o / 8] = (uint8_t)k;
        if (r != v) { memcpy(buf + o, &r, 8); changed++; }
    }
    return changed;
}

/* ------------------------------------------------------------------ v0.24.0-C1b2 F1: the tier-1 pointer map (C1B-R2-SPEC.md §2.2) */
/* the AI block's genuine pointer units (div/ptrmap.py census over 110 dumps of 5 launches, VERIFIED there): per side image +0x08 +0x10
 * +0x28 +0xba8 +0xbb0 +0xbc8 +0xcc8, heap +0x30 +0xbb8 +0xbc0 +0xbd0 +0xbd8 +0xbe0 +0xcd8 +0xce0 +0x1998; side 1 at +0x3038 */
#define AIS(o) (o), (o) + 0x3038u
const uint32_t k_t1_ai_ptr[C1A_T1_NAI] = {
    0x08, 0x10, 0x28, 0x30, 0xba8, 0xbb0, 0xbb8, 0xbc0, 0xbc8, 0xbd0, 0xbd8, 0xbe0, 0xcc8, 0xcd8, 0xce0, 0x1998,
    0x08 + 0x3038, 0x10 + 0x3038, 0x28 + 0x3038, 0x30 + 0x3038, 0xba8 + 0x3038, 0xbb0 + 0x3038, 0xbb8 + 0x3038, 0xbc0 + 0x3038,
    0xbc8 + 0x3038, 0xbd0 + 0x3038, 0xbd8 + 0x3038, 0xbe0 + 0x3038, 0xcc8 + 0x3038, 0xcd8 + 0x3038, 0xce0 + 0x3038, 0x1998 + 0x3038,
};
#undef AIS
/* the controller block: image +0x00 +0x10 +0x60 +0x70, pin +0x50 +0xb0 */
const uint32_t k_t1_ctl_ptr[C1A_T1_NCTL] = { 0x00, 0x10, 0x50, 0x60, 0x70, 0xb0 };

uint64_t c1a_t1map_hash(void) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int l = 0; l < 2; l++) {
        const uint32_t *o = l ? k_t1_ctl_ptr : k_t1_ai_ptr; int n = l ? C1A_T1_NCTL : C1A_T1_NAI;
        for (int i = 0; i < n; i++) for (int b = 0; b < 4; b++) { h ^= (uint8_t)(o[i] >> (8 * b)); h *= 0x100000001b3ULL; }
    }
    return h;
}
int c1a_t1_mapped(const uint32_t *offs, int noffs, uint32_t off) {
    int lo = 0, hi = noffs - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (offs[m] == off) return 1; if (offs[m] < off) lo = m + 1; else hi = m - 1; }
    return 0;
}
size_t c1a_norm_buf_map(const C1aNorm *n, uint8_t *buf, uintptr_t va, size_t len, const uint32_t *offs, int noffs, uint8_t *kinds) {
    size_t changed = 0;
    if (kinds) memset(kinds, C1A_K_RAW, (len + 7) / 8);
    C1aNorm nr = *n; nr.readable = NULL; nr.heap_mode = 0;   /* the map rule never asks readable() */
    size_t o = (size_t)((8 - (va & 7)) & 7);
    for (; o + 8 <= len; o += 8) {
        if (!c1a_t1_mapped(offs, noffs, (uint32_t)o)) continue;   /* unmapped: raw, kind 0, whatever it holds */
        uint64_t v; memcpy(&v, buf + o, 8);
        if (!v) continue;                                          /* mapped 0: raw 0 (as the v0.23 rule) */
        int k; uint64_t r = c1a_norm_qword(&nr, v, &k);
        if (k == C1A_K_RAW) { k = C1A_K_HEAP; r = C1A_TAG_HEAP; }  /* any other mapped nonzero value: a heap tag */
        if (kinds && !(va & 7)) kinds[o / 8] = (uint8_t)k;
        if (r != v) { memcpy(buf + o, &r, 8); changed++; }
    }
    return changed;
}

/* ------------------------------------------------------------------ C1B3: (C1B-R3-SPEC.md §2.6) the tier-2 heap map */
const uint32_t k_t2map[] = {
#include "t2map.inc"
};
const int k_t2map_n = (int)(sizeof k_t2map / sizeof k_t2map[0]);
#ifdef SC6RB_HARNESS
/* the harness's own heap units in class G (its fake core keeps real-heap pointers there; the census of the real game cannot know them):
 * PLAYER0+0x21d8 (A7, a raw CRT pointer) and the units found by the PH2X / RHXM runs (C1B-R3-SPEC.md §2.6.3) */
static const uint32_t k_t2map_h[] = { (uint32_t)(0x1447156f0ULL + 0x21d8 - C1A_T2MAP_BASE) };
#endif
/* C2 (C2-SPEC.md §5.2): t2map.inc is map2 (the census + the G1447a unit, test/gen_t2map.py EXTRA_MAP2). map1 = map2 minus these units: a run
 * replaying a t2rule=map1 recording computes tier-2 with map1 (c1a_run.c rf_load sets the version), so it stays comparable */
static const uint32_t k_t2map_extra2[] = { (uint32_t)(0x1447ac880ULL - C1A_T2MAP_BASE) };   /* PLAYER0+0x97190 */
int g_c1a_t2map_ver = 2;
static int is_extra2(uint32_t off) { for (size_t i = 0; i < sizeof k_t2map_extra2 / sizeof k_t2map_extra2[0]; i++) if (k_t2map_extra2[i] == off) return 1; return 0; }
int c1a_t2map_units(void) { return g_c1a_t2map_ver == 1 ? k_t2map_n - (int)(sizeof k_t2map_extra2 / sizeof k_t2map_extra2[0]) : k_t2map_n; }
int c1a_t2rule_parse(const char *hdr) {
    char tr[16] = "";
    if (!c1a_kv(hdr, "t2rule", tr, sizeof tr) || !strcmp(tr, "raw")) return 0;
    return !strcmp(tr, "heaptag") ? 1 : (!strcmp(tr, "map1") || !strcmp(tr, "map2")) ? 2 : -1;   /* C2: + map2 */
}
int c1a_t2rule_ver(const char *hdr) { char tr[16] = ""; if (!c1a_kv(hdr, "t2rule", tr, sizeof tr)) return 0; return !strcmp(tr, "map1") ? 1 : !strcmp(tr, "map2") ? 2 : 0; }
uint64_t c1a_t2map_hash(void) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < k_t2map_n; i++) { if (g_c1a_t2map_ver == 1 && is_extra2(k_t2map[i])) continue; for (int b = 0; b < 4; b++) { h ^= (uint8_t)(k_t2map[i] >> (8 * b)); h *= 0x100000001b3ULL; } }
    return h;
}
static int t2map_bs(const uint32_t *a, int n, uint32_t off) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a[m] == off) return 1; if (a[m] < off) lo = m + 1; else hi = m - 1; }
    return 0;
}
int c1a_t2map_has(uint64_t pref_va) {
    if (pref_va < C1A_T2MAP_BASE || pref_va - C1A_T2MAP_BASE >= 0x1000000ULL) return 0;
    uint32_t off = (uint32_t)(pref_va - C1A_T2MAP_BASE);
    if (t2map_bs(k_t2map, k_t2map_n, off)) return g_c1a_t2map_ver != 1 || !is_extra2(off);   /* C2: map1 = map2 minus the extras */
#ifdef SC6RB_HARNESS
    for (size_t i = 0; i < sizeof k_t2map_h / sizeof k_t2map_h[0]; i++) if (k_t2map_h[i] == off) return 1;
#endif
    return 0;
}
size_t c1a_norm_buf_t2map(const C1aNorm *n, uint8_t *buf, uintptr_t va, uint64_t pref, size_t len, uint8_t *kinds) {
    size_t changed = 0;
    if (kinds) memset(kinds, C1A_K_RAW, (len + 7) / 8);
    C1aNorm nr = *n; nr.readable = NULL; nr.heap_mode = 0;   /* the map rule never asks readable() */
    size_t o = (size_t)((8 - (va & 7)) & 7);
    for (; o + 8 <= len; o += 8) {
        uint64_t v; memcpy(&v, buf + o, 8);
        if (!v) continue;
        int k = C1A_K_RAW; uint64_t r = v;
        if (v >= 0x10000 && v < 0x800000000000ULL) r = c1a_norm_qword(&nr, v, &k);
        /* a mapped unit: a nonzero value in the user-mode pointer range is a heap tag. C1B3 fix (round-3 review): a value outside
         * [0x10000, 0x800000000000) is never a heap address and stays raw, so a union unit holding small gameplay values
         * (PLAYER0/1+0x43da8 / +0x44070: 0x5a, 0x78, 0x96 ...) still shows a divergence in tier-2 */
        if (k == C1A_K_RAW && v >= 0x10000 && v < 0x800000000000ULL && c1a_t2map_has(pref + o)) { k = C1A_K_HEAP; r = C1A_TAG_HEAP; }
        if (kinds && !(va & 7)) kinds[o / 8] = (uint8_t)k;
        if (r != v) { memcpy(buf + o, &r, 8); changed++; }
    }
    return changed;
}

/* ------------------------------------------------------------------ line format / parse */
static void hexn(char *o, const uint8_t *p, size_t n) { static const char d[] = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { o[2 * i] = d[p[i] >> 4]; o[2 * i + 1] = d[p[i] & 15]; } o[2 * n] = 0; }
static int unhex(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

size_t c1a_format_frame(char *out, size_t cap, const C1aFrame *fr) {
    char xs[0x18 * 2 + 1]; hexn(xs, fr->in.x, 0x18);
    int o = snprintf(out, cap, "F %u fr=%u p1=%016llx p2=%016llx x=%s ff=%02x rp=%u pt=%u t1=%016llx", fr->f, fr->fr,
                     (unsigned long long)fr->in.p1, (unsigned long long)fr->in.p2, xs, fr->in.ff, fr->rp, fr->pt, (unsigned long long)fr->t1);
    if (o < 0 || (size_t)o >= cap) return 0;
    for (int i = 0; i < C1A_NCOMP; i++) {
        int k = snprintf(out + o, cap - (size_t)o, " %s=%016llx", k_c1a_comp[i], (unsigned long long)fr->c[i]);
        if (k < 0 || (size_t)(o + k) >= cap) return 0;
        o += k;
    }
    int k = snprintf(out + o, cap - (size_t)o, " ifc=%s q=%u\n", fr->ifc[0] ? fr->ifc : "-", fr->q);
    if (k < 0 || (size_t)(o + k) >= cap) return 0;
    return (size_t)(o + k);
}

int c1a_kv(const char *line, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == line || p[-1] == ' ') && p[kl] == '=') {
            p += kl + 1;
            size_t n = 0;
            while (p[n] && p[n] != ' ' && p[n] != '\n' && p[n] != '\r') n++;
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n); out[n] = 0;
            return 1;
        }
        p += kl;
    }
    return 0;
}

uint64_t c1a_kv_u64(const char *line, const char *key, int *ok) {
    char b[64];
    if (!c1a_kv(line, key, b, sizeof b) || !b[0]) { if (ok) *ok = 0; return 0; }
    char *e; unsigned long long v = strtoull(b, &e, 0);
    if (ok) *ok = *e == 0;
    return (uint64_t)v;
}

static int kv_hex64(const char *line, const char *key, uint64_t *v) {
    char b[40];
    if (!c1a_kv(line, key, b, sizeof b) || strlen(b) != 16) return 0;
    uint64_t r = 0;
    for (int i = 0; i < 16; i++) { int d = unhex(b[i]); if (d < 0) return 0; r = r << 4 | (uint64_t)d; }
    *v = r;
    return 1;
}

int c1a_parse_frame(const char *line, C1aFrame *fr) {
    if (line[0] != 'F' || line[1] != ' ') return 0;
    memset(fr, 0, sizeof *fr);
    char *e; unsigned long f = strtoul(line + 2, &e, 10);
    if (e == line + 2 || *e != ' ') return 0;
    fr->f = (uint32_t)f;
    int ok = 1; uint64_t v;
    v = c1a_kv_u64(line, "fr", &ok); if (!ok) return 0; fr->fr = (uint32_t)v;
    if (!kv_hex64(line, "p1", &fr->in.p1) || !kv_hex64(line, "p2", &fr->in.p2)) return 0;
    char xs[64];
    if (!c1a_kv(line, "x", xs, sizeof xs) || strlen(xs) != 0x18 * 2) return 0;
    for (int i = 0; i < 0x18; i++) { int a = unhex(xs[2 * i]), b = unhex(xs[2 * i + 1]); if (a < 0 || b < 0) return 0; fr->in.x[i] = (uint8_t)(a << 4 | b); }
    char ffs[8]; if (!c1a_kv(line, "ff", ffs, sizeof ffs) || strlen(ffs) != 2 || unhex(ffs[0]) < 0 || unhex(ffs[1]) < 0) return 0;
    fr->in.ff = (uint8_t)(unhex(ffs[0]) << 4 | unhex(ffs[1]));
    v = c1a_kv_u64(line, "rp", &ok); if (!ok) return 0; fr->rp = (uint8_t)v;
    v = c1a_kv_u64(line, "pt", &ok); if (!ok) return 0; fr->pt = (uint8_t)v;
    if (!kv_hex64(line, "t1", &fr->t1)) return 0;
    for (int i = 0; i < C1A_NCOMP; i++) if (!kv_hex64(line, k_c1a_comp[i], &fr->c[i])) return 0;
    if (!c1a_kv(line, "ifc", fr->ifc, sizeof fr->ifc)) return 0;
    v = c1a_kv_u64(line, "q", &ok); if (!ok) return 0; fr->q = (uint32_t)v;
    return 1;
}

void c1a_cell_name(uint32_t key, char out[16]) { snprintf(out, 16, "%c%x", (char)(key >> 24), key & 0xffffffu); }

size_t c1a_format_t2(char *out, size_t cap, uint32_t f, const C1aCell *c, int n) {
    int o = snprintf(out, cap, "T2 %u n=%d", f, n);
    if (o < 0 || (size_t)o >= cap) return 0;
    for (int i = 0; i < n; i++) {
        char nm[16]; c1a_cell_name(c[i].key, nm);
        int k = snprintf(out + o, cap - (size_t)o, " %s=%016llx", nm, (unsigned long long)c[i].h);
        if (k < 0 || (size_t)(o + k) >= cap) return 0;
        o += k;
    }
    if ((size_t)o + 2 > cap) return 0;
    out[o++] = '\n'; out[o] = 0;
    return (size_t)o;
}

/* v0.32.0 (OPTA-M2-SPEC.md §21.6, D-B) */
const C1aAbSpan k_c1a_ab[C1A_AB_NSPAN] = { { 0x16d8, 0x28, 0 }, { 0x1968, 0x1d0, 0x28 }, { 0x4412a, 8, 0x1f8 } };
size_t c1a_format_ab(char *out, size_t cap, uint32_t f, const uint8_t cur[2][C1A_AB_LEN], const uint8_t prev[2][C1A_AB_LEN]) {
    static const char hx[] = "0123456789abcdef";
    int o = snprintf(out, cap, "A %u", f), items = 0;
    if (o < 0 || (size_t)o >= cap) return 0;
    for (int p = 0; p < 2; p++)
        for (int s = 0; s < C1A_AB_NSPAN; s++) {
            const C1aAbSpan *sp = &k_c1a_ab[s];
            uint32_t nu = sp->len / 8;
            for (uint32_t u = 0; u < nu; ) {
                const uint8_t *a = cur[p] + sp->at + u * 8;
                if (prev && !memcmp(a, prev[p] + sp->at + u * 8, 8)) { u++; continue; }
                uint32_t v = u + 1;
                while (v < nu && (!prev || memcmp(cur[p] + sp->at + v * 8, prev[p] + sp->at + v * 8, 8))) v++;
                int k = snprintf(out + o, cap - (size_t)o, " p%d+%x=", p, sp->off + u * 8);
                if (k < 0 || (size_t)(o + k) + (size_t)(v - u) * 16 + 2 >= cap) return 0;
                o += k;
                for (uint32_t b = u * 8; b < v * 8; b++) { out[o++] = hx[cur[p][sp->at + b] >> 4]; out[o++] = hx[cur[p][sp->at + b] & 15]; }
                items++;
                u = v;
            }
        }
    if (!items) return 0;
    out[o++] = '\n'; out[o] = 0;
    return (size_t)o;
}

int c1a_parse_t2(const char *line, uint32_t *f, C1aCell *c, int max, int *n) {
    if (line[0] != 'T' || line[1] != '2' || line[2] != ' ') return 0;
    char *e; unsigned long fv = strtoul(line + 3, &e, 10);
    if (e == line + 3) return 0;
    *f = (uint32_t)fv;
    int ok = 1; uint64_t want = c1a_kv_u64(line, "n", &ok); if (!ok) return 0;
    const char *p = strstr(e, " n=");
    if (!p) return 0;
    p += 3; while (*p && *p != ' ' && *p != '\n') p++;
    int k = 0;
    while (*p == ' ') {
        p++;
        char cls = *p;
        if (cls < 'A' || cls > 'Z') return 0;
        unsigned long g = strtoul(p + 1, &e, 16);
        if (e == p + 1 || *e != '=' || g > 0xffffffUL) return 0;
        p = e + 1;
        uint64_t h = 0; int nd = 0;
        while (unhex(*p) >= 0) { h = h << 4 | (uint64_t)unhex(*p); p++; nd++; }
        if (nd != 16) return 0;
        if (k < max) { c[k].key = (uint32_t)cls << 24 | (uint32_t)g; c[k].h = h; }
        k++;
    }
    if ((uint64_t)k != want || k > max) return 0;
    *n = k;
    return 1;
}

/* ------------------------------------------------------------------ refusal set */
uint32_t c1a_refuse_parse(const char *s, int *bad) {
    if (bad) *bad = 0;
    if (!s || !*s || !_stricmp(s, "default")) return C1A_REFUSE_DEFAULT;
    if (!_stricmp(s, "none")) return 0;
    uint32_t m = 0;
    char b[256]; strncpy(b, s, sizeof b - 1); b[sizeof b - 1] = 0;
    for (char *t = strtok(b, ", "); t; t = strtok(NULL, ", ")) {
        int f = 0;
        for (int i = 0; i < C1A_NSETUP; i++) if (!_stricmp(t, k_c1a_setup[i])) { m |= 1u << i; f = 1; }
        if (!f && bad) *bad = 1;
    }
    return m;
}
size_t c1a_refuse_str(uint32_t mask, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < C1A_NSETUP; i++) if (mask & (1u << i)) { int k = snprintf(out + o, cap - o, "%s%s", o ? "," : "", k_c1a_setup[i]); if (k > 0 && (size_t)k < cap - o) o += (size_t)k; }
    if (!o) { snprintf(out, cap, "none"); o = 4; }
    return o;
}

/* v0.23.0-C1b (C1B-SPEC.md X13, §2.9): the C1a session recording was made by 0.22.2. This build keeps its step path, normalization and
 * tier-1 checksums for mode=replay / rbreplay; the C1b changes (tier-2 heap tag, the mt tail region, the rb keys) are outside simcfg and
 * named in the header (t2rule, mt_tail, rbcfg). The v0.22.1 list (c1a_setup.c) is kept unchanged and asked first. */
const char *c1a_plugin_compat2(uint64_t rec_plugin_hash) {
    const char *c = c1a_plugin_compat(rec_plugin_hash);
    if (c) return c;
    /* v0.32.0 (OPTA-M2-SPEC.md §21.7): + 0.31.0-optA-M2 (the S-M2c recording c1a_20261026_011013_b2.rec replays on v0.32.0: the C1a / rb step path, the
     * normalization and tier-1 / tier-2 are unchanged; v0.32.0's changes are rbonline's CHECK message and the D-B A lines, which a replay only adds) */
    static const char *const k_compat_c1b[] = { "0.22.2-gateC-C1a", "0.23.0-gateC-C1b", "0.24.0-gateC-C1b2", "0.25.0-gateC-C1b3", "0.26.0-gateC-C1b4", "0.31.0-optA-M2" };   /* C2: + the C1b4 build (v0.26 recordings replay) */   /* C1B4: + the round-3 build (L2's live recording) */   /* v0.24.0-C1b2: + the session-1 build; C1B3: + the round-2 build (both live attempts) */
    for (size_t i = 0; i < sizeof k_compat_c1b / sizeof k_compat_c1b[0]; i++) if (c1a_plugin_hash(k_compat_c1b[i]) == rec_plugin_hash) return k_compat_c1b[i];
    return NULL;
}
