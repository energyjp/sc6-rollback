/* C2 (C2-SPEC.md §3.2): the one-pad battle entry. See entry.h. */
#include "entry.h"
#include "c1a.h"
#include "c1a_run.h"
#include "netsess.h"
#include "modes.h"
#include "rb.h"
#include <stdlib.h>

#define PL0   0x1447156f0ULL
#define PSTR  0x973f0ULL
#define CTLB  0x144715400ULL
#define AI1   (0x14470f390ULL + 0x3038ULL)

static int g_conv, g_ctl_bad, g_restored;
static char g_tok[16] = "-";
static uint32_t g_r1_was, g_c1_was;

void entry_battle_reset(void) { g_conv = 0; g_ctl_bad = 0; g_restored = 0; snprintf(g_tok, sizeof g_tok, "-"); }
const char *entry_token(void) { return g_tok; }
void entry_set_token(const char *t) { snprintf(g_tok, sizeof g_tok, "%s", t); g_title_dirty = 1; }
int entry_converted(void) { return g_conv; }

static uint32_t rd32(uint64_t va) { return *(volatile uint32_t *)VA(va); }
static void wr32(uint64_t va, uint32_t v) { *(volatile uint32_t *)VA(va) = v; }

int entry_check(void) {
    uint32_t r0 = rd32(ENT_RULE0), c0 = rd32(ENT_COPY0), r1 = rd32(ENT_RULE1), c1 = rd32(ENT_COPY1);
    int ok = r0 == 1 && c0 == 1 && ((r1 == 2 && c1 == 2) || (r1 == 1 && c1 == 1));
    if (!ok) {
        LOG("ENTRY CONV REFUSE type=%u,%u,%u,%u (need side 0 = 1,1 and side 1 = 2,2 or 1,1: Player vs CPU with the host as P1; nothing written, the battle runs locally)\n", r0, c0, r1, c1);
        entry_set_token("CONVREFUSE");
    }
    return ok;
}
int entry_convert(void) {
    g_r1_was = rd32(ENT_RULE1); g_c1_was = rd32(ENT_COPY1);
#ifdef SC6RB_HARNESS
    if (g_cfg.h_c2_noconv) {   /* harness-only control (NCTL): the conversion is not written; the step-1 check must fail loudly */
        LOG("ENTRY CONV skipped (harness control h_c2_noconv=1: side 1 stays rule=%u copy=%u)\n", g_r1_was, g_c1_was);
        g_conv = 0; entry_set_token("CONV");
        return 1;
    }
#endif
    wr32(ENT_RULE1, 1); wr32(ENT_COPY1, 1);
    g_conv = 1;
    LOG("ENTRY CONV ok side=1 rule=%u->1 copy=%u->1\n", g_r1_was, g_c1_was);
    entry_set_token("CONV");
    return 1;
}
void entry_restore(int step1_ran) {
    if (!g_conv || g_restored) return;
    if (step1_ran) { LOG("ENTRY RESTORE too_late (step 1 ran: the control types are derived; nothing written)\n"); return; }
    uint32_t r1 = rd32(ENT_RULE1), c1 = rd32(ENT_COPY1);
    if (r1 == 1) wr32(ENT_RULE1, 2);
    if (c1 == 1) wr32(ENT_COPY1, 2);
    g_restored = 1; g_conv = 0;
    LOG("ENTRY RESTORE side=1 rule=%u->%u copy=%u->%u (before step 1: the battle runs locally vs CPU)\n", r1, rd32(ENT_RULE1), c1, rd32(ENT_COPY1));
}

