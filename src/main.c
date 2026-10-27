/* sc6rb entry: version guard, patching, manager Tick hook, lifecycle hooks,
 * CRT rand redirection, hotkeys and the crash logger. */
#include "common.h"
#include "modes.h"
#include "mem.h"
#include "fwd.h"
#include "MinHook.h"
#include "l1.h"
#include "c1a_run.h"
#include "netsess.h"   /* C2 */
#include "nobs.h"      /* Option A M1 */
#include "rbo.h"       /* Option A M2 */
#include "vm.h"        /* v0.32.0 review fix B1 */
#include "argcfix.h"   /* v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix */
#include <xmmintrin.h>

void nobs_install(void);   /* src/nobs_install.c */
static void env_log(void);

static void title_update(void);
uintptr_t g_base;
size_t g_image_size;
volatile const char *g_last_action = "load";
volatile int g_mode;
volatile uint64_t g_seq;
volatile int g_inert = 1;
volatile DWORD g_ctx_tid;
volatile uint32_t g_ctx_flags;
volatile uint64_t g_cur_seq;
SnapExtra g_x;
static HMODULE g_self;
HMODULE g_ns_self;   /* C2: the plugin module for netsess.c (HELLO plugin_sha, the plugins folder) */
static uintptr_t g_self_lo, g_self_hi;

uint32_t core_frame(void) { return g_base ? *(volatile uint32_t *)VA(A_FRAMECTR) : 0; }

/* ------------------------------------------------------------------ version guard */
typedef struct { uint64_t va; const char *hex; const char *name; } Sig;
static const Sig k_sigs[] = {
    { 0x1403d2a20ULL, "488bcae93892f0ff", "step_thunk" },
    { 0x1402dbc60ULL, "4c8bdc49895b1049896b18", "step_prologue" },
    { 0x1403c3650ULL, "488bcae9586bf1ff", "init_thunk" },
    { 0x1403bcee0ULL, "e93bd6f1ff", "shutdown_thunk" },
    { 0x1403cfe80ULL, "e91bb5f0ff", "setup_thunk" },
    { 0x1403fbf30ULL, "40534883ec30488bd9", "mgr_tick_prologue" },
    { 0x1403fe520ULL, "4c8bdc534881ece0000000", "updatebattle_prologue" },
    { 0x140d46a00ULL, "4885c9741d4c8b05bc074503", "fmemory_free" },
    { 0x140d4f760ULL, "4c8bc9488b0d5e7a4403", "fmemory_malloc" },
    { 0x1402d9c30ULL, "b801000000c3", "null_listener_q138" },
    { 0x1402d9c40ULL, "32c0c3", "null_listener_q140" },
    { 0x1402d2bc0ULL, "c20000", "null_listener_ret0" },
};
typedef struct { uint64_t slot; uint64_t val; const char *name; } PtrSig;
static const PtrSig k_ptrs[] = {
    { A_IFC_VT + IFC_STEP, A_THUNK_STEP, "slot_step" },
    { A_IFC_VT + IFC_INIT, A_THUNK_INIT, "slot_init" },
    { A_IFC_VT + IFC_SHUTDOWN, A_THUNK_SHUTDOWN, "slot_shutdown" },
    { A_IFC_VT + IFC_SETUP, A_THUNK_SETUP, "slot_setup" },
    { A_NULL_LISTENER, A_NULL_LISTENER_VT, "null_listener_vt" },
    { A_NULL_LISTENER_VT + 0x138, 0x1402d9c30ULL, "null_vt_138" },
    { A_NULL_LISTENER_VT + 0x140, 0x1402d9c40ULL, "null_vt_140" },
    { A_NULL_LISTENER_VT + 0x08, 0x1402d2bc0ULL, "null_vt_008" },
};

static int hexbyte(const char *h) { int v; sscanf(h, "%2x", &v); return v; }

static int version_guard(void) {
    int fails = 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    uint32_t ts = nt->FileHeader.TimeDateStamp, soi = nt->OptionalHeader.SizeOfImage;
    int pe_ok = ts == 0x61717836u && soi == 0x4ba2000u;
    LOG("GUARD pe timestamp=0x%08x size_of_image=0x%x %s\n", ts, soi, pe_ok ? "ok" : "MISMATCH");
    if (!pe_ok) {
        /* a different build: do not touch any other address (they may be outside the image) */
        LOG("GUARD result=FAIL (plugin inert) fails=1 reason=pe_header\n");
        return 0;
    }
    for (size_t i = 0; i < sizeof k_sigs / sizeof k_sigs[0]; i++) {
        const uint8_t *p = (const uint8_t *)VA(k_sigs[i].va);
        size_t n = strlen(k_sigs[i].hex) / 2;
        int ok = mem_readable(p, n);
        char got[64] = "";
        for (size_t j = 0; ok && j < n; j++) {
            snprintf(got + j * 2, 3, "%02x", p[j]);
            if (p[j] != hexbyte(k_sigs[i].hex + j * 2)) ok = 0;
        }
        if (!ok) { fails++; LOG("GUARD sig %s rva=0x%llx expected=%s got=%s MISMATCH\n", k_sigs[i].name, (unsigned long long)(k_sigs[i].va - IMG_PREF), k_sigs[i].hex, got); }
    }
    for (size_t i = 0; i < sizeof k_ptrs / sizeof k_ptrs[0]; i++) {
        if (!mem_readable((void *)VA(k_ptrs[i].slot), 8)) { fails++; LOG("GUARD ptr %s unreadable\n", k_ptrs[i].name); continue; }
        uintptr_t v = *(uintptr_t *)VA(k_ptrs[i].slot);
        if (v != VA(k_ptrs[i].val)) {
            fails++;
            LOG("GUARD ptr %s at rva=0x%llx expected_rva=0x%llx got=%p MISMATCH\n", k_ptrs[i].name, (unsigned long long)(k_ptrs[i].slot - IMG_PREF),
                (unsigned long long)(k_ptrs[i].val - IMG_PREF), (void *)v);
        }
    }
    const uint8_t *d = (const uint8_t *)VA(A_DELAY_SITE);
    static const uint8_t stock[8] = { 0x74, 0x06, 0x2b, 0x9f, 0x90, 0x03, 0x00, 0x00 };
    if (!mem_readable(d, 8)) { LOG("GUARD result=FAIL (plugin inert) fails=%d reason=delay_site_unreadable\n", fails + 1); return 0; }
    LOG("GUARD info offline_delay_site=%02x%02x%02x%02x%02x%02x%02x%02x (%s)\n", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
        memcmp(d, stock, 8) ? "MODIFIED, e.g. 1-frame delay patch" : "stock");
    LOG("GUARD result=%s fails=%d\n", fails ? "FAIL (plugin inert)" : "ok", fails);
    return fails == 0;
}

