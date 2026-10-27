/* v0.23.0-C1b: the rollback session core for mode=rbreplay (C1B-SPEC.md §2). See rb.h.
 * v0.24.0-C1b2 PART A (C1B-R2-SPEC.md §2): mode=rblive (the same core; the truth is the live table L of UE's FrameIn delayed by D instead of
 * the .rec: rb_truth), the lockstep zones (rb_zone=1: the remote record of a zone frame is known at its capture step), S1BFIX G2 (pindeny),
 * the new RBEND / RBEND2 / RBNET / RBBARRIER fields and the F suffix z=.
 *
 * Per step s (= the C1a battle step f, the .rec F number):
 *   step entry: tick bookkeeping, quarantined pins out of the lookups, pending barriers (re-pins, pin reallocs), fp_capture of the
 *   between-step writes, the replay-flag edges; drain (frontier F = F(s), new frames (C, min(F, s-1)]: I = mispredicted remote
 *   records, P = frames whose state-class listener call was muted); decide (refuse at or before the barrier, RPB for a pending frame
 *   behind the barrier, else one rollback per tick to min(I u P)); the live pass s (or a verify-only segment pass); release horizon
 *   H = C + 1 (deferred frees, the pin quarantine); confirm (the .play lines of the final passes, tier-1 against the recording).
 * Every refusal, abort and barrier is a log line; nothing falls back silently. */
#include "rb.h"
#include "modes.h"
#include "mem.h"
#include "emu.h"
#include "fwd.h"
#include "c1a_run.h"
#include "lipsync.h"   /* LIPSYNC: PART B hooks (REVERT-LIPSYNC.md) */
#include "netsess.h"
#include "net.h"
#include "entry.h"
#include "netproto.h"
#include "rbo_int.inc"   /* optA M2: the rbonline source (src/rbo_fetch.c) and the CHECK / drain hand-off (src/rbo.c) */
#include <stdlib.h>

volatile int g_rb_mem_on;

/* v0.23.1 review fixes: harness-only switches that turn ONE fix off, for the negative-control cases of test/harness_c1b.inc. They do
 * not exist in the plugin build (RBT(x) is the constant 0 there), so no ini value can reach them. */
#ifdef SC6RB_HARNESS
int g_rbt_old_freeq, g_rbt_no_relguard, g_rbt_old_dhit, g_rbt_no_dpin, g_rbt_rpb_dup, g_rbt_no_fpov;
int g_rbt_rpb_old, g_rbt_abort_nodeliver;   /* C1B3 fix: the pre-fix RPB (no re-apply, no rpb_free barrier); an abort that drops a pending state-class call */
int g_rbt_abort_old;   /* optA M2 v0.29.1 (XABORT): rbonline's v0.29.0 abort (a bare drain: no verification, no CHECK after it, no lost rule) */
int g_rbt_armf_zone;   /* v0.29.3 (review M2) control h_rb_armf_zone=1: the v0.29.2 pre-F rule (frames < rb_arm_f lockstep only inside a zone) */
int g_h_rs_pinf_plant;   /* v0.31.0 (XRSPINF) h_rbo_rs_pinf_plant=1: the pin filter is cleared after every rebuild (it misses every pin) */
#define RBT(x) (x)
#else
#define RBT(x) 0
#endif

/* ------------------------------------------------------------------ configuration */
static int g_cfg_ok;                 /* mode=rbreplay accepted at startup */
static int g_lv;                     /* v0.24.0-C1b2: mode=rblive (the live table is the truth); C2: also mode=rbnet */
static int g_nt;                     /* C2 (C2-SPEC.md §2.5): mode=rbnet (the live table holds the local records + the peer's) */
static int g_on;                     /* optA M2 (OPTA-M2-SPEC.md §5.4): mode=rbonline (the live table holds the local words + the remote's from the game's own
                                      * ring, src/rbo_fetch.c; the prediction is the stock rule on the last confirmed raw; used_ok compares the remote word) */
static int g_in_tail;                /* C2: flush_tail runs (the tail frames are not confirmed: no CHECK; fix r1: and no ZONE message, KA5) */
/* optA M2 v0.29.1 (review B2): rbonline's frames committed without a correcting core. At an abort, every pending frame whose truth is not
 * composed yet is recorded with the remote word it ran; after the abort every straight pass that ran on an unconfirmed word too. rbo_compose
 * checks each one when its truth arrives (a difference = a committed misprediction -> rbo DESYNC, loud). g_tail_chk: the abort's flush CHECKs
 * its frames (they are committed state); g_ab_*: the aborted side's own conservative zone rule for the CHECKs of its straight passes. */
#define UNV_N 256
static struct { uint32_t f; uint64_t w; } g_unv[UNV_N];
static int g_nunv; static uint32_t g_unv_verified, g_unv_lost, g_unv_dropped;
static int g_tail_chk; static uint32_t g_ab_lastz; static int g_ab_zset;
static float *g_l4m_bk; static uint32_t g_l4m_n;   /* C2 (5.3): the L4MISS microseconds of each step (p99_l4miss_us) */
static double g_l4_0, g_l4m_0; static uint32_t g_step_fwd;   /* C2: this step's L4 / L4MISS start values, forwarded listener calls (RBKC) */
static int g_step_depth;             /* C2: this step's rollback depth (-1 none) for ns_step_done */
static uint32_t g_nsl, g_nsr;        /* C2: the frames composed from the session's local / remote tables */
static char g_refused[48];           /* the first refusing key */
static int g_R;                      /* ring size */
static uint32_t g_gen_ctr;
static uint64_t g_bat;               /* rb battle counter */

static void refuse(const char *k, const char *v, const char *need) {
    LOG("CFG C1B REFUSE key=%s value=%s need=%s\n", k, v, need);
    if (!g_refused[0]) snprintf(g_refused, sizeof g_refused, "%s", k);
}
static void refuse_i(const char *k, long long v, const char *need) { char b[32]; snprintf(b, sizeof b, "%lld", v); refuse(k, b, need); }

