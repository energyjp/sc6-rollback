/* C2 (C2-SPEC.md §2.3-§2.10): the netplay session of mode=rbnet. See netsess.h.
 * Game thread only (the net thread is net.c). Every refusal, abort and stall is a log line; nothing falls back silently. */
#include "netsess.h"
#include "net.h"
#include "netproto.h"
#include "sha256.h"
#include "entry.h"
#include "modes.h"
#include "c1a_run.h"
#include "rb.h"
#include "emu.h"
#include <stdlib.h>
#include <math.h>
#include <xmmintrin.h>

#define T_MS   16.6667
#define NR     512                      /* the input / check tables (frames, ring) */
#define NCR    256                      /* the tier-1 component ring */
#define HB_MS  2000.0                   /* NORESP after this long without a packet */

/* ------------------------------------------------------------------ configuration (validated by rb_cfg_check) */
static int g_on, s_role, s_script, s_lrec, s_D, s_maxrb, s_lock_fixed, s_lever_one, s_timeout_s, s_pausecap_s;
static uint32_t s_peer_ip; static uint16_t s_peer_port;
static uint32_t s_nonce, s_seq;
static int s_netup;                      /* the socket is up */
/* ------------------------------------------------------------------ session (process lifetime) */
enum { SS_HELLO = 0, SS_IDLE, SS_REFUSED, SS_QUIT, SS_SOCKFAIL };
static int s_ss;
static char s_refcat[16];               /* REFUSED-<CAT> (session) */
static NpHello s_hl, s_ph;               /* local / peer HELLO */
static int s_hl_ready, s_ph_ok;
static uint32_t s_peer_nonce;
static double s_last_rx, s_last_hello;
static int s_noresp;
static volatile LONG s_ms_ready; static uint64_t s_modset2; static uint32_t s_ms_files;
static char s_envdiff[160];
/* seeds */
static uint32_t s_seed_seq, s_seed_k, s_seed_acked;      /* host: issued k, its seed, the last acked k */
static uint32_t s_rx_seed_seq, s_rx_seed; static int s_rx_seed_ok;   /* guest */
static double s_seed_tx;
/* RTT (INPUT ping / echo) */
#define PINGR 256
static uint64_t s_ping_qpc[PINGR]; static uint32_t s_ping_id;
static uint32_t s_echo_id; static uint64_t s_echo_qpc;  /* the peer's last ping id and its receive time */
static double s_rtt[64]; static int s_nrtt, s_irtt;
static float *s_rtt_all; static uint32_t s_nrtt_all;
/* time sync */
static double s_adv[60]; static int s_nadv, s_iadv; static double s_peer_adv;
static uint32_t s_peer_s, s_last_sync;
static int s_peer_paused, s_local_paused; static double s_pause_t0;
static void *s_mgr;
/* outgoing control resend queue (QUIT / ABORT x5, ZONE x3, PAUSE x3); fix r1: 64 entries (was 16), a full queue is a loud NETQ line */
#define NRS 64
typedef struct { uint8_t type; uint16_t battle; uint8_t b[64]; uint8_t n; int left; double next, every; } Rs;
static Rs s_rs[NRS]; static int s_nrs; static uint32_t s_rs_full;
static int s_quit_pending;

/* ------------------------------------------------------------------ the battle */
enum { NB_NONE = 0, NB_BARRIER, NB_ON, NB_DONE };
typedef struct { uint64_t t1, t15; uint32_t ued, f; uint8_t have, cmp; } Chk;
typedef struct {
    int st; char tok[24];
    uint16_t bat, peer_bat; int peer_bat_ok;
    uint32_t seed_seq, seed; int seed_net;
    double t_enter, t_done;
    int inited, setup_seen, built; uint32_t pre_setup_ticks; uint64_t c1a_b, rb_b;   /* fix r1: the barrier work waits for Setup; this battle's C1a / rb numbers (the .rec name, RBEND b=) */
    uint8_t blob[4096]; size_t blen; uint64_t bfnv; int blob_acked; double blob_tx;
    uint8_t pblob[4096]; uint32_t pgot; uint16_t pcount; uint32_t plen; uint64_t pfnv; int pok;
    int matched, ready_tx_n; uint64_t setup_x, setup_full, peer_full; int peer_ready, peer_input; double ready_tx;
    int dl, peer_dl;
    uint64_t tree[4];
    /* the input tables */
    NpRec L[NR]; uint32_t Lf[NR]; uint32_t next_t; NpRec last; int pend; NpRec pendr; uint64_t fills, merges;
    NpRec R[NR]; uint32_t Rf[NR]; uint32_t F; uint32_t peer_ack;
    uint64_t rx_dup, rx_old, conflicts;
    /* checks */
    Chk CK[NR], PK[NR]; uint32_t cconf, pc_contig, peer_cack;
    uint64_t comps[NCR][C1A_NCOMP]; uint32_t compf[NCR];
    uint32_t checked, t1_diff, t15_diff, ued_diff; int kd_lines, ud_lines;
    /* desync / dumps */
    int desync; uint32_t ds_f; uint64_t ds_lt1, ds_pt1; int ds_first_local; double ds_note_tx;
    int dump_kind; uint32_t dump_g, dump_f; int dump_done, dump_late, dump_reqd, dump_acked, khit_dumps; double dump_t, dumpreq_tx;
    uint64_t peer_comps[C1A_NCOMP]; int peer_comps_ok;
    int abort_reason; uint32_t abort_frame; int abort_by_peer; int want_abort; char abort_cause[32];
    int ctltype;
    /* stalls, stats */
    uint64_t stall[6]; int stall_lines[6];
    uint32_t depth_hist[16]; int depth_max;
    double zone_ms; uint32_t zone_n; double last_step_ms; uint32_t last_step_s;
    uint32_t s_last; uint32_t C;
    /* zones (ZONE messages) */
    uint32_t zl[64][2]; int nzl; uint32_t pzl[64][2]; int npzl; int zdiff_lines;
    uint64_t netff;
    int end_logged;
    uint32_t khit_last_g, ued_last;
    uint32_t peer_seq0; int peer_seq0_ok, peer_left; double peer_disarm_t;   /* fix r1: the first seq of the peer's INPUT for this battle (older packets
                                                                                * are stale), the peer left (1) / ignored by the control (-1), DISARM seen */
} Bat;
static Bat *B;
static long long g_crcbad, g_badlen;   /* datagrams dropped by the decoder (CRC / magic or length) */
static uint16_t s_bat_ctr;
static double s_tail_until;               /* after Shutdown: keep sending this battle's INPUT until then */
enum { NSC_START = 0, NSC_MAXRB, NSC_LOCK, NSC_SYNC, NSC_PAUSE, NSC_PEER };
static const char *const k_sc[6] = { "start", "maxrb", "lock", "sync", "pause", "peer" };
/* the script peer (launch E) and the local recording (net_local_src=rec) */
static C1aIn *s_scr; static uint32_t s_nscr, s_scr_from;
static C1aIn *s_lr; static uint32_t s_nlr;
/* light-dump history: the records of both sides (last 120 frames are read from the rings) */

/* ------------------------------------------------------------------ helpers */
static double nowms(void) { return now_ms(); }
static uint64_t qnow(void) { return qpc(); }
static void title_dirty(void) { g_title_dirty = 1; }
static const char *role_s(void) { return s_role == 1 ? "host" : "guest"; }
int ns_active(void) { return g_on; }
int ns_remote_side(void) { return s_role == 1 ? 2 : 1; }
int ns_lipsync_off(void) { return 1; }

static void set_abort(int reason, uint32_t frame, int by_peer, const char *how);
static void send_raw(uint8_t type, uint16_t battle, const void *pay, size_t n) {
    if (!s_netup) return;
    uint8_t dg[NP_MAXDG]; NpHdr h; memset(&h, 0, sizeof h);
    h.type = type; h.nonce_tx = s_nonce; h.nonce_rx = s_ph_ok ? s_peer_nonce : 0; h.battle = battle; h.seq = ++s_seq;
    size_t k = np_encode(dg, sizeof dg, &h, pay, n);
    if (!k) { LOG("NETQ encode_refused type=%s len=%zu (a payload over %d bytes: a bug)\n", k_np_type[type < NP_NTYPES ? type : 0], n, NP_MAXPAY); return; }
    if (!net_tx_push(dg, k)) {
        static int lines; if (lines++ < 20) LOG("NETQ overflow dir=tx n=%lld\n", (long long)net_counters()->tx_overflow);
        if (B && (B->st == NB_ON || B->st == NB_BARRIER) && !B->abort_reason) set_abort(NPR_NETQ_OVERFLOW, B->s_last, 0, NULL);
    }
}
static void rs_add(uint8_t type, uint16_t battle, const void *pay, size_t n, int times, double every) {
#ifdef SC6RB_HARNESS
    if (g_cfg.h_c2_noabort_tx && (type == NP_ABORT || type == NP_QUIT)) { LOG("NETQ harness plant h_c2_noabort_tx=1: %s not sent (all copies lost)\n", k_np_type[type]); return; }
#endif
    if (s_nrs >= NRS || n > 64) {   /* fix r1: never silent (a lost ABORT is still covered by the DISARM flag of every INPUT, read since fix r1) */
        s_rs_full++;
        if (s_rs_full <= 20) LOG("NETQ overflow dir=resend type=%s battle=%u n=%zu queued=%d lost=%u (a control message is not queued)\n", k_np_type[type < NP_NTYPES ? type : 0], battle, n, s_nrs, s_rs_full);
        log_flush();
        return;
    }
    Rs *r = &s_rs[s_nrs++]; r->type = type; r->battle = battle; memcpy(r->b, pay, n); r->n = (uint8_t)n; r->left = times; r->next = 0; r->every = every;
}
static void rs_run(double t) {
    int sent = 0;   /* fix r1: at most 16 resends per tick (the queue holds 64; the tx ring holds NET_TX_SLOTS = 64 datagrams: a full queue in one
                     * tick overflowed it, harness NRSQ); the rest go out on the next ticks */
    for (int i = 0; i < s_nrs && sent < 16; i++) {
        Rs *r = &s_rs[i];
        if (r->left > 0 && t >= r->next) { sent++; send_raw(r->type, r->battle, r->b, r->n); r->left--; r->next = t + r->every; }
    }
    int w = 0; for (int i = 0; i < s_nrs; i++) if (s_rs[i].left > 0) s_rs[w++] = s_rs[i];
    s_nrs = w;
}

/* ------------------------------------------------------------------ RTT */
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void rtt_stats(double *p50, double *jit) {
    *p50 = 0; *jit = 0;
    if (!s_nrtt) return;
    double v[64]; int n = s_nrtt; memcpy(v, s_rtt, sizeof(double) * (size_t)n);
    qsort(v, (size_t)n, sizeof(double), dcmp);
    *p50 = v[n / 2];
    double d[64]; for (int i = 0; i < n; i++) d[i] = fabs(v[i] - *p50);
    qsort(d, (size_t)n, sizeof(double), dcmp);
    *jit = d[n / 2];
}
static void rtt_add(double ms) {
    if (ms < 0 || ms > 5000) return;
    s_rtt[s_irtt] = ms; s_irtt = (s_irtt + 1) % 64; if (s_nrtt < 64) s_nrtt++;
    if (!s_rtt_all) s_rtt_all = (float *)malloc(sizeof(float) * 65536);
    if (s_rtt_all && s_nrtt_all < 65536) s_rtt_all[s_nrtt_all++] = (float)ms;
}
static int fcmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static double rtt_all_pct(double p) {
    if (!s_nrtt_all) return 0;
    float *c = (float *)malloc(sizeof(float) * s_nrtt_all); if (!c) return 0;
    memcpy(c, s_rtt_all, sizeof(float) * s_nrtt_all); qsort(c, s_nrtt_all, sizeof(float), fcmpf);
    double r = c[(uint32_t)(p * (s_nrtt_all - 1) + 0.5)]; free(c); return r;
}
static double adv_avg(void) { double s = 0; for (int i = 0; i < s_nadv; i++) s += s_adv[i]; return s_nadv ? s / s_nadv : 0; }

/* ------------------------------------------------------------------ HELLO fields */
static void cpuid4(uint32_t leaf, uint32_t sub, uint32_t r[4]) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub)); }
static void ucrt_version(uint32_t *ms, uint32_t *ls) {
    *ms = *ls = 0;
    HMODULE uc = GetModuleHandleA("ucrtbase.dll"); char path[MAX_PATH];
    if (!uc || !GetModuleFileNameA(uc, path, sizeof path)) return;
    HMODULE vd = LoadLibraryA("version.dll"); if (!vd) return;
    typedef DWORD (WINAPI *GFVIS)(LPCSTR, LPDWORD); typedef BOOL (WINAPI *GFVI)(LPCSTR, DWORD, DWORD, LPVOID); typedef BOOL (WINAPI *VQV)(LPCVOID, LPCSTR, LPVOID *, PUINT);
    GFVIS gs = (GFVIS)(void *)GetProcAddress(vd, "GetFileVersionInfoSizeA"); GFVI gi = (GFVI)(void *)GetProcAddress(vd, "GetFileVersionInfoA"); VQV vq = (VQV)(void *)GetProcAddress(vd, "VerQueryValueA");
    if (gs && gi && vq) {
        DWORD hdl = 0, n = gs(path, &hdl);
        void *b = n ? malloc(n) : NULL;
        if (b && gi(path, 0, n, b)) { VS_FIXEDFILEINFO *fi = NULL; UINT l = 0; if (vq(b, "\\", (LPVOID *)&fi, &l) && fi) { *ms = fi->dwFileVersionMS; *ls = fi->dwFileVersionLS; } }
        free(b);
    }
}
extern HMODULE g_ns_self;   /* main.c: the plugin module */
static void plugins_dir(char *out, size_t cap) {
    out[0] = 0;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_modset_root[0]) { snprintf(out, cap, "%s\\Binaries\\Win64\\plugins", g_cfg.h_modset_root); return; }
