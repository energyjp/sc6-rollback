/* C2 (C2-SPEC.md §3): one-pad battle entry. Both PCs start Versus -> Player vs CPU with the same picks; the host is P1. Side 1 (P2) is
 * converted from CPU to human AT THE SOURCE before step 1: the SetMatchRule word 0x14470f22c and Setup's copy word 0x144843f7c := 1
 * (V: FUN_1402facb0 -> FUN_1402e5660 derive PLAYER+0x324 / controller+0x30 from the copy inside step 1). The host's side-1 CPU-setup
 * bytes travel in SETUP; the guest injects them (c1a_inj_plan / c1a_inj_apply). */
#pragma once
#include "common.h"

#define ENT_RULE0 0x14470f1d4ULL   /* SetMatchRule per-player block side 0 +0x04: the control type */
#define ENT_RULE1 0x14470f22cULL   /* side 1 */
#define ENT_COPY0 0x144843f24ULL   /* Setup's copy 0x144843ef0 + 0x34 (+ side * 0x58) */
#define ENT_COPY1 0x144843f7cULL
#define ENT_T324  0x324            /* PLAYER + 0x324: the control type the input fetch switches on (FUN_140312510) */
#define ENT_CTL30 0x30             /* controller 0x144715400 + side * 0x60 + 0x30 */

void entry_battle_reset(void);
/* 1 = the type words are (1,1) for side 0 and (2,2) or (1,1) for side 1; else logs ENTRY CONV REFUSE type=r0,c0,r1,c1 */
int  entry_check(void);
int  entry_convert(void);          /* writes 1 into both side-1 words (logs ENTRY CONV ok); 1 = done */
void entry_restore(int step1_ran); /* before step 1: 2 back into the side-1 words that still read 1 (ENTRY RESTORE ...); after: ENTRY RESTORE too_late */
int  entry_converted(void);
/* the host's side-1 CPU-setup spans: {u64 va, u16 len, bytes} x n into out; returns the bytes used (0 = no room) */
size_t entry_spans_put(uint8_t *out, size_t cap, uint16_t *nspans);
/* the guest: plan + apply the host's spans; 1 = ok (ENTRY INJ ok spans= bytes= full=), 0 = refused (ENTRY INJ REFUSE cat=; *cat set) */
int  entry_spans_apply(const uint8_t *p, size_t n, uint16_t nspans, char *cat, size_t catcap);
/* after step 1 (the live pass of f = 1): +0x324 and controller+0x30 = 1,1 -> ENTRY CHECK step1 ok; else ENTRY CHECK FAIL (0) */
int  entry_step1_check(void);
/* every later live pass: the two reads; 0 = a change (logs CTLTYPE f= side= t324= ctl30=) */
int  entry_step_assert(uint32_t f);
const char *entry_token(void);     /* "-", "CONV", "CONVREFUSE", "INJREFUSE", "CTLTYPE" */
void entry_set_token(const char *t);
