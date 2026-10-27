/* Option A M1: the netobserve writer thread, analysis, runtime init/shutdown and the record-only VEH (OPTA-M0.md §2.4, §5, the
 * M1 review). Nothing here runs on the game thread during a battle except nobs_runtime_init (process start) and nobs_shutdown
 * (detach). This TU never reads game memory itself: CHAN4 / SEL5 go through nobs_body.c. It contains no pointer cast. */
#include "nobs.h"
#include <intrin.h>

/* a HANDLE is usable: not NULL and not INVALID_HANDLE_VALUE (compared as an integer: this TU has no pointer cast, and the
 * INVALID_HANDLE_VALUE macro is one) */
static int h_ok(HANDLE h) { return h && (intptr_t)h != -1; }

#ifdef SC6RB_HARNESS
volatile int nobs_h_title_ms = 1000;
#endif

/* ---------------------------------------------------------------- runtime state owned by the writer */
static HANDLE g_writer;
static volatile LONG g_writer_stop;
static HANDLE g_trace;
static uint64_t g_trace_bytes, g_trace_cap;
static int g_trace_full;
static double g_qpf_us;               /* microseconds per QPC tick */
static uintptr_t g_self_lo, g_self_hi;
static void *g_pstacks[NOBS_H_N];
static char g_gate_name[24];
static int g_regs_seen, g_regs_bad;   /* session: any REGSEQ comparison, any mismatch (sticky) */
static uint32_t g_title_bat; static int g_title_online;
static uint64_t g_t1_hash; static int g_t1_frames, g_t1_off, g_t1_slow;

void *nobs_pstack_top(int hook) { return g_pstacks[hook]; }

/* ---------------------------------------------------------------- small helpers */
static uint64_t fnv_u64(uint64_t h, uint64_t v) { if (!h) h = 0xcbf29ce484222325ULL; for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 0x100000001b3ULL; } return h; }
static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...) { char b[1600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); logf_("%s", b); }
static uint64_t rva_or_abs(uint64_t v) { return v >= g_base && v < g_base + g_image_size ? v - g_base : v; }
static const char *rva_tag(uint64_t v) { return v >= g_base && v < g_base + g_image_size ? "game+" : "abs:"; }

/* ---------------------------------------------------------------- per-battle analysis state */
#define VH_N 8
typedef struct { NobsX128 v[VH_N]; uint64_t c[VH_N]; int n; uint64_t other; } VHist;
static void vh_add(VHist *h, NobsX128 x) {
    for (int i = 0; i < h->n; i++) if (h->v[i].lo == x.lo && h->v[i].hi == x.hi) { h->c[i]++; return; }
    if (h->n < VH_N) { h->v[h->n] = x; h->c[h->n] = 1; h->n++; } else h->other++;
}
typedef struct { uint32_t f, round, value; int set; } RxVal;
typedef struct {
    uint32_t b, seed; int online, recorded, inpobj, battle_line, chan_done; uint32_t last_cls;
    int unexp_seen[32]; int unexp_n;
    /* REGSEQ */ uint64_t rs_cmp, rs_bad; uint32_t rs_first_tick; char rs_first_reg[12];
    VHist rt[10], rsx[10], rmx[2], rfc[2];
    /* SSM / stalls */ int res_hist[8], paused_ticks, stall_on, stall_res; uint32_t st_start, st_end; int st_pf, st_pl, stalls;
    uint32_t last_state, last_res, ssm_n, last_ssm_res;
    /* packets */ int rx_type[16], rx_len[66], rx_hi4[16], rx_plr[16], rx_rnd_mm, rx_plr_mm, tx_code[16], pkt_lines, pkt_round; uint32_t last_gr, last_remote;
    int64_t rel_last; int rel_gaps, rel_dups, rel_reord, t0_dups; uint32_t last_t0; RxVal rx[512]; int ringpkt_match, ringpkt_miss, ringpkt_norx;
    /* D2 */ uint32_t prev[2]; int prev_ok[2]; int d2_frames, d2_lok, d2_rok, d2_m12, d2_c3, d2_tm, d2_un, d2_raw, d2_hi16, d2_first, d2_rep, d2_lines;
    /* T3 / SLACK / tags */ int t3_frames, t3_eq, t3_neq, t3_tail, t3_lead[17]; int sl_n, sl_hist[34]; uint64_t sl_sum; int sl_min, sl_max; int ftag_miss;
    /* phase / mask */ int last_ph, last_m12_set; uint32_t last_m12; int phases, masks;
    /* bOnlineSync (+0x4405) transitions: INPOBJ samples it once, at the first online tick, which is ONE tick before
     * BeginOnlineSync sets it (S-A 2026-10-12: onlinesync=0 was first misread as 'never set'). */
    int last_os; uint32_t os_on_ticks, os_off_ticks;
    /* intro */ int64_t intro_first, intro_end;
    /* cost */ uint64_t c_n[NOBS_H_N], c_sum[NOBS_H_N], c_max[NOBS_H_N]; uint64_t c_bucket[24];
    int ticks, steps;
} Bat;
static Bat B;          /* the current battle (index B.b) */
static int g_have_battle;
static int g_unexp_total, g_battles;

static void bat_reset(uint32_t b, uint32_t seed) {
    memset(&B, 0, sizeof B);
    B.b = b; B.seed = seed; B.last_ph = -1; B.rel_last = -1; B.intro_first = -1; B.intro_end = -1; B.sl_min = 1 << 30; B.sl_max = -1;
    B.last_cls = 99; B.pkt_round = -1; B.last_state = 99; B.last_res = 99; B.last_os = -1;
    g_have_battle = 1;
}
static void unexpected(int kind, const char *name, const char *detail) {
    g_unexp_total++; B.unexp_n++;
    if (kind >= 0 && kind < 32 && B.unexp_seen[kind]) return;
    if (kind >= 0 && kind < 32) B.unexp_seen[kind] = 1;
    emit("NOBS UNEXPECTED kind=%s b=%u tick=%u detail=%s", name, B.b, nobs_tick, detail);
}

