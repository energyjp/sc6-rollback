/* Container discovery (GATE-B addition to R1).
 * MSVC std::list / std::map keep a heap-allocated sentinel node. A step that
 * links or unlinks nodes writes the sentinel's (and neighbors') link fields.
 * When the sentinel or nodes are outside every snapshotted range (allocated
 * before routing started), restoring .bss alone would leave the links stale.
 * We find such containers structurally ({Myhead*, Mysize} pairs whose ring or
 * tree walk is exact) and pin their link bytes into every snapshot. */
#include "common.h"
#include "snap.h"
#include "scan.h"

/* VirtualQuery cache (game thread only) */
typedef struct { uintptr_t lo, hi; } RR;
static RR g_rr[32]; static int g_rr_n, g_rr_next;

/* v0.31.0 (OPTA-M2-SPEC.md §20.3; scan.h): the probe memo of one scan_containers call (rbonline) and the probe counts */
int g_scan_memo_on;
ScanCost g_scan_cost;
static int g_pm_active, g_pm_res; static uintptr_t g_pm_p; static size_t g_pm_n;
static int readable_probe(uintptr_t p, size_t n) {
    {   /* round 15: a probe read that fails instead of faulting. The region cache alone let the scan
         * read a VirtualFree'd motion region (harness anim_kill case), so every check now proves the
         * bytes readable right before they are used. */
        uint8_t tmp[64]; SIZE_T got = 0;
        if (n > sizeof tmp) n = sizeof tmp;
        g_scan_cost.rpm++;
        if (!ReadProcessMemory(GetCurrentProcess(), (const void *)p, tmp, n, &got) || got != n) return 0;
    }
    for (int i = 0; i < g_rr_n; i++) if (p >= g_rr[i].lo && p + n <= g_rr[i].hi) return 1;
    MEMORY_BASIC_INFORMATION mbi;
    g_scan_cost.vq++;
    if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    DWORD pr = mbi.Protect & 0xff;
    if (!(pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    RR r = { (uintptr_t)mbi.BaseAddress, (uintptr_t)mbi.BaseAddress + mbi.RegionSize };
    g_rr[g_rr_next] = r; g_rr_next = (g_rr_next + 1) % 32; if (g_rr_n < 32) g_rr_n++;
    return p + n <= r.hi;
}
static int readable_cached(uintptr_t p, size_t n) {
    if (p < 0x10000 || p > 0x7ffffffeffffULL) return 0;
    if (n > 64) n = 64;   /* the probe proves at most 64 bytes (as before: readable_probe clamps the same way) */
    /* v0.31.0: the same address as the probe right before it (no other probe in between: one candidate's checks) reuses its answer; readability
     * is monotone in n (ok for n <= the proven length, not ok for n >= the failed one) */
    if (g_pm_active && p == g_pm_p && (g_pm_res ? n <= g_pm_n : n >= g_pm_n)) { g_scan_cost.memo++; return g_pm_res; }
    int r = readable_probe(p, n);
    if (g_pm_active) { g_pm_p = p; g_pm_n = n; g_pm_res = r; }
    return r;
}

int mem_readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = (uintptr_t)p, end = a + n;
    while (a < end) {
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

static int plausible_ptr(uintptr_t v) { return v >= 0x10000 && v < 0x7fffffff0000ULL && !(v & 7); }

#define LIST_LIMIT 200000
/* v0.29.2 (OPTA-M2-SPEC.md §16.3): exported (scan.h) for the Q1 root walk (src/rbo_q1.inc); behavior unchanged */
int check_list(uintptr_t h, uint64_t size, uintptr_t *nodes, int maxnodes, int *nn) {
    if (!readable_cached(h, 16)) return 0;
    uintptr_t next = ((uintptr_t *)h)[0], prev = ((uintptr_t *)h)[1];
    if (size == 0) return next == h && prev == h;
    if (!plausible_ptr(next) || !plausible_ptr(prev) || next == h) return 0;
    uintptr_t cur = next, last = h;
    uint64_t k = 0;
    *nn = 0;
    while (cur != h) {
        if (k >= size || k > LIST_LIMIT || !plausible_ptr(cur) || !readable_cached(cur, 16)) return 0;
        if (((uintptr_t *)cur)[1] != last) return 0;
        if (*nn < maxnodes) nodes[(*nn)++] = cur;
        last = cur;
        cur = ((uintptr_t *)cur)[0];
        k++;
    }
    return k == size && prev == last;
}

/* MSVC _Tree_node: _Left +0, _Parent +8, _Right +0x10, _Color +0x18, _Isnil +0x19 */
int check_tree(uintptr_t h, uint64_t size, uintptr_t *nodes, int maxnodes, int *nn) {   /* v0.29.2: exported (scan.h) */
    if (!readable_cached(h, 0x1a)) return 0;
    uint8_t *hb = (uint8_t *)h;
    if (hb[0x19] != 1 || hb[0x18] > 1) return 0;
    uintptr_t left = ((uintptr_t *)h)[0], root = ((uintptr_t *)h)[1], right = ((uintptr_t *)h)[2];
    if (size == 0) return left == h && root == h && right == h;
    if (!plausible_ptr(root) || !readable_cached(root, 0x1a)) return 0;
    if (((uintptr_t *)root)[1] != h || ((uint8_t *)root)[0x19] != 0) return 0;
    /* iterative in-order walk */
    static uintptr_t stack[256];
    int sp = 0;
    uintptr_t cur = root, first = 0, lastn = 0;
    uint64_t k = 0;
    *nn = 0;
    while (sp || (cur != h)) {
        while (cur != h) {
            if (!plausible_ptr(cur) || !readable_cached(cur, 0x1a) || ((uint8_t *)cur)[0x19] != 0) return 0;
            if (sp >= 256) return 0;
            stack[sp++] = cur;
            cur = ((uintptr_t *)cur)[0];
        }
        cur = stack[--sp];
        if (!first) first = cur;
        lastn = cur;
        if (*nn < maxnodes) nodes[(*nn)++] = cur;
        if (++k > size || k > LIST_LIMIT) return 0;
        cur = ((uintptr_t *)cur)[2];
    }
    return k == size && first == left && lastn == right;
}

int g_scan_q1;   /* v0.29.4 Q1: see scan.h */
#ifdef SC6RB_HARNESS
int g_scan_h_map1_old;   /* v0.29.6 harness only (scan.h): 1 = v0.29.5's classification (no tree-first rule for a non-empty head; control of Q1MAP1) */
#define SCAN_MAP1_OLD() (g_scan_h_map1_old != 0)
#else
#define SCAN_MAP1_OLD() 0
#endif

/* ------------------------------------------------------------------ v0.29.5 Q1R (m4): the Q1-only census of what the scan cannot record
 * (scan.h ScanQ1Stat; design_v0295/q1r/DESIGN.md §1). Reached only while g_scan_q1 is set; check_list / check_tree above are unchanged. */
ScanQ1Stat g_scan_q1_stat;
#define Q1R_BUDGET (2L * LIST_LIMIT + 2)   /* node visits per scan for the shape walks below (one oversize ring fits whole) */
static long g_q1r_budget;
/* the list ring from h WITHOUT the size field: >= 0 its node count (closed, every prev link right), -1 a NULL link (not an MSVC list: a
 * NULL-terminated chain), -2 broken (an implausible or unreadable link, a wrong prev link), -3 past LIST_LIMIT nodes, -4 no budget left */
static long q1r_ring(uintptr_t h) {
    uintptr_t cur = ((uintptr_t *)h)[0], last = h;
    long k = 0;
    while (cur != h) {
        if (!cur) return -1;
        if (k > LIST_LIMIT) return -3;
        if (g_q1r_budget <= 0) return -4;
        g_q1r_budget--;
        if (!plausible_ptr(cur) || !readable_cached(cur, 16) || ((uintptr_t *)cur)[1] != last) return -2;
        last = cur;
        cur = ((uintptr_t *)cur)[0];
        k++;
    }
    if (k > LIST_LIMIT) return -3;   /* a ring of exactly LIST_LIMIT + 1 nodes closes here */
    return ((uintptr_t *)h)[1] == last ? k : -2;
}
/* the in-order walk of an MSVC tree head h WITHOUT the size field (the same results; -1 a NULL child) */
static long q1r_tree(uintptr_t h) {
    static uintptr_t stack[256];
    uintptr_t left = ((uintptr_t *)h)[0], cur = ((uintptr_t *)h)[1], right = ((uintptr_t *)h)[2], first = 0, lastn = 0;
    int sp = 0;
    long k = 0;
    while (sp || cur != h) {
        while (cur != h) {
            if (!cur) return -1;
            if (g_q1r_budget <= 0) return -4;
            g_q1r_budget--;
            if (!plausible_ptr(cur) || !readable_cached(cur, 0x1a) || ((uint8_t *)cur)[0x19] != 0 || sp >= 256) return -2;
            stack[sp++] = cur;
            cur = ((uintptr_t *)cur)[0];
        }
        cur = stack[--sp];
        if (!first) first = cur;
        lastn = cur;
        if (++k > LIST_LIMIT) return -3;
        cur = ((uintptr_t *)cur)[2];
    }
    return first == left && lastn == right ? k : -2;
}
/* a {head, size} pair the exact checks did not accept (or whose size is past LIST_LIMIT): an MSVC container SHAPE at h (a list ring that is
 * consistent at h, or a tree head whose root points back at it) whose bounded walk breaks (twice), runs past LIST_LIMIT or runs out of budget is
 * counted unchecked; a closed walk with another count (a stored iterator followed by another field) and a NULL link are not containers */
static void q1r_note(uintptr_t a, uintptr_t h, uint64_t size) {
    if (!readable_cached(h, 16)) return;
    uintptr_t next = ((uintptr_t *)h)[0], prev = ((uintptr_t *)h)[1];
    int kind = 0;
    if (next != h && prev != h && plausible_ptr(next) && plausible_ptr(prev) && readable_cached(next, 16) && readable_cached(prev, 16) &&
        ((uintptr_t *)next)[1] == h && ((uintptr_t *)prev)[0] == h) kind = 1;
    else if (readable_cached(h, 0x1a) && ((uint8_t *)h)[0x19] == 1 && ((uint8_t *)h)[0x18] <= 1) {
        uintptr_t root = ((uintptr_t *)h)[1];
        if (root != h && plausible_ptr(root) && readable_cached(root, 0x1a) && ((uintptr_t *)root)[1] == h && ((uint8_t *)root)[0x19] == 0) kind = 2;
    }
    if (!kind) return;
    long r = kind == 1 ? q1r_ring(h) : q1r_tree(h);
    if (r == -2) r = kind == 1 ? q1r_ring(h) : q1r_tree(h);   /* broken: once more (a structure another thread was changing) */
    if (r == -1 || (r >= 0 && (uint64_t)r != size)) return;
    ScanQ1Stat *s = &g_scan_q1_stat;
    int why = r == -3 ? SCANQ1_OVERSIZE : r == -4 ? SCANQ1_BUDGET : kind == 1 ? SCANQ1_RING : SCANQ1_TREE;
    if (why == SCANQ1_OVERSIZE) s->oversize++;
    if (why == SCANQ1_BUDGET) s->budget++;
    if (s->unchecked < SCANQ1_U) { s->u[s->unchecked].obj = a; s->u[s->unchecked].head = h; s->u[s->unchecked].size = size; s->u[s->unchecked].kind = kind; s->u[s->unchecked].why = why; }
    s->unchecked++;
}

int scan_containers(const ScanRange *ranges, int nranges, int (*covered)(uintptr_t a, size_t n),
                    FoundCont *found, int maxfound, SPin *pins, int maxpins, int *npins_out) {
    static uintptr_t nodes[8192];
    int nf = 0, np = 0;
    g_rr_n = 0;
    memset(&g_scan_cost, 0, sizeof g_scan_cost);   /* v0.31.0 */
    g_pm_active = g_scan_memo_on; g_pm_p = 0;      /* v0.31.0: the memo lives inside this call only */
    if (g_scan_q1) { memset(&g_scan_q1_stat, 0, sizeof g_scan_q1_stat); g_q1r_budget = Q1R_BUDGET; }   /* v0.29.5 Q1R (m4) */
    for (int r = 0; r < nranges; r++) {
        uintptr_t a = (ranges[r].lo + 7) & ~(uintptr_t)7;
        for (; a + 16 <= ranges[r].hi; a += 8) {
            uintptr_t h = *(uintptr_t *)a;
            uint64_t size = *(uint64_t *)(a + 8);
            if (!plausible_ptr(h) || size > LIST_LIMIT) {
                /* v0.29.5 Q1R (m4): a head past LIST_LIMIT (a count below 2^32: a larger qword is a pointer or other data, not a count) */
                if (g_scan_q1 && plausible_ptr(h) && size <= 0xffffffffULL && !IN_IMAGE(h)) q1r_note(a, h, size);
                continue;
            }
            if (IN_IMAGE(h)) continue;   /* sentinels are heap nodes */
            int nn = 0, kind = 0;
            /* v0.29.6 (review MINOR 6, OPTA-M2-SPEC.md §18.9): in the Q1 modes a non-empty head whose _Isnil byte [h+0x19] is 1 is tried as a tree
             * FIRST. A 1-node std::map (head: _Left = _Parent = _Right = the node; node: _Left = _Parent = _Right = the head) also passes check_list
             * (next = prev = the node, node.prev = head), and v0.29.5 recorded it as a list with 16-byte pins: the head's _Right and the node's
             * _Right / _Color / _Isnil were not pinned (fail-open, the size-1 sibling of v0.29.4's empty-map rule). A wrong tree reading of a list
             * only over-pins (0x1a bytes over 16). tree1st is 0 outside the Q1 modes (no read, no call): every other mode is unchanged. */
            int tree1st = g_scan_q1 && size && !SCAN_MAP1_OLD() && readable_cached(h, 0x1a) && ((uint8_t *)h)[0x19] == 1;
            if (g_scan_q1 && size == 0 && check_tree(h, 0, nodes, 8192, &nn)) kind = 2;   /* v0.29.4 Q1: an empty std::map head (scan.h) */
            else if (tree1st && check_tree(h, size, nodes, 8192, &nn)) kind = 2;         /* v0.29.6: a non-empty std::map head (above) */
            else if (check_list(h, size, nodes, 8192, &nn)) kind = 1;
            else if (!tree1st && check_tree(h, size, nodes, 8192, &nn)) kind = 2;        /* tree1st: check_tree already failed above */
            if (!kind) { if (g_scan_q1) q1r_note(a, h, size); continue; }   /* v0.29.5 Q1R (m4): a head the exact checks did not accept */
            size_t link = kind == 1 ? 16 : 0x1a;
            int sent_cov = covered(h, link);
            int out = 0;
            if (!sent_cov) { if (np < maxpins) { pins[np].addr = h; pins[np].len = (uint32_t)link; np++; } else if (g_scan_q1) g_scan_q1_stat.pins_dropped++; }   /* v0.29.5 Q1R (m4): counted */
            for (int i = 0; i < nn; i++) {
                if (covered(nodes[i], link)) continue;
                out++;
                if (np < maxpins) { pins[np].addr = nodes[i]; pins[np].len = (uint32_t)link; np++; }
                else if (g_scan_q1) g_scan_q1_stat.pins_dropped++;   /* v0.29.5 Q1R (m4) */
            }
            if (g_scan_q1 && (uint64_t)nn < size) g_scan_q1_stat.nodes_cut++;   /* v0.29.5 Q1R (m4): nodes past the 8192-node buffer: unpinned, uncounted */
            if (nf < maxfound) {
                found[nf].obj = a; found[nf].sentinel = h; found[nf].size = size; found[nf].kind = kind;
                found[nf].sentinel_covered = sent_cov; found[nf].nodes_out = out; found[nf].nodes_seen = nn;
            }
            nf++;
            a += 8; /* skip the size field */
        }
    }
    g_pm_active = 0; g_pm_p = 0;   /* v0.31.0: never carried into a later call (the Q1 walk's checks probe afresh) */
    *npins_out = np;
    return nf;
}