#endif
    char p[MAX_PATH]; if (!GetModuleFileNameA(g_ns_self, p, sizeof p)) return;
    char *sl = strrchr(p, '\\'); if (sl) *sl = 0;
    snprintf(out, cap, "%s", p);
}
static uint64_t plugins_hash(void) {
    char d[MAX_PATH]; plugins_dir(d, sizeof d);
    if (!d[0]) return 0;
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*.asi", d);
    struct { char n[64]; uint64_t sz; } e[64]; int ne = 0;
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (ne < 64 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { size_t i = 0; for (; fd.cFileName[i] && i < 63; i++) { char c = fd.cFileName[i]; e[ne].n[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } e[ne].n[i] = 0;
                                                                                 e[ne].sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; ne++; } } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (int i = 1; i < ne; i++) for (int j = i; j > 0 && strcmp(e[j - 1].n, e[j].n) > 0; j--) { __typeof__(e[0]) t = e[j]; e[j] = e[j - 1]; e[j - 1] = t; }
    uint64_t hh = 0xcbf29ce484222325ULL;
    for (int i = 0; i < ne; i++) { hh = np_fnv64(e[i].n, strlen(e[i].n) + 1, hh); hh = np_fnv64(&e[i].sz, 8, hh); }
    return hh;
}
/* modset2 (§2.3): every .pak under Content\Paks (recursive): relative path, size, SHA-256 of the first and last MB; ~mods paks in full.
 * The game folder is only read. Background thread at start. */
static char s_ms_root[MAX_PATH];
static void ms2_walk(const char *root, const char *dir, Sha256 *top, int depth) {
    if (depth > 16) return;
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    /* sorted: collect names first */
    char (*names)[MAX_PATH] = (char (*)[MAX_PATH])malloc(sizeof(char[MAX_PATH]) * 512); int nn = 0; int *isdir = (int *)malloc(sizeof(int) * 512);
    if (!names || !isdir) { free(names); free(isdir); FindClose(h); return; }
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || nn >= 512) continue;
        snprintf(names[nn], MAX_PATH, "%s", fd.cFileName); isdir[nn] = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT); nn++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (int i = 1; i < nn; i++) for (int j = i; j > 0 && _stricmp(names[j - 1], names[j]) > 0; j--) { char t[MAX_PATH]; memcpy(t, names[j], MAX_PATH); memcpy(names[j], names[j - 1], MAX_PATH); memcpy(names[j - 1], t, MAX_PATH); int ti = isdir[j]; isdir[j] = isdir[j - 1]; isdir[j - 1] = ti; }
    for (int i = 0; i < nn; i++) {
        char full[MAX_PATH]; snprintf(full, sizeof full, "%s\\%s", dir, names[i]);
        if (isdir[i]) { ms2_walk(root, full, top, depth + 1); continue; }
        const char *dot = strrchr(names[i], '.'); if (!dot || _stricmp(dot, ".pak")) continue;
        HANDLE f = CreateFileA(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
        const char *rel = full + strlen(root) + 1;
        char lrel[MAX_PATH]; size_t k = 0; for (; rel[k] && k + 1 < sizeof lrel; k++) { char c = rel[k]; lrel[k] = c == '\\' ? '/' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } lrel[k] = 0;
        int full_hash = strstr(lrel, "~mods") != NULL;
        Sha256 s; sha256_init(&s);
        uint8_t *buf = (uint8_t *)malloc(1 << 20); DWORD got;
        if (buf) {
            if (full_hash) { while (ReadFile(f, buf, 1 << 20, &got, NULL) && got) sha256_update(&s, buf, got); }
            else {
                if (ReadFile(f, buf, 1 << 20, &got, NULL)) sha256_update(&s, buf, got);
                if (sz.QuadPart > (1 << 20)) { LARGE_INTEGER o; o.QuadPart = sz.QuadPart - (1 << 20); SetFilePointerEx(f, o, NULL, FILE_BEGIN); if (ReadFile(f, buf, 1 << 20, &got, NULL)) sha256_update(&s, buf, got); }
            }
            free(buf);
        }
        CloseHandle(f);
        uint8_t d[32]; sha256_final(&s, d);
        sha256_update(top, lrel, strlen(lrel) + 1); sha256_update(top, &sz.QuadPart, 8); sha256_update(top, d, 32);
        s_ms_files++;
    }
    free(names); free(isdir);
}
static DWORD WINAPI ms2_thread(LPVOID p) {
    (void)p;
    Sha256 top; sha256_init(&top);
    if (s_ms_root[0]) { char d[MAX_PATH]; snprintf(d, sizeof d, "%s\\Content\\Paks", s_ms_root); ms2_walk(s_ms_root, d, &top, 0); }
    uint8_t dg[32]; sha256_final(&top, dg);
    uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | dg[i];
    s_modset2 = v;
    MemoryBarrier();
    InterlockedExchange(&s_ms_ready, 1);
    return 0;
}
static void hello_build(void) {
    NpHello *h = &s_hl; memset(h, 0, sizeof *h);
    h->role = (uint8_t)s_role;
    snprintf(h->version, sizeof h->version, "%s", SC6RB_VERSION);
    {   char p[MAX_PATH]; GetModuleFileNameA(g_ns_self, p, sizeof p); h->plugin_sha = sha256_file_prefix64(p); }
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    h->exe_ts = nt->FileHeader.TimeDateStamp; h->exe_soi = nt->OptionalHeader.SizeOfImage;
    h->modset2 = s_modset2; h->plugins_list = plugins_hash(); h->simcfg_net = ns_simcfg_net();
    h->mxcsr = _mm_getcsr(); { unsigned short cw; __asm__ volatile("fnstcw %0" : "=m"(cw)); h->x87cw = cw; }
    ucrt_version(&h->ucrt_ms, &h->ucrt_ls);
    {   typedef LONG (NTAPI *RGV)(OSVERSIONINFOW *); RGV rgv = (RGV)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW ov; memset(&ov, 0, sizeof ov); ov.dwOSVersionInfoSize = sizeof ov; if (rgv) rgv(&ov); h->win_build = ov.dwBuildNumber; }
    uint32_t r[4]; cpuid4(0, 0, r); uint32_t maxl = r[0];
    memcpy(h->cpu_vendor, &r[1], 4); memcpy(h->cpu_vendor + 4, &r[3], 4); memcpy(h->cpu_vendor + 8, &r[2], 4);
    cpuid4(1, 0, r); h->cpu_fms = r[0]; h->cpu_feat = ((r[2] >> 12) & 1);
    if (maxl >= 7) { cpuid4(7, 0, r); h->cpu_feat |= ((r[1] >> 5) & 1) << 1; h->cpu_feat |= ((r[1] >> 16) & 1) << 2; }
#ifdef SC6RB_HARNESS
    {   const char *hs = g_cfg.h_c2_hs;   /* harness-only plants (NHS-*): one HELLO field differs */
        if (!strcmp(hs, "plugin")) h->plugin_sha ^= 1; else if (!strcmp(hs, "exe")) h->exe_ts ^= 1; else if (!strcmp(hs, "modset")) h->modset2 ^= 1;
        else if (!strcmp(hs, "plugins")) h->plugins_list ^= 1; else if (!strcmp(hs, "version")) h->version[0] = 'x';
        if (hs[0]) LOG("NETHS harness plant h_c2_hs=%s (one HELLO field flipped: a negative control)\n", hs);
    }
#endif
    s_hl_ready = 1;
    LOG("NETHS tx role=%s ver=%s plugin=%016llx exe=%08x:%x modset2=%016llx plugins=%016llx simcfg=%016llx (modset2 over %u .pak files)\n", role_s(), h->version,
        (unsigned long long)h->plugin_sha, h->exe_ts, h->exe_soi, (unsigned long long)h->modset2, (unsigned long long)h->plugins_list, (unsigned long long)h->simcfg_net, s_ms_files);
    LOG("NETENV local mxcsr=%08x x87cw=%04x ucrt=%u.%u.%u.%u win_build=%u cpu=%.12s fms=%08x feat=%x\n", h->mxcsr, h->x87cw, h->ucrt_ms >> 16, h->ucrt_ms & 0xffff, h->ucrt_ls >> 16,
        h->ucrt_ls & 0xffff, h->win_build, h->cpu_vendor, h->cpu_fms, h->cpu_feat);
}
uint64_t ns_simcfg_net(void) {
    const Config *c = &g_cfg;
    uint64_t h = cfg_simcfg_hash(c);
    RbCfgVals v; memset(&v, 0, sizeof v);   /* the rbcfg values as rblive computes them, minus the per-PC keys (remote side, net_delay, emu_*) */
    v.max_rb = c->net_max_rollback; v.state_slots_mask = c->rb_state_mask; v.mt_tail = c->rb_mt_tail; v.pinguard = c->rb_pinguard; v.quarantine = c->rb_quarantine;
    v.t2_heap_tag = c->c1a_t2_heap_tag; v.rollback = c->rb_rollback; v.skip_restore = c->rb_skip_restore; v.tick_limit = c->rb_tick_limit; v.state_gate = c->rb_state_gate;
    v.test_layout_at = c->rb_test_layout_at; v.state_quiet = c->rb_state_quiet;
    v.zone = c->rb_zone; v.zone_mode = c->rb_zone_fight_mode; v.zone_gap = c->rb_zone_entry_gap; v.zone_margin = c->rb_zone_exit_margin;
    v.live = 2; v.live_cap = c->live_cap;
    uint64_t r = rb_cfg_hash(&v);
    int64_t x[] = { c->net_max_rollback, c->rb_zone, c->rb_zone_fight_mode, c->rb_zone_entry_gap, c->rb_zone_exit_margin, c->lock_delay, c->l4_miss, c->c1a_t1_rule };
    uint64_t k = np_fnv64(x, sizeof x, 0xcbf29ce484222325ULL);
    return h ^ (r * 0x9E3779B97F4A7C15ULL) ^ (k * 0xC2B2AE3D27D4EB4FULL);
}

/* ------------------------------------------------------------------ startup */
static int load_rec_side(const char *path, int side, C1aIn **out, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char line[16384];
    uint32_t cap = 0; *n = 0; *out = NULL;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'F' || line[1] != ' ') continue;
        C1aFrame fr; if (!c1a_parse_frame(line, &fr)) continue;
        if (*n == cap) { cap = cap ? cap * 2 : 8192; C1aIn *ne = (C1aIn *)realloc(*out, sizeof(C1aIn) * cap); if (!ne) { fclose(f); return 0; } *out = ne; }
        C1aIn r; memset(&r, 0, sizeof r);
        if (side == 1) { r.p1 = fr.in.p1; memcpy(r.x, fr.in.x, 0x18); } else r.p2 = fr.in.p2;
        r.ff = 7; (*out)[(*n)++] = r;
    }
    fclose(f);
    return *n > 0;
}
void ns_start(void) {
    const Config *c = &g_cfg;
    s_role = c->net_role; s_script = c->net_peer_kind == 1; s_lrec = c->net_local_src == 1; s_D = c->net_delay; s_maxrb = c->net_max_rollback;
    s_lock_fixed = c->lock_delay; s_lever_one = c->net_lever == 1; s_timeout_s = c->net_timeout_s; s_pausecap_s = c->pause_cap_s;
    {   LARGE_INTEGER q; QueryPerformanceCounter(&q); s_nonce = (uint32_t)(q.QuadPart ^ (q.QuadPart >> 32) ^ ((uint64_t)GetCurrentProcessId() << 16)); if (!s_nonce) s_nonce = 1; }
    B = (Bat *)VirtualAlloc(NULL, sizeof(Bat), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!B) { LOG("NETUP FAIL wsa=-1 (no memory for the battle tables)\n"); s_ss = SS_SOCKFAIL; snprintf(s_refcat, sizeof s_refcat, "SOCKET"); g_on = 1; return; }
    memset(B, 0, sizeof *B); snprintf(B->tok, sizeof B->tok, "-");
    int loop = 0;
    if (!s_script) { net_parse_peer(c->net_peer, &s_peer_ip, &s_peer_port); loop = net_is_loopback(s_peer_ip); }
    char lt[24]; if (s_lock_fixed) snprintf(lt, sizeof lt, "%d", s_lock_fixed); else snprintf(lt, sizeof lt, "auto");
    LOG("NETCFG role=%s peer=%s port=%d bind=%s D=%d max_rb=%d lock_delay=%s lever=%s local_src=%s timeout_s=%d pause_cap_s=%d quit=%s simcfg_net=%016llx\n", role_s(),
        s_script ? "script" : loop ? "set(loopback)" : "set(remote)", c->net_port, c->net_bind == 1 ? "loopback" : c->net_bind == 2 ? "any" : "auto", s_D, s_maxrb, lt,
        s_lever_one ? "one" : "clamp", s_lrec ? "rec" : "pad", s_timeout_s, s_pausecap_s, c->key_quit ? "F3" : "off", (unsigned long long)ns_simcfg_net());
    if (s_lrec && !load_rec_side(c->net_rec_file, s_role == 1 ? 1 : 2, &s_lr, &s_nlr)) LOG("NETREC load FAIL (net_rec_file unreadable)\n");
    else if (s_lrec) LOG("NETREC local side from a recording: frames=%u side=%s\n", s_nlr, s_role == 1 ? "p1+x" : "p2");
    g_on = 1;
    if (s_script) {
        if (!load_rec_side(c->net_script_rec, 1, &s_scr, &s_nscr)) { LOG("NETUP FAIL script (net_script_rec unreadable)\n"); s_ss = SS_SOCKFAIL; snprintf(s_refcat, sizeof s_refcat, "SOCKET"); return; }
        s_scr_from = (uint32_t)(c->net_script_from > 0 ? c->net_script_from : 1);
        if (s_scr_from > s_nscr) s_scr_from = 1;
        LOG("NETUP ok script frames=%u from=%u (net_peer_kind=script: no socket; the remote P1 words + x come from the recording, looped)\n", s_nscr, s_scr_from);
        s_ss = SS_IDLE; s_hl_ready = 1; s_ph_ok = 1;
        title_dirty();
        return;
    }
    /* modset2 root: the exe is <game>\SoulcaliburVI\Binaries\Win64\SoulcaliburVI.exe */
    s_ms_root[0] = 0;
#ifdef SC6RB_HARNESS
    if (c->h_modset_root[0]) snprintf(s_ms_root, sizeof s_ms_root, "%s", c->h_modset_root);
#endif
    if (!s_ms_root[0]) {
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
        char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        size_t l = strlen(exe), k = strlen("\\Binaries\\Win64");
        if (l > k && !_stricmp(exe + l - k, "\\Binaries\\Win64")) { exe[l - k] = 0; snprintf(s_ms_root, sizeof s_ms_root, "%s", exe); }
    }
    HANDLE th = CreateThread(NULL, 0, ms2_thread, NULL, 0, NULL); if (th) CloseHandle(th); else ms2_thread(NULL);
    NetCfg nc; memset(&nc, 0, sizeof nc);
    nc.port = (uint16_t)c->net_port; nc.peer_ip = s_peer_ip; nc.peer_port = s_peer_port;
    nc.bind_loopback = c->net_bind == 1 || (c->net_bind == 0 && loop);
#ifdef SC6RB_HARNESS
    snprintf(nc.netem, sizeof nc.netem, "%s", c->h_netem); nc.netem_dir = s_role == 1 ? 0 : 1; nc.netem_seed = (uint64_t)c->h_netem_seed + (uint64_t)s_role;
    nc.h_corrupt_every = c->h_c2_corrupt;
#endif
    int wsa = 0;
    if (!net_start(&nc, &wsa)) { LOG("NETUP FAIL wsa=%d (netplay off for this process; title net=REFUSED-SOCKET)\n", wsa); s_ss = SS_SOCKFAIL; snprintf(s_refcat, sizeof s_refcat, "SOCKET"); title_dirty(); return; }
    s_netup = 1;
    LOG("NETUP ok bind=%s%s\n", nc.bind_loopback ? "loopback" : "any",
#ifdef SC6RB_HARNESS
        c->h_netem[0] ? " netem=on(harness)" : ""
#else
        ""
#endif
        );
    s_ss = SS_HELLO; s_last_rx = nowms();
    title_dirty();
}