static void pres_cfg(void);   /* v0.33.0 (OPTA-M2-SPEC.md §22.1): rb_pres_slots / rb_pres_repair, below */
static void refuse2(const char *k, const char *v, const char *need) {   /* C2 (C2-SPEC.md §6.1): the rbnet refusals */
    LOG("CFG C2 REFUSE key=%s value=%s need=%s\n", k, v, need);
    if (!g_refused[0]) snprintf(g_refused, sizeof g_refused, "%s", k);
}
static void refuse2_i(const char *k, long long v, const char *need) { char b[32]; snprintf(b, sizeof b, "%lld", v); refuse2(k, b, need); }
static int file_ok(const char *p) { if (!p[0]) return 0; DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
/* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.9, §16.11 contracts): the rbreplay hybrid keys. CFG Q1 REFUSE key= value= need= (the title shows
 * rb=REFUSED:<key> like every C1b refusal). rbonline ignores the ini values (its rules are by mode: CFG Q1 is logged by rbo_install). */
static const char *const k_q1_keys[5] = { "rb_l3stock", "rb_arm_f", "rb_init_pins", "q1_roots", "q1_cov" };
static void q1refuse(const char *k, long long v, const char *need) {
    LOG("CFG Q1 REFUSE key=%s value=%lld need=%s\n", k, v, need);
    if (!g_refused[0]) snprintf(g_refused, sizeof g_refused, "%s", k);
}
static void q1_cfg_check(const Config *c) {
    if (!c->q1_present) return;
    if (c->mode == MODE_RBLIVE || c->mode == MODE_RBNET) {   /* rbreplay-only keys */
        for (int i = 0; i < 5; i++) if (c->q1_present & (1 << i)) { LOG("CFG Q1 REFUSE key=%s value=set need=mode=rbreplay\n", k_q1_keys[i]); if (!g_refused[0]) snprintf(g_refused, sizeof g_refused, "%s", k_q1_keys[i]); }
        return;
    }
    if (c->mode != MODE_RBREPLAY) return;
    int sig = modes_q1_sig();
    LOG("CFG Q1 mode=rbreplay roots=%d init_pins=%d l3stock=%d arm_f=%d cov=%d table=%d sig=%s\n", c->q1_roots, c->rb_init_pins, c->rb_l3stock, c->rb_arm_f, c->q1_cov,
        modes_q1_rows(), sig ? "ok" : "BAD");
    if (c->rb_l3stock != 0 && c->rb_l3stock != 1) q1refuse("rb_l3stock", c->rb_l3stock, "0|1");
    if (c->rb_l3stock == 1 && c->l3_ini != 1) q1refuse("rb_l3stock", c->rb_l3stock, "l3=1");
    if (c->rb_arm_f < 0) q1refuse("rb_arm_f", c->rb_arm_f, ">=0(below_the_recording's_frames)");
    if (c->rb_arm_f > 0 && c->rb_zone != 1) q1refuse("rb_arm_f", c->rb_arm_f, "rb_zone=1(rbonline's_zone_rules)");   /* v0.29.3: the frames < F are lockstep by rb_arm_f itself (M2) */
    if (c->rb_init_pins != 0 && c->rb_init_pins != 1) q1refuse("rb_init_pins", c->rb_init_pins, "0|1");
    if (c->q1_roots != 0 && c->q1_roots != 1) q1refuse("q1_roots", c->q1_roots, "0|1");
    if (c->q1_cov != 0 && c->q1_cov != 1) q1refuse("q1_cov", c->q1_cov, "0|1");
    if (c->q1_roots == 1 && !sig) q1refuse("q1_roots", c->q1_roots, "Q1_SIG_ok=1(the_table_differs_from_this_exe)");
}

static void rbnet_cfg_check(void) {   /* C2: before the shared checks (it sets the remote side from the role) */
    Config *c = &g_cfg;
    if (c->present_net_remote_side) refuse2("net_remote_side", "set", "absent(the sides come from the role)");
    if (c->present_replay_file) refuse2("replay_file", "set", "absent");
    if (c->c1a_inject_set) refuse2("c1a_setup_inject", "set", "absent|none");
    if (c->present_emu) refuse2("emu_*", "set", "absent(no emulator in rbnet)");
    if (c->net_role != 1 && c->net_role != 2) refuse2("role", c->net_role ? "bad" : "(missing)", "host|guest");
    uint32_t ip; uint16_t pt;
    if (c->net_peer_kind < 0) refuse2("net_peer_kind", "bad", "udp|script");
    if (c->net_peer_kind == 0 && (!c->net_peer[0] || !net_parse_peer(c->net_peer, &ip, &pt))) refuse2("peer", c->net_peer[0] ? "unparsable" : "(missing)", "a.b.c.d:port");
    if (c->net_peer_kind == 1 && !file_ok(c->net_script_rec)) refuse2("net_script_rec", c->net_script_rec[0] ? "unreadable" : "(missing)", "an existing .rec");
    if (c->net_peer_kind == 1 && c->net_role == 1) refuse2("net_peer_kind", "script", "role=guest(launch E is a guest-only check)");
    if (c->net_local_src < 0) refuse2("net_local_src", "bad", "pad|rec");
    if (c->net_local_src == 1 && !file_ok(c->net_rec_file)) refuse2("net_rec_file", c->net_rec_file[0] ? "unreadable" : "(missing)", "an existing .rec");
    if (c->net_bind < 0) refuse2("net_bind", "bad", "auto|loopback|any");
    if (c->net_port < 1024 || c->net_port > 65535) refuse2_i("net_port", c->net_port, "1024..65535");
    if (c->lock_delay < 0) refuse2("lock_delay", "bad", "auto|4..12");
    if (c->net_lever < 0) refuse2("net_lever", "bad", "clamp|one");
    if (c->net_timeout_s < 4 || c->net_timeout_s > 60) refuse2_i("net_timeout_s", c->net_timeout_s, "4..60");
    if (c->pause_cap_s < 30 || c->pause_cap_s > 900) refuse2_i("pause_cap_s", c->pause_cap_s, "30..900");
    if (c->rb_zone != 1) refuse2_i("rb_zone", c->rb_zone, "1");
    {   int dlmax = c->lock_delay > 0 ? c->lock_delay : 12, need = c->net_max_rollback + dlmax + 2;   /* D12 */
        if (c->rb_zone_entry_gap < need) { char nd[32]; snprintf(nd, sizeof nd, ">=%d", need); refuse2_i("rb_zone_entry_gap", c->rb_zone_entry_gap, nd); }
        if (c->rb_zone_exit_margin < dlmax) { char nd[32]; snprintf(nd, sizeof nd, ">=%d", dlmax); refuse2_i("rb_zone_exit_margin", c->rb_zone_exit_margin, nd); } }
    if (c->net_role == 1 || c->net_role == 2) c->net_remote_side = c->net_role == 1 ? 2 : 1;   /* host: the guest drives P2; guest: the host drives P1 */
}
int rb_cfg_check(void) {
    g_nt = g_cfg.mode == MODE_RBNET;
    g_on = g_cfg.mode == MODE_RBONLINE;   /* optA M2 */
    if (g_nt) rbnet_cfg_check();
    /* C2 (D14): rbnet keeps lip-sync off without a PART B edit: the shared checks below read a copy whose lipsync is 0 (config_log printed
     * PART B's own CFG LIPSYNC REFUSE, since is_rb_mode(rbnet) = 0) */
    static Config s_cnet;
    const Config *c = &g_cfg;
    if (g_nt) { s_cnet = g_cfg; s_cnet.lipsync = 0; c = &s_cnet; }
    g_lv = c->mode == MODE_RBLIVE || g_nt || g_on;   /* optA M2: + rbonline */
    char ss[64]; rb_state_slots_str(c->rb_state_mask, ss, sizeof ss);
    char ctl[200]; cfg_controls_str(c, ctl, sizeof ctl);
    /* C1B3: (C1B-R3-SPEC.md §2.3.2) the zone events: the state class without slot 0x0c0 (a mid-fight +0x0c0 opens no lockstep zone) */
    /* C1B3 fix (round-3 review): printed as the spec's <hexmask> (bit i = slot i*8; 0x58 only = 0x800), never as a slot list: '58' read as a mask is slots 0x18,0x20,0x30 */
    char ze[64]; { uint64_t zm = c->rb_state_mask & ~(1ULL << (0xc0 / 8)); if (zm) snprintf(ze, sizeof ze, "0x%llx", (unsigned long long)zm); else snprintf(ze, sizeof ze, "none"); }
    g_R = modes_ring_size(); if (g_R < c->net_max_rollback + 3) g_R = c->net_max_rollback + 3;
    LOG("CFG C1B version=%s mode=%s remote=%d D=%d max_rb=%d lat_ms=%d jit_ms=%d loss_pct=%d red=%d seed=%lld state_slots=%s mt_tail=%d pinguard=%d quarantine=%d t2_heap=%s ring=%d controls=%s rbcfg=%016llx state_quiet=%d "
        "t1rule=%s mm_dumps=%d zone=%d zone_mode=%d zone_gap=%d zone_margin=%d live_cap=%d xpred=%s zone_events=%s t2heap=%s(%s)\n",
        SC6RB_VERSION, g_mode_names[c->mode], c->net_remote_side, c->net_delay, c->net_max_rollback, c->emu_latency_ms, c->emu_jitter_ms, c->emu_loss_pct,
        c->emu_redundancy, c->emu_seed, c->rb_state_mask ? ss : "INVALID", c->rb_mt_tail, c->rb_pinguard, c->rb_quarantine, c->c1a_t2_heap_tag ? "tag" : "raw", g_R, ctl,
        (unsigned long long)cfg_rbcfg_hash(c), c->rb_state_quiet, c->c1a_t1_rule == 1 ? "heaptag" : c->c1a_t1_rule == 0 ? "ptrmap" : "INVALID", c->c1a_mm_dumps,
        c->rb_zone, c->rb_zone_fight_mode, c->rb_zone_entry_gap, c->rb_zone_exit_margin, c->live_cap, RBT(c->h_old_xpred) ? "old" : "new", ze,
        c->c1a_t2_heap_s[0] ? c->c1a_t2_heap_s : "tag", c->c1a_t2_heap_tag == 2 ? "map" : c->c1a_t2_heap_tag == 1 ? "tag" : "raw");   /* C1B3: */
    if (c->l4 != 1) refuse_i("l4", c->l4, "1");
    if (c->l3_ini != 1) refuse_i("l3", c->l3_ini, "1");
    if (c->l3_fpenv != 1) refuse_i("l3_fpenv", c->l3_fpenv, "1");
    if (c->l3_mxcsr != 0x1f80) refuse_i("l3_mxcsr", c->l3_mxcsr, "0x1f80");
    if (c->l3_x87cw != 0x027f) refuse_i("l3_x87cw", c->l3_x87cw, "0x027f");
    if (c->cam_pin_full != 1) refuse_i("cam_pin_full", c->cam_pin_full, "1");
    if (c->camera_exclude) refuse("camera", "exclude", "include");
    if (c->ws_statics != 0) refuse_i("ws_statics", c->ws_statics, "0");
    if (c->query_forward) refuse("query_policy", "forward", "null");
    if (c->lsn_replay != 0) refuse_i("listener_replay", c->lsn_replay, "0");
    if (!c->ai_rand_det) refuse("ai_rand", "crt", "deterministic");
    if (c->r7_cam_keep != 0) refuse_i("r7_cam_keep", c->r7_cam_keep, "0");
    if (c->pres_keep != 1) refuse_i("pres_keep", c->pres_keep, "1");
    if (!g_lv && !c->replay_file[0]) refuse("replay_file", "(empty)", "a .rec path");
    /* v0.24.0-C1b2 (C1B-R2-SPEC.md §2.5, §4.1) */
    if (g_lv && !g_nt && c->replay_file[0]) refuse("replay_file", c->replay_file, "(empty)");
    if (g_lv && !g_nt && c->c1a_inject_set) refuse("c1a_setup_inject", "set", "absent|none");
    if (g_lv && !g_nt && !g_on && c->net_remote_side != 1) refuse_i("net_remote_side", c->net_remote_side, "1");   /* C2: the rblive refusal stays (D2); optA M2: rbonline sets it at ARM */
    if (g_lv && (c->live_cap < 600 || c->live_cap > 432000)) refuse_i("live_cap", c->live_cap, "600..432000");
    if (c->c1a_t1_rule < 0) refuse("c1a_t1_rule", "?", "ptrmap|heaptag");
    if (c->c1a_mm_dumps < 0 || c->c1a_mm_dumps > 16) refuse_i("c1a_mm_dumps", c->c1a_mm_dumps, "0..16");
    if (c->rb_zone != 0 && c->rb_zone != 1) refuse_i("rb_zone", c->rb_zone, "0|1");
    if (c->rb_zone_fight_mode < 0 || c->rb_zone_fight_mode > 255) refuse_i("rb_zone_fight_mode", c->rb_zone_fight_mode, "0..255");
    if (!g_nt && c->key_quit) refuse_i("hotkey_quit", c->key_quit, "0(rbnet only)");   /* C2 */
    if (c->rb_zone_entry_gap < c->net_max_rollback + c->net_delay + 2 || c->rb_zone_entry_gap > 120) {
        char nd[32]; snprintf(nd, sizeof nd, "%d..120", c->net_max_rollback + c->net_delay + 2); refuse_i("rb_zone_entry_gap", c->rb_zone_entry_gap, nd);
    }
    if (c->rb_zone_exit_margin < 8 || c->rb_zone_exit_margin > 240) refuse_i("rb_zone_exit_margin", c->rb_zone_exit_margin, "8..240");
    { struct { const char *k; int v; } hk[] = { { "hotkey_cycle", c->key_cycle }, { "hotkey_depth", c->key_depth }, { "hotkey_loopback_player", c->key_lbplayer },
                                               { "hotkey_camera", c->key_camera }, { "hotkey_l3", c->key_l3 }, { "hotkey_scrub", c->key_scrub },
                                               { "hotkey_passes", c->key_passes }, { "hotkey_anim", c->key_anim } };
      for (size_t i = 0; i < sizeof hk / sizeof hk[0]; i++) if (hk[i].v) refuse_i(hk[i].k, hk[i].v, "0"); }
    if (c->net_remote_side != 1 && c->net_remote_side != 2) refuse_i("net_remote_side", c->net_remote_side, "1|2");
    if (c->net_delay < 0 || c->net_delay > 4) refuse_i("net_delay", c->net_delay, "0..4");
    if (c->net_max_rollback < 1 || c->net_max_rollback > 7) refuse_i("net_max_rollback", c->net_max_rollback, "1..7");
    if (c->emu_latency_ms < 0 || c->emu_latency_ms > 250) refuse_i("emu_latency_ms", c->emu_latency_ms, "0..250");
    if (c->emu_jitter_ms < 0 || c->emu_jitter_ms > 100 || c->emu_jitter_ms > c->emu_latency_ms) refuse_i("emu_jitter_ms", c->emu_jitter_ms, "0..100_and_<=latency");
    if (c->emu_loss_pct < 0 || c->emu_loss_pct > 20) refuse_i("emu_loss_pct", c->emu_loss_pct, "0..20");
    if (c->emu_redundancy < 1 || c->emu_redundancy > 8) refuse_i("emu_redundancy", c->emu_redundancy, "1..8");
    if (c->emu_seed < 0 || c->emu_seed > 4294967295LL) refuse_i("emu_seed", c->emu_seed, "0..4294967295");
    if (!c->rb_state_mask) refuse("rb_state_slots", c->rb_state_bad[0] ? c->rb_state_bad : c->rb_state_slots_s, "hex_slot_list_multiples_of_8_in_0x8..0x130");
    if (c->c1a_t2_heap_bad) refuse("c1a_t2_heap", c->c1a_t2_heap_s, "raw|tag|map|follow");   /* C1B3: + map, follow */
    /* C1B3: (C1B-R3-SPEC.md §2.3.2, §2.6.2) rblive needs slot 0x0c0 in the state class (a mid-fight +0x0c0 is muted in predicted live passes and
     * delivered once from the confirmed pass); follow takes the recording's rule, so it exists only in the replay modes */
    if (g_lv && c->rb_state_mask && !((c->rb_state_mask >> (0xc0 / 8)) & 1) && !RBT(c->h_c0_refuse_off)) refuse("rb_state_slots", c->rb_state_slots_s, "includes_0xc0");   /* h_c0_refuse_off: harness only (KOCX) */
    if (g_lv && c->c1a_t2_follow) refuse("c1a_t2_heap", "follow", "raw|tag|map");
    { struct { const char *k; int v; } b[] = { { "rb_mt_tail", c->rb_mt_tail }, { "rb_pinguard", c->rb_pinguard }, { "rb_quarantine", c->rb_quarantine },
                                              { "rb_rollback", c->rb_rollback }, { "rb_skip_restore", c->rb_skip_restore }, { "rb_tick_limit", c->rb_tick_limit },
                                              { "rb_state_gate", c->rb_state_gate } };
      for (size_t i = 0; i < sizeof b / sizeof b[0]; i++) if (b[i].v != 0 && b[i].v != 1) refuse_i(b[i].k, b[i].v, "0|1"); }
    if (c->rb_test_layout_at < 0) refuse_i("rb_test_layout_at", c->rb_test_layout_at, ">=0");
    if (c->rb_state_quiet < 0 || c->rb_state_quiet > 600) refuse_i("rb_state_quiet", c->rb_state_quiet, "0..600");
    if (c->lipsync != 0 && c->lipsync != 1) refuse("lipsync", c->lipsync_s, "0|1");   /* LIPSYNC: refusal hook (C1B-R2-SPEC.md §3.6) */
    q1_cfg_check(c);   /* v0.29.2 (OPTA-M2-SPEC.md §16.9): the hybrid keys (no line, no refusal without them: every existing ini keeps its bytes) */
    pres_cfg();        /* v0.33.0 (OPTA-M2-SPEC.md §22.1): rb_pres_slots / rb_pres_repair (presentation: clamped, never refused) */
    g_cfg_ok = !g_refused[0];
    if (!g_cfg_ok) LOG("CFG C1B REFUSED key=%s: mode=%s refused, the plugin runs mode=off (title rb=REFUSED:%s); l4=2 and every other value outside the C1B-SPEC §2.10 set are never accepted\n", g_refused, g_mode_names[c->mode], g_refused);
    lipsync_set_active(g_cfg_ok && c->lipsync == 1);   /* LIPSYNC: CFG LIPSYNC on=1 */
    log_flush();
    return g_cfg_ok;
}

/* ------------------------------------------------------------------ battle state */
enum { RB_IDLE = 0, RB_OFF, RB_ON, RB_ABORTED, RB_ENDED };
static int g_st;
static char g_abort[48];
static EmuSched g_es; static int g_es_ok;
static uint32_t g_N, g_C, g_b, g_s, g_live_done, g_F;
static char g_bcause[48];            /* the cause of the current barrier */
static int g_seg; static uint32_t g_seg_start, g_seg_frames, g_seg_verified, g_seg_mp; static int g_rp_prev;
static uint64_t g_remote_changes, g_local_changes;
/* v0.23.1 review fixes */
static volatile uint32_t g_live_pre;  /* F2: the frame whose live (or segment) pass is running with its ring pre-snapshot taken (0 = none) */
static int g_post_want;              /* F4/F8: the previous step took (or tried to take) the post-snapshot the next fp_capture needs */
static int g_next_quiet;             /* F1/F7: the next live pass may forward state-class calls (pass=quiet_live) */
static uint64_t g_fpov, g_fpmiss, g_quiet_fwd, g_rpb_dup, g_dpin, g_dpin_over, g_dpin_hits;
/* v0.24.0-C1b2 */
static C1aIn *g_L; static uint8_t *g_Lok; static uint32_t g_Lcap;   /* rblive: L[f] = the record of frame f (UE's FrameIn of step f - D) */
static uint32_t g_live_prev_held; static uint64_t g_livein_bad; static int g_livein_lines;
static int g_tailT;                                                   /* rblive battle end: the tail is written ps=T */
static int g_pd_pending; static uint64_t g_pindeny; static int g_pindeny_lines;   /* S1BFIX G2 */
static int g_q1_pend;                /* v0.29.2: a Q1 walk failed before the battle's first step (RBABORT cause=q1_walk at battle start) */
#define ZI_CAP 1024
typedef struct { uint32_t z, e; } ZInt;                               /* zone frames [z, e); e = UINT32_MAX while the exit is open */
static ZInt g_zi[ZI_CAP]; static int g_nzi;
static uint32_t g_zlast_ev, g_zmode_prev, g_Fz; static int g_zrp_prev, g_zprev_ok;
static uint64_t g_zones, g_zone_frames;
typedef struct { uintptr_t addr; uint32_t len; int slot; uint32_t t; uint32_t rkey; } DpEnt;   /* F10: a deferred free of a pinned block */
#define DP_CAP 1024
static DpEnt g_dp[DP_CAP]; static int g_ndp;   /* addr 0 = a tombstone (compacted on the step thread) */

/* v0.33.0 (OPTA-M2-SPEC.md §22.1): the presentation slots (rb_pres_slots, default +0x120 / +0x128). Each pass of a frame records the calls of
 * these slots with their payload (the bytes UE reads, V: the UE implementations at 0x140400280 / 0x1403ffff0 copy them by value); the calls a
 * pass FORWARDED to UE are the frame's delivered set. At the frame's confirmation, every call of the confirmed (last) pass that UE never got is
 * delivered once, late (rb.c pres_confirm -> modes_lsn_deliver, guarded by L4 like a forwarded call); a delivered call that the confirmed pass
 * does not have is an orphan (UE cannot be told to undo it; for +0x120, a value channel keyed by its first 8 bytes, the confirmed value of
 * that key is sent back when the orphan is still UE's latest value of the key: the repair). Review fix M1 (§22.9): a +0x128 call goes out late
 * only when the confirmed pass made more +0x128 calls for the frame than UE got, and never for an effect UE already showed at another position. */
#define PRES_MAXC 16     /* calls of the presentation slots per pass (more: counted as over=, not recorded) */
#define PRES_MAXD 32     /* delivered calls per frame */
#define PRES_HIST 10     /* passes per frame kept for the delay counterfactual */
#define PRES_PAY 0x30
typedef struct { uint8_t slot, fwd, pad[6]; uint64_t h, idh; uint8_t pay[PRES_PAY]; } PresCall;   /* idh: review fix M1 (pres_idh) */
typedef struct { uint64_t h, key, idh; uint8_t slot, has_key, pad[6]; } PresDlv;
typedef struct {
    int nl, nd, nh, over;
    PresCall last[PRES_MAXC];                                  /* the frame's last pass (the confirmed one at confirmation) */
    PresDlv dlv[PRES_MAXD];                                    /* every call of the frame that reached UE (live passes; a muted resim delivers none) */
    struct { uint32_t s; int n; uint64_t h[PRES_MAXC]; } hist[PRES_HIST];   /* each pass: its step and its calls (the counterfactual) */
} PresFrame;

#define PEND_N 16
typedef struct {
    uint32_t f; int valid;
    uint32_t mode;                   /* v0.24.0-C1b2: the battle mode id (0x144846364) after the frame's last pass (zones) */
    C1aPass pass;                    /* diagnostics of the frame's LAST pass */
    char ifc[C1A_IFC_CAP]; uint32_t q;
    C1aIn used;                      /* the full input of the last pass */
    char ps;
    int pend_st, pend_slot;          /* state-class calls the last pass muted (0 = none) */
    uint16_t calls[LISTENER_NSLOTS], fwd[LISTENER_NSLOTS];
    uint64_t sfwd[LISTENER_NSLOTS];  /* v0.23.1 (review F9): bit n = the n-th state-class call of this slot in a pass of the frame already
                                      * reached UE (any pass); an RPB forwards only the calls whose bit is clear */
    PresFrame pres;                  /* v0.33.0 (OPTA-M2-SPEC.md §22.1) */
    uint16_t pres_late[LISTENER_NSLOTS], pres_sup[LISTENER_NSLOTS];   /* v0.33.0: delivered late / superseded at the confirmation (RBLEDGER) */
} Pend;
static Pend g_pend[PEND_N];

typedef struct { uint32_t f; int valid; Snap pre; FPatch fp; } RbEnt;
static RbEnt *g_ring;
static Snap g_post; static uint32_t g_post_f; static int g_post_ok;
static Snap g_scratch;

/* counters (per battle) */
static uint64_t g_live, g_rbk, g_dhist[16], g_dmax, g_cin, g_cst, g_cboth, g_rpb, g_refused_n, g_barriers, g_clears, g_pinq, g_pinq_bytes,
                g_pinguard, g_layout, g_stateev, g_statelost, g_ueskip, g_multi_ticks, g_def_ticks, g_tail_unconf, g_tail_mm, g_confirmed;
static uint64_t g_chain;
typedef struct { char c[32]; uint64_t n; } CauseN;
static CauseN g_bc[16]; static int g_nbc;
static uint64_t g_led_fwd[LISTENER_NSLOTS], g_led_final[LISTENER_NSLOTS], g_led_orphan[LISTENER_NSLOTS], g_led_missed[LISTENER_NSLOTS];
static uint64_t g_led_late[LISTENER_NSLOTS], g_led_sup[LISTENER_NSLOTS];   /* v0.33.0 (§22.1): the presentation slots' late deliveries / superseded calls */
static int g_lines_clamp, g_lines_pinq, g_lines_tick;
static uint64_t g_c0_mid; static int g_c0_late_max;   /* C1B3: RBSTATEEV slot=+0x0c0 with z=0 (mid-fight) and their largest late */
static double g_diag;                /* us of diagnostics in this step */

/* cost buckets: depth 0..7, 8 = rpb */
#define NBK 9
#define BK_CAP 16384
static float *g_bk_core[NBK], *g_bk_diag[NBK]; static uint32_t g_bk_n[NBK];

/* ticks */
static struct { uint64_t id; int steps, reruns, deferred; uint32_t s0; } g_tk;
static uint64_t g_tick_ctr;

/* pending barriers (any thread) */
typedef struct { int64_t f; char cause[40]; } PBar;
static PBar g_pbar[32]; static int g_npbar; static Spin g_pbar_lock;

/* the pin quarantine (C1B-SPEC §2.5) */
#define PINQ_CAP 4096
typedef RbQEnt QEnt;
static QEnt g_q[PINQ_CAP]; static int g_nq;
static volatile uint32_t g_pass_k;   /* the frame of the pass running on the step thread (0 = none) */
static DWORD g_step_tid;

/* the pass context the listener hook reads */
enum { PT_NONE = 0, PT_LIVE, PT_SEG, PT_CRESIM, PT_URESIM, PT_RPB, PT_STRAIGHT };
static struct { int type; uint32_t k, s; uint64_t c0; int final; uint16_t calls[LISTENER_NSLOTS], fwd[LISTENER_NSLOTS], stn[LISTENER_NSLOTS]; int muted, mslot;
                int quiet, quiet_fwd; void *pend;
                int pn, pover; PresCall pc[PRES_MAXC]; } g_pass;   /* v0.33.0: + the pass's presentation calls */

int rb_on(void) { return g_st == RB_ON; }
int rb_ss_stable(void) { return is_rbcore_mode(g_cfg.mode) && is_rbcore_mode(g_mode); }
uint32_t rb_layout_gen(void) { return ++g_gen_ctr; }
int rb_test_tick_reruns(void) { return g_tk.reruns; }
/* C1B3: (C1B-R3-SPEC.md §3.3) the current tick id and the step's position in it (1 = the tick's first step); generic, for diagnostics */
void rb_tick_info(uint64_t *tick, int *step_in_tick) { if (tick) *tick = g_tk.id; if (step_in_tick) *step_in_tick = g_tk.steps; }
/* LIPSYNC: v0.25 (C1B-R3-SPEC.md §3.3): the name of the pass running on the step thread, for LIPSYNCEV pass= (src/lipsync.h) */
const char *rb_lips_pass_name(void) {   /* LIPSYNC: */
    switch (g_pass.type) {   /* LIPSYNC: */
    case PT_LIVE: return g_pass.final ? "live_confirmed" : "live";   /* LIPSYNC: */
    case PT_SEG: return "seg"; case PT_CRESIM: return "resim_confirmed"; case PT_URESIM: return "resim";   /* LIPSYNC: */
    case PT_RPB: return "rpb"; case PT_STRAIGHT: return "straight"; default: return "none";   /* LIPSYNC: */
    }   /* LIPSYNC: */
}   /* LIPSYNC: */
/* C1B3: (C1B-R3-SPEC.md §2.3.1) harness-only h_old_c0count=1: modes.c lsn_pre counts +0x0c0 before rb_lsn_pre (the v0.24 order); 0 in the plugin */
int rb_t_old_c0count(void) { return RBT(g_cfg.h_old_c0count) ? 1 : 0; }

static int is_query(int slot) { return slot == 0x138 / 8 || slot == 0x140 / 8; }
static uint64_t rec_remote_word(const C1aIn *r) { return g_cfg.net_remote_side == 2 ? r->p2 : r->p1; }
/* v0.24.0-C1b2 (C1B-R2-SPEC.md §2.5): ONE truth function. rbreplay: the .rec; rblive: the live table L (NULL when not captured) */
static const C1aIn *rb_truth(uint32_t f) {
    if (g_lv) return g_L && f >= 1 && f < g_Lcap && g_Lok[f] ? &g_L[f] : NULL;   /* C2 rbnet: bit 1 = the local side, bit 2 = the peer's (f <= F has both) */
    const C1aFrame *r = c1a_rec_frame(f);
    return r ? &r->in : NULL;
}
/* v0.24.0-C1b2 §2.6: the lockstep zones */
static int zone_of(uint32_t f) {
    if (!g_cfg.rb_zone) return 0;
    for (int i = g_nzi - 1; i >= 0; i--) { if (f >= g_zi[i].z && f < g_zi[i].e) return 1; if (g_zi[i].e != UINT32_MAX && g_zi[i].e <= f) break; }
    return 0;
}

/* ------------------------------------------------------------------ helpers */
static void cause_add(const char *c) {
    for (int i = 0; i < g_nbc; i++) if (!strcmp(g_bc[i].c, c)) { g_bc[i].n++; return; }
    if (g_nbc < 16) { snprintf(g_bc[g_nbc].c, sizeof g_bc[0].c, "%s", c); g_bc[g_nbc].n = 1; g_nbc++; }
}
static int npending(void) { int n = 0; for (uint32_t f = g_C + 1; f <= g_live_done; f++) { Pend *p = &g_pend[f % PEND_N]; if (p->valid && p->f == f && p->pend_st) n++; } return n; }
static void barrier(uint32_t f, const char *cause, int log_always) {
    if (f > g_b || log_always) {
        g_barriers++; cause_add(cause);
        LOG("RBBARRIER f=%u cause=%s unconfirmed=%lld pending_state=%d zone=%d\n", f, cause, (long long)g_s - 1 - (long long)g_C, npending(), zone_of(f));
        if (g_nt && !zone_of(f) && (long long)g_s - 1 - (long long)g_C > 0) ns_fight_barrier(f, (long long)g_s - 1 - (long long)g_C);   /* C2 */
    }
    if (f >= g_b) { g_b = f; snprintf(g_bcause, sizeof g_bcause, "%s", cause); }
}
static void pbar_add(int64_t f, const char *cause) {
    spin_lock(&g_pbar_lock);
    if (g_npbar < 32) { g_pbar[g_npbar].f = f; snprintf(g_pbar[g_npbar].cause, sizeof g_pbar[0].cause, "%s", cause); g_npbar++; }
    spin_unlock(&g_pbar_lock);
}
static void pbar_apply(void) {
    PBar loc[32]; int n;
    spin_lock(&g_pbar_lock); n = g_npbar; memcpy(loc, g_pbar, sizeof(PBar) * (size_t)n); g_npbar = 0; spin_unlock(&g_pbar_lock);
    for (int i = 0; i < n; i++) barrier(loc[i].f < 0 ? g_s - 1 : (uint32_t)loc[i].f, loc[i].cause, 1);
}
static Pend *pend_at(uint32_t f) { Pend *p = &g_pend[f % PEND_N]; return p->valid && p->f == f ? p : NULL; }
static Pend *pend_new(uint32_t f) {
    Pend *p = &g_pend[f % PEND_N];
    if (p->valid && p->f != f) c1a_pass_drop(&p->pass);
    memset(p->calls, 0, sizeof p->calls); memset(p->fwd, 0, sizeof p->fwd); memset(p->sfwd, 0, sizeof p->sfwd);
    p->f = f; p->valid = 1; p->ps = '?'; p->pend_st = 0; p->pend_slot = 0; p->mode = 0;
    p->pres.nl = p->pres.nd = p->pres.nh = p->pres.over = 0; memset(p->pres_late, 0, sizeof p->pres_late); memset(p->pres_sup, 0, sizeof p->pres_sup);   /* v0.33.0 */
    if (p->pass.dump < -1 || p->pass.dump >= C1A_DUMP_SLOTS) p->pass.dump = -1;
    if (p->pass.mm < -1 || p->pass.mm > 8) p->pass.mm = -1;
    return p;
}
static RbEnt *ring_slot(uint32_t f) { return &g_ring[f % (uint32_t)g_R]; }
static RbEnt *ring_get(uint32_t f) { RbEnt *e = ring_slot(f); return e->valid && e->f == f ? e : NULL; }
static void c1a_from(C1aIn *c, const FrameIn *f) { c->p1 = f->p1; c->p2 = f->p2; memcpy(c->x, f->extra, 0x18); c->ff = f->flags; }
static void fi_from(FrameIn *f, const C1aIn *c) { f->p1 = c->p1; f->p2 = c->p2; memcpy(f->extra, c->x, 0x18); f->flags = (uint8_t)(c->ff & 7); }
static int used_ok(uint32_t f, const C1aIn *u) {
    const C1aIn *r = rb_truth(f);
    if (!r) return 1;
    if (g_on) return g_cfg.net_remote_side == 2 ? u->p2 == r->p2 : u->p1 == r->p1;   /* optA M2 (D6): online only the remote word is compared */
    if (g_cfg.net_remote_side == 2) return u->p2 == r->p2;
    return u->p1 == r->p1 && !memcmp(u->x, r->x, 0x18);
}
static uint64_t used_remote_word(uint32_t f) {   /* the remote word frame f used in its last pass (the recorded one when unknown here) */
    if (!f) return 0;
    Pend *p = pend_at(f);
    if (p) return g_cfg.net_remote_side == 2 ? p->used.p2 : p->used.p1;
    const C1aIn *r = rb_truth(f);
    return r ? rec_remote_word(r) : 0;
}
/* the inputs of frame k under frontier F (C1B-SPEC §2.3): local side and ff recorded; the remote record recorded when k <= F, else
 * predicted from rec(F) (held repeated, pressed recomputed against the previous used word); x[0..8) = the predicted next word */
static void make_inputs(uint32_t k, uint32_t F, FrameIn *in) {
    const C1aIn *r = rb_truth(k);
    memset(in, 0, sizeof *in);
    if (r) fi_from(in, r);
    if (k <= F) return;
    if (g_on) {   /* optA M2 (spec §5.2): the remote word of an unconfirmed frame = the stock rule on the last confirmed raw (pressed 0) with the
                   * frame's own live masks; the local word and the third block are the live capture (L[k] bit 1), reused in every resim */
        uint64_t w = rbo_fetch_pred_word(k);
        if (g_cfg.net_remote_side == 2) { in->p2 = w; in->flags |= 2; } else { in->p1 = w; in->flags |= 1; }
        return;
    }
    const C1aIn *rF = F ? rb_truth(F) : NULL;
    uint64_t base = rF ? rec_remote_word(rF) : 0, prev = used_remote_word(k - 1);
    uint64_t w = emu_predict(prev, base);
    if (g_cfg.net_remote_side == 2) { in->p2 = w; in->flags |= 2; return; }
    /* C1B3: (C1B-R3-SPEC.md §2.2, Z2) the third block. x(F)[0..8) = p1(F+1) is carried by record F: frame F+1's remote word is known exactly,
     * later frames are predicted from it, and the analog tail x[8..0x18) holds the last known one. A wrong guess is only a misprediction
     * (used_ok is unchanged). h_old_xpred=1 (harness only): the v0.24 predictor (tail zero, word from rec(F)) = the negative control. */
    uint64_t nx = 0; if (rF) memcpy(&nx, rF->x, 8);
    int oldp = RBT(g_cfg.h_old_xpred);
    uint64_t b2 = oldp ? base : nx;
    w = (rF && k == F + 1 && !oldp) ? nx : emu_predict(prev, b2);
    in->p1 = w; in->flags |= 1;
    uint64_t xw = emu_predict(w, b2);
    memset(in->extra, 0, 0x18); memcpy(in->extra, &xw, 8);
    if (rF && !oldp) memcpy(in->extra + 8, rF->x + 8, 0x10);   /* C1B3: hold the last known analog tail */
}

/* v0.23.1 (review F1/F7): rb_state_quiet. 1 = the remote record (word + x for remote side 1, the word for side 2) is the same on every known
 * frame F-N+1..F, every predicted frame F+1..f-1 used exactly that record, and the live input of f carries it too. Uses only what the
 * receiver knows at step f (records <= F, its own predictions): no look at the future. */
static int rec_remote_eq(const C1aIn *a, const C1aIn *b) {
    if (g_cfg.net_remote_side == 2) return a->p2 == b->p2;
    return a->p1 == b->p1 && !memcmp(a->x, b->x, 0x18);
}
static int remote_quiet(uint32_t F, uint32_t f, const FrameIn *live) {
    uint32_t N = (uint32_t)g_cfg.rb_state_quiet;
    if (!N || F < N || F >= f) return 0;
    const C1aIn *rF = rb_truth(F);
    if (!rF) return 0;
    for (uint32_t j = F - N + 1; j < F; j++) { const C1aIn *r = rb_truth(j); if (!r || !rec_remote_eq(r, rF)) return 0; }
    for (uint32_t j = F + 1; j < f; j++) { Pend *p = pend_at(j); if (!p || !rec_remote_eq(&p->used, rF)) return 0; }
    C1aIn li; c1a_from(&li, live);
    return rec_remote_eq(&li, rF);
}

/* ------------------------------------------------------------------ v0.33.0 (OPTA-M2-SPEC.md §22.1): the presentation slots (missed effects)
 * The payload of each slot is the bytes UE's implementation reads, nothing else (V, the installed exe, design_v0330/scratch_build/lsn_sites.py):
 *   +0x120 (UE 0x140400280): u32 [rdx], u32 [rdx+4], f32 [rdx+8] -> a 12-byte struct broadcast on the native multicast delegate at listener+0xf58
 *          (0x141d38300). The core's two call sites (0x140379988: a move-script opcode that writes obj+0x4c+4*i = value first; 0x14037e790: a loop
 *          that re-sends all eight values) pass {player byte obj+0x23c, index 0..7, value}: a value channel, keyed by its first 8 bytes.
 *   +0x128 (UE 0x1403ffff0): u32 [rdx], the vector at rdx+0x10 (0x140455910 reads 12 bytes), u32 [rdx+0x20], [rdx+0x24], [rdx+0x28], u8 [rdx+0x2c]
 *          -> the delegate at listener+0xfc8. Core call sites 0x1403418ea, 0x140346735 (a positioned one-shot: no key). [rdx+4..0x10) and [rdx+0x1c]
 *          are not read by UE (stack garbage at the call sites): never compared, never passed on (zero in the late copy).
 * Neither UE implementation re-enters the core or writes core state (V: no L4W / L4REENTRY line naming +0x120 / +0x128 in any of the 117 logs of
 * sc6rb\logs); the late delivery still runs inside L4's guard, so a write would be reverted and logged like a forwarded call's. */
typedef struct { int slot, n, key; uint8_t off[3], len[3]; } PresSpec;
static const PresSpec k_pres_spec[] = {
    { 0x120 / 8, 1, 8, { 0, 0, 0 }, { 12, 0, 0 } },
    { 0x128 / 8, 3, 0, { 0, 0x10, 0x20 }, { 4, 12, 13 } },
};
#define PRES_VERIFIED_MASK ((1ULL << (0x120 / 8)) | (1ULL << (0x128 / 8)))
static uint64_t g_pres_mask;        /* rb_pres_slots (default +0x120,+0x128; "none" = v0.32.0: nothing recorded, nothing delivered late) */
static int g_pres_repair = 1;       /* rb_pres_repair: the +0x120 orphan repair */
#ifdef SC6RB_HARNESS
static int g_pres_nol4;             /* harness-only control (h_rb_pres_nol4=1, XFXW): the late call without L4's guard (RBT: the constant 0 in the plugin) */
static int g_pres_128old;           /* review fix M1, harness-only control (h_rb_pres_128old=1, XFX128 / XGFX128): v0.33.0's first +0x128 rule (every
                                     * confirmed call without a byte-identical delivered partner went out late: a moved effect was shown twice) */
static int g_pres_128noid;          /* review fix M1, harness-only control (h_rb_pres_128noid=1, XFX128ID): the count gate without the identity pairing */
#endif
static const PresSpec *pres_spec(int slot) { for (size_t i = 0; i < sizeof k_pres_spec / sizeof k_pres_spec[0]; i++) if (k_pres_spec[i].slot == slot) return &k_pres_spec[i]; return NULL; }
static uint64_t pres_hash(int slot, const uint8_t *pay) {
    uint64_t h = 0xcbf29ce484222325ULL;
    h = (h ^ (uint8_t)slot) * 0x100000001b3ULL;
    for (int i = 0; i < PRES_PAY; i++) h = (h ^ pay[i]) * 0x100000001b3ULL;
    return h;
}
static uint64_t pres_key(const PresCall *c) { uint64_t k; memcpy(&k, c->pay, 8); return k; }
/* v0.33.0 review fix M1 (OPTA-M2-SPEC.md §22.9): a +0x128 call's identity = the bytes UE reads WITHOUT the position (rdx+0x10..+0x1b): the player
 * dword, +0x20 / +0x24 (r8d / r9d at 0x1403418ea; 0 / 3 at 0x140346735), +0x28 (0x1403417b0 of the flag word) and the flag byte +0x2c. The same
 * effect a little off after a misprediction keeps its identity. +0x120 (a keyed value channel): the full hash. */
static uint64_t pres_idh(int slot, const uint8_t *pay) {
    if (slot != 0x128 / 8) return pres_hash(slot, pay);
    uint8_t m[PRES_PAY]; memcpy(m, pay, PRES_PAY); memset(m + 0x10, 0, 12);
    return pres_hash(slot, m);
}
static const char *pres_rule128(void) { return RBT(g_pres_128old) ? "content(harness control)" : RBT(g_pres_128noid) ? "count(harness control)" : "count+id"; }
/* per battle (step thread only) */
static uint64_t g_pres_late, g_pres_sup, g_pres_rep, g_pres_unrep, g_pres_orph, g_pres_tail, g_pres_over, g_pres_badptr, g_pres_fail, g_pres_calls, g_pres_batches;
static uint64_t g_pres_l128, g_pres_moved, g_pres_held, g_pres_dbl, g_pres_cm;   /* review fix M1: +0x128 late / moved / withheld / possible-double frames /
                                                                                  * content-mismatch frames (RBPRES128) */
static uint64_t g_pres_cf[9], g_pres_lat_sum, g_pres_lat_hist[16]; static uint32_t g_pres_lat_max;
static double g_pres_us, g_pres_us_max;
/* the value channel (+0x120): UE's latest value per key (from which frame's pass, and the payload) and the confirmed timeline's latest value per key */
#define PRES_KEYS 64
typedef struct { uint64_t key; uint32_t f; int valid; uint8_t pay[PRES_PAY]; } PresKey;
static PresKey g_pres_ue[PRES_KEYS], g_pres_conf[PRES_KEYS];
static PresKey *pres_key_get(PresKey *t, uint64_t key, int make) {
    for (int i = 0; i < PRES_KEYS; i++) if (t[i].valid && t[i].key == key) return &t[i];
    if (!make) return NULL;
    int o = 0; for (int i = 0; i < PRES_KEYS; i++) { if (!t[i].valid) { o = i; break; } if (t[i].f < t[o].f) o = i; }   /* a free entry, else the oldest */
    memset(&t[o], 0, sizeof t[o]); t[o].key = key; t[o].valid = 1;
    return &t[o];
}
/* the late deliveries of one confirm_upto (flushed together: one batch per step) */
#define PRESQ_N 64
static uint8_t g_presq_slot[PRESQ_N], g_presq_pay[PRESQ_N][PRES_PAY]; static int g_npresq;
static void pres_flush(void) {
    if (!g_npresq) return;
    uint64_t t0 = qpc();
    int done = modes_lsn_deliver(g_npresq, g_presq_slot, g_presq_pay, !RBT(g_pres_nol4));
    double us = qpc_us(qpc() - t0);
    g_pres_us += us; if (us > g_pres_us_max) g_pres_us_max = us;
    g_pres_batches++; g_pres_calls += (uint64_t)(done > 0 ? done : 0);
    if (done != g_npresq) { g_pres_fail += (uint64_t)(g_npresq - (done > 0 ? done : 0)); if (g_pres_fail <= 3) LOG("RBPRES deliver FAIL n=%d done=%d (UE's listener was not reachable between passes: those calls were dropped)\n", g_npresq, done); }
    g_npresq = 0;
}
static void pres_queue(int slot, const uint8_t *pay, uint32_t lat) {
    if (g_npresq == PRESQ_N) pres_flush();
    g_presq_slot[g_npresq] = (uint8_t)slot; memcpy(g_presq_pay[g_npresq], pay, PRES_PAY); g_npresq++;
    g_pres_late++; g_pres_lat_sum += lat; if (lat > g_pres_lat_max) g_pres_lat_max = lat; g_pres_lat_hist[lat < 15 ? lat : 15]++;
}
/* a call of a presentation slot inside a pass (step thread): its payload, whether this pass forwards it to UE */
static void pres_note(int slot, uint64_t rdx, int fwd) {
    const PresSpec *sp = pres_spec(slot);
    if (!sp) return;
    if (g_pass.pn >= PRES_MAXC) { g_pass.pover++; return; }
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();   /* both core call sites pass a local of the step's stack (V); anything else is not read */
    if (rdx < (uint64_t)(uintptr_t)tib->StackLimit || rdx + PRES_PAY > (uint64_t)(uintptr_t)tib->StackBase) { g_pres_badptr++; return; }
    PresCall *c = &g_pass.pc[g_pass.pn++];
    memset(c, 0, sizeof *c); c->slot = (uint8_t)slot; c->fwd = (uint8_t)(fwd != 0);
    for (int i = 0; i < sp->n; i++) memcpy(c->pay + sp->off[i], (const void *)(uintptr_t)(rdx + sp->off[i]), sp->len[i]);
    c->h = pres_hash(slot, c->pay);
    c->idh = pres_idh(slot, c->pay);   /* review fix M1 */
}
/* after a recorded (non-RPB) pass of frame k: the frame's last calls, its delivered set, the pass in the history; UE's latest value per key */
static void pres_record(Pend *p, uint32_t k) {
    PresFrame *P = &p->pres;
    if (!g_pres_mask) return;
    P->nl = g_pass.pn; memcpy(P->last, g_pass.pc, sizeof(PresCall) * (size_t)g_pass.pn);
    P->over += g_pass.pover; g_pres_over += (uint64_t)g_pass.pover;
    if (P->nh == PRES_HIST) { memmove(&P->hist[0], &P->hist[1], sizeof P->hist[0] * (PRES_HIST - 1)); P->nh--; }
    P->hist[P->nh].s = g_pass.s; P->hist[P->nh].n = g_pass.pn;
    for (int i = 0; i < g_pass.pn; i++) P->hist[P->nh].h[i] = g_pass.pc[i].h;
    P->nh++;
    for (int i = 0; i < g_pass.pn; i++) {
        const PresCall *c = &g_pass.pc[i];
        if (!c->fwd) continue;
        const PresSpec *sp = pres_spec(c->slot);
        if (P->nd < PRES_MAXD) { PresDlv *d = &P->dlv[P->nd++]; d->h = c->h; d->idh = c->idh; d->slot = c->slot; d->has_key = sp && sp->key; d->key = d->has_key ? pres_key(c) : 0; }
        if (sp && sp->key) { PresKey *u = pres_key_get(g_pres_ue, pres_key(c), 1); u->f = k; memcpy(u->pay, c->pay, PRES_PAY); }
    }
}
/* frame f is confirmed at step s (its last pass is the timeline). Missed calls go into the batch (once), orphans are counted (and repaired for a key).
 * v0.33.0 review fix M1 (OPTA-M2-SPEC.md §22.9): +0x128 (a positioned one-shot that cannot be undone) is matched in three tiers:
 *   1. the same call (every byte UE reads): UE has it;
 *   2. the same effect at another position (pres_idh): UE showed it once, at the guess's position: not sent again (`moved`);
 *   3. the count gate: of the rest, at most (the confirmed pass's +0x128 calls - the +0x128 calls UE got for this frame) go out late; the others are
 *      `withheld` (UE already showed as many +0x128 effects for this frame as the timeline has: v0.32.0's behavior for them).
 * So UE never gets more +0x128 calls for a frame than max(what it got live, what the confirmed pass made): no effect twice. v0.33.0's first rule
 * (tier 1 only) delivered a content-mismatched call next to its live twin: that is the harness control h_rb_pres_128old (XFX128, XGFX128). */
static void pres_confirm(uint32_t f, uint32_t s, Pend *p) {
    PresFrame *P = &p->pres;
    const int S8 = 0x128 / 8;
    uint8_t used[PRES_MAXD]; memset(used, 0, sizeof used);   /* a delivered call: 1 = tier 1 (the same call), 2 = tier 2 (the same +0x128 effect, moved) */
    uint8_t cm[PRES_MAXC]; memset(cm, 0, sizeof cm);         /* a confirmed call: the same */
    for (int i = 0; i < P->nl; i++)
        for (int j = 0; j < P->nd; j++) if (!used[j] && P->dlv[j].h == P->last[i].h) { used[j] = 1; cm[i] = 1; break; }
    { int nc = 0, nd = 0;   /* a content-mismatch frame: a +0x128 call on each side without a byte-identical partner (v0.33.0's first rule doubled them) */
      for (int i = 0; i < P->nl; i++) if (!cm[i] && P->last[i].slot == S8) nc++;
      for (int j = 0; j < P->nd; j++) if (!used[j] && P->dlv[j].slot == S8) nd++;
      if (nc && nd) g_pres_cm++; }
    int old128 = RBT(g_pres_128old) != 0, id128 = !old128 && !RBT(g_pres_128noid);
    if (id128)
        for (int i = 0; i < P->nl; i++) {
            if (cm[i] || P->last[i].slot != S8) continue;
            for (int j = 0; j < P->nd; j++) if (!used[j] && P->dlv[j].slot == S8 && P->dlv[j].idh == P->last[i].idh) { used[j] = 2; cm[i] = 2; g_pres_moved++; break; }
        }
    int allow8 = (int)p->calls[S8] - (int)p->fwd[S8], late8 = 0;   /* the count gate: what the confirmed pass made beyond what UE got (any pass) */
    if (allow8 < 0) allow8 = 0;
    for (int i = 0; i < P->nl; i++) {
        if (cm[i]) continue;
        const PresCall *c = &P->last[i];
        const PresSpec *sp = pres_spec(c->slot);
        if (sp && sp->key) {
            PresKey *u = pres_key_get(g_pres_ue, pres_key(c), 0);
            if (u && u->f > f) { p->pres_sup[c->slot]++; g_pres_sup++; continue; }   /* UE already holds a newer frame's value of this key */
        }
        if (c->slot == S8) {
            if (!old128 && late8 >= allow8) { g_pres_held++; continue; }   /* UE already showed as many +0x128 effects for this frame */
            late8++;
        }
        pres_queue(c->slot, c->pay, s >= f ? s - f : 0);
        p->pres_late[c->slot]++;
        if (sp && sp->key) { PresKey *u = pres_key_get(g_pres_ue, pres_key(c), 1); u->f = f; memcpy(u->pay, c->pay, PRES_PAY); }
    }
    if (late8) {   /* a possible double: a +0x128 delivered late in a frame where UE also holds an unpaired +0x128 guess (RBPRES128 dbl_frames) */
        g_pres_l128 += (uint64_t)late8;
        for (int j = 0; j < P->nd; j++) if (!used[j] && P->dlv[j].slot == S8) { g_pres_dbl++; break; }
    }
    for (int i = 0; i < P->nl; i++) {   /* the confirmed timeline's value of each key, in the pass's order */
        const PresSpec *sp = pres_spec(P->last[i].slot);
        if (sp && sp->key) { PresKey *q = pres_key_get(g_pres_conf, pres_key(&P->last[i]), 1); q->f = f; memcpy(q->pay, P->last[i].pay, PRES_PAY); }
    }
    for (int j = 0; j < P->nd; j++) {
        if (used[j] == 1) continue;   /* review fix M1: a moved +0x128 (used 2) stays an orphan by content (UE shows it at the guess's position) */
        g_pres_orph++;
        if (!P->dlv[j].has_key || !g_pres_repair) continue;
        int in_final = 0;
        for (int i = 0; i < P->nl && !in_final; i++) { const PresSpec *sp = pres_spec(P->last[i].slot); if (sp && sp->key && pres_key(&P->last[i]) == P->dlv[j].key) in_final = 1; }
        if (in_final) continue;   /* the confirmed pass set this key itself (matched or delivered above) */
        PresKey *u = pres_key_get(g_pres_ue, P->dlv[j].key, 0);
        if (!u || u->f != f) continue;   /* a newer frame's call already replaced the orphan's value in UE (that frame is checked at its own confirmation) */
        PresKey *q = pres_key_get(g_pres_conf, P->dlv[j].key, 0);
        if (!q) { g_pres_unrep++; continue; }   /* no confirmed value of this key yet this battle: nothing to send back */
        if (!memcmp(q->pay, u->pay, PRES_PAY)) continue;   /* UE already holds the confirmed value */
        if (g_npresq == PRESQ_N) pres_flush();
        g_presq_slot[g_npresq] = P->dlv[j].slot; memcpy(g_presq_pay[g_npresq], q->pay, PRES_PAY); g_npresq++;
        g_pres_rep++; memcpy(u->pay, q->pay, PRES_PAY);
    }
    /* the counterfactual (information): a delivery delay of N frames would have handed UE frame f's latest pass at step f + N (when f was still
     * unconfirmed then) instead of its live pass; the calls of that pass the confirmed pass does not have are the orphans of delay N */
    for (int N = 0; N <= 8; N++) {
        if (s <= f + (uint32_t)N) continue;   /* confirmed by then: delay N delivers the confirmed pass (no orphan) */
        int hi = -1; for (int k = 0; k < P->nh; k++) if (P->hist[k].s <= f + (uint32_t)N) hi = k;
        if (hi < 0) continue;
        uint8_t u2[PRES_MAXC]; memset(u2, 0, sizeof u2);
        for (int a = 0; a < P->hist[hi].n; a++) {
            int m = 0; for (int i = 0; i < P->nl; i++) if (!u2[i] && P->last[i].h == P->hist[hi].h[a]) { u2[i] = 1; m = 1; break; }
            if (!m) g_pres_cf[N]++;
        }
    }
}
static void pres_reset(void) {
    g_pres_late = g_pres_sup = g_pres_rep = g_pres_unrep = g_pres_orph = g_pres_tail = g_pres_over = g_pres_badptr = g_pres_fail = g_pres_calls = g_pres_batches = 0;
    g_pres_l128 = g_pres_moved = g_pres_held = g_pres_dbl = g_pres_cm = 0;   /* review fix M1 */
    memset(g_pres_cf, 0, sizeof g_pres_cf); g_pres_lat_sum = 0; memset(g_pres_lat_hist, 0, sizeof g_pres_lat_hist); g_pres_lat_max = 0;
    g_pres_us = g_pres_us_max = 0; g_npresq = 0;
    memset(g_pres_ue, 0, sizeof g_pres_ue); memset(g_pres_conf, 0, sizeof g_pres_conf);
}
static void pres_slots_str(uint64_t m, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 1; i < LISTENER_NSLOTS; i++) if ((m >> i) & 1) { int k = snprintf(out + o, cap - o, "%s0x%03x", o ? "," : "", i * 8); if (k > 0 && (size_t)k < cap - o) o += (size_t)k; }
    if (!o) snprintf(out, cap, "none");
}
/* rb_cfg_check: rb_pres_slots / rb_pres_repair (presentation keys: outside every hash; a bad value is clamped and said so, never refused) */
static void pres_cfg(void) {
    char b[160]; cfg_gets("rb_pres_slots", "0x120,0x128", b, sizeof b);
    uint64_t m = 0; int bad = 0;
    if (_stricmp(b, "none") && strcmp(b, "0")) {
        const char *q = b;
        while (*q) {
            while (*q == ' ' || *q == ',' || *q == '+') q++;
            if (!*q) break;
            char *e; unsigned long v = strtoul(q, &e, 16);
            if (e == q) { bad = 1; break; }
            q = e;
            if (v % 8 || v / 8 >= LISTENER_NSLOTS || !((PRES_VERIFIED_MASK >> (v / 8)) & 1)) { bad = 1; continue; }   /* only slots whose payload layout is verified */
            m |= 1ULL << (v / 8);
        }
    }
    m &= ~g_cfg.rb_state_mask;   /* the state class keeps its own delivery (RPB / resim_confirmed) */
    g_pres_mask = m;
    g_pres_repair = cfg_geti("rb_pres_repair", 1) != 0;
#ifdef SC6RB_HARNESS
    g_pres_nol4 = cfg_geti("h_rb_pres_nol4", 0);
    g_pres_128old = cfg_geti("h_rb_pres_128old", 0);
    g_pres_128noid = cfg_geti("h_rb_pres_128noid", 0);
#endif
    char sl[96]; pres_slots_str(m, sl, sizeof sl);
    LOG("CFG RBPRES slots=%s repair=%d%s%s (v0.33.0: the missed presentation notifications are delivered once from the confirmed pass; outside every hash)\n", sl, g_pres_repair,
        bad ? " CLAMPED(ini value has a slot without a verified payload layout: only 0x120 and 0x128 are accepted)" : "", RBT(g_pres_nol4) ? " h_rb_pres_nol4=1(harness control XFXW: no L4 guard)" : "");
    if (RBT(g_pres_128old) || RBT(g_pres_128noid)) LOG("CFG RBPRES harness control: rule128=%s\n", pres_rule128());
}
static void pres_report(void) {
    if (!g_pres_mask) return;
    uint64_t n = 0, k = 0, p50 = 0; for (int i = 0; i < 16; i++) n += g_pres_lat_hist[i];
    for (int i = 0; i < 16 && n; i++) { k += g_pres_lat_hist[i]; if (2 * k >= n) { p50 = (uint64_t)i; break; } }
    char sl[96]; pres_slots_str(g_pres_mask, sl, sizeof sl);
    LOG("RBPRES b=%llu slots=%s late=%llu superseded=%llu repaired=%llu unrepaired=%llu orphan=%llu tail=%llu over=%llu badptr=%llu fail=%llu calls=%llu batches=%llu us=%.0f us_max=%.0f "
        "late_lat_p50=%llu late_lat_max=%u late_lat_avg=%.2f cf_orphan=0:%llu,1:%llu,2:%llu,3:%llu,4:%llu,5:%llu,6:%llu,7:%llu,8:%llu\n", (unsigned long long)g_bat, sl,
        (unsigned long long)g_pres_late, (unsigned long long)g_pres_sup, (unsigned long long)g_pres_rep, (unsigned long long)g_pres_unrep, (unsigned long long)g_pres_orph,
        (unsigned long long)g_pres_tail, (unsigned long long)g_pres_over, (unsigned long long)g_pres_badptr, (unsigned long long)g_pres_fail, (unsigned long long)g_pres_calls,
        (unsigned long long)g_pres_batches, g_pres_us, g_pres_us_max, (unsigned long long)p50, g_pres_lat_max, g_pres_late ? (double)g_pres_lat_sum / (double)g_pres_late : 0.0,
        (unsigned long long)g_pres_cf[0], (unsigned long long)g_pres_cf[1], (unsigned long long)g_pres_cf[2], (unsigned long long)g_pres_cf[3], (unsigned long long)g_pres_cf[4],
        (unsigned long long)g_pres_cf[5], (unsigned long long)g_pres_cf[6], (unsigned long long)g_pres_cf[7], (unsigned long long)g_pres_cf[8]);
    /* review fix M1 (OPTA-M2-SPEC.md §22.9): +0x128 on its own line (the RBPRES field list is unchanged). late = delivered late; moved = the same effect
     * (pres_idh) at another position, shown once at the guess's; withheld = held back by the count gate (UE already showed as many +0x128 effects for
     * that frame); dbl_frames = frames with a late +0x128 AND an unpaired +0x128 guess (a possible double); cm_frames = frames with a +0x128 call on
     * each side without a byte-identical partner (what v0.33.0's first rule delivered twice; rule-independent: the game's own rate) */
    if ((g_pres_mask >> (0x128 / 8)) & 1)
        LOG("RBPRES128 b=%llu rule=%s late=%llu moved=%llu withheld=%llu dbl_frames=%llu cm_frames=%llu\n", (unsigned long long)g_bat, pres_rule128(),
            (unsigned long long)g_pres_l128, (unsigned long long)g_pres_moved, (unsigned long long)g_pres_held, (unsigned long long)g_pres_dbl, (unsigned long long)g_pres_cm);
}