/* ---------------------------------------------------------------- per-battle summary lines (at Shutdown, and the totals at detach) */
static void emit_regseq(void) {
    emit("NOBS REGSEQ b=%u ticks=%llu mismatch=%llu first_bad_tick=%u reg=%s", B.b, (unsigned long long)B.rs_cmp, (unsigned long long)B.rs_bad,
         B.rs_bad ? B.rs_first_tick : 0, B.rs_bad ? B.rs_first_reg : "-");
}
static void emit_vh(const char *site, const char *reg, const VHist *h) {
    char v[700]; size_t o = 0; v[0] = 0;
    for (int i = 0; i < h->n && o + 60 < sizeof v; i++)
        o += (size_t)snprintf(v + o, sizeof v - o, "%s%016llx%016llx:%llu", i ? "," : "", (unsigned long long)h->v[i].hi, (unsigned long long)h->v[i].lo, (unsigned long long)h->c[i]);
    emit("NOBS REGS b=%u site=%s reg=%s distinct=%d vals=%s other=%llu", B.b, site, reg, h->n, v[0] ? v : "-", (unsigned long long)h->other);
}
static double cost_pct(double q) {
    uint64_t n = 0; for (int i = 0; i < 24; i++) n += B.c_bucket[i];
    if (!n) return 0;
    uint64_t t = (uint64_t)(q * (double)n), a = 0;
    for (int i = 0; i < 24; i++) { a += B.c_bucket[i]; if (a >= t) return i ? (double)(1u << (i - 1)) : 0.5; }
    return 0;
}
static void hist_str(const int *h, int n, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < n && o + 16 < cap; i++) if (h[i]) o += (size_t)snprintf(out + o, cap - o, "%s%d:%d", o ? "," : "", i, h[i]);
    if (!o) snprintf(out, cap, "-");
}
static void bat_finish(void) {
    if (!g_have_battle) return;
    if (B.stall_on) {   /* a stall still open at the battle end */
        emit("NOBS STALL b=%u r=%u res=%d start_tick=%u end_tick=%u ticks=%u pause_first=%d pause_last=%d lag_in=%d lag_out=%d open=1",
             B.b, B.last_gr, B.stall_res, B.st_start, B.st_end, B.st_end - B.st_start + 1, B.st_pf, B.st_pl,
             B.st_pf < 0 ? -1 : B.st_pf - (int)B.st_start, B.st_pl < 0 ? -1 : B.st_pl - (int)B.st_end);
        B.stall_on = 0;
    }
    char a[256], b2[256], c[256], d[256], e[256], f[256];
    emit_regseq();
    static const char *tn[10] = { "xmm1", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15" };
    static const char *sn[10] = { "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15" };
    for (int i = 0; i < 10; i++) emit_vh("tick", tn[i], &B.rt[i]);
    for (int i = 0; i < 10; i++) emit_vh("step", sn[i], &B.rsx[i]);
    emit_vh("tick", "mxcsr", &B.rmx[0]); emit_vh("step", "mxcsr", &B.rmx[1]);
    emit_vh("tick", "fcw", &B.rfc[0]); emit_vh("step", "fcw", &B.rfc[1]);
    emit("NOBS RESHIST b=%u r0=%d r1=%d r2=%d r3=%d r4=%d r5=%d paused_ticks=%d stalls=%d", B.b, B.res_hist[0], B.res_hist[1], B.res_hist[2],
         B.res_hist[3], B.res_hist[4], B.res_hist[5], B.paused_ticks, B.stalls);
    hist_str(B.rx_type, 16, a, sizeof a); hist_str(B.rx_len, 66, b2, sizeof b2); hist_str(B.rx_hi4, 16, c, sizeof c);
    hist_str(B.rx_plr, 16, d, sizeof d); hist_str(B.tx_code, 16, e, sizeof e);
    emit("NOBS HDRS b=%u rx_types=%s rx_lens=%s rx_hi4=%s rx_plr=%s rx_rnd_mismatch=%d rx_plr_mismatch=%d tx_codes=%s ring_drops=%lld",
         B.b, a, b2, c, d, B.rx_rnd_mm, B.rx_plr_mm, e, (long long)nobs_ring_drops);
    emit("NOBS REL b=%u rx1_gaps=%d rx1_dups=%d rx1_reorders=%d rx0_dups=%d", B.b, B.rel_gaps, B.rel_dups, B.rel_reord, B.t0_dups);
    emit("NOBS RINGPKT b=%u match=%d miss=%d no_rx=%d", B.b, B.ringpkt_match, B.ringpkt_miss, B.ringpkt_norx);
    emit("NOBS D2 b=%u frames=%d local_ok=%d remote_ok=%d m12f8=%d c3f0=%d timer=%d unexplained=%d rawcheck_miss=%d hi16=%d first_skipped=%d repeats=%d",
         B.b, B.d2_frames, B.d2_lok, B.d2_rok, B.d2_m12, B.d2_c3, B.d2_tm, B.d2_un, B.d2_raw, B.d2_hi16, B.d2_first, B.d2_rep);
    hist_str(B.t3_lead, 17, f, sizeof f);
    emit("NOBS T3 b=%u frames=%d eq_localpad=%d neq=%d tail_nonzero=%d lead_hist=%s (16=none)", B.b, B.t3_frames, B.t3_eq, B.t3_neq, B.t3_tail, f);
    hist_str(B.sl_hist, 34, f, sizeof f);
    emit("NOBS SLACK b=%u frames=%d ahead_mean=%.2f ahead_min=%d ahead_max=%d hist=%s fetch_tag_miss=%d", B.b, B.sl_n,
         B.sl_n ? (double)B.sl_sum / B.sl_n : 0.0, B.sl_n ? B.sl_min : 0, B.sl_max, f, B.ftag_miss);
    uint64_t cn = 0, cs = 0, cmx = 0; for (int i = 0; i < NOBS_H_N; i++) { cn += B.c_n[i]; cs += B.c_sum[i]; if (B.c_max[i] > cmx) cmx = B.c_max[i]; }
    emit("NOBS COST b=%u samples=%llu hook_us_p50=%.2f hook_us_p99=%.2f hook_us_max=%.2f hook_us_mean=%.3f tick_us_max=%.2f step_us_max=%.2f trace_bytes=%llu",
         B.b, (unsigned long long)cn, cost_pct(0.50), cost_pct(0.99), (double)cmx * g_qpf_us, cn ? (double)cs * g_qpf_us / (double)cn : 0.0,
         (double)B.c_max[NOBS_H_TICK] * g_qpf_us, (double)B.c_max[NOBS_H_IFC_STEP] * g_qpf_us, (unsigned long long)g_trace_bytes);
    emit("NOBS SUMMARY b=%u online=%d recorded=%d ticks=%d steps=%d unexpected=%d dropped_recs=%lld patchset_ok=%d onlinesync_ticks=%u/%u regs=%s",
         B.b, B.online, B.recorded, B.ticks, B.steps, B.unexp_n, (long long)nobs_ring_drops, !nobs_partial, B.os_on_ticks, B.os_on_ticks + B.os_off_ticks,
         B.rs_cmp == 0 ? "?" : B.rs_bad ? "BAD" : "ok");
    log_flush();
    g_battles++;
    g_have_battle = 0;
}

/* ---------------------------------------------------------------- record handlers */
static void w_tick(const NobsRec *r) {
    if (!g_have_battle) bat_reset(r->bat, 0);
    B.ticks++;
    B.last_gr = r->gr; B.last_remote = r->u.t.remote;
    int cls = (int)r->u.t.obj_class;
    if (cls == 2) { B.online = 1; g_title_online = 1; } else g_title_online = 0;
    if (cls == 2 || (cls == 1 && nobs_offline_rec)) B.recorded = 1;
    g_title_bat = r->bat;
    if ((uint32_t)cls != B.last_cls || !B.inpobj) {
        B.last_cls = (uint32_t)cls; B.inpobj = 1;
        emit("NOBS INPOBJ b=%u tick=%u vt=%s0x%llx class=%s bOnline=%u remote=0x%x local=0x%x onlinesync=%u spectator=%u armable=%u",
             B.b, r->tick, rva_tag(r->u.t.obj_vt), (unsigned long long)rva_or_abs(r->u.t.obj_vt),
             cls == 0 ? "none" : cls == 1 ? "offline_log" : cls == 2 ? "online_sync" : "unknown", r->u.t.bonline, r->u.t.remote, r->u.t.local,
             r->u.t.onlinesync, r->u.t.spectator, cls == 2 && r->u.t.bonline && r->u.t.remote && r->u.t.onlinesync && !r->u.t.spectator);
        if (cls == 3) unexpected(0, "class_unknown", "input_object_vtable_not_sync_or_log");
        if (cls == 2 && r->u.t.spectator) unexpected(1, "spectator", "remote_flags_0");
    }
    if (cls == 2) {   /* every online tick: count bOnlineSync, and log each change with the state and phase it happened in */
        int os_ = (int)r->u.t.onlinesync;
        if (os_) B.os_on_ticks++; else B.os_off_ticks++;
        if (os_ != B.last_os) {
            if (B.last_os >= 0)
                emit("NOBS ONLINESYNC b=%u tick=%u from=%d to=%d state=%u phase=%u gr=%u gt=%u", B.b, r->tick, B.last_os, os_,
                     r->u.t.state, r->u.t.phase, r->gr, r->gt);
            B.last_os = os_;
        }
    }
    if (cls == 2 && !B.chan_done) {   /* M3: CHAN4 / SEL5 at the first online classification of the battle (writer thread) */
        B.chan_done = 1;
        if (g_cfg.netobs_chan4) {
            NobsChan4 c; nobs_read_chan4(&c);
            emit("NOBS CHAN4 b=%u commsys=0x%llx readable=%d n=%d ch4_vt=%s0x%llx ch4_slot0=%s0x%llx", B.b, (unsigned long long)NOBS_A_COMMSYS, c.ok, c.n,
                 rva_tag(c.ch4_vt), (unsigned long long)rva_or_abs(c.ch4_vt), rva_tag(c.ch4_slot0), (unsigned long long)rva_or_abs(c.ch4_slot0));
        }
        NobsSel5 s; nobs_read_sel5(&s);
        emit("NOBS SEL5 b=%u el0_readable=%d el0_id_is_minus1=%d session_alive=%d session_vt=%s0x%llx opponent_readable=%d el0_ne_opponent=%d",
             B.b, s.el0_ok, s.el0_minus1, s.sess_ok, rva_tag(s.sess_vt), (unsigned long long)rva_or_abs(s.sess_vt), s.opp_ok, s.el0_ne_opp);
    }
    if (!B.battle_line && r->u.t.mgrstate == 2) {
        B.battle_line = 1;
        char s150[24]; if (r->u.t.setup150_ok) snprintf(s150, sizeof s150, "%u", r->u.t.setup150); else snprintf(s150, sizeof s150, "n/a");
        emit("NOBS BATTLE b=%u ev=fight seed=0x%08x online=%d mt=none setup150=%s", B.b, B.seed, cls == 2, s150);
    }
    if ((int)r->u.t.phase != B.last_ph) {
        if (B.last_ph >= 0) {
            emit("NOBS PHASE b=%u cf=%u gt=%u from=%d to=%u m12f8=0x%x", B.b, r->frame, r->gt, B.last_ph, r->u.t.phase, r->u.t.m12f8);
            emit("NOBS EV b=%u tick=%u cf=%u gt=%u r=%u src=phase mode=%u ph=%u", B.b, r->tick, r->frame, r->gt, r->gr, r->u.t.phase, r->u.t.phase);
        }
        B.last_ph = (int)r->u.t.phase; B.phases++;
    }
    if (!B.last_m12_set || r->u.t.m12f8 != B.last_m12) {
        if (B.last_m12_set) {
            emit("NOBS MASK b=%u cf=%u gt=%u ph=%u m12f8_from=0x%x m12f8_to=0x%x", B.b, r->frame, r->gt, r->u.t.phase, B.last_m12, r->u.t.m12f8);
            if (r->u.t.phase == 2) unexpected(14, "mask_in_fight", "m12f8_changed_in_phase_2");
            B.masks++;
        }
        B.last_m12 = r->u.t.m12f8; B.last_m12_set = 1;
    }
}

static void w_regs(const NobsRec *r) {
    static const char *nm[10] = { "xmm6/xmm1", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15" };
    const char *bad = 0;
    for (int i = 0; i < 10 && !bad; i++) if (r->u.r.tx[i].lo != r->u.r.sx[i].lo || r->u.r.tx[i].hi != r->u.r.sx[i].hi) bad = nm[i];
    /* MXCSR: only the control bits are nonvolatile across calls (the exception flags 0-5 are volatile, Win64 ABI); FCW in full */
    if (!bad && (r->u.r.t_mxcsr & ~0x3fu) != (r->u.r.s_mxcsr & ~0x3fu)) bad = "mxcsr";
    if (!bad && r->u.r.t_fcw != r->u.r.s_fcw) bad = "fcw";
    B.rs_cmp++; g_regs_seen = 1;
    for (int i = 0; i < 10; i++) { vh_add(&B.rt[i], r->u.r.tx[i]); vh_add(&B.rsx[i], r->u.r.sx[i]); }
    NobsX128 m0 = { r->u.r.t_mxcsr, 0 }, m1 = { r->u.r.s_mxcsr, 0 }, f0 = { r->u.r.t_fcw, 0 }, f1 = { r->u.r.s_fcw, 0 };
    vh_add(&B.rmx[0], m0); vh_add(&B.rmx[1], m1); vh_add(&B.rfc[0], f0); vh_add(&B.rfc[1], f1);
    if (bad) {
        B.rs_bad++; g_regs_bad = 1;
        if (B.rs_bad == 1) { B.rs_first_tick = r->tick; snprintf(B.rs_first_reg, sizeof B.rs_first_reg, "%s", bad); emit_regseq(); log_flush(); }
    }
    if (B.rs_cmp % 600 == 0) { emit_regseq(); log_flush(); }   /* live, while the game runs */
}

static void w_ssm(const NobsRec *r) {
    const __typeof__(r->u.ssm) *m = &r->u.ssm;
    if (m->state > 5) unexpected(2, "state_range", "state_gt_5");
    if (m->result > 5) unexpected(3, "res_range", "result_gt_5");
    B.res_hist[m->result < 8 ? m->result : 7]++;
    if (m->ispaused) B.paused_ticks++;
    B.last_ssm_res = m->result;
    /* D4 stall: result 2 (or 4) runs; IsPaused returns PauseTime_old != 0 || result == 1, so the pause starts one tick after the stall
     * starts and ends one tick after it ends (the tick right after the run still returns paused: that tick is counted here) */
    if (m->result == 2 || m->result == 4) {
        if (!B.stall_on) { B.stall_on = 1; B.stall_res = (int)m->result; B.st_start = r->tick; B.st_pf = -1; B.st_pl = -1; }
        B.st_end = r->tick;
        if (m->ispaused) { if (B.st_pf < 0) B.st_pf = (int)r->tick; B.st_pl = (int)r->tick; }
    } else if (B.stall_on) {
        if (m->ispaused) { if (B.st_pf < 0) B.st_pf = (int)r->tick; B.st_pl = (int)r->tick; }
        emit("NOBS STALL b=%u r=%u res=%d start_tick=%u end_tick=%u ticks=%u pause_first=%d pause_last=%d lag_in=%d lag_out=%d",
             B.b, r->gr, B.stall_res, B.st_start, B.st_end, B.st_end - B.st_start + 1, B.st_pf, B.st_pl,
             B.st_pf < 0 ? -1 : B.st_pf - (int)B.st_start, B.st_pl < 0 ? -1 : B.st_pl - (int)B.st_end);
        B.stall_on = 0; B.stalls++;
    }
    if (m->result == 4) unexpected(4, "timeout_res4", "sync_timeout_pause");
    if (m->state != B.last_state || m->result != B.last_res || (++B.ssm_n % 600) == 0) {
        B.last_state = m->state; B.last_res = m->result;
        float rtt; memcpy(&rtt, &m->rtt_bits, sizeof rtt);
        emit("NOBS SSM b=%u tick=%u r=%u gt=%u st=%u syt=%u res=%u state=%u lag=%u rtt=%.1f to=%u sto=%u pt=%u",
             B.b, r->tick, r->gr, r->gt, m->sendtime, m->synctime, m->result, m->state, m->lag, (double)rtt,
             m->timeout_cnt, m->synctimeout, m->pausetime);
    }
}

static void w_pkt(const NobsRec *r) {
    const __typeof__(r->u.p) *p = &r->u.p;
    unsigned type = p->nbytes >= 2 ? (p->bytes[1] & 0xfu) : 0xffu, rnd = p->nbytes >= 1 ? (p->bytes[0] & 0xfu) : 0xffu;
    unsigned plr = p->nbytes >= 1 ? (unsigned)(p->bytes[0] >> 4) : 0xffu, hi4 = p->nbytes >= 2 ? (unsigned)(p->bytes[1] >> 4) : 0xffu;
    int anomaly = 0;
    if (p->nbytes < 2 || p->len < 2) { unexpected(5, "pkt_short", "len_lt_2"); anomaly = 1; }
    if (p->mode != 1 || p->len > p->cap || p->cur_off != 0) { unexpected(6, "pkt_layout", p->cur_off != 0 ? "cursor_not_at_data" : "mode_or_len"); anomaly = 1; }
    if (type < 16) B.rx_type[type]++;
    B.rx_len[p->len < 65 ? p->len : 65]++;
    if (hi4 < 16) B.rx_hi4[hi4]++;
    if (plr < 16) B.rx_plr[plr]++;
    if (type != 0xff && type >= 2) { unexpected(7, "pkt_type", "type_ge_2"); anomaly = 1; }
    if (type == 1 && rnd != (B.last_gr & 0xf)) { B.rx_rnd_mm++; unexpected(8, "pkt_rnd", "type1_round_nibble_ne_gameround"); }
    if (plr < 16 && B.last_remote && !((B.last_remote >> plr) & 1)) { B.rx_plr_mm++; unexpected(9, "pkt_plr", "player_not_in_remote_flags"); }
    if (type == 0 && p->nbytes >= 3) {
        if (p->bytes[2] == 1) unexpected(10, "code1", "rx_type0_code1");
        uint32_t w = p->bytes[0] | (uint32_t)p->bytes[1] << 8 | (uint32_t)p->bytes[2] << 16;
        if (w == B.last_t0) B.t0_dups++;
        B.last_t0 = w;
    }
    uint32_t start = 0, ack = 0; int n = 0;
    if (type == 1 && p->nbytes >= 10) {
        start = p->bytes[2] | (uint32_t)p->bytes[3] << 8 | (uint32_t)p->bytes[4] << 16 | (uint32_t)p->bytes[5] << 24;
        ack = p->bytes[6] | (uint32_t)p->bytes[7] << 8 | (uint32_t)p->bytes[8] << 16 | (uint32_t)p->bytes[9] << 24;
        n = ((int)p->len - 10) >> 1;
        if (B.rel_last >= 0) {
            if ((int64_t)start == B.rel_last) B.rel_dups++;
            else if ((int64_t)start < B.rel_last) B.rel_reord++;
            else if ((int64_t)start > B.rel_last + 1) B.rel_gaps++;
        }
        B.rel_last = start;
        for (int i = 0; i < n && 10 + 2 * i + 1 < (int)p->nbytes; i++) {   /* RINGPKT: remember the remote inputs per frame */
            RxVal *e = &B.rx[(start + (uint32_t)i) & 0x1ff];
            e->f = start + (uint32_t)i; e->round = rnd; e->value = p->bytes[10 + 2 * i] | (uint32_t)p->bytes[11 + 2 * i] << 8; e->set = 1;
        }
    }
    if ((int)B.last_gr != B.pkt_round) { B.pkt_round = (int)B.last_gr; B.pkt_lines = 0; }
    if (type == 0 || anomaly || (type == 1 && B.pkt_lines++ < g_cfg.netobs_pkt_lines)) {
        char ex[80] = "";
        if (type == 0 && p->nbytes >= 3) snprintf(ex, sizeof ex, " code=%u", p->bytes[2]);
        if (type == 1 && p->nbytes >= 10) snprintf(ex, sizeof ex, " start=%u ack=%u n=%d", start, ack, n);
        emit("NOBS PKT dir=rx b=%u tick=%u r=%u type=%u rnd=%u plr=%u hi4=%u len=%u mode=%u cur_off=%lld%s",
             B.b, r->tick, B.last_gr, type, rnd, plr, hi4, p->len, p->mode, (long long)p->cur_off, ex);
    }
}

static void w_sendctl(const NobsRec *r) {
    if (r->u.sc.code < 16) B.tx_code[r->u.sc.code]++;
    if (r->u.sc.code == 1) unexpected(11, "code1", "tx_type0_code1");
    emit("NOBS PKT dir=tx b=%u tick=%u gt=%u r=%u type=0 code=%u state=%u", B.b, r->tick, r->gt, r->u.sc.round, r->u.sc.code, r->u.sc.state);
}

static void w_life(const NobsRec *r) {
    static const char *ev[] = { "init", "shutdown", "reset", "setstate" };
    if (r->u.life.ev == LIFE_INIT) { bat_finish(); bat_reset(r->bat, r->u.life.seed); }
    else if (!g_have_battle) bat_reset(r->bat, 0);
    emit("NOBS LIFE b=%u tick=%u ev=%s r=%u from=%u to=%u gt=%u st=%u syt=%u ut=%u%s", B.b, r->tick, r->u.life.ev < 4 ? ev[r->u.life.ev] : "?",
         r->u.life.ev == LIFE_RESET ? r->u.life.round : r->gr, r->u.life.from, r->u.life.ev == LIFE_RESET ? r->u.life.round : r->u.life.to, r->gt,
         r->u.life.sendtime, r->u.life.synctime, r->u.life.updatetime, r->u.life.ev == LIFE_INIT ? "" : "");
    if (r->u.life.ev == LIFE_SETSTATE && r->gr == 9 && r->u.life.from == 1 && r->u.life.to == 2 && B.intro_first < 0) B.intro_first = r->tick;
    if (r->u.life.ev == LIFE_RESET && r->u.life.round == 1 && B.intro_first >= 0 && B.intro_end < 0) {
        B.intro_end = r->tick;
        emit("NOBS INTRO b=%u first12_tick=%lld reset1_tick=%lld ticks=%lld", B.b, (long long)B.intro_first, (long long)B.intro_end, (long long)(B.intro_end - B.intro_first));
    }
    /* the per-round reset (FUN_1403f7c60) invalidates the InputSync rings; the manager's 0x1498 prev raw is NOT reset (it lives in
     * the manager), so the D2 prev chain carries over, like FetchInput's */
    /* The per-round reset zeroes GameTime and both rings, so the next round's type-1 stream restarts at frame 0: the REL sequence
     * tracker has to restart with it, or every round boundary would be counted as a reorder (found by the harness truth table). */
    if (r->u.life.ev == LIFE_RESET) { memset(B.rx, 0, sizeof B.rx); B.rel_last = -1; }
    if (r->u.life.ev == LIFE_SHUTDOWN) bat_finish();
}

static void w_step(const NobsRec *r) {
    const __typeof__(r->u.s) *s = &r->u.s;
    if (!g_have_battle) bat_reset(r->bat, 0);
    B.steps++;
    if ((int)s->phase != B.last_ph && B.last_ph >= 0) {
        emit("NOBS PHASE b=%u cf=%u gt=%u from=%d to=%u m12f8=0x%x", B.b, r->frame, r->gt, B.last_ph, s->phase, s->m12f8);
        emit("NOBS EV b=%u tick=%u cf=%u gt=%u r=%u src=phase mode=%u ph=%u", B.b, r->tick, r->frame, r->gt, r->gr, s->phase, s->phase);
        B.last_ph = (int)s->phase;
    }
    if (s->repeat) { B.d2_rep++; return; }
    uint32_t local = s->local, remote = 1 - local, f = s->fetch_f;
    uint64_t M = ~((uint64_t)s->m12f8 | (uint64_t)s->m12f8 << 32), C3 = 0xffffc3f0ffffc3f0ULL;
    for (uint32_t p = 0; p < 2; p++) {
        int tagok = s->ring[p][0] == r->gr && s->ring[p][1] == f;
        uint32_t raw = tagok ? s->ring[p][2] : 0;   /* FetchInput: the value iff both tags match (0 otherwise) */
        if (p == remote && !tagok && B.last_ssm_res != 2) { B.ftag_miss++; unexpected(12, "fetch_tag_miss", "remote_ring_tag_mismatch_unstalled"); }
        if (raw != s->prev1498[p]) B.d2_raw++;
        if (p == local && raw >= 0x10000) { B.d2_hi16++; unexpected(13, "hi16", "local_raw_ge_0x10000"); }
        if (!B.prev_ok[p]) { B.prev[p] = raw; B.prev_ok[p] = 1; B.d2_first++; continue; }
        /* FetchInput (dec_tick1.c:578-628): held = raw (0 if mgr+0x146c > 0), pressed = (prev ^ raw) & raw (0 if mgr+0x1468 > 0),
         * both & ~m12f8, both & 0xffffc3f0 when FUN_1403f2220 says so (c3f0). Classes in order: ok, m12f8, c3f0, timer. */
        uint32_t press = (B.prev[p] ^ raw) & raw;
        uint64_t der = (uint64_t)raw | (uint64_t)press << 32, w = s->w[p];
        uint64_t dert = (uint64_t)(s->t146c > 0 ? 0 : raw) | (uint64_t)(s->t1468 > 0 ? 0 : press) << 32;
        B.d2_frames++;
        if (der == w) { if (p == remote) B.d2_rok++; else B.d2_lok++; }
        else if (s->m12f8 && (der & M) == w) B.d2_m12++;
        else if (((der & M) & C3) == w) B.d2_c3++;
        else if ((s->t1468 > 0 || s->t146c > 0) && ((dert & M) == w || ((dert & M) & C3) == w)) B.d2_tm++;
        else {
            B.d2_un++;
            if (B.d2_lines++ < g_cfg.netobs_d2_lines)
                emit("NOBS D2MISS b=%u r=%u f=%u p=%u side=%s raw=0x%x prev=0x%x derived=0x%016llx word=0x%016llx m12f8=0x%x t1468=%d t146c=%d ph=%u why=unexplained",
                     B.b, r->gr, f, p + 1, p == remote ? "remote" : "local", raw, B.prev[p], (unsigned long long)der, (unsigned long long)w, s->m12f8,
                     s->t1468, s->t146c, s->phase);
            unexpected(15, "d2_unexplained", "derived_word_ne_stepin");
        }
        B.prev[p] = raw;
    }
    /* RINGPKT: the remote ring value at the fetch frame vs the last rx type-1 payload for that frame */
    if (s->ring[remote][0] == r->gr && s->ring[remote][1] == f) {
        const RxVal *e = &B.rx[f & 0x1ff];
        if (!e->set || e->f != f || e->round != (r->gr & 0xf)) B.ringpkt_norx++;
        else if (e->value == s->ring[remote][2]) B.ringpkt_match++;
        else B.ringpkt_miss++;
    }
    /* T3 */
    uint32_t pad = (uint32_t)s->third[0] | (uint32_t)s->third[1] << 8 | (uint32_t)s->third[2] << 16 | (uint32_t)s->third[3] << 24;
    B.t3_frames++;
    if (s->pad_ok && pad == s->pad_raw) B.t3_eq++; else B.t3_neq++;
    int lead = 16; for (int j = 0; j < 16; j++) if (s->loc_ahead[j] == pad && (pad || j == 0)) { lead = j; break; }
    B.t3_lead[lead]++;
    for (int i = 8; i < 0x18; i++) if (s->third[i]) { B.t3_tail++; break; }
    /* SLACK */
    int a = (int)s->ahead; B.sl_n++; B.sl_sum += (uint64_t)a; if (a < B.sl_min) B.sl_min = a; if (a > B.sl_max) B.sl_max = a; B.sl_hist[a < 33 ? a : 33]++;
    /* M1b proxy digest */
    if (s->t1_on && !g_t1_off) {
        g_t1_hash = fnv_u64(g_t1_hash, s->w[0]); g_t1_hash = fnv_u64(g_t1_hash, s->w[1]);
        g_t1_hash = fnv_u64(g_t1_hash, (uint64_t)s->ring[0][2] | (uint64_t)s->ring[1][2] << 32);
        g_t1_hash = fnv_u64(g_t1_hash, (uint64_t)r->gr << 32 | r->gt); g_t1_frames++;
    }
}

static void w_cost(const NobsRec *r) {
    uint32_t h = r->u.cost.hook < NOBS_H_N ? r->u.cost.hook : 0;
    uint64_t t = r->u.cost.ticks;
    B.c_n[h]++; B.c_sum[h] += t; if (t > B.c_max[h]) B.c_max[h] = t;
    double us = (double)t * g_qpf_us; int bk = 0; double x = us; while (bk < 23 && x >= 1.0) { x /= 2.0; bk++; }
    B.c_bucket[bk]++;
    /* Three strikes, not one: the first live session lost the digest to a single 12.9 ms sample in ifc_init during loading (a
     * one-off page-fault/first-VirtualQuery cost, not a per-frame cost). A real cost problem repeats. */
    if (us > 500.0 && !g_t1_off && ++g_t1_slow >= 3) {
        g_t1_off = 1;
        emit("NOBS T1 off=cost b=%u hook=%u sample_us=%.1f slow_samples=%d (M1b per-frame digest auto-disabled)", B.b, h, us, g_t1_slow);
    }
}
static void w_unexp(const NobsRec *r) {
    if (r->u.u.kind == UNEXP_THREAD) {
        char d[64]; snprintf(d, sizeof d, "hook=%u tid=%u", r->u.u.hook, r->u.u.tid);
        unexpected(16 + (int)(r->u.u.hook & 7), "thread", d);
    } else if (r->u.u.kind == UNEXP_COST_PLANT && !g_t1_off) {
        g_t1_off = 1; emit("NOBS T1 off=cost b=%u hook=%u sample_us=999.0 (M1b per-frame digest auto-disabled; harness plant)", B.b, r->u.u.hook);
    }
}

static void trace_write(const NobsRec *r) {
    if (!h_ok(g_trace) || g_trace_full) return;
    if (g_trace_bytes + sizeof *r > g_trace_cap) { g_trace_full = 1; emit("NOBS TRACE full=1 bytes=%llu", (unsigned long long)g_trace_bytes); return; }
    DWORD w = 0; WriteFile(g_trace, r, (DWORD)sizeof *r, &w, NULL); g_trace_bytes += w;
}
static int ring_pop(NobsRec *out) {
    NobsCell *c = &nobs_ring[nobs_ring_deq & (NOBS_RING_CAP - 1)];
    LONG64 seq = c->seq;
    if (seq - (nobs_ring_deq + 1) != 0) return 0;
    _ReadWriteBarrier();
    *out = c->u.rec;
    LONG64 pos = nobs_ring_deq;
    nobs_ring_deq = pos + 1;
    _InterlockedExchange64(&c->seq, pos + NOBS_RING_CAP);
    return 1;
}
static void handle(const NobsRec *r) {
    if (g_cfg.netobs_trace && (r->type == REC_STEP || r->type == REC_TICK || r->type == REC_PKT || r->type == REC_LIFE)) trace_write(r);
    switch (r->type) {
    case REC_TICK: w_tick(r); break;
    case REC_STEP: w_step(r); break;
    case REC_REGS: if (!g_have_battle) bat_reset(r->bat, 0); w_regs(r); break;
    case REC_PKT: if (!g_have_battle) bat_reset(r->bat, 0); w_pkt(r); break;
    case REC_SSM: if (!g_have_battle) bat_reset(r->bat, 0); w_ssm(r); break;
    case REC_SENDCTL: if (!g_have_battle) bat_reset(r->bat, 0); w_sendctl(r); break;
    case REC_LIFE: w_life(r); break;
    case REC_COST: w_cost(r); break;
    case REC_UNEXP: if (!g_have_battle) bat_reset(r->bat, 0); w_unexp(r); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- the window-title token (writer thread only; M1) */
static HWND g_title_hwnd, g_best; static LONG g_best_area; static char g_title_orig[160], g_title_last[240];
static BOOL CALLBACK title_enum(HWND h, LPARAM lp) {
    (void)lp;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT rc; if (!GetWindowRect(h, &rc)) return TRUE;
    LONG a = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (!g_best || a > g_best_area) { g_best = h; g_best_area = a; }
    return TRUE;
}
NobsTitleExtFn nobs_title_ext_fn;   /* Option A M2: rbonline's token (NULL in netobserve) */
NobsTickExtFn nobs_tick_ext_fn;     /* v0.31.0: rbonline's badge / cue tick (NULL in netobserve) */
int nobs_regs_bad(void) { return g_regs_seen && g_regs_bad; }
void nobs_title_token(char *out, size_t cap) {
    const char *rg = !g_regs_seen ? "?" : g_regs_bad ? "BAD" : "ok";
    if (nobs_title_ext_fn) { nobs_title_ext_fn(out, cap, rg); return; }
    if (nobs_off) snprintf(out, cap, "netobs=OFF:%s", g_gate_name[0] ? g_gate_name : "gate");
    else if (nobs_partial) snprintf(out, cap, "netobs=PARTIAL:%s b=%u regs=%s", g_gate_name[0] ? g_gate_name : "hook", g_title_bat, rg);
    else snprintf(out, cap, "netobs=ok online=%d b=%u regs=%s", g_title_online, g_title_bat, rg);
}
#ifdef SC6RB_HARNESS
volatile int nobs_h_title_any;
HANDLE nobs_h_writer(void) { return g_writer; }
static BOOL CALLBACK title_enum_any(HWND h, LPARAM lp) {   /* the v0.28.0 behavior: the first visible window of this process */
    (void)lp; DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    g_best = h; return FALSE;
}
#endif
static void title_pump(void) {
#ifdef SC6RB_HARNESS
    if (nobs_h_title_any) {   /* NOBS-TITLEX plant: cache the first window forever, no game-thread restriction, no re-validation */
        if (!g_title_hwnd) { g_best = NULL; EnumWindows(title_enum_any, 0); if (!g_best) return; g_title_hwnd = g_best; g_title_orig[0] = 0; }
        char tok[80], full[240]; nobs_title_token(tok, sizeof tok);
        snprintf(full, sizeof full, "SOULCALIBUR VI  [%s]", tok);
        DWORD_PTR res = 0; SendMessageTimeoutA(g_title_hwnd, WM_SETTEXT, 0, (LPARAM)full, SMTO_ABORTIFHUNG, 200, &res);
        return;
    }
#endif
    DWORD gt = (DWORD)nobs_game_tid;
    if (!gt) return;   /* not before the game thread is known: the UE splash runs on another thread */
    if (g_title_hwnd && (!IsWindow(g_title_hwnd) || !IsWindowVisible(g_title_hwnd))) { g_title_hwnd = NULL; g_title_last[0] = 0; }
    g_best = NULL; g_best_area = 0;
    EnumThreadWindows(gt, title_enum, 0);
    if (!g_best) return;
    if (g_best != g_title_hwnd) {
        g_title_hwnd = g_best; g_title_last[0] = 0;
        DWORD_PTR res = 0; g_title_orig[0] = 0;
        SendMessageTimeoutA(g_title_hwnd, WM_GETTEXT, sizeof g_title_orig, (LPARAM)g_title_orig, SMTO_ABORTIFHUNG, 200, &res);
        g_title_orig[sizeof g_title_orig - 1] = 0;
        char *cut = strstr(g_title_orig, "  [netobs"); if (cut) *cut = 0;
        cut = strstr(g_title_orig, "  [rb="); if (cut) *cut = 0;   /* Option A M2: rbonline's token */
    }
    char tok[80], full[240]; nobs_title_token(tok, sizeof tok);
    snprintf(full, sizeof full, "%s  [%s]", g_title_orig[0] ? g_title_orig : "SOULCALIBUR VI", tok);
    if (strcmp(full, g_title_last)) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutA(g_title_hwnd, WM_SETTEXT, 0, (LPARAM)full, SMTO_ABORTIFHUNG, 200, &res)) snprintf(g_title_last, sizeof g_title_last, "%s", full);
    }
}

static DWORD WINAPI writer_main(LPVOID p) {
    (void)p;
    NobsRec r;
    uint64_t last_title = 0, last_tick = 0;
    for (;;) {
        int stop = g_writer_stop != 0, did = 0;
        while (ring_pop(&r)) { did = 1; handle(&r); }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
#ifdef SC6RB_HARNESS
        double period = (double)nobs_h_title_ms * 1000.0;
#else
        double period = 1000000.0;
#endif
        if ((double)((uint64_t)now.QuadPart - last_title) * g_qpf_us > period) { last_title = (uint64_t)now.QuadPart; title_pump(); }
        if (nobs_tick_ext_fn && (double)((uint64_t)now.QuadPart - last_tick) * g_qpf_us > 50000.0) { last_tick = (uint64_t)now.QuadPart; nobs_tick_ext_fn(); }   /* v0.31.0 */
        if (stop) break;
        if (!did) { log_flush(); Sleep(2); }
    }
    return 0;
}

/* ---------------------------------------------------------------- the record-only VEH (m3): only our own faults, no lock, no CRT */
static volatile LONG g_veh_n;
static size_t put_s(char *b, size_t o, const char *s) { while (*s && o < 250) b[o++] = *s++; return o; }
static size_t put_x(char *b, size_t o, uint64_t v) { o = put_s(b, o, "0x"); int st = 0; for (int i = 60; i >= 0; i -= 4) { int d = (int)((v >> i) & 15); if (d || st || i == 0) { b[o++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); st = 1; } } return o; }
static LONG CALLBACK nobs_veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR && code != 0xC0000409 && code != EXCEPTION_DATATYPE_MISALIGNMENT) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip, rsp = (uintptr_t)ep->ContextRecord->Rsp;
    int ours = rip >= g_self_lo && rip < g_self_hi, stk = 0;
    for (int i = 0; i < NOBS_H_N; i++) if (rsp >= nobs_pstk_lo[i] && rsp < nobs_pstk_hi[i]) { ours = 1; stk = 1; }
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;   /* not ours: touch nothing */
    if (_InterlockedIncrement(&g_veh_n) > 4) return EXCEPTION_CONTINUE_SEARCH;
    static char b[256]; size_t o = 0;
    o = put_s(b, o, "NOBS CRASH code="); o = put_x(b, o, code);
    o = put_s(b, o, rip >= g_self_lo && rip < g_self_hi ? " rip=sc6rb+" : " rip=abs:"); o = put_x(b, o, rip >= g_self_lo && rip < g_self_hi ? rip - g_self_lo : rip);
    o = put_s(b, o, stk ? " stack=private" : " stack=game");
    if (ep->ExceptionRecord->NumberParameters >= 2) { o = put_s(b, o, " av_kind="); o = put_x(b, o, ep->ExceptionRecord->ExceptionInformation[0]);
                                                      o = put_s(b, o, " av_addr="); o = put_x(b, o, ep->ExceptionRecord->ExceptionInformation[1]); }
    o = put_s(b, o, " tid="); o = put_x(b, o, __readgsdword(0x48));
    o = put_s(b, o, " (written directly; may precede unflushed buffered lines)\r\n");
    HANDLE f = log_handle(); DWORD w;
    if (h_ok(f)) WriteFile(f, b, (DWORD)o, &w, NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------------------------------------------------------------- install-time log lines (game thread, process start) */
void nobs_note_patch(const char *name, uint64_t va, const char *kind, int sig_ok, uint64_t want, uint64_t got, int status) {
    LOG("NOBS PATCH name=%s va=game+0x%llx kind=%s sig=%s want=0x%llx got=0x%llx status=%d\n", name, (unsigned long long)(va - IMG_PREF), kind,
        sig_ok ? "ok" : "BAD", (unsigned long long)want, (unsigned long long)got, status);
}
void nobs_patchset(int n, const char *names, int ok) {
    LOG("NOBS PATCHSET n=%d names=%s expected=tick,ifc_init,ifc_shutdown,ifc_step,sendctl,ssm,ispaused,setstate,rreset,enq ok=%d\n", n, names, ok);
}
void nobs_ready(void) {
    LOG("NOBS READY mode=netobserve exe_ts=0x61717836 writes=none_by_construction sends=none_linked xmask=0x%x%08x\n", nobs_xmask[1], nobs_xmask[0]);
}
void nobs_cfg_ignored(void) {
    static const char *known[] = { "mode", "log_dir", "netobs_trace", "netobs_trace_mb", "netobs_pkt_bytes", "netobs_pkt_lines", "netobs_d2_lines",
                                   "netobs_chan4", "netobs_offline", "netobs_cost", "netobs_t1", "h_nobs_plant_write", "h_nobs_plant_send",
                                   "h_nobs_plant_sig", "h_nobs_plant_xmm9", "h_nobs_t1_cost" };
    char keys[4096], csv[1024]; size_t o = 0; csv[0] = 0;
    DWORD n = GetPrivateProfileStringA("sc6rb", NULL, "", keys, sizeof keys, config_ini_path());
    for (const char *k = keys; n && *k && k < keys + n; k += strlen(k) + 1) {
        int ok = 0; for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) if (!_stricmp(k, known[i])) ok = 1;
        if (!ok && o + strlen(k) + 2 < sizeof csv) o += (size_t)snprintf(csv + o, sizeof csv - o, "%s%s", o ? "," : "", k);
    }
    LOG("NOBS CFG ignored=%s\n", o ? csv : "-");
}
void nobs_gate_fail(const char *what, int core) {
    LOG("NOBS GATE FAIL %s=%s -> mode=%s\n", core ? "core" : "hook", what, core ? "off" : "netobserve_partial");
    if (!g_gate_name[0]) snprintf(g_gate_name, sizeof g_gate_name, "%s", what);
    if (core) nobs_off = 1; else nobs_partial = 1;
}

/* ---------------------------------------------------------------- runtime init / shutdown */
static void cpuidx(uint32_t leaf, uint32_t sub, uint32_t r[4]) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub)); }
static uint64_t xgetbv0(void) { uint32_t lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); return (uint64_t)hi << 32 | lo; }