/* ------------------------------------------------------------------ the battle: reset, records */
static void bat_reset(void) {
    memset(B, 0, sizeof *B);
    snprintf(B->tok, sizeof B->tok, "-");
    entry_battle_reset();
    s_nadv = s_iadv = 0; s_peer_adv = 0; s_last_sync = 0;
    s_nrtt_all = 0;
}
static void lput(uint32_t f, const NpRec *r) { B->L[f % NR] = *r; B->Lf[f % NR] = f; }
static int lget(uint32_t f, NpRec *r) { if (!f || B->Lf[f % NR] != f) return 0; *r = B->L[f % NR]; return 1; }
static int rget(uint32_t f, NpRec *r) { if (!f || B->Rf[f % NR] != f) return 0; *r = B->R[f % NR]; return 1; }
static NpRec rec_from_ue(const C1aIn *ue) {
    NpRec r; memset(&r, 0, sizeof r);
    r.w = ue->p1;                                   /* host: P1's word; guest: its own pad's P1 word, remapped to the P2 record */
    if (s_role == 1) memcpy(r.x, ue->x, 24);        /* the guest drops its own UE x (D3) */
    return r;
}
static NpRec rec_from_lr(uint32_t f, int *ok) {
    NpRec r; memset(&r, 0, sizeof r); *ok = 0;
    if (!s_lr || f < 1 || f > s_nlr) return r;
    const C1aIn *c = &s_lr[f - 1];
    r.w = s_role == 1 ? c->p1 : c->p2; if (s_role == 1) memcpy(r.x, c->x, 24);
    *ok = 1;
    return r;
}
static int s_last_abort;
const char *ns_last_abort(void) { return s_last_abort ? k_np_reason[s_last_abort] : ""; }
static void set_abort(int reason, uint32_t frame, int by_peer, const char *how) {
    if (!B->st || B->st == NB_DONE || B->abort_reason) return;
    s_last_abort = reason;
    B->abort_reason = reason; B->abort_frame = frame; B->abort_by_peer = by_peer;
    B->want_abort = 1;
    snprintf(B->abort_cause, sizeof B->abort_cause, "net_%s", k_np_reason[reason]);
    LOG("NETABORT reason=%s f=%u by=%s%s%s\n", k_np_reason[reason], frame, by_peer ? "peer" : "local", how ? " " : "", how ? how : "");
    if (!by_peer) { NpEnd e = { (uint8_t)reason, frame }; uint8_t b[8]; size_t n = np_end_put(b, &e); rs_add(NP_ABORT, B->bat, b, n, 5, 20.0); }
    const char *tk = reason == NPR_DESYNC ? "DESYNC" : reason == NPR_PEER_TIMEOUT ? "ABORT-TIMEOUT" : reason == NPR_CTLTYPE ? "ABORT-CTLTYPE" :
                     reason == NPR_BARRIER_MISPREDICT ? "ABORT-BARRIER" : reason == NPR_DEPTH ? "ABORT-DEPTH" : reason == NPR_PAUSE_CAP ? "ABORT-PAUSECAP" :
                     reason == NPR_PEER_RESTART ? "ABORT-RESTART" : reason == NPR_NETQ_OVERFLOW ? "ABORT-NETQ" : reason == NPR_REC_END ? "ABORT-RECEND" :
                     reason == NPR_QUIT_F3 ? "QUIT" : reason == NPR_PEER_LEFT ? "ABORT-PEERLEFT" : "ABORT-INTERNAL";
    snprintf(B->tok, sizeof B->tok, "%s", tk);
    if (B->st == NB_BARRIER) { entry_restore(0); B->st = NB_DONE; }
    title_dirty();
    log_flush();
}
static void refuse_battle(const char *cat_log, const char *cat_title, int tell_peer, int cat_idx) {
    if (B->st == NB_DONE) return;
    LOG("NETSETUP REFUSE cat=%s b=%u (netplay disarmed for this battle: it runs locally vs CPU; title net=REFUSED-%s)\n", cat_log, B->bat, cat_title);
    entry_restore(0);
    snprintf(B->tok, sizeof B->tok, "REFUSED-%s", cat_title);
    B->st = NB_DONE; B->t_done = nowms();
    if (tell_peer && B->bat) { NpEnd e = { NPR_REFUSED, 0xff000000u | (uint32_t)cat_idx }; uint8_t b[8]; size_t n = np_end_put(b, &e); rs_add(NP_ABORT, B->bat, b, n, 5, 20.0); }
    title_dirty();
    log_flush();
}
static const char *const k_refcat[] = { "BATTLE", "SETUP", "CONTENT", "PARAMS", "SPANS", "ENTRY", "BARRIER", "SEED" };

/* ------------------------------------------------------------------ Init / Setup / Shutdown */
int ns_seed_for_init(uint32_t ue_seed, uint32_t *used) {
    *used = ue_seed;
    if (!g_on || ue_seed == C1A_DEMO_SEED) return 0;
    if (s_script) return 0;
    if (s_ss != SS_IDLE) return 0;
    if (s_role == 1) { if (s_seed_seq && s_seed_acked == s_seed_seq) { *used = s_seed_k; return 1; } return 0; }
    if (s_rx_seed_ok) { *used = s_rx_seed; return 1; }
    return 0;
}
void ns_on_init(uint32_t ue_seed) {
    if (!g_on) return;
    ns_battle_end("init", B->s_last, B->C);
    bat_reset();
    if (ue_seed == C1A_DEMO_SEED) { LOG("NETBARRIER skip why=demo_battle (UE's demo Init seed %u: no netplay)\n", C1A_DEMO_SEED); snprintf(B->tok, sizeof B->tok, "-"); return; }
    B->bat = ++s_bat_ctr; B->inited = 1; B->t_enter = nowms(); B->c1a_b = c1a_battle_no(); B->rb_b = rb_battle_no();
    if (s_ss == SS_QUIT) { snprintf(B->tok, sizeof B->tok, "QUIT"); B->st = NB_DONE; return; }
    if (s_ss == SS_REFUSED || s_ss == SS_SOCKFAIL) { snprintf(B->tok, sizeof B->tok, "REFUSED-%s", s_refcat); B->st = NB_DONE; return; }
    if (s_script) {
        B->st = NB_BARRIER; B->dl = s_lock_fixed ? s_lock_fixed : 4;
        LOG("NETBARRIER enter b=%u c1a_b=%llu rb_b=%llu script=1\n", B->bat, (unsigned long long)B->c1a_b, (unsigned long long)B->rb_b);
        title_dirty();
        return;
    }
    if (s_ss != SS_IDLE) { LOG("NETSEED missing b=%u (no handshake: the battle runs locally)\n", B->bat); B->st = NB_BARRIER; refuse_battle("seed", "SEED", 0, 7); return; }
    if (s_role == 1) { B->seed_seq = s_seed_seq; B->seed = s_seed_k; B->seed_net = s_seed_seq && s_seed_acked == s_seed_seq; }
    else { B->seed_seq = s_rx_seed_seq; B->seed = s_rx_seed; B->seed_net = s_rx_seed_ok; }
    B->st = NB_BARRIER;
    LOG("NETBARRIER enter b=%u c1a_b=%llu rb_b=%llu\n", B->bat, (unsigned long long)B->c1a_b, (unsigned long long)B->rb_b);   /* fix r1: the .rec is c1a_<ts>_b<c1a_b>.rec; RBEND/RBKC b= is rb_b */
    if (!B->seed_net) { LOG("NETSEED missing b=%u (%s)\n", B->bat, s_role == 1 ? "no acked seed" : "no SEED received"); refuse_battle("seed", "SEED", 1, 7); return; }
    LOG("NETSEED use b=%u seq=%u seed=%u\n", B->bat, B->seed_seq, B->seed);
    title_dirty();
}
void ns_on_setup(void) {
    if (!g_on || !B || B->st != NB_BARRIER || B->setup_seen) return;
    B->setup_seen = 1;
    /* C2 fix r1: Setup runs inside the manager Tick at its state 1 -> 2 edge (dec_gb_tick.c FUN_1403fbf30 -> FUN_1403fb660), one or more
     * ticks after Init; the copy words 0x144843f24 / 0x144843f7c are written there (C2-SPEC E4). The barrier work starts at the next tick. */
    LOG("NETBARRIER setup b=%u ticks_after_init=%u ms_after_init=%.0f\n", B->bat, B->pre_setup_ticks, nowms() - B->t_enter);
}
void ns_on_shutdown(void) {
    if (!g_on) return;
    ns_battle_end("shutdown", B->s_last, B->C);
    if (B->bat && B->st == NB_DONE && !B->abort_reason && strncmp(B->tok, "REFUSED", 7)) s_tail_until = nowms() + 3000.0;
    if (s_role == 1 && !s_script && s_ss == SS_IDLE && B->seed_net) {   /* a new seed for the next battle (after the battle: never between the peers' Inits) */
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        s_seed_seq++; s_seed_k = (uint32_t)(q.QuadPart ^ (q.QuadPart >> 29) ^ s_nonce); s_seed_tx = 0;
        LOG("NETSEED tx seq=%u seed=%u (issued after battle %u)\n", s_seed_seq, s_seed_k, B->bat);
    }
}

