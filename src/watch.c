/* Round 8: hardware write watches (debug registers DR0..DR3) on a few core-state addresses.
 * Answers "who writes this byte, and in which phase": the live step, a UE listener call inside
 * the live step, a test/resim step, between steps on the step thread, or another thread.
 *
 * Arming: a helper thread suspends each thread of the process for a moment and sets DR0..DR3/DR7
 * (skipping any thread whose DR7 is already in use, e.g. under a debugger). Threads created later
 * carry no watch. A hit is a trap after the writing instruction: the VEH records (watch, rip,
 * phase, value class), unwinds three callers once per new entry, and continues. No locks are
 * taken in the VEH other than a short spin on our own table; nothing is logged from it.
 * Off unless the ini names targets (watch=...). */
#include "common.h"
#include <tlhelp32.h>

#define W_MAX 4
#define WE_MAX 192
typedef struct { uintptr_t addr; int len; char name[40]; } WTarget;
typedef struct { uint64_t rip, last, callers[3]; uint32_t hits; uint8_t dr, ph, vcls, used; } WEnt;

static WTarget g_wt[W_MAX];
static int g_nwt, g_w_armed;
static WEnt g_we[WE_MAX];
static volatile LONG g_we_lock, g_we_full;
static volatile LONG64 g_w_hits[W_MAX], g_w_phase_hits[W_MAX][8];
static DWORD g_w_step_tid;
static int g_w_threads, g_w_set, g_w_busy, g_w_fail;
volatile int g_run_muted;
volatile int g_passthrough_active;
static uintptr_t g_self_lo, g_self_hi;

static const char *k_ph[8] = { "between", "live", "live_ue", "test", "test_ue", "ours", "other_thread", "passthrough_step" };

/* "PLAYER0+0x4529c:4" or "0x14470d0dc:4" */
static int parse_target(const char *s, WTarget *t) {
    uintptr_t base = 0; const char *p = s;
    while (*p == ' ') p++;
    if (!_strnicmp(p, "PLAYER", 6)) {
        int n = p[6] - '0'; if (n < 0 || n > 1) return 0;
        base = 0x1447156f0ULL + (uintptr_t)n * 0x973f0ULL; p += 7;
        if (*p != '+') return 0; p++;
    }
    char *e; unsigned long long off = strtoull(p, &e, 16);
    int len = 4;
    if (*e == ':') len = atoi(e + 1);
    if (len != 1 && len != 2 && len != 4 && len != 8) return 0;
    uintptr_t va = base + (uintptr_t)off;
    if (va & (uintptr_t)(len - 1)) return 0;   /* DR targets must be aligned to their length */
    t->addr = va; t->len = len;
    int i = 0; for (; s[i] && s[i] != ',' && i < (int)sizeof t->name - 1; i++) t->name[i] = s[i];
    t->name[i] = 0;
    return 1;
}

void watch_config(const char *spec) {
    g_nwt = 0;
    const char *p = spec;
    while (*p && g_nwt < W_MAX) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        WTarget t;
        if (parse_target(p, &t)) g_wt[g_nwt++] = t;
        else LOG("WATCH bad target '%.40s' (want PLAYERn+0xOFF:len or 0xVA:len, aligned, len 1/2/4/8)\n", p);
        while (*p && *p != ',') p++;
    }
    if (g_nwt) {
        HMODULE m = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)watch_config, &m) && m) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
            g_self_lo = (uintptr_t)m; g_self_hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
        }
    }
}

static uint64_t dr7_bits(void) {
    uint64_t d = 0;
    for (int i = 0; i < g_nwt; i++) {
        int lenbits = g_wt[i].len == 1 ? 0 : g_wt[i].len == 2 ? 1 : g_wt[i].len == 8 ? 2 : 3;
        d |= 1ULL << (i * 2);                         /* local enable */
        d |= 1ULL << (16 + i * 4);                    /* R/W = 01: data writes */
        d |= (uint64_t)lenbits << (18 + i * 4);
    }
    return d;
}