/* ------------------------------------------------------------------ patch helpers */
static int patch_ptr(void *slot, void *val) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer((void **)slot, val);
    DWORD tmp;
    VirtualProtect(slot, sizeof(void *), old, &tmp);
    return 1;
}

/* ------------------------------------------------------------------ lifecycle hooks (IBattleCore vtable) */
typedef void (*Fn4)(void *, void *, void *, void *);
static Fn4 o_init, o_shutdown, o_setup;
static int g_last_class = -1;        /* input object class from the last manager Tick */
static double g_last_class_ms = -1e9;

static int may_route_init(const char **why) {
    if (!g_cfg.init_routing) { *why = "disabled"; return 0; }
    if (!g_arena_ok || !g_mem_hooked) { *why = "arena_or_memhook"; return 0; }
    if (g_mode == MODE_OFF) { *why = "mode_off"; return 0; }
    if (g_last_class != 1) { *why = g_last_class == 2 ? "online" : g_last_class == 3 ? "unknown_input_object" : "no_input_object_seen"; return 0; }
    if (now_ms() - g_last_class_ms > 2000) { *why = "stale_classification"; return 0; }
    *why = "offline";
    return 1;
}

static int g_late_done;
static void late_install(void);

/* Round 1: Init always ran before the manager's first Tick (routed=0 for every battle), so
 * the battle's online/offline class is unknown at Init. Default since round 2: allocations made
 * inside Init/Setup are only TRACKED (bookkeeping, also fine online); rollback modes snapshot
 * the live tracked blocks. init_routing=1 restores the old routing (needs a fresh offline Tick). */
static void init_like_call(const char *what, Fn4 fn, void *iface, void *a2, void *a3, void *a4, int *routed_out) {
    const char *why;
    int route = may_route_init(&why);
    uint32_t flags = CTX_INIT | (g_cfg.track_init && g_mem_hooked ? CTX_TRACK : 0) | (route ? CTX_ROUTE : 0);
    g_ctx_flags = flags; g_ctx_tid = GetCurrentThreadId();
    fn(iface, a2, a3, a4);
    g_ctx_tid = 0; g_ctx_flags = 0;
    LOG("BATTLE %s_done routed=%d why=%s tracked=%d tid=%lu\n", what, route, why, (flags & CTX_TRACK) != 0, GetCurrentThreadId());
    *routed_out = route;
}

static void hk_init(void *iface, void *rule, void *a3, void *a4) {
    uint32_t seed = rule ? *(uint32_t *)((char *)rule + 0x10) : 0;
    g_last_action = "init";
    if (!g_late_done) late_install();   /* Init runs before the first manager Tick */
    void *ret = __builtin_return_address(0);
    char rb[0x40 * 2 + 1] = "";
    if (rule && mem_readable(rule, 0x40)) for (int i = 0; i < 0x40; i++) snprintf(rb + i * 2, 3, "%02x", ((uint8_t *)rule)[i]);
    LOG("BATTLE init_call caller=%s0x%llx rule40=%s\n", IN_IMAGE(ret) ? "game+" : "abs:", (unsigned long long)(IN_IMAGE(ret) ? RVA(ret) : (uintptr_t)ret), rb);
    /* v0.22.0-C1a: mode=replay with a loaded recording: Init runs on a private copy of the rule with the recorded seed at +0x10.
     * Init reads only rule+0x10 (mov ecx,[rcx+0x10] at 0x1402da1cc), stores the value at 0x144844010 and keeps no pointer to the rule
     * (audit/c1a/RECON.md §2.1, §4); the round reseeds use 0x144844010, so they follow. UE's own rule is never written. */
    uint32_t used = seed; void *rule_used = rule;
    uint8_t rcopy[0x40];
    int ovr = 0;   /* v0.22.1: the override really applied (never for the menu demo Init, C1A-AUDIT C4) */
    if (rule && c1a_seed_for_init(seed, &used)) {
        size_t n = mem_readable(rule, 0x40) ? 0x40 : mem_readable(rule, 0x14) ? 0x14 : 0;
        memset(rcopy, 0, sizeof rcopy);
        if (n) { memcpy(rcopy, rule, n); memcpy(rcopy + 0x10, &used, 4); rule_used = rcopy; ovr = 1; }
        else { LOG("C1A WARN rule not readable: Init runs with UE's seed\n"); used = seed; }
    }
    modes_on_init(1, used, 0);
    int route = 0;
    init_like_call("init", o_init, iface, rule_used, a3, a4, &route);
    modes_on_init(0, used, route);
    c1a_after_init(seed, used, ovr);
    if (g_mode == MODE_RBNET) ns_on_init(seed);   /* C2: the start barrier (§2.4) */
    g_last_action = "init:done";
}

static void hk_setup(void *iface, void *a2, void *a3, void *a4) {
    g_last_action = "setup";
    if (!g_late_done) late_install();
    void *ret = __builtin_return_address(0);
    LOG("BATTLE setup_call caller=%s0x%llx\n", IN_IMAGE(ret) ? "game+" : "abs:", (unsigned long long)(IN_IMAGE(ret) ? RVA(ret) : (uintptr_t)ret));
    modes_on_setup(1);
    int route = 0;
    init_like_call("setup", o_setup, iface, a2, a3, a4, &route);
    modes_on_setup(0);
    if (g_mode == MODE_RBNET) ns_on_setup();   /* C2 */
    g_last_action = "setup:done";
}

static void hk_shutdown(void *iface, void *a2, void *a3, void *a4) {
    g_last_action = "shutdown";
    modes_on_shutdown(1);
    o_shutdown(iface, a2, a3, a4);
    modes_on_shutdown(0);
    if (g_mode == MODE_RBNET) ns_on_shutdown();   /* C2: NETEND, the next battle's seed (host) */
    g_last_action = "shutdown:done";
}

/* ------------------------------------------------------------------ CRT rand redirection */
typedef int (*RandFn)(void);
typedef void (*SrandFn)(unsigned);
static RandFn o_rand;
static SrandFn o_srand;

