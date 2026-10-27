/* v0.31.0 (OPTA-M2-SPEC.md §20.1): the in-game status badge. See src/overlay.h for the design, the threads and the failure rule.
 * Everything here is presentation: no game memory is read or written, nothing is sent, and the simulation never waits on it. */
#define COBJMACROS
#include "common.h"
#include <d3d11_1.h>
#include <dxgi.h>
#include "MinHook.h"
#include "overlay.h"

#include "overlay_font.inc"

/* the interface ids (own copies: no uuid / dxguid import library, no d3d11.dll import in the plugin: d3d11 is found at run time) */
static const GUID k_iid_dev = { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40 } };    /* ID3D11Device */
static const GUID k_iid_tex = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };    /* ID3D11Texture2D */
static const GUID k_iid_ctx1 = { 0xbb2c6faa, 0xb5fb, 0x4082, { 0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1 } };   /* ID3D11DeviceContext1 */

/* ------------------------------------------------------------------ the text of a state (pure) */
static void seg_put(BadgeSnap *o, int color, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seg_put(BadgeSnap *o, int color, const char *fmt, ...) {
    if (o->nseg >= BADGE_SEGS) return;
    va_list ap; va_start(ap, fmt); vsnprintf(o->seg[o->nseg].text, BADGE_SEG_LEN, fmt, ap); va_end(ap);
    o->seg[o->nseg].color = color; o->nseg++;
}
int overlay_badge_text(const BadgeIn *in, BadgeSnap *o) {
    memset(o, 0, sizeof *o);
    const char *ver = in->version ? in->version : "?";
    if (in->off) seg_put(o, BC_RED, "ROLLBACK PLUGIN OFF: %s", in->off_gate && in->off_gate[0] ? in->off_gate : "gate");
    else if (!in->in_battle || !in->online) seg_put(o, BC_WHITE, "sc6rb %s loaded", ver);
    else if (in->desync && in->st >= 3) seg_put(o, BC_RED, "DESYNC - save your logs and report it");
    else if (in->asym) seg_put(o, BC_ORANGE, "FALLBACK: asym - stock netcode for this battle");
    else if (in->st == 3 && in->at_limit) {   /* v0.33.0 (OPTA-M2-SPEC.md §22.3): the rollback window is too short for this ping: this PC waits on the other */
        if (in->rtt_ms >= 0) seg_put(o, BC_YELLOW, "ROLLBACK ON - at limit (waiting on the other PC)  delay %d  ping %d ms", in->d, in->rtt_ms);
        else seg_put(o, BC_YELLOW, "ROLLBACK ON - at limit (waiting on the other PC)  delay %d  ping -", in->d);
    }
    else if (in->st == 3) {
        if (in->rtt_ms >= 0) seg_put(o, BC_GREEN, "ROLLBACK ON  delay %d  ping %d ms  rollbacks %llu", in->d, in->rtt_ms, in->rollbacks);
        else seg_put(o, BC_GREEN, "ROLLBACK ON  delay %d  ping -  rollbacks %llu", in->d, in->rollbacks);
    }
    else if (in->st == 4) seg_put(o, BC_ORANGE, "FALLBACK: %s - stock netcode for this battle", in->drained_why ? in->drained_why : "?");
    else if (in->st == 1 || in->st == 2) seg_put(o, BC_YELLOW, "PAIRING...");
    else if (in->st == 5 && in->armed_ever) {   /* the battle is over: what it was, until the next battle's Init */
        if (in->drained_why) seg_put(o, BC_ORANGE, "FALLBACK: %s - stock netcode for this battle", in->drained_why);
        else seg_put(o, BC_GREEN, "ROLLBACK ON - battle over  rollbacks %llu", in->rollbacks);
    }
    else if (!in->passive_why || !strcmp(in->passive_why, "not_online")) seg_put(o, BC_WHITE, "sc6rb %s loaded", ver);
    else if (!strcmp(in->passive_why, "no_answer")) seg_put(o, BC_GREY, "STOCK NETCODE (opponent not modded)");
    else seg_put(o, BC_GREY, "STOCK NETCODE (%s)", in->passive_why);
    if (in->regs_bad) seg_put(o, BC_RED, "  regs=BAD");
    if (in->lips_fail) seg_put(o, BC_RED, "  lips=FAIL");
    return o->nseg;
}

/* ------------------------------------------------------------------ the layout (pure) */
const uint8_t *overlay_glyph(int ch) { if (ch < 0x20 || ch > 0x7e) ch = '?'; return k_ov_font[ch - 0x20]; }
void overlay_color(int cls, float rgba[4]) {
    static const uint8_t k[OV_NCLASS][3] = { { 0x00, 0x00, 0x00 },   /* the outline */
        { 0xE6, 0xE6, 0xE6 }, { 0xB4, 0xB4, 0xB4 }, { 0x40, 0xE0, 0x40 }, { 0xF0, 0xE0, 0x30 }, { 0xFF, 0x98, 0x20 }, { 0xFF, 0x40, 0x40 } };
    if (cls < 0 || cls >= OV_NCLASS) cls = 0;
    for (int i = 0; i < 3; i++) rgba[i] = (float)k[cls][i] / 255.0f;
    rgba[3] = 1.0f;
}
int overlay_scale_for(int height, int scale_cfg) {
    if (scale_cfg > 0) return scale_cfg > 16 ? 16 : scale_cfg;
    int s = (height + 270) / 540;   /* 720p 1, 1080p 2, 1440p 3, 2160p 4 */
    return s < 1 ? 1 : s;
}
#define OV_MAXCH 100   /* characters per line (longer text is cut) */
#define OV_COLS (OV_MAXCH * 8 + 2)
#define OV_ROWS 10
int overlay_layout(const BadgeSnap *s, int W, int H, int x_pct, int y_pct, int scale_cfg, OvLayout *L) {
    static uint8_t cell[OV_ROWS][OV_COLS];   /* 0 empty, 1 outline, 2 + c fill of color c */
    memset(L->n, 0, sizeof L->n); L->overflow = 0;
    int nch = 0;
    memset(cell, 0, sizeof cell);
    for (int g = 0; g < s->nseg && g < BADGE_SEGS; g++)
        for (const char *p = s->seg[g].text; *p && nch < OV_MAXCH; p++, nch++) {
            const uint8_t *gl = overlay_glyph((unsigned char)*p);
            int col = s->seg[g].color < 0 || s->seg[g].color >= BC_N ? BC_WHITE : s->seg[g].color;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if (gl[y] >> x & 1) cell[1 + y][1 + nch * 8 + x] = (uint8_t)(2 + col);
        }
    int cols = nch * 8 + 2, rows = OV_ROWS;
    for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) {
        if (cell[y][x]) continue;
        int adj = 0;
        for (int dy = -1; dy <= 1 && !adj; dy++) for (int dx = -1; dx <= 1; dx++) {
            int yy = y + dy, xx = x + dx;
            if (yy >= 0 && yy < rows && xx >= 0 && xx < cols && cell[yy][xx] >= 2) { adj = 1; break; }
        }
        if (adj) cell[y][x] = 1;
    }
    int sc = overlay_scale_for(H, scale_cfg);
    int bw = cols * sc, bh = rows * sc;
    int x0 = (int)((long long)W * x_pct / 100) - bw / 2, y0 = (int)((long long)H * y_pct / 100) - bh;
    if (x0 + bw > W) x0 = W - bw;
    if (y0 + bh > H) y0 = H - bh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    L->x0 = x0; L->y0 = y0; L->scale = sc; L->cols = cols; L->rows = rows; L->chars = nch;
    /* runs per row, merged downward with an identical run of the row above (same class, same columns) */
    static int open_idx[OV_NCLASS][OV_COLS];   /* per class: the rect index of an open run starting at column c, or -1 */
    for (int k = 0; k < OV_NCLASS; k++) for (int c = 0; c < cols; c++) open_idx[k][c] = -1;
    static int prev_end[OV_NCLASS][OV_COLS];
    for (int y = 0; y < rows; y++) {
        int x = 0;
        static int cur_start[OV_NCLASS][OV_COLS]; static int cur_n[OV_NCLASS];
        memset(cur_n, 0, sizeof cur_n);
        while (x < cols) {
            int v = cell[y][x];
            if (!v) { x++; continue; }
            int e = x; while (e + 1 < cols && cell[y][e + 1] == v) e++;
            int k = v - 1;   /* class: 0 outline, 1 + c */
            int ri = open_idx[k][x];
            if (ri >= 0 && prev_end[k][x] == e && L->r[k][ri].bottom == y0 + y * sc) L->r[k][ri].bottom += sc;
            else if (L->n[k] < OV_RCAP) {
                ri = L->n[k]++;
                L->r[k][ri].left = x0 + x * sc; L->r[k][ri].top = y0 + y * sc; L->r[k][ri].right = x0 + (e + 1) * sc; L->r[k][ri].bottom = y0 + (y + 1) * sc;
            } else { L->overflow++; ri = -1; }
            cur_start[k][cur_n[k]++] = x;
            open_idx[k][x] = ri; prev_end[k][x] = e;
            x = e + 1;
        }
        /* runs of the row above that did not continue are closed */
        for (int k = 0; k < OV_NCLASS; k++) for (int c = 0; c < cols; c++) {
            if (open_idx[k][c] < 0) continue;
            int still = 0; for (int i = 0; i < cur_n[k]; i++) if (cur_start[k][i] == c) { still = 1; break; }
            if (!still) open_idx[k][c] = -1;
        }
    }
    for (int k = 0; k < OV_NCLASS; k++) {   /* a box larger than the buffer: clipped, and an empty rect dropped (ClearView gets only real rects) */
        int w = 0;
        for (int i = 0; i < L->n[k]; i++) {
            OvRect r = L->r[k][i];
            if (r.right > W) r.right = W;
            if (r.bottom > H) r.bottom = H;
            if (r.left < r.right && r.top < r.bottom) L->r[k][w++] = r;
        }
        L->n[k] = w;
    }
    return nch;
}

