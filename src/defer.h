/* Deferred frees of non-arena blocks freed inside a step.
 * Each entry is tagged with the step sequence number that freed it.
 *   - restore to "before step S"  -> defer_drop_from(S): those frees never happened
 *   - no snapshot older than S can be restored any more -> defer_take_before(S)
 *     returns the entries that may now be freed for real. */
#pragma once
#include <stdint.h>

typedef struct { uintptr_t p; uint64_t seq; } DefEnt;
typedef struct { DefEnt *e; int n, cap; } DeferList;

int  defer_init(DeferList *d, int cap);
int  defer_add(DeferList *d, uintptr_t p, uint64_t seq);  /* 1 added, 2 already pending, 0 full */
int  defer_drop_from(DeferList *d, uint64_t seq);
int  defer_take_before(DeferList *d, uint64_t seq, uintptr_t *out, int max);
int  defer_find(const DeferList *d, uintptr_t p);
int  defer_remove(DeferList *d, uintptr_t p);
int  defer_copy(DeferList *dst, const DeferList *src);