/* v0.22.0-C1a RANDX (audit/c1a/RECON.md R2, HYPOTHESIS: CRT rand from core code outside the step uses per-launch CRT state, e.g. the
 * camera layer init FUN_140326230 at 0x140326362). In record/replay only: every CRT rand/srand call that does NOT take the LCG and
 * whose return address is in core code 0x1402c0000..0x1403a0000 is counted per caller (RANDXSUM at Shutdown) and logged (budget 50).
 * Init's own calls after srand(seed >> 4) (0x14034f62e / 0x14034f652, ctx_flags 0x10|0x20) are expected and deterministic. */
#define RANDX_MAX 16
static volatile LONG g_randx_lines;
static uint32_t g_randx_rva[RANDX_MAX]; static volatile LONG g_randx_cnt[RANDX_MAX]; static Spin g_randx_lock;
static void randx_note(void *ra, const char *fn) {
    if (!c1a_active() || !IN_IMAGE(ra)) return;
    uint32_t rva = (uint32_t)RVA(ra);
    if (rva < 0x2c0000 || rva >= 0x3a0000) return;
    spin_lock(&g_randx_lock);
    for (int i = 0; i < RANDX_MAX; i++) {
        if (g_randx_rva[i] == rva || !g_randx_rva[i]) { g_randx_rva[i] = rva; g_randx_cnt[i]++; break; }
    }
    spin_unlock(&g_randx_lock);
    if (InterlockedIncrement(&g_randx_lines) <= 50)
        LOG("RANDX fn=%s caller=game+0x%x seq=%llu fr=%u ctx_flags=0x%x step_ctx=%d tid=%lu (CRT state: per launch unless seeded right before)\n", fn, rva,
            (unsigned long long)g_seq, core_frame(), g_ctx_flags, in_ctx(), GetCurrentThreadId());
}
void randx_report(const char *why) {
    if (!c1a_active()) return;
    int n = 0;
    for (int i = 0; i < RANDX_MAX; i++) if (g_randx_rva[i]) { n++; LOG("RANDXSUM why=%s caller=game+0x%x n=%ld\n", why, g_randx_rva[i], g_randx_cnt[i]); }
    LOG("RANDXSUM why=%s callers=%d lines=%ld (CRT rand/srand from core code 0x1402c0000..0x1403a0000 outside the step LCG)\n", why, n, (long)g_randx_lines);
}

/* identical formula to the MSVC CRT rand, but the state is ours and snapshotted */
static int hk_rand(void) {
    if (in_ctx() && (g_ctx_flags & CTX_STEP)) {
        g_x.lcg = g_x.lcg * 214013u + 2531011u;
        g_x.rand_calls++;
        return (int)((g_x.lcg >> 16) & 0x7fff);
    }
    randx_note(__builtin_return_address(0), "rand");
    c1a_crt_note(0);   /* v0.22.1 (C1A-AUDIT C5): every CRT draw off the LCG, per thread (record/replay only) */
    return o_rand();
}
static void hk_srand(unsigned s) {
    if (in_ctx() && (g_ctx_flags & CTX_STEP)) { g_x.lcg = s; return; }
    randx_note(__builtin_return_address(0), "srand");
    c1a_crt_note(1);
    o_srand(s);
}

static void **find_iat(const char *dll, const char *fn) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(g_base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(g_base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(g_base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData);
            if (!strcmp((const char *)ibn->Name, fn)) return (void **)&iat->u1.Function;
        }
    }
    return NULL;
}

static void install_rand(void) {
    void **ir = find_iat("api-ms-win-crt-utility-l1-1-0.dll", "rand");
    void **is = find_iat("api-ms-win-crt-utility-l1-1-0.dll", "srand");
    LOG("RAND iat rand=%p (expected %p) srand=%p (expected %p) mode=%s\n", (void *)ir, (void *)VA(A_IAT_RAND), (void *)is,
        (void *)VA(A_IAT_SRAND), g_cfg.ai_rand_det ? "deterministic" : "crt");
    if (!g_cfg.ai_rand_det) return;
    if (ir != (void **)VA(A_IAT_RAND) || is != (void **)VA(A_IAT_SRAND)) { LOG("RAND iat mismatch, not patched\n"); return; }
    o_rand = (RandFn)*ir;
    o_srand = (SrandFn)*is;
    int ok = patch_ptr(ir, (void *)hk_rand) && patch_ptr(is, (void *)hk_srand);
    LOG("RAND patched=%d\n", ok);
}

/* ------------------------------------------------------------------ manager Tick hook */
typedef void (*TickFn)(void *mgr, float dt, void *a3, void *a4);
static TickFn o_tick;
static uint8_t g_keys_prev[256];
static uint64_t g_tick_hist[8];

#ifdef SC6RB_HARNESS
volatile int g_h_key_inject;   /* C2 harness only: one injected key press (NQUIT presses F3 through the real hotkey path) */
#endif
static int key_edge(int vk) {
    if (vk <= 0 || vk > 255) return 0;
#ifdef SC6RB_HARNESS
    if (g_h_key_inject && g_h_key_inject == vk) { g_h_key_inject = 0; return 1; }
#endif
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int edge = down && !g_keys_prev[vk];
    g_keys_prev[vk] = (uint8_t)down;
    return edge;
}

