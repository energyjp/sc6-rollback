/* v0.21.0-S1: P/X tables and pure helpers (see pres.h). */
#include "pres.h"
#include <string.h>

/* P (presentation), PLAYER-relative, sorted. audit/direction/P-fields.md §2 "Mask":
 *   bank rows of the face bones 60..94 plus the unnamed face helpers 95..97 in all three bone-matrix arrays (step-computed from the face rig);
 *   +0x29120..+0x2b3c8 face object, face weights and the 7 face layers (P-fields §1.3: the voice-family DIFF lines
 *     +0x2af18..+0x2b17c; +0x295d0 face controller; +0x2b270 lip data pointer, also X);
 *   +0x95ec0..+0x95f60 lip-sync controller (FUN_1402f7670(P+0x95ec0, ...), CHECK.md §1);
 *   +0x97268..+0x973e8 voice controller (slot +0x038 writes +0x972a0).
 * NOT P (a conservative choice): the sway pool +0x45700..+0x95700, bank rows >= 95 and anything weapon-related
 * (Ivy's whip-sword sway nodes may carry hit capsules). */
/* v0.24.0-C1b2 F2 (C1B-R2-SPEC.md §2.3, Y3): "face" is split around the per-player wind vector PLAYER+0x29310..+0x2931f. Step s writes it
 * (FUN_140333fd0) and step s+1's sway solve reads it (FUN_14034b8d0 -> FUN_1403310a0): a field step s writes and step s+1 reads must never be
 * P (pres_keep would put back the predicted lineage's value after a rollback). It is G now (still hash class F in tier-2, modes.c class_of_va,
 * so no tier-2 hash changes). 7 ranges, 16,696 bytes per player (14 ranges, 33,392 bytes for both). */
/* C1B3: (C1B-R3-SPEC.md §2.4.1) KEEP. The world-space face rows and face_a / face_c / face_d left P (they are POSE: rolled back like G).
 * 4 ranges, 1,936 bytes per player (8 ranges, 3,872 bytes for both). */
#define PRES_KEEP_INIT \
    { 0x295d0, 0x29670, "face_w" },        /* 160 B: UE's face controller write window */ \
    { 0x2aef8, 0x2b3c8, "face_layers" },   /* 1,232 B: 7 layers x 0xb0, incl. the lip data pointer +0x2b270 (also X) */ \
    { 0x95ec0, 0x95f60, "lipsync_ctl" },   /* 160 B */ \
    { 0x97268, 0x973e8, "voice_ctl" }      /* 384 B */
/* the v0.24 P table (7 ranges, shared 0x3610 base), byte for byte: the frozen hash class and the harness control h_pres_old=2 */
#define PRES_V024_INIT \
    { PRES_ROW_OFF(0, PRES_FACE_ROW_LO), PRES_ROW_OFF(0, PRES_FACE_ROW_HI + 1), "bank0_face_rows" },   /* 0x4510..0x4e90 */ \
    { PRES_ROW_OFF(1, PRES_FACE_ROW_LO), PRES_ROW_OFF(1, PRES_FACE_ROW_HI + 1), "bank1_face_rows" },   /* 0x10510..0x10e90 */ \
    { PRES_ROW_OFF(2, PRES_FACE_ROW_LO), PRES_ROW_OFF(2, PRES_FACE_ROW_HI + 1), "bank2_face_rows" },   /* 0x1c510..0x1ce90 */ \
    { 0x29120, 0x29310, "face_a" }, \
    { 0x29320, 0x2b3c8, "face_b" }, \
    { 0x95ec0, 0x95f60, "lipsync_ctl" }, \
    { 0x97268, 0x973e8, "voice_ctl" }
const PresRange k_pres_hashfx_v024[] = { PRES_V024_INIT };
const int k_pres_nhashfx = (int)(sizeof k_pres_hashfx_v024 / sizeof k_pres_hashfx_v024[0]);
#ifdef SC6RB_HARNESS
PresRange k_pres_p[7] = { PRES_KEEP_INIT };
int k_pres_np = 4;
static const PresRange k_pres_keep_c[] = { PRES_KEEP_INIT };
/* harness-only control h_pres_old: 0 KEEP; 1 the v0.23 table (face_b from 0x29310: the wind vector in P, case PKCX); 2 the v0.24 table (the
 * world-space face rows 60..97 at 0x3610 for both players and face_a / face_b whole in P: the round-2 face-stretch build, cases PFSX / PR60X) */