static DWORD WINAPI arm_proc(void *arg) {
    int disarm = arg != NULL;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { g_w_fail = -1; return 0; }
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    uint64_t d7 = dr7_bits();
    THREADENTRY32 te; te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; te.dwSize = sizeof te, ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        if (!disarm) g_w_threads++;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) { g_w_fail++; continue; }
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &c)) {
                if (disarm) {
                    if ((c.Dr7 & 0xffff00ffULL) == (d7 & 0xffff00ffULL)) { c.Dr7 = 0; c.Dr6 = 0; for (int i = 0; i < 4; i++) (&c.Dr0)[i] = 0; c.ContextFlags = CONTEXT_DEBUG_REGISTERS; if (SetThreadContext(h, &c)) g_w_set--; }
                } else if (c.Dr7 & 0xff) g_w_busy++;          /* someone else's breakpoints: leave them */
                else {
                    for (int i = 0; i < g_nwt; i++) (&c.Dr0)[i] = VA(g_wt[i].addr);
                    c.Dr6 = 0; c.Dr7 = d7;
                    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(h, &c)) g_w_set++; else g_w_fail++;
                }
            } else g_w_fail++;
            ResumeThread(h);
        } else g_w_fail++;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return 0;
}

/* called on the step thread at the first battle step */
void watch_arm(void) {
    if (!g_nwt || g_w_armed) return;
    g_w_armed = 1;
    g_w_step_tid = GetCurrentThreadId();
    HANDLE t = CreateThread(NULL, 64 * 1024, arm_proc, NULL, 0, NULL);
    if (!t) { LOG("WATCH arm failed: CreateThread %lu\n", GetLastError()); return; }
    DWORD w = WaitForSingleObject(t, 3000);
    CloseHandle(t);
    char tl[200] = ""; size_t o = 0;
    for (int i = 0; i < g_nwt && o < sizeof tl - 48; i++) o += (size_t)snprintf(tl + o, sizeof tl - o, "%sDR%d=%s", i ? "," : "", i, g_wt[i].name);
    LOG("WATCH arm %s threads=%d set=%d busy=%d failed=%d step_tid=%lu targets=%s\n", w == WAIT_OBJECT_0 ? "done" : "pending",
        g_w_threads, g_w_set, g_w_busy, g_w_fail, (unsigned long)g_w_step_tid, tl);
}

static void unwind3(const CONTEXT *ctx, uint64_t out[3]) {
    CONTEXT c = *ctx;
    for (int i = 0; i < 3; i++) {
        out[i] = 0;
        DWORD64 ib = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &ib, NULL);
        if (!f) {   /* leaf: return address on top of the stack */
            NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
            if (c.Rsp < (DWORD64)(uintptr_t)tib->StackLimit || c.Rsp + 8 > (DWORD64)(uintptr_t)tib->StackBase) return;
            c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp; c.Rsp += 8;
        } else {
            void *hd = NULL; DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, f, &c, &hd, &ef, NULL);
        }
        if (!c.Rip) return;
        out[i] = c.Rip;
    }
}

static int caller_is_self(const CONTEXT *ctx) {
    uint64_t cl[3]; unwind3(ctx, cl);
    for (int i = 0; i < 2; i++) if (cl[i] >= g_self_lo && cl[i] < g_self_hi) return 1;
    return 0;
}

