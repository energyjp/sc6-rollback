/* Buffered session log. Lines are appended to an in-memory buffer and written
 * with WriteFile on flush (every N steps, round/battle end, mode switch, crash).
 * WriteFile data survives a process crash (it is in the OS cache). */
#include "common.h"

char g_log_path[MAX_PATH];
static HANDLE g_file = INVALID_HANDLE_VALUE;
static char *g_buf;
static size_t g_cap = 4u << 20, g_len;
static Spin g_lock;
static volatile LONG g_bounded;   /* Option A M1 (m7): at process exit, lock waits are bounded (a dead thread may hold the lock) */
static volatile LONG g_lock_dead; /* bounded mode: one failed wait marks the lock dead, so later lines drop at once (no N x 2M spins) */
void log_set_bounded(int on) { g_bounded = on; }
HANDLE log_handle(void) { return g_file; }
static int log_lock(void) {
    if (!g_bounded) { spin_lock(&g_lock); return 1; }
    if (g_lock_dead) return 0;
    if (spin_trylock(&g_lock, 2000000)) return 1;
    g_lock_dead = 1;
    return 0;
}
#ifdef SC6RB_HARNESS
void log_h_take_lock(void) { spin_lock(&g_lock); }   /* NOBS-LOCK: a thread that dies holding the log lock */
#endif
static LARGE_INTEGER g_freq, g_t0;

uint64_t qpc(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)c.QuadPart; }
double qpc_us(uint64_t t) { return (double)t * 1e6 / (double)g_freq.QuadPart; }
double now_ms(void) { return (double)(qpc() - (uint64_t)g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart; }

static void mkdirs(const char *dir) {
    char tmp[MAX_PATH];
    strncpy(tmp, dir, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    for (char *p = tmp + 3; *p; p++) {
        if (*p == '\\' || *p == '/') { char c = *p; *p = 0; CreateDirectoryA(tmp, NULL); *p = c; }
    }
    CreateDirectoryA(tmp, NULL);
}

void log_open(const char *dir) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_buf = (char *)VirtualAlloc(NULL, g_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_buf) g_cap = 0;
    mkdirs(dir);
    SYSTEMTIME st; GetLocalTime(&st);
    size_t dl = strlen(dir);
    snprintf(g_log_path, sizeof g_log_path, "%s%ssc6rb_%04d%02d%02d_%02d%02d%02d_%lu.log", dir,
             (dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) ? "" : "\\",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    g_file = CreateFileA(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void flush_locked(void) {
    if (g_file != INVALID_HANDLE_VALUE && g_len) {
        DWORD w;
        WriteFile(g_file, g_buf, (DWORD)g_len, &w, NULL);
    }
    g_len = 0;
}

void logf_(const char *fmt, ...) {
    if (!g_buf) return;
    if (!log_lock()) return;   /* bounded mode only: the line is dropped rather than racing a lock holder */
    if (g_len + 8192 > g_cap) flush_locked();   /* v0.19.0 R9: L1CMP lines reach 6000 bytes */
    int n = snprintf(g_buf + g_len, g_cap - g_len, "%.3f ", now_ms());
    if (n > 0) g_len += (size_t)n;
    va_list ap; va_start(ap, fmt);
    n = vsnprintf(g_buf + g_len, g_cap - g_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_len += (size_t)n < g_cap - g_len ? (size_t)n : g_cap - g_len - 1;
    if (g_len && g_buf[g_len - 1] != '\n' && g_len < g_cap) g_buf[g_len++] = '\n';
    spin_unlock(&g_lock);
}

void log_flush(void) {
    if (!g_buf) return;
    if (!log_lock()) return;
    flush_locked();
    spin_unlock(&g_lock);
}
/* v0.31.0: test/overlay_test.c closes its own log before it deletes its temp folder (the plugin never calls it) */
void log_close(void) {
    if (!log_lock()) return;
    flush_locked();
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
    spin_unlock(&g_lock);
}

/* crash path: do not wait forever on a lock held by the faulting thread */
void log_flush_crash(void) {
    if (!g_buf) return;
    int got = spin_trylock(&g_lock, 2000000);
    flush_locked();
    if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
    if (got) spin_unlock(&g_lock);
}