void pres_test_table(int which) {
    if (which == 1 || which == 2) {
        for (int i = 0; i < k_pres_nhashfx; i++) k_pres_p[i] = k_pres_hashfx_v024[i];
        if (which == 1) k_pres_p[4].lo = 0x29310;
        k_pres_np = k_pres_nhashfx;
    } else {
        for (int i = 0; i < 4; i++) k_pres_p[i] = k_pres_keep_c[i];
        k_pres_np = 4;
    }
}
/* C1B3: the v0.24 entry point (config.c calls it right after pres_test_table, unchanged, so the lip-sync patch context stays byte-equal):
 * the table was already selected by pres_test_table(h_pres_old); nothing to do */
void pres_test_old_face(int on) { (void)on; }
#else
const PresRange k_pres_p[] = { PRES_KEEP_INIT };
const int k_pres_np = (int)(sizeof k_pres_p / sizeof k_pres_p[0]);
#endif

/* C1B3: POSE (C1B-R3-SPEC.md §2.4.1), per player */
uint32_t pres_bank_off(int c) {
    uint64_t pl = PRES_PLAYER0 + (uint64_t)c * PRES_STRIDE;
    return (uint32_t)(((pl + 0x35a0 + 0xbf) & ~(uint64_t)0x7f) - pl);
}
int pres_pose_ranges(int c, PresRange out[PRES_NPOSE]) {
    static const char *const bn[3] = { "bank0_face_rows", "bank1_face_rows", "bank2_face_rows" };
    uint32_t base = pres_bank_off(c);
    for (int b = 0; b < 3; b++) {
        out[b].lo = base + (uint32_t)b * PRES_BANK_STRIDE + PRES_POSE_ROW_LO * PRES_ROW;
        out[b].hi = base + (uint32_t)b * PRES_BANK_STRIDE + (PRES_POSE_ROW_HI + 1) * PRES_ROW;
        out[b].name = bn[b];
    }
    out[3].lo = 0x29120; out[3].hi = 0x29310; out[3].name = "face_a";
    out[4].lo = 0x29320; out[4].hi = 0x295d0; out[4].name = "face_c";
    out[5].lo = 0x29670; out[5].hi = 0x2aef8; out[5].name = "face_d";   /* the core spans 0x29690..0x296af, the prev-pose snapshot 0x296b0, the gates +0x2aef0/+0x2aef4 */
    return PRES_NPOSE;
}
size_t pres_pose_bytes(int c) { PresRange r[PRES_NPOSE]; int n = pres_pose_ranges(c, r); size_t b = 0; for (int i = 0; i < n; i++) b += r[i].hi - r[i].lo; return b; }
int pres_pose_index(int c, uint64_t off) {
    PresRange r[PRES_NPOSE]; int n = pres_pose_ranges(c, r);
    for (int i = 0; i < n; i++) if (off >= r[i].lo && off < r[i].hi) return i;
    return -1;
}
int pres_hashfx_index(uint64_t off) {
    for (int i = 0; i < k_pres_nhashfx; i++) if (off >= k_pres_hashfx_v024[i].lo && off < k_pres_hashfx_v024[i].hi) return i;
    return -1;
}

/* X (per-PC pointers), P-fields.md §4 and the launch C log (sc6rb_20261004_185348_28544.log):
 *   +0x3590 (8): the std::map head pointer. MSVC std::map = { _Myhead*, _Mysize }; every logged difference was the
 *     5 low bytes of the qword at +0x3590 (now=200143000a ref=e0fdbc6f0b); +0x3598 (_Mysize, a count) never differed
 *     and stays G-core;
 *   +0x2b270 (8): the lip data pointer (now=2080f3650a02 ref=0). */
const PresRange k_pres_x[] = {
    { 0x3590, 0x3598, "std_map_head" },
    { 0x2b270, 0x2b278, "lip_data_ptr" },
};
const int k_pres_nx = (int)(sizeof k_pres_x / sizeof k_pres_x[0]);