static int our_window_focused(void) {
#ifdef SC6RB_HARNESS
    if (g_mode == MODE_RBNET) return 1;   /* C2 harness: the children have no focused window */
#endif
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

static void poll_hotkeys(void) {
    int fc = key_edge(g_cfg.key_cycle), fm = key_edge(g_cfg.key_mark), fl = key_edge(g_cfg.key_lbplayer), fk = key_edge(g_cfg.key_camera);
    int fs = key_edge(g_cfg.key_scrub), fd = key_edge(g_cfg.key_depth), fa = g_cfg.key_anim ? key_edge(g_cfg.key_anim) : 0, fp = key_edge(g_cfg.key_passes);
    int f3 = key_edge(g_cfg.key_l3);
    int fq = g_cfg.key_quit ? key_edge(g_cfg.key_quit) : 0;   /* C2: hotkey_quit (rbnet only) */
    if (!(fc || fm || fl || fk || fs || fd || fa || fp || f3 || fq) || !our_window_focused()) return;
    if (fq && g_mode == MODE_RBNET) { LOG("KEY quit (F3): netplay QUIT\n"); ns_quit_local("F3"); }
    if (f3) { g_pending_l3 = 1; LOG("KEY L3 toggle (canonical call context)\n"); }
    if (fp) { g_pending_passes = 1; LOG("KEY F5 sync1x passes toggle\n"); }
    if (fa) { g_pending_anim = 1; LOG("KEY F5 anim_pins toggle\n"); }
    if (fs) { g_pending_bigscrub = 1; LOG("KEY F7 big scrub toggle\n"); }
    if (fd) { int cur = g_pending_depth > 0 ? g_pending_depth : g_depth_rt; g_pending_depth = cur >= g_cfg.roll_depth ? 1 : cur == 1 ? (g_cfg.roll_depth >= 3 ? 3 : g_cfg.roll_depth) : g_cfg.roll_depth;
              LOG("KEY roll_depth -> %d (applies at the next step)\n", g_pending_depth); }
    if (fk) {
        int cur = g_pending_cam >= 0 ? g_pending_cam : g_cfg.camera_exclude;
        g_pending_cam = !cur;
        LOG("KEY camera -> %s (applies at the next step)\n", g_pending_cam ? "exclude" : "include");
    }
    if (fc) {
        int cur = g_pending_mode >= 0 ? g_pending_mode : g_mode, next = g_cfg.cycle[0];
        for (int i = 0; i < g_cfg.ncycle; i++) if (g_cfg.cycle[i] == cur) { next = g_cfg.cycle[(i + 1) % g_cfg.ncycle]; break; }
        g_pending_mode = next;
        LOG("KEY cycle -> %s (applies at the next step)\n", g_mode_names[next]);
    }
    if (fm) { g_mark_pending = 1; LOG("KEY mark\n"); log_flush(); }
    if (fl) { g_pending_lbp = (g_pending_lbp > 0 ? g_pending_lbp : g_cfg.lb_player) == 1 ? 2 : 1; g_cfg.lb_player = g_pending_lbp; LOG("KEY loopback_player -> %d\n", g_pending_lbp); }
}

static void classify(void *mgr) {
    void *obj = *(void **)((char *)mgr + MGR_INPUTOBJ);
    int cls = 0;
    uintptr_t vt = 0;
    uint8_t bon = 0;
    if (obj && mem_readable(obj, 8)) {
        vt = *(uintptr_t *)obj;
        if (vt == VA(A_INPUTLOG_VT)) cls = 1;
        else if (vt == VA(A_INPUTSYNC_VT)) { cls = 2; bon = *((uint8_t *)obj + INPUT_BONLINE); }
        else {
            /* Another class. The game's own online path (UpdateBattle 0x1403fe520) requires
             * IsA(ALuxBattleFrameInputSync) && bOnline, so replicate its IsA test:
             * Sync class = 0x140914490(); depth at +0x90, base-chain array at +0x88 of the UClass;
             * the object's class at obj+0x10. IsA(Sync) or unreadable -> unknown (passthrough). */
            cls = 3;
            void *ocls = mem_readable((char *)obj + 0x10, 8) ? *(void **)((char *)obj + 0x10) : NULL;
            if (ocls && mem_readable((char *)ocls + 0x88, 0x10)) {
                char *sync = ((char *(*)(void))VA(0x140914490ULL))();
                int sd = *(int *)(sync + 0x90), od = *(int *)((char *)ocls + 0x90);
                void **chain = *(void ***)((char *)ocls + 0x88);
                int is_sync = sd <= od && chain && mem_readable(chain + sd, 8) && chain[sd] == (void *)(sync + 0x88);
                if (!is_sync) cls = 4;   /* offline, other input class */
            }
            for (int i = 0; i < g_cfg.nallow_vt; i++) if (vt == VA(g_cfg.allow_vt[i])) cls = 1;
        }
    }
    static int last_raw = -1;
    if (cls != last_raw) {
        last_raw = cls;
        LOG("INPOBJ mgr=%p obj=%p vt_rva=0x%llx class=%s bOnline=%u state=%u\n", mgr, obj, (unsigned long long)(vt && IN_IMAGE(vt) ? RVA(vt) : 0),
            cls == 0 ? "none" : cls == 1 ? "offline_log" : cls == 2 ? "online_sync" : cls == 4 ? "offline_other_class" : "unknown", bon,
            *((uint8_t *)mgr + MGR_STATE));
        if (cls == 2 || cls == 3) { LOG("ONLINE battle detected (or unknown input object): every mode is passthrough for it\n"); watch_disarm("online"); }
        log_flush();
    }
    if (cls == 4) cls = 1;   /* not IsA(Sync): the game itself treats it as offline */
    g_last_class = cls;
    g_last_class_ms = now_ms();
    if (cls) g_online = (cls == 2 || cls == 3);
}

static void install_crt_heap(void) {
    const char *dll = "api-ms-win-crt-heap-l1-1-0.dll";
    void **im = find_iat(dll, "malloc"), **ifr = find_iat(dll, "free"), **ire = find_iat(dll, "realloc"), **ica = find_iat(dll, "calloc");
    int match = im == (void **)VA(0x14322d2f8ULL) && ifr == (void **)VA(0x14322d2f0ULL) && ire == (void **)VA(0x14322d2e0ULL) &&
                ica == (void **)VA(0x14322d2e8ULL);
    LOG("CRTHEAP iat malloc=%p free=%p realloc=%p calloc=%p expected=%d enabled=%d\n", (void *)im, (void *)ifr, (void *)ire, (void *)ica,
        match, g_cfg.crt_hooks);
    if (!g_cfg.crt_hooks || !match || !g_mem_hooked) return;
    LOG("CRTHEAP patched=%d\n", mem_install_crt(im, ifr, ire, ica));
}

static void late_install(void) {
    g_late_done = 1;
    mem_install();
    install_crt_heap();
    install_rand();
    if (g_mode == MODE_RBNET) ns_start();   /* C2: the socket and the net thread (never in DllMain) */
    log_flush();
}

static void hk_tick(void *mgr, float dt, void *a3, void *a4) {
    if (g_inert) { o_tick(mgr, dt, a3, a4); return; }
    if (!g_late_done) late_install();
    classify(mgr);
    poll_hotkeys();
    if (g_mode == MODE_RBNET) ns_tick_begin(mgr);   /* C2: drain the rx ring once per tick, before any step of the tick (R14) */
    if (g_title_dirty) title_update();
    g_in_tick = 1;
    g_tick_steps = 0;
    o_tick(mgr, dt, a3, a4);
    g_in_tick = 0;
    if (g_mode == MODE_RBNET) ns_tick_end();      /* C2: INPUT / control sends */
    int n = g_tick_steps;
    g_tick_hist[n < 7 ? n : 7]++;
    if (n > 1) LOG("TICK steps=%d seq=%llu fr=%u\n", n, (unsigned long long)g_seq, core_frame());
}

/* ------------------------------------------------------------------ C2 (C2-SPEC.md §2.7): the stall lever. ALuxBattleFrameInputLog::Advance (vtable +0x630,
 * 0x1403e1fc0; V static, audit/C0-static.md §4) calls +0x650, +0x640, +0x648, then GameTime(+0x3a4)++. After the original returns, rbnet may
 * lower GameTime (never below lastFrame), so UpdateBattle's N = GameTime - lastFrame is the allowed step count. Never FrameInputSync. */
typedef void (*AdvFn)(void *self);
static AdvFn o_adv;
static void hk_advance(void *self) {
    o_adv(self);
    if (g_mode == MODE_RBNET) ns_advance_after(self);
}
static int install_lever(void) {
    void **slot = (void **)(VA(A_INPUTLOG_VT) + 0x630);
    if (!mem_readable(slot, 8) || *slot != (void *)VA(0x1403e1fc0ULL)) {
        LOG("NETUP FAIL lever (ALuxBattleFrameInputLog vtable +0x630 = %p, expected game+0x3e1fc0: the stall lever cannot be installed; netplay off)\n", mem_readable(slot, 8) ? *slot : NULL);
        return 0;
    }
    MH_STATUS s = MH_CreateHook((void *)VA(0x1403e1fc0ULL), (void *)hk_advance, (void **)&o_adv);
    if (s == MH_OK) s = MH_EnableHook((void *)VA(0x1403e1fc0ULL));
    LOG("PATCH advance_lever minhook=%d (%s)\n", (int)s, MH_StatusToString(s));
    return s == MH_OK;
}

/* ------------------------------------------------------------------ v0.20.0: threads, environment, window title
 * Thread names come from two sources: the MSVC SetThreadName exception (0x406D1388, seen by our VEH)
 * and GetThreadDescription during the per-battle THREAD census (normal context, never in the VEH).
 * The crash logger only reads this table and NtQueryInformationThread (a syscall, no heap). */
typedef LONG (NTAPI *NtQIT_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef HRESULT (WINAPI *GetThreadDescription_t)(HANDLE, PWSTR *);
static NtQIT_t p_ntqit;
static GetThreadDescription_t p_gtd;
typedef struct { volatile DWORD tid; char name[48]; } TName;
#define TNAME_MAX 512
static TName g_tnames[TNAME_MAX];
static volatile LONG g_ntnames;
static void tname_set(DWORD tid, const char *nm, size_t n) {
    if (!tid) return;
    LONG cnt = g_ntnames < TNAME_MAX ? g_ntnames : TNAME_MAX;
    TName *t = NULL;
    for (LONG i = 0; i < cnt; i++) if (g_tnames[i].tid == tid) { t = &g_tnames[i]; break; }
    if (!t) { LONG k = InterlockedIncrement(&g_ntnames) - 1; if (k >= TNAME_MAX) return; t = &g_tnames[k]; }
    if (n > sizeof t->name - 1) n = sizeof t->name - 1;
    for (size_t i = 0; i < n; i++) t->name[i] = (nm[i] >= 0x20 && nm[i] < 0x7f) ? nm[i] : '_';
    t->name[n] = 0;
    t->tid = tid;
}
static const char *tname_of(DWORD tid) {
    LONG cnt = g_ntnames < TNAME_MAX ? g_ntnames : TNAME_MAX;
    for (LONG i = 0; i < cnt; i++) if (g_tnames[i].tid == tid && g_tnames[i].name[0]) return g_tnames[i].name;
    return "-";
}
/* "module+0xoff" of a thread's start address (ThreadQuerySetWin32StartAddress = 9) */
static void thread_start_str(HANDLE th, char *out, size_t cap) {
    void *sa = NULL;
    if (!p_ntqit || p_ntqit(th, 9, &sa, sizeof sa, NULL) != 0 || !sa) { snprintf(out, cap, "?"); return; }
    uintptr_t a = (uintptr_t)sa;
    if (IN_IMAGE(a)) { snprintf(out, cap, "game+0x%llx", (unsigned long long)RVA(a)); return; }
    if (a >= g_self_lo && a < g_self_hi) { snprintf(out, cap, "sc6rb+0x%llx", (unsigned long long)(a - g_self_lo)); return; }
    HMODULE m = NULL; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        snprintf(out, cap, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m));
    } else snprintf(out, cap, "%p", sa);
}
/* VEH side: the MSVC SetThreadName exception {dwType=0x1000, szName, dwThreadID, dwFlags} */
static void tname_from_exception(EXCEPTION_RECORD *er) {
    if (er->NumberParameters < 3 || (er->ExceptionInformation[0] & 0xffffffffu) != 0x1000) return;
    const char *nm = (const char *)er->ExceptionInformation[1];
    DWORD tid = (DWORD)(er->ExceptionInformation[2] & 0xffffffffu);
    if (tid == 0xffffffffu) tid = GetCurrentThreadId();
    if (!nm || !mem_readable(nm, 1)) return;
    size_t n = 0;
    while (n < 47 && mem_readable(nm + n, 1) && nm[n]) n++;
    tname_set(tid, nm, n);
}
/* Per-battle census of the process's threads (normal context): every thread once per launch, then
 * only new ones. The crash log's tid can be matched against these lines. */
