/* v0.22.1-C1a setup injection: pure helpers (see c1a.h). No game state, no log: shared by the plugin (c1a_run.c), the unit test and
 * the offline check (test/c1a_offline.c, ./build.sh offline), so the offline proof on the real dumps runs this exact code.
 * Sources: C1A-AUDIT.md §1-§2 (D1-D4, VERIFIED there), FUN_1402fa9f0 / FUN_140364bc0 / FUN_140394fe0 decompiles
 * (gate-a/dumps/dec_c1aS_ai*.c, re-read for this build), the wind manager FUN_140333fd0 / FUN_140334750 / FUN_140334430
 * (dec_c1aS_s1.c, s2.c, s4.c). */
#include "c1a.h"
#include "snap.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const C1aRange k_c1a_range[C1A_NRANGE] = {
    { 0x14470f1d0ULL, 0x14470f310ULL, "rules", C1A_S_RULES },   /* SetMatchRule (+0x48) output, the rules hash range rounded to 8 */
    { 0x144843f20ULL, 0x144843fd0ULL, "copy",  C1A_S_RULES },   /* Setup's clamped per-player rule copy 0x144843ef0 +0x30..+0xdf */
    { 0x14470f390ULL, 0x144715400ULL, "ai",    C1A_S_AI },      /* both CPU AI blocks */
};

uint32_t c1a_inject_parse(const char *s, int *bad) {
    if (bad) *bad = 0;
    if (!s || !*s || !_stricmp(s, "default")) return C1A_INJECT_DEFAULT;
    if (!_stricmp(s, "none")) return 0;
    uint32_t m = 0;
    char b[256]; strncpy(b, s, sizeof b - 1); b[sizeof b - 1] = 0;
    for (char *t = strtok(b, ", "); t; t = strtok(NULL, ", ")) {
        if (!_stricmp(t, "rules")) m |= 1u << C1A_S_RULES;
        else if (!_stricmp(t, "ai")) m |= 1u << C1A_S_AI;
        else if (bad) *bad = 1;
    }
    return m;
}

/* ------------------------------------------------------------------ dumps */
int c1a_dump_parse(const uint8_t *buf, size_t n, C1aDump *d, const char **why) {
    const char *w = NULL;
    memset(d, 0, sizeof *d);
    if (!buf || n < 64) { w = "short"; goto bad; }
    if (memcmp(buf, "C1ADUMP1", 8)) { w = "magic"; goto bad; }
    memcpy(&d->version, buf + 8, 4); memcpy(&d->frame, buf + 12, 4); memcpy(&d->battle, buf + 16, 4); memcpy(&d->nsect, buf + 20, 4);
    memcpy(&d->module_base, buf + 24, 8); memcpy(&d->arena_base, buf + 32, 8); memcpy(&d->flags, buf + 40, 8);
    if (d->version != 1) { w = "version"; goto bad; }
    if (d->nsect < 1 || d->nsect > 8) { w = "nsect"; goto bad; }
    size_t o = 64;
    for (int i = 0; i < d->nsect; i++) {
        C1aDumpSec *s = &d->s[i];
        if (o + 32 > n) { w = "truncated_section_header"; goto bad; }
        memcpy(&s->va, buf + o, 8); memcpy(&s->len, buf + o + 8, 4); memcpy(s->name, buf + o + 16, 16); s->name[16] = 0; o += 32;
        size_t need = (size_t)s->len + (s->len + 3) / 4 + (s->len + 7) / 8;
        if (s->len == 0 || need > n - o) { w = "truncated_section"; goto bad; }
        s->data = buf + o; o += s->len; s->cls = buf + o; o += (s->len + 3) / 4; s->kind = buf + o; o += (s->len + 7) / 8;
    }
    if (o != n) { w = "trailing_bytes"; goto bad; }
    if (why) *why = "ok";
    return 1;
bad:
    if (why) *why = w;
    return 0;
}

