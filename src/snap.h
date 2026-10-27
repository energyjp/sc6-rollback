/* Region snapshots: copy, restore, masked hash and masked diff.
 * A SnapSet lists what a snapshot covers:
 *   - fixed regions (core .bss block, .data singletons, WorkMemory, our extras)
 *   - the arena's used range [base, base+used) (length varies per snapshot)
 *   - pins: small heap ranges (container sentinels/links) gathered into one blob
 * Masks are absolute address ranges excluded from hashes and diffs (but still
 * copied and restored). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "arena.h"

#define SNAP_MAXREG  24
#define SNAP_MAXMASK 64
#define SNAP_CHUNK   4096

typedef struct { uintptr_t addr; size_t len; char name[24]; } SRegion;
typedef struct { uintptr_t addr; uint32_t len; } SPin;
typedef struct { uintptr_t addr; size_t len; } SMask;

typedef struct {
    int nreg; SRegion reg[SNAP_MAXREG];
    Arena *arena;                  /* optional */
    volatile long *arena_lock;     /* optional spinlock held while touching the arena range */
    int npin, pincap; SPin *pins;  /* sorted by addr (equal addr: longer first); no pin lies wholly inside another */
    size_t pin_bytes;
    int pins_dropped;              /* v0.20.2: covered sub-pins (and exact duplicates) dropped by the last snapset_set_pins */
    size_t pins_dropped_bytes;
    uint8_t *pin_scratch; size_t pin_scratch_cap;
    int nmask; SMask mask[SNAP_MAXMASK];
    /* v0.23.0-C1b stable pin layout (rb only; stable = 0 keeps every function above exactly as before). In stable mode pins[] is in
     * SLOT order (never sorted, never compacted): a pin keeps its slot and its blob offset for the whole battle, a vanished or freed pin
     * is marked dead (neither taken nor restored, its bytes stay in the layout), a new pin is appended. Lookups by address go through
     * spins/sslot (the visible pins: alive and not hidden, sorted by address). gen changes at every rebuild (all older snapshots
     * become non-prefix). Restoring a snapshot writes the slots below its npin that were alive both when it was taken and now. */
    int stable;
    uint8_t *pstate;               /* per slot: SPIN_ALIVE, SPIN_VANISHED, SPIN_RELEASED, SPIN_FREED */
    uint8_t *phidden;              /* per slot: bit 0 hidden from lookups; bit 1 quarantined (alive, never vanished by a rescan; rb.c sets
                                    * bit 0 per pass: hidden for passes of frames >= its q) */
    int32_t *pdq;                  /* per slot: the frame from which the block is gone (PINGUARD), 0 = never guards */
    int pstcap;
    SPin *spins; int *sslot; int nspin, spcap;
    uint32_t gen;
    uint64_t layout_skips;         /* non-stable / old API: restores whose pins were skipped because the layout differed */
} SnapSet;
enum { SPIN_ALIVE = 0, SPIN_VANISHED = 1, SPIN_RELEASED = 2, SPIN_FREED = 3 };

typedef struct {
    uint8_t *buf[SNAP_MAXREG]; size_t cap[SNAP_MAXREG];
    uint8_t *abuf; size_t acap, alen;
    uint8_t *pbuf; size_t pcap, plen;
    int npin;                      /* pin count at capture time (sanity) */
    int valid;
    uint64_t seq;
    uint32_t gen;                  /* v0.23.0-C1b: the layout generation at capture time (stable mode) */
    uint8_t *pdead; size_t pdcap;  /* v0.23.0-C1b: per slot 1 = dead when taken (stable mode) */
} Snap;

/* region ids used in diff callbacks: >=0 fixed region index, -1 arena, -2 pins */
typedef void (*snap_diff_cb)(void *user, int region, size_t off, size_t len, uintptr_t addr);

void     snapset_init(SnapSet *s);
int      snapset_add_region(SnapSet *s, uintptr_t addr, size_t len, const char *name);
int      snapset_add_mask(SnapSet *s, uintptr_t addr, size_t len);
int      snapset_set_pins(SnapSet *s, const SPin *pins, int n); /* copies + sorts; v0.20.2: drops pins wholly inside one other pin */
void     snapset_clear_pins(SnapSet *s);
int      snapset_remove_pins_in(SnapSet *s, uintptr_t lo, uintptr_t hi); /* returns removed count */
int      snapset_find_pin(const SnapSet *s, uintptr_t addr);

int      snap_take(SnapSet *s, Snap *sn);          /* 0 on allocation failure */
void     snap_restore(SnapSet *s, const Snap *sn);
void     snap_restore_nopins(SnapSet *s, const Snap *sn);   /* v0.20.0 fix R1: regions and arena only */
uint64_t snap_hash_live(SnapSet *s);
uint64_t snap_hash_snap(SnapSet *s, const Snap *sn);
size_t   snap_diff_live(SnapSet *s, const Snap *sn, snap_diff_cb cb, void *user, int max_ranges, int *nranges_out);
void     snap_release(Snap *sn);
size_t   snap_bytes(const SnapSet *s);             /* bytes a snapshot copies right now */

uint64_t hash_bytes(const void *p, size_t n, uint64_t seed);

/* v0.23.0-C1b stable layout */
void     snapset_stable_reset(SnapSet *s, uint32_t gen);    /* stable on, no pins, generation gen */
/* merge a new candidate set (any order; deduped like snapset_set_pins): same (addr, len) keeps its slot, an alive slot not in the set
 * becomes SPIN_VANISHED, a new pin is appended. Returns 0 on allocation failure. *appended / *vanished get the counts. */
int      snapset_merge_pins(SnapSet *s, const SPin *pins, int n, int *appended, int *vanished);
int      snapset_lookup(const SnapSet *s, uintptr_t v);       /* slot of a visible pin covering v, or -1 (both modes) */
int      snapset_find_start(const SnapSet *s, uintptr_t a);   /* slot of a visible pin starting at a, or -1 (both modes) */
void     snapset_pin_kill(SnapSet *s, int slot, int state, int32_t dq);   /* stable: mark dead (and hidden) */
void     snapset_pin_hide(SnapSet *s, int slot);              /* stable: out of the lookups, still taken and restored */
/* v0.23.1 (review F3/F5): stable: mark dead EVERY alive slot that starts at lo or lies wholly inside [lo, hi) (duplicates, sub-pins,
 * hidden or not); returns the count (the visible set is rebuilt when > 0) */
int      snapset_kill_range(SnapSet *s, uintptr_t lo, uintptr_t hi, int state, int32_t dq);
int      snapset_find_alive_start(const SnapSet *s, uintptr_t a);   /* stable: the first ALIVE slot (hidden or not) starting at a, or -1 */
int      snapset_pin_flags(SnapSet *s, int slot, uint8_t flags);   /* stable: set phidden (1 = changed; the caller rebuilds the visible set) */
void     snapset_rebuild_visible(SnapSet *s);
/* stable: 1 = sn's layout is a prefix of the current one (same generation, sn->npin <= npin) */
int      snap_layout_prefix(const SnapSet *s, const Snap *sn);
/* stable: restores regions, the arena and the prefix pins (alive when taken and now). Returns 1, or 0 when the layout is not a prefix
 * (then nothing at all is written). Non-stable sets: snap_restore. */
int      snap_restore_ex(SnapSet *s, const Snap *sn);
uint64_t snap_hash_range(SnapSet *s, const uint8_t *view, uintptr_t live_addr, size_t len, uint64_t seed);
