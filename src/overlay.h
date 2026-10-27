/* v0.31.0 (OPTA-M2-SPEC.md §20.1): the in-game status badge (presentation only).
 *
 * What it draws: one line of text in a built-in 8x8 bitmap font (src/overlay_font.inc), each glyph pixel a square of `scale` screen pixels, a
 * one-font-pixel black outline around the glyphs (readable on any background), NO box over the game: every other pixel of the frame is the
 * game's. How: an inline hook (MinHook) on IDXGISwapChain::Present (dxgi.dll's function, found through the vtable of a throw-away WARP swapchain
 * on a hidden window); inside it, per frame, the back buffer's render-target view is created, ID3D11DeviceContext1::ClearView fills the glyph
 * rectangles (one call per color, an array of rects), and the view and the buffer are released before the game's Present runs: no reference
 * survives Present (ResizeBuffers keeps working), no pipeline state is set or read back (ClearView binds nothing), no shader, no blend.
 *
 * Threads: the render thread (Present) reads the badge text through a seqlock (overlay_publish, one writer: the M1 writer thread's badge tick)
 * and never blocks, never logs: it leaves events (the first frame, a failure, a hotkey press) in volatile fields that overlay_poll (the writer
 * thread) turns into OVERLAY lines. Any failure (no D3D11 device behind the swapchain, no ID3D11DeviceContext1, the back buffer or the view
 * refused, the device removed, the hook not installable) disables the badge for the rest of the launch with ONE line OVERLAY OFF why=<...>;
 * the plugin, the window title and the game run on. The badge writes no game memory and sends nothing. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { BC_WHITE = 0, BC_GREY, BC_GREEN, BC_YELLOW, BC_ORANGE, BC_RED, BC_N };
#define BADGE_SEGS 4
#define BADGE_SEG_LEN 80
typedef struct { char text[BADGE_SEG_LEN]; int color; } BadgeSeg;
typedef struct { int nseg; BadgeSeg seg[BADGE_SEGS]; } BadgeSnap;

/* the plugin state the badge shows (filled by rbo.c on the writer thread from the same globals the window title reads) */
typedef struct {
    int off; const char *off_gate;       /* rbo_off: ROLLBACK PLUGIN OFF: <gate> (q1sig included) */
    int in_battle, online;               /* between the battle's Init and Shutdown; the battle is online (M1's class 2 / PROBE ran) */
    int st;                              /* RboState: 0 passive 1 probe 2 paired 3 armed 4 drained 5 end */
    int armed_ever, desync, asym;
    const char *drained_why, *passive_why;
    int d, rtt_ms;                       /* rtt_ms < 0: unknown */
    unsigned long long rollbacks;
    int regs_bad, lips_fail;
    const char *version;                 /* SC6RB_VERSION's number part ("0.31.0") */
    int at_limit;                        /* v0.33.0 (OPTA-M2-SPEC.md §22.3): armed and >= 10 % of the last 120 fight ticks stopped at the max_rb bound */
} BadgeIn;
/* the text of a state (pure; overlay_test checks every state's string). Returns the number of segments */
int overlay_badge_text(const BadgeIn *in, BadgeSnap *out);

typedef struct { int on, x_pct, y_pct, scale, key; } OverlayCfg;
/* rbonline install (rbo_install, plugin_init): a thread hooks Present. Nothing when cfg->on = 0 (rbonline_badge=0: no hook, no thread) */
void overlay_start(const OverlayCfg *c);
/* the same install, synchronous (the thread above and test/overlay_test.c call it). 1 = hooked; 0 = why says why (OVERLAY OFF) */
int overlay_install_now(const OverlayCfg *c, char *why, size_t cap);
void overlay_uninstall(void);            /* test only: MH_DisableHook + MH_RemoveHook (LIFO with any later hook on the same function) */
void overlay_publish(const BadgeSnap *s);   /* the seqlock writer (one writer thread) */
void overlay_set_hidden(int hidden);     /* the hotkey's toggle, also for the test's negative control */
void overlay_poll(void);                 /* the writer thread: the render thread's events -> OVERLAY lines (never per frame) */
void overlay_last_text(char *out, size_t cap);   /* the last published text (all segments), for the harness and the log */

/* the layout (pure; overlay_test checks it against its own rasterizer). Rect class 0 = the outline (black), 1 + c = color c */
#define OV_NCLASS (1 + BC_N)
#define OV_RCAP 4096
typedef struct { int left, top, right, bottom; } OvRect;
typedef struct { OvRect r[OV_NCLASS][OV_RCAP]; int n[OV_NCLASS]; int x0, y0, scale, cols, rows, chars, overflow; } OvLayout;
int  overlay_scale_for(int height, int scale_cfg);
int  overlay_layout(const BadgeSnap *s, int W, int H, int x_pct, int y_pct, int scale_cfg, OvLayout *L);
const uint8_t *overlay_glyph(int ch);    /* the 8 rows of a glyph (bit 0 = the leftmost pixel); '?' for anything outside 0x20..0x7e */
void overlay_color(int cls, float rgba[4]);   /* the ClearView color of a class (k/255 values: exact in a UNORM8 target) */

typedef struct { unsigned long long frames, drawn, rects, hidden_frames; int installed, off, first_w, first_h, first_fmt; double us_max, us_sum; } OverlayStats;
void overlay_stats(OverlayStats *s);