#include <tlhelp32.h>
void threads_census(const char *why) {
    static DWORD seen[1024]; static int nseen;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { LOG("THREAD census failed why=%s err=%lu\n", why, GetLastError()); return; }
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId(), step = modes_step_tid();
    int total = 0, newn = 0, lines = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        total++;
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == te.th32ThreadID) { known = 1; break; }
        if (known) continue;
        if (nseen < 1024) seen[nseen++] = te.th32ThreadID;
        newn++;
        char st[160] = "?", desc[64] = "";
        HANDLE th = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (th) {
            thread_start_str(th, st, sizeof st);
            PWSTR w = NULL;
            if (p_gtd && SUCCEEDED(p_gtd(th, &w)) && w) {
                int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, desc, (int)sizeof desc - 1, NULL, NULL);
                if (n > 0) { desc[n] = 0; if (desc[0]) tname_set(te.th32ThreadID, desc, strlen(desc)); }
                LocalFree(w);
            }
            CloseHandle(th);
        }
        if (lines++ < 200)
            LOG("THREAD why=%s tid=%lu step=%d start=%s desc=%s name=%s prio=%ld\n", why, te.th32ThreadID, te.th32ThreadID == step, st, desc[0] ? desc : "-",
                tname_of(te.th32ThreadID), (long)te.tpBasePri);
    }
    CloseHandle(snap);
    LOG("THREADS why=%s total=%d new=%d step_thread=%lu\n", why, total, newn, step);
}

