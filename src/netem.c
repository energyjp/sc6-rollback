/* C2 (C2-SPEC.md §2.1, D9): see netem.h. Compiled only into the harness and the reflector (-DSC6RB_NETEM). */
#include "netem.h"
#include <stdlib.h>
#include <string.h>

static uint64_t sm64(uint64_t *s) { uint64_t z = (*s += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
static double u01(uint64_t *s) { return (double)(sm64(s) >> 11) * (1.0 / 9007199254740992.0); }

int ne_profile(const char *nm, NeDir *out, NeDir *back) {
    NeDir z; memset(&z, 0, sizeof z);
    NeDir a = z, b = z;
    if (!nm || !nm[0] || !strcmp(nm, "NONE")) { *out = a; *back = b; return 1; }
    if (!strcmp(nm, "LAN")) { a.lat_ms = 1; b.lat_ms = 1; }
    else if (!strcmp(nm, "P60")) { a.lat_ms = 60; a.jit_ms = 10; b = a; }
    else if (!strcmp(nm, "P120")) { a.lat_ms = 120; a.jit_ms = 30; a.loss_pct = 3; a.reorder_pct = 5; a.dup_pct = 1; b = a; }
    else if (!strcmp(nm, "ASYM")) { a.lat_ms = 30; b.lat_ms = 90; }
    else return 0;
    *out = a; *back = b;
    return 1;
}
int ne_active(const NeDir *d) { return d->lat_ms || d->jit_ms || d->loss_pct > 0 || d->dup_pct > 0 || d->reorder_pct > 0; }
int ne_init(NetEm *e, const NeDir *d, uint64_t seed) {
    memset(e, 0, sizeof *e);
    e->d = *d; e->rng = seed ? seed : 1;
    e->q = (NePkt *)calloc(NE_QCAP, sizeof(NePkt));
    return e->q != NULL;
}
void ne_free(NetEm *e) { free(e->q); e->q = NULL; e->n = 0; }
static int q1(NetEm *e, uint64_t due, const uint8_t *b, size_t n) {
    if (e->n >= NE_QCAP || n > NE_PKT) { e->overflow++; return 0; }
    NePkt *p = &e->q[e->n++];
    p->due_us = due; p->order = e->order++; p->len = (uint16_t)n; memcpy(p->b, b, n);
    return 1;
}
int ne_push(NetEm *e, uint64_t now_us, const uint8_t *b, size_t n) {
    e->pushed++;
    if (e->d.loss_pct > 0 && u01(&e->rng) * 100.0 < e->d.loss_pct) { e->lost++; return 0; }
    int copies = 1 + (e->d.dup_pct > 0 && u01(&e->rng) * 100.0 < e->d.dup_pct);
    if (copies == 2) e->dups++;
    int qd = 0;
    for (int c = 0; c < copies; c++) {
        double j = e->d.jit_ms ? (u01(&e->rng) * 2.0 - 1.0) * e->d.jit_ms : 0.0;
        double ms = e->d.lat_ms + j;
        if (e->d.reorder_pct > 0 && u01(&e->rng) * 100.0 < e->d.reorder_pct) { ms += u01(&e->rng) * 2.0 * (e->d.jit_ms ? e->d.jit_ms : 1); e->reordered++; }
        if (ms < 0) ms = 0;
        qd += q1(e, now_us + (uint64_t)(ms * 1000.0), b, n);
    }
    return qd;
}
int ne_pop(NetEm *e, uint64_t now_us, uint8_t *b, size_t *n) {
    int best = -1;
    for (int i = 0; i < e->n; i++) {
        if (e->q[i].due_us > now_us) continue;
        if (best < 0 || e->q[i].due_us < e->q[best].due_us || (e->q[i].due_us == e->q[best].due_us && e->q[i].order < e->q[best].order)) best = i;
    }
    if (best < 0) return 0;
    memcpy(b, e->q[best].b, e->q[best].len); *n = e->q[best].len;
    e->q[best] = e->q[--e->n];
    return 1;
}