/* ------------------------------------------------------------------ SETUP */
static void put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t get_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
#define BLOB_HDR (2 + 2 + 4 + 8 + 4 + 8 + 4 + 8 + 1 + 1 + 1 + 1 + 2)
static int dlock_auto(void) {
    double p50, jit; rtt_stats(&p50, &jit);
    int d = (int)ceil((p50 / 2.0 + 2.0 * jit) / T_MS) + 1;
    return d < 4 ? 4 : d > 12 ? 12 : d;
}
static void setup_build(void) {
    uint64_t x = 0, full = 0; uint64_t tr[4];
    c1a_net_setup(&x, &full, tr);
    B->setup_x = x; memcpy(B->tree, tr, sizeof tr);
#ifdef SC6RB_HARNESS
    if (!strcmp(g_cfg.h_c2_su, "setup")) { B->setup_x ^= 1; LOG("NETSETUP harness plant h_c2_su=setup (setup_x flipped: a negative control)\n"); }
    if (!strcmp(g_cfg.h_c2_su, "content")) { B->tree[1] ^= 1; LOG("NETSETUP harness plant h_c2_su=content (tree0 flipped: a negative control)\n"); }
#endif
    B->dl = s_role == 1 ? (s_lock_fixed ? s_lock_fixed : dlock_auto()) : 0;
    uint8_t *b = B->blob; size_t o = 0;
    put_u16(b + o, 1); o += 2; put_u16(b + o, B->bat); o += 2; put_u32(b + o, B->seed_seq); o += 4; put_u64(b + o, B->setup_x); o += 8;
    put_u32(b + o, (uint32_t)B->tree[0]); o += 4; put_u64(b + o, B->tree[1]); o += 8; put_u32(b + o, (uint32_t)B->tree[2]); o += 4; put_u64(b + o, B->tree[3]); o += 8;
    b[o++] = (uint8_t)s_D; b[o++] = (uint8_t)B->dl; b[o++] = (uint8_t)s_maxrb; b[o++] = (uint8_t)s_role;
    uint16_t ns = 0; size_t sp = 0;
    if (s_role == 1) { sp = entry_spans_put(b + o + 2, sizeof B->blob - o - 2, &ns); }
#ifdef SC6RB_HARNESS
    if (s_role == 1 && !strcmp(g_cfg.h_c2_su, "spans") && sp > 40) { b[o + 2 + 8] = 0x40; LOG("NETSETUP harness plant h_c2_su=spans (the first span's length changed: a negative control)\n"); }
#endif
    put_u16(b + o, ns); o += 2 + sp;
    B->blen = o; B->bfnv = np_fnv64(b, o, 0xcbf29ce484222325ULL); B->built = 1; B->blob_tx = 0;
    LOG("NETSETUP tx b=%u setup_x=%016llx tree0=%llu:%016llx tree1=%llu:%016llx D=%d Dl=%d max_rb=%d spans=%u bytes=%zu\n", B->bat, (unsigned long long)B->setup_x,
        (unsigned long long)B->tree[0], (unsigned long long)B->tree[1], (unsigned long long)B->tree[2], (unsigned long long)B->tree[3], s_D, B->dl, s_maxrb, ns, o);
}
static void setup_send(void) {
    uint16_t cnt = (uint16_t)((B->blen + NP_SETUP_FRAG - 1) / NP_SETUP_FRAG);
    for (uint16_t i = 0; i < cnt; i++) {
        size_t off = (size_t)i * NP_SETUP_FRAG, n = B->blen - off; if (n > NP_SETUP_FRAG) n = NP_SETUP_FRAG;
        NpSetupPart sp = { i, cnt, (uint32_t)B->blen, B->bfnv, (uint16_t)n, B->blob + off };
        uint8_t pay[NP_MAXPAY]; size_t k = np_setup_part_put(pay, sizeof pay, &sp);
        send_raw(NP_SETUP_PART, B->bat, pay, k);
    }
}
static void barrier_release(void);
static void setup_compare(void) {
    if (B->matched || B->st != NB_BARRIER || !B->built || !B->pok) return;
    const uint8_t *p = B->pblob;
    if (B->plen < BLOB_HDR) { refuse_battle("setup_blob_short", "SETUP", 1, 1); return; }
    uint16_t pbat = get_u16(p + 2); uint32_t pseq = get_u32(p + 4); uint64_t px = get_u64(p + 8);
    uint64_t pt[4] = { get_u32(p + 16), get_u64(p + 20), get_u32(p + 28), get_u64(p + 32) };
    int pD = p[40], pDl = p[41], pmax = p[42], prole = p[43]; uint16_t pns = get_u16(p + 44);
    B->peer_bat = pbat; B->peer_bat_ok = 1;
    LOG("NETSETUP rx b=%u setup_x=%016llx tree0=%llu:%016llx tree1=%llu:%016llx D=%d Dl=%d max_rb=%d spans=%u bytes=%u role=%d\n", pbat, (unsigned long long)px,
        (unsigned long long)pt[0], (unsigned long long)pt[1], (unsigned long long)pt[2], (unsigned long long)pt[3], pD, pDl, pmax, pns, B->plen, prole);
    if (pseq != B->seed_seq) { LOG("NETSETUP mismatch cat=battle seed_seq local=%u peer=%u\n", B->seed_seq, pseq); refuse_battle("battle", "BATTLE", 1, 0); return; }
    if (px != B->setup_x) { LOG("NETSETUP mismatch cat=setup local=%016llx peer=%016llx\n", (unsigned long long)B->setup_x, (unsigned long long)px); refuse_battle("setup", "SETUP", 1, 1); return; }
    if (memcmp(pt, B->tree, sizeof pt)) { LOG("NETSETUP mismatch cat=content local=%llu:%016llx,%llu:%016llx peer=%llu:%016llx,%llu:%016llx\n", (unsigned long long)B->tree[0],
                                              (unsigned long long)B->tree[1], (unsigned long long)B->tree[2], (unsigned long long)B->tree[3], (unsigned long long)pt[0],
                                              (unsigned long long)pt[1], (unsigned long long)pt[2], (unsigned long long)pt[3]); refuse_battle("content", "CONTENT", 1, 2); return; }
    if (pmax != s_maxrb) { LOG("NETSETUP mismatch cat=params max_rb local=%d peer=%d\n", s_maxrb, pmax); refuse_battle("params", "PARAMS", 1, 3); return; }
    if (s_role == 2) B->dl = pDl;
    B->peer_dl = pDl;
    /* the entry writes (§3.2): the type-word check and the conversion on both; the guest injects the host's spans */
    if (!entry_check()) { refuse_battle("entry", "ENTRY", 1, 5); return; }
    if (s_role == 2) {
        char cat[64];
        if (!entry_spans_apply(p + BLOB_HDR, B->plen - BLOB_HDR, pns, cat, sizeof cat)) { refuse_battle("spans", "SPANS", 1, 4); return; }
    }
    entry_convert();
    uint64_t x2, full; uint64_t tr[4]; c1a_net_setup(&x2, &full, tr);
    B->setup_full = full; B->matched = 1;
    LOG("NETSETUP match b=%u peer_b=%u Dl=%d setup_full=%016llx\n", B->bat, B->peer_bat, B->dl, (unsigned long long)full);
    /* the start barrier's first records: frames 1 .. Dl are a repeat of the zero record (the fill rule at step 1), sent at once */
    NpRec z; memset(&z, 0, sizeof z);
    for (uint32_t f = 1; f <= (uint32_t)B->dl; f++) {
        if (s_lrec) { int ok; NpRec r = rec_from_lr(f, &ok); if (!ok) { set_abort(NPR_REC_END, f, 0, "(NETREC end before the first step)"); return; } lput(f, &r); B->last = r; }
        else { lput(f, &z); B->last = z; }
    }
    B->next_t = (uint32_t)B->dl + 1;
    B->ready_tx = 0;
}
static void ready_check(void) {
    if (B->st != NB_BARRIER || !B->matched) return;
    if (B->peer_ready && B->peer_full != B->setup_full) {
        LOG("NETSETUP mismatch cat=setup_full local=%016llx peer=%016llx (after the entry writes)\n", (unsigned long long)B->setup_full, (unsigned long long)B->peer_full);
        refuse_battle("setup_full", "SETUP", 1, 1);
        return;
    }
    if ((B->peer_ready || B->peer_input) && B->ready_tx_n) barrier_release();
}
static void barrier_release(void) {
    double p50, jit; rtt_stats(&p50, &jit);
    B->st = NB_ON;
    snprintf(B->tok, sizeof B->tok, "ON");
    LOG("NETBARRIER release b=%u wait_ms=%.0f rtt_ms=%.1f jit_ms=%.1f Dl=%d\n", B->bat, nowms() - B->t_enter, p50, jit, B->dl);
    title_dirty();
}
static void script_release(void) {
    if (!entry_check()) { refuse_battle("entry", "ENTRY", 0, 5); return; }
    entry_convert();
    NpRec z; memset(&z, 0, sizeof z);
    for (uint32_t f = 1; f <= (uint32_t)B->dl; f++) lput(f, &z);
    B->last = z; B->next_t = (uint32_t)B->dl + 1; B->matched = 1;
    B->st = NB_ON; snprintf(B->tok, sizeof B->tok, "SCRIPT");
    LOG("NETBARRIER release b=%u wait_ms=%.0f rtt_ms=0.0 jit_ms=0.0 Dl=%d script=1\n", B->bat, nowms() - B->t_enter, B->dl);
    title_dirty();
}

/* ------------------------------------------------------------------ the core interface */
int ns_first_step(void) {
    if (!g_on || !B->inited) return 0;
    if (B->st == NB_BARRIER) {
        LOG("NETBARRIER missed b=%u (step 1 arrived while the barrier held: the battle is refused, no conversion)\n", B->bat);
        refuse_battle("barrier", "BARRIER", 1, 6);
        return 0;
    }
    if (B->st != NB_ON) return 0;
    if (!s_script) {
        uint64_t x, full, tr[4]; c1a_net_setup(&x, &full, tr);
        if (full != B->setup_full) {
            LOG("NETSETUP REFUSE cat=changed_after_barrier local=%016llx exchanged=%016llx\n", (unsigned long long)full, (unsigned long long)B->setup_full);
            refuse_battle("changed_after_barrier", "SETUP", 1, 1);
            return 0;
        }
    }
    return 1;
}
int ns_battle_on(void) { return g_on && B && B->st == NB_ON && !B->abort_reason; }
int ns_want_abort(char *cause, size_t cap) {
    if (!g_on || !B || !B->want_abort) return 0;
    B->want_abort = 0;
    snprintf(cause, cap, "%s", B->abort_cause[0] ? B->abort_cause : "net_abort");
    if (B->st == NB_ON) B->st = NB_DONE;
    return 1;
}
uint32_t ns_frontier(void) { if (!g_on || !B) return 0; if (s_script) return B->s_last + 4096; return B->F; }
uint32_t ns_local_next(void) { return g_on && B ? B->next_t : 0; }
int ns_local(uint32_t f, C1aIn *out) {
    NpRec r; memset(out, 0, sizeof *out); out->ff = 7;
    if (!g_on || !B || !lget(f, &r)) return 0;
    if (s_role == 1) { out->p1 = r.w; memcpy(out->x, r.x, 24); } else out->p2 = r.w;
    return 1;
}
int ns_remote(uint32_t f, C1aIn *out) {
    memset(out, 0, sizeof *out); out->ff = 7;
    if (!g_on || !B) return 0;
    if (B->abort_reason && f > B->abort_frame) return 1;   /* after an abort: the remote side is neutral (word 0) */
    NpRec r; int ok = 0;
    if (s_script) {
        if (!s_nscr) return 0;
        uint32_t span = s_nscr - s_scr_from + 1, i = s_scr_from - 1 + (f - 1) % span;
        const C1aIn *c = &s_scr[i]; r.w = c->p1; memcpy(r.x, c->x, 24); ok = 1;
    } else ok = rget(f, &r);
    if (!ok) return 0;
    if (s_role == 1) out->p2 = r.w; else { out->p1 = r.w; memcpy(out->x, r.x, 24); }
    return 1;
}
/* the owner capture (§2.5): target lag L(s) = Dl if zone_of(s + Dl) else D; fill (lag growing), write, or merge (lag shrinking) */
uint64_t ns_stalls_total(void) { uint64_t t = 0; if (B) for (int i = 0; i < 6; i++) t += B->stall[i]; return t; }
void ns_capture(uint32_t s, const C1aIn *ue) {
    if (!g_on || !B || !B->matched || !(B->st == NB_ON || (B->st == NB_DONE && B->abort_reason))) return;   /* after an abort the local side keeps being captured */
    if ((ue->ff & 7) != 7 && B->netff++ < 8) LOG("NETFF f=%u ff=%02x\n", s, ue->ff);
    uint32_t L = rb_zone_of(s + (uint32_t)B->dl) ? (uint32_t)B->dl : (uint32_t)s_D, tgt = s + L;
    if (s_lrec) {
        while (B->next_t <= tgt) {
            int ok; NpRec r = rec_from_lr(B->next_t, &ok);
            if (!ok) { if (!B->abort_reason) { LOG("NETREC end f=%u\n", B->next_t); set_abort(NPR_REC_END, s, 0, NULL); } return; }
            lput(B->next_t, &r); B->last = r; B->next_t++;
        }
        return;
    }
    NpRec u = rec_from_ue(ue);
    if (B->pend) {   /* merge the pending sample into this one: the later held word, pressed against the previous record, x from the later sample */
        uint32_t held = (uint32_t)u.w, ph = (uint32_t)B->last.w;
        u.w = (uint64_t)held | ((uint64_t)(held & ~ph) << 32);
        B->pend = 0; B->merges++;
    }
    if (B->next_t < tgt) {
        NpRec fill = B->last; fill.w = (uint64_t)(uint32_t)B->last.w;   /* held repeated, pressed 0, x repeated */
        while (B->next_t < tgt) { lput(B->next_t, &fill); B->next_t++; B->fills++; }
    }
    if (B->next_t == tgt) { lput(tgt, &u); B->last = u; B->next_t = tgt + 1; }
    else { B->pend = 1; B->pendr = u; }   /* next_t > s + L: merged into the next step's sample */
}