/* ------------------------------------------------------------------ the listener hook (modes.c lsn_pre, step thread, inside a pass) */
void rb_straight_mark(uint32_t f) {
    if (f) { memset(&g_pass, 0, sizeof g_pass); g_pass.type = PT_STRAIGHT; g_pass.k = f; g_pass.s = f; } else g_pass.type = PT_NONE;
}
int rb_lsn_pre(int slot, uint64_t rdx) {
    if (!g_pass.type || slot <= 0 || slot >= LISTENER_NSLOTS || is_query(slot)) return 0;
    if (GetCurrentThreadId() != g_step_tid && g_pass.type != PT_STRAIGHT) return 0;
    g_pass.calls[slot]++;
    int st = (int)((g_cfg.rb_state_mask >> slot) & 1);
    int fwd_fn = lp_family.fn[slot] != NULL;
    if (((g_pres_mask >> slot) & 1) && g_pass.type != PT_STRAIGHT && g_pass.type != PT_RPB) pres_note(slot, rdx, fwd_fn);   /* v0.33.0 (§22.1) */
    if (!st) { if (fwd_fn) { g_pass.fwd[slot]++; g_step_fwd++; } return 0; }
    const char *pn = NULL;
    switch (g_pass.type) {
    case PT_SEG: pn = "seg"; break;
    case PT_LIVE:
        if (g_pass.final) pn = "live_confirmed";
        else if (modes_lsn_c0_calls() != g_pass.c0) pn = "barrier_live";
        else if (g_pass.quiet) { pn = "quiet_live"; g_pass.quiet_fwd++; }   /* v0.23.1 rb_state_quiet: the step sets RBBARRIER cause=state_quiet at k */
        else if (!g_cfg.rb_state_gate) pn = "live_ungated";   /* control rb_state_gate=0 */
        break;
    case PT_CRESIM: pn = "resim_confirmed"; break;
    case PT_RPB: pn = "rpb"; break;
    case PT_STRAIGHT: pn = "straight"; break;
    default: break;
    }
    int n = g_pass.stn[slot]++;
    Pend *pp = (Pend *)g_pass.pend;
    if (g_pass.type == PT_RPB && pp && n < 64 && ((pp->sfwd[slot] >> n) & 1) && !RBT(g_rbt_rpb_dup)) {
        /* v0.23.1 (review F9): this (frame, slot, n) already reached UE (barrier_live / quiet_live in the frame's live pass): the RPB
         * re-run mutes it, so UE gets every state-class call of the frame exactly once */
        g_rpb_dup++;
        LOG("RBSTATEDUP f=%u slot=+0x%03x n=%d pass=rpb s=%u (already forwarded: muted)\n", g_pass.k, slot * 8, n, g_pass.s);
        return 1;
    }
    if (pn && fwd_fn) {
        if (pp && n < 64) pp->sfwd[slot] |= 1ULL << n;
        g_pass.fwd[slot]++; g_stateev++; g_step_fwd++;
        int zk = zone_of(g_pass.k), lt = (int)g_pass.s - (int)g_pass.k;   /* C1B3: + z= (the frame's zone membership); c0_mid / c0_late_max */
        if (slot == 0xc0 / 8 && !zk && g_pass.type != PT_STRAIGHT) { g_c0_mid++; if (lt > g_c0_late_max) g_c0_late_max = lt; }
        LOG("RBSTATEEV f=%u slot=+0x%03x n=%d pass=%s s=%u late=%d z=%d\n", g_pass.k, slot * 8, n, pn, g_pass.s, lt, zk);
        return 0;
    }
    if (g_pass.type == PT_STRAIGHT) return 0;
    if (!g_pass.muted) g_pass.mslot = slot;
    g_pass.muted++;
    return 1;   /* muted: UE never sees it; the frame is state-pending */
}

