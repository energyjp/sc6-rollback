/* Generic vtable forwarding thunks (fwd_thunks.S).
 * Each thunk counts the call, optionally swaps 'this' (rcx), optionally clears a
 * thread-id variable for the duration of the call (so UE code running inside a
 * listener call is not treated as "inside the step"), forwards all register and
 * up to 12 stack arguments, and counts nonzero returns. A NULL target makes the
 * thunk return 0 without calling anything (used to mute notifications).
 * Optional pre/post hooks see (slot, rdx, result[2]); a pre hook returning nonzero
 * skips the target and returns result[0] in rax and result[1] in xmm0 (replay). */
#pragma once
#include <stdint.h>

#define FWD_MAXSLOTS 64
#define FWD_BANKS 2

typedef int (*FwdHook)(int slot, uint64_t rdx, uint64_t *result, uint64_t caller_rsp);

typedef struct {
    void **fn;                 /* +0x00 target per slot (NULL -> return 0) */
    void **self;               /* +0x08 replacement this per slot (NULL table or entry -> keep rcx) */
    uint64_t *counts;          /* +0x10 [FWD_BANKS][FWD_MAXSLOTS] */
    uint64_t *nonzero;         /* +0x18 [FWD_MAXSLOTS] (live bank only meaningful) */
    volatile uint32_t *bank;   /* +0x20 current bank */
    volatile uint32_t *tidvar; /* +0x28 cleared during the call, restored after (NULL -> none) */
    FwdHook pre;               /* +0x30 */
    FwdHook post;              /* +0x38 */
    void (*after)(void);       /* +0x40 called after every call path, before returning (stack scrub); NULL -> none */
    uint64_t clean_zero;       /* +0x48 nonzero: zero 4 KB below the thunk frame after the after-hook */
} FwdFamily;

extern FwdFamily lp_family;   /* listener proxy, 41 slots */
extern FwdFamily ifc_family;  /* IBattleCore interface census, 37 slots (+0x00..+0x120) */
extern void *lp_thunk_table[41];
extern void *ifc_thunk_table[37];