/* ------------------------------------------------------------------ CHECK, DESYNC, dumps */
static void comp_names(const uint64_t *a, const uint64_t *b, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < C1A_NCOMP; i++) if (a[i] != b[i]) { int k = snprintf(out + o, cap - o, "%s%s", o ? "," : "", k_c1a_comp[i]); if (k > 0 && (size_t)k < cap - o) o += (size_t)k; }
    if (!o) snprintf(out, cap, "none");
}
static const uint64_t *comps_at(uint32_t f) { return B->compf[f % NCR] == f ? B->comps[f % NCR] : NULL; }
static void light_dump(uint32_t f) {
    SYSTEMTIME st; GetLocalTime(&st);
    size_t dl = strlen(g_cfg.log_dir);
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s%snet_%04d%02d%02d_%02d%02d%02d_b%u_f%u.ndump", g_cfg.log_dir, (dl && g_cfg.log_dir[dl - 1] == '\\') ? "" : "\\", st.wYear, st.wMonth, st.wDay,
                               st.wHour, st.wMinute, st.wSecond, B->bat, f);
    FILE *o = fopen(p, "w");
    if (!o) { LOG("NETDUMP light FAIL path=%s\n", p); return; }
    fprintf(o, "NDUMP version=1 sc6rb=%s role=%s b=%u f=%u local_t1=%016llx peer_t1=%016llx\n", SC6RB_VERSION, role_s(), B->bat, f, (unsigned long long)B->ds_lt1, (unsigned long long)B->ds_pt1);
    fprintf(o, "NETENV local mxcsr=%08x x87cw=%04x ucrt=%08x.%08x win_build=%u cpu=%.12s fms=%08x feat=%x\n", s_hl.mxcsr, s_hl.x87cw, s_hl.ucrt_ms, s_hl.ucrt_ls, s_hl.win_build, s_hl.cpu_vendor, s_hl.cpu_fms, s_hl.cpu_feat);
    fprintf(o, "NETENV peer mxcsr=%08x x87cw=%04x ucrt=%08x.%08x win_build=%u cpu=%.12s fms=%08x feat=%x\n", s_ph.mxcsr, s_ph.x87cw, s_ph.ucrt_ms, s_ph.ucrt_ls, s_ph.win_build, s_ph.cpu_vendor, s_ph.cpu_fms, s_ph.cpu_feat);
    for (uint32_t k = f > NCR ? f - NCR + 1 : 1; k <= B->cconf && k <= f + 8; k++) {
        const uint64_t *c = comps_at(k); if (!c) continue;
        fprintf(o, "C %u t1=%016llx", k, (unsigned long long)B->CK[k % NR].t1);
        for (int i = 0; i < C1A_NCOMP; i++) fprintf(o, " %s=%016llx", k_c1a_comp[i], (unsigned long long)c[i]);
        Chk *pc = &B->PK[k % NR]; if (pc->have && pc->f == k) fprintf(o, " peer_t1=%016llx t15=%016llx peer_t15=%016llx ued=%08x peer_ued=%08x", (unsigned long long)pc->t1,
                                                                        (unsigned long long)B->CK[k % NR].t15, (unsigned long long)pc->t15, B->CK[k % NR].ued, pc->ued);
        fprintf(o, "\n");
    }
    if (B->peer_comps_ok) { fprintf(o, "PEERCOMPS f=%u", B->ds_f); for (int i = 0; i < C1A_NCOMP; i++) fprintf(o, " %s=%016llx", k_c1a_comp[i], (unsigned long long)B->peer_comps[i]); fprintf(o, "\n"); }
    for (uint32_t k = f > 120 ? f - 120 : 1; k <= f + 8; k++) {
        NpRec l, r; int hl = lget(k, &l), hr = rget(k, &r);
        if (!hl && !hr) continue;
        fprintf(o, "R %u local=%016llx peer=%016llx%s%s\n", k, hl ? (unsigned long long)l.w : 0ULL, hr ? (unsigned long long)r.w : 0ULL, hl ? "" : " local=missing", hr ? "" : " peer=missing");
    }
    for (int i = 0; i < B->nzl; i++) fprintf(o, "Z local %u %u\n", B->zl[i][0], B->zl[i][1]);
    for (int i = 0; i < B->npzl; i++) fprintf(o, "Z peer %u %u\n", B->pzl[i][0], B->pzl[i][1]);
    fclose(o);
    const char *nm = strrchr(p, '\\');
    LOG("NETDUMP light file=%s f=%u\n", nm ? nm + 1 : p, f);
}
static void dump_schedule(int kind, uint32_t f, uint32_t g) {
    if (B->dump_kind) return;
    uint32_t s = B->s_last;
    int late = 0;
    if (g <= s) { g = s + 1; late = 1; }   /* already past g: the next confirmed pass */
    B->dump_kind = kind; B->dump_f = f; B->dump_g = g; B->dump_late = late; B->dump_done = 0;
    c1a_dump_frame_add(g);
    LOG("NETDUMP schedule kind=%d f=%u g=%u late=%d\n", kind, f, g, late);
}
static void desync_detect(uint32_t f, uint64_t lt1, uint64_t pt1, int first_local, const uint64_t *pcomps) {
    if (B->desync) return;
    B->desync = 1; B->ds_f = f; B->ds_lt1 = lt1; B->ds_pt1 = pt1; B->ds_first_local = first_local;
    if (pcomps) { memcpy(B->peer_comps, pcomps, sizeof B->peer_comps); B->peer_comps_ok = 1; }
    char cs[400] = "unavailable"; const uint64_t *lc = comps_at(f);
    if (lc && B->peer_comps_ok) comp_names(lc, B->peer_comps, cs, sizeof cs);
    int md = (lc && B->peer_comps_ok && lc[5] != B->peer_comps[5]) || (B->ued_last && f >= B->ued_last && f - B->ued_last <= 64);   /* §2.8: md differs, or a UEDIFF within 64 frames */
    LOG("DESYNC f=%u local=%016llx peer=%016llx comps=%s first_by=%s%s\n", f, (unsigned long long)lt1, (unsigned long long)pt1, cs, first_local ? "local" : "peer", md ? " hint=ueedge" : "");
    snprintf(B->tok, sizeof B->tok, "DESYNC");
    light_dump(f);
    title_dirty();
    if (s_role == 1) {
        uint32_t g = (B->s_last > s_peer_s ? B->s_last : s_peer_s) + 60;
        dump_schedule(1, f, g);
        B->dump_reqd = 1; B->dumpreq_tx = 0;
    } else B->ds_note_tx = 0;   /* the guest: DESYNC_NOTE until the host's DUMP_REQ */
    log_flush();
}
static void chk_compare(uint32_t f) {
    Chk *l = &B->CK[f % NR], *p = &B->PK[f % NR];
    if (!l->have || !p->have || l->f != f || p->f != f || l->cmp) return;
    l->cmp = 1; B->checked++;
    if (l->t1 != p->t1) { B->t1_diff++; desync_detect(f, l->t1, p->t1, 1, NULL); return; }
    if (l->t15 != p->t15) {
        B->t15_diff++;
        if (B->kd_lines++ < 8) LOG("KHITDIFF f=%u local=%016llx peer=%016llx\n", f, (unsigned long long)l->t15, (unsigned long long)p->t15);
        if (s_role == 1 && B->khit_dumps < 2 && !B->dump_kind && !B->desync) { B->khit_dumps++; dump_schedule(2, f, B->s_last + 60); B->dump_reqd = 1; B->dumpreq_tx = 0; B->dump_acked = 0; }   /* the host initiates (one dump pair per request) */
        title_dirty();
    }
    if (l->ued != p->ued) {
        B->ued_diff++; B->ued_last = f;
        if (B->ud_lines++ < 8) LOG("UEDIFF f=%u local=%08x peer=%08x\n", f, l->ued, p->ued);
        title_dirty();
    }
}
void ns_confirmed(uint32_t f, uint64_t t1, uint64_t t15, uint32_t ued, const uint64_t comps[C1A_NCOMP]) {
    if (!g_on || !B || !B->st) return;
    Chk *c = &B->CK[f % NR]; c->t1 = t1; c->t15 = t15; c->ued = ued; c->f = f; c->have = 1; c->cmp = 0;
    memcpy(B->comps[f % NCR], comps, sizeof(uint64_t) * C1A_NCOMP); B->compf[f % NCR] = f;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_c2_check_xor_at && f == (uint32_t)g_cfg.h_c2_check_xor_at) { c->t1 ^= 1; LOG("NETCHECK harness plant h_c2_check_xor_at=%u (this peer's sent t1 flipped)\n", f); }
#endif
    if (f == B->cconf + 1) B->cconf = f;
    chk_compare(f);
    if (B->dump_kind && !B->dump_done && f == B->dump_g) {
        B->dump_done = 1; B->dump_t = nowms();
        LOG("DESYNCDUMP kind=%d f=%u g=%u light=%d full=1 late=%d\n", B->dump_kind, B->dump_f, B->dump_g, B->dump_kind == 1, B->dump_late);
        NpDumpAck a = { (uint8_t)B->dump_kind, B->dump_g, 1 }; uint8_t b[8]; size_t n = np_dumpack_put(b, &a);
        rs_add(NP_DUMP_ACK, B->bat, b, n, 3, 50.0);
        if (B->dump_kind == 2) { B->dump_kind = 0; B->dump_reqd = 0; }   /* KHITDIFF: a finding, netplay goes on */
        log_flush();
    }
}
void ns_ctltype(uint32_t f, int side, uint32_t t324, uint32_t ctl30) {
    (void)side; (void)t324; (void)ctl30;
    if (!g_on || !B || B->st != NB_ON || B->ctltype) return;
    B->ctltype = 1;
    LOG("NETCTLTYPE f=%u (the control-type assert fired: a dump, then ABORT ctltype; DUMP_REQ kind=3)\n", f);
    if (!B->dump_kind) dump_schedule(1, f, B->s_last + 60);
    B->dump_reqd = 1; B->dumpreq_tx = 0; B->dump_acked = 0;
}

/* ------------------------------------------------------------------ zones, barriers, core aborts */
void ns_zone_event(int idx, uint32_t z, uint32_t e) {
    if (!g_on || !B || B->st != NB_ON || s_script) return;
    if (idx < 64) { if (idx >= B->nzl) B->nzl = idx + 1; B->zl[idx][0] = z; B->zl[idx][1] = e; }
    NpZone zz = { (uint16_t)idx, z, e }; uint8_t b[16]; size_t n = np_zone_put(b, &zz);
    rs_add(NP_ZONE, B->bat, b, n, 3, 50.0);
    if (idx < B->npzl && (B->pzl[idx][0] != z || (B->pzl[idx][1] != e && B->pzl[idx][1] != 0xffffffffu && e != 0xffffffffu)) && B->zdiff_lines++ < 8)
        LOG("ZONEDIFF idx=%d local=%u,%u peer=%u,%u\n", idx, z, e, B->pzl[idx][0], B->pzl[idx][1]);
}
void ns_fight_barrier(uint32_t f, long long unconfirmed) { if (g_on && B && B->st == NB_ON) LOG("NETBARRIER fight f=%u unconfirmed=%lld\n", f, unconfirmed); }
void ns_mispredict_behind_barrier(uint32_t f) { if (g_on && B && B->st == NB_ON) set_abort(NPR_BARRIER_MISPREDICT, f, 0, NULL); }
void ns_core_abort(const char *cause) {
    if (!g_on || !B || B->abort_reason || B->st != NB_ON) return;
    int r = !strcmp(cause, "depth") ? NPR_DEPTH : NPR_INTERNAL;
    B->abort_reason = r; B->abort_frame = B->s_last; s_last_abort = r; snprintf(B->tok, sizeof B->tok, "%s", r == NPR_DEPTH ? "ABORT-DEPTH" : "ABORT-INTERNAL");
    LOG("NETABORT reason=%s f=%u by=local (the core aborted: cause=%s)\n", k_np_reason[r], B->s_last, cause);
    NpEnd e = { (uint8_t)r, B->s_last }; uint8_t b[8]; size_t n = np_end_put(b, &e); rs_add(NP_ABORT, B->bat, b, n, 5, 20.0);
    B->st = NB_DONE; title_dirty();
}
void ns_step_done(uint32_t s, uint32_t C, int depth) {
    if (!g_on || !B) return;
    double t = nowms();
    if (B->last_step_s && s == B->last_step_s + 1 && rb_zone_of(s)) { B->zone_ms += t - B->last_step_ms; B->zone_n++; }
    B->last_step_ms = t; B->last_step_s = s;
    B->s_last = s; B->C = C;
    if (depth >= 0) { if (depth < 16) B->depth_hist[depth]++; if (depth > B->depth_max) B->depth_max = depth; }
    if (B->st == NB_ON && s % 600 == 0) {
        double p50, jit; rtt_stats(&p50, &jit); const NetCounters *n = net_counters();
        LOG("NETSTAT b=%u f=%u rtt_ms=%.1f jit_ms=%.1f adv=%.2f peer_adv=%.2f F=%u depth_max=%d rx=%lld tx=%lld dup=%llu old=%llu foreign=%lld crcbad=%lld badlen=%lld\n", B->bat, s, p50, jit,
            adv_avg(), s_peer_adv, B->F, B->depth_max, n->rx, n->tx, (unsigned long long)B->rx_dup, (unsigned long long)B->rx_old, n->rx_foreign, (long long)g_crcbad, (long long)g_badlen);
    }
}
void ns_battle_end(const char *why, uint32_t s, uint32_t C) {
    if (!g_on || !B || !B->bat || B->end_logged) return;
    B->end_logged = 1;
    char sc[160]; size_t o = 0; sc[0] = 0;
    for (int i = 0; i < 6; i++) o += (size_t)snprintf(sc + o, sizeof sc - o, "%s%s:%llu", i ? "," : "", k_sc[i], (unsigned long long)B->stall[i]);
    char dh[160]; o = 0; dh[0] = 0;
    for (int d = 0; d < 16; d++) if (B->depth_hist[d]) o += (size_t)snprintf(dh + o, sizeof dh - o, "%s%d:%u", o ? "," : "", d, B->depth_hist[d]);
    char bd[200]; rb_p99_by_depth(bd, sizeof bd);
    LOG("NETEND b=%u c1a_b=%llu rb_b=%llu why=%s frames=%u confirmed=%u checked=%u t1_diff=%u t15_diff=%u ued_diff=%u stalls_by_cause=%s depth_hist=%s rtt_p50=%.1f rtt_p99=%.1f zone_frame_ms=%.2f "
        "p99_core_us=%.0f p99_core_us_by_depth=%s abort=%s fills=%llu merges=%llu conflicts=%llu rs_full=%u peer_left=%d\n", B->bat, (unsigned long long)B->c1a_b, (unsigned long long)B->rb_b, why, s, C, B->checked, B->t1_diff, B->t15_diff, B->ued_diff, sc, o ? dh : "-",
        rtt_all_pct(0.5), rtt_all_pct(0.99), B->zone_n ? B->zone_ms / B->zone_n : 0.0, rb_p99_core(), bd, B->abort_reason ? k_np_reason[B->abort_reason] : "0",
        (unsigned long long)B->fills, (unsigned long long)B->merges, (unsigned long long)B->conflicts, s_rs_full, B->peer_left);
    if (B->st != NB_DONE) { B->st = NB_DONE; if (!strcmp(B->tok, "ON") || !strcmp(B->tok, "SCRIPT")) snprintf(B->tok, sizeof B->tok, "IDLE"); }
    title_dirty();
    log_flush();
}

