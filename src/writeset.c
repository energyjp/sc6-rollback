/* Write-set discovery (B0): diff .data+.bss of the exe and WorkMemory
 *   - before vs after each step     -> writes by the step
 *   - after step f vs before f+1    -> writes from outside the step (foreign writers)
 * Counters are kept per 8-byte word and reported as merged ranges. */
#include "common.h"
#include "writeset.h"

typedef struct { uintptr_t lo; size_t len; const char *name; uint8_t *shadow; uint16_t *cstep, *cfor, *clsn; } WsRegion;
static WsRegion g_ws[2];
static int g_ws_n;
int g_ws_active;
static int g_ws_done, g_ws_target;
static uint64_t g_ws_step_words, g_ws_for_words;

static void *valloc(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

static void free_regions(void) {
    for (int i = 0; i < g_ws_n; i++) {
        if (g_ws[i].shadow) VirtualFree(g_ws[i].shadow, 0, MEM_RELEASE);
        if (g_ws[i].cstep) VirtualFree(g_ws[i].cstep, 0, MEM_RELEASE);
        if (g_ws[i].cfor) VirtualFree(g_ws[i].cfor, 0, MEM_RELEASE);
        if (g_ws[i].clsn) VirtualFree(g_ws[i].clsn, 0, MEM_RELEASE);
    }
    memset(g_ws, 0, sizeof g_ws);
    g_ws_n = 0;
}

static int add_region(uintptr_t lo, size_t len, const char *name) {
    if (!mem_readable((void *)lo, len)) { LOG("WS region_unreadable name=%s va=%p len=%zu\n", name, (void *)lo, len); return 0; }
    WsRegion *r = &g_ws[g_ws_n];
    r->lo = lo; r->len = len & ~(size_t)7; r->name = name;
    r->shadow = (uint8_t *)valloc(r->len);
    r->cstep = (uint16_t *)valloc(r->len / 8 * 2);
    r->cfor = (uint16_t *)valloc(r->len / 8 * 2);
    r->clsn = (uint16_t *)valloc(r->len / 8 * 2);
    if (!r->shadow || !r->cstep || !r->cfor || !r->clsn) return 0;
    memcpy(r->shadow, (void *)lo, r->len);
    g_ws_n++;
    return 1;
}

void ws_begin(int frames, uintptr_t workmem) {
    free_regions();
    g_ws_done = 0; g_ws_target = frames; g_ws_step_words = g_ws_for_words = 0;
    if (frames <= 0) { g_ws_active = 0; return; }
    add_region(VA(A_IMG_DATA_LO), (size_t)(A_IMG_DATA_HI - A_IMG_DATA_LO), "img_data");
    if (workmem) add_region(workmem, WORKMEM_SIZE, "workmem");
    g_ws_active = g_ws_n > 0;
    LOG("WS begin frames=%d regions=%d workmem=%p\n", frames, g_ws_n, (void *)workmem);
}

static uint64_t diff_into(WsRegion *r, uint16_t *cnt) {
    const uint64_t *live = (const uint64_t *)r->lo;
    uint64_t *sh = (uint64_t *)r->shadow;
    size_t nw = r->len / 8;
    uint64_t changed = 0;
    for (size_t i = 0; i < nw; i += 8) {
        size_t lim = nw - i < 8 ? nw - i : 8;
        if (lim == 8 && !memcmp(live + i, sh + i, 64)) continue;
        for (size_t j = i; j < i + lim; j++) {
            uint64_t v = live[j];
            if (v != sh[j]) { sh[j] = v; if (cnt[j] != 0xffff) cnt[j]++; changed++; }
        }
    }
    return changed;
}

uint64_t ws_pre(void) {
    if (!g_ws_active) return 0;
    uint64_t n = 0;
    for (int i = 0; i < g_ws_n; i++) n += diff_into(&g_ws[i], g_ws[i].cfor);
    g_ws_for_words += n;
    return n;
}

static void report_kind(WsRegion *r, uint16_t *cnt, const char *kind, int *budget) {
    size_t nw = r->len / 8;
    size_t i = 0;
    while (i < nw) {
        if (!cnt[i]) { i++; continue; }
        size_t j = i;
        uint32_t mn = 0xffff, mx = 0;
        uint64_t sum = 0;
        size_t gap = 0;
        size_t last = i;
        for (; j < nw; j++) {
            if (cnt[j]) { if (cnt[j] < mn) mn = cnt[j]; if (cnt[j] > mx) mx = cnt[j]; sum += cnt[j]; last = j; gap = 0; }
            else if (++gap > 2) break;   /* merge gaps of up to 16 bytes */
        }
        if (*budget > 0) {
            char d[96]; describe_va(r->lo + i * 8, d, sizeof d);
            /* va = preferred-base VA (0x14xxxxxxx) for the image region, offset for workmem */
            int img = r->name[0] == 'i';
            LOG("WS kind=%s region=%s va=0x%llx len=%zu hmin=%u hmax=%u hsum=%llu at=%s\n", kind, r->name,
                (unsigned long long)(img ? RVA(r->lo + i * 8) + IMG_PREF : i * 8),
                (last - i + 1) * 8, mn, mx, (unsigned long long)sum, d);
            (*budget)--;
        }
        i = last + 1;
    }
}

/* round 14 export: words of img_data with step writes, no foreign writes, outside the core .bss and
 * the singletons (already in every snapshot) and outside the classified must-never-restore ranges */
static const uint64_t k_ws_block[][2] = {
    { 0x144168040ULL, 0x144168080ULL },   /* allocator / memory statistics (lock xadd counters) */
    { 0x14406f7c8ULL, 0x14406f7e0ULL },   /* engine counters (0x140d17290, 0x140d17e00, 0x1401bb3c0, ...) */
    { 0x1440e9250ULL, 0x1440e9260ULL },   /* CRT thread-safe-static epoch (_Init_thread_footer) */
};
int ws_export_statics(WsxRange *out, int max, int *n_for, int *n_blk, int *n_ue, int *n_ptr) {
    int n = 0; *n_for = 0; *n_blk = 0; *n_ue = 0; *n_ptr = 0;
    for (int ri = 0; ri < g_ws_n; ri++) {
        WsRegion *r = &g_ws[ri];
        if (r->name[0] != 'i') continue;   /* img_data only */
        size_t nw = r->len / 8;
        for (size_t i = 0; i < nw; i++) {
            if (!r->cstep[i] && !(g_cfg.ws_lsn_attrib && r->clsn[i])) continue;
            uintptr_t a = r->lo + i * 8;
            uint64_t va = (uint64_t)(a - g_base + IMG_PREF);
            if (va >= A_CORE_BSS_LO && va < A_CORE_BSS_HI) continue;
            if (va >= A_DATA_SINGLE_LO && va < A_DATA_SINGLE_HI) continue;
            int blk = 0;
            for (size_t b = 0; b < sizeof k_ws_block / sizeof k_ws_block[0]; b++) if (va >= k_ws_block[b][0] && va < k_ws_block[b][1]) blk = 1;
            if (blk) { (*n_blk)++; continue; }
            if (r->cfor[i]) { (*n_for)++; continue; }
            if (g_cfg.ws_lsn_attrib && r->clsn[i]) { (*n_ue)++; continue; }   /* written by UE inside a listener call */
            {   /* round 14b: never restore a pointer-valued static (registries, sentinels, lists) */
                uint64_t v = *(const uint64_t *)a;
                if (IN_IMAGE(v) || (v >= 0x100000000ULL && v < 0x7fffffffffffULL)) { (*n_ptr)++; continue; }
            }
            if (n > 0 && out[n - 1].addr + out[n - 1].len == a) { out[n - 1].len += 8; if (r->cstep[i] > out[n - 1].hits) out[n - 1].hits = r->cstep[i]; continue; }
            if (n >= max) continue;
            out[n].addr = a; out[n].len = 8; out[n].hits = r->cstep[i]; n++;
        }
    }
    return n;
}
void ws_collect_hook(void);   /* modes.c: copies the export before the region is freed */

void ws_report(const char *why) {
    if (!g_ws_n) return;
    LOG("WS report why=%s frames=%d step_words=%llu foreign_words=%llu\n", why, g_ws_done,
        (unsigned long long)g_ws_step_words, (unsigned long long)g_ws_for_words);
    int bs = 4000, bf = 4000;
    for (int i = 0; i < g_ws_n; i++) {
        report_kind(&g_ws[i], g_ws[i].cstep, "step", &bs);
        report_kind(&g_ws[i], g_ws[i].cfor, "foreign", &bf);
    }
    if (bs <= 0 || bf <= 0) LOG("WS truncated step_budget=%d foreign_budget=%d\n", bs, bf);
    ws_collect_hook();
    log_flush();
}

/* round 14b: around each top-level listener call inside a step (img_data only): words changed before the
 * call are the core's (cstep), words changed during it are UE's (clsn) */
void ws_lsn_pre(void) {
    if (!g_ws_active) return;
    for (int i = 0; i < g_ws_n; i++) if (g_ws[i].name[0] == 'i') g_ws_step_words += diff_into(&g_ws[i], g_ws[i].cstep);
}
void ws_lsn_post(void) {
    if (!g_ws_active) return;
    for (int i = 0; i < g_ws_n; i++) if (g_ws[i].name[0] == 'i') diff_into(&g_ws[i], g_ws[i].clsn);
}
uint64_t ws_post(void) {
    if (!g_ws_active) return 0;
    uint64_t n = 0;
    for (int i = 0; i < g_ws_n; i++) n += diff_into(&g_ws[i], g_ws[i].cstep);
    g_ws_step_words += n;
    if (++g_ws_done >= g_ws_target) {
        ws_report("done");
        g_ws_active = 0;
        free_regions();
    }
    return n;
}

void ws_abort(const char *why) {
    if (g_ws_n && g_ws_done > 0) ws_report(why);
    g_ws_active = 0;
    free_regions();
}
