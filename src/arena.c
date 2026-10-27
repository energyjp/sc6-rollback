#include "arena.h"
#include <string.h>
#include <stdio.h>

#define ARENA_MAGIC  0x534336524241524EULL /* "SC6RBARN" */
#define BLK_MAGIC    0xA7E4A11Cu
#define ST_LIVE 1
#define ST_FREE 2

typedef struct {
    uint32_t magic;
    uint16_t cls;
    uint16_t state;
    uint32_t pad;      /* bytes from block start to this header */
    uint32_t r;
    uint64_t usize;    /* requested size */
    uint64_t bsize;    /* total block size (start .. end) */
} Blk;
_Static_assert(sizeof(Blk) == ARENA_HDR_SIZE, "block header size");

static const uint32_t k_cls[ARENA_NCLS] = {
    64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
    640, 768, 1024, 1280, 1536, 2048, 3072, 4096, 8192
};

static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

int arena_init(Arena *a, void *mem, size_t cap) {
    memset(a, 0, sizeof *a);
    if (!mem || cap < 1 << 20) return 0;
    a->base = (uint8_t *)mem;
    a->cap = cap;
    a->h = (ArenaHdr *)mem;
    arena_reset(a);
    return 1;
}

void arena_reset(Arena *a) {
    ArenaHdr *h = a->h;
    memset(h, 0, sizeof *h);
    h->magic = ARENA_MAGIC;
    h->cap = a->cap;
    h->bump = align_up(sizeof(ArenaHdr), 64);
}

static int cls_for(uint64_t need) {
    for (int i = 0; i < ARENA_NCLS; i++)
        if (k_cls[i] >= need) return i;
    return -1;
}

static void *bump_block(Arena *a, uint64_t bsize) {
    ArenaHdr *h = a->h;
    if (h->bump + bsize > a->cap) return NULL;
    uint8_t *start = a->base + h->bump;
    h->bump += bsize;
    if (h->bump > a->peak_bump) a->peak_bump = h->bump;
    memset(start, 0, (size_t)bsize);
    return start;
}

void *arena_alloc(Arena *a, size_t size, size_t align) {
    ArenaHdr *h = a->h;
    if (!h) return NULL;
    if (align < 16) align = 16;
    if (align & (align - 1)) return NULL;
    if (size == 0) size = 1;
    uint8_t *start = NULL;
    uint64_t bsize;
    int cls;
    uint64_t need = (uint64_t)size + ARENA_HDR_SIZE;
    if (align == 16 && (cls = cls_for(need)) >= 0) {
        bsize = k_cls[cls];
        if (h->free_head[cls]) {
            start = a->base + h->free_head[cls];
            h->free_head[cls] = *(uint64_t *)(start + ARENA_HDR_SIZE);
            memset(start + ARENA_HDR_SIZE, 0, 8); /* clear the link; rest keeps restored contents */
        } else {
            start = bump_block(a, bsize);
        }
    } else {
        cls = ARENA_LARGE;
        need = (uint64_t)size + ARENA_HDR_SIZE + (align - 16);
        bsize = align_up(need, 64);
        /* first fit without splitting, bounded waste */
        uint64_t *link = &h->large_head;
        while (*link) {
            uint8_t *b = a->base + *link;
            Blk *fb = (Blk *)b;
            if (fb->bsize >= bsize && fb->bsize <= bsize * 2 + 4096) {
                *link = *(uint64_t *)(b + ARENA_HDR_SIZE);
                bsize = fb->bsize;
                start = b;
                memset(start + ARENA_HDR_SIZE, 0, 8);
                break;
            }
            link = (uint64_t *)(b + ARENA_HDR_SIZE);
        }
        if (!start) start = bump_block(a, bsize);
    }
    if (!start) { a->fail_count++; return NULL; }
    uint8_t *user = (uint8_t *)align_up((uint64_t)(uintptr_t)(start + ARENA_HDR_SIZE), align);
    Blk *b = (Blk *)(user - ARENA_HDR_SIZE);
    if ((uint8_t *)b != start) ((Blk *)start)->magic = 0; /* drop a stale header left by a reused large block */
    b->magic = BLK_MAGIC;
    b->cls = (uint16_t)cls;
    b->state = ST_LIVE;
    b->pad = (uint32_t)((uint8_t *)b - start);
    b->r = 0;
    b->usize = size;
    b->bsize = bsize;
    h->live_blocks++;
    h->live_bytes += bsize - ARENA_HDR_SIZE - b->pad;
    h->nallocs++;
    return user;
}