/* ------------------------------------------------------------------ receive */
static void hs_refuse(const char *cat, const char *CAT, uint64_t l, uint64_t p) {
    LOG("NETHS REFUSE cat=%s local=%016llx peer=%016llx (netplay disarmed for this process; the game runs vanilla-local; restart the game to retry; title net=REFUSED-%s)\n", cat,
        (unsigned long long)l, (unsigned long long)p, CAT);
    s_ss = SS_REFUSED; snprintf(s_refcat, sizeof s_refcat, "%s", CAT);
    if (B && (B->st == NB_BARRIER || B->st == NB_ON)) refuse_battle("handshake", CAT, 0, 1);
    title_dirty();
    log_flush();
}
static void env_compare(void) {
    char d[160]; size_t o = 0; d[0] = 0;
#define EDIFF(f, n) if (s_hl.f != s_ph.f) o += (size_t)snprintf(d + o, sizeof d - o, "%s%s", o ? "," : "", n)
    EDIFF(mxcsr, "mxcsr"); EDIFF(x87cw, "x87cw"); EDIFF(ucrt_ms, "ucrt"); if (s_hl.ucrt_ms == s_ph.ucrt_ms) EDIFF(ucrt_ls, "ucrt"); EDIFF(win_build, "win_build");
    if (memcmp(s_hl.cpu_vendor, s_ph.cpu_vendor, 12)) o += (size_t)snprintf(d + o, sizeof d - o, "%scpu_vendor", o ? "," : "");
    EDIFF(cpu_fms, "cpu_fms"); EDIFF(cpu_feat, "cpu_feat");
#undef EDIFF
    snprintf(s_envdiff, sizeof s_envdiff, "%s", o ? d : "none");
    LOG("NETENV peer mxcsr=%08x x87cw=%04x ucrt=%u.%u.%u.%u win_build=%u cpu=%.12s fms=%08x feat=%x\n", s_ph.mxcsr, s_ph.x87cw, s_ph.ucrt_ms >> 16, s_ph.ucrt_ms & 0xffff,
        s_ph.ucrt_ls >> 16, s_ph.ucrt_ls & 0xffff, s_ph.win_build, s_ph.cpu_vendor, s_ph.cpu_fms, s_ph.cpu_feat);
    LOG("NETENV diff=%s\n", s_envdiff);
}
static void on_hello(const NpHdr *h, const uint8_t *p, size_t n, int is_ack) {
    NpHello ph;
    if (!np_hello_get(p, n, &ph)) return;
    if (!is_ack && s_hl_ready) { uint8_t b[NP_HELLO_LEN]; np_hello_put(b, &s_hl); send_raw(NP_HELLO_ACK, 0, b, NP_HELLO_LEN); }
    if (s_ph_ok && h->nonce_tx != s_peer_nonce) {   /* the peer restarted */
        LOG("NETHS peer_restart old_nonce=%08x new_nonce=%08x\n", s_peer_nonce, h->nonce_tx);
        if (B && (B->st == NB_ON || B->st == NB_BARRIER)) set_abort(NPR_PEER_RESTART, B->s_last, 0, NULL);
        s_ph_ok = 0; if (s_ss == SS_IDLE) s_ss = SS_HELLO;
        s_seed_acked = 0; s_seed_tx = 0; s_rx_seed_ok = 0;   /* the new peer instance has no seed: send it again (host); wait for a new one (guest) */
    }
    if (s_ph_ok || s_ss == SS_REFUSED || s_ss == SS_QUIT || !s_hl_ready) return;
    s_ph = ph;
    if (strncmp(ph.version, s_hl.version, sizeof ph.version)) { hs_refuse("version", "VERSION", np_fnv64(s_hl.version, strlen(s_hl.version), 0), np_fnv64(ph.version, strnlen(ph.version, 24), 0)); return; }
    if (ph.plugin_sha != s_hl.plugin_sha) { hs_refuse("plugin", "PLUGIN", s_hl.plugin_sha, ph.plugin_sha); return; }
    if (ph.exe_ts != s_hl.exe_ts || ph.exe_soi != s_hl.exe_soi) { hs_refuse("exe", "EXE", ((uint64_t)s_hl.exe_ts << 32) | s_hl.exe_soi, ((uint64_t)ph.exe_ts << 32) | ph.exe_soi); return; }
    if (ph.simcfg_net != s_hl.simcfg_net) { hs_refuse("simcfg", "SIMCFG", s_hl.simcfg_net, ph.simcfg_net); return; }
    if (ph.modset2 != s_hl.modset2) { hs_refuse("modset", "MODSET", s_hl.modset2, ph.modset2); return; }
    if (ph.plugins_list != s_hl.plugins_list) { hs_refuse("plugins", "PLUGINS", s_hl.plugins_list, ph.plugins_list); return; }
    if (ph.role == s_hl.role || (ph.role != 1 && ph.role != 2)) { hs_refuse("role", "ROLE", s_hl.role, ph.role); return; }
    s_ph_ok = 1; s_peer_nonce = h->nonce_tx;
    double p50, jit; rtt_stats(&p50, &jit);
    LOG("NETHS ok peer_role=%s peer_nonce=%08x rtt_ms=%.1f\n", ph.role == 1 ? "host" : "guest", h->nonce_tx, p50);
    env_compare();
    s_ss = SS_IDLE;
    if (s_role == 1 && !s_seed_seq) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        s_seed_seq = 1; s_seed_k = (uint32_t)(q.QuadPart ^ (q.QuadPart >> 29) ^ s_nonce); s_seed_tx = 0;
#ifdef SC6RB_HARNESS
        if (g_cfg.h_c2_seed) { s_seed_k = g_cfg.h_c2_seed; LOG("NETSEED harness h_c2_seed=%u (the host's seed is a recording's)\n", s_seed_k); }
#endif
        LOG("NETSEED tx seq=%u seed=%u\n", s_seed_seq, s_seed_k);
    }
    title_dirty();
    log_flush();
}
static void on_input(const NpHdr *h, const NpInput *in, uint64_t qrx) {
    /* RTT and time sync (any battle) */
    if (in->echo_id && in->echo_id <= s_ping_id && s_ping_id - in->echo_id < PINGR) {
        uint64_t t0 = s_ping_qpc[in->echo_id % PINGR];
        if (t0) { double rtt = qpc_us(qrx - t0) / 1000.0 - in->echo_hold_us / 1000.0; rtt_add(rtt); s_ping_qpc[in->echo_id % PINGR] = 0; }
    }
    if (in->ping_id) { s_echo_id = in->ping_id; s_echo_qpc = qrx; }
    s_peer_paused = (in->iflags & NP_IF_PAUSED) != 0;
    if (B && B->bat && B->peer_bat_ok && h->battle == B->peer_bat && !B->peer_seq0_ok) { B->peer_seq0 = h->seq; B->peer_seq0_ok = 1; }
    if (B && B->st == NB_ON && !B->abort_reason && B->peer_seq0_ok && !s_script && (int32_t)(h->seq - B->peer_seq0) > 0) {
        /* fix r1: the peer left this battle (its Shutdown and 3 s tail are over, or it is in a newer battle) while this PC is armed: frames
         * past F will never come. Its battle-0 heartbeats keep arriving, so NORESP / peer_timeout never fire: the lever disarms (ABORT
         * peer_left) at the first step that needs a frame past F (allowed_steps); a PC that never steps again (results) is not aborted */
        int off = 0;
#ifdef SC6RB_HARNESS
        off = g_cfg.h_c2_peerleft_off;   /* NENDX / NDISARMX (harness-only control): the pre-fix behavior, neither the battle id nor DISARM is read */
#endif
        if (h->battle != B->peer_bat && !B->peer_left) {
            B->peer_left = off ? -1 : 1;
            LOG("NETPEERLEFT b=%u peer_b=%u peer_battle=%u iflags=%x f=%u F=%u peer_s=%u%s\n", B->bat, B->peer_bat, h->battle, in->iflags, B->s_last, B->F, in->s,
                off ? " (ignored: harness control h_c2_peerleft_off=1)" : " (the peer left this battle: a step that needs a frame past F disarms here)");
            log_flush();
        }
        /* the peer disarmed this battle (DISARM in its INPUT) but no ABORT / QUIT arrived: its 5 copies were lost. 1 s grace (timers) */
        if (h->battle == B->peer_bat && (in->iflags & NP_IF_DISARM) && !B->peer_disarm_t) {
            if (!off) B->peer_disarm_t = nowms();
            else { B->peer_disarm_t = -1; LOG("NETPEERLEFT disarm ignored b=%u (harness control h_c2_peerleft_off=1)\n", B->bat); }
        }
    }
    if (!B || !B->bat || !B->peer_bat_ok || h->battle != B->peer_bat) return;
    if (B->st == NB_BARRIER && B->matched && !(in->iflags & NP_IF_BARRIER)) B->peer_input = 1;
    s_peer_s = in->s; s_peer_adv = in->adv16 / 16.0;
    {   double rtt_f = 0, p50, jit; rtt_stats(&p50, &jit); rtt_f = ceil(p50 / 2.0 / T_MS);
        double a = (double)B->s_last - ((double)in->s + rtt_f);
        s_adv[s_iadv] = a; s_iadv = (s_iadv + 1) % 60; if (s_nadv < 60) s_nadv++; }
    if (in->ack > B->peer_ack) B->peer_ack = in->ack;
    if (in->cack > B->peer_cack) B->peer_cack = in->cack;
    if (B->st != NB_ON && B->st != NB_BARRIER && !(B->st == NB_DONE && B->abort_reason)) return;
    for (int i = 0; i < in->count; i++) {
        uint32_t f = in->first + (uint32_t)i;
        if (!f) continue;
        NpRec r = in->rec[i];
        if (in->rec_size == NP_REC_WORD) memset(r.x, 0, 24);
        if (B->Rf[f % NR] == f) {
            if (memcmp(&B->R[f % NR], &r, sizeof r)) {
                B->conflicts++;
                LOG("NETRXCONFLICT f=%u old=%016llx new=%016llx (a record changed after it was received: a protocol bug)\n", f, (unsigned long long)B->R[f % NR].w, (unsigned long long)r.w);
                set_abort(NPR_INTERNAL, B->s_last, 0, "(NETRXCONFLICT)");
            } else B->rx_dup++;
            continue;
        }
        if (f <= B->F) { B->rx_old++; continue; }
        if (f > B->F + NR - 8) continue;   /* outside the ring (a bug or an overflow): dropped */
        B->R[f % NR] = r; B->Rf[f % NR] = f;
    }
    while (B->Rf[(B->F + 1) % NR] == B->F + 1) B->F++;
    for (int i = 0; i < in->ccount; i++) {
        uint32_t f = in->cfirst + (uint32_t)i;
        if (!f) continue;
        Chk *p = &B->PK[f % NR];
        if (p->have && p->f == f) continue;
        p->t1 = in->chk[i].t1; p->t15 = in->chk[i].t15; p->ued = in->chk[i].ued; p->f = f; p->have = 1; p->cmp = 0;
        chk_compare(f);
    }
    while (B->PK[(B->pc_contig + 1) % NR].have && B->PK[(B->pc_contig + 1) % NR].f == B->pc_contig + 1) B->pc_contig++;
}
static void on_packet(const uint8_t *dg, size_t n, uint64_t qrx) {
    NpHdr h; const uint8_t *p = NULL;
    int r = np_decode(dg, n, &h, &p);
    if (r == NP_EMAGIC || r == NP_ELEN) { g_badlen++; return; }
    if (r == NP_ECRC) { g_crcbad++; return; }
    if (r == NP_EPROTO) { if (s_ss == SS_HELLO) hs_refuse("proto", "PROTO", NP_PROTO, h.proto); return; }
    s_last_rx = nowms();
    if (s_noresp) { s_noresp = 0; LOG("NETNORESP end (packets again)\n"); title_dirty(); }
    switch (h.type) {
    case NP_HELLO: on_hello(&h, p, h.len, 0); return;
    case NP_HELLO_ACK: on_hello(&h, p, h.len, 1); return;
    default: break;
    }
    if (!s_ph_ok || h.nonce_tx != s_peer_nonce) return;   /* nothing but HELLO before the handshake (or from a stale peer instance) */
    switch (h.type) {
    case NP_PING: { NpPing pg; if (np_ping_get(p, h.len, &pg)) { pg.hold_us = (uint32_t)qpc_us(qnow() - qrx); uint8_t b[16]; size_t k = np_ping_put(b, &pg); send_raw(NP_PONG, 0, b, k); } return; }
    case NP_PONG: return;
    case NP_SEED: {
        NpSeed sd; if (!np_seed_get(p, h.len, &sd) || s_role != 2) return;
        if (!s_rx_seed_ok || sd.seq != s_rx_seed_seq) LOG("NETSEED rx seq=%u seed=%u\n", sd.seq, sd.seed);
        s_rx_seed_seq = sd.seq; s_rx_seed = sd.seed; s_rx_seed_ok = 1;
        uint8_t b[8]; NpSeed a = { sd.seq, 0 }; size_t k = np_seed_put(b, &a); send_raw(NP_SEED_ACK, 0, b, 4); (void)k;
        return;
    }
    case NP_SEED_ACK: { NpSeed sd; if (np_seed_get(p, h.len, &sd) && sd.seq == s_seed_seq && s_seed_acked != sd.seq) { s_seed_acked = sd.seq; LOG("NETSEED ack seq=%u\n", sd.seq); } return; }
    case NP_SETUP_PART: {
        NpSetupPart sp; if (!np_setup_part_get(p, h.len, &sp) || !B || !B->bat || B->st != NB_BARRIER) return;
        if (sp.total_len > sizeof B->pblob || sp.count > 32) return;
        if (B->pok) { if (sp.total_fnv == B->pfnv) { uint8_t b[8]; put_u64(b, B->pfnv); send_raw(NP_SETUP_ACK, B->bat, b, 8); } return; }
        if (B->pcount && (sp.count != B->pcount || sp.total_fnv != B->pfnv)) { B->pgot = 0; }
        B->pcount = sp.count; B->plen = sp.total_len; B->pfnv = sp.total_fnv;
        size_t off = (size_t)sp.idx * NP_SETUP_FRAG; if (off + sp.dlen > sizeof B->pblob) return;
        memcpy(B->pblob + off, sp.data, sp.dlen); B->pgot |= 1u << sp.idx;
        if (B->pgot == (sp.count >= 32 ? 0xffffffffu : (1u << sp.count) - 1u)) {
            if (np_fnv64(B->pblob, B->plen, 0xcbf29ce484222325ULL) != B->pfnv) { B->pgot = 0; return; }
            B->pok = 1; uint8_t b[8]; put_u64(b, B->pfnv); send_raw(NP_SETUP_ACK, B->bat, b, 8);
            setup_compare();
        }
        return;
    }
    case NP_SETUP_ACK: if (B && B->built && h.len >= 8 && get_u64(p) == B->bfnv) B->blob_acked = 1; return;
    case NP_READY: if (B && B->st == NB_BARRIER && h.len >= 8 && B->peer_bat_ok && h.battle == B->peer_bat) { B->peer_ready = 1; B->peer_full = get_u64(p); ready_check(); } return;
    case NP_INPUT: { NpInput in; if (np_input_get(p, h.len, &in)) { on_input(&h, &in, qrx); if (B && B->st == NB_BARRIER) ready_check(); } return; }
    case NP_ZONE: {
        NpZone z; if (!np_zone_get(p, h.len, &z) || !B || !B->peer_bat_ok || h.battle != B->peer_bat || z.idx >= 64) return;
        if (z.idx >= B->npzl) B->npzl = z.idx + 1;
        B->pzl[z.idx][0] = z.z; B->pzl[z.idx][1] = z.e;
        if (z.idx < B->nzl && (B->zl[z.idx][0] != z.z || (B->zl[z.idx][1] != z.e && B->zl[z.idx][1] != 0xffffffffu && z.e != 0xffffffffu)) && B->zdiff_lines++ < 8)
            LOG("ZONEDIFF idx=%u local=%u,%u peer=%u,%u\n", z.idx, B->zl[z.idx][0], B->zl[z.idx][1], z.z, z.e);
        return;
    }
    case NP_PAUSE: { NpPause pz; if (np_pause_get(p, h.len, &pz) && !s_peer_paused) { s_peer_paused = 1; LOG("NETPAUSE peer f=%u\n", pz.frame); title_dirty(); } return; }
    case NP_RESUME: { NpPause pz; if (np_pause_get(p, h.len, &pz)) { if (s_peer_paused) LOG("NETRESUME peer f=%u paused_ms=%u\n", pz.frame, pz.paused_ms); s_peer_paused = 0; title_dirty(); } return; }
    case NP_DESYNC_NOTE: {
        NpDesyncNote d; if (!np_desync_get(p, h.len, &d) || !B || !B->peer_bat_ok || h.battle != B->peer_bat || B->st == NB_NONE) return;
        if (!B->desync) desync_detect(d.f, d.peer_t1, d.local_t1, 0, d.comps_ok ? d.comps : NULL);
        else if (d.comps_ok && !B->peer_comps_ok) { memcpy(B->peer_comps, d.comps, sizeof B->peer_comps); B->peer_comps_ok = 1;
            const uint64_t *lc = comps_at(d.f); char cs[400]; if (lc) { comp_names(lc, B->peer_comps, cs, sizeof cs); LOG("DESYNC comps f=%u comps=%s (the peer's tier-1 components at f)\n", d.f, cs); } }
        if (s_role == 1 && !B->dump_reqd) { uint32_t g = (B->s_last > s_peer_s ? B->s_last : s_peer_s) + 60; dump_schedule(1, d.f, g); B->dump_reqd = 1; B->dumpreq_tx = 0; }
        return;
    }
    case NP_DUMP_REQ: {
        NpDumpReq d; if (!np_dumpreq_get(p, h.len, &d) || !B || !B->peer_bat_ok || h.battle != B->peer_bat || B->st == NB_NONE) return;
        if (d.kind == 3) { if (!B->ctltype) LOG("NETCTLTYPE peer f=%u (the peer's control-type assert fired)\n", d.f); B->ctltype = 1; d.kind = 1; }
        if (d.kind == 2) {   /* KHITDIFF: at most 2 dumps per battle; a request already served is only re-acked */
            if (B->dump_kind == 0 && B->khit_last_g == d.g) { NpDumpAck a = { 2, d.g, 1 }; uint8_t b2[8]; size_t k2 = np_dumpack_put(b2, &a); send_raw(NP_DUMP_ACK, B->bat, b2, k2); return; }
            if (B->khit_dumps >= 2 || B->dump_kind) return;
            B->khit_dumps++; B->khit_last_g = d.g;
        }
        if (d.kind == 1 && !B->desync && !B->ctltype) desync_detect(d.f, B->CK[d.f % NR].f == d.f ? B->CK[d.f % NR].t1 : 0, 0, 0, d.comps_ok ? d.comps : NULL);
        if (d.comps_ok && !B->peer_comps_ok) {
            memcpy(B->peer_comps, d.comps, sizeof B->peer_comps); B->peer_comps_ok = 1;
            const uint64_t *lc = comps_at(d.f); char cs[400];
            if (lc && d.kind == 1) { comp_names(lc, B->peer_comps, cs, sizeof cs); LOG("DESYNC comps f=%u comps=%s (the peer's tier-1 components at f)\n", d.f, cs); }
        }
        if (!B->dump_kind || B->dump_g != d.g) {
            if (B->dump_kind && B->dump_g != d.g && s_role == 2) { B->dump_kind = 0; }   /* the host's g wins */
            dump_schedule(d.kind, d.f, d.g);
        }
        B->ds_note_tx = -1;   /* the guest stops its DESYNC_NOTE */
        if (B->dump_done) { NpDumpAck a = { (uint8_t)B->dump_kind, B->dump_g, 1 }; uint8_t b[8]; size_t k = np_dumpack_put(b, &a); send_raw(NP_DUMP_ACK, B->bat, b, k); }
        return;
    }
    case NP_DUMP_ACK: { NpDumpAck a; if (np_dumpack_get(p, h.len, &a) && B && B->dump_reqd && a.g == B->dump_g) { B->dump_acked = 1; } return; }
    case NP_QUIT: {
        NpEnd e; if (!np_end_get(p, h.len, &e)) return;
        if (s_ss != SS_QUIT) {
            LOG("NETQUIT by=peer f=%u\n", e.frame);
            s_ss = SS_QUIT;
            if (B && (B->st == NB_ON || B->st == NB_BARRIER) && B->abort_reason != NPR_QUIT_F3) { B->abort_reason = 0; set_abort(NPR_QUIT_F3, B->s_last, 1, NULL); }   /* fix r1: the peer's ABORT(quit_f3) may arrive first (netem reorders): one NETABORT */
            title_dirty(); log_flush();
        }
        return;
    }
    case NP_ABORT: {
        NpEnd e; if (!np_end_get(p, h.len, &e) || !B || !B->peer_bat_ok || h.battle != B->peer_bat) return;
        if (e.reason == NPR_REFUSED && (e.frame & 0xff000000u) == 0xff000000u && B->st == NB_BARRIER) {
            uint32_t ci = e.frame & 0xff; const char *ct = ci < sizeof k_refcat / sizeof k_refcat[0] ? k_refcat[ci] : "SETUP";
            LOG("NETSETUP peer_refused cat=%s\n", ct);
            refuse_battle("peer", ct, 0, (int)ci);
            return;
        }
        if (B->st == NB_ON || B->st == NB_BARRIER) set_abort(e.reason < NPR_N ? e.reason : NPR_INTERNAL, e.frame, 1, NULL);
        return;
    }
    default: return;
    }
}

