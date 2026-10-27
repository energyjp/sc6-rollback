/* v0.19.0 localizer: L1 capture inside FUN_1402edb90 (PLAYER0), L3 canonical call context. */
#pragma once
#include "common.h"

enum { L1P_NONE = 0, L1P_LIVE, L1P_RESIM, L1P_REPLAY };
enum { L1S_STEP, L1S_ENTRY, L1S_SAMP1, L1S_SAMP2, L1S_SAMP3, L1S_MASK, L1S_LOOPX, L1S_HOLD, L1S_EXIT, L1S_N };

int  l1_install(void);                                   /* after MH_Initialize; 1 = probes installed */
void l1_pass_begin(int kind, uint64_t seq, uint64_t k);  /* before a step pass (live / resim k / sync replay) */
void l1_pass_end(void);
void l1_iter_begin(uint64_t s, uint64_t k0);             /* before the resims (or the sync replay) of frame s */
void l1_check(uint64_t s, uint64_t k, int gameplay_bad, const char *path);   /* after resim k was compared */
void l1_frame_end(uint64_t s, const char *path, const char *lsn);           /* one L1W line per frame */
void l1_segment(const char *why, const char *path, int depth);              /* new path segment: E and budgets reset */
void l1_report(const char *why);
int  l1_veh(EXCEPTION_POINTERS *ep);                     /* kill switch: a fault inside the capture handler */
int  l1_active(void);

extern volatile int g_l3_on;
extern volatile uint8_t g_l3_vzero;                     /* v0.20: vzeroall in l3_call_step (AVX CPUs only) */
size_t l3_zero_bytes(void);                              /* bytes the L3 wrapper zeroes below its frame */
void l3_call_step(void *fn, void *iface, void *in, size_t zero_bytes);   /* l1_stub.S */

#ifdef SC6RB_HARNESS
void l1_harness_sites(const uintptr_t *abs_sites);       /* absolute addresses (L1S_N entries, 0 = real VA) */
extern volatile int g_l1_test_fault;
#endif
