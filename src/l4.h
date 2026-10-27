/* v0.20.0 C0b: pure helpers for the L4 canonical listener and the random-depth roll7
 * (no game state; shared by the plugin and the unit test). */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Called once per differing 64-byte chunk with the trimmed span [first changed byte, last changed
 * byte] as an offset from the start of the range. */
typedef void (*l4_span_cb)(void *user, size_t off, size_t len);

/* Puts back every byte of live[0..len) that differs from before[0..len). Works in 64-byte chunks:
 * a differing chunk is copied back whole, which is the same as reverting only its changed bytes.
 * Returns the exact number of bytes that differed. cb may be NULL. */
size_t l4_revert(uint8_t *live, const uint8_t *before, size_t len, l4_span_cb cb, void *user);

/* v0.20.0 fix R2: as l4_revert, and then for every 8-byte word (offset a multiple of 8 from the start) whose
 * old value differs from the live one, keep(kuser, off, before_val, live_val) nonzero keeps the live value
 * (the old one would be a dangling pointer). *kept counts the kept words. keep may be NULL. */
typedef int (*l4_keep_cb)(void *user, size_t off, uint64_t before_val, uint64_t live_val);
size_t l4_revert_ex(uint8_t *live, const uint8_t *before, size_t len, l4_span_cb cb, void *user, l4_keep_cb keep, void *kuser, size_t *kept);

static inline uint64_t l4_splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/* Rollback depth for one frame of the random-depth roll7: a pure function of (seed, battle number,
 * battle step), uniform in 0..maxd, so the same seed gives the same depth sequence in every run. */
static inline int r7_depth_draw(uint32_t seed, uint64_t battle, uint64_t step, int maxd) {
    if (maxd < 0) return 0;
    uint64_t h = l4_splitmix64(l4_splitmix64(((uint64_t)seed << 32) ^ battle) ^ step);
    return (int)(h % (uint64_t)(maxd + 1));
}