/* returns 1 when the exception was one of our watch traps */
int watch_veh(EXCEPTION_POINTERS *ep) {
    if (!g_nwt || ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return 0;
    CONTEXT *c = ep->ContextRecord;
    uint64_t dr6 = c->Dr6;
    int mine = 0;
    DWORD tid = GetCurrentThreadId();
    uint64_t rip = c->Rip;
    int ph;
    int self_rip = rip >= g_self_lo && rip < g_self_hi;
#ifdef SC6RB_HARNESS
    self_rip = 0;   /* the harness links the fake core into the same module */
#endif
    if (tid != g_w_step_tid) ph = 6;
    else if (self_rip) ph = 5;
    else if (!IN_IMAGE(rip) && !(rip >= g_self_lo && rip < g_self_hi) && caller_is_self(c)) ph = 5;   /* our snapshot restores go through the CRT DLL's memcpy */
    else if (g_passthrough_active) ph = 7;
    else if (!g_ctx_flags) ph = 0;
    else { int ue = g_ctx_tid == 0; ph = g_run_muted ? (ue ? 4 : 3) : (ue ? 2 : 1); }
    for (int i = 0; i < g_nwt; i++) {
        if (!((dr6 >> i) & 1)) continue;
        mine = 1;
        InterlockedIncrement64(&g_w_hits[i]);
        InterlockedIncrement64(&g_w_phase_hits[i][ph]);
        uintptr_t a = VA(g_wt[i].addr);
        uint64_t v = 0; memcpy(&v, (void *)a, (size_t)g_wt[i].len);
        uint32_t lo = (uint32_t)v;
        uint8_t vc = lo == 0xffffffffu ? 2 : v == 0 ? 0 : 1;
        if (ph == 5) continue;   /* our own snapshot restores: counted, not itemized */
        while (InterlockedExchange(&g_we_lock, 1)) YieldProcessor();
        int k = 0, fresh = 0;
        for (; k < WE_MAX && g_we[k].used; k++)
            if (g_we[k].dr == i && g_we[k].rip == rip && g_we[k].ph == ph && g_we[k].vcls == vc) break;
        if (k < WE_MAX) {
            if (!g_we[k].used) { g_we[k].used = 1; g_we[k].dr = (uint8_t)i; g_we[k].rip = rip; g_we[k].ph = (uint8_t)ph; g_we[k].vcls = vc; fresh = 1; }
            g_we[k].hits++; g_we[k].last = v;
        } else g_we_full++;
        InterlockedExchange(&g_we_lock, 0);
        if (fresh) { uint64_t cl[3]; unwind3(c, cl); memcpy(g_we[k].callers, cl, sizeof cl); }
    }
    if (!mine) return 0;
    c->Dr6 = 0;
    return 1;
}

static void fmt_addr(uint64_t a, char *out, size_t cap) {
    if (IN_IMAGE(a)) snprintf(out, cap, "0x%llx", (unsigned long long)(a - g_base + IMG_PREF));
    else if (a >= g_self_lo && a < g_self_hi) snprintf(out, cap, "sc6rb+0x%llx", (unsigned long long)(a - g_self_lo));
    else snprintf(out, cap, "ext:%p", (void *)(uintptr_t)a);
}

void watch_report(const char *why) {
    if (!g_nwt || !g_w_armed) return;
    for (int i = 0; i < g_nwt; i++) {
        char ph[256] = ""; size_t o = 0;
        for (int p = 0; p < 8; p++)
            if (g_w_phase_hits[i][p]) o += (size_t)snprintf(ph + o, sizeof ph - o, " %s=%lld", k_ph[p], (long long)g_w_phase_hits[i][p]);
        LOG("WATCHSUM why=%s dr=%d at=%s hits=%lld%s\n", why, i, g_wt[i].name, (long long)g_w_hits[i], ph);
    }
    static const char *vcn[3] = { "zero", "other", "ffffffff" };
    for (int k = 0; k < WE_MAX && g_we[k].used; k++) {
        WEnt *e = &g_we[k];
        char r[48], c0[48], c1[48], c2[48];
        fmt_addr(e->rip, r, sizeof r); fmt_addr(e->callers[0], c0, sizeof c0); fmt_addr(e->callers[1], c1, sizeof c1); fmt_addr(e->callers[2], c2, sizeof c2);
        LOG("WATCHW why=%s dr=%d at=%s rip_after=%s phase=%s value=%s hits=%u last=0x%llx callers=%s,%s,%s\n", why, e->dr, g_wt[e->dr].name, r,
            k_ph[e->ph], vcn[e->vcls], e->hits, (unsigned long long)e->last, c0, c1, c2);
    }
    if (g_we_full) LOG("WATCH table_full=%ld\n", (long)g_we_full);
}

/* called on the game thread when an online (or unknown) battle is detected: the watches never trap online */
void watch_disarm(const char *why) {
    if (!g_nwt || g_w_armed != 1) return;
    g_w_armed = 2;
    int before = g_w_set;
    HANDLE t = CreateThread(NULL, 64 * 1024, arm_proc, (void *)1, 0, NULL);
    if (!t) { LOG("WATCH disarm failed: CreateThread %lu\n", GetLastError()); return; }
    DWORD w = WaitForSingleObject(t, 3000);
    CloseHandle(t);
    LOG("WATCH disarm %s why=%s cleared=%d (they stay off for the rest of the session)\n", w == WAIT_OBJECT_0 ? "done" : "pending", why, before - g_w_set);
}

/* harness check helpers */
long long watch_phase_hits(int dr, int ph) { return dr < g_nwt && ph >= 0 && ph < 8 ? g_w_phase_hits[dr][ph] : -1; }
int watch_armed_threads(void) { return g_w_set; }