int pres_p_index(uint64_t off) {
    for (int i = 0; i < k_pres_np; i++) if (off >= k_pres_p[i].lo && off < k_pres_p[i].hi) return i;
    return -1;
}
int pres_is_x(uint64_t off) {
    for (int i = 0; i < k_pres_nx; i++) if (off >= k_pres_x[i].lo && off < k_pres_x[i].hi) return 1;
    return 0;
}
size_t pres_p_bytes(void) {
    size_t n = 0;
    for (int i = 0; i < k_pres_np; i++) n += k_pres_p[i].hi - k_pres_p[i].lo;
    return n;
}

int pres_va_kind(uint64_t va, uint64_t end, uint64_t *run_end) {
    uint64_t lim = end;
    int kind = PRES_K_OTHER;
    if (va >= PRES_FX_LO && va < PRES_FX_HI) { kind = PRES_K_FX; if (PRES_FX_HI < lim) lim = PRES_FX_HI; }
    else if (va < PRES_FX_LO && PRES_FX_LO < lim) lim = PRES_FX_LO;
    if (kind == PRES_K_OTHER && va >= PRES_PLAYER0 && va < PRES_PLAYER0 + 2 * PRES_STRIDE) {
        uint64_t c = (va - PRES_PLAYER0) / PRES_STRIDE, pb = PRES_PLAYER0 + c * PRES_STRIDE, off = va - pb;
        int i = pres_p_index(off);
        if (i >= 0) { kind = PRES_K_P; if (pb + k_pres_p[i].hi < lim) lim = pb + k_pres_p[i].hi; }
        else {
            for (int j = 0; j < k_pres_np; j++) if (k_pres_p[j].lo > off) { if (pb + k_pres_p[j].lo < lim) lim = pb + k_pres_p[j].lo; break; }
            if (pb + PRES_STRIDE < lim) lim = pb + PRES_STRIDE;   /* the next player's first range */
        }
    } else if (kind == PRES_K_OTHER && va < PRES_PLAYER0 && PRES_PLAYER0 < lim) lim = PRES_PLAYER0;
    *run_end = lim > va ? lim : end;
    return kind;
}

char pres_keep_apply(const PresBlk *b, int n, const uint8_t *live, pres_is_ptr_cb is_ptr, void *pu, pres_adopt_cb adopt, void *au, PresKeep *out) {
    PresKeep r = { 0, 0, 0, 0 };
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *L = live + o;
        uint8_t *M = (uint8_t *)b[i].addr;
        o += b[i].len;
        if (!memcmp(L, M, b[i].len)) continue;
        r.differ++;
        int why = 0; size_t woff = 0; uint64_t wl = 0, wr = 0;
        for (uintptr_t a = (b[i].addr + 7) & ~(uintptr_t)7; is_ptr && a + 8 <= b[i].addr + b[i].len; a += 8) {
            uint64_t lv, rv; memcpy(&lv, L + (a - b[i].addr), 8); memcpy(&rv, M + (a - b[i].addr), 8);
            if (lv == rv) continue;
            int pl = is_ptr(pu, lv), pr = is_ptr(pu, rv);
            int k = pl == 1 || pr == 1 ? 1 : pl == 2 || pr == 2 ? 2 : 0;
            if (k) { why = k; woff = a - b[i].addr; wl = lv; wr = rv; break; }
        }
        if (why) { r.adopted++; if (adopt) adopt(au, i, woff, wl, wr, why); continue; }
        for (uint32_t k = 0; k < b[i].len; k++) r.bytes += L[k] != M[k];
        memcpy(M, L, b[i].len);
        r.kept++;
    }
    if (out) *out = r;
    return r.adopted ? 'A' : r.kept ? 'K' : '-';
}

int pres_ptr_filter(int kind, int rule) {
    if (kind == 2) return 2;                                  /* arena: keeps the resim bytes under both rules */
    if (kind == 1) return rule == PRES_RULE_IMAGE_ARENA ? 1 : 0;   /* image: only under the v0.21.0 rule */
    return 0;
}
static int pres_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}
int pres_rule_parse(const char *s) {
    if (!s) return -1;
    if (pres_ieq(s, "arena")) return PRES_RULE_ARENA;
    if (pres_ieq(s, "image_arena")) return PRES_RULE_IMAGE_ARENA;
    return -1;
}
const char *pres_rule_name(int rule) { return rule == PRES_RULE_IMAGE_ARENA ? "image_arena" : "arena"; }