/* v0.23.1 (review F3 iii): the slot of a quarantined / deferred-freed block, re-resolved by address when the cached slot is stale (an
 * ss_build rebuild renumbers every slot; a rescan may re-collect the block). rkey caches "not found under this layout". Pin lock held. */
static int slot_resolve(SnapSet *ss, uintptr_t addr, int *slot, uint32_t *rkey) {
    int s = *slot;
    if (s >= 0 && s < ss->npin && ss->pins[s].addr == addr && ss->pstate[s] == SPIN_ALIVE) return s;
    uint32_t key = (ss->gen * 0x9E3779B1u) ^ (uint32_t)ss->npin ^ 0x80000000u;
    if (s == -1 && *rkey == key) return -1;
    s = snapset_find_alive_start(ss, addr);
    *slot = s; *rkey = key;
    return s;
}
/* the pins' visibility for a pass of frame k (or the step entry of frame k):
 *   a quarantined pin (an outside free held back, C1B-SPEC §2.5): hidden iff k >= q (C1a removes the pin at the entry of step q);
 *   v0.23.1 (review F10) a pin whose block a pass of frame t freed (the deferred free, not yet confirmed): hidden iff k > t (C1a confirms
 *   the free right after the pass of t and removes the pin at the entry of step t+1). Pin lock held inside. */
static void q_visibility(uint32_t k) {
    SnapSet *ss = modes_ssw(); Spin *pl = modes_pin_lock(); int ch = 0;
    spin_lock(pl);
    for (int i = 0; i < g_ndp; i++) {   /* first, so a quarantine flag on the same block (never expected) wins */
        if (!g_dp[i].addr) continue;
        int sl = slot_resolve(ss, g_dp[i].addr, &g_dp[i].slot, &g_dp[i].rkey);
        if (sl >= 0) ch += snapset_pin_flags(ss, sl, (uint8_t)(k > g_dp[i].t ? 1 : 0));
    }
    for (int i = 0; i < g_nq; i++) {
        int sl = slot_resolve(ss, g_q[i].addr, &g_q[i].slot, &g_q[i].rkey);
        if (sl >= 0) ch += snapset_pin_flags(ss, sl, (uint8_t)(k >= g_q[i].q ? 3 : 2));
    }
    if (ch) snapset_rebuild_visible(ss);
    spin_unlock(pl);
}
void rb_step_entry_pins(void) { if (g_st == RB_ON && (g_nq || g_ndp)) q_visibility(c1a_next_f()); }

/* ------------------------------------------------------------------ v0.23.1 (review F10) deferred frees of pinned blocks */
/* mem.c: a free on the step thread inside a pass was deferred (tag = the pass's frame). A pinned block gets an entry: hidden from the
 * lookups for later frames (as C1a), killed at its confirmation, made visible again when a rollback or an RPB drops the free. */
void rb_def_note(uintptr_t p) {
    if (!g_rb_mem_on || RBT(g_rbt_no_dpin)) return;
    uint32_t k = g_pass_k;
    if (!k || GetCurrentThreadId() != g_step_tid) return;
    SnapSet *ss = modes_ssw(); Spin *pl = modes_pin_lock();
    spin_lock(pl);
    int slot = ss->stable && ss->npin ? snapset_find_start(ss, p) : -1;
    for (int i = 0; i < g_nq && slot >= 0; i++) if (g_q[i].addr == p) slot = -1;   /* quarantined: the quarantine owns its pin */
    int over = 0;
    if (slot >= 0) {
        if (g_ndp < DP_CAP) { DpEnt *e = &g_dp[g_ndp++]; e->addr = p; e->len = ss->pins[slot].len; e->slot = slot; e->t = k; e->rkey = 0; g_dpin++; }
        else { g_dpin_over++; over = g_dpin_over == 1; }
    }
    spin_unlock(pl);
    if (over) LOG("RBDPIN overflow f=%u addr=0x%llx cap=%d (more deferred frees of pinned blocks than the list holds: those pins stay visible until their confirmation; tier-1 pointer normalization may differ from C1a)\n",
                  k, (unsigned long long)p, DP_CAP);
}
static void dp_unhide_drop(int i, SnapSet *ss, int *ch) {   /* pin lock held */
    int sl = slot_resolve(ss, g_dp[i].addr, &g_dp[i].slot, &g_dp[i].rkey);
    if (sl >= 0) *ch += snapset_pin_flags(ss, sl, 0);
    g_dp[i].addr = 0;
}
static void dp_compact(void) {   /* step thread, pin lock held */
    int w = 0;
    for (int i = 0; i < g_ndp; i++) if (g_dp[i].addr) g_dp[w++] = g_dp[i];
    g_ndp = w;
}
/* a rollback to r dropped the deferred frees tagged >= r (def_drop_from): their pins are alive and visible again */
static void dp_drop_from(uint32_t r) {
    if (!g_ndp) return;
    SnapSet *ss = modes_ssw(); Spin *pl = modes_pin_lock(); int ch = 0;
    spin_lock(pl);
    for (int i = 0; i < g_ndp; i++) if (g_dp[i].addr && g_dp[i].t >= r) dp_unhide_drop(i, ss, &ch);
    dp_compact();
    if (ch) snapset_rebuild_visible(ss);
    spin_unlock(pl);
}
/* an RPB's def_restore dropped the frees its pass deferred: the entries added since n0 go */
static void dp_truncate(int n0) {
    SnapSet *ss = modes_ssw(); Spin *pl = modes_pin_lock(); int ch = 0;
    spin_lock(pl);
    for (int i = n0; i < g_ndp; i++) if (g_dp[i].addr) dp_unhide_drop(i, ss, &ch);
    if (g_ndp > n0) g_ndp = n0;
    if (ch) snapset_rebuild_visible(ss);
    spin_unlock(pl);
}
static int dp_find(uintptr_t p) { for (int i = 0; i < g_ndp; i++) if (g_dp[i].addr == p) return i; return -1; }   /* pin lock held */
/* one pass of frame k (the C0 primitives: run_fi with L3, the L4 listener, routing, deferred frees tagged with k) */
static void pass_run(int type, uint32_t k, uint32_t s, FrameIn *in, int final) {
    if (g_nq || g_ndp) q_visibility(k);
    Pend *pp = pend_at(k); if (!pp && type != PT_RPB) pp = pend_new(k);
    memset(&g_pass, 0, sizeof g_pass);
    g_pass.type = type; g_pass.k = k; g_pass.s = s; g_pass.final = final; g_pass.c0 = modes_lsn_c0_calls();
    g_pass.pend = pp;
    g_pass.quiet = type == PT_LIVE && !final && g_next_quiet;
    g_next_quiet = 0;
    int muted = type == PT_CRESIM || type == PT_URESIM || type == PT_RPB;
    g_lp_extra_fwd = (type == PT_CRESIM || type == PT_RPB) ? g_cfg.rb_state_mask : 0;
    g_pass_k = k;
    lipsync_pass_start(k, s);   /* LIPSYNC: pass-start guard (the first pass after a hooked restore) + watchdog */
    run_fi(in, muted, k, RB);
    g_pass_k = 0;
    g_lp_extra_fwd = 0;
}
/* after a non-RPB pass: its diagnostics become the frame's (the last pass is the one written at confirmation) */
static void pass_record(uint32_t k, const FrameIn *in, char ps) {
    Pend *p = pend_at(k); if (!p) p = pend_new(k);
    c1a_from(&p->used, in);
    p->ps = ps; p->pend_st = g_pass.muted; p->pend_slot = g_pass.mslot;
    p->mode = *(volatile uint32_t *)VA(0x144846364ULL);   /* v0.24.0-C1b2 zones: the battle mode id after the pass */
    memcpy(p->calls, g_pass.calls, sizeof p->calls);
    for (int i = 0; i < LISTENER_NSLOTS; i++) p->fwd[i] = (uint16_t)(p->fwd[i] + g_pass.fwd[i]);
    pres_record(p, k);   /* v0.33.0 (§22.1) */
    c1a_pass_compute(k, &p->used, p->ifc, p->q, &p->pass, &g_diag);
    g_pass.type = PT_NONE;
}

/* ------------------------------------------------------------------ pins: PINGUARD, the quarantine */
static int pinguard(const Snap *sn, uint32_t target, const char *what) {
    SnapSet *ss = modes_ssw();
    uint64_t t0 = qpc(); int bad = 0;
    if (!snap_layout_prefix(ss, sn)) {
        g_layout++;
        LOG("RBLAYOUT target=%u entry_npin=%d cur_npin=%d entry_plen=%zu cur_plen=%zu what=%s entry_gen=%u cur_gen=%u\n", target, sn->npin, ss->npin, sn->plen, ss->pin_bytes, what, sn->gen, ss->gen);
        g_diag += qpc_us(qpc() - t0);
        return 2;
    }
    if (g_cfg.rb_pinguard)
        for (int i = 0; i < sn->npin; i++) {
            /* v0.23.1 (review F2): a RELEASED pin (the quarantine let its block go at q) guards too: a target before q would bring back
             * the core state from before the free. With a correct q this never fires; it is the check behind the release horizon. */
            int st = ss->pstate[i];
            if (!sn->pdead[i] && (st == SPIN_FREED || (st == SPIN_RELEASED && !RBT(g_rbt_no_relguard))) && ss->pdq[i] > (int32_t)target) {
                if (!bad) LOG("RBPINGUARD target=%u pin=%d addr=0x%llx len=%u state=%s freed_from=%d what=%s (the restore would write a block that was freed for real)\n",
                              target, i, (unsigned long long)ss->pins[i].addr, ss->pins[i].len, st == SPIN_FREED ? "freed" : "released", ss->pdq[i], what);
                bad++;
            }
        }
    if (bad) g_pinguard++;
    g_diag += qpc_us(qpc() - t0);
    return bad ? 1 : 0;
}
static uint32_t free_q(int *ctx) {   /* q of a free happening now (C1B-SPEC §2.5) */
    uint32_t k = g_pass_k;
    int st = GetCurrentThreadId() == g_step_tid;
    if (st && k) { *ctx = 1; return k + 1; }
    *ctx = st ? 2 : 0;
    /* v0.23.1 (review F2): while the live pass of s runs, ring[s].pre was taken BEFORE this free, so ring[s] is a pre-free target: q = s + 1
     * (before, g_live_done + 1 = s let H = s release the block at the end of step s while a rollback to s was still legal) */
    uint32_t lp = g_live_pre;
    if (lp && !RBT(g_rbt_old_freeq)) return lp + 1;
    return g_live_done + 1;
}
/* v0.31.0 (OPTA-M2-SPEC.md §20.3): the step thread's pin filter (rbonline). One bit per hash of a block start; a SUPERSET of every address the
 * locked path below can match: every slot of pins[] (the stable layout never drops a slot's address within a battle; dead slots stay), every
 * deferred pin (g_dp) and every quarantined block (g_q). It is rebuilt under the pin lock by the step thread (modes.c do_pin_scan, after the
 * merge) and only the step thread reads it, without the lock: no other thread ever writes pins[] / g_dp, and a g_q entry is always a pin start
 * (already in the filter). A free on the step thread whose bit is clear (and which is not a deferred hit) cannot be a pin, a deferred pin or a
 * quarantined block: rb_mem_free returns 0 without the lock, as its locked path would. S-B on v0.30.0: the +0x058 START listener freed ~1.18 M
 * blocks per armed round end on the step thread, each through this lock (79-90 ms per round end). Every other thread keeps the locked path. */
#define PINF_BITS 18
static uint64_t g_pinf[(1u << PINF_BITS) / 64]; static int g_pinf_on;
static inline uint32_t pinf_h(uintptr_t p) { return (uint32_t)(((uint64_t)(p >> 4) * 0x9E3779B97F4A7C15ULL) >> (64 - PINF_BITS)); }
static inline void pinf_set(uintptr_t p) { uint32_t h = pinf_h(p); g_pinf[h >> 6] |= 1ULL << (h & 63); }
void rb_pinf_rebuild(const SnapSet *ss) {   /* pin lock held; the step thread */
    memset(g_pinf, 0, sizeof g_pinf);
    for (int i = 0; i < ss->npin; i++) pinf_set(ss->pins[i].addr);
    for (int i = 0; i < g_ndp; i++) if (g_dp[i].addr) pinf_set(g_dp[i].addr);
    for (int i = 0; i < g_nq; i++) pinf_set(g_q[i].addr);
#ifdef SC6RB_HARNESS
    if (g_h_rs_pinf_plant) memset(g_pinf, 0, sizeof g_pinf);   /* harness plant (XRSPINF): a filter that misses every pin (the teeth of RSPINF) */
#endif
    g_pinf_on = !RS_OLD(RS_OLD_FILTERS);
}
static uint64_t g_pinf_skips;   /* frees the filter answered (RB EDGE pinf_skips=) */
uint64_t rb_pinf_skips(void) { return g_pinf_skips; }
int rb_mem_free(uintptr_t p, int crt, int deferred_hit) {
    if (!g_rb_mem_on) return 0;
    if (deferred_hit && RBT(g_rbt_old_dhit)) return 0;   /* harness control: the v0.23.0 path (freed now, the pin left alive) */
    if (g_pinf_on && !deferred_hit && GetCurrentThreadId() == g_step_tid) {   /* v0.31.0: the step thread's lock-free negative answer (above) */
        uint32_t h = pinf_h(p);
        if (!(g_pinf[h >> 6] & (1ULL << (h & 63)))) { g_pinf_skips++; return 0; }
    }
    SnapSet *ss = modes_ssw(); Spin *pl = modes_pin_lock();
    spin_lock(pl);
    for (int i = 0; i < g_nq; i++) if (g_q[i].addr == p) { spin_unlock(pl); LOG("RBPINQ dup addr=0x%llx (a second free of a quarantined block: swallowed)\n", (unsigned long long)p); return 1; }
    /* v0.23.1 (review F3 ii): an outside free that took back a deferred free (deferred_hit: mem.c removed the deferred entry, so the block
     * is freed by this call, not at a confirmation) goes through the quarantine like any other free of a pinned block. Before, it returned 0:
     * the block was freed at once with its pin left alive (every later snapshot read it, every restore wrote it). Its pin may be hidden
     * (a deferred free of frame t, F10): found through the deferred list. */
    int slot = ss->stable && ss->npin ? snapset_find_start(ss, p) : -1;
    if (slot < 0 && ss->stable) { int di = dp_find(p); if (di >= 0) slot = slot_resolve(ss, p, &g_dp[di].slot, &g_dp[di].rkey); }
    if (deferred_hit) { int di = dp_find(p); if (di >= 0) g_dp[di].addr = 0; g_dpin_hits++; }   /* the pin is the quarantine's now */
    if (slot < 0 || ss->pstate[slot] != SPIN_ALIVE) { spin_unlock(pl); return 0; }
    int ctx; uint32_t q = free_q(&ctx);
    uint32_t len = ss->pins[slot].len;
    if (!g_cfg.rb_quarantine) {   /* control: immediate free, the pin is dead from q on (PINGUARD names it) */
        snapset_kill_range(ss, p, p + len, SPIN_FREED, (int32_t)q);
        spin_unlock(pl);
        LOG("RBPINQ immediate addr=0x%llx len=%u pin=%d q=%u (rb_quarantine=0: freed now, pin dead)\n", (unsigned long long)p, len, slot, q);
        return 2;
    }
    if (g_nq >= PINQ_CAP) {
        snapset_kill_range(ss, p, p + len, SPIN_FREED, (int32_t)q);
        spin_unlock(pl);
        LOG("RBPINQ overflow addr=0x%llx\n", (unsigned long long)p);
        pbar_add((int64_t)q - 1, "pinq_overflow");
        return 2;
    }
    g_q[g_nq].addr = p; g_q[g_nq].len = len; g_q[g_nq].slot = slot; g_q[g_nq].crt = crt; g_q[g_nq].q = q; g_q[g_nq].rkey = 0; g_nq++;
    /* quarantined: never vanished by a rescan; visible to the lookups (pointer normalization, L4 windows, KHit coverage) for passes of
     * frames < q only, as the C1a straight run removes the pin at the entry of step q (pins_process_freed) */
    if (snapset_pin_flags(ss, slot, (uint8_t)(g_pass_k && g_pass_k >= q ? 3 : 2))) snapset_rebuild_visible(ss);
    g_pinq++; g_pinq_bytes += len;
    spin_unlock(pl);
    if (g_lines_pinq < 200) {
        g_lines_pinq++;
        LOG("RBPINQ add q=%u addr=0x%llx len=%u pin=%d thr=%s ctx=%s%s\n", q, (unsigned long long)p, len, slot, GetCurrentThreadId() == g_step_tid ? "step" : "other",
            ctx == 1 ? "incall" : ctx == 2 ? "step" : "outside", deferred_hit ? " dhit=1" : "");
    }
    return 1;
}
void rb_mem_realloc(uintptr_t p) {
    if (!g_rb_mem_on) return;
    SnapSet *ss = modes_ssw(); Spin *pl = modes_pin_lock();
    spin_lock(pl);
    int slot = ss->stable && ss->npin ? snapset_find_start(ss, p) : -1;
    int ctx; uint32_t q = free_q(&ctx);
    if (slot >= 0) snapset_kill_range(ss, p, p + ss->pins[slot].len, SPIN_FREED, (int32_t)q);   /* v0.23.1: every slot of the block */
    spin_unlock(pl);
    if (slot >= 0) { LOG("RBPINQ realloc addr=0x%llx pin=%d q=%u (immediate path; pin dead)\n", (unsigned long long)p, slot, q); pbar_add((int64_t)q - 1, "pin_realloc"); }
}
void rb_pin_freed_confirm(uintptr_t p) {   /* a confirmed deferred free (frames < H): no rollback reaches its pre-free state */
    SnapSet *ss = modes_ssw(); Spin *pl = modes_pin_lock();
    spin_lock(pl);
    /* v0.23.1 (review F3/F10): the pin may be hidden (its deferred-free entry): found there; every alive slot of the block dies */
    int di = ss->stable ? dp_find(p) : -1, slot = -1; uint32_t len = 0;
    if (di >= 0) { slot = slot_resolve(ss, p, &g_dp[di].slot, &g_dp[di].rkey); len = g_dp[di].len; g_dp[di].addr = 0; }
    if (slot < 0) { slot = ss->stable && ss->npin ? snapset_find_start(ss, p) : -1; if (slot >= 0) len = ss->pins[slot].len; }
    if (slot >= 0) snapset_kill_range(ss, p, p + len, SPIN_FREED, 0);
    spin_unlock(pl);
}
static void pinq_release(uint32_t H, int all) {
    SnapSet *ss = modes_ssw(); Spin *pl = modes_pin_lock();
    QEnt rel[256]; int nrel = 0; uint64_t bytes = 0; int total = 0;
    for (;;) {
        spin_lock(pl);
        nrel = rbq_take(g_q, &g_nq, H, all, rel, 256);
        /* v0.23.1 (review F3): every alive slot of the block (by address, so a slot renumbered by a rebuild, a duplicate, or a pin inside
         * the block are all killed); before, only the recorded slot, and only when its index still matched */
        for (int i = 0; i < nrel; i++)
            if (ss->stable) snapset_kill_range(ss, rel[i].addr, rel[i].addr + rel[i].len, SPIN_RELEASED, (int32_t)rel[i].q);
        spin_unlock(pl);
        for (int i = 0; i < nrel; i++) { mem_free_now(rel[i].addr, rel[i].crt); bytes += rel[i].len; }
        total += nrel;
        if (nrel < 256) break;
    }
    if (total) LOG("RBPINQ release H=%u n=%d bytes=%llu%s\n", H, total, (unsigned long long)bytes, all ? " all=1" : "");
}