int nobs_runtime_init(void) {
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); g_qpf_us = 1e6 / (double)fq.QuadPart;
    nobs_offline_rec = g_cfg.netobs_offline != 0;
    /* XSAVE (M4): the thunks save the full extended state with the XCR0 mask, minus the AMX tile components (17, 18) */
    uint32_t r[4]; cpuidx(1, 0, r);
    if (!((r[2] >> 27) & 1) || !((r[2] >> 26) & 1)) { LOG("NOBS INIT no OSXSAVE/XSAVE: the R-STACK thunks need XSAVE64\n"); return 0; }
    uint64_t xcr0 = xgetbv0(), mask = xcr0 & ~0x60000ULL;
    cpuidx(0xd, 0, r); uint32_t xsize = r[1];
    cpuidx(0xd, 2, r); nobs_xoff_avx = r[1];
    nobs_xmask[0] = (uint32_t)mask; nobs_xmask[1] = (uint32_t)(mask >> 32);
    LOG("NOBS INIT xcr0=0x%llx xsave_mask=0x%llx xsave_size=%u avx_off=%u\n", (unsigned long long)xcr0, (unsigned long long)mask, xsize, nobs_xoff_avx);
    if (xsize < 576 || xsize > 0x10000) return 0;
    for (int i = 0; i < NOBS_H_N; i++) {
        uint8_t *a = VirtualAlloc(NULL, (xsize + 4095) & ~4095u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   /* page = 64-byte aligned, zeroed header */
        uint8_t *s = VirtualAlloc(NULL, 256u << 10, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);              /* fully committed private stack */
        if (!a || !s) return 0;
        nobs_xarea[i] = a;
        nobs_pstk_lo[i] = (uintptr_t)s; nobs_pstk_hi[i] = (uintptr_t)s + (256u << 10);
        g_pstacks[i] = s + (256u << 10) - 64;
    }
    nobs_ring = VirtualAlloc(NULL, sizeof(NobsCell) * NOBS_RING_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!nobs_ring) return 0;
    for (uint32_t i = 0; i < NOBS_RING_CAP; i++) nobs_ring[i].seq = i;
    if (g_cfg.netobs_trace) {
        char path[MAX_PATH]; size_t L = strlen(g_log_path);
        snprintf(path, sizeof path, "%.*s.nobs", (int)(L > 4 ? L - 4 : L), g_log_path);
        g_trace = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        g_trace_cap = (uint64_t)g_cfg.netobs_trace_mb << 20;
        if (h_ok(g_trace)) {
            uint32_t h[4] = { 0x53424f4eu /* "NOBS" */, 2, 0x61717836u, (uint32_t)sizeof(NobsRec) };
            DWORD w; WriteFile(g_trace, h, sizeof h, &w, NULL); g_trace_bytes = sizeof h;
            LOG("NOBS TRACE file=%s records=0 bytes=%llu full=0 recsz=%u\n", path, (unsigned long long)g_trace_bytes, (unsigned)sizeof(NobsRec));
        }
    }
    nobs_self_range(&g_self_lo, &g_self_hi);
    AddVectoredExceptionHandler(1, nobs_veh);   /* record-only: our own faults get a CRASH line, then CONTINUE_SEARCH */
    g_writer = CreateThread(NULL, 1u << 20, writer_main, NULL, 0, NULL);
    return g_writer != NULL;
}

