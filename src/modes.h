#pragma once
#include "common.h"

typedef struct { uint32_t *p1, *p2; uint8_t *extra; } StepIn;   /* in[] passed to IBattleCore::Step */
typedef void (*StepFn)(void *iface, StepIn *in);
extern StepFn o_step;

extern volatile int g_online;            /* current battle is online (or unknown input object): passthrough */
extern volatile int g_in_tick;           /* inside ALuxBattleManager::Tick */
extern volatile int g_tick_steps;        /* steps run during the current Tick */
extern volatile int g_pending_mode;      /* -1 none */
extern volatile int g_pending_lbp;       /* -1 none, else 1/2 */
extern volatile int g_mark_pending;
extern volatile int g_pending_scrub, g_pending_depth;
extern volatile int g_pending_anim;   /* round 15: F5 anim_pins toggle */
extern volatile int g_pending_bigscrub;
extern volatile int g_pending_passes;   /* round 16: F5 sync1x passes toggle */   /* round 15: F7 big scrub toggle */   /* round 8: F7 stack scrub, F6 roll depth */
extern int g_depth_rt;
extern volatile int g_pending_cam;       /* -1 none, 0 include, 1 exclude */
extern volatile int g_pending_l3;        /* v0.19: F4 L3 canonical-context toggle */

void modes_init(void);                   /* after config + arena */
void hk_step(void *iface, StepIn *in);
void modes_on_init(int before, uint32_t seed, int routed);
void modes_on_setup(int before);
void modes_on_shutdown(int before);
void modes_periodic_report(const char *why);
void lp_install_family(void);
/* v0.20.0 C0b */
extern volatile int g_title_dirty;       /* the window title needs a refresh (main.c hk_tick) */
void modes_title(char *out, size_t cap);
/* optA M2 (rbonline; src/rbo_core.c): the armed path's entry points (additive: no existing mode calls them) */
int  modes_rbo_init(void);               /* process start: rb_cfg_check, the L4 buffer, the listener family, c1a_init, rb_init; 1 = ok */
int  modes_rbo_arm(void *orig_step);     /* ARM (game thread): the per-battle resets, the C1a/rb battle, the snapshot set; 1 = ok */
void modes_rbo_step(void *iface, StepIn *in);   /* the armed IFC step (the core's hk_step path) */
void modes_rbo_edge(char *out, size_t cap);     /* v0.31.0 (OPTA-M2-SPEC.md §20.3): this battle's round-edge counters (RB EDGE) */
void modes_rbo_end(void);                /* END: the rb tail and RBEND, the C1a END, the pins cleared */
void modes_rbo_step_plain(void *orig_step, void *iface, StepIn *in);   /* v0.29.1 (review M2): the degraded arm's step: one live pass through
                                                                         * run_step (listener proxy, scrubs, L3-stock + pre-zero), no rb core */