/* ------------------------------------------------------------------ per tick */
static void timers(double t) {
    /* NORESP and the timeouts */
    double since = t - s_last_rx;
    int paused = s_peer_paused || s_local_paused;
    double lim = (paused ? s_pausecap_s : s_timeout_s) * 1000.0;
    {   /* §2.9: a pause (either side) longer than pause_cap_s -> ABORT pause_cap */
        static double pt0; static int was;
        if (paused && !was) pt0 = t;
        was = paused;
        if (paused && t - pt0 > s_pausecap_s * 1000.0 && B && B->st == NB_ON && !B->abort_reason) { LOG("NETPAUSE cap paused_ms=%.0f cap_s=%d\n", t - pt0, s_pausecap_s); set_abort(NPR_PAUSE_CAP, B->s_last, 0, NULL); }
    }
    if (s_ss == SS_IDLE || s_ss == SS_HELLO) {
        if (since > HB_MS && !s_noresp && s_ph_ok) { s_noresp = 1; LOG("NETNORESP since_ms=%.0f\n", since); title_dirty(); log_flush(); }
        {   /* while stalled the step-paced flush does not run: flush once a second so a killed or stuck game keeps its log */
            static double lf; if (s_noresp && t - lf > 1000.0) { lf = t; log_flush(); } }
        if (since > lim && s_ph_ok) {
            if (B && (B->st == NB_ON || B->st == NB_BARRIER)) {
                if (paused) set_abort(NPR_PAUSE_CAP, B->s_last, 0, NULL);
                else set_abort(NPR_PEER_TIMEOUT, B->s_last, 0, NULL);
            }
            LOG("NETHS lost since_ms=%.0f (the session goes back to HELLO)\n", since);
            s_ph_ok = 0; s_ss = SS_HELLO; s_noresp = 1; title_dirty();
        }
    }
    if (B && B->st == NB_ON && !B->abort_reason && B->peer_disarm_t > 0 && t - B->peer_disarm_t >= 1000.0) {   /* fix r1 */
        LOG("NETPEERLEFT disarm b=%u f=%u (the peer's INPUT carries DISARM for 1 s and no ABORT / QUIT arrived)\n", B->bat, B->s_last);
        B->peer_left = 1;
        set_abort(NPR_PEER_LEFT, B->s_last, 1, "(DISARM without ABORT)");
    }
    if (B && B->st == NB_BARRIER && !s_script && t - B->t_enter > s_timeout_s * 1000.0) { LOG("NETBARRIER timeout b=%u wait_ms=%.0f\n", B->bat, t - B->t_enter); refuse_battle("barrier_timeout", "BARRIER", 1, 6); }
    /* desync: the guest's DESYNC_NOTE, the host's DUMP_REQ, the aborts after the dump */
    if (B && B->desync && s_role == 2 && B->ds_note_tx >= 0 && t - B->ds_note_tx >= 100.0 && B->bat) {
        NpDesyncNote d; memset(&d, 0, sizeof d); d.f = B->ds_f; d.local_t1 = B->ds_lt1; d.peer_t1 = B->ds_pt1;
        const uint64_t *lc = comps_at(B->ds_f); if (lc) { memcpy(d.comps, lc, sizeof d.comps); d.comps_ok = 1; }
        uint8_t b[256]; size_t k = np_desync_put(b, &d); send_raw(NP_DESYNC_NOTE, B->bat, b, k); B->ds_note_tx = t;
    }
    if (B && B->dump_reqd && !B->dump_acked && B->dump_kind && t - B->dumpreq_tx >= 100.0 && B->bat) {
        NpDumpReq d; memset(&d, 0, sizeof d); d.kind = (uint8_t)(B->ctltype && B->dump_kind == 1 ? 3 : B->dump_kind); d.f = B->dump_f; d.g = B->dump_g;   /* C2: kind 3 = ctltype (a kind-1 dump) */
        const uint64_t *lc = comps_at(B->dump_f); if (lc) { memcpy(d.comps, lc, sizeof d.comps); d.comps_ok = 1; }
        uint8_t b[256]; size_t k = np_dumpreq_put(b, &d); send_raw(NP_DUMP_REQ, B->bat, b, k); B->dumpreq_tx = t;
    }
    if (B && B->dump_kind == 1 && B->dump_done && !B->abort_reason && (B->st == NB_ON)) {
        int reason = B->ctltype ? NPR_CTLTYPE : NPR_DESYNC;
        if ((s_role == 1 || B->ctltype) && (B->dump_acked || t - B->dump_t >= 2000.0)) set_abort(reason, B->s_last, 0, B->dump_acked ? "(after both dumps)" : "(2 s after the dump)");
        else if (s_role == 2 && !B->ctltype && t - B->dump_t >= 2000.0) set_abort(reason, B->s_last, 0, "(self-abort 2 s after the dump)");
    }
}
static void pause_poll(void) {
    if (!s_mgr) return;
    void *il = *(void **)((char *)s_mgr + MGR_INPUTOBJ);
    if (!il) return;
    int pt = *(volatile int *)((char *)il + 0x3a8);
    int p = pt != 0;
    if (p == s_local_paused) return;
    s_local_paused = p;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_c2_nopauseflag) { LOG("NETPAUSE local f=%u harness control h_c2_nopauseflag=1: the pause is not sent\n", B ? B->s_last : 0); return; }
