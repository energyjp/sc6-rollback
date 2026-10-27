/* v0.20.0 C0b: L4 revert primitive (see l4.h). */
#include "l4.h"
#include <string.h>

size_t l4_revert(uint8_t *live, const uint8_t *before, size_t len, l4_span_cb cb, void *user) {
    return l4_revert_ex(live, before, len, cb, user, NULL, NULL, NULL);
}

size_t l4_revert_ex(uint8_t *live, const uint8_t *before, size_t len, l4_span_cb cb, void *user, l4_keep_cb keep, void *kuser, size_t *kept) {
    size_t total = 0;
    for (size_t off = 0; off < len; off += 64) {
        size_t cl = len - off < 64 ? len - off : 64;
        if (!memcmp(live + off, before + off, cl)) continue;
        size_t i0 = off, i1 = off + cl, n = 0;
        while (i0 < i1 && live[i0] == before[i0]) i0++;
        while (i1 > i0 && live[i1 - 1] == before[i1 - 1]) i1--;
        for (size_t i = i0; i < i1; i++) n += live[i] != before[i];
        if (cb) cb(user, i0, i1 - i0);
        uint8_t save[64];
        if (keep) memcpy(save, live + off, cl);
        memcpy(live + off, before + off, cl);
        if (keep)
            for (size_t q = 0; q + 8 <= cl; q += 8) {
                uint64_t bv, lv;
                memcpy(&bv, before + off + q, 8); memcpy(&lv, save + q, 8);
                if (bv != lv && keep(kuser, off + q, bv, lv)) { memcpy(live + off + q, save + q, 8); if (kept) (*kept)++; }
            }
        total += n;
    }
    return total;
}