static Blk *live_hdr(const Arena *a, const void *p) {
    if (!arena_owns(a, p)) return NULL;
    const uint8_t *u = (const uint8_t *)p;
    if (((uintptr_t)u & 15) || u < a->base + sizeof(ArenaHdr) + ARENA_HDR_SIZE) return NULL;
    if ((uint64_t)(u - a->base) > a->h->bump) return NULL;
    Blk *b = (Blk *)(u - ARENA_HDR_SIZE);
    if (b->magic != BLK_MAGIC || b->state != ST_LIVE) return NULL;
    return b;
}

size_t arena_usize(const Arena *a, const void *p) {
    Blk *b = live_hdr(a, p);
    return b ? (size_t)(b->bsize - ARENA_HDR_SIZE - b->pad) : 0;
}

int arena_free(Arena *a, void *p) {
    ArenaHdr *h = a->h;
    Blk *b = live_hdr(a, p);
    if (!b) { a->bad_free++; return 0; }
    uint8_t *start = (uint8_t *)b - b->pad;
    uint64_t bsize = b->bsize;
    h->live_blocks--;
    h->live_bytes -= bsize - ARENA_HDR_SIZE - b->pad;
    h->nfrees++;
    if (b->cls < ARENA_NCLS) {
        b->state = ST_FREE;
        *(uint64_t *)(start + ARENA_HDR_SIZE) = h->free_head[b->cls];
        h->free_head[b->cls] = (uint64_t)(start - a->base);
    } else {
        Blk *nb = (Blk *)start;   /* normalize: header at block start */
        b->magic = 0;             /* old header (if pad>0) is no longer valid */
        nb->magic = BLK_MAGIC;
        nb->cls = ARENA_LARGE;
        nb->state = ST_FREE;
        nb->pad = 0;
        nb->usize = 0;
        nb->bsize = bsize;
        *(uint64_t *)(start + ARENA_HDR_SIZE) = h->large_head;
        h->large_head = (uint64_t)(start - a->base);
    }
    return 1;
}

int arena_check(const Arena *a, char *err, size_t errcap) {
    const ArenaHdr *h = a->h;
    uint64_t off = (sizeof(ArenaHdr) + 63) & ~63ULL, live = 0, livebytes = 0, nfree = 0;
    while (off < h->bump) {
        const Blk *b = (const Blk *)(a->base + off);
        /* class/large block with header at start (free or pad==0) */
        if (b->magic != BLK_MAGIC) {
            /* aligned large live block: header is further in; scan forward on 16 */
            uint64_t o2 = off + 16;
            const Blk *b2 = NULL;
            while (o2 < off + 65536 && o2 < h->bump) {
                const Blk *c = (const Blk *)(a->base + o2);
                if (c->magic == BLK_MAGIC && c->pad == o2 - off) { b2 = c; break; }
                o2 += 16;
            }
            if (!b2) { snprintf(err, errcap, "bad block magic at off %llu", (unsigned long long)off); return 0; }
            b = b2;
        }
        if (b->bsize == 0 || off + b->bsize > h->bump) { snprintf(err, errcap, "bad bsize at off %llu", (unsigned long long)off); return 0; }
        if (b->state == ST_LIVE) { live++; livebytes += b->bsize - ARENA_HDR_SIZE - b->pad; }
        else if (b->state == ST_FREE) nfree++;
        else { snprintf(err, errcap, "bad state at off %llu", (unsigned long long)off); return 0; }
        off += b->bsize;
    }
    if (live != h->live_blocks || livebytes != h->live_bytes) {
        snprintf(err, errcap, "live mismatch walk=%llu/%llu hdr=%llu/%llu",
                 (unsigned long long)live, (unsigned long long)livebytes,
                 (unsigned long long)h->live_blocks, (unsigned long long)h->live_bytes);
        return 0;
    }
    /* free lists must only contain free blocks */
    uint64_t listed = 0;
    for (int c = 0; c < ARENA_NCLS; c++) {
        uint64_t o = h->free_head[c];
        while (o) {
            const Blk *b = (const Blk *)(a->base + o);
            if (b->magic != BLK_MAGIC || b->state != ST_FREE || b->cls != c) { snprintf(err, errcap, "bad free-list entry cls %d", c); return 0; }
            listed++;
            o = *(const uint64_t *)(a->base + o + ARENA_HDR_SIZE);
            if (listed > nfree) { snprintf(err, errcap, "free-list cycle"); return 0; }
        }
    }
    uint64_t o = h->large_head;
    while (o) {
        const Blk *b = (const Blk *)(a->base + o);
        if (b->magic != BLK_MAGIC || b->state != ST_FREE) { snprintf(err, errcap, "bad large free entry"); return 0; }
        listed++;
        o = *(const uint64_t *)(a->base + o + ARENA_HDR_SIZE);
        if (listed > nfree) { snprintf(err, errcap, "large-list cycle"); return 0; }
    }
    if (listed != nfree) { snprintf(err, errcap, "free blocks %llu listed %llu", (unsigned long long)nfree, (unsigned long long)listed); return 0; }
    return 1;
}