#endif
    uint32_t f = B ? B->s_last : 0;
    if (p) {
        s_pause_t0 = nowms();
        LOG("NETPAUSE local f=%u\n", f);
        NpPause pz = { f, 0 }; uint8_t b[8]; size_t n = np_pause_put(b, &pz); rs_add(NP_PAUSE, B ? B->bat : 0, b, n, 3, 50.0);
    } else {
        uint32_t ms = (uint32_t)(nowms() - s_pause_t0);
        LOG("NETRESUME local f=%u paused_ms=%u\n", f, ms);
        NpPause pz = { f, ms }; uint8_t b[8]; size_t n = np_pause_put(b, &pz); rs_add(NP_RESUME, B ? B->bat : 0, b, n, 3, 50.0);
    }
    title_dirty();
}
static void rx_drain(void) {
    uint8_t dg[NP_SLOT]; size_t n; uint64_t q;
    while (net_rx_pop(dg, sizeof dg, &n, &q)) on_packet(dg, n, q);
}
void ns_tick_begin(void *mgr) {
    if (!g_on) return;
    s_mgr = mgr;
    double t = nowms();
    int gate_ok = B && B->setup_seen;   /* C2 fix r1: no barrier / entry work before UE's Setup returned (the copy words are written there) */
#ifdef SC6RB_HARNESS
    if (g_cfg.h_c2_setup_gate_off) gate_ok = 1;   /* NPRESETX (harness-only control): the pre-fix behavior, the work runs on the first tick after Init */
#endif
    if (B && B->st == NB_BARRIER && B->inited && !B->setup_seen) B->pre_setup_ticks++;
    if (s_script) {
        if (B->st == NB_BARRIER && B->inited && gate_ok) script_release();
        return;
    }
    if (!s_netup) return;
    rx_drain();
    if (net_counters()->rx_overflow && B && (B->st == NB_ON || B->st == NB_BARRIER) && !B->abort_reason) {
        static long long seen; if (net_counters()->rx_overflow != seen) { seen = net_counters()->rx_overflow; LOG("NETQ overflow dir=rx n=%lld\n", seen); set_abort(NPR_NETQ_OVERFLOW, B->s_last, 0, NULL); }
    }
    pause_poll();
    timers(t);
    /* barrier work */
    if (B && B->st == NB_BARRIER && B->inited && gate_ok && s_ss == SS_IDLE) {
        if (!B->built && (s_role == 2 || s_nrtt >= 10 || t - B->t_enter >= 500.0)) { setup_build(); setup_compare(); }   /* the peer's blob may have arrived first */
        if (B->built && !B->blob_acked && t - B->blob_tx >= 250.0) { setup_send(); B->blob_tx = t; }
        if (B->matched && t - B->ready_tx >= 100.0) { uint8_t b[8]; put_u64(b, B->setup_full); send_raw(NP_READY, B->bat, b, 8); B->ready_tx = t; B->ready_tx_n++; ready_check(); }
    }
}
static void send_input(double t) {
    NpInput in; memset(&in, 0, sizeof in);
    int armed = B && B->bat && (B->st == NB_ON || (B->st == NB_BARRIER && B->matched) || (B->st == NB_DONE && t < s_tail_until && !B->abort_reason) ||
                                (B->st == NB_DONE && B->abort_reason && B->peer_bat_ok));
    uint16_t bat = armed ? B->bat : 0;
    in.rec_size = s_role == 1 ? NP_REC_WORDX : NP_REC_WORD;
    int lp = s_local_paused;
#ifdef SC6RB_HARNESS
    if (g_cfg.h_c2_nopauseflag) lp = 0;
#endif
    in.iflags = (uint16_t)((lp ? NP_IF_PAUSED : 0) | (B && B->st == NB_BARRIER ? NP_IF_BARRIER : 0) | (B && B->abort_reason ? NP_IF_DISARM : 0));
    in.s = B ? B->s_last : 0; in.cconf = B ? B->C : 0;
    { double a = adv_avg(); in.adv16 = (int16_t)(a * 16.0 > 32000 ? 32000 : a * 16.0 < -32000 ? -32000 : a * 16.0); }
    if (armed) { in.ack = B->F; in.cack = B->pc_contig; }
    int packets = 1;
    uint32_t have = armed ? B->next_t - 1 : 0;
    uint32_t first = armed ? B->peer_ack + 1 : 0;
    if (armed && have >= first && have - first + 1 > NP_IN_MAXREC) packets = 2;
    for (int pk = 0; pk < packets; pk++) {
        in.first = first; in.count = 0;
        if (armed) for (uint32_t f = first; f <= have && in.count < NP_IN_MAXREC; f++) { NpRec r; if (!lget(f, &r)) break; in.rec[in.count++] = r; }
        in.cfirst = 0; in.ccount = 0;
        if (armed && pk == 0) {
            in.cfirst = B->peer_cack + 1;
            for (uint32_t f = in.cfirst; f <= B->cconf && in.ccount < NP_IN_MAXCHK; f++) { Chk *c = &B->CK[f % NR]; if (!c->have || c->f != f) break; in.chk[in.ccount].t1 = c->t1; in.chk[in.ccount].t15 = c->t15; in.chk[in.ccount].ued = c->ued; in.ccount++; }
        }
        in.ping_id = ++s_ping_id; s_ping_qpc[in.ping_id % PINGR] = qnow();
        in.echo_id = s_echo_id; in.echo_hold_us = s_echo_id ? (uint32_t)qpc_us(qnow() - s_echo_qpc) : 0;
        uint8_t pay[NP_MAXPAY]; size_t k = np_input_put(pay, sizeof pay, &in);
        if (k) send_raw(NP_INPUT, bat, pay, k);
        first += in.count;
    }
#ifdef SC6RB_HARNESS
    if (g_cfg.h_c2_flood_at && B && B->s_last >= (uint32_t)g_cfg.h_c2_flood_at && !B->abort_reason) {   /* NOVF: a burst far larger than the peer's rx ring */
        static int done; if (!done) { done = 1; LOG("NETQ harness plant h_c2_flood_at=%d: 2000 copies of a datagram at once (net thread)\n", g_cfg.h_c2_flood_at); net_h_burst(2000); }
    }
#endif
}
void ns_tick_end(void) {
    if (!g_on || s_script || !s_netup) return;
    double t = nowms();
    if (s_ss == SS_HELLO && s_hl_ready == 0 && s_ms_ready) hello_build();
    if (s_ss == SS_HELLO && s_hl_ready && t - s_last_hello >= 250.0) { uint8_t b[NP_HELLO_LEN]; np_hello_put(b, &s_hl); send_raw(NP_HELLO, 0, b, NP_HELLO_LEN); s_last_hello = t; }
    int noseed = 0;
#ifdef SC6RB_HARNESS
    noseed = g_cfg.h_c2_noseed;   /* NSU-SEED (harness only): the host never sends its seed */
#endif
    if (s_ss == SS_IDLE && s_role == 1 && s_seed_seq && s_seed_acked != s_seed_seq && t - s_seed_tx >= 250.0 && !noseed) {
        NpSeed sd = { s_seed_seq, s_seed_k }; uint8_t b[8]; size_t k = np_seed_put(b, &sd); send_raw(NP_SEED, 0, b, k); s_seed_tx = t;
    }
    if (s_quit_pending && s_ss != SS_QUIT) {
        s_quit_pending = 0; s_ss = SS_QUIT;
        NpEnd e = { NPR_QUIT_F3, B ? B->s_last : 0 }; uint8_t b[8]; size_t n = np_end_put(b, &e);
        rs_add(NP_QUIT, B ? B->bat : 0, b, n, 5, 20.0);
        if (B && (B->st == NB_ON || B->st == NB_BARRIER)) set_abort(NPR_QUIT_F3, B->s_last, 0, "(F3)");
        title_dirty();
    }
#ifdef SC6RB_HARNESS
    if (g_cfg.h_c2_rs_burst && B && B->st == NB_ON && B->s_last >= 600) {   /* NRSQ (harness-only plant): more control messages than the resend queue holds */
        static int done; if (!done) { done = 1; LOG("NETQ harness plant h_c2_rs_burst=%d: that many PONG resends queued at once\n", g_cfg.h_c2_rs_burst);
                                      uint8_t z[8] = { 0 }; for (int i = 0; i < g_cfg.h_c2_rs_burst; i++) rs_add(NP_PONG, 0, z, 8, 1, 50.0); }
    }
#endif
    if (s_ph_ok && (s_ss == SS_IDLE || s_ss == SS_QUIT)) send_input(t);
    rs_run(t);
}
void ns_quit_local(const char *why) {
    if (!g_on || s_ss == SS_QUIT || s_quit_pending) return;
    LOG("NETQUIT by=local f=%u why=%s\n", B ? B->s_last : 0, why);
    s_quit_pending = 1;
    log_flush();
}
void ns_h_pause(int on) { (void)on; }

/* ------------------------------------------------------------------ the stall lever (§2.7) */
static int allowed_steps(int N, int *cause) {
    *cause = -1;
    if (!B || !B->inited) return N;
    if (B->st == NB_BARRIER) { *cause = NSC_START; return 0; }
    if (B->st != NB_ON || B->abort_reason) return N;
    if (s_script) return N;
    if (s_peer_paused) { *cause = NSC_PAUSE; return 0; }
    if (s_noresp || nowms() - s_last_rx > HB_MS) { *cause = NSC_PEER; return 0; }
    uint32_t s0 = B->s_last, F = B->F; int k = 0;
    for (int i = 1; i <= N; i++) {
        uint32_t f = s0 + (uint32_t)i;
        int zone = s0 == 0 || rb_zone_of(f);
        if (zone ? f > F : f > F + (uint32_t)s_maxrb) { *cause = zone ? NSC_LOCK : NSC_MAXRB; break; }
        k = i;
    }
    if (k < N && B->peer_left == 1) {   /* fix r1: the peer left this battle: F is final, this stall would never end */
        LOG("NETPEERLEFT stall b=%u f=%u F=%u cause=%s (a frame past the peer's last one is needed: ABORT peer_left)\n", B->bat, s0 + (uint32_t)k + 1, F, k_sc[*cause]);
        set_abort(NPR_PEER_LEFT, s0, 1, NULL);
        *cause = -1;
        return N;
    }
    if (k == N && N >= 1) {
        double la = adv_avg();
        if ((la - s_peer_adv) / 2.0 >= 1.0 && s0 >= s_last_sync + 30) { k = N - 1; *cause = NSC_SYNC; s_last_sync = s0; }
    }
    return k;
}
void ns_advance_after(void *il) {
    if (!g_on || !s_mgr || !B) return;
    if (*(void **)((char *)s_mgr + MGR_INPUTOBJ) != il) return;
    /* C2: the rx ring is drained here too. When the input object ticks before the manager (order A, HYPOTHESIS for the real game), the lever
     * would otherwise decide on the previous tick's frontier; F and the zone list only grow, so a decision made on a fresher F stays valid for
     * the steps of the next manager tick (which drains again) */
    if (s_netup && !s_script) rx_drain();
    volatile int32_t *gt = (volatile int32_t *)((char *)il + 0x3a4);
    int32_t round = *(volatile int32_t *)((char *)il + 0x3a0), lr = *(volatile int32_t *)((char *)s_mgr + 0x1488), lf = *(volatile int32_t *)((char *)s_mgr + 0x148c);
    int32_t lfe = round != lr ? 0 : lf;   /* UpdateBattle zeroes lastFrame when GameRound changed (dec_tick1.c FUN_1403fe520) */
    int N = *gt - lfe;
    if (N <= 0) return;
    int cause; int A = allowed_steps(N, &cause);
    if (A >= N) return;
    int32_t ng = lfe + A;
    if (s_lever_one && ng < *gt - 1) ng = *gt - 1;   /* net_lever=one: undo at most this tick's ++ */
    *gt = ng;
    if (cause >= 0) {
        B->stall[cause]++;
        if (B->stall_lines[cause]++ < 20) LOG("NETSTALL cause=%s f=%u n=%d\n", k_sc[cause], B->s_last + 1, N - A);
    }
}

/* ------------------------------------------------------------------ title, header */
void ns_title(char *out, size_t cap) {
    const char *N;
    char nb[32];
    if (s_ss == SS_QUIT) N = "QUIT";
    else if (s_ss == SS_REFUSED || s_ss == SS_SOCKFAIL) { snprintf(nb, sizeof nb, "REFUSED-%s", s_refcat); N = nb; }
    else if (B && B->st == NB_BARRIER) N = "BARRIER";
    else if (B && B->st == NB_ON && s_peer_paused) N = "PEERPAUSE";
    else if (B && B->st == NB_ON && s_noresp) N = "NORESP";
    else if (B && B->st == NB_ON) N = s_script ? "SCRIPT" : "ON";
    else if (B && B->bat && B->st == NB_DONE && strcmp(B->tok, "-") && strcmp(B->tok, "IDLE") && strcmp(B->tok, "ON")) N = B->tok;
    else if (s_ss == SS_HELLO) N = s_noresp && s_ph_ok == 0 && s_last_hello > 0 && s_hl_ready ? "HELLO" : "HELLO";
    else if (s_noresp) N = "NORESP";
    else N = s_script ? "SCRIPT" : "IDLE";
    int o = snprintf(out, cap, " net=%s ent=%s role=%s", N, entry_token(), role_s());
    if (o > 0 && (size_t)o < cap && B && B->t15_diff) o += snprintf(out + o, cap - (size_t)o, " kd=%u", B->t15_diff);
    if (o > 0 && (size_t)o < cap && B && B->ued_diff) snprintf(out + o, cap - (size_t)o, " ud=%u", B->ued_diff);
}
static char s_blobhex[8300];
const char *ns_setup_blob_hex(void) {   /* the SETUP blob the host used: the host's own, the guest's received copy */
    if (!g_on || !B) return NULL;
    const uint8_t *bl = s_role == 1 ? B->blob : B->pblob; size_t n = s_role == 1 ? B->blen : B->plen;
    if (s_role == 1 ? !B->built : !B->pok) return NULL;
    static const char hx[] = "0123456789abcdef";
    if (n * 2 + 1 > sizeof s_blobhex) n = (sizeof s_blobhex - 1) / 2;
    for (size_t i = 0; i < n; i++) { s_blobhex[2 * i] = hx[bl[i] >> 4]; s_blobhex[2 * i + 1] = hx[bl[i] & 15]; }
    s_blobhex[2 * n] = 0;
    return s_blobhex;
}
void ns_header_keys(char *out, size_t cap) {
    out[0] = 0;
    if (!g_on || !B) return;
    snprintf(out, cap, " mode=rbnet role=%s conv=%d D=%d Dl=%d max_rb=%d setup_x=%016llx setup_full=%016llx simcfg_net=%016llx modset2=%016llx net_remote=%d", role_s(),
             entry_converted(), s_D, B->dl, s_maxrb, (unsigned long long)B->setup_x, (unsigned long long)B->setup_full, (unsigned long long)ns_simcfg_net(),
             (unsigned long long)s_modset2, ns_remote_side());
}