/* CPU / OS / CRT environment (GATE-C-DESIGN §5.3, logged for the float-environment row) and the
 * vzeroall decision: AVX in CPUID.1:ECX[28], OSXSAVE in ECX[27], and XCR0 bits 1-2 (SSE + AVX state). */
static void cpuid2(uint32_t leaf, uint32_t sub, uint32_t r[4]) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub)); }
static uint64_t xgetbv0(void) { uint32_t lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); return ((uint64_t)hi << 32) | lo; }
static void env_log(void) {
    uint32_t r[4]; char vendor[13];
    cpuid2(0, 0, r); uint32_t maxleaf = r[0];
    memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4); vendor[12] = 0;
    cpuid2(1, 0, r);
    uint32_t fam = (r[0] >> 8) & 0xf, model = (r[0] >> 4) & 0xf, stepping = r[0] & 0xf;
    if (fam == 0xf) fam += (r[0] >> 20) & 0xff;
    if (fam >= 6) model |= ((r[0] >> 16) & 0xf) << 4;
    int sse42 = (r[2] >> 20) & 1, fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    int avx2 = 0, avx512f = 0;
    if (maxleaf >= 7) { cpuid2(7, 0, r); avx2 = (r[1] >> 5) & 1; avx512f = (r[1] >> 16) & 1; }
    uint64_t xcr0 = osxsave ? xgetbv0() : 0;
    int os_ymm = osxsave && (xcr0 & 6) == 6;
    g_l3_vzero = (uint8_t)(g_cfg.l3_vzeroall && avx && os_ymm);
    typedef LONG (NTAPI *RtlGetVersion_t)(OSVERSIONINFOW *);
    RtlGetVersion_t rgv = (RtlGetVersion_t)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW ov; memset(&ov, 0, sizeof ov); ov.dwOSVersionInfoSize = sizeof ov;
    if (rgv) rgv(&ov);
    uint32_t ucrt_ts = 0, ucrt_soi = 0;
    HMODULE uc = GetModuleHandleA("ucrtbase.dll");
    if (uc) {
        IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)uc;
        IMAGE_NT_HEADERS64 *nh = (IMAGE_NT_HEADERS64 *)((uintptr_t)uc + dh->e_lfanew);
        ucrt_ts = nh->FileHeader.TimeDateStamp; ucrt_soi = nh->OptionalHeader.SizeOfImage;
    }
    LOG("ENV cpu=%s family=0x%x model=0x%x stepping=%u sse42=%d avx=%d avx2=%d fma=%d avx512f=%d osxsave=%d xcr0=0x%llx os=%lu.%lu.%lu ucrtbase=%s ts=0x%08x size=0x%x mxcsr_now=%08x l3_vzeroall_cfg=%d vzeroall=%d\n",
        vendor, fam, model, stepping, sse42, avx, avx2, fma, avx512f, osxsave, (unsigned long long)xcr0, ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber,
        uc ? "loaded" : "not_loaded", ucrt_ts, ucrt_soi, _mm_getcsr(), g_cfg.l3_vzeroall, (int)g_l3_vzero);
}

/* Window title (GATE-C-DESIGN §7.5): set on the game thread from the Tick hook, only on a visible,
 * unowned top-level window that this thread created (a same-thread SetWindowText calls the window
 * procedure directly; no cross-thread message wait). The original title is kept as the prefix. */
typedef struct { HWND best; LONG area; } TitleFind;
static BOOL CALLBACK title_enum(HWND h, LPARAM lp) {
    TitleFind *f = (TitleFind *)lp;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT rc; if (!GetWindowRect(h, &rc)) return TRUE;
    LONG a = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (!f->best || a > f->area) { f->best = h; f->area = a; }
    return TRUE;
}
static void title_update(void) {
    static HWND hw; static char orig[160]; static unsigned tries;
    if (!g_cfg.title_tag[0]) { g_title_dirty = 0; return; }
    if (tries++ % 60) return;   /* at most every 60 ticks while pending */
    TitleFind f = { NULL, 0 };
    EnumThreadWindows(GetCurrentThreadId(), title_enum, (LPARAM)&f);
    if (!f.best) { if (tries < 600) LOG("TITLE no visible window on the game thread yet (tid=%lu)\n", GetCurrentThreadId()); return; }
    if (f.best != hw) {
        hw = f.best;
        GetWindowTextA(hw, orig, sizeof orig);
        char *cut = strstr(orig, "  [sc6rb"); if (cut) *cut = 0;
    }
    char tag[200], full[400];
    modes_title(tag, sizeof tag);
    snprintf(full, sizeof full, "%s  %s", orig[0] ? orig : "SOULCALIBUR VI", tag);
    SetWindowTextA(hw, full);
    LOG("TITLE set \"%s\"\n", full);
    g_title_dirty = 0; tries = 0;
}