void nobs_shutdown(int process_exit) {
    if (process_exit) log_set_bounded(1);   /* m7: never spin forever on the log lock a dead thread may hold */
    g_writer_stop = 1;
    DWORD wr = g_writer ? WaitForSingleObject(g_writer, process_exit ? 0 : 3000) : WAIT_OBJECT_0;
    if (wr == WAIT_OBJECT_0 && nobs_ring) { NobsRec r; while (ring_pop(&r)) handle(&r); }   /* single consumer: only if the writer is gone */
    bat_finish();
    LOG("NOBS T1 enabled=%d cost_off=%d frames=%d digest=0x%llx (M1b proxy: input/ring per frame, NOT the full c1a tier-1)\n",
        g_cfg.netobs_t1, g_t1_off, g_t1_frames, (unsigned long long)g_t1_hash);
    LOG("NOBS TRACE records=final bytes=%llu full=%d\n", (unsigned long long)g_trace_bytes, g_trace_full);
    LOG("NOBS FINAL battles=%d ticks=%u unexpected=%d dropped_recs=%lld regs=%s writer_wait=%lu\n", g_battles, nobs_tick, g_unexp_total,
        (long long)nobs_ring_drops, !g_regs_seen ? "?" : g_regs_bad ? "BAD" : "ok", wr);
    if (h_ok(g_trace)) { FlushFileBuffers(g_trace); CloseHandle(g_trace); g_trace = NULL; }
    log_flush();
}
