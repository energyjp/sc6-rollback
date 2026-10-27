/* v0.31.0 (OPTA-M2-SPEC.md §20.2): the sound cue. See src/cue.h. */
#include "common.h"
#include "cue.h"
#include <math.h>

int cue_next(CueState *cs, uint32_t bat, int kind) {
    if (!cs->have_bat || cs->bat != bat) { cs->have_bat = 1; cs->bat = bat; cs->prev = CUEK_NEUTRAL; cs->played = 0; }
    int cue = CUE_NONE;
    if (kind == CUEK_ROLLBACK && cs->prev != CUEK_ROLLBACK && !(cs->played & 1)) { cue = CUE_ARMED; cs->played |= 1; }
    else if (kind == CUEK_FALLBACK && cs->prev != CUEK_FALLBACK && !(cs->played & 2)) { cue = CUE_FALLBACK; cs->played |= 2; }
    cs->prev = kind;
    return cue;
}

#define CUE_RATE 22050
#define CUE_MAXS (CUE_RATE / 4)
static uint8_t g_wav[2][44 + 2 * CUE_MAXS]; static size_t g_wav_len[2]; static int g_wav_ok;
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static size_t synth(uint8_t *out, const double *hz, const int *ms, int nn) {
    int16_t *s = (int16_t *)(out + 44); size_t n = 0;
    for (int k = 0; k < nn; k++) {
        int len = CUE_RATE * ms[k] / 1000;
        for (int i = 0; i < len && n < CUE_MAXS; i++, n++) {
            double t = (double)i / CUE_RATE, env = (i < CUE_RATE / 200 ? (double)i / (CUE_RATE / 200) : 1.0) * exp(-t * 18.0);   /* 5 ms attack, a quick decay */
            double v = 0.18 * env * (sin(2.0 * 3.14159265358979 * hz[k] * t) + 0.25 * sin(4.0 * 3.14159265358979 * hz[k] * t)) / 1.25;
            s[n] = (int16_t)(v * 32767.0);
        }
    }
    memcpy(out, "RIFF", 4); put32(out + 4, (uint32_t)(36 + 2 * n)); memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16); put16(out + 20, 1); put16(out + 22, 1); put32(out + 24, CUE_RATE); put32(out + 28, CUE_RATE * 2); put16(out + 32, 2); put16(out + 34, 16);
    memcpy(out + 36, "data", 4); put32(out + 40, (uint32_t)(2 * n));
    return 44 + 2 * n;
}
const uint8_t *cue_wav(int cue, size_t *len) {
    if (!g_wav_ok) {
        static const double up[2] = { 660.0, 990.0 }, down[2] = { 523.0, 349.0 };
        static const int up_ms[2] = { 70, 110 }, down_ms[2] = { 110, 110 };
        g_wav_len[0] = synth(g_wav[0], up, up_ms, 2);       /* armed: a rising fifth */
        g_wav_len[1] = synth(g_wav[1], down, down_ms, 2);   /* fallback: a falling fifth */
        g_wav_ok = 1;
    }
    int i = cue == CUE_FALLBACK ? 1 : 0;
    if (len) *len = g_wav_len[i];
    return g_wav[i];
}
typedef BOOL (WINAPI *PlaySoundW_t)(LPCWSTR, HMODULE, DWORD);
double cue_play(int cue) {
    if (cue != CUE_ARMED && cue != CUE_FALLBACK) return -1;
    size_t n; const uint8_t *w = cue_wav(cue, &n); (void)n;
#ifdef SC6RB_HARNESS
    (void)w; return -1;   /* the harness never makes a sound */
#else
    static PlaySoundW_t ps; static int tried;
    if (!tried) {
        tried = 1;
        HMODULE m = GetModuleHandleW(L"winmm.dll");   /* the game imports winmm.dll: already loaded */
        if (!m) m = LoadLibraryW(L"winmm.dll");
        if (m) ps = (PlaySoundW_t)(void *)GetProcAddress(m, "PlaySoundW");
    }
    if (!ps) return -1;
    uint64_t t0 = qpc();
    ps((LPCWSTR)(const void *)w, NULL, 0x0004 /* SND_MEMORY */ | 0x0001 /* SND_ASYNC */ | 0x0002 /* SND_NODEFAULT */);
    return qpc_us(qpc() - t0);
#endif
}