/* ------------------------------------------------------------------ confirmation and the end of the battle */
static void bk_add(int b, double core, double diag) {
    if (b < 0 || b >= NBK) return;
    if (!g_bk_core[b]) { g_bk_core[b] = (float *)malloc(sizeof(float) * BK_CAP); g_bk_diag[b] = (float *)malloc(sizeof(float) * BK_CAP); }
    if (!g_bk_core[b] || !g_bk_diag[b] || g_bk_n[b] >= BK_CAP) return;
    g_bk_core[b][g_bk_n[b]] = (float)core; g_bk_diag[b][g_bk_n[b]] = (float)diag; g_bk_n[b]++;
}
static int fcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static double pct(float *v, uint32_t n, double p) { if (!n) return 0; uint32_t i = (uint32_t)(p * (double)(n - 1) + 0.5); return v[i < n ? i : n - 1]; }
static double cost_report(int partial) {
    double p99max = 0;
    for (int b = 0; b < NBK; b++) {
        uint32_t n = g_bk_n[b];
        if (!n) continue;
        float *c = (float *)malloc(sizeof(float) * n), *d = (float *)malloc(sizeof(float) * n);
        if (!c || !d) { free(c); free(d); continue; }
        memcpy(c, g_bk_core[b], sizeof(float) * n); memcpy(d, g_bk_diag[b], sizeof(float) * n);
        qsort(c, n, sizeof(float), fcmp); qsort(d, n, sizeof(float), fcmp);
        double p99 = pct(c, n, 0.99);
        if (n >= 20 && p99 > p99max) p99max = p99;
        char dn[8]; if (b == 8) snprintf(dn, sizeof dn, "rpb"); else snprintf(dn, sizeof dn, "%d", b);
        LOG("RBCOST b=%llu depth=%s n=%u p50_us=%.0f p99_us=%.0f max_us=%.0f diag_p50_us=%.0f diag_p99_us=%.0f%s%s\n", (unsigned long long)g_bat, dn, n, pct(c, n, 0.5), p99,
            (double)c[n - 1], pct(d, n, 0.5), pct(d, n, 0.99), n < 20 ? " small=1(max_only)" : "", partial ? " partial=1" : "");
        free(c); free(d);
    }
    return p99max;
}
/* v0.24.0-C1b2 §2.6: the zone decisions, on confirmed frames in order (the inputs are final). Entry: the first confirmed frame k outside a
 * zone whose mode id is not the fight mode, or rp = 1, or a state-class call -> zone frames from z = k + gap. Exit: the first confirmed
 * frame j >= z with the fight mode, rp = 0 and no state-class call in [j - M, j] (M = net_max_rollback) -> the zone ends at e = j + margin. */
static uint32_t c2_stall_est(void) {
    int lat = (g_cfg.emu_latency_ms * 1000 + EMU_T_US - 1) / EMU_T_US - g_cfg.net_delay;
    return lat > 0 ? (uint32_t)lat : 0;
}
static void zone_add(uint32_t k, uint32_t z, const char *cause) {
    if (g_nzi == ZI_CAP) { memmove(g_zi, g_zi + ZI_CAP / 2, sizeof(ZInt) * (ZI_CAP / 2)); g_nzi = ZI_CAP / 2; }
    g_zi[g_nzi].z = z; g_zi[g_nzi].e = UINT32_MAX; g_nzi++; g_zones++;
    LOG("RBLOCK enter z=%u k=%u cause=%s pending_unconfirmed=%lld c2_stall_est=%u\n", z, k, cause, (long long)g_live_done - (long long)g_C, c2_stall_est());
    if (g_nt && !g_in_tail) ns_zone_event((int)(g_zones - 1), z, UINT32_MAX);   /* C2: ZONE messages */
}
static int pend_event(const Pend *p) {
    /* C1B3: (C1B-R3-SPEC.md §2.3.2) slot 0x0c0 is not a zone event: the zone set of 0x58,0xc0 equals v0.24's of 0x58 on the same input */
    for (int i = 1; i < LISTENER_NSLOTS; i++) if (((g_cfg.rb_state_mask >> i) & 1) && p->calls[i] && i != 0xc0 / 8) return 1;
    return 0;
}
static void zone_confirm(uint32_t f, const Pend *p) {
    if (!g_cfg.rb_zone) return;
    uint32_t mode = p->mode; int rp = p->pass.fr.rp != 0, ev = pend_event(p);
    if (!g_zprev_ok || mode != g_zmode_prev || rp != g_zrp_prev) {
        char old[16]; if (g_zprev_ok) snprintf(old, sizeof old, "%u", g_zmode_prev); else snprintf(old, sizeof old, "-");
        LOG("RBMODE f=%u old=%s new=%u rp=%d\n", f, old, mode, rp);
        g_zprev_ok = 1; g_zmode_prev = mode; g_zrp_prev = rp;
    }
    if (ev) g_zlast_ev = f;
    if (zone_of(f)) g_zone_frames++;
    int open = g_nzi && g_zi[g_nzi - 1].e == UINT32_MAX;
    uint32_t fm = (uint32_t)g_cfg.rb_zone_fight_mode, M = (uint32_t)g_cfg.net_max_rollback;
    if (open) {
        ZInt *zi = &g_zi[g_nzi - 1];
        if (f >= zi->z && mode == fm && !rp && (!g_zlast_ev || g_zlast_ev + M < f)) {
            zi->e = f + (uint32_t)g_cfg.rb_zone_exit_margin;
            LOG("RBLOCK exit e=%u j=%u frames=%u\n", zi->e, f, zi->e - zi->z);
            if (g_nt && !g_in_tail) ns_zone_event((int)(g_zones - 1), zi->z, zi->e);   /* C2 */
        }
    } else {
        char cause[24] = "";
        if (mode != fm) snprintf(cause, sizeof cause, "mode:%u", mode); else if (rp) snprintf(cause, sizeof cause, "rp"); else if (ev) snprintf(cause, sizeof cause, "event");
        if (!cause[0]) return;
        /* v0.24.0 (deviation from the literal §2.6 entry rule, found by harness case GZ): a trigger inside the closing tail [j, e) of the last zone
         * re-opens it at e (a contiguous interval [e, inf), logged as RBLOCK enter ... reopen=1) when that is still causal: a zone frame is
         * confirmed at most max_rb - 1 frames after it, so frames >= e are not delivered yet iff f + D + max_rb - 1 < e. The literal rule ("the
         * first confirmed frame outside a zone") waited for e and opened the next zone gap frames later: a mode/rp change inside a short event
         * zone left the replay edge right after it outside every zone (GZ: RBREFUSED at the 730 edge). A trigger later in the tail opens a new
         * zone at f + gap (> e). */
        ZInt *last = g_nzi ? &g_zi[g_nzi - 1] : NULL;
        int in_tail = last && f >= last->z && f < last->e;
        if (in_tail && f + (uint32_t)g_cfg.net_delay + (uint32_t)g_cfg.net_max_rollback - 1 < last->e) {
            uint32_t eo = last->e;
            if (g_nzi == ZI_CAP) { memmove(g_zi, g_zi + ZI_CAP / 2, sizeof(ZInt) * (ZI_CAP / 2)); g_nzi = ZI_CAP / 2; }
            g_zi[g_nzi].z = eo; g_zi[g_nzi].e = UINT32_MAX; g_nzi++; g_zones++;
            if (g_nt && !g_in_tail) ns_zone_event((int)(g_zones - 1), eo, UINT32_MAX);   /* C2 */
            LOG("RBLOCK enter z=%u k=%u cause=%s pending_unconfirmed=%lld c2_stall_est=%u reopen=1\n", eo, f, cause, (long long)g_live_done - (long long)g_C, c2_stall_est());
        } else zone_add(f, f + (uint32_t)g_cfg.rb_zone_entry_gap, cause);
    }
}
/* v0.29.3 (review M2, OPTA-M2-SPEC.md §16.14): in the rbreplay hybrid every frame before rb_arm_f is lockstep (zone semantics: known at its
 * capture step f - D, never predicted), as in rbonline, where the stock netcode runs every frame before ARM. v0.29.2 required those frames
 * to be zones, so an ARM at round 2 or later (the first zone after round 1's fight) aborted (RBARMF): S-M2a could only arm at round 1.
 * Only with rb_arm_f > 0 (a hybrid key): every other configuration keeps its frontier. h_rb_armf_zone=1 (harness only) = the v0.29.2 rule. */
static int pre_arm_lockstep(uint32_t g) { return g_cfg.rb_arm_f > 0 && !g_lv && g < (uint32_t)g_cfg.rb_arm_f && !RBT(g_rbt_armf_zone); }
/* the frontier: the emulator's (rb_zone=0, unchanged), or with zones the contiguous frontier of the effective known step (a zone frame f is
 * known at its capture step f - D; v0.29.3: so is every frame before rb_arm_f) */
static uint32_t rb_frontier(uint32_t s) {
    if (g_on) return g_nsr < g_nsl ? g_nsr : g_nsl;   /* optA M2: the confirmed remote frames composed into L, capped at the captured local ones */
    if (g_nt) {   /* C2: the session's contiguous frontier, capped at the composed frames (both sides known) */
        uint32_t F = g_nsr < g_nsl ? g_nsr : g_nsl;
        return F;
    }
    if (!g_cfg.rb_zone) return emu_frontier(&g_es, s);
    while (g_Fz < g_N) {
        uint32_t g = g_Fz + 1;
        int64_t kn = zone_of(g) || pre_arm_lockstep(g) ? (int64_t)g - g_cfg.net_delay : (int64_t)g_es.known[g];   /* v0.29.3: + pre_arm_lockstep (M2) */
        if (kn > (int64_t)s) break;
        g_Fz++;
    }
    return g_Fz;
}
static uint32_t pass_ued(const C1aPass *p) { return np_fnv32(p->fr.ifc, strlen(p->fr.ifc), 0x811c9dc5u); }   /* C2: the UE->core call digest (CHECK ued, F line ued=) */
static void confirm_frame(uint32_t f, uint32_t s) {
    Pend *p = pend_at(f);
    const C1aIn *r = rb_truth(f);
    if (!p || !r) { LOG("RBCONFIRM missing f=%u pend=%d rec=%d\n", f, p != NULL, r != NULL); return; }
    uint64_t t0 = qpc();
    int ok = used_ok(f, &p->used);
    char ps = g_tailT ? 'T' : p->ps;
    char xr[0x18 * 2 + 1]; for (int i = 0; i < 0x18; i++) snprintf(xr + 2 * i, 3, "%02x", r->x[i]);
    char sfx[176]; snprintf(sfx, sizeof sfx, "ps=%c lt=%u ok=%d p1r=%016llx xr=%s z=%d", ps, s >= f ? s - f : 0, ok, (unsigned long long)r->p1, xr, zone_of(f));
    uint32_t ued = pass_ued(&p->pass);
    if (g_nt) { size_t o = strlen(sfx); snprintf(sfx + o, sizeof sfx - o, " ued=%08x", ued); }   /* C2 fix r1 (C2-SPEC 6.4): every rbnet F line carries ued= */
    p->pass.ps = ps;
    c1a_confirm(&p->pass, sfx);
    if (g_nt && !g_in_tail) ns_confirmed(f, p->pass.fr.t1, p->pass.t15, ued, p->pass.fr.c);   /* C2: CHECK */
    if (g_on && (!g_in_tail || g_tail_chk) && !zone_of(f)) rbo_core_confirmed_frame(f, &p->pass);   /* optA M2: CHECK (no zone frame, D6) + the light-dump record;
                                                                                                    * v0.29.1 (B2): an abort's flush too (committed state) */
    else if (g_on && (!g_in_tail || g_tail_chk)) rbo_core_zone_frame(f);   /* v0.32.0 review fix M1: a zone entry sends the open CHECK chain */
    zone_confirm(f, p);
    { uint8_t b8[8]; uint64_t t = p->pass.fr.t1; for (int i = 0; i < 8; i++) b8[i] = (uint8_t)(t >> (8 * i)); g_chain = emu_fnv1a(b8, 8, g_chain); }
    if (g_pres_mask) {   /* v0.33.0 (§22.1): the missed presentation calls of the confirmed pass, once (a flushed tail is not delivered: counted) */
        if (!g_in_tail) pres_confirm(f, s, p);
        else { PresFrame *P = &p->pres; uint8_t u[PRES_MAXD]; memset(u, 0, sizeof u);
               for (int i = 0; i < P->nl; i++) { int m = 0; for (int j = 0; j < P->nd; j++) if (!u[j] && P->dlv[j].h == P->last[i].h) { u[j] = 1; m = 1; break; } if (!m) g_pres_tail++; } }
    }
    for (int i = 1; i < LISTENER_NSLOTS; i++) {
        if (is_query(i)) continue;
        uint64_t fw = (uint64_t)p->fwd[i] + p->pres_late[i] + p->pres_sup[i], fi = p->calls[i];   /* v0.33.0: + the late deliveries and the superseded calls */
        g_led_fwd[i] += fw; g_led_final[i] += fi; g_led_late[i] += p->pres_late[i]; g_led_sup[i] += p->pres_sup[i];
        if (fw > fi) g_led_orphan[i] += fw - fi; else g_led_missed[i] += fi - fw;
    }
    if (p->pend_st) { g_statelost++; LOG("RBSTATELOST f=%u slot=+0x%03x cause=%s\n", f, p->pend_slot * 8, g_cfg.rb_rollback ? "ring" : "control_no_rollback"); }
    g_confirmed++;
    if (f % 600 == 0) LOG("RBCHAIN f=%u chain=%016llx\n", f, (unsigned long long)g_chain);
    if (g_confirmed % 3600 == 0) cost_report(1);
    p->valid = 0;
    g_diag += qpc_us(qpc() - t0);
}
static void confirm_upto(uint32_t newC, uint32_t s) {
    while (g_C < newC) { g_C++; confirm_frame(g_C, s); }
    pres_flush();   /* v0.33.0 (§22.1): this step's late deliveries, in frame order, between passes */
}
/* write every remaining frame (abort, battle end): used == recorded counts as final, otherwise tail_mismatch (ok=0) */
static void flush_tail(const char *cause, int count_tail) {
    g_in_tail = 1;   /* C2: no CHECK for the tail (its frames are not confirmed) */
    for (uint32_t f = g_C + 1; f <= g_live_done; f++) {
        Pend *p = pend_at(f);
        if (!p) continue;
        if (count_tail) { g_tail_unconf++; if (!used_ok(f, &p->used)) g_tail_mm++; }
        if (p->pend_st) { g_statelost++; LOG("RBSTATELOST f=%u slot=+0x%03x cause=%s\n", f, p->pend_slot * 8, cause); p->pend_st = 0; }
        g_C = f; confirm_frame(f, g_s);
    }
    g_in_tail = 0;
}
static const char *abort_cause(void) { return g_abort[0] ? g_abort : "0"; }
static void do_rpb(uint32_t k, uint32_t s);   /* C1B3 fix: rb_abort delivers pending state-class calls through it */
static void rb_end_lines(const char *why) {
    char dh[160]; size_t o = 0; dh[0] = 0;
    for (int d = 0; d < 16; d++) if (g_dhist[d]) o += (size_t)snprintf(dh + o, sizeof dh - o, "%s%d:%llu", o ? "," : "", d, (unsigned long long)g_dhist[d]);
    char bc[400]; o = 0; bc[0] = 0;
    for (int i = 0; i < g_nbc; i++) o += (size_t)snprintf(bc + o, sizeof bc - o, "%s%s:%llu", o ? "," : "", g_bc[i].c, (unsigned long long)g_bc[i].n);
    uint64_t orph = 0, miss = 0; for (int i = 0; i < LISTENER_NSLOTS; i++) { orph += g_led_orphan[i]; miss += g_led_missed[i]; }
    double p99 = cost_report(0);
    char t2[24]; if (c1a_t2_comparable()) snprintf(t2, sizeof t2, "%u", c1a_t2_points()); else snprintf(t2, sizeof t2, "incomparable");
    char sp[24] = "-";   /* v0.24.0-C1b2: rblive: FNV-1a 64 over u32le(f) || u32le(known(f)) of the emulator schedule, f = 1 .. the confirmed frames */
    if (g_lv && g_es_ok) {
        uint64_t h = EMU_FNV_BASIS;
        for (uint32_t f = 1; f <= g_C && f <= g_es.N; f++) {
            uint8_t b[8]; uint32_t w0 = f, w1 = (uint32_t)g_es.known[f];
            for (int i = 0; i < 4; i++) { b[i] = (uint8_t)(w0 >> (8 * i)); b[4 + i] = (uint8_t)(w1 >> (8 * i)); }
            h = emu_fnv1a(b, 8, h);
        }
        snprintf(sp, sizeof sp, "%016llx", (unsigned long long)h);
    }
    LOG("RBEND b=%llu why=%s live=%llu confirmed=%u tail_unconfirmed=%llu tail_mismatch=%llu rollbacks=%llu d_hist=%s dmax=%llu cause_input=%llu cause_state=%llu cause_both=%llu rpb=%llu refused=%llu abort=%s "
        "clamps=%u stalls=%llu barriers=%llu barrier_causes=%s clears=%llu pinq=%llu pinq_bytes=%llu pinguard=%llu layout=%llu stateev=%llu statelost=%llu orphan=%llu missed=%llu t1mm=%u t2cmp=%s "
        "seg_mispredict=%u ueskip=%llu multi_rb_ticks=%llu deferred_ticks=%llu p99_core_us=%.0f chain=%016llx t1norm=%u zones=%llu zone_frames=%llu live=%d livein_bad=%llu mmdumps=%u sched_played=%s t1norm_rescue=%u t1norm_oldrule=%u "
        "c0_mid=%llu c0_late_max=%d xpred=%s\n",
        (unsigned long long)g_bat, why, (unsigned long long)g_live, g_C, (unsigned long long)g_tail_unconf, (unsigned long long)g_tail_mm, (unsigned long long)g_rbk, o ? dh : "-",
        (unsigned long long)g_dmax, (unsigned long long)g_cin, (unsigned long long)g_cst, (unsigned long long)g_cboth, (unsigned long long)g_rpb, (unsigned long long)g_refused_n,
        abort_cause(), g_es_ok ? g_es.clamps : 0, (unsigned long long)(g_nt ? ns_stalls_total() : 0), (unsigned long long)g_barriers, g_nbc ? bc : "-", (unsigned long long)g_clears, (unsigned long long)g_pinq,
        (unsigned long long)g_pinq_bytes, (unsigned long long)g_pinguard, (unsigned long long)g_layout, (unsigned long long)g_stateev, (unsigned long long)g_statelost,
        (unsigned long long)orph, (unsigned long long)miss, c1a_t1mm(), t2, g_seg_mp, (unsigned long long)g_ueskip, (unsigned long long)g_multi_ticks,
        (unsigned long long)g_def_ticks, p99, (unsigned long long)g_chain, c1a_t1norm(), (unsigned long long)g_zones, (unsigned long long)g_zone_frames, g_lv,
        (unsigned long long)g_livein_bad, c1a_mm_written(), sp, c1a_t1norm_rescue(), c1a_t1norm_oldrule(), (unsigned long long)g_c0_mid, g_c0_late_max,
        RBT(g_cfg.h_old_xpred) ? "old" : "new");   /* C1B3: */
    /* v0.23.1: the review-fix counters (a separate line, so the RBEND field list of C1B-SPEC §4.1 is unchanged) */
    char bd2[200]; rb_p99_by_depth(bd2, sizeof bd2);
    double l4mp = 0; if (g_l4m_n) { float *c = (float *)malloc(sizeof(float) * g_l4m_n); if (c) { memcpy(c, g_l4m_bk, sizeof(float) * g_l4m_n); qsort(c, g_l4m_n, sizeof(float), fcmp); l4mp = pct(c, g_l4m_n, 0.99); free(c); } }
    LOG("RBEND3 b=%llu p99_core_us_by_depth=%s p99_l4miss_us=%.0f (C2 5.3: the L4MISS watchdog time is diag)\n", (unsigned long long)g_bat, bd2, l4mp);   /* C2: a separate line (RBEND / RBEND2 field lists unchanged) */
    LOG("RBEND2 b=%llu fp_overflow=%llu fp_missing=%llu quiet_fwd=%llu state_quiet=%d rpb_dup_muted=%llu dpin=%llu dpin_overflow=%llu dhit=%llu pinpool=%d pindeny=%llu\n",
        (unsigned long long)g_bat, (unsigned long long)g_fpov, (unsigned long long)g_fpmiss, (unsigned long long)g_quiet_fwd, g_cfg.rb_state_quiet,
        (unsigned long long)g_rpb_dup, (unsigned long long)g_dpin, (unsigned long long)g_dpin_over, (unsigned long long)g_dpin_hits, modes_pinpool_count(),
        (unsigned long long)g_pindeny);
    for (int i = 1; i < LISTENER_NSLOTS; i++)
        if (g_led_fwd[i] || g_led_final[i]) {
            if ((g_pres_mask >> i) & 1)   /* v0.33.0 (§22.1): fwd counts the live, late and superseded deliveries; the late ones and the superseded ones are named */
                LOG("RBLEDGER b=%llu slot=+0x%03x fwd=%llu final=%llu orphan=%llu missed=%llu late=%llu superseded=%llu\n", (unsigned long long)g_bat, i * 8, (unsigned long long)g_led_fwd[i],
                    (unsigned long long)g_led_final[i], (unsigned long long)g_led_orphan[i], (unsigned long long)g_led_missed[i], (unsigned long long)g_led_late[i], (unsigned long long)g_led_sup[i]);
            else
            LOG("RBLEDGER b=%llu slot=+0x%03x fwd=%llu final=%llu orphan=%llu missed=%llu\n", (unsigned long long)g_bat, i * 8, (unsigned long long)g_led_fwd[i],
                (unsigned long long)g_led_final[i], (unsigned long long)g_led_orphan[i], (unsigned long long)g_led_missed[i]);
        }
    pres_report();   /* v0.33.0 (§22.1) */
    log_flush();
}
static void tick_close(void) {
    if (!g_tk.steps) return;
    if (g_tk.reruns >= 2) g_multi_ticks++;
    if (g_tk.deferred) g_def_ticks++;
    if ((g_tk.steps > 1 || g_tk.deferred) && g_lines_tick < 2000) {
        g_lines_tick++;
        LOG("RBTICK tick=%llu steps=%d deferred=%d s0=%u reruns=%d\n", (unsigned long long)g_tk.id, g_tk.steps, g_tk.deferred, g_tk.s0, g_tk.reruns);
    }
    memset(&g_tk, 0, sizeof g_tk);
}
static void rb_finish(const char *why) {
    if (g_st != RB_ON && g_st != RB_ABORTED) return;
    tick_close();
    if (g_st == RB_ON) { g_tailT = g_lv && !strcmp(why, "battle_end"); flush_tail("tail", 1); g_tailT = 0; }   /* v0.24.0: the rblive tail is ps=T */
    rb_end_lines(g_st == RB_ABORTED ? "abort" : why);
    modes_q1cov_report();   /* v0.29.2 (rbreplay q1_cov=1; nothing otherwise) */
    if (g_nt) ns_battle_end(g_st == RB_ABORTED ? "abort" : why, g_s, g_C);   /* C2: NETEND */
    pinq_release(0, 1);
    g_rb_mem_on = 0;
    g_st = RB_ENDED;
    g_title_dirty = 1;
}
/* C1B3 fix (round-3 review): a state-class call muted in a predicted pass and still pending at an abort (a mid-fight +0x0c0 within the
 * ~4 frames before it) was dropped for good (RBSTATELOST cause=abort): UE never saw it, while v0.24 forwarded +0x0c0 live. It is now
 * delivered once, by an RPB pass of its frame (with the RPB's dangle handling), before the battle goes straight. Only for the aborts of
 * the drain phase, where the core holds the end of frame s-1 exactly as at any RPB: refused, depth, seg_mispredict (a segment frame has
 * no ring entry: RBSTATELOST cause=ring, loud). Every other abort (a failed restore, snap_alloc, layout, pinguard, lipsync_guard,
 * route_or_defer_broken, campin_fail) restores nothing more: RBSTATELOST cause=abort as before (item 4 FAILs it). */
