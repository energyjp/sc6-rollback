#include "common.h"
#include "pres.h"
#include "c1a.h"
#include "emu.h"
#include "snap.h"
#include <stdlib.h>
#include <ctype.h>

Config g_cfg;
const char *g_mode_names[MODE_COUNT] = { "off", "observe", "sync1", "roll7", "loopback", "sync1x", "record", "replay", "rbreplay", "rblive", "rbnet", "netobserve", "rbonline" };   /* C2: + rbnet; optA M1: + netobserve; optA M2: + rbonline */

int mode_from_name(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    for (int i = 0; i < MODE_COUNT; i++) {
        size_t n = strlen(g_mode_names[i]);
        if (!_strnicmp(s, g_mode_names[i], n) && !isalnum((unsigned char)s[n])) return i;
    }
    return -1;
}

static const char *SEC = "sc6rb";
static char g_ini[MAX_PATH];

static int geti(const char *k, int def) { return (int)GetPrivateProfileIntA(SEC, k, def, g_ini); }
static void gets_(const char *k, const char *def, char *out, DWORD cap) { GetPrivateProfileStringA(SEC, k, def, out, cap, g_ini); }
int  cfg_geti(const char *k, int def) { return geti(k, def); }                                       /* optA M2 (common.h) */
void cfg_gets(const char *k, const char *def, char *out, unsigned cap) { gets_(k, def, out, (DWORD)cap); }   /* optA M2 (common.h) */

static uintptr_t parse_hex(const char **p) {
    while (**p == ' ' || **p == ',' || **p == '\t') (*p)++;
    char *e; unsigned long long v = strtoull(*p, &e, 0);
    if (e == *p) return 0;
    *p = e;
    return (uintptr_t)v;
}

/* "VA:len, VA:len" */
static int parse_ranges(const char *s, uintptr_t *va, size_t *len, int max) {
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        uintptr_t a = parse_hex(&p);
        if (!a) break;
        size_t l = 8;
        if (*p == ':') { p++; l = (size_t)parse_hex(&p); }
        va[n] = a; len[n] = l; n++;
        while (*p == ' ' || *p == ',') p++;
    }
    return n;
}