/* ------------------------------------------------------------------ crash logger */
static volatile LONG g_crash_n;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388) { tname_from_exception(ep->ExceptionRecord); return EXCEPTION_CONTINUE_SEARCH; }   /* v0.20: SetThreadName */
    if (code == EXCEPTION_SINGLE_STEP && watch_veh(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    if (l1_veh(ep)) return EXCEPTION_CONTINUE_EXECUTION;   /* v0.19 kill switch: a fault inside the L1 capture handler */
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case 0xC0000409: case EXCEPTION_DATATYPE_MISALIGNMENT: break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&g_crash_n) > 8) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t ip = (uintptr_t)ep->ContextRecord->Rip;
    char where[80];
    if (IN_IMAGE(ip)) snprintf(where, sizeof where, "game+0x%llx", (unsigned long long)RVA(ip));
    else if (ip >= g_self_lo && ip < g_self_hi) snprintf(where, sizeof where, "sc6rb+0x%llx", (unsigned long long)(ip - g_self_lo));
    else {
        HMODULE m = NULL; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &m) && m) {
            GetModuleFileNameA(m, name, sizeof name);
            const char *b = strrchr(name, '\\');
            snprintf(where, sizeof where, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(ip - (uintptr_t)m));
        } else snprintf(where, sizeof where, "%p", (void *)ip);
    }
    uintptr_t info0 = 0, info1 = 0;
    if (ep->ExceptionRecord->NumberParameters >= 2) { info0 = ep->ExceptionRecord->ExceptionInformation[0]; info1 = ep->ExceptionRecord->ExceptionInformation[1]; }
    LOG("CRASH n=%ld code=0x%08lx ip=%s av_kind=%llu av_addr=%p mode=%s seq=%llu fr=%u action=%s ctx_tid=%lu ctx_flags=0x%x thread=%lu in_tick=%d\n",
        g_crash_n, code, where, (unsigned long long)info0, (void *)info1, g_mode_names[g_mode], (unsigned long long)g_seq,
        core_frame(), (const char *)g_last_action, g_ctx_tid, g_ctx_flags, GetCurrentThreadId(), g_in_tick);
    if (code != EXCEPTION_STACK_OVERFLOW) {
        uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
        char buf[512]; int o = 0, k = 0;
        for (int i = 0; i < 256 && k < 16 && o < 480; i++) {
            if (!mem_readable(sp + i, 8)) break;
            uintptr_t v = sp[i];
            if (IN_IMAGE(v)) { o += snprintf(buf + o, sizeof buf - o, " g+%llx", (unsigned long long)RVA(v)); k++; }
            else if (v >= g_self_lo && v < g_self_hi) { o += snprintf(buf + o, sizeof buf - o, " s+%llx", (unsigned long long)(v - g_self_lo)); k++; }
        }
        buf[o] = 0;
        LOG("CRASH stack_candidates:%s\n", buf);
        LOG("CRASH regs rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx rbx=%llx rsi=%llx rdi=%llx\n",
            (unsigned long long)ep->ContextRecord->Rax, (unsigned long long)ep->ContextRecord->Rcx, (unsigned long long)ep->ContextRecord->Rdx,
            (unsigned long long)ep->ContextRecord->R8, (unsigned long long)ep->ContextRecord->R9, (unsigned long long)ep->ContextRecord->Rbx,
            (unsigned long long)ep->ContextRecord->Rsi, (unsigned long long)ep->ContextRecord->Rdi);
    }
    {   /* v0.20 (GATE-C-DESIGN §3.9): which thread crashed, and is it the game's step thread? */
        DWORD tid = GetCurrentThreadId(), step = modes_step_tid();
        char st[160]; thread_start_str(GetCurrentThread(), st, sizeof st);
        LOG("CRASH thread tid=%lu step_thread=%lu is_step=%d start=%s tname=%s in_step=%d (%s)\n", tid, step, step && tid == step, st, tname_of(tid),
            g_ctx_tid == tid, step && tid == step ? "the game's step thread" : "NOT the step thread: GATE-C stop rule 4 applies");
    }
    c1a_flush_crash();   /* v0.22.0-C1a: the .rec / .play lines written so far */
    log_flush_crash();
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------ init */
#ifdef SC6RB_HARNESS
static uintptr_t g_harness_base;
static const char *g_harness_ini;
#endif
static void plugin_init(void) {
    g_ns_self = g_self;
#ifdef SC6RB_HARNESS
    g_base = g_harness_base;
#else
    g_base = (uintptr_t)GetModuleHandleA(NULL);
#endif
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    g_image_size = nt->OptionalHeader.SizeOfImage;
    {
        IMAGE_DOS_HEADER *sd = (IMAGE_DOS_HEADER *)g_self;
        IMAGE_NT_HEADERS64 *sn = (IMAGE_NT_HEADERS64 *)((uintptr_t)g_self + sd->e_lfanew);
        g_self_lo = (uintptr_t)g_self; g_self_hi = g_self_lo + sn->OptionalHeader.SizeOfImage;
    }
    char ini[MAX_PATH];
    GetModuleFileNameA(g_self, ini, sizeof ini);
    char *dot = strrchr(ini, '.');
    if (dot) strcpy(dot, ".ini"); else strcat(ini, ".ini");
#ifdef SC6RB_HARNESS
    strncpy(ini, g_harness_ini, sizeof ini - 1);
#endif
    config_load(ini);
    if (!g_cfg.log_dir[0]) {   /* 0.34.1: no log_dir key = <the plugin's folder>\sc6rb_logs */
        const char *slash = strrchr(ini, '\\');
        int n = slash ? (int)(slash - ini) : 0;
        snprintf(g_cfg.log_dir, sizeof g_cfg.log_dir, "%.*s%ssc6rb_logs", n, ini, n ? "\\" : "");
    }
    log_open(g_cfg.log_dir);
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    if (g_cfg.mode == MODE_OFF) {
        LOG("HDR sc6rb %s pid=%lu mode=off: plugin is a no-op (nothing patched)\n", SC6RB_VERSION, GetCurrentProcessId());
        log_flush();
        return;
    }
    LOG("HDR sc6rb %s pid=%lu base=%p image_size=0x%zx ini=%s exe=%s self=%p\n", SC6RB_VERSION, GetCurrentProcessId(), (void *)g_base, g_image_size, ini, exe, (void *)g_self);
    config_log();
    if (!version_guard()) { log_flush(); return; }

    /* Option A M1 (OPTA-M0.md §2.2, §5): netobserve branches here and RETURNS before every other install path (L1/L3, mem/CRT/rand
     * hooks, listener proxy/L4, the lever, stack scrub, arena/init routing, seed/setup injection, the watch DRs, ns_start, and the
     * generic VEH). It installs only the R-STACK thunks + the enqueue retarget. */
    if (g_cfg.mode == MODE_NETOBSERVE) {
        g_mode = MODE_NETOBSERVE;   /* modes_init (which normally sets g_mode) is on the path we are about to skip */
        env_log();   /* read-only CPUID/OS log line; sets g_l3_vzero (unused here) */
        nobs_install();
        LOG("READY mode=%s\n", g_mode_names[g_mode]);
        log_flush();
        return;
    }
    /* Option A M2 (OPTA-M2-SPEC.md, sc6rb/M2-INTERFACES.md §3): rbonline also branches here and RETURNS before every generic
     * installer. rbo_install() installs the rbonline patch set (M1's R-STACK patch set through src/rbo_thunks.S + the FetchInput front
     * door) and starts the side channel; everything the rollback core needs beyond that is enabled by rbo at ARM, on the game thread. */
    if (g_cfg.mode == MODE_RBONLINE) {
        g_mode = MODE_RBONLINE;
        env_log();
        rbo_install();
        LOG("READY mode=%s\n", g_mode_names[g_mode]);
        log_flush();
        return;
    }

    p_ntqit = (NtQIT_t)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    p_gtd = (GetThreadDescription_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    modes_init();
    env_log();
    if (!arena_setup(g_cfg.arena_mb, g_cfg.arena_fixed)) LOG("WARN arena unavailable: sync1/roll7/loopback disabled\n");
    itrack_init();
    watch_config(g_cfg.watch);
    AddVectoredExceptionHandler(1, veh);

    /* IBattleCore vtable: lifecycle + step hooks, counting thunks on the other slots */
    void **ivt = (void **)VA(A_IFC_VT);
    o_init = (Fn4)ivt[IFC_INIT / 8];
    o_shutdown = (Fn4)ivt[IFC_SHUTDOWN / 8];
    o_setup = (Fn4)ivt[IFC_SETUP / 8];
    o_step = (StepFn)ivt[IFC_STEP / 8];
    int ok = 1;
    for (int i = 0; i < IFC_NSLOTS; i++) {
        int off = i * 8;
        void *nv;
        ifc_family.fn[i] = ivt[i];
        if (off == IFC_INIT) nv = (void *)hk_init;
        else if (off == IFC_SHUTDOWN) nv = (void *)hk_shutdown;
        else if (off == IFC_SETUP) nv = (void *)hk_setup;
        else if (off == IFC_STEP) nv = (void *)hk_step;
        else if (g_cfg.ifc_census) nv = ifc_thunk_table[i];
        else continue;
        ok &= patch_ptr(&ivt[i], nv);
    }
    LOG("PATCH ifc_vtable ok=%d\n", ok);

    MH_STATUS s = MH_Initialize();
    if (s == MH_OK) s = MH_CreateHook((void *)VA(A_MGR_TICK), (void *)hk_tick, (void **)&o_tick);
    if (s == MH_OK) s = MH_EnableHook((void *)VA(A_MGR_TICK));
    LOG("PATCH mgr_tick minhook=%d (%s)\n", (int)s, MH_StatusToString(s));
    if (s != MH_OK) {
        /* without the Tick hook the step hook stays passthrough (g_in_tick never set) */
        LOG("WARN tick hook failed: step hook is passthrough only\n");
    }
    if (s == MH_OK) l1_install(); else LOG("L1 off: MinHook unavailable\n");
    if (g_mode == MODE_RBNET) {   /* C2: no lever = no netplay (loud: NETUP FAIL lever, title rb=REFUSED:lever) */
#ifdef SC6RB_HARNESS
        if (g_cfg.h_c2_nolever) LOG("PATCH advance_lever skipped (harness control h_c2_nolever=1: step 1 can arrive while the barrier holds)\n"); else
#endif
        if (s != MH_OK || !install_lever()) { LOG("CFG C2 REFUSE key=lever value=not_installed need=the_Advance_hook (mode=rbnet refused: the plugin runs mode=off)\n"); g_mode = MODE_OFF; }
    }
    vm_install();   /* v0.32.0 review fix B1 (src/vm.c): patches nothing unless vm_trace / vm_fill / vm_fix is on in record / replay / rbreplay */
    argc_install(); /* v0.34.0 (OPTA-M2-SPEC.md §23.1): rbreplay with argc_fix=1 (the default) fixes EvaluateIf sub-ops 0x25 / 0x91 for the whole process (no game thread runs a battle yet) */
    g_inert = 0;
    LOG("READY mode=%s\n", g_mode_names[g_mode]);
    log_flush();
}

#ifdef SC6RB_HARNESS
void sc6rb_harness_init(uintptr_t base, const char *ini) {
    g_self = GetModuleHandleA(NULL);
    g_harness_base = base;
    g_harness_ini = ini;
    plugin_init();
}
void sc6rb_harness_detach(void) {
    if (g_cfg.mode == MODE_NETOBSERVE) nobs_shutdown(0);   /* Option A M1: drain the writer + emit the summaries (DllMain's role) */
    if (g_cfg.mode == MODE_RBONLINE) rbo_shutdown(0);      /* Option A M2: the side channel, then the observer's writer + summaries */
    if (g_cfg.mode != MODE_RBONLINE && g_cfg.mode != MODE_NETOBSERVE) argc_detach();   /* v0.34.0: rbreplay's fix back to the stock bytes */
    LOG("DETACH harness seq=%llu\n", (unsigned long long)g_seq); log_flush();
}
#else
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        plugin_init();
    } else if (reason == DLL_PROCESS_DETACH && g_cfg.mode == MODE_RBONLINE) {
        rbo_shutdown(r != NULL);    /* Option A M2: as netobserve below (r != NULL = process exit: bounded waits) */
        LOG("DETACH rbonline\n");
        log_flush();
    } else if (reason == DLL_PROCESS_DETACH && g_cfg.mode == MODE_NETOBSERVE) {
        nobs_shutdown(r != NULL);   /* Option A M1: flush the writer + the battle summaries; r != NULL = process exit (m7: bounded waits) */
        LOG("DETACH netobserve\n");
        log_flush();
    } else if (reason == DLL_PROCESS_DETACH && g_cfg.mode != MODE_OFF) {
        argc_detach();   /* v0.34.0 (OPTA-M2-SPEC.md §23.1): rbreplay's fix back to the stock bytes (a no-op unless it was applied) */
        LOG("DETACH seq=%llu ticks0=%llu ticks1=%llu ticks2=%llu ticks3+=%llu\n", (unsigned long long)g_seq,
            (unsigned long long)g_tick_hist[0], (unsigned long long)g_tick_hist[1], (unsigned long long)g_tick_hist[2],
            (unsigned long long)(g_tick_hist[3] + g_tick_hist[4] + g_tick_hist[5] + g_tick_hist[6] + g_tick_hist[7]));
        log_flush();
    }
    return TRUE;
}
#endif
