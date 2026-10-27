/* C2 (C2-SPEC.md §2.1, D9): netem, a seeded delay queue applied on a sender's tx path. Built into the harness and the reflector ONLY
 * (-DSC6RB_NETEM); never into sc6rb.asi (static check: grep -c NETEM build/sc6rb.asi = 0). Pure C, no Windows.
 * Per direction: latency ms, jitter ms (uniform +-), loss %, duplicate %, reorder % (an extra 0..2 x jitter of delay). */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { int lat_ms, jit_ms; double loss_pct, dup_pct, reorder_pct; } NeDir;
#define NE_QCAP 2048
#define NE_PKT  1280
typedef struct { uint64_t due_us; uint64_t order; uint16_t len; uint8_t b[NE_PKT]; } NePkt;
typedef struct {
    NeDir d; uint64_t rng, order;
    int n; NePkt *q;            /* heap-allocated NE_QCAP slots (unsorted; pop takes the earliest due) */
    uint64_t pushed, lost, dups, reordered, overflow;
} NetEm;

/* "LAN" | "P60" | "P120" | "ASYM" | "NONE": out = this sender's direction, back = the other one (C2-SPEC.md §4). 1 = known. */
int  ne_profile(const char *name, NeDir *out, NeDir *back);
int  ne_init(NetEm *e, const NeDir *d, uint64_t seed);      /* 1 = ok */
void ne_free(NetEm *e);
int  ne_push(NetEm *e, uint64_t now_us, const uint8_t *b, size_t n);   /* copies queued (0 = lost or overflow) */
int  ne_pop(NetEm *e, uint64_t now_us, uint8_t *b, size_t *n);         /* 1 = a due packet was popped (the earliest due first) */
int  ne_active(const NeDir *d);
