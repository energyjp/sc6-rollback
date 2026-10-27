/* Snapshot-friendly arena allocator.
 * All allocator metadata (header, free lists, block headers) lives inside the
 * arena memory, so copying [base, base+used) captures the allocator state and
 * copying it back restores it. Freshly bumped blocks are zero-filled so a
 * resimulation never sees bytes left over from an abandoned timeline. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ARENA_NCLS 22
#define ARENA_HDR_SIZE 32u          /* per-block header in front of the user pointer */
#define ARENA_LARGE 0xffff

typedef struct {
    uint64_t magic;
    uint64_t cap;
    uint64_t bump;                  /* offset of the first never-used byte */
    uint64_t live_blocks;
    uint64_t live_bytes;            /* usable bytes of live blocks */
    uint64_t nallocs, nfrees;
    uint64_t free_head[ARENA_NCLS]; /* offsets of free class blocks (0 = empty) */
    uint64_t large_head;            /* offset of the first free large block */
} ArenaHdr;

typedef struct {
    uint8_t *base;
    size_t cap;
    ArenaHdr *h;
    /* not snapshotted: statistics that must survive restores */
    uint64_t peak_bump;
    uint64_t fail_count;
    uint64_t bad_free;
} Arena;

int    arena_init(Arena *a, void *mem, size_t cap);
void   arena_reset(Arena *a);
void  *arena_alloc(Arena *a, size_t size, size_t align);
int    arena_free(Arena *a, void *p);          /* 1 ok, 0 bad pointer / double free */
size_t arena_usize(const Arena *a, const void *p); /* usable size, 0 if not a live block */
int    arena_check(const Arena *a, char *err, size_t errcap); /* full consistency walk */

static inline int arena_owns(const Arena *a, const void *p) {
    return a->base && (const uint8_t *)p >= a->base && (const uint8_t *)p < a->base + a->cap;
}
static inline size_t arena_used(const Arena *a) { return a->h ? (size_t)a->h->bump : 0; }