/* ------------------------------------------------------------------ the seqlock (one writer: the badge tick) */
static volatile LONG g_sq;
static BadgeSnap g_sb;
static Spin g_pub;   /* writers only (the writer thread's tick; an install failure on the DllMain thread): the reader never takes it */
void overlay_publish(const BadgeSnap *s) {
    spin_lock(&g_pub);
    InterlockedIncrement(&g_sq);   /* odd: a write in progress (a full barrier) */
    memcpy(&g_sb, s, sizeof g_sb);
    InterlockedIncrement(&g_sq);   /* even: stable */
    spin_unlock(&g_pub);
}
static int snap_read(BadgeSnap *out, LONG *ver) {
    for (int i = 0; i < 4; i++) {
        LONG a = *(volatile LONG *)&g_sq;
        _ReadWriteBarrier();
        if (a & 1) { YieldProcessor(); continue; }
        memcpy(out, (const void *)&g_sb, sizeof *out);
        _ReadWriteBarrier();
        LONG b = *(volatile LONG *)&g_sq;
        if (a == b) { *ver = a; return 1; }
    }
    return 0;   /* the writer was busy every time: the caller keeps its last good copy (never waits) */
}
void overlay_last_text(char *out, size_t cap) {
    BadgeSnap s; LONG v;
    if (!snap_read(&s, &v)) { snprintf(out, cap, "?"); return; }
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < s.nseg && o + 1 < cap; i++) { int w = snprintf(out + o, cap - o, "%s", s.seg[i].text); if (w > 0) o += (size_t)w; }
}

