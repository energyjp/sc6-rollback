#pragma once
#include <stdint.h>
#include "snap.h"

typedef struct { uintptr_t lo, hi; } ScanRange;
typedef struct {
    uintptr_t obj;        /* address of the {Myhead, Mysize} pair */
    uintptr_t sentinel;
    uint64_t size;
    int kind;             /* 1 std::list, 2 std::map/set */
    int sentinel_covered; /* sentinel already inside a snapshotted range */
    int nodes_out;        /* nodes outside snapshotted ranges (pinned) */
    int nodes_seen;
} FoundCont;

int scan_containers(const ScanRange *ranges, int nranges, int (*covered)(uintptr_t a, size_t n),
                    FoundCont *found, int maxfound, SPin *pins, int maxpins, int *npins_out);
/* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.3): the container checks, exported for the Q1 root walk (src/rbo_q1.inc). h = the sentinel / head,
 * size = the container's count; nodes (up to maxnodes) receive the node addresses in walk order, *nn their number. 1 = the walk is exact
 * (std::list: every prev link and the count; std::map: the head flags, every node's isnil, the in-order count, first/last = left/right). */
int check_list(uintptr_t h, uint64_t size, uintptr_t *nodes, int maxnodes, int *nn);
int check_tree(uintptr_t h, uint64_t size, uintptr_t *nodes, int maxnodes, int *nn);
/* v0.29.4 Q1: 1 = an EMPTY head that passes check_tree (an MSVC std::map head: _Isnil 1, left = parent = right = itself) is a tree (link pin
 * 0x1a), tried before the list rule (an empty tree head also passes check_list and was pinned 16 bytes: core:0x14470f310). Set by modes.c
 * q1_setup in the Q1 modes only (rbonline; rbreplay with a q1 key); 0 everywhere else, so every other mode's scan is byte-identical. */
extern int g_scan_q1;
/* v0.29.6 (review MINOR 6): in the Q1 modes a NON-empty head whose _Isnil byte [h+0x19] is 1 is also tried as a tree before the list rule (a
 * 1-node std::map passes check_list too and was pinned 16 bytes). Harness builds only: g_scan_h_map1_old = 1 restores v0.29.5's order (the
 * control of the in-process case Q1MAP1). */
#ifdef SC6RB_HARNESS
extern int g_scan_h_map1_old;
#endif
/* v0.29.5 Q1R (m4): what a Q1-mode scan could NOT record, so E7 (src/rbo_q1.inc) fails closed on it instead of never seeing it. Filled only
 * while g_scan_q1 is set (every other mode's scan is byte-identical: no new read, no new pin); reset by every scan_containers call.
 *   unchecked     container heads with an MSVC shape (a locally consistent list ring / a tree head) whose bounded walk broke (twice), ran past
 *                 LIST_LIMIT nodes (oversize) or hit the per-scan walk budget (budget); a closed walk whose count differs from the size field
 *                 (a stored iterator) and a NULL-terminated chain (not an MSVC container) are NOT counted (DESIGN.md §1.2)
 *   pins_dropped  link pins that did not fit the caller's pin array (v0.29.4 dropped them silently)
 *   nodes_cut     containers with more nodes than the scan's node buffer (8192): the nodes past it were neither pinned nor counted
 *   u[]           the first SCANQ1_U unchecked heads (obj = the {head, size} pair's address); why: SCANQ1_RING / _TREE / _OVERSIZE / _BUDGET */
#define SCANQ1_U 4
enum { SCANQ1_RING = 1, SCANQ1_TREE, SCANQ1_OVERSIZE, SCANQ1_BUDGET };
typedef struct {
    int unchecked, oversize, budget, pins_dropped, nodes_cut;
    struct { uintptr_t obj, head; uint64_t size; int kind, why; } u[SCANQ1_U];
} ScanQ1Stat;
extern ScanQ1Stat g_scan_q1_stat;
/* v0.31.0 (OPTA-M2-SPEC.md §20.3): the probe memo of scan_containers. g_scan_memo_on (set by modes.c in rbonline; 0 everywhere else, so
 * every other mode's scan is byte-identical): a probe of the SAME address as the probe right before it (the 2-5 checks of one {head, size}
 * candidate: the tree flag, check_list, check_tree, the Q1 shape note) reuses that probe's answer instead of another ReadProcessMemory +
 * VirtualQuery; any probe of another address ends it, and it lives only inside one scan_containers call. g_scan_cost = the last call's
 * counts (the SCAN line's rpm= vq= memo= in rbonline). */
extern int g_scan_memo_on;
typedef struct { unsigned rpm, vq, memo; } ScanCost;
extern ScanCost g_scan_cost;
