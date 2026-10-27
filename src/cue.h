/* v0.31.0 (OPTA-M2-SPEC.md §20.2): the sound cue (presentation only). A short, quiet chime when this battle switches to rollback (armed) and a
 * different one when it leaves rollback for a fallback (drained / ASYM / DESYNC / OFF), at most once per battle per kind; nothing for passive,
 * probe or paired. Synthesized in code into an in-memory WAV (no sound file, nothing downloaded), played with PlaySoundW(SND_MEMORY | SND_ASYNC)
 * from the M1 writer thread (never the game thread). rbonline_sound=0 turns it off. Harness builds never play (the CUE line says so). */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { CUE_NONE = 0, CUE_ARMED = 1, CUE_FALLBACK = 2 };
enum { CUEK_NEUTRAL = 0, CUEK_ROLLBACK = 1, CUEK_FALLBACK = 2 };   /* what the badge state is, for the cue */
typedef struct { uint32_t bat; int have_bat, prev, played; } CueState;
/* the transition rule (pure): kind = the state now (CUEK_*), bat = the battle index. Returns CUE_ARMED on an entry into rollback, CUE_FALLBACK on an
 * entry into a fallback, each at most once per battle; a new battle index starts afresh */
int cue_next(CueState *cs, uint32_t bat, int kind);
/* the WAV of a cue (built once; valid for the process): 22,050 Hz, 16-bit mono, peak 0.18 of full scale, < 0.25 s */
const uint8_t *cue_wav(int cue, size_t *len);
/* play it (writer thread). Returns the call's cost in microseconds (-1 = not played: harness build or no winmm) */
double cue_play(int cue);