static int g_aborting;
static void abort_deliver(const char *cause) {
    if (RBT(g_rbt_abort_nodeliver)) return;
    if (strcmp(cause, "refused") && strcmp(cause, "depth") && strcmp(cause, "seg_mispredict")) return;
    for (uint32_t k = g_C + 1; k <= g_live_done && g_st == RB_ON; k++) {
        Pend *p = pend_at(k);
        if (!p || !p->pend_st) continue;
        LOG("RBABORTRPB f=%u slot=+0x%03x s=%u cause=%s (an abort with a pending state-class call: delivered once by an RPB pass before the battle goes straight)\n",
            k, p->pend_slot * 8, g_s, cause);
        do_rpb(k, g_s);
    }
}
/* optA M2 v0.29.1 (review B2): the verification list (see g_unv above) */
static void unv_add(uint32_t f, uint64_t w) {
    for (int i = 0; i < g_nunv; i++) if (g_unv[i].f == f) { g_unv[i].w = w; return; }
    if (g_nunv < UNV_N) { g_unv[g_nunv].f = f; g_unv[g_nunv].w = w; g_nunv++; } else g_unv_dropped++;
}
static void unv_check(uint32_t k, uint64_t truth, int lost) {   /* rbo_compose: frame k's remote truth is known now */
    for (int i = 0; i < g_nunv; i++) {
        if (g_unv[i].f != k) continue;
        uint64_t w = g_unv[i].w;
        g_unv[i] = g_unv[--g_nunv];
        if (lost) { g_unv_lost++; return; }   /* its ring slot was reset before it arrived: never verifiable (CHECK still covers the state) */
        g_unv_verified++;
        if (w != truth) rbo_core_committed_mispredict(k, w, truth);
        return;
    }
}
void rb_unverified_stats(uint32_t *pending, uint32_t *verified, uint32_t *lost, uint32_t *dropped) {
    *pending = (uint32_t)g_nunv; *verified = g_unv_verified; *lost = g_unv_lost; *dropped = g_unv_dropped;
}
int rb_is_aborted(void) { return g_st == RB_ABORTED && !RBT(g_rbt_abort_old); }
static void rb_abort(const char *cause) {
    if (g_st != RB_ON || g_aborting) return;
    g_aborting = 1; abort_deliver(cause); g_aborting = 0;   /* C1B3 fix: a failure inside the delivery cannot abort twice */
    LOG("RBABORT f=%u cause=%s unconfirmed=%lld\n", g_s, cause, (long long)g_live_done - (long long)g_C);
    if (g_on && RBT(g_rbt_abort_old)) rbo_drain(!strcmp(cause, "depth") || !strcmp(cause, "refused") ? RBW_DEPTH : RBW_INTERNAL);   /* XABORT: v0.29.0 */
    else if (g_on) {   /* optA M2 v0.29.1 (review B2): the core cannot roll back any more. Never silent: the pending frames are committed by the flush
                   * below as they ran; one whose truth is already known to differ is a committed misprediction (DESYNC now), the others are
                   * verified when their truth arrives (rbo_compose), and the flush CHECKs them like confirmed frames */
        uint32_t mm_f = 0, unv = 0; uint64_t mm_u = 0, mm_t = 0;
        for (uint32_t k = g_C + 1; k <= g_live_done; k++) {
            Pend *p = pend_at(k);
            if (!p) continue;
            uint64_t uw = g_cfg.net_remote_side == 2 ? p->used.p2 : p->used.p1;
            if (k <= g_nsr && g_Lok && k < g_Lcap && (g_Lok[k] & 2)) {
                uint64_t tw = rec_remote_word(&g_L[k]);
                if (uw != tw && !mm_f) { mm_f = k; mm_u = uw; mm_t = tw; }
            } else { unv_add(k, uw); unv++; }
        }
        g_ab_lastz = g_s; g_ab_zset = zone_of(g_s);
        rbo_core_aborted(cause, g_s, (uint32_t)(g_live_done - g_C), mm_f, mm_u, mm_t, unv);
        g_tail_chk = 1;
    }
    if (g_nt && strncmp(cause, "net_", 4)) { if (!strcmp(cause, "refused")) ns_mispredict_behind_barrier(g_s); else ns_core_abort(cause); }   /* C2: the peer learns */
    snprintf(g_abort, sizeof g_abort, "%s", cause);
    flush_tail("abort", 0);
    g_tail_chk = 0;
    g_rb_mem_on = 0;
    g_live_pre = 0; g_next_quiet = 0;
    int nconf = def_confirm_all();
    pinq_release(0, 1);
    LOG("RBCLEAR f=%u cause=abort unconfirmed=0 confirmed_frees=%d\n", g_s, nconf);
    g_clears++;
    g_st = RB_ABORTED;
    g_title_dirty = 1;
    log_flush();
}

/* ------------------------------------------------------------------ lifecycle */
void rb_init(void) {
    g_step_tid = 0;
    if (!is_rbcore_mode(g_cfg.mode)) return;
    if (!g_cfg_ok) return;
#ifdef SC6RB_HARNESS
    if (g_on) { g_rbt_abort_old = cfg_geti("h_rbo_abort_old", 0); if (g_rbt_abort_old) LOG("RB harness control h_rbo_abort_old=1 (XABORT: the v0.29.0 abort path)\n"); }
    if (g_cfg.mode == MODE_RBREPLAY && g_cfg.rb_arm_f > 0) {   /* v0.29.3 (M2): the control of the pre-F lockstep rule */
        g_rbt_armf_zone = cfg_geti("h_rb_armf_zone", 0);
        if (g_rbt_armf_zone) LOG("RB harness control h_rb_armf_zone=1 (the v0.29.2 pre-F rule: frames before rb_arm_f are lockstep only inside a zone)\n");
    }
#endif
    g_ring = (RbEnt *)calloc((size_t)g_R, sizeof(RbEnt));
    if (!g_ring) { LOG("RB WARN ring allocation failed: rollback refused\n"); snprintf(g_refused, sizeof g_refused, "ring_alloc"); g_cfg_ok = 0; g_mode = MODE_OFF; return; }
    for (int i = 0; i < PEND_N; i++) { g_pend[i].pass.dump = -1; g_pend[i].pass.mm = -1; }
    if (g_cfg.mode == MODE_RBREPLAY && g_cfg.rb_arm_f > 0 && c1a_rec_n() && (uint32_t)g_cfg.rb_arm_f >= c1a_rec_n()) {   /* v0.29.2: F inside the recording */
        char nd[48]; snprintf(nd, sizeof nd, "<%u(the_recording's_frames)", c1a_rec_n());
        q1refuse("rb_arm_f", g_cfg.rb_arm_f, nd);
        LOG("CFG C1B REFUSED key=%s: mode=%s refused, the plugin runs mode=off (title rb=REFUSED:%s)\n", g_refused, g_mode_names[g_cfg.mode], g_refused);
        g_cfg_ok = 0; g_mode = MODE_OFF; return;
    }
    if (g_lv) {   /* v0.24.0-C1b2 rblive: the live table (live_cap + D + 8 entries, never resized during a battle) */
        g_Lcap = (uint32_t)g_cfg.live_cap + (uint32_t)g_cfg.net_delay + 8;
        g_L = (C1aIn *)calloc(g_Lcap, sizeof(C1aIn)); g_Lok = (uint8_t *)calloc(g_Lcap, 1);
        if (!g_L || !g_Lok) { LOG("RB WARN live table allocation failed (%u entries): rollback refused\n", g_Lcap); snprintf(g_refused, sizeof g_refused, "live_alloc"); g_cfg_ok = 0; g_mode = MODE_OFF; return; }
        LOG("RB live table=%u entries (live_cap=%d + D=%d + 8) bytes=%zu\n", g_Lcap, g_cfg.live_cap, g_cfg.net_delay, (size_t)g_Lcap * (sizeof(C1aIn) + 1));
    }
    LOG("RB init ring=%d pend=%d quarantine_cap=%d (C1B-SPEC §2; frames = the C1a battle step f)\n", g_R, PEND_N, PINQ_CAP);
}
static void battle_reset(void) {
    g_st = RB_IDLE; g_abort[0] = 0; g_C = g_b = g_s = g_live_done = g_F = 0; snprintf(g_bcause, sizeof g_bcause, "setup");
    g_seg = 0; g_seg_start = g_seg_frames = g_seg_verified = g_seg_mp = 0; g_rp_prev = 0;
    g_live = g_rbk = g_dmax = g_cin = g_cst = g_cboth = g_rpb = g_refused_n = g_barriers = g_clears = g_pinq = g_pinq_bytes = 0;
    g_pinguard = g_layout = g_stateev = g_statelost = g_ueskip = g_multi_ticks = g_def_ticks = g_tail_unconf = g_tail_mm = g_confirmed = 0;
    memset(g_dhist, 0, sizeof g_dhist); g_nbc = 0;
    memset(g_led_fwd, 0, sizeof g_led_fwd); memset(g_led_final, 0, sizeof g_led_final); memset(g_led_orphan, 0, sizeof g_led_orphan); memset(g_led_missed, 0, sizeof g_led_missed); memset(g_led_late, 0, sizeof g_led_late); memset(g_led_sup, 0, sizeof g_led_sup); pres_reset();   /* v0.33.0 */
    g_lines_clamp = g_lines_pinq = g_lines_tick = 0;
    g_c0_mid = 0; g_c0_late_max = 0;   /* C1B3: */
    for (int b = 0; b < NBK; b++) g_bk_n[b] = 0;
    g_l4m_n = 0;   /* C2 */
    memset(&g_tk, 0, sizeof g_tk); g_tick_ctr = 0;
    g_chain = EMU_FNV_BASIS;
    for (int i = 0; i < PEND_N; i++) { if (g_pend[i].valid) c1a_pass_drop(&g_pend[i].pass); g_pend[i].valid = 0; g_pend[i].pass.dump = -1; }
    for (int i = 0; g_ring && i < g_R; i++) g_ring[i].valid = 0;
    g_post_ok = 0; g_npbar = 0;
    g_live_pre = 0; g_post_want = 0; g_next_quiet = 0; g_ndp = 0;
    g_fpov = g_fpmiss = g_quiet_fwd = g_rpb_dup = g_dpin = g_dpin_over = g_dpin_hits = 0;
    /* v0.24.0-C1b2 */
    for (int i = 0; i < PEND_N; i++) g_pend[i].pass.mm = -1;
    if (g_Lok) memset(g_Lok, 0, g_Lcap);
    g_live_prev_held = 0; g_livein_bad = 0; g_livein_lines = 0; g_tailT = 0;
    g_pd_pending = 0; g_pindeny = 0; g_pindeny_lines = 0;
    g_nzi = 0; g_zlast_ev = 0; g_zmode_prev = 0; g_zrp_prev = 0; g_zprev_ok = 0; g_Fz = 0; g_zones = 0; g_zone_frames = 0;
    g_nsl = g_nsr = 0; g_in_tail = 0;   /* C2 */
    g_nunv = 0; g_unv_verified = g_unv_lost = g_unv_dropped = 0; g_tail_chk = 0; g_ab_lastz = 0; g_ab_zset = 0;   /* optA M2 v0.29.1 (B2) */
    g_q1_pend = 0;   /* v0.29.2 */
}
void rb_battle_begin(void) {
    if (!is_rbcore_mode(g_cfg.mode) || !g_cfg_ok) return;
    if (g_st == RB_ON || g_st == RB_ABORTED) rb_finish("shutdown");
    pinq_release(0, 1);
    g_bat++;
    lipsync_battle_begin(g_bat);   /* LIPSYNC: battle start */
    battle_reset();
    g_pinf_on = 0; g_pinf_skips = 0;   /* v0.31.0: the locked path until this battle's first pin-set merge rebuilds the filter */
    LOG("RBCLEAR f=0 cause=init unconfirmed=0 battle=%llu\n", (unsigned long long)g_bat);
}
void rb_battle_setup(void) {
    if (!is_rbcore_mode(g_cfg.mode) || !g_cfg_ok) return;
    if (g_st == RB_ON) {   /* a Setup inside a running battle: the ring is gone and nothing before it can be rolled back */
        g_clears++;
        LOG("RBCLEAR f=%u cause=setup unconfirmed=%lld\n", g_s, (long long)g_live_done - (long long)g_C);
        for (int i = 0; i < g_R; i++) g_ring[i].valid = 0;
        g_post_ok = 0;
        barrier(g_live_done, "setup", 0);
    } else LOG("RBCLEAR f=0 cause=setup unconfirmed=0\n");
}
void rb_battle_end(void) {
    if (!is_rbcore_mode(g_cfg.mode) || !g_cfg_ok) return;
    if (g_st == RB_ON || g_st == RB_ABORTED) rb_finish(g_lv ? "battle_end" : "shutdown");   /* C2: rbnet too (g_lv) */
    lipsync_battle_end(g_bat);   /* LIPSYNC: LIPSYNCSTAT why=battle_end */
    pinq_release(0, 1);
    g_rb_mem_on = 0;
    LOG("RBCLEAR f=%u cause=shutdown unconfirmed=0\n", g_s);
}
void rb_passthrough_step(void) { if (g_st == RB_ON) rb_abort("passthrough_step"); }
int rb_ring_clear(const char *why) {
    static const char *const repin[] = { "khit_list_changed", "extw_autopin", "new_tracked_blocks", "campin_rescan", "campin_ok", "anim_kill" };
    for (size_t i = 0; i < sizeof repin / sizeof repin[0]; i++)
        if (!strcmp(why, repin[i])) { char c[48]; snprintf(c, sizeof c, "repin:%s", why); pbar_add(-1, c); return 1; }
    if (!strcmp(why, "replay_start") || !strcmp(why, "replay_end") || !strcmp(why, "setup")) return 1;   /* rb_step / rb_battle_setup */
    if (!strcmp(why, "init") || !strcmp(why, "shutdown")) return 0;
    if (!strcmp(why, "campin_fail")) { rb_abort("campin_fail"); return 0; }
    char c[64]; snprintf(c, sizeof c, "clear:%s", why);
    rb_abort(c);
    return 0;
}
void rb_note_repin(const char *why) { char c[48]; snprintf(c, sizeof c, "repin:%s", why); pbar_add(-1, c); }
void rb_note_rebuild(const char *why) {
    LOG("RBPINS rebuild f=%u why=%s gen=%u (all older ring entries are no longer a prefix)\n", g_s, why, modes_ssw()->gen);
}
void rb_note_extend(int old_n, int new_n, int appended, int vanished) {
    LOG("RBPINS extend f=%u old=%d new=%d appended=%d dead=%d\n", g_s, old_n, new_n, appended, vanished);
}
void rb_header_keys(char *out, size_t cap) {
    out[0] = 0;
    if (!g_cfg_ok) return;
    if (g_on) {   /* optA M2: the rbonline recording's keys (the armed timeline; f = the armed step) */
        snprintf(out, cap, " live=1 mode=rbonline cap=%d net_remote=%d d=%d max_rb=%d armed_round=%u", g_cfg.live_cap, g_cfg.net_remote_side, rbo_d, g_cfg.net_max_rollback, rbo_armed_round);
        return;
    }
    if (g_nt) {   /* C2 (C2-SPEC.md §6.4): the rbnet recording's keys */
        char nk[400]; ns_header_keys(nk, sizeof nk);
        snprintf(out, cap, " live=1%s cap=%d", nk, g_cfg.live_cap);
        return;
    }
    if (g_lv) {   /* v0.24.0-C1b2 (C1B-R2-SPEC.md §2.5): the live recording's keys */
        snprintf(out, cap, " live=1 mode=rblive cap=%d sched_cap=%016llx net_remote=1 net_delay=%d emu_lat_ms=%d emu_jit_ms=%d emu_loss_pct=%d emu_red=%d emu_seed=%lld max_rb=%d",
                 g_cfg.live_cap, (unsigned long long)(g_es_ok ? g_es.sched : 0), g_cfg.net_delay, g_cfg.emu_latency_ms, g_cfg.emu_jitter_ms, g_cfg.emu_loss_pct, g_cfg.emu_redundancy,
                 g_cfg.emu_seed, g_cfg.net_max_rollback);
        return;
    }
    snprintf(out, cap, " ghost=1 mode=rbreplay net_remote=%d net_delay=%d emu_lat_ms=%d emu_jit_ms=%d emu_loss_pct=%d emu_red=%d emu_seed=%lld max_rb=%d sched=%016llx",
             g_cfg.net_remote_side, g_cfg.net_delay, g_cfg.emu_latency_ms, g_cfg.emu_jitter_ms, g_cfg.emu_loss_pct, g_cfg.emu_redundancy, g_cfg.emu_seed,
             g_cfg.net_max_rollback, (unsigned long long)(g_es_ok ? g_es.sched : 0));
}
void rb_header_keys2(char *out, size_t cap) {
    out[0] = 0;
    if (g_cfg_ok && g_cfg.rb_zone) snprintf(out, cap, " zone=1 zone_mode=%d zone_gap=%d zone_margin=%d", g_cfg.rb_zone_fight_mode, g_cfg.rb_zone_entry_gap, g_cfg.rb_zone_exit_margin);
}
/* v0.24.0-C1b2 S1BFIX G2 (C1B-R2-SPEC.md §2.4): a pinned block whose allocation caller is in the TaskGraph deny ranges (modes.c, at every pin
 * build and extension): RBPINDENY, then RBABORT cause=pindeny (the battle goes on as straight passes). Before the battle's first step: at it. */
