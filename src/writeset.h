#pragma once
#include <stdint.h>
extern int g_ws_active;
void ws_begin(int frames, uintptr_t workmem);
uint64_t ws_pre(void);    /* call right before the step: counts foreign writes since the last step */
uint64_t ws_post(void);   /* call right after the step: counts writes by the step */
void ws_report(const char *why);
void ws_abort(const char *why);
/* round 14: statics the step wrote (outside the given exclusions) and that were never written between
 * steps, merged; returns the number of ranges written to out (addr,len pairs) */
typedef struct { uintptr_t addr; uint32_t len; uint32_t hits; } WsxRange;
int ws_export_statics(WsxRange *out, int max, int *n_foreign_excluded, int *n_block_excluded, int *n_ue_excluded, int *n_ptr_excluded);
void ws_lsn_pre(void);
void ws_lsn_post(void);