/* ------------------------------------------------------------------ the render thread */
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *sc, UINT sync, UINT flags);
static PresentFn o_present;
static void *g_present_target;
static OverlayCfg g_ocfg;
static volatile LONG g_off;              /* the badge is off for the rest of the launch */
static volatile LONG g_hidden;           /* the hotkey's toggle */
static volatile LONG g_installed;
/* events for overlay_poll (written by the render thread only, read by the writer thread) */
static volatile LONG g_ev_first, g_ev_off_logged, g_ev_hot, g_ev_resize;
static char g_off_why[96];
static volatile LONG g_first_w, g_first_h, g_first_fmt, g_cur_w, g_cur_h;
static OverlayStats g_st;
static OvLayout g_lay;                   /* render thread only */
static LONG g_lay_ver = -1; static int g_lay_w, g_lay_h;
static BadgeSnap g_rs_snap; static LONG g_rs_ver = -1;
static int g_key_down;

static void ov_fail(const char *why, HRESULT hr) {   /* render thread: off for the launch; the line comes from overlay_poll */
    if (InterlockedCompareExchange(&g_off, 1, 0) != 0) return;
    if (hr) snprintf(g_off_why, sizeof g_off_why, "%s hr=0x%08lx", why, (unsigned long)hr); else snprintf(g_off_why, sizeof g_off_why, "%s", why);
    g_st.off = 1;
}
static void ov_hotkey(HWND out_wnd) {
    int vk = g_ocfg.key;
    if (vk <= 0 || vk > 255) return;
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (down && !g_key_down) {
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (fg && (fg == out_wnd || pid == GetCurrentProcessId())) { InterlockedExchange(&g_hidden, !g_hidden); InterlockedIncrement(&g_ev_hot); }
    }
    g_key_down = down;
}
static void ov_draw(IDXGISwapChain *sc) {
    uint64_t t0 = qpc();
    g_st.frames++;
    DXGI_SWAP_CHAIN_DESC sd; memset(&sd, 0, sizeof sd);
    if (FAILED(IDXGISwapChain_GetDesc(sc, &sd))) return;
    ov_hotkey(sd.OutputWindow);
    if (g_hidden) { g_st.hidden_frames++; return; }
    if (sd.OutputWindow && GetWindow(sd.OutputWindow, GW_OWNER)) return;   /* an owned (tool) window: not the game's main swapchain */
    ID3D11Device *dev = NULL;
    HRESULT hr = IDXGISwapChain_GetDevice(sc, &k_iid_dev, (void **)&dev);
    if (FAILED(hr) || !dev) { ov_fail("not_d3d11", hr); return; }
    hr = ID3D11Device_GetDeviceRemovedReason(dev);
    if (hr != S_OK) { ID3D11Device_Release(dev); ov_fail("device_removed", hr); return; }
    ID3D11Texture2D *tex = NULL;
    hr = IDXGISwapChain_GetBuffer(sc, 0, &k_iid_tex, (void **)&tex);
    if (FAILED(hr) || !tex) { ID3D11Device_Release(dev); ov_fail("getbuffer", hr); return; }
    D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(tex, &td);
    if (td.Width < 160 || td.Height < 120) { ID3D11Texture2D_Release(tex); ID3D11Device_Release(dev); return; }   /* a tiny swapchain: not the game's */
    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv);
    if (FAILED(hr) || !rtv) { ID3D11Texture2D_Release(tex); ID3D11Device_Release(dev); ov_fail("rtv", hr); return; }
    ID3D11DeviceContext *ctx = NULL; ID3D11DeviceContext1 *ctx1 = NULL;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    if (ctx) hr = ID3D11DeviceContext_QueryInterface(ctx, &k_iid_ctx1, (void **)&ctx1);
    if (!ctx || FAILED(hr) || !ctx1) {
        if (ctx) ID3D11DeviceContext_Release(ctx);
        ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(tex); ID3D11Device_Release(dev);
        ov_fail("no_d3d11_1", hr); return;
    }
    {   BadgeSnap s; LONG v;
        if (snap_read(&s, &v) && v != g_rs_ver) { g_rs_snap = s; g_rs_ver = v; } }
    if (g_rs_ver != g_lay_ver || (int)td.Width != g_lay_w || (int)td.Height != g_lay_h) {
        overlay_layout(&g_rs_snap, (int)td.Width, (int)td.Height, g_ocfg.x_pct, g_ocfg.y_pct, g_ocfg.scale, &g_lay);
        if (g_lay_w && ((int)td.Width != g_lay_w || (int)td.Height != g_lay_h)) InterlockedIncrement(&g_ev_resize);
        g_lay_ver = g_rs_ver; g_lay_w = (int)td.Width; g_lay_h = (int)td.Height;
        InterlockedExchange(&g_cur_w, (LONG)td.Width); InterlockedExchange(&g_cur_h, (LONG)td.Height);
    }
    unsigned long long nr = 0;
    for (int k = 0; k < OV_NCLASS; k++) {
        if (!g_lay.n[k]) continue;
        float c[4]; overlay_color(k, c);
        ID3D11DeviceContext1_ClearView(ctx1, (ID3D11View *)rtv, c, (const D3D11_RECT *)g_lay.r[k], (UINT)g_lay.n[k]);
        nr += (unsigned long long)g_lay.n[k];
    }
    ID3D11DeviceContext1_Release(ctx1); ID3D11DeviceContext_Release(ctx);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(tex); ID3D11Device_Release(dev);
    g_st.drawn++; g_st.rects += nr;
    if (!g_ev_first) { g_first_w = (LONG)td.Width; g_first_h = (LONG)td.Height; g_first_fmt = (LONG)td.Format; InterlockedExchange(&g_ev_first, 1); }
    double us = qpc_us(qpc() - t0);
    g_st.us_sum += us; if (us > g_st.us_max) g_st.us_max = us;
}
static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (!g_off && !(flags & DXGI_PRESENT_TEST)) ov_draw(sc);
    return o_present(sc, sync, flags);
}