void rb_pindeny(uintptr_t addr, uint32_t len, uint32_t caller) {
    g_pindeny++;
    if (g_pindeny_lines < 20) { g_pindeny_lines++; LOG("RBPINDENY addr=0x%llx len=%u caller=0x%x (a UE TaskGraph small-task block is pinned: S1BFIX G1 missed it)\n", (unsigned long long)addr, len, caller); }
    if (g_st == RB_ON) rb_abort("pindeny"); else if (g_st == RB_IDLE) g_pd_pending = 1;
}
void rb_title(char *out, size_t cap, const char *l4, const char *l3, const char *cam) {
    char ctl[200]; cfg_controls_str(&g_cfg, ctl, sizeof ctl);
    int ctrl = strcmp(ctl, "none") != 0;
    if (!g_cfg_ok) {
        snprintf(out, cap, "[sc6rb %s %s mode=off] rb=REFUSED:%s", SC6RB_VERSION, g_cfg.title_tag, g_refused[0] ? g_refused : "?");
        return;
    }
    char pl[64]; c1a_title2(pl, sizeof pl, 0);
    /* v0.24.0-C1b2 (C1B-R2-SPEC.md §2.5, §4.3): the mode name, " Z" with zones, lk=on|off (the current step's frame is a zone frame); rblive has
     * rec=<n> (frames recorded) and no t1mm (no reference) */
    int o;
    if (g_nt) {   /* C2 (C2-SPEC.md §2.10, §6.2): no emulator values; " net=<N> ent=<E> role=<r>" is appended at the end */
        o = snprintf(out, cap, "[sc6rb %s %s mode=%s L4=%s L3=%s%s D%d%s%s]%s", SC6RB_VERSION, g_cfg.title_tag, g_mode_names[g_cfg.mode], l4, l3, cam,
                     g_cfg.net_delay, g_cfg.rb_zone ? " Z" : "", ctrl ? " CTRL" : "", pl);
        if (o > 0 && (size_t)o < cap) { char nt[96]; ns_title(nt, sizeof nt); int k2 = snprintf(out + o, cap - (size_t)o, "%s", nt); if (k2 > 0) o += k2; }
        if (o < 0 || (size_t)o >= cap) return;
    } else
    o = snprintf(out, cap, "[sc6rb %s %s mode=%s L4=%s L3=%s%s net=%d/%d/%d D%d%s%s]%s", SC6RB_VERSION, g_cfg.title_tag, g_mode_names[g_cfg.mode], l4, l3, cam,
                     g_cfg.emu_latency_ms, g_cfg.emu_jitter_ms, g_cfg.emu_loss_pct, g_cfg.net_delay, g_cfg.rb_zone ? " Z" : "", ctrl ? " CTRL" : "", pl);
    if (o < 0 || (size_t)o >= cap || g_st == RB_IDLE || g_st == RB_OFF) return;
    const char *st = g_st == RB_ON ? "on" : g_st == RB_ENDED ? (g_abort[0] ? "ABORT:" : "end") : "ABORT:";
    int k = snprintf(out + o, cap - (size_t)o, " rb=%s%s C=%u rbk=%llu d=%llu st=%u", st, (g_st == RB_ABORTED || (g_st == RB_ENDED && g_abort[0])) ? g_abort : "", g_C,
                     (unsigned long long)g_rbk, (unsigned long long)g_dmax, g_es_ok ? g_es.clamps : 0);
    if (k < 0 || (size_t)(o + k) >= cap) return;
    o += k;
    if (g_cfg.rb_zone) { k = snprintf(out + o, cap - (size_t)o, " lk=%s", zone_of(g_s) ? "on" : "off"); if (k < 0 || (size_t)(o + k) >= cap) return; o += k; }
    if (!g_lv) snprintf(out + o, cap - (size_t)o, " t1mm=%u", c1a_t1mm());
}

/* ------------------------------------------------------------------ battle start (f == 1) */
static void sched_build(void) {
    if (g_nt || g_on) { g_N = (uint32_t)g_cfg.live_cap; return; }   /* C2: no emulator schedule in rbnet; optA M2: nor in rbonline */
    uint32_t N = g_lv ? (uint32_t)g_cfg.live_cap : c1a_rec_n();   /* v0.24.0-C1b2: rblive builds the schedule for live_cap (once: valid for any shorter battle) */
    if (g_lv && g_es_ok && g_es.N == N) { g_N = N; return; }
    if (g_es_ok) emu_free(&g_es);
    g_es_ok = 0;
    g_N = N;
    if (!g_N) return;
    EmuCfg ec; memset(&ec, 0, sizeof ec);
    ec.N = g_N; ec.seed = (uint64_t)g_cfg.emu_seed; ec.lat_ms = g_cfg.emu_latency_ms; ec.jit_ms = g_cfg.emu_jitter_ms; ec.loss_pct = g_cfg.emu_loss_pct;
    ec.D = g_cfg.net_delay; ec.R = g_cfg.emu_redundancy; ec.max_rb = g_cfg.net_max_rollback;
    g_es_ok = emu_build(&ec, &g_es);
}
static void battle_start(void) {
    if (g_lv && c1a_demo_battle()) {   /* v0.24.0-C1b2: the title/menu demo battle (UE seed 0x765): no rollback, no recording */
        g_st = RB_OFF;
        LOG("RB off f=1 why=demo_battle (rblive: UE's demo Init seed %u; no rollback, no recording; the battle runs UE's input straight)\n", C1A_DEMO_SEED);
        g_title_dirty = 1;
        return;
    }
    if (!g_lv && !c1a_play_ok()) {
        g_st = RB_OFF;
        if (c1a_play_refused()) LOG("RBABORT f=1 cause=setup unconfirmed=0 (the C1a setup check refused: rb stays off; the battle runs as plain C1a)\n");
        else LOG("RB off f=1 why=no_recording (play is not ok: plain C1a, nothing injected)\n");
        g_title_dirty = 1;
        return;
    }
    if (g_nt && !ns_first_step()) {   /* C2: the barrier was not released, the battle was refused, or the setup changed after the barrier */
        g_st = RB_OFF;
        LOG("RB off f=1 why=net_refused (rbnet: netplay is off for this battle; it runs UE's input straight, locally vs CPU)\n");
        g_title_dirty = 1;
        return;
    }
    if (g_on) {   /* optA M2 */
        g_N = (uint32_t)g_cfg.live_cap; g_remote_changes = g_local_changes = 0;
        LOG("RBNET b=%llu mode=rbonline remote=%d D=%d max_rb=%d live=1 cap=%d armed_round=%u\n", (unsigned long long)g_bat, g_cfg.net_remote_side, rbo_d, g_cfg.net_max_rollback, g_cfg.live_cap, rbo_armed_round);
    } else
    if (g_nt) {   /* C2 */
        g_N = (uint32_t)g_cfg.live_cap; g_remote_changes = g_local_changes = 0;
        LOG("RBNET b=%llu mode=rbnet remote=%d D=%d max_rb=%d live=1 cap=%d\n", (unsigned long long)g_bat, g_cfg.net_remote_side, g_cfg.net_delay, g_cfg.net_max_rollback, g_cfg.live_cap);
    } else {
    if (!g_es_ok) sched_build();
    if (!g_es_ok) { g_st = RB_ON; rb_abort("setup"); return; }
    g_remote_changes = g_local_changes = 0;
    uint32_t pr = 0, pl = 0;
    for (uint32_t f = 1; !g_lv && f <= g_N; f++) {   /* rblive: unknown in advance (-1) */
        const C1aIn *r = rb_truth(f);
        uint32_t hr = (uint32_t)rec_remote_word(r), hl = (uint32_t)(g_cfg.net_remote_side == 2 ? r->p1 : r->p2);
        if (hr != pr) g_remote_changes++;
        if (hl != pl) g_local_changes++;
        pr = hr; pl = hl;
    }
    char hs[256]; emu_hist_str(&g_es, hs, sizeof hs);
    LOG("RBNET b=%llu remote=%d D=%d lat_ms=%d jit_ms=%d loss_pct=%d red=%d seed=%lld frames=%u sched=%016llx lat_hist=%s clamps=%u max_rb=%d remote_changes=%lld local_changes=%lld live=%d cap=%d\n",
        (unsigned long long)g_bat, g_cfg.net_remote_side, g_cfg.net_delay, g_cfg.emu_latency_ms, g_cfg.emu_jitter_ms, g_cfg.emu_loss_pct, g_cfg.emu_redundancy, g_cfg.emu_seed,
        g_N, (unsigned long long)g_es.sched, hs, g_es.clamps, g_cfg.net_max_rollback, g_lv ? -1LL : (long long)g_remote_changes, g_lv ? -1LL : (long long)g_local_changes,
        g_lv, g_lv ? g_cfg.live_cap : 0);
    for (uint32_t f = 1; f <= g_N && g_lines_clamp < 200; f++)
        if (g_es.raw_late[f] > g_cfg.net_max_rollback - 1) {
            g_lines_clamp++;
            if (g_es.raw_late[f] == INT32_MAX) LOG("RBCLAMP f=%u raw=inf used=%d\n", f, g_cfg.net_max_rollback - 1);
            else LOG("RBCLAMP f=%u raw=%d used=%d\n", f, g_es.raw_late[f], g_cfg.net_max_rollback - 1);
        }
    }
    g_step_tid = GetCurrentThreadId();
    g_st = RB_ON;
    g_rb_mem_on = 1;
    g_title_dirty = 1;
    if (g_cfg.rb_zone) zone_add(0, 1, "battle_start");   /* v0.24.0-C1b2: a battle starts inside a zone */
    if (!g_lv && !g_remote_changes) rb_abort("vacuous_remote");
    if (g_pd_pending) { g_pd_pending = 0; rb_abort("pindeny"); }   /* v0.24.0 S1BFIX G2: a pin scan before the first step found a TaskGraph block */
    if (g_q1_pend) { g_q1_pend = 0; rb_abort("q1_walk"); }         /* v0.29.2: the ARM build's walk (rb_arm_f = 0) failed a gameplay entry */
}
/* v0.24.0-C1b2 (C1B-R2-SPEC.md §2.5 Y13): at rb_step entry for step s, before the drain: L[s + D] = UE's FrameIn of step s, verbatim. Frames
 * 1..D: p1 = 0, x = 0, p2 / ff = UE's at step 1, except L[D].x[0..8) = UE's p1 at step 1 (x(f)[0..8) = p1(f + 1)). RBLIVEIN when UE's pressed
 * half is not held & ~prev_held. */
static void live_capture(uint32_t s, const C1aIn *ue) {
    uint32_t D = (uint32_t)g_cfg.net_delay;
    if (s == 1)
        for (uint32_t f = 1; f <= D && f < g_Lcap; f++) {
            memset(&g_L[f], 0, sizeof g_L[f]); g_L[f].p2 = ue->p2; g_L[f].ff = ue->ff; g_Lok[f] = 1;
            if (f == D) memcpy(g_L[f].x, &ue->p1, 8);
        }
    if (s + D < g_Lcap) { g_L[s + D] = *ue; g_Lok[s + D] = 1; }
    uint32_t held = (uint32_t)ue->p1, pressed = (uint32_t)(ue->p1 >> 32);
    if (pressed != (held & ~g_live_prev_held)) {
        g_livein_bad++;
        if (g_livein_lines < 20) { g_livein_lines++; LOG("RBLIVEIN f=%u held=%08x pressed=%08x prev=%08x\n", s, held, pressed, g_live_prev_held); }
    }
    g_live_prev_held = held;
}

/* C2 (C2-SPEC.md §2.5): rbnet capture. The session fills the local table (the owner sample with the zone-edge fill / merge); the frames the
 * session knows are composed into the live table g_L (bit 1 the local side, bit 2 the peer's; ff = 7). F = the frames with both sides. */
static void net_compose(uint32_t s, const C1aIn *ue) {
    ns_capture(s, ue);
    uint32_t ln = ns_local_next(), rf = ns_frontier();
    if (ln && ln - 1 > g_nsl) {
        for (uint32_t k = g_nsl + 1; k < ln && k < g_Lcap; k++) {
            C1aIn l; if (!ns_local(k, &l)) break;
            if (!(g_Lok[k] & 2)) { memset(&g_L[k], 0, sizeof g_L[k]); }
            if (ns_remote_side() == 2) { g_L[k].p1 = l.p1; memcpy(g_L[k].x, l.x, 0x18); } else g_L[k].p2 = l.p2;
            g_L[k].ff = 7; g_Lok[k] |= 1; g_nsl = k;
        }
    }
    if (rf > ln + 64) rf = ln + 64;   /* the script peer's frontier is unbounded */
    for (uint32_t k = g_nsr + 1; k <= rf && k < g_Lcap; k++) {
        C1aIn r; if (!ns_remote(k, &r)) break;
        if (!(g_Lok[k] & 1)) { memset(&g_L[k], 0, sizeof g_L[k]); }
        if (ns_remote_side() == 2) g_L[k].p2 = r.p2; else { g_L[k].p1 = r.p1; memcpy(g_L[k].x, r.x, 0x18); }
        g_L[k].ff = 7; g_Lok[k] |= 2; g_nsr = k;
    }
}

/* optA M2 (OPTA-M2-SPEC.md §5.4): rbonline capture. L[s] bit 1 = UE's words of step s (the local word FetchInput built, the remote's as it ran,
 * the third block); bit 2 = the remote word of every newly confirmed frame by the stock rule on the confirmed raws (src/rbo_fetch.c, the game's
 * own ring filled by its own ReceiveInputs). F = the confirmed frames, capped at the captured ones. */
static void rbo_compose(uint32_t s, const C1aIn *ue) {
    if (s < g_Lcap) { g_L[s] = *ue; g_L[s].ff = 7; g_Lok[s] = 1; g_nsl = s; }
    uint32_t front = rbo_fetch_frontier();
    for (uint32_t k = g_nsr + 1; k <= front && k <= s && k < g_Lcap; k++) {
        uint64_t lw, rw; int known;
        rbo_fetch_truth(k, &lw, &rw, &known);
        if (!known) break;
        if (g_cfg.net_remote_side == 2) g_L[k].p2 = rw; else g_L[k].p1 = rw;
        g_Lok[k] |= 2; g_nsr = k;
        if (g_nunv) unv_check(k, rw, known == 2);   /* v0.29.1 (B2): a frame committed without a correcting core: verified now */
    }
}
/* optA M2 v0.29.1 (review B2): a straight pass after an abort. A frame that ran on an unconfirmed (predicted) word is verified when its truth
 * arrives; the pass's tier-1 goes to CHECK like a confirmed frame's (it is committed state), outside the aborted side's own conservative zone
 * rule (rb's zone tracking stops with the core): not the fight mode, the short replay, or a state-class event, plus rb_zone_exit_margin
 * frames after the last such frame. Zones only remove CHECKs (a frame checked by one side only is never compared). */
static void rbo_straight_aborted(uint32_t f, const C1aIn *use, Pend *p) {
    if (!(g_Lok && f < g_Lcap && (g_Lok[f] & 2))) unv_add(f, rec_remote_word(use));
    uint32_t mode = *(volatile uint32_t *)VA(0x144846364ULL);
    int rp = *(volatile uint32_t *)VA(0x1448463f8ULL) != 0, ev = 0;
    for (int i = 1; i < LISTENER_NSLOTS; i++) if (((g_cfg.rb_state_mask >> i) & 1) && g_pass.calls[i] && i != 0xc0 / 8) ev = 1;
    if (mode != (uint32_t)g_cfg.rb_zone_fight_mode || rp || ev) { g_ab_lastz = f; g_ab_zset = 1; }
    if (!g_ab_zset || f > g_ab_lastz + (uint32_t)g_cfg.rb_zone_exit_margin) rbo_core_confirmed_frame(f, &p->pass);
    else rbo_core_zone_frame(f);   /* v0.32.0 review fix M1: the aborted side's own zone rule sends the open chain too (the peer mirrors it) */
}

/* ------------------------------------------------------------------ one step */
static void straight_pass(uint32_t f, const C1aIn *use, int aborted) {
    FrameIn run; fi_from(&run, use);
    if (aborted) { memset(&g_pass, 0, sizeof g_pass); g_pass.type = PT_STRAIGHT; g_pass.k = f; g_pass.s = f; }
    run_fi(&run, 0, f, RB);
    g_pass.type = PT_NONE;
    def_confirm_before((uint64_t)f + 1);
    if (!aborted) { c1a_step_end(f); return; }
    Pend *p = pend_new(f);
    c1a_cur_ifc(p->ifc, sizeof p->ifc, &p->q);
    p->used = *use; p->ps = 'C';
    c1a_pass_compute(f, use, p->ifc, p->q, &p->pass, NULL);
    const C1aIn *r = rb_truth(f);
    char sfx[176] = "";
    if (r) { char xr[0x18 * 2 + 1]; for (int i = 0; i < 0x18; i++) snprintf(xr + 2 * i, 3, "%02x", r->x[i]);
             snprintf(sfx, sizeof sfx, "ps=C lt=0 ok=%d p1r=%016llx xr=%s z=0", used_ok(f, use), (unsigned long long)r->p1, xr); }
    if (g_nt) { size_t o = strlen(sfx); snprintf(sfx + o, sizeof sfx - o, "%sued=%08x", o ? " " : "", pass_ued(&p->pass)); }   /* C2 fix r1 (6.4) */
    p->pass.ps = 'C';
    c1a_confirm(&p->pass, sfx);
    if (g_on && !RBT(g_rbt_abort_old)) rbo_straight_aborted(f, use, p);   /* optA M2 v0.29.1 (B2): verify an unconfirmed word later; CHECK keeps running */
    p->valid = 0;
    c1a_step_rt();
}
static int ifc_has_88(const char *ifc) {
    for (const char *p = ifc; (p = strstr(p, "88:")) != NULL; p++) if (p == ifc || p[-1] == ',') return 1;
    return 0;
}
/* rollback to r (C1B-SPEC §2.4 step 8). 0 = refused (the caller aborts) */
static int do_rollback(uint32_t r, uint32_t s, uint32_t F, uint32_t top) {
    RbEnt *er = ring_get(r);
    if (!er) { LOG("RBREFUSED s=%u r=%u barrier=%u barrier_cause=%s kind=ring (the ring entry is gone)\n", s, r, g_b, g_bcause); g_refused_n++; rb_abort("layout"); return 0; }
    pk_save();
    int pg = pinguard(&er->pre, r, "rollback");
    if (pg == 2) { rb_abort("layout"); return 0; }
    if (pg == 1) { rb_abort("pinguard"); return 0; }
    SnapSet *ss = modes_ssw();
    if (!g_cfg.rb_skip_restore) lipsync_before_restore(LIPS_SITE_ROLLBACK);   /* LIPSYNC: P never rolls back (hook pair) */
    if (!g_cfg.rb_skip_restore) { if (!snap_restore_ex(ss, &er->pre)) { g_layout++; rb_abort("layout"); return 0; } }
    if (!g_cfg.rb_skip_restore) lipsync_after_restore(LIPS_SITE_ROLLBACK);    /* LIPSYNC: */
    def_drop_from(r);
    dp_drop_from(r);   /* v0.23.1 (F10): the dropped frees' pins are visible again */
    for (uint32_t k = r; k < s; k++) {
        RbEnt *ek = ring_slot(k);
        if (k > r) {
            if (ek->valid && ek->f == k) fp_apply(&ek->fp);
            if (!snap_take(ss, &ek->pre)) { LOG("RB WARN snap_take failed k=%u\n", k); rb_abort("snap_alloc"); return 0; }
            ek->f = k; ek->valid = 1;
        }
        FrameIn in; make_inputs(k, F, &in);
        int conf = k <= top;
        pass_run(conf ? PT_CRESIM : PT_URESIM, k, s, &in, conf);
        pass_record(k, &in, 'R');
    }
    pk_apply(s);
    return 1;
}
/* C1B3 fix (round-3 review, RPB use-after-free): the RPB pass delivers a muted call against ring[k].pre, then the scratch restore brings the
 * present back. If UE freed a block inside that call (a pinned block quarantined at q = k + 1, or any block a core .bss word pointed to,
 * kept by L4DANGLE), the restore put the old pointer back and the quarantine freed the block at H = k + 1: a use-after-free. Now:
 *  1. every word L4 kept in the RPB pass gets UE's value back in the present wherever the present still points into the freed block;
 *  2. frames before s are frozen (RBBARRIER f=s-1 cause=rpb_free): ring[k+1 .. s-1].pre were taken before the free and hold the old
 *     pointer, so no rollback may restore them (a later misprediction there is RBREFUSED -> RBABORT, loud);
 *  3. more kept words than the record holds (1,024) cannot be put back: rb_abort("rpb_dangle_overflow"), loud.
 * BLIND SPOTS: pointers into the freed block held outside core .bss (in pinned blocks, the arena) are not re-applied; L4 checks only the
 * first L4F_MAX (4,096) frees of a call (L4DANGLE overflow). */