static int keyval(const char *k, int def) {
    char b[32]; gets_(k, "", b, sizeof b);
    if (!b[0]) return def;
    if ((b[0] == 'F' || b[0] == 'f') && isdigit((unsigned char)b[1])) {
        int n = atoi(b + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return (int)strtol(b, NULL, 0);
}

void config_load(const char *ini_path) {
    strncpy(g_ini, ini_path, sizeof g_ini - 1);
    Config *c = &g_cfg;
    memset(c, 0, sizeof *c);
    char b[1024];

    gets_("mode", "observe", b, sizeof b);
    c->mode = mode_from_name(b); if (c->mode < 0) c->mode = MODE_OBSERVE;

    gets_("cycle", "observe,sync1,roll7,loopback", b, sizeof b);
    for (char *t = strtok(b, ", "); t && c->ncycle < MODE_COUNT; t = strtok(NULL, ", ")) {
        int m = mode_from_name(t);
        if (m > MODE_OFF) c->cycle[c->ncycle++] = m;
    }
    if (!c->ncycle) { c->cycle[0] = MODE_OBSERVE; c->ncycle = 1; }

    c->key_cycle = keyval("hotkey_cycle", VK_F9);
    c->key_mark = keyval("hotkey_mark", VK_F10);
    c->key_lbplayer = keyval("hotkey_loopback_player", VK_F11);

    gets_("log_dir", "", c->log_dir, sizeof c->log_dir);   /* 0.34.1: empty = sc6rb_logs next to the plugin (src/main.c) */
    c->flush_every = geti("flush_every", 60); if (c->flush_every < 1) c->flush_every = 60;
    c->step_log = geti("step_log", 1);
    c->ws_frames = geti("ws_frames", 600);
    c->census = geti("census", 1);
    c->proxy = geti("listener_proxy", 1);
    c->arena_mb = geti("arena_mb", 64); if (c->arena_mb < 4) c->arena_mb = 4; if (c->arena_mb > 1024) c->arena_mb = 1024;
    c->init_routing = geti("init_routing", 0);
    c->pin_containers = geti("pin_containers", 1);
    c->scan_containers = geti("scan_containers", 1);
    c->roll_depth = geti("roll_depth", 7); if (c->roll_depth < 1) c->roll_depth = 1; if (c->roll_depth > 30) c->roll_depth = 30;
    c->fake_delay = geti("fake_delay", 7); if (c->fake_delay < 1) c->fake_delay = 1; if (c->fake_delay > 30) c->fake_delay = 30;
    c->lb_player = geti("loopback_player", 2); if (c->lb_player != 1) c->lb_player = 2;
    gets_("ai_rand", "crt", b, sizeof b);
    c->ai_rand_det = !_strnicmp(b, "det", 3);
    c->max_reports = geti("max_reports", 20);
    c->max_diff_ranges = geti("max_diff_ranges", 24);
    c->ifc_census = geti("ifc_census", 1);
    c->check_foreign = geti("check_foreign", 1);
    c->crt_hooks = geti("crt_hooks", 1);
    c->lsnw = geti("listener_write_learning", 1);
    c->extw_autopin = geti("extw_autopin", 1);
    gets_("camera", "include", b, sizeof b);
    c->camera_exclude = !_strnicmp(b, "excl", 4);
    c->camera_resync = geti("camera_resync", 1);
    c->key_camera = keyval("hotkey_camera", VK_F8);   /* F12 is the Steam screenshot key */
    c->extw_frames = geti("extw_frames", 30);
    c->track_init = geti("track_init", 1);
    c->lsn_replay = geti("listener_replay", 1);
    c->foreign_replay = geti("foreign_replay", 1);
    c->stack_scrub_kb = geti("stack_scrub_kb", 64); if (c->stack_scrub_kb < 0) c->stack_scrub_kb = 0; if (c->stack_scrub_kb > 512) c->stack_scrub_kb = 512;
    c->lsn_scrub_kb = geti("lsn_scrub_kb", 32); if (c->lsn_scrub_kb < 0) c->lsn_scrub_kb = 0; if (c->lsn_scrub_kb > 256) c->lsn_scrub_kb = 256;
    c->stack_scrub = c->stack_scrub_kb > 0;
    { char qp[32]; gets_("query_policy", "null", qp, sizeof qp); c->query_forward = !_strnicmp(qp, "forw", 4); }
    c->stack_outparams = geti("stack_outparams", 1);
    c->gs_pins = geti("game_state_pins", 1);
    c->replay_pause = geti("replay_pause", 1);
    c->xr = 0;   /* round 12: XR hashed raw foreign pointers and crashed once; removed */
    c->static_learn = geti("static_learn_frames", 0);   /* roll7 frames to learn from; 0 = off: it cannot see same-value writes (GATE-B 19.8) */
    c->fp_guard = geti("fp_guard", 1);
    c->ws_statics = geti("ws_statics", 1);
    c->ws_lsn_attrib = geti("ws_lsn_attrib", 1);
    c->anim_probe = geti("anim_probe", 0);
    c->key_anim = keyval("hotkey_anim", 0);
    c->sync1x_passes = geti("sync1x_passes", 4) == 2 ? 2 : 4;
    c->key_passes = keyval("hotkey_passes", VK_F5);
    c->r7_plant_stale = geti("r7_plant_stale", 0);
    c->r7_unified_plant = geti("r7_unified_plant", 0);
    { char rp[32]; gets_("roll7_path", "unified", rp, sizeof rp);
      c->r7_path_ini = !_strnicmp(rp, "ring", 4) ? 0 : !_strnicmp(rp, "sync", 4) ? 1 : !_strnicmp(rp, "fresh", 5) ? 2 : 3; }
    c->clean_zero = geti("clean_zero", 1);
    { char bp[32]; gets_("between_policy", "record", bp, sizeof bp); c->between_record = _strnicmp(bp, "off", 3) != 0; if (!c->between_record) c->foreign_replay = 0; }
    c->key_scrub = keyval("hotkey_scrub", VK_F7);
    c->key_depth = keyval("hotkey_depth", VK_F6);
    c->l1 = geti("l1", 1);
    c->l3_ini = geti("l3", 0);
    c->key_l3 = keyval("hotkey_l3", VK_F4);
    c->l3_zero_kb = geti("l3_zero_kb", 256); if (c->l3_zero_kb < 64) c->l3_zero_kb = 64; if (c->l3_zero_kb > 1024) c->l3_zero_kb = 1024;
    c->l1_cmp_every = geti("l1_cmp_every", 0);
    c->l1_full_budget = geti("l1_full_budget", 12);
    c->l1_cmp_budget = geti("l1_cmp_budget", 80);
    c->greport_budget = geti("greport_budget", 20);
    gets_("l1_extra", "", b, sizeof b);
    c->l1_nextra = parse_ranges(b, c->l1_extra_va, c->l1_extra_len, 4);
    for (int i = 0; i < c->l1_nextra; i++) if (c->l1_extra_va[i] < 0x140000000ULL || c->l1_extra_va[i] + c->l1_extra_len[i] > 0x144ba2000ULL || c->l1_extra_len[i] > 64) { c->l1_nextra = i; break; }   /* image only, <= 64 bytes */
    /* v0.20.0 C0b */
    c->l4 = geti("l4", 0); c->l4 = c->l4 == 2 ? 2 : c->l4 ? 1 : 0;   /* v0.21.0-S1: 2 = guard; any other nonzero value = 1 as before */
    c->pres_keep = geti("pres_keep", 0) != 0;
    c->r7_setup_barrier = geti("r7_setup_barrier", 0) != 0;
    c->khit_census = geti("khit_census", 0) != 0;
    { gets_("pres_ptr_rule", "arena", b, sizeof b); int r = pres_rule_parse(b);   /* v0.21.1 */
      c->pres_ptr_rule = r < 0 ? PRES_RULE_ARENA : r; c->pres_ptr_rule_bad = r < 0; }
    c->l4_miss = geti("l4_miss", 1); if (c->l4_miss < 0) c->l4_miss = 0; if (c->l4_miss > 2) c->l4_miss = 2;
    c->r7_random_depth = geti("r7_random_depth", 0);
    gets_("r7_random_seed", "1", b, sizeof b); c->r7_random_seed = (uint32_t)strtoul(b, NULL, 0);
    c->r7_commit = geti("r7_commit", 0);
    c->r7_forward = geti("r7_forward", 0);
    c->r7_cam_keep = geti("r7_cam_keep", 0);   /* v0.20.2: off by default (gameplay reads the camera: it must be rolled back, not kept live) */
    c->cam_pin_full = geti("cam_pin_full", 1);
    c->l3_fpenv = geti("l3_fpenv", 1);
    gets_("l3_mxcsr", "0x1f80", b, sizeof b); c->l3_mxcsr = (uint32_t)strtoul(b, NULL, 0) & 0xffffu;
    gets_("l3_x87cw", "0x027f", b, sizeof b); c->l3_x87cw = (uint32_t)strtoul(b, NULL, 0) & 0xffffu;
    c->l3_vzeroall = geti("l3_vzeroall", 1);
    gets_("title_tag", "", c->title_tag, sizeof c->title_tag);
    /* v0.22.0-C1a: new keys only; nothing else changes. arena_fixed defaults to 1 only in the C1a modes */
    gets_("replay_file", "", c->replay_file, sizeof c->replay_file);
    c->c1a_dump_every = geti("c1a_dump_every", 600); if (c->c1a_dump_every < 0) c->c1a_dump_every = 0;
    gets_("c1a_refuse", "default", b, sizeof b); c->c1a_refuse = c1a_refuse_parse(b, &c->c1a_refuse_bad);
    c->arena_fixed = geti("arena_fixed", c->mode == MODE_RECORD || c->mode == MODE_REPLAY || is_rbcore_mode(c->mode)) != 0;   /* v0.24.0: + rblive; C2: + rbnet */
    /* v0.22.1-C1a: the replay's CpuPersonality injection (C1A-AUDIT C1-C2); not part of the simcfg hash (record and replay keep one simcfg) */
    gets_("c1a_setup_inject", "", b, sizeof b); c->c1a_inject_set = b[0] != 0 && _stricmp(b, "none") != 0;   /* v0.24.0: rblive refuses a set value */
    if (!b[0]) snprintf(b, sizeof b, "rules,ai");
    c->c1a_inject = c1a_inject_parse(b, &c->c1a_inject_bad);
    /* v0.23.0-C1b (C1B-SPEC.md §3.1): read once; mode=rbreplay validates the ranges (rb.c), the other modes use rb_mt_tail / c1a_t2_heap only */
    c->net_remote_side = geti("net_remote_side", 1);
    c->net_delay = geti("net_delay", 2);
    c->net_max_rollback = geti("net_max_rollback", 7);
    c->emu_latency_ms = geti("emu_latency_ms", 40);
    c->emu_jitter_ms = geti("emu_jitter_ms", 0);
    c->emu_loss_pct = geti("emu_loss_pct", 0);
    c->emu_redundancy = geti("emu_redundancy", 8);
    gets_("emu_seed", "1", b, sizeof b); { char *e; c->emu_seed = strtoll(b, &e, 0); if (*e && *e != ' ' && *e != '\t' && *e != ';') c->emu_seed = -1; }
    gets_("rb_state_slots", "", b, sizeof b); c->rb_state_slots_set = b[0] != 0;
    snprintf(c->rb_state_slots_s, sizeof c->rb_state_slots_s, "%s", b[0] ? b : "0x58");
    c->rb_state_mask = rb_parse_state_slots(c->rb_state_slots_s, c->rb_state_bad, sizeof c->rb_state_bad);
    c->rb_mt_tail = geti("rb_mt_tail", 0);
    c->rb_pinguard = geti("rb_pinguard", 1);
    c->rb_quarantine = geti("rb_quarantine", 1);
    gets_("c1a_t2_heap", "tag", b, sizeof b);
    /* C1B3: (C1B-R3-SPEC.md §2.6.2) raw | tag | map | follow; c1a_t2_heap_tag = the resolved rule (0 raw, 1 tag, 2 map). follow is resolved from
     * the recording's t2rule when it loads (c1a_run.c rf_load); until then it reads 1 (tag), the code default */
    snprintf(c->c1a_t2_heap_s, sizeof c->c1a_t2_heap_s, "%s", b);
    c->c1a_t2_follow = !_strnicmp(b, "follow", 6);
    c->c1a_t2_heap_tag = !_strnicmp(b, "raw", 3) ? 0 : !_strnicmp(b, "map", 3) ? 2 : 1;
    c->c1a_t2_heap_bad = _strnicmp(b, "raw", 3) && _strnicmp(b, "tag", 3) && _strnicmp(b, "map", 3) && !c->c1a_t2_follow;
    c->rb_rollback = geti("rb_rollback", 1);
    c->rb_skip_restore = geti("rb_skip_restore", 0);
    c->rb_tick_limit = geti("rb_tick_limit", 1);
    c->rb_state_gate = geti("rb_state_gate", 1);
    c->rb_state_quiet = geti("rb_state_quiet", 0);   /* v0.23.1: off by default */
    c->rb_test_layout_at = geti("rb_test_layout_at", 0);   /* harness control (case GL): a forced non-prefix pin rebuild at this frame */
    /* v0.24.0-C1b2 PART A (C1B-R2-SPEC.md §4.1): read once; the rb modes validate the ranges (rb.c CFG C1B REFUSE) */
    c->live_cap = geti("live_cap", 216000);
    gets_("c1a_t1_rule", "ptrmap", b, sizeof b);
    c->c1a_t1_rule = !_stricmp(b, "ptrmap") ? 0 : !_stricmp(b, "heaptag") ? 1 : -1;
    c->c1a_mm_dumps = geti("c1a_mm_dumps", 4);
    /* C1B4: (round 4, item 1) the diagnostic dump keys (outside every hash). c1a_dump_at: decimal frames >= 1 separated by ',' or ' ', at most
     * C1A_DUMP_AT_MAX, sorted and unique; a bad list is refused loudly (CFG C1A4 REFUSE) and dumps nothing extra */
    {   gets_("c1a_dump_at", "", c->c1a_dump_at_s, sizeof c->c1a_dump_at_s);
        c->c1a_dump_at_n = 0; c->c1a_dump_at_bad = 0;
        const char *p = c->c1a_dump_at_s;
        while (*p && !c->c1a_dump_at_bad) {
            while (*p == ',' || *p == ' ' || *p == '\t') p++;
            if (!*p || *p == ';') break;
            char *e; unsigned long v = strtoul(p, &e, 10);
            if (e == p || v < 1 || v > 10000000UL || (*e && *e != ',' && *e != ' ' && *e != '\t' && *e != ';')) { c->c1a_dump_at_bad = 1; break; }
            int dup = 0; for (int i = 0; i < c->c1a_dump_at_n; i++) dup |= c->c1a_dump_at[i] == (uint32_t)v;
            if (!dup) { if (c->c1a_dump_at_n >= C1A_DUMP_AT_MAX) { c->c1a_dump_at_bad = 2; break; } c->c1a_dump_at[c->c1a_dump_at_n++] = (uint32_t)v; }
            p = e;
        }
        if (c->c1a_dump_at_bad) c->c1a_dump_at_n = 0;
        for (int i = 1; i < c->c1a_dump_at_n; i++) { uint32_t x = c->c1a_dump_at[i]; int j = i - 1; while (j >= 0 && c->c1a_dump_at[j] > x) { c->c1a_dump_at[j + 1] = c->c1a_dump_at[j]; j--; } c->c1a_dump_at[j + 1] = x; }
    }
    c->c1a_t2mm_dumps = geti("c1a_t2mm_dumps", 0); if (c->c1a_t2mm_dumps < 0) c->c1a_t2mm_dumps = 0; if (c->c1a_t2mm_dumps > 64) c->c1a_t2mm_dumps = 64;
#ifdef SC6RB_HARNESS
    c->h_dump_first_pass = geti("h_dump_first_pass", 0);   /* C1B4 control DARBX */
    c->h_ab_at_confirm = geti("h_ab_at_confirm", 0);       /* v0.32.0 control XGAB */
    c->h_vm_pushres = geti("h_vm_pushres", 0); c->h_vm_sig_bad = geti("h_vm_sig_bad", 0); c->h_vm_every_pass = geti("h_vm_every_pass", 0);   /* v0.32.0 review fix B1 controls */
    c->h_vm_nodiff = geti("h_vm_nodiff", 0); c->h_vm_bc_once = geti("h_vm_bc_once", 0); c->h_vm_cet = geti("h_vm_cet", 0);   /* v0.32.0 finish review fix controls (F4, n6, n7) */
#endif
    c->c1a_clash_bytes = geti("c1a_clash_bytes", 0) == 1;   /* v0.32.0 (D-B): 1 = on; anything else = off */
    {   /* v0.32.0 review fix (B1, OPTA-M2-SPEC.md §21.13; src/vm.c): the move-script VM diagnostics (off by default) */
        c->vm_trace = geti("vm_trace", 0) == 1;
        c->vm_trace_frames = geti("vm_trace_frames", 10); if (c->vm_trace_frames < 1) c->vm_trace_frames = 1; if (c->vm_trace_frames > 120) c->vm_trace_frames = 120;
        /* finish review fixes: up to 48 frames back (the ring keeps the latest pass of the last 64 frames, the rb core's pending frames included) */
        c->vm_trace_pre = geti("vm_trace_pre", 2); if (c->vm_trace_pre < 0) c->vm_trace_pre = 0; if (c->vm_trace_pre > 48) c->vm_trace_pre = 48;
        c->vm_trace_diff_pre = geti("vm_trace_diff_pre", 48); if (c->vm_trace_diff_pre < -1) c->vm_trace_diff_pre = -1; if (c->vm_trace_diff_pre > 48) c->vm_trace_diff_pre = 48;
        c->vm_trace_clashes = geti("vm_trace_clashes", 4); if (c->vm_trace_clashes < 1) c->vm_trace_clashes = 1; if (c->vm_trace_clashes > 16) c->vm_trace_clashes = 16;
        c->vm_trace_cap = geti("vm_trace_cap", 4096); if (c->vm_trace_cap < 256) c->vm_trace_cap = 256; if (c->vm_trace_cap > 65536) c->vm_trace_cap = 65536;
        char fs[32]; gets_("vm_fill", "off", fs, sizeof fs);
        snprintf(c->vm_fill_s, sizeof c->vm_fill_s, "%s", fs);
        c->vm_fill_on = 0; c->vm_fill_rand = 0; c->vm_fill_val = 0; c->vm_fill_bad = 0;
        if (!fs[0] || !_stricmp(fs, "off")) { }
        else if (!_stricmp(fs, "rand")) { c->vm_fill_on = 1; c->vm_fill_rand = 1; }
        else { char *e = NULL; unsigned long v = strtoul(fs, &e, 0); if (e && e != fs && !*e && v <= 0xffff) { c->vm_fill_on = 1; c->vm_fill_val = (uint32_t)v; } else c->vm_fill_bad = 1; }
        c->vm_fix = geti("vm_fix", 0) == 1;
        c->vm_census = geti("vm_census", 0) == 1;   /* v0.34.0 (OPTA-M2-SPEC.md §23.2, D-I): count every EvaluateIf sub-0x25 / 0x91 evaluation's stock vs fixed return */
        c->vm_census_cap = geti("vm_census_cap", 64); if (c->vm_census_cap < 0) c->vm_census_cap = 0; if (c->vm_census_cap > 4096) c->vm_census_cap = 4096;
        c->vm_bytecode = geti("vm_bytecode", 0) == 1;
        char bs[128]; gets_("vm_bytecode_slots", "0xbd,0x30da", bs, sizeof bs);
        c->vm_bc_n = 0; c->vm_bc_bad = 0;
        for (char *p = bs; *p && c->vm_bc_n < 8;) {
            while (*p == ' ' || *p == ',') p++;
            if (!*p) break;
            char *e = NULL; unsigned long v = strtoul(p, &e, 0);
            if (!e || e == p || v > 0xffff) { c->vm_bc_bad = 1; break; }
            c->vm_bc_slots[c->vm_bc_n++] = (uint16_t)v; p = e;
        }
    }
    c->rb_zone = geti("rb_zone", 0);
    c->rb_zone_fight_mode = geti("rb_zone_fight_mode", 2);
    c->rb_zone_entry_gap = geti("rb_zone_entry_gap", 16);
    c->rb_zone_exit_margin = geti("rb_zone_exit_margin", 30);
#ifdef SC6RB_HARNESS
    c->h_pres_old = geti("h_pres_old", 0);       /* F2 control PKCX: the v0.23 face range (0x29310..0x2931f back in P) */
    c->h_deny_off = geti("h_deny_off", 0);       /* S1BFIX controls: 1 = no deny list (G1 and G2 off, TBX*); 2 = G1 misses the caller, G2 on (TBD) */
    /* C1B3: (C1B-R3-SPEC.md §4.1) harness-only controls (one fix off each) and the fake game tree of the modset listing */
    c->h_old_xpred = geti("h_old_xpred", 0); c->h_old_c0count = geti("h_old_c0count", 0); c->h_old_t1align = geti("h_old_t1align", 0);
    c->h_modset = geti("h_modset", 0); gets_("h_modset_root", "", c->h_modset_root, sizeof c->h_modset_root);
    c->h_content_gate_off = geti("h_content_gate_off", 0);
    c->h_c0_refuse_off = geti("h_c0_refuse_off", 0);   /* KOCX: rblive with the v0.24 mask 0x58 (the round-2 KO abort, reproduced) */   /* the v0.24 behavior (no content gate): reproduces the round-2 launch-3 desync */
    pres_test_table(c->h_pres_old);   /* C1B3: 0 KEEP, 1 the v0.23 table (PKCX), 2 the v0.24 table (PFSX, PR60X) */
    c->h_barrier_at = geti("h_barrier_at", 0);   /* GLIVE-AB: a forced repin barrier at the entry of this step */
    pres_test_old_face(c->h_pres_old != 0);
#endif
    /* LIPSYNC: the PART B key (C1B-R2-SPEC.md §3.1, §3.6; REVERT-LIPSYNC.md): 0 = absent = off; a value that is not a number is -1 (refused) */
    { gets_("lipsync", "", b, sizeof b); snprintf(c->lipsync_s, sizeof c->lipsync_s, "%s", b); char *e; long v = strtol(b, &e, 0);   /* LIPSYNC: */
      c->lipsync = !b[0] ? 0 : (e != b && (!*e || *e == ' ' || *e == '\t' || *e == ';')) ? (int)v : -1;   /* LIPSYNC: */
#ifdef SC6RB_HARNESS   /* LIPSYNC: */
      c->h_lips_no_after = geti("h_lips_no_after", 0); gets_("h_lips_skip_site", "", c->h_lips_skip_site, sizeof c->h_lips_skip_site);   /* LIPSYNC: harness-only controls */
      c->h_lips_ev_budget = geti("h_lips_ev_budget", 300);   /* LIPSYNC: v0.25 harness-only (LSEVX: 0 = no LIPSYNCEV line; LSEVB: a small budget) */
#endif   /* LIPSYNC: */
    }   /* LIPSYNC: */
    /* C2 (C2-SPEC.md §6.1): mode=rbnet. Raw values; rb_cfg_check refuses bad ones in rbnet (CFG C2 REFUSE). Presence tracking: rbnet refuses an
     * explicit net_remote_side, replay_file, c1a_setup_inject or emu_* key (the sides come from the role; there is no emulator) */
    {   static const char *const pk[] = { "emu_latency_ms", "emu_jitter_ms", "emu_loss_pct", "emu_redundancy", "emu_seed" };
        gets_("net_remote_side", "\x01", b, sizeof b); c->present_net_remote_side = strcmp(b, "\x01") != 0;
        gets_("replay_file", "\x01", b, sizeof b); c->present_replay_file = strcmp(b, "\x01") != 0;
        c->present_emu = 0; for (size_t i = 0; i < sizeof pk / sizeof pk[0]; i++) { gets_(pk[i], "\x01", b, sizeof b); if (strcmp(b, "\x01")) c->present_emu = 1; }
        gets_("role", "", b, sizeof b); c->net_role = !b[0] ? 0 : !_stricmp(b, "host") ? 1 : !_stricmp(b, "guest") ? 2 : -1;
        gets_("peer", "", c->net_peer, sizeof c->net_peer);
        c->net_port = geti("net_port", 7550);
        gets_("net_bind", "auto", b, sizeof b); c->net_bind = !_stricmp(b, "auto") ? 0 : !_stricmp(b, "loopback") ? 1 : !_stricmp(b, "any") ? 2 : -1;
        gets_("net_peer_kind", "udp", b, sizeof b); c->net_peer_kind = !_stricmp(b, "udp") ? 0 : !_stricmp(b, "script") ? 1 : -1;
        gets_("net_script_rec", "", c->net_script_rec, sizeof c->net_script_rec);
        c->net_script_from = geti("net_script_from", 1000);
        gets_("net_local_src", "pad", b, sizeof b); c->net_local_src = !_stricmp(b, "pad") ? 0 : !_stricmp(b, "rec") ? 1 : -1;
        gets_("net_rec_file", "", c->net_rec_file, sizeof c->net_rec_file);
        gets_("lock_delay", "auto", b, sizeof b); { char *e; long v = strtol(b, &e, 10); c->lock_delay = !_stricmp(b, "auto") ? 0 : (e != b && !*e && v >= 4 && v <= 12) ? (int)v : -1; }
        gets_("net_lever", "clamp", b, sizeof b); c->net_lever = !_stricmp(b, "clamp") ? 0 : !_stricmp(b, "one") ? 1 : -1;
        c->net_timeout_s = geti("net_timeout_s", 10);
        c->pause_cap_s = geti("pause_cap_s", 300);
        c->key_quit = keyval("hotkey_quit", 0);
#ifdef SC6RB_HARNESS
        gets_("h_netem", "", c->h_netem, sizeof c->h_netem); c->h_netem_seed = geti("h_netem_seed", 1);
        c->h_c2_corrupt = geti("h_c2_corrupt", 0); c->h_c2_check_xor_at = geti("h_c2_check_xor_at", 0); c->h_c2_flood_at = geti("h_c2_flood_at", 0); c->h_c2_noconv = geti("h_c2_noconv", 0); c->h_c2_nolever = geti("h_c2_nolever", 0); c->h_c2_nopauseflag = geti("h_c2_nopauseflag", 0); c->h_t2map_ver = geti("h_t2map_ver", 0); c->h_c2_fbar_after = geti("h_c2_fbar_after", 0); c->h_c2_ueskip_keep = geti("h_c2_ueskip_keep", 0); c->h_c2_noseed = geti("h_c2_noseed", 0);
        c->h_c2_setup_gate_off = geti("h_c2_setup_gate_off", 0); c->h_c2_peerleft_off = geti("h_c2_peerleft_off", 0); c->h_c2_rs_burst = geti("h_c2_rs_burst", 0); c->h_c2_noabort_tx = geti("h_c2_noabort_tx", 0);   /* C2 fix r1: NPRESETX / NENDX controls */
        gets_("h_c2_seed", "", b, sizeof b); c->h_c2_seed = (uint32_t)strtoul(b, NULL, 0);
        gets_("h_c2_hs", "", c->h_c2_hs, sizeof c->h_c2_hs); gets_("h_c2_su", "", c->h_c2_su, sizeof c->h_c2_su);
#endif
    }
    /* Option A M1 (OPTA-M0.md §2.5): mode=netobserve keys. Raw parsed values; nobs_install validates / clamps them. */
    c->netobs_trace = geti("netobs_trace", 1);
    c->netobs_trace_mb = geti("netobs_trace_mb", 64); if (c->netobs_trace_mb < 1) c->netobs_trace_mb = 1; if (c->netobs_trace_mb > 1024) c->netobs_trace_mb = 1024;
    c->netobs_pkt_bytes = geti("netobs_pkt_bytes", 48); if (c->netobs_pkt_bytes < 2) c->netobs_pkt_bytes = 2; if (c->netobs_pkt_bytes > 64) c->netobs_pkt_bytes = 64;
    c->netobs_pkt_lines = geti("netobs_pkt_lines", 8); if (c->netobs_pkt_lines < 0) c->netobs_pkt_lines = 0;
    c->netobs_d2_lines = geti("netobs_d2_lines", 32); if (c->netobs_d2_lines < 0) c->netobs_d2_lines = 0;
    c->netobs_chan4 = geti("netobs_chan4", 1);
    c->netobs_offline = geti("netobs_offline", 0);
    c->netobs_cost = geti("netobs_cost", 1);
    c->netobs_t1 = geti("netobs_t1", 1);
#ifdef SC6RB_HARNESS
    c->h_nobs_plant_write = geti("h_nobs_plant_write", 0);
    c->h_nobs_plant_send = geti("h_nobs_plant_send", 0);
    c->h_nobs_plant_sig = geti("h_nobs_plant_sig", 0);
    c->h_nobs_plant_xmm9 = geti("h_nobs_plant_xmm9", 0);
    c->h_nobs_t1_cost = geti("h_nobs_t1_cost", 0);
#endif
    /* Option A M2 (OPTA-M2-SPEC.md §2 D-D, sc6rb/M2-INTERFACES.md §6): mode=rbonline keys. Raw parsed values; rbo_install validates them. */
    c->rbonline_d = geti("rbonline_d", 2);
    c->rbonline_max_rb = geti("rbonline_max_rb", 7);
    c->rbonline_ranked = geti("rbonline_ranked", 0);
    c->rbonline_check_every = geti("rbonline_check_every", 60);
    c->rbonline_arm_round = geti("rbonline_arm_round", 0);
    gets_("rbonline_channel", "0x52424F4E", b, sizeof b); c->rbonline_channel = (uint32_t)strtoul(b, NULL, 0);
    c->rbonline_probe_max = geti("rbonline_probe_max", 10);
    c->rbonline_probe_gap_ms = geti("rbonline_probe_gap_ms", 500);
    c->rbonline_resend_ms = geti("rbonline_resend_ms", 250);
    c->rbonline_peer_timeout_ms = geti("rbonline_peer_timeout_ms", 3000);
    c->rbonline_poll_ms = geti("rbonline_poll_ms", 5);
    c->rbonline_arm_margin_ms = geti("rbonline_arm_margin_ms", 2000);
    c->rbonline_l3_zero_kb = geti("rbonline_l3_zero_kb", 256);
    c->rbonline_dump = geti("rbonline_dump", 1);
    c->rbonline_unranked_ack = geti("rbonline_unranked_ack", 0);   /* v0.29.1 (review M3): no ranked detector exists; 0 = PROBE off */
    c->rbonline_zone_lag_lead = geti("rbonline_zone_lag_lead", 13);   /* v0.30.0 (§19.2): rb_zone_entry_gap 21 - max_rb 7 - 1; 0 = v0.29.9's pin */
    c->rbonline_tsync = geti("rbonline_tsync", 1);                    /* v0.30.0 (§19.4) */
    c->rbonline_badge = geti("rbonline_badge", 1);                    /* v0.31.0 (§20.1): the in-game badge (DECISIONS item 3: on by default) */
    c->rbonline_badge_x = geti("rbonline_badge_x", 50);               /* the text's center, percent of the frame width */
    c->rbonline_badge_y = geti("rbonline_badge_y", 94);               /* the text's bottom, percent of the frame height (between the Soul gauges) */
    c->rbonline_badge_scale = geti("rbonline_badge_scale", 0);        /* 0 = (height + 270) / 540: 720p 1, 1080p 2, 1440p 3, 4K 4 */
    c->rbonline_badge_key = keyval("rbonline_badge_key", VK_F2);      /* show / hide */
    c->rbonline_sound = geti("rbonline_sound", 1);                    /* v0.31.0 (§20.2): the armed / fallback cue */
    /* v0.29.2-optA-M2 (OPTA-M2-SPEC.md §16.9): the rbreplay hybrid keys. Raw values + presence; rb_cfg_check validates them (CFG Q1 REFUSE).
     * Outside cfg_simcfg_hash and cfg_rbcfg_hash: every existing configuration keeps both hashes. */
    {   static const char *const qk[5] = { "rb_l3stock", "rb_arm_f", "rb_init_pins", "q1_roots", "q1_cov" };
        c->q1_present = 0;
        for (int i = 0; i < 5; i++) { gets_(qk[i], "\x01", b, sizeof b); if (strcmp(b, "\x01")) c->q1_present |= 1 << i; }
        c->rb_l3stock = geti("rb_l3stock", 0); c->rb_arm_f = geti("rb_arm_f", 0); c->rb_init_pins = geti("rb_init_pins", 1);
        c->q1_roots = geti("q1_roots", 0); c->q1_cov = geti("q1_cov", 0);
    }
#ifdef SC6RB_HARNESS
    c->h_rbo_fetch_no_c3f0 = geti("h_rbo_fetch_no_c3f0", 0); c->h_rbo_lift_unbounded = geti("h_rbo_lift_unbounded", 0);
    c->h_rbo_ctx_xmm9_zero = geti("h_rbo_ctx_xmm9_zero", 0); c->h_rbo_drain_at = geti("h_rbo_drain_at", 0); c->h_rbo_crash_at = geti("h_rbo_crash_at", 0);
    c->h_rbo_no_wall = geti("h_rbo_no_wall", 0); c->h_rbo_lag_norestore = geti("h_rbo_lag_norestore", 0); c->h_rbo_hello_ctx_xor = geti("h_rbo_hello_ctx_xor", 0);
    c->h_rbo_leak_arena = geti("h_rbo_leak_arena", 0); c->h_rbo_no_restore = geti("h_rbo_no_restore", 0);
    c->h_p2p_accept_any_id = geti("h_p2p_accept_any_id", 0); c->h_p2p_no_guard = geti("h_p2p_no_guard", 0);
    c->h_p2p_no_resend = geti("h_p2p_no_resend", 0); c->h_p2p_corrupt_every = geti("h_p2p_corrupt_every", 0);
#endif
    gets_("watch", "", c->watch, sizeof c->watch);
    if (!_strnicmp(c->watch, "off", 3)) c->watch[0] = 0;

    /* C1B3: (C1B-R3-SPEC.md §2.6.2) c1a_t2_heap=follow in the replay modes: the recording's t2rule (absent = raw, heaptag = tag, map1 = map),
     * read from the replay_file's header now, so every later reader (rbcfg, CFG lines, the header, t2_compute) sees the resolved rule. An
     * unreadable file leaves tag (the replay itself then fails loud: NO_FILE / BAD_FILE). */
    if (c->c1a_t2_follow && (c->mode == MODE_REPLAY || c->mode == MODE_RBREPLAY) && c->replay_file[0]) {
        FILE *rf = fopen(c->replay_file, "rb");
        if (rf) { static char hl[16384]; if (fgets(hl, sizeof hl, rf)) { int r = c1a_t2rule_parse(hl); if (r >= 0) c->c1a_t2_heap_tag = r; } fclose(rf); }
    }
    gets_("hash_masks", "", b, sizeof b);
    c->nmask = parse_ranges(b, c->mask_va, c->mask_len, MAX_EXTRA);
    gets_("extra_regions", "", b, sizeof b);
    c->nregion = parse_ranges(b, c->region_va, c->region_len, MAX_EXTRA);
    gets_("allow_input_vtables", "", b, sizeof b);
    {
        const char *p = b;
        while (*p && c->nallow_vt < MAX_EXTRA) { uintptr_t v = parse_hex(&p); if (!v) break; c->allow_vt[c->nallow_vt++] = v; }
    }
}

void config_log(void) {
    Config *c = &g_cfg;
    char cyc[128] = "";
    for (int i = 0; i < c->ncycle; i++) { strcat(cyc, g_mode_names[c->cycle[i]]); if (i + 1 < c->ncycle) strcat(cyc, ","); }
    LOG("CFG mode=%s cycle=%s keys=%#x/%#x/%#x step_log=%d ws_frames=%d census=%d proxy=%d arena_mb=%d init_routing=%d pin=%d scan=%d roll_depth=%d fake_delay=%d lb_player=%d ai_rand=%s max_reports=%d masks=%d regions=%d allow_vt=%d check_foreign=%d ifc_census=%d\n",
        g_mode_names[c->mode], cyc, c->key_cycle, c->key_mark, c->key_lbplayer, c->step_log, c->ws_frames, c->census, c->proxy,
        c->arena_mb, c->init_routing, c->pin_containers, c->scan_containers, c->roll_depth, c->fake_delay, c->lb_player,
        c->ai_rand_det ? "deterministic" : "crt", c->max_reports, c->nmask, c->nregion, c->nallow_vt, c->check_foreign, c->ifc_census);
    LOG("CFG r9 camera=%s listener_replay=%d foreign_replay=%d stack_scrub_kb=%d lsn_scrub_kb=%d stack_outparams=%d game_state_pins=%d query_policy=%s key_depth=%#x watch=%s\n", c->camera_exclude ? "exclude" : "include",
        c->lsn_replay, c->foreign_replay, c->stack_scrub_kb, c->lsn_scrub_kb, c->stack_outparams, c->gs_pins, c->query_forward ? "forward" : "null", c->key_depth, c->watch[0] ? c->watch : "(off)");
    LOG("CFG v19 l1=%d l3=%d key_l3=%#x l3_zero_kb=%d l1_cmp_every=%d l1_full_budget=%d l1_cmp_budget=%d greport_budget=%d l1_extra=%d roll7_path=%d roll_depth=%d cycle_n=%d\n",
        c->l1, c->l3_ini, c->key_l3, c->l3_zero_kb, c->l1_cmp_every, c->l1_full_budget, c->l1_cmp_budget, c->greport_budget, c->l1_nextra, c->r7_path_ini, c->roll_depth, c->ncycle);
    LOG("CFG v20 l4=%d l4_miss=%d r7_random_depth=%d r7_random_seed=%u r7_commit=%d r7_forward=%d l3_fpenv=%d l3_mxcsr=0x%04x l3_x87cw=0x%04x l3_vzeroall=%d ws_statics=%d listener_replay=%d lsnw=%d fp_guard=%d r7_cam_keep=%d title_tag=%s cam_pin_full=%d\n",
        c->l4, c->l4_miss, c->r7_random_depth, c->r7_random_seed, c->r7_commit, c->r7_forward, c->l3_fpenv, c->l3_mxcsr, c->l3_x87cw, c->l3_vzeroall,
        c->ws_statics, c->lsn_replay, c->lsnw, c->fp_guard, c->r7_cam_keep, c->title_tag[0] ? c->title_tag : "(none)", c->cam_pin_full);
    /* v0.21.0-S1: p_ranges / p_bytes / x_masks count both players (6 P ranges and 2 X masks per player); v0.21.1: + pres_ptr_rule */
    LOG("CFG s1 pres_keep=%d r7_setup_barrier=%d khit_census=%d l4_mode=%s p_ranges=%d p_bytes=%zu x_masks=%d pres_ptr_rule=%s\n",
        c->pres_keep, c->r7_setup_barrier, c->khit_census, c->l4 == 2 ? "guard" : c->l4 ? "on" : "off", 2 * k_pres_np, 2 * pres_p_bytes(), 2 * k_pres_nx,
        pres_rule_name(c->pres_ptr_rule));
    if (c->pres_ptr_rule_bad) LOG("CFG WARN pres_ptr_rule: unknown value in the ini (allowed: arena | image_arena); the default arena is used\n");
    {   /* C1B3: (C1B-R3-SPEC.md §2.4.2) KEEP (pres_keep, the lip-sync allowlist) and POSE (rolled back like G), both players; the frozen hash class */
        PresRange r[PRES_NPOSE]; int np0 = pres_pose_ranges(0, r), np1 = pres_pose_ranges(1, r);
        LOG("CFG PRES keep_ranges=%d keep_bytes=%zu pose_ranges=%d pose_bytes=%zu bank_off0=%x bank_off1=%x hashfx=v024%s\n", 2 * k_pres_np, 2 * pres_p_bytes(), np0 + np1,
            pres_pose_bytes(0) + pres_pose_bytes(1), pres_bank_off(0), pres_bank_off(1), c->h_pres_old ? (c->h_pres_old == 1 ? " CTRL=h_pres_old:1(v0.23_table)" : " CTRL=h_pres_old:2(v0.24_table)") : "");
    }
    {   /* C1B4: (round 4, item 1) the diagnostic dumps: the explicit frames and the tier-2 first-mismatch dumps (mode=replay only) */
        char fl[160]; size_t o = 0; fl[0] = 0;
        for (int i = 0; i < c->c1a_dump_at_n && o + 12 < sizeof fl; i++) { int k = snprintf(fl + o, sizeof fl - o, "%s%u", i ? "," : "", c->c1a_dump_at[i]); if (k > 0) o += (size_t)k; }
        if (c->c1a_dump_at_bad) LOG("CFG C1A4 REFUSE key=c1a_dump_at value=%s need=up_to_%d_frames_1..10000000 (%s: no extra dump this launch)\n", c->c1a_dump_at_s, C1A_DUMP_AT_MAX,
                                    c->c1a_dump_at_bad == 2 ? "too many frames" : "not a frame list");
        LOG("CFG C1A4 dump_at=%d%s%s t2mm_dumps=%d t2mm=%s (diagnostic dumps; outside every setup / simcfg / rbcfg hash)\n", c->c1a_dump_at_n, c->c1a_dump_at_n ? ":" : "", fl,
            c->c1a_t2mm_dumps, !c->c1a_t2mm_dumps ? "off" : c->mode == MODE_REPLAY ? "on" : "ignored(mode)");
        LOG("CFG C1A5 clash_bytes=%d (v0.32.0 D-B: the raw clash / aif bytes of both players as an A line in the .rec / .play on every change; outside every "
            "setup / simcfg / rbcfg hash; rbonline: armed frames only)%s\n", c->c1a_clash_bytes, c->h_ab_at_confirm ? " h_ab_at_confirm=1 (harness control XGAB)" : "");
        {   char sl[96]; size_t so = 0; sl[0] = 0;
            for (int i = 0; i < c->vm_bc_n && so + 8 < sizeof sl; i++) { int k = snprintf(sl + so, sizeof sl - so, "%s0x%x", i ? "," : "", c->vm_bc_slots[i]); if (k > 0) so += (size_t)k; }
            int vmode = c->mode == MODE_RECORD || c->mode == MODE_REPLAY || c->mode == MODE_RBREPLAY;
            int any = c->vm_trace || c->vm_fill_on || c->vm_fix || c->vm_bytecode || c->vm_census;
            if (c->vm_fill_bad) LOG("CFG VM REFUSE key=vm_fill value=%s need=off|rand|0..0xffff (the fill stays off)\n", c->vm_fill_s);
            if (c->vm_bc_bad) LOG("CFG VM REFUSE key=vm_bytecode_slots (a slot list 0..0xffff; the slots up to the bad entry are kept)\n");
            LOG("CFG VM trace=%d frames=%d pre=%d diff_pre=%d clashes=%d cap=%d fill=%s fix=%d census=%d census_cap=%d bytecode=%d slots=%s%s (v0.32.0 review fix B1, src/vm.c: the "
                "move-script VM trace D-F, the argument-tail fill D-H, the zero-tail fix, the bytecode dump D-G; v0.34.0: the D-I census (sub 0x25 / 0x91 stock vs fixed return, "
                "§23.2); off by default; record / replay / rbreplay only, never in rbonline; outside every setup / simcfg / rbcfg hash)%s\n", c->vm_trace, c->vm_trace_frames,
                c->vm_trace_pre, c->vm_trace_diff_pre, c->vm_trace_clashes, c->vm_trace_cap, c->vm_fill_on ? c->vm_fill_s : "off", c->vm_fix, c->vm_census, c->vm_census_cap,
                c->vm_bytecode, sl, c->vm_fix && c->vm_fill_on ? " (vm_fix wins: the tail is zeroed)" : "", any && !vmode ? " IGNORED(mode)" : "");
        }
    }
    {   /* v0.22.0-C1a */
        char rs[200]; c1a_refuse_str(c->c1a_refuse, rs, sizeof rs);
        char is[64]; c1a_refuse_str(c->c1a_inject, is, sizeof is);
        LOG("CFG c1a mode=%s c1a_dump_every=%d c1a_refuse=%s%s arena_fixed=%d c1a_setup_inject=%s%s replay_file=%s content_gate=hard\n", g_mode_names[c->mode], c->c1a_dump_every, rs,
            c->c1a_refuse_bad ? " (unknown name ignored)" : "", c->arena_fixed, is, c->c1a_inject_bad ? " (unknown name ignored)" : "",
            c->replay_file[0] ? c->replay_file : "-");
    }
    if (c->mode == MODE_NETOBSERVE)   /* Option A M1 (OPTA-M0.md §2.5) */
        LOG("CFG netobs trace=%d trace_mb=%d pkt_bytes=%d pkt_lines=%d d2_lines=%d chan4=%d offline=%d cost=%d t1=%d\n",
            c->netobs_trace, c->netobs_trace_mb, c->netobs_pkt_bytes, c->netobs_pkt_lines, c->netobs_d2_lines, c->netobs_chan4, c->netobs_offline, c->netobs_cost, c->netobs_t1);
    if (c->mode == MODE_RBONLINE) {   /* Option A M2 (M2-INTERFACES.md §6): the observer layer's keys (above) apply too */
        LOG("CFG netobs trace=%d trace_mb=%d pkt_bytes=%d pkt_lines=%d d2_lines=%d chan4=%d offline=%d cost=%d t1=%d\n",
            c->netobs_trace, c->netobs_trace_mb, c->netobs_pkt_bytes, c->netobs_pkt_lines, c->netobs_d2_lines, c->netobs_chan4, c->netobs_offline, c->netobs_cost, c->netobs_t1);
        LOG("CFG rbonline d=%d max_rb=%d ranked=%d check_every=%d arm_round=%d channel=0x%08x probe_max=%d probe_gap_ms=%d resend_ms=%d peer_timeout_ms=%d poll_ms=%d arm_margin_ms=%d l3_zero_kb=%d dump=%d unranked_ack=%d\n",
            c->rbonline_d, c->rbonline_max_rb, c->rbonline_ranked, c->rbonline_check_every, c->rbonline_arm_round, c->rbonline_channel, c->rbonline_probe_max,
            c->rbonline_probe_gap_ms, c->rbonline_resend_ms, c->rbonline_peer_timeout_ms, c->rbonline_poll_ms, c->rbonline_arm_margin_ms, c->rbonline_l3_zero_kb, c->rbonline_dump,
            c->rbonline_unranked_ack);
        LOG("CFG rbonline v0.30.0 zone_lag_lead=%d tsync=%d\n", c->rbonline_zone_lag_lead, c->rbonline_tsync);
        LOG("CFG rbonline v0.31.0 badge=%d badge_x=%d badge_y=%d badge_scale=%d badge_key=0x%x sound=%d\n", c->rbonline_badge, c->rbonline_badge_x, c->rbonline_badge_y,
            c->rbonline_badge_scale, c->rbonline_badge_key, c->rbonline_sound);
#ifdef SC6RB_HARNESS
        LOG("CFG rbonline harness h_rbo fetch_no_c3f0=%d lift_unbounded=%d ctx_xmm9_zero=%d drain_at=%d crash_at=%d no_wall=%d lag_norestore=%d hello_ctx_xor=%d leak_arena=%d no_restore=%d h_p2p accept_any_id=%d no_guard=%d no_resend=%d corrupt_every=%d\n",
            c->h_rbo_fetch_no_c3f0, c->h_rbo_lift_unbounded, c->h_rbo_ctx_xmm9_zero, c->h_rbo_drain_at, c->h_rbo_crash_at, c->h_rbo_no_wall, c->h_rbo_lag_norestore,
            c->h_rbo_hello_ctx_xor, c->h_rbo_leak_arena, c->h_rbo_no_restore, c->h_p2p_accept_any_id, c->h_p2p_no_guard, c->h_p2p_no_resend, c->h_p2p_corrupt_every);
#endif
    }
    /* LIPSYNC: outside rbreplay / rblive the key is refused loudly (the rb modes refuse a bad value in rb_cfg_check: CFG C1B REFUSE key=lipsync) */
    if (c->lipsync != 0 && !is_rb_mode(c->mode) && !(c->mode == MODE_RBONLINE && c->lipsync == 1)) {   /* LIPSYNC: */   /* v0.29.4 FACES: rbonline accepts lipsync=1 (armed battles only) */
        if (c->lipsync != 1) LOG("CFG LIPSYNC REFUSE key=lipsync value=%s need=0|1 (this mode runs without lip-sync)\n", c->lipsync_s);   /* LIPSYNC: */
        else LOG("CFG LIPSYNC REFUSE mode=%s (lip-sync runs only in rbreplay / rblive; this mode runs without it, title lips=REFUSED)\n", g_mode_names[c->mode]);   /* LIPSYNC: */
    }   /* LIPSYNC: */
}

/* v0.23.0-C1b: the simulation-configuration hash of the C1a setup category simcfg (moved here from c1a_run.c so the offline check
 * computes the same value; the fields are exactly the v0.22.x ones) and the separate rbcfg hash (C1B-SPEC.md §2.9) */
uint64_t cfg_simcfg_hash(const Config *c) {
    struct { int v, l3, fpenv, vz, l4, qf, aird, campin, ssk, lsk, cz, proxy, initr, track, pinc, gsp, nmask, nreg; uint32_t mx, cw; } s;
    memset(&s, 0, sizeof s);
    s.v = 1; s.l3 = c->l3_ini; s.fpenv = c->l3_fpenv; s.vz = c->l3_vzeroall; s.l4 = c->l4; s.qf = c->query_forward; s.aird = c->ai_rand_det;
    s.campin = c->cam_pin_full; s.ssk = c->stack_scrub_kb; s.lsk = c->lsn_scrub_kb; s.cz = c->clean_zero; s.proxy = c->proxy;
    s.initr = c->init_routing; s.track = c->track_init; s.pinc = c->pin_containers; s.gsp = c->gs_pins;
    s.nmask = c->nmask; s.nreg = c->nregion; s.mx = c->l3_mxcsr; s.cw = c->l3_x87cw;
    uint64_t h = hash_bytes(&s, sizeof s, 0x5e2);
    h = hash_bytes(c->mask_va, sizeof(uintptr_t) * (size_t)c->nmask, h); h = hash_bytes(c->mask_len, sizeof(size_t) * (size_t)c->nmask, h);
    h = hash_bytes(c->region_va, sizeof(uintptr_t) * (size_t)c->nregion, h); h = hash_bytes(c->region_len, sizeof(size_t) * (size_t)c->nregion, h);
    return h;
}
uint64_t cfg_rbcfg_hash(const Config *c) {
    RbCfgVals v; memset(&v, 0, sizeof v);
    v.remote_side = c->net_remote_side; v.net_delay = c->net_delay; v.max_rb = c->net_max_rollback; v.lat_ms = c->emu_latency_ms;
    v.jit_ms = c->emu_jitter_ms; v.loss_pct = c->emu_loss_pct; v.redundancy = c->emu_redundancy; v.seed = (uint64_t)c->emu_seed;
    v.state_slots_mask = c->rb_state_mask; v.mt_tail = c->rb_mt_tail; v.pinguard = c->rb_pinguard; v.quarantine = c->rb_quarantine;
    v.t2_heap_tag = c->c1a_t2_heap_tag; /* C1B3: the resolved rule (raw 0, tag 1, map 2) */ v.rollback = c->rb_rollback; v.skip_restore = c->rb_skip_restore; v.tick_limit = c->rb_tick_limit;
    v.state_gate = c->rb_state_gate; v.test_layout_at = c->rb_test_layout_at;
    v.state_quiet = c->rb_state_quiet;
    /* v0.24.0-C1b2 (C1B-R2-SPEC.md §2.9): the zone keys only when rb_zone != 0, live_cap only in rblive (every v0.23 configuration keeps its value) */
    v.zone = c->rb_zone; v.zone_mode = c->rb_zone_fight_mode; v.zone_gap = c->rb_zone_entry_gap; v.zone_margin = c->rb_zone_exit_margin;
    v.live = c->mode == MODE_RBLIVE; v.live_cap = c->live_cap;
    return rb_cfg_hash(&v);
}
/* the control keys that are not at their code default, comma separated ("none" when all are) */
size_t cfg_controls_str(const Config *c, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    struct { const char *k; int on; } t[] = { { "rb_rollback=0", c->rb_rollback != 1 }, { "rb_skip_restore=1", c->rb_skip_restore != 0 },
                                              { "rb_tick_limit=0", c->rb_tick_limit != 1 }, { "rb_state_gate=0", c->rb_state_gate != 1 },
                                              { "rb_quarantine=0", c->rb_quarantine != 1 }, { "rb_pinguard=0", c->rb_pinguard != 1 },
                                              { "c1a_t2_heap=raw", !c->c1a_t2_heap_tag && !c->c1a_t2_follow }, { "rb_test_layout_at", c->rb_test_layout_at != 0 },
                                              { "c1a_t1_rule=heaptag", c->c1a_t1_rule == 1 }, { "rb_zone=0", c->mode == MODE_RBLIVE && c->rb_zone != 1 },
                                              { "h_c2_noconv", c->h_c2_noconv != 0 },   /* C2: the harness-only control of round C2 */
                                              { "h_c2_setup_gate_off", c->h_c2_setup_gate_off != 0 }, { "h_c2_peerleft_off", c->h_c2_peerleft_off != 0 },   /* C2 fix r1 */
                                              { "h_dump_first_pass", c->h_dump_first_pass != 0 },   /* C1B4: the harness-only control of round 4 */
                                              { "h_ab_at_confirm", c->h_ab_at_confirm != 0 },       /* v0.32.0: the harness-only control XGAB */
                                              /* C1B3: the harness-only controls of round 3 */
                                              { "h_old_xpred", c->h_old_xpred != 0 }, { "h_old_c0count", c->h_old_c0count != 0 }, { "h_old_t1align", c->h_old_t1align != 0 },
                                              { "h_modset", c->h_modset != 0 }, { "h_content_gate_off", c->h_content_gate_off != 0 }, { "h_c0_refuse_off", c->h_c0_refuse_off != 0 },
                                              { "h_pres_old", c->h_pres_old != 0 }, { "h_deny_off", c->h_deny_off != 0 }, { "h_barrier_at", c->h_barrier_at != 0 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].on) { int k = snprintf(out + o, cap > o ? cap - o : 0, "%s%s", o ? "," : "", t[i].k); if (k > 0 && o + (size_t)k < cap) o += (size_t)k; }
    if (c->h_lips_no_after) { int k = snprintf(out + o, cap > o ? cap - o : 0, "%sh_lips_no_after", o ? "," : ""); if (k > 0 && o + (size_t)k < cap) o += (size_t)k; }       /* LIPSYNC: harness-only control */
    if (c->h_lips_skip_site[0]) { int k = snprintf(out + o, cap > o ? cap - o : 0, "%sh_lips_skip_site", o ? "," : ""); if (k > 0 && o + (size_t)k < cap) o += (size_t)k; }   /* LIPSYNC: harness-only control */
    if (!o) o = (size_t)snprintf(out, cap, "none");
    return o;
}

/* Option A M1: the ini path, for the NOBS CFG ignored= key listing (nobs.c) */
const char *config_ini_path(void) { return g_ini; }