/* ------------------------------------------------------------------ install */
typedef HRESULT (WINAPI *CreateDevSc_t)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
                                        IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static LRESULT CALLBACK ov_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static int g_mh_init;
int overlay_install_now(const OverlayCfg *c, char *why, size_t cap) {
    g_ocfg = *c;
    if (g_installed) return 1;
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    if (!d3d) d3d = LoadLibraryW(L"d3d11.dll");
    CreateDevSc_t create = d3d ? (CreateDevSc_t)(void *)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!create) { snprintf(why, cap, "no_d3d11"); return 0; }
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = ov_wndproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"sc6rb_badge_probe";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"sc6rb", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    if (!hw) { snprintf(why, cap, "probe_window err=%lu", GetLastError()); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 0; }
    DXGI_SWAP_CHAIN_DESC d; memset(&d, 0, sizeof d);
    d.BufferCount = 1; d.BufferDesc.Width = 64; d.BufferDesc.Height = 64; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow = hw; d.SampleDesc.Count = 1; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *sc = NULL; ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d, &sc, &dev, NULL, &ctx);   /* WARP: the GPU is never touched */
    if (FAILED(hr)) hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d, &sc, &dev, NULL, &ctx);
    void *target = NULL;
    if (SUCCEEDED(hr) && sc) target = (*(void ***)sc)[8];   /* IDXGISwapChain::Present: dxgi.dll's, the same function for every swapchain */
    if (sc) IDXGISwapChain_Release(sc);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    DestroyWindow(hw); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (!target) { snprintf(why, cap, "probe_swapchain hr=0x%08lx", (unsigned long)hr); return 0; }
    if (!g_mh_init) { MH_STATUS s = MH_Initialize(); if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { snprintf(why, cap, "minhook_init=%d", (int)s); return 0; } g_mh_init = 1; }
    MH_STATUS s = MH_CreateHook(target, (void *)hk_present, (void **)&o_present);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) { MH_RemoveHook(target); snprintf(why, cap, "minhook=%d (%s)", (int)s, MH_StatusToString(s)); return 0; }
    g_present_target = target;
    g_off = 0; g_ev_off_logged = 0; g_off_why[0] = 0;
    InterlockedExchange(&g_installed, 1); g_st.installed = 1;
    HMODULE m = NULL; char mn[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)target, &m) && m) {
        GetModuleFileNameA(m, mn, sizeof mn); const char *b = strrchr(mn, '\\'); if (b) memmove(mn, b + 1, strlen(b));
    }
    snprintf(why, cap, "present=%s+0x%llx", mn, (unsigned long long)((uintptr_t)target - (uintptr_t)m));
    return 1;
}
void overlay_uninstall(void) {
    if (!g_installed || !g_present_target) return;
    MH_DisableHook(g_present_target); MH_RemoveHook(g_present_target);
    g_present_target = NULL; o_present = NULL;
    InterlockedExchange(&g_installed, 0); g_st.installed = 0;
    g_lay_ver = -1; g_lay_w = g_lay_h = 0; g_rs_ver = -1; g_key_down = 0;
    InterlockedExchange(&g_ev_first, 0);
}
void overlay_set_hidden(int hidden) { InterlockedExchange(&g_hidden, hidden ? 1 : 0); }
void overlay_stats(OverlayStats *s) { *s = g_st; s->installed = g_installed; s->off = g_off; s->first_w = g_first_w; s->first_h = g_first_h; s->first_fmt = g_first_fmt; }