int c1a_dump_view(const C1aDump *d, uint64_t va, size_t len, C1aView *v) {
    memset(v, 0, sizeof *v);
    for (int i = 0; i < d->nsect; i++) {
        const C1aDumpSec *s = &d->s[i];
        if (va < s->va || va + len > s->va + s->len || ((va - s->va) & 7)) continue;
        v->lo = va; v->len = len; v->b = s->data + (va - s->va); v->k = s->kind + (va - s->va) / 8;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ the plan */
static int view_u32(const C1aView *v, uint64_t va, int32_t *out) {   /* a RAW unit's 4 bytes */
    if (!v->b || va < v->lo || va + 4 > v->lo + v->len) return 0;
    size_t off = (size_t)(va - v->lo);
    if (v->k[off / 8] != C1A_K_RAW) return 0;
    memcpy(out, v->b + off, 4);
    return 1;
}
static void pers_read(const C1aView v[C1A_NRANGE], int side, int32_t out[3], int *ok) {
    int a = view_u32(&v[C1A_R_RULES], C1A_INJ_RULES + (uint64_t)side * C1A_INJ_RULESTR + C1A_INJ_PERS, &out[0]);
    int b = view_u32(&v[C1A_R_COPY], C1A_INJ_GAME + C1A_INJ_PCOPY + (uint64_t)side * C1A_INJ_RULESTR, &out[1]);
    int c = view_u32(&v[C1A_R_AI], C1A_INJ_AI + (uint64_t)side * C1A_INJ_AISTR + C1A_INJ_PAI, &out[2]);
    if (!a) out[0] = -0x7fffffff; if (!b) out[1] = -0x7fffffff; if (!c) out[2] = -0x7fffffff;
    *ok = a && b && c;
}
/* v0.22.2: the CPU difficulty values of one side: [rule rank, rule waza, copy rank, copy waza, ai rank, ai waza, ai +0x1a28] */
static void lvl_read(const C1aView v[C1A_NRANGE], int side, int32_t out[7], int *ok) {
    uint64_t r = C1A_INJ_RULES + (uint64_t)side * C1A_INJ_RULESTR, c = C1A_INJ_GAME + C1A_INJ_PCOPY + (uint64_t)side * C1A_INJ_RULESTR;
    uint64_t a = C1A_INJ_AI + (uint64_t)side * C1A_INJ_AISTR;
    const uint64_t va[7] = { r + C1A_INJ_RRANK, r + C1A_INJ_RWAZA, c - 8, c - 4, a + C1A_INJ_ARANK, a + C1A_INJ_AWAZA, a + C1A_INJ_ALVL };
    const int rg[7] = { C1A_R_RULES, C1A_R_RULES, C1A_R_COPY, C1A_R_COPY, C1A_R_AI, C1A_R_AI, C1A_R_AI };
    *ok = 1;
    for (int i = 0; i < 7; i++) if (!view_u32(&v[rg[i]], va[i], &out[i])) { out[i] = -0x7fffffff; *ok = 0; }
}
static int clamp12(int32_t x) { return x < 0 ? 0 : x > 12 ? 12 : x; }
/* rank: rule = copy = AI +0x2bf8; waza: rule = copy, AI +0x2c00 = AI +0x1a28 = clamp(rule, 0, 12) (seen: 02/02/02 and 03/03/03/03) */
static int lvl_consistent(const int32_t l[7]) { return l[0] == l[2] && l[0] == l[4] && l[1] == l[3] && l[5] == clamp12(l[1]) && l[6] == clamp12(l[1]); }
/* FUN_1402fa9f0: copy = (int)rule < 100 ? rule : 99; FUN_1402d4af0 stores the setter's value at +0x2c0c (the value UE also puts in the rule) */
static int pers_consistent(const int32_t p[3]) { return p[2] == p[0] && p[1] == (p[0] < 100 ? p[0] : 99); }
static int in_allowed(uint64_t va, const int pd[2], const int ld[2]) {
    for (int s = 0; s < 2; s++) {
        if (!pd[s] && !ld[s]) continue;
        uint64_t r = C1A_INJ_RULES + (uint64_t)s * C1A_INJ_RULESTR + C1A_INJ_PERS, c = C1A_INJ_GAME + C1A_INJ_PCOPY + (uint64_t)s * C1A_INJ_RULESTR;
        uint64_t a = C1A_INJ_AI + (uint64_t)s * C1A_INJ_AISTR;
        if (pd[s] && ((va >= r && va < r + 4) || (va >= c && va < c + 4) || (va >= a + C1A_INJ_PAI && va < a + C1A_INJ_PAI + 4) ||
            (va >= a + C1A_INJ_TAB_LO && va < a + C1A_INJ_TAB_HI))) return 1;
        /* v0.22.2: the CPU difficulty fields (rule block +0x8..+0xf, the copy 8 bytes before the personality, AI +0x2bf8/+0x2c00/+0x1a28) */
        uint64_t rb = C1A_INJ_RULES + (uint64_t)s * C1A_INJ_RULESTR;
        if (ld[s] && ((va >= rb + C1A_INJ_RRANK && va < rb + C1A_INJ_RWAZA + 4) || (va >= c - 8 && va < c) ||
            (va >= a + C1A_INJ_ARANK && va < a + C1A_INJ_ARANK + 4) || (va >= a + C1A_INJ_AWAZA && va < a + C1A_INJ_AWAZA + 4) ||
            (va >= a + C1A_INJ_ALVL && va < a + C1A_INJ_ALVL + 4))) return 1;
    }
    return 0;
}
/* one byte: -1 equal (or both units per-launch heap pointers), 0 = write (allowed), 1 = bad, 2 = bad pointer unit */
static int classify(const C1aView *rv, const C1aView *nv, size_t off, int cat_ok, const int pd[2], const int ld[2]) {
    uint8_t kr = rv->k[off / 8], kn = nv->k[off / 8];
    if (kr == C1A_K_HEAP && kn == C1A_K_HEAP) return -1;   /* the setup hash drops them (C1A_TAG_HEAP); never written */
    if (rv->b[off] == nv->b[off]) return -1;
    if (kr != C1A_K_RAW || kn != C1A_K_RAW) return 2;
    return cat_ok && in_allowed(rv->lo + off, pd, ld) ? 0 : 1;
}

void c1a_inj_plan(const C1aView rec[C1A_NRANGE], const C1aView now[C1A_NRANGE], uint32_t inject, C1aPlan *p) {
    memset(p, 0, sizeof *p);
    int okr[2], okn[2], lokr[2], lokn[2];
    for (int s = 0; s < 2; s++) { pers_read(rec, s, p->pers_rec[s], &okr[s]); pers_read(now, s, p->pers_now[s], &okn[s]); }
    for (int s = 0; s < 2; s++) { lvl_read(rec, s, p->lvl_rec[s], &lokr[s]); lvl_read(now, s, p->lvl_now[s], &lokn[s]); }
    p->cons_rec = p->cons_now = p->lvl_cons_rec = p->lvl_cons_now = 1;
    for (int s = 0; s < 2; s++) {
        /* v0.22.2: the personality OR the difficulty (rule rank/waza) differs; each group's copies must agree only when that group
         * differs (an equal group writes nothing: its bytes must simply be equal) */
        int pd = p->pers_rec[s][0] != p->pers_now[s][0], ld = p->lvl_rec[s][0] != p->lvl_now[s][0] || p->lvl_rec[s][1] != p->lvl_now[s][1];
        p->side_ovr[s] = pd || ld; p->side_pd[s] = pd; p->side_ld[s] = ld;
        if (!p->side_ovr[s]) continue;   /* a side whose CPU setup is equal: its bytes must simply be equal */
        if (pd && (!okr[s] || !pers_consistent(p->pers_rec[s]))) p->cons_rec = 0;
        if (pd && (!okn[s] || !pers_consistent(p->pers_now[s]))) p->cons_now = 0;
        if (ld && (!lokr[s] || !lvl_consistent(p->lvl_rec[s]))) p->lvl_cons_rec = 0;
        if (ld && (!lokn[s] || !lvl_consistent(p->lvl_now[s]))) p->lvl_cons_now = 0;
    }
    for (int r = 0; r < C1A_NRANGE; r++) {
        const C1aView *rv = &rec[r], *nv = &now[r];
        if (!rv->b || !nv->b || rv->len != nv->len || rv->lo != nv->lo) { p->bad_bytes++; p->verdict = C1A_INJ_REFUSE; p->why = "range_missing"; return; }
        int cat_ok = (inject >> k_c1a_range[r].cat) & 1;
        for (size_t off = 0; off < rv->len; off++) {
            int c = classify(rv, nv, off, cat_ok, p->side_pd, p->side_ld);
            if (c < 0) continue;
            p->diff_bytes++;
            uint8_t bad = c != 0, ptr = c == 2;
            if (c == 0) p->wr_bytes[r]++; else { p->bad_bytes++; if (ptr) p->ptr_bytes++; }
            uint64_t va = rv->lo + off;
            C1aSpan *L = p->nspan ? &p->span[p->nspan - 1] : NULL;
            if (L && L->range == r && L->bad == bad && L->ptr == ptr && L->va + L->len == va) { L->len++; continue; }
            if (c == 0) p->wr_spans[r]++; else p->bad_spans++;
            if (p->nspan < C1A_INJ_MAXSPAN) { C1aSpan *S = &p->span[p->nspan++]; S->va = va; S->len = 1; S->range = (uint8_t)r; S->bad = bad; S->ptr = ptr; }
        }
    }
    uint32_t wr = p->wr_bytes[0] + p->wr_bytes[1] + p->wr_bytes[2];
    if (wr && (!p->cons_rec || !p->cons_now)) { p->verdict = C1A_INJ_REFUSE; p->why = "personality_copies_disagree"; }
    else if (wr && (!p->lvl_cons_rec || !p->lvl_cons_now)) { p->verdict = C1A_INJ_REFUSE; p->why = "cpu_level_copies_disagree"; }
    else if (p->bad_bytes) { p->verdict = C1A_INJ_REFUSE; p->why = p->bad_bytes > p->ptr_bytes ? "not_cpu_setup" : "pointer_differs"; }
    else if (!wr) { p->verdict = C1A_INJ_EQUAL; p->why = "equal"; }
    else { p->verdict = C1A_INJ_OK; p->why = "cpu_setup"; }
}

uint32_t c1a_inj_apply(const C1aView rec[C1A_NRANGE], const C1aView now[C1A_NRANGE], const C1aPlan *p, uint8_t *const dst[C1A_NRANGE]) {
    if (p->verdict != C1A_INJ_OK) return 0;
    uint32_t n = 0;
    for (int r = 0; r < C1A_NRANGE; r++) {
        const C1aView *rv = &rec[r], *nv = &now[r];
        int cat_ok = 1;   /* the plan was OK: every written byte passed the category check */
        for (size_t off = 0; off < rv->len; off++)
            if (classify(rv, nv, off, cat_ok, p->side_pd, p->side_ld) == 0) { dst[r][off] = rv->b[off]; n++; }
    }
    return n;
}

/* ------------------------------------------------------------------ the setup hashes from views */
static uint64_t hash_units(const C1aView *v, size_t off, size_t len, uint64_t h, int *exact) {
    /* c1a_run.c nh() with heap_mode=1 on [lo+off, lo+off+len): whole 8-byte units normalized (the dump already holds the
     * arena/pin/image/region/stack tags; a heap unit becomes C1A_TAG_HEAP); a partial unit is raw there (exact only if RAW here) */
    uint8_t buf[0x200];
    if (len > sizeof buf || off + len > v->len || (off & 7)) { *exact = 0; return h; }
    memcpy(buf, v->b + off, len);
    for (size_t u = 0; u * 8 < len; u++) {
        uint8_t k = v->k[off / 8 + u];
        if ((u + 1) * 8 <= len) { if (k == C1A_K_HEAP) { uint64_t t = C1A_TAG_HEAP; memcpy(buf + u * 8, &t, 8); } }
        else if (k != C1A_K_RAW) *exact = 0;
    }
    return hash_bytes(buf, len, h);
}
uint64_t c1a_hash_rules_view(const C1aView *r, int *exact) {
    *exact = r->b && r->lo == C1A_INJ_RULES && r->len >= 0x140;
    if (!*exact) return 0;
    return hash_units(r, 0, 0x13c, 0x5e3, exact);
}
uint64_t c1a_hash_ai_view(const C1aView *ai, int *exact) {
    *exact = ai->b && ai->lo == C1A_INJ_AI && ai->len >= 2 * C1A_INJ_AISTR;
    if (!*exact) return 0;
    uint64_t h = 0x5e8;
    for (int p = 0; p < 2; p++) {
        size_t a = (size_t)p * C1A_INJ_AISTR;
        h = hash_units(ai, a + 0x1a28, 4, h, exact);
        h = hash_units(ai, a + 0x2bf8, 0x118, h, exact);
    }
    return h;
}
uint64_t c1a_hash_va_view(const C1aView *v, uint64_t va, size_t len, uint64_t h, int raw, int *exact) {
    uint8_t buf[0x200];
    if (!v->b || len > sizeof buf || va < v->lo || va + len > v->lo + v->len) { *exact = 0; return h; }
    size_t off = (size_t)(va - v->lo);
    memcpy(buf, v->b + off, len);
    for (uint64_t u = va & ~7ULL; u < va + len; u += 8) {   /* every 8-byte unit (VA grid) the range touches */
        uint8_t k = v->k[(u - v->lo) / 8];
        int whole = u >= va && u + 8 <= va + len;
        if (raw || !whole) { if (k != C1A_K_RAW) *exact = 0; continue; }
        if (k == C1A_K_HEAP) { uint64_t t = C1A_TAG_HEAP; memcpy(buf + (u - va), &t, 8); }
    }
    return hash_bytes(buf, len, h);
}
/* v0.24.0-C1b2 F1 (offline t1map_rec): the tier-1 ai / ctl component of a dump view under the map rule (0) or the heaptag rule (1) */
uint64_t c1a_t1_view_hash(const C1aView *v, const uint32_t *offs, int noffs, int rule, uint64_t seed, int *exact) {
    *exact = v->b != NULL && !(v->lo & 7);
    if (!*exact || v->len > 0x8000) { *exact = 0; return 0; }
    static uint8_t buf[0x8000];
    memcpy(buf, v->b, v->len);
    for (size_t u = 0; u * 8 + 8 <= v->len; u++) {
        uint8_t k = v->k[u]; uint64_t val; memcpy(&val, buf + u * 8, 8);
        if (rule == 1) { if (k == C1A_K_HEAP) { uint64_t t = C1A_TAG_HEAP; memcpy(buf + u * 8, &t, 8); } continue; }
        int mapped = c1a_t1_mapped(offs, noffs, (uint32_t)(u * 8));
        if (!mapped) { if (k != C1A_K_RAW && k != C1A_K_HEAP) *exact = 0; continue; }   /* unmapped: the raw value (kind 6 keeps it) */
        if (!val) continue;
        if (k == C1A_K_RAW || k == C1A_K_HEAP) { uint64_t t = C1A_TAG_HEAP; memcpy(buf + u * 8, &t, 8); }   /* kinds 1-5: the dump's tag */
    }
    return hash_bytes(buf, v->len, seed);
}
uint64_t c1a_plugin_hash(const char *version) { return hash_bytes(version, strlen(version), 0x5e1); }
/* C1B3: (C1B-R3-SPEC.md §2.1.2) a legacy recording's reference: the tree sizes PLAYER0/1+0x455d8 from its f0 dump (sizes only) */
int c1a_content_sizes_from_dump(const C1aDump *d, uint64_t n[2]) {
    for (int p = 0; p < 2; p++) {
        C1aView v;
        if (!c1a_dump_view(d, 0x1447156f0ULL + (uint64_t)p * 0x973f0ULL + C1A_CONTENT_SIZE, 8, &v) || v.k[0] != C1A_K_RAW) return 0;
        memcpy(&n[p], v.b, 8);
    }
    return 1;
}
/* recorded builds this build replays: their step path (straight live pass, L3, L4, routing, the normalization, tier-1/2) is unchanged;
 * v0.22.1 changes only the replay setup (injection, the demo seed, the wind category, the CRT counters, the +0xc8 log) */
static const char *const k_compat[] = { "0.22.0-gateC-C1a" };
const char *c1a_plugin_compat(uint64_t rec_plugin_hash) {
    for (size_t i = 0; i < sizeof k_compat / sizeof k_compat[0]; i++) if (c1a_plugin_hash(k_compat[i]) == rec_plugin_hash) return k_compat[i];
    return NULL;
}

/* ------------------------------------------------------------------ the wind manager */
void c1a_wind_view(const C1aView *v, C1aWind *w) {
    memset(w, 0, sizeof *w);
    if (!v || !v->b || v->len < C1A_WIND_LEN) return;
    const uint8_t *b = v->b;
    w->ok = 1;
    memcpy(&w->head, b, 8); w->head_kind = v->k[0];
    memcpy(&w->idx, b + 0x98, 4); memcpy(&w->cnt, b + 0x9c, 4); memcpy(&w->a0, b + 0xa0, 4); memcpy(&w->a4, b + 0xa4, 4);
    w->nq = w->cnt < 0 ? 0 : w->cnt > 8 ? 8 : w->cnt;
    for (int k = 0; k < w->nq; k++) memcpy(&w->q[k], b + 0x18 + ((size_t)(w->idx & 1) * 8 + (size_t)k) * 8, 8);
    memcpy(w->prm, b + 0xb0, 16); memcpy(w->outv, b + 0xc0, 48);
}
int c1a_wind_equal(const C1aWind *a, const C1aWind *b, char *why, size_t cap) {
    size_t o = 0; int eq = 1;
    why[0] = 0;
#define WHY(...) do { int k_ = snprintf(why + o, cap - o, "%s", o ? "," : ""); if (k_ > 0 && (size_t)k_ < cap - o) o += (size_t)k_; \
                      k_ = snprintf(why + o, cap - o, __VA_ARGS__); if (k_ > 0 && (size_t)k_ < cap - o) o += (size_t)k_; } while (0)
    if (!a->ok || !b->ok) { WHY("absent"); return a->ok == b->ok; }
    if (a->cnt != b->cnt) { eq = 0; WHY("cnt"); }
    if (a->a0 != b->a0) { eq = 0; WHY("a0"); }
    if (a->a4 != b->a4) { eq = 0; WHY("a4"); }
    if (memcmp(a->prm, b->prm, 16)) { eq = 0; WHY("spawn_params"); }
    if ((a->head != 0) != (b->head != 0)) { eq = 0; WHY("list_empty"); }
    for (int k = 0; k < a->nq && k < b->nq; k++) if (a->q[k] != b->q[k]) { eq = 0; WHY("queued_%d", k); }
    if (a->idx != b->idx) WHY("slot_index_only");
    if (memcmp(a->outv, b->outv, 48)) WHY("out_vectors_only");
#undef WHY
    if (!o) snprintf(why, cap, "identical");
    return eq;
}
size_t c1a_wind_str(const C1aWind *w, char *out, size_t cap) {
    if (!w->ok) return (size_t)snprintf(out, cap, "absent");
    char q[200]; size_t o = 0; q[0] = 0;
    for (int k = 0; k < w->nq; k++) {
        uint64_t v = w->q[k];
        int n = (v >> 60) == 0xD ? snprintf(q + o, sizeof q - o, "%s%llx", k ? "+" : "", (unsigned long long)(v & 0x0fffffffffffffffULL))
                                 : snprintf(q + o, sizeof q - o, "%sraw%llx", k ? "+" : "", (unsigned long long)v);
        if (n > 0 && (size_t)n < sizeof q - o) o += (size_t)n;
    }
    uint32_t p0, p1; memcpy(&p0, w->prm, 4); memcpy(&p1, w->prm + 4, 4);
    return (size_t)snprintf(out, cap, "idx:%u,cnt:%d,a0:%d,a4:%u,head:%s,q:%s,prm:%08x%08x", w->idx, w->cnt, (int32_t)w->a0, w->a4,
                            !w->head ? "null" : w->head_kind == C1A_K_PIN ? "pin" : w->head_kind == C1A_K_HEAP ? "heap" : "other", o ? q : "-", p0, p1);
}
