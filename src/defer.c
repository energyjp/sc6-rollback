#include "defer.h"
#include <stdlib.h>
#include <string.h>

int defer_init(DeferList *d, int cap) {
    d->e = (DefEnt *)calloc((size_t)cap, sizeof(DefEnt));
    d->n = 0; d->cap = d->e ? cap : 0;
    return d->e != NULL;
}

int defer_find(const DeferList *d, uintptr_t p) {
    for (int i = 0; i < d->n; i++) if (d->e[i].p == p) return i;
    return -1;
}

int defer_add(DeferList *d, uintptr_t p, uint64_t seq) {
    if (defer_find(d, p) >= 0) return 2;
    if (d->n >= d->cap) return 0;
    d->e[d->n].p = p; d->e[d->n].seq = seq; d->n++;
    return 1;
}

int defer_drop_from(DeferList *d, uint64_t seq) {
    int w = 0, dropped = 0;
    for (int i = 0; i < d->n; i++) {
        if (d->e[i].seq >= seq) { dropped++; continue; }
        d->e[w++] = d->e[i];
    }
    d->n = w;
    return dropped;
}

int defer_take_before(DeferList *d, uint64_t seq, uintptr_t *out, int max) {
    int w = 0, k = 0;
    for (int i = 0; i < d->n; i++) {
        if (d->e[i].seq < seq && k < max) { out[k++] = d->e[i].p; continue; }
        d->e[w++] = d->e[i];
    }
    d->n = w;
    return k;
}

int defer_remove(DeferList *d, uintptr_t p) {
    int i = defer_find(d, p);
    if (i < 0) return 0;
    d->e[i] = d->e[--d->n];
    return 1;
}

int defer_copy(DeferList *dst, const DeferList *src) {
    if (dst->cap < src->n) return 0;
    memcpy(dst->e, src->e, sizeof(DefEnt) * (size_t)src->n);
    dst->n = src->n;
    return 1;
}