static DWORD WINAPI ov_thread(LPVOID p) {
    (void)p;
    char why[160];
    uint64_t t0 = qpc();
    int ok = overlay_install_now(&g_ocfg, why, sizeof why);
    if (ok) LOG("OVERLAY HOOK %s minhook=0 install_ms=%.1f x=%d y=%d scale=%d key=0x%x (the badge draws from the next frame)\n", why, qpc_us(qpc() - t0) / 1000.0, g_ocfg.x_pct, g_ocfg.y_pct,
                g_ocfg.scale, g_ocfg.key);
    else { InterlockedExchange(&g_off, 1); InterlockedExchange(&g_ev_off_logged, 1); LOG("OVERLAY OFF why=%s (the badge is off for this launch; the plugin and the window title run on)\n", why); }
    return 0;
}
void overlay_start(const OverlayCfg *c) {
    g_ocfg = *c;
    if (!c->on) { LOG("OVERLAY disabled by the ini (rbonline_badge=0): no hook\n"); return; }
    HANDLE h = CreateThread(NULL, 0, ov_thread, NULL, 0, NULL);
    if (h) CloseHandle(h); else LOG("OVERLAY OFF why=thread err=%lu (the badge is off for this launch)\n", GetLastError());
}

/* the writer thread: the render thread's events -> OVERLAY lines (never per frame) */
void overlay_poll(void) {
    static int first_logged; static LONG hot_seen, resize_seen; static uint64_t t_inst; static int nop_logged;
    if (g_ev_first && !first_logged) {
        first_logged = 1;
        LOG("OVERLAY ON first_frame w=%ld h=%ld fmt=%ld scale=%d (drawn with ClearView after the game's frame)\n", (long)g_first_w, (long)g_first_h, (long)g_first_fmt,
            overlay_scale_for((int)g_first_h, g_ocfg.scale));
    }
    if (g_off && !g_ev_off_logged) { InterlockedExchange(&g_ev_off_logged, 1); LOG("OVERLAY OFF why=%s (the badge is off for this launch; the plugin and the window title run on)\n", g_off_why); }
    LONG hk = g_ev_hot; if (hk != hot_seen) { hot_seen = hk; LOG("OVERLAY hotkey hidden=%ld (presses=%ld)\n", (long)g_hidden, (long)hk); }
    LONG rz = g_ev_resize; if (rz != resize_seen) { resize_seen = rz; LOG("OVERLAY resize w=%ld h=%ld scale=%d (the back buffer changed size)\n", (long)g_cur_w, (long)g_cur_h, overlay_scale_for((int)g_cur_h, g_ocfg.scale)); }
    if (g_installed && !t_inst) t_inst = qpc();
    if (g_installed && !g_ev_first && !g_off && !nop_logged && t_inst && qpc_us(qpc() - t_inst) > 120e6) {   /* 2 minutes hooked and not one frame drawn */
        nop_logged = 1;
        LOG("OVERLAY OFF why=no_present (the hooked IDXGISwapChain::Present has not run in 2 minutes: the game presents another way; the title still shows the state)\n");
    }
}