static void rpb_free_fix(uint32_t k, uint32_t s, uint64_t pq0) {
    int kept = 0, over = 0, fixed = RBT(g_rbt_rpb_old) ? 0 : modes_l4kept_reapply(&kept, &over);
    modes_l4kept_arm(0);
    int pq = (int)(g_pinq - pq0);
    if (RBT(g_rbt_rpb_old) || (!kept && !pq)) return;
    LOG("RBRPBFREE k=%u s=%u kept=%d reapplied=%d pinq_added=%d overflow=%d (UE freed blocks inside the RPB's call: UE's values put back in the present; frames < %u frozen)\n",
        k, s, kept, fixed, pq, over, s);
    barrier(s - 1, "rpb_free", 1);
    if (over) rb_abort("rpb_dangle_overflow");
}
static void do_rpb(uint32_t k, uint32_t s) {
    uint64_t t0 = qpc(); double d0 = g_diag;
    RbEnt *ek = ring_get(k);
    Pend *p = pend_at(k);
    if (!ek) { g_statelost++; LOG("RBSTATELOST f=%u slot=+0x%03x cause=ring\n", k, p ? p->pend_slot * 8 : 0); if (p) p->pend_st = 0; return; }
    SnapSet *ss = modes_ssw();
    int pg = pinguard(&ek->pre, k, "rpb");
    if (pg) { g_statelost++; LOG("RBSTATELOST f=%u slot=+0x%03x cause=%s\n", k, p ? p->pend_slot * 8 : 0, pg == 2 ? "layout" : "pinguard"); if (p) p->pend_st = 0; return; }
    if (!snap_take(ss, &g_scratch)) { LOG("RB WARN scratch snapshot failed\n"); rb_abort("snap_alloc"); return; }
    def_save();
    int dp0 = g_ndp;   /* v0.23.1 (F10): the deferred-pin entries the RPB pass adds go with def_restore */
    lipsync_before_restore(LIPS_SITE_RPB);   /* LIPSYNC: P never rolls back (hook pair) */
    snap_restore_ex(ss, &ek->pre);
    lipsync_after_restore(LIPS_SITE_RPB);    /* LIPSYNC: */
    const C1aIn *r = rb_truth(k);
    FrameIn in; memset(&in, 0, sizeof in); if (r) fi_from(&in, r);
    uint64_t pq0 = g_pinq;
    modes_l4kept_arm(!RBT(g_rbt_rpb_old));   /* C1B3 fix: record the words L4 keeps in this pass */
    pass_run(PT_RPB, k, s, &in, 1);
    if (p) { for (int i = 0; i < LISTENER_NSLOTS; i++) p->fwd[i] = (uint16_t)(p->fwd[i] + g_pass.fwd[i]); p->pend_st = 0; }
    g_pass.type = PT_NONE;
    lipsync_before_restore(LIPS_SITE_RPB_SCRATCH);   /* LIPSYNC: P never rolls back (hook pair) */
    snap_restore_ex(ss, &g_scratch);
    lipsync_after_restore(LIPS_SITE_RPB_SCRATCH);    /* LIPSYNC: */
    rpb_free_fix(k, s, pq0);   /* C1B3 fix: UE's kept values back in the present; frames < s frozen */
    def_restore();
    dp_truncate(dp0);
    g_rpb++;
    double core = qpc_us(qpc() - t0) - (g_diag - d0);
    LOG("RBRPB s=%u k=%u barrier=%u us_core=%.0f\n", s, k, g_b, core);
}

static void rb_step1(void *iface, void *gin, FrameIn *fi, uint64_t seq);
void rb_step(void *iface, void *gin, FrameIn *fi, uint64_t seq) {
    g_step_depth = -1;
    rb_step1(iface, gin, fi, seq);
    if (g_nt) ns_step_done(g_s, g_C, g_step_depth);   /* C2: lastFrame, C, depth; the per-step control-type assert */
}
static void rb_step1(void *iface, void *gin, FrameIn *fi, uint64_t seq) {
    (void)iface; (void)gin;
    modes_l4_times(&g_l4_0, &g_l4m_0); g_step_fwd = 0;
    uint64_t t_entry = g_hk_entry_valid ? g_hk_entry_qpc : qpc();
    g_diag = 0;
    if (!g_step_tid) g_step_tid = GetCurrentThreadId();
    if (g_tick_steps == 1) { tick_close(); g_tk.id = ++g_tick_ctr; }
    uint32_t f = c1a_next_f();
    if (f == 1) sched_build();
    if (g_st == RB_ON && g_N && f > g_N) rb_finish(g_lv ? "live_cap" : "rec_end");   /* the tail while the C1a comparison is still on; rblive: RBEND why=live_cap */
    if (g_st == RB_ON && !g_tk.steps) g_tk.s0 = f;
    g_tk.steps++;
    C1aIn ue, use;
    c1a_from(&ue, fi);
    c1a_step_begin(seq, &ue, &use, 0);
    f = c1a_next_f() - 1;
    g_s = f;
    if (f == 1 && g_st == RB_IDLE) battle_start();
    if (g_nt && g_st == RB_ON) { char ac[40]; if (ns_want_abort(ac, sizeof ac)) rb_abort(ac); }   /* C2: a session abort (desync, timeout, quit, ...) */
    if (g_lv && g_st != RB_OFF && g_st != RB_IDLE) {   /* v0.24.0-C1b2 rblive: capture, then every pass of f (and every straight pass) runs the delayed record */
        if (g_on) rbo_compose(f, &ue); else   /* optA M2 */
        if (g_nt) net_compose(f, &ue); else   /* C2 */
        live_capture(f, &ue);
        const C1aIn *t = rb_truth(f);
        if (g_on && t && !(g_Lok[f] & 2)) t = NULL;   /* optA M2: an unconfirmed frame runs UE's words (local + the prediction FetchInput made) */
        if (t) { use = *t; c1a_set_cur_in(&use); }
    }
    if (g_st == RB_ON && modes_cam_fail()) rb_abort("campin_fail");
    if (g_st != RB_ON) { straight_pass(f, &use, g_st == RB_ABORTED); return; }
    modes_q1cov_step(f, 0);   /* v0.29.2 Q1COV (rbreplay q1_cov=1; nothing otherwise) */

    /* step entry */
    char ifc[C1A_IFC_CAP]; uint32_t q; c1a_cur_ifc(ifc, sizeof ifc, &q);
    if (ifc_has_88(ifc)) { g_ueskip++; LOG("RBUESKIP f=%u\n", f); }
    Pend *ps0 = pend_new(f); snprintf(ps0->ifc, sizeof ps0->ifc, "%s", ifc); ps0->q = q;
    pbar_apply();
    if (g_cfg.h_barrier_at && f == (uint32_t)g_cfg.h_barrier_at) {   /* v0.24.0 harness-only (GLIVE-AB): a forced mid-fight repin barrier */
        LOG("RBPINS test_barrier f=%u (h_barrier_at: a forced repin barrier at %u)\n", f, f - 1);
        barrier(f - 1, "repin:test", 1);
    }
#ifdef SC6RB_HARNESS
    if (g_nt && g_cfg.h_c2_fbar_after && f > (uint32_t)g_cfg.h_c2_fbar_after && !zone_of(f - 1) && (long long)f - 1 - (long long)g_C >= 2)   /* C2 NBAR (harness only): */
        barrier(f - 1, "repin:test", 1);   /* a forced barrier on every fight frame with >= 2 unconfirmed frames after h_c2_fbar_after, until a misprediction lands behind one */
#endif
    if (g_cfg.rb_test_layout_at && f == (uint32_t)g_cfg.rb_test_layout_at) {   /* control GL: a forced non-prefix layout (no barrier) */
        SnapSet *ss = modes_ssw(); spin_lock(modes_pin_lock()); ss->gen = rb_layout_gen(); spin_unlock(modes_pin_lock());
        LOG("RBPINS rebuild f=%u why=test_layout gen=%u (rb_test_layout_at: no barrier; a rollback before it must refuse)\n", f, ss->gen);
    }
    if (f > g_C + (uint32_t)g_R - 1 || f > g_C + PEND_N - 2) {
        g_refused_n++;
        LOG("RBREFUSED s=%u r=%u barrier=%u barrier_cause=%s kind=depth (unconfirmed frames exceed the ring)\n", f, g_C + 1, g_b, g_bcause);
        rb_abort("depth"); straight_pass(f, &use, 1); return;
    }
    RbEnt *es = ring_slot(f);
    es->valid = 0; es->f = f;
    es->fp.n = 0; es->fp.used = 0; es->fp.overflow = 0;
    {   /* v0.23.1 (review F4/F8): a between-step patch that is incomplete (more than FP_MAXR ranges / FP_BYTES bytes, a pin offset not
         * found) or missing (the previous live pass's post-snapshot failed) must never be re-applied by a rollback: barrier at f-1, so
         * no rollback target <= f-1 (the only ones that re-apply ring[f].fp) is accepted (RBREFUSED -> RBABORT if one is needed) */
        int fp_ok = g_post_ok && g_post_f == f - 1;
        if (fp_ok) fp_capture(&es->fp, &g_post);
        if (fp_ok && es->fp.overflow && !RBT(g_rbt_no_fpov)) {
            g_fpov++;
            LOG("RBFP overflow f=%u ranges=%d bytes=%u dropped=%d max_ranges=%d max_bytes=%d (UE's between-step writes do not fit the patch: no rollback may re-apply it)\n",
                f, es->fp.n, es->fp.used, es->fp.overflow, FP_MAXR, FP_BYTES);
            barrier(f - 1, "fp_overflow", 1);
        } else if (!fp_ok && g_post_want && !RBT(g_rbt_no_fpov)) {
            g_fpmiss++;
            LOG("RBFP missing f=%u (no post-snapshot of frame %u: the between-step writes are unknown)\n", f, f - 1);
            barrier(f - 1, "fp_missing", 1);
        }
        g_post_want = 0;
    }
    int rp = *(volatile uint32_t *)VA(0x1448463f8ULL) != 0;
    if (rp != g_rp_prev) {
        if (rp) {
            barrier(f - 1, "replay_start", 1);
            g_seg = 1; g_seg_start = f; g_seg_frames = 0; g_seg_verified = 0;
            LOG("RBSEG enter f=%u barrier=%u\n", f, g_b);
        } else {
            barrier(f - 1, "replay_end", 1);
            LOG("RBSEG exit f=%u frames=%u verified=%u mispredict=%u\n", f - 1, g_seg_frames, g_seg_verified, g_seg_mp);
            g_seg = 0;
        }
        g_rp_prev = rp;
    }

    /* drain */
    uint32_t F = rb_frontier(f);   /* v0.24.0-C1b2: + the zones (rb_zone=1) */
    g_F = F;
    uint32_t top = F < f - 1 ? F : f - 1;
    uint32_t minI = 0, minP = 0, minPb = 0; int nI = 0, nP = 0;
    for (uint32_t k = g_C + 1; k <= top; k++) {
        Pend *p = pend_at(k);
        if (!p) continue;
        if (!used_ok(k, &p->used)) {
            nI++; if (!minI) minI = k;
            if (p->ps == 'S') {
                g_seg_mp++;
                LOG("RBSEG mispredict f=%u\n", k);
                rb_abort("seg_mispredict"); straight_pass(f, &use, 1); return;
            }
        }
        if (p->pend_st) { nP++; if (!minP) minP = k; if (k <= g_b && !minPb) minPb = k; }
    }
    /* decide */
    int rolled = 0, depth = -1; uint32_t r = 0;
    int can = g_tk.reruns == 0 || !g_cfg.rb_tick_limit;
    if (minI && minI <= g_b) {
        g_refused_n++;
        LOG("RBREFUSED s=%u r=%u barrier=%u barrier_cause=%s kind=input\n", f, minI, g_b, g_bcause);
        rb_abort("refused"); straight_pass(f, &use, 1); return;
    } else if (minPb) {
        if (can) {
            confirm_upto(minPb - 1, f);   /* frames before k are clean (an earlier I or P at or before the barrier would be k) */
            do_rpb(minPb, f);
            if (g_st != RB_ON) { straight_pass(f, &use, 1); return; }
            confirm_upto(minPb, f);       /* C = k: its earlier pass stays final (its inputs were equal) */
            g_tk.reruns++; depth = 8;
        } else g_tk.deferred = 1;
    } else if (nI || nP) {
        r = minI && (!minP || minI < minP) ? minI : minP;
        if (!g_cfg.rb_rollback) {   /* control: never roll back; the predictions are committed */
            confirm_upto(top, f);
        } else if (f - r > (uint32_t)g_cfg.net_max_rollback) {
            g_refused_n++;
            LOG("RBREFUSED s=%u r=%u barrier=%u barrier_cause=%s kind=depth\n", f, r, g_b, g_bcause);
            rb_abort("depth"); straight_pass(f, &use, 1); return;
        } else if (can) {
            if (!do_rollback(r, f, F, top)) { straight_pass(f, &use, 1); return; }
            rolled = 1; depth = (int)(f - r); g_step_depth = depth;
            g_rbk++; g_tk.reruns++;
            if (depth < 16) g_dhist[depth]++;
            if ((uint64_t)depth > g_dmax) g_dmax = (uint64_t)depth;
            if (nI && nP) g_cboth++; else if (nI) g_cin++; else g_cst++;
            confirm_upto(top, f);
        } else g_tk.deferred = 1;
    } else confirm_upto(top, f);
    if (lipsync_guard_tripped()) { rb_abort("lipsync_guard"); straight_pass(f, &use, 1); return; }   /* LIPSYNC: LIPSYNCGUARD FAIL -> no further restore */

    /* the live pass (or the verify-only segment pass) */
    {
        FrameIn in; make_inputs(f, F, &in);
        int known = f <= F, final = known && g_C == f - 1;
        if (g_cfg.rb_arm_f > 0 && !g_lv && f < (uint32_t)g_cfg.rb_arm_f && !final) {   /* v0.29.3 (M2, §16.14): the guard of the pre-F rule */
            /* every frame before F runs final (lockstep) and unrouted: its allocations land on the heap outside the snapshot set, so it must
             * never be predicted, never rolled back. v0.29.2 aborted here on any non-zone frame (arm_f_not_zone); now only a frame that
             * would really run predicted aborts (reached only under the control h_rb_armf_zone=1 or a frontier bug) */
            LOG("RBARMF f=%u arm_f=%d known=%d C=%u zone=%d (a frame before rb_arm_f would run predicted: its allocations are not routed, a rollback could need it)\n",
                f, g_cfg.rb_arm_f, known, g_C, zone_of(f));
            rb_abort("arm_f_predicted"); straight_pass(f, &use, 1); return;
        }
        uint64_t c0 = modes_lsn_c0_calls();
        g_live_pre = f;   /* v0.23.1 (F2): from here to the end of the pass, an outside free gets q = f + 1 */
        if (!g_seg) {
            if (rolled && es->fp.n) fp_apply(&es->fp);
            if (!snap_take(modes_ssw(), &es->pre)) { g_live_pre = 0; LOG("RB WARN snap_take failed f=%u\n", f); rb_abort("snap_alloc"); straight_pass(f, &use, 1); return; }
            es->f = f; es->valid = 1;
        }
        /* v0.23.1 (review F1/F7) rb_state_quiet=N: a predicted live pass whose remote record has been constant over the last N known frames
         * (and whose predicted frames since then repeat it) forwards its state-class calls at once (pass=quiet_live) and sets RBBARRIER
         * f=<f> cause=state_quiet, so UE reacts on the recorded frame (C1a: UE's +0xe8 x4 right after the f844 +0x058). The pass is then
         * final or the battle aborts loud: a later misprediction at or before f is RBREFUSED kind=input. */
        g_next_quiet = !g_seg && !final && g_cfg.rb_state_quiet > 0 && g_C == top && remote_quiet(F, f, &in);
        if (rolled) modes_q1cov_step(f, 1);   /* v0.29.2 Q1COV */
        pass_run(g_seg ? PT_SEG : PT_LIVE, f, f, &in, final);
        modes_q1cov_step(f, 2);               /* v0.29.2 Q1COV */
        int qf = g_pass.quiet_fwd;
        pass_record(f, &in, g_seg ? 'S' : known ? 'C' : 'L');
        g_live_done = f; g_live++;
        g_live_pre = 0;
        if (g_nt && f == 1 && !g_seg && !entry_step1_check()) ns_ctltype(1, 1, 0, 0);   /* C2: ENTRY CHECK after step 1 (§3.2 step 4) */
        else if (g_nt && f > 1 && entry_converted()) entry_step_assert(f);              /* C2: the per-step control-type assert, before this step's confirmation */
        if (g_seg) g_seg_frames++;
        if (modes_lsn_c0_calls() != c0) barrier(f, "lsn_c0", 0);
        if (qf) { g_quiet_fwd += (uint64_t)qf; barrier(f, "state_quiet", 1); }
        if (!g_seg) { g_post_ok = snap_take(modes_ssw(), &g_post); g_post_f = f; g_post_want = 1; } else g_post_ok = 0;
        if (final) confirm_upto(f, f);
        if (g_seg) { uint32_t v = 0; for (uint32_t k = g_seg_start; k <= g_C; k++) v++; g_seg_verified = v; }
    }
    /* release horizon */
    uint32_t H = g_C + 1;
    for (uint32_t k = g_C + 1; k <= g_live_done; k++) { Pend *p = pend_at(k); if (p && p->pend_st) { if (k < H) H = k; break; } }
    def_confirm_before(H);
    pinq_release(H, 0);
    if (g_ndp) { Spin *pl = modes_pin_lock(); spin_lock(pl); dp_compact(); spin_unlock(pl); }   /* v0.23.1: confirmed entries are tombstones */
    c1a_step_rt();
    if (g_live % 60 == 0) g_title_dirty = 1;
    /* cost */
    double l4n, l4mn; modes_l4_times(&l4n, &l4mn);
    double dl4 = l4n - g_l4_0, dl4m = l4mn - g_l4m_0;
    g_diag += dl4m;   /* C2 (§5.3): L4MISS is a diagnostic watchdog: its time is diag, not core (p99_l4miss_us logs it apart) */
    if (!g_l4m_bk) g_l4m_bk = (float *)malloc(sizeof(float) * BK_CAP);
    if (g_l4m_bk && g_l4m_n < BK_CAP) g_l4m_bk[g_l4m_n++] = (float)dl4m;
    double total = qpc_us(qpc() - t_entry), core = total - g_diag;
    bk_add(depth < 0 ? 0 : depth, core, g_diag);
    if (rolled && core > 4000.0) LOG("RBKC b=%llu k=%u depth=%d us=%.0f fwd=%u l4us=%.0f l4miss_us=%.0f\n", (unsigned long long)g_bat, f, depth, core, g_step_fwd, dl4, dl4m);   /* C2 (§5.3) */
    if (rolled) LOG("RBK s=%u r=%u d=%d cause=%s C=%u tick=%llu nstep=%d us_core=%.0f us_diag=%.0f\n", f, r, depth, nI && nP ? "both" : nI ? "input" : "state", g_C,
                    (unsigned long long)g_tk.id, g_tk.steps, core, g_diag);
    if (rolled && g_on) rbo_core_note_rollback(f, r, depth, nI && nP ? "both" : nI ? "mispredict" : "state");   /* optA M2: RB ROLLBACK */
    if (g_route_broken || g_defer_broken) rb_abort("route_or_defer_broken");
    if (lipsync_guard_tripped()) rb_abort("lipsync_guard");   /* LIPSYNC: a FAIL at the live pass (site rpb_scratch) */
}

/* ------------------------------------------------------------------ C2 exports (netsess.c) */
int rb_zone_of(uint32_t f) { return zone_of(f); }
uint32_t rb_last_step(void) { return g_s; }
uint64_t rb_battle_no(void) { return g_bat; }   /* C2 fix r1 */
double rb_p99_core(void) {
    double m = 0;
    for (int b = 0; b < NBK; b++) {
        uint32_t n = g_bk_n[b]; if (n < 20) continue;
        float *c = (float *)malloc(sizeof(float) * n); if (!c) continue;
        memcpy(c, g_bk_core[b], sizeof(float) * n); qsort(c, n, sizeof(float), fcmp);
        double p = pct(c, n, 0.99); if (p > m) m = p; free(c);
    }
    return m;
}
void rb_p99_by_depth(char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int b = 0; b < NBK; b++) {
        uint32_t n = g_bk_n[b]; if (!n) continue;
        float *c = (float *)malloc(sizeof(float) * n); if (!c) continue;
        memcpy(c, g_bk_core[b], sizeof(float) * n); qsort(c, n, sizeof(float), fcmp);
        int k = snprintf(out + o, cap - o, "%s%s:%.0f", o ? "," : "", b == 8 ? "rpb" : (char[2]){ (char)('0' + b), 0 }, pct(c, n, 0.99));
        if (k > 0 && (size_t)k < cap - o) o += (size_t)k;
        free(c);
    }
    if (!o) snprintf(out, cap, "-");
}
uint32_t rb_confirmed(void) { return g_C; }
int rb_is_on(void) { return g_st == RB_ON; }
/* optA M2 (rbonline) */
void rb_abort_ext(const char *cause) { if (g_st == RB_ON) rb_abort(cause); }
void rb_q1_walk_failed(void) {   /* v0.29.2 (rbreplay): RBABORT cause=q1_walk; title q1=FAIL:E# (modes.c): the run is invalid and says so */
    if (g_st == RB_ON) rb_abort("q1_walk"); else if (g_st == RB_IDLE) g_q1_pend = 1;
}
uint32_t rb_rbk(void) { return (uint32_t)g_rbk; }
uint32_t rb_dmax(void) { return (uint32_t)g_dmax; }
void rb_dhist_str(char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int d = 0; d < 16; d++) if (g_dhist[d]) { int k = snprintf(out + o, cap - o, "%s%d:%llu", o ? "," : "", d, (unsigned long long)g_dhist[d]); if (k > 0 && (size_t)k < cap - o) o += (size_t)k; }
    if (!o) snprintf(out, cap, "-");
}