/* the side-1 CPU-setup spans (C2-SPEC.md §3.2 step 3): rule block +0x08..+0x13, the copy +0x90..+0x9b, AI1 +0x38..+0x987, +0x1a28, +0x2bf8..+0x2c0f */
static const struct { uint64_t va; uint16_t len; } k_spans[] = {
    { 0x14470f1d0ULL + 0x58 + 0x08, 0x0c },
    { 0x144843ef0ULL + 0x90, 0x0c },
    { AI1 + 0x38, 0x950 },
    { AI1 + 0x1a28, 4 },
    { AI1 + 0x2bf8, 0x18 },
};
#define NSPAN (int)(sizeof k_spans / sizeof k_spans[0])
size_t entry_spans_put(uint8_t *out, size_t cap, uint16_t *nspans) {
    size_t o = 0;
    for (int i = 0; i < NSPAN; i++) {
        if (o + 10 + k_spans[i].len > cap) return 0;
        uint64_t va = k_spans[i].va; uint16_t len = k_spans[i].len;
        memcpy(out + o, &va, 8); memcpy(out + o + 8, &len, 2); memcpy(out + o + 10, (const void *)VA(va), len);
        o += 10 + (size_t)len;
    }
    *nspans = (uint16_t)NSPAN;
    return o;
}
int entry_spans_apply(const uint8_t *p, size_t n, uint16_t nspans, char *cat, size_t catcap) {
    static C1aView now[C1A_NRANGE], rec[C1A_NRANGE];
    static uint8_t *rb[C1A_NRANGE];
    static C1aPlan plan;
    snprintf(cat, catcap, "-");
    if (!c1a_live_views(now)) { snprintf(cat, catcap, "out_of_memory"); goto refuse; }
    for (int r = 0; r < C1A_NRANGE; r++) {   /* rec = this launch's views with the host's span bytes laid over them */
        free(rb[r]); rb[r] = (uint8_t *)malloc(now[r].len);
        if (!rb[r]) { snprintf(cat, catcap, "out_of_memory"); goto refuse; }
        memcpy(rb[r], now[r].b, now[r].len);
        rec[r] = now[r]; rec[r].b = rb[r];
    }
    size_t o = 0; uint32_t bytes = 0;
    for (int i = 0; i < nspans; i++) {
        if (o + 10 > n) { snprintf(cat, catcap, "spans_truncated"); goto refuse; }
        uint64_t va; uint16_t len; memcpy(&va, p + o, 8); memcpy(&len, p + o + 8, 2); o += 10;
        if (o + len > n) { snprintf(cat, catcap, "spans_truncated"); goto refuse; }
        int placed = 0;
        for (int r = 0; r < C1A_NRANGE && !placed; r++)
            if (va >= rec[r].lo && va + len <= rec[r].lo + rec[r].len) { memcpy(rb[r] + (va - rec[r].lo), p + o, len); placed = 1; }
        if (!placed) { snprintf(cat, catcap, "span_outside_ranges:0x%llx", (unsigned long long)va); goto refuse; }
        int known = 0; for (int k = 0; k < NSPAN; k++) if (k_spans[k].va == va && k_spans[k].len == len) known = 1;
        if (!known) { snprintf(cat, catcap, "span_not_cpu_setup:0x%llx", (unsigned long long)va); goto refuse; }
        o += len; bytes += len;
    }
    c1a_inj_plan(rec, now, (1u << C1A_S_RULES) | (1u << C1A_S_AI), &plan);
    LOG("ENTRY INJ plan verdict=%s why=%s diff_bytes=%u write_bytes=%u/%u/%u bad_bytes=%u pointer_bytes=%u personality host=%d,%d guest=%d,%d\n",
        plan.verdict == C1A_INJ_OK ? "inject" : plan.verdict == C1A_INJ_EQUAL ? "equal" : "REFUSE", plan.why, plan.diff_bytes, plan.wr_bytes[0], plan.wr_bytes[1], plan.wr_bytes[2],
        plan.bad_bytes, plan.ptr_bytes, plan.pers_rec[0][0], plan.pers_rec[1][0], plan.pers_now[0][0], plan.pers_now[1][0]);
    if (plan.verdict == C1A_INJ_REFUSE) { snprintf(cat, catcap, "%s", plan.why); goto refuse; }
    uint32_t wrote = 0;
    if (plan.verdict == C1A_INJ_OK) {
        uint8_t *dst[C1A_NRANGE]; for (int r = 0; r < C1A_NRANGE; r++) dst[r] = (uint8_t *)VA(k_c1a_range[r].lo);
        wrote = c1a_inj_apply(rec, now, &plan, dst);
    }
    {   static C1aView now2[C1A_NRANGE]; static C1aPlan p2;
        if (!c1a_live_views(now2)) { snprintf(cat, catcap, "out_of_memory"); goto refuse; }
        c1a_inj_plan(rec, now2, (1u << C1A_S_RULES) | (1u << C1A_S_AI), &p2);
        if (p2.verdict != C1A_INJ_EQUAL) { snprintf(cat, catcap, "inject_verify_bytes"); goto refuse; }
    }
    LOG("ENTRY INJ ok spans=%u bytes=%u wrote=%u verdict=%s\n", nspans, bytes, wrote, plan.verdict == C1A_INJ_OK ? "inject" : "equal");
    return 1;
refuse:
    LOG("ENTRY INJ REFUSE cat=%s (nothing usable written; the battle runs locally)\n", cat);
    entry_set_token("INJREFUSE");
    return 0;
}
static void read_types(uint32_t t[2], uint32_t c[2]) {
    for (int s = 0; s < 2; s++) { t[s] = *(volatile uint32_t *)VA(PL0 + (uint64_t)s * PSTR + ENT_T324); c[s] = *(volatile uint32_t *)VA(CTLB + (uint64_t)s * 0x60 + ENT_CTL30); }
}
int entry_step1_check(void) {
    uint32_t t[2], c[2]; read_types(t, c);

    if (t[0] == 1 && t[1] == 1 && c[0] == 1 && c[1] == 1) { LOG("ENTRY CHECK step1 ok t324=%u,%u ctl30=%u,%u\n", t[0], t[1], c[0], c[1]); return 1; }
    LOG("ENTRY CHECK FAIL t324=%u,%u ctl30=%u,%u (after step 1 a side is not human: the conversion did not reach the core)\n", t[0], t[1], c[0], c[1]);
    entry_set_token("CTLTYPE");
    g_ctl_bad = 1;
    return 0;
}
int entry_step_assert(uint32_t f) {
    if (g_ctl_bad) return 1;
    uint32_t t[2], c[2]; read_types(t, c);
    for (int s = 0; s < 2; s++)
        if (t[s] != 1 || c[s] != 1) {
            LOG("CTLTYPE f=%u side=%d t324=%u ctl30=%u (a side's control type changed inside the battle)\n", f, s, t[s], c[s]);
            entry_set_token("CTLTYPE");
            g_ctl_bad = 1;
            ns_ctltype(f, s, t[s], c[s]);
            return 0;
        }
    return 1;
}
