/* v0.21.0-S1: the presentation class P and the per-PC pointer masks X, in ONE place.
 * Sources: audit/direction/P-fields.md §2 "Mask" and §4, audit/direction/CHECK.md §1-§2, and a design rule
 * (2026-10-04): face, hair and clothing bones never carry hitboxes in SC6; weapons do.
 *
 * P = presentation (lip-sync, face). The plugin hashes P as class F ("effects + presentation, never gameplay"),
 *     keeps it in every snapshot (restored at k0 like everything else) and, with pres_keep=1 and r7_commit=1, puts
 *     the live P back after the resims (lip-sync keeps playing on the committed lineage).
 * X = per-PC pointer values inside PLAYER: excluded from every hash and diff by snapshot masks, still restored.
 *
 * Pure tables and helpers (no game state): shared by the plugin (modes.c, config.c) and the unit test.
 * Offsets are relative to PLAYER[c] = 0x1447156f0 + c * 0x973f0 and half-open [lo, hi). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define PRES_PLAYER0      0x1447156f0ULL
#define PRES_STRIDE       0x973f0ULL
/* CMatrixBankImpl<768> at PLAYER+0x35a0: three arrays of 768 rows (0x40 bytes each), the arrays at PLAYER+0x3610 +
 * b*0xc000 (AUDIT.md E9, verify-approach.md §bones, STEP3-static.md). The bank's row getters (vt 0x143e87638 slots
 * 0x14030b7b0 / 0x14030b790) return [obj+0x28] + i*0x40 and [obj+0x30] + i*0x40: bone i is row i of the selected
 * array (PLAYER+0x35d0 is the second array POINTER, not a row base; an earlier "+0x35d0 base" note was a misread).
 * The core bone-name table 0x143e84f50 has 95 names; 60..94 = MIKEN_R .. BERO2 are the face (names.py, rerun
 * 2026-10-04). Rows 95..97 have no name. v0.21.0-S1 decision (2026-10-04): they are P too. In the C0a and launch C logs they
 * change ONLY as the continuation of face-row runs on voice lines (62/62/28 and 9/9/5 runs touching 95/96/97; 0 runs start
 * in 95..97 in any of the four C0 logs), and face bones never carry hitboxes (the design rule above). */
#define PRES_BANK_BASE    0x3610u
#define PRES_BANK_STRIDE  0xc000u
#define PRES_ROW          0x40u
#define PRES_FACE_ROW_LO  60
#define PRES_FACE_ROW_HI  97      /* inclusive (60..94 named face bones + unnamed face helpers 95..97) */
#define PRES_ROW_OFF(b, r) (PRES_BANK_BASE + (uint32_t)(b) * PRES_BANK_STRIDE + (uint32_t)(r) * PRES_ROW)
/* the known FX scratch the UE listener writes (slots +0x008/+0x080/+0x088; C0 static pass: dead on entry) */
#define PRES_FX_LO        0x14470e9b0ULL
#define PRES_FX_HI        0x14470e9d0ULL   /* 0x14470e9b0..0x14470e9cf */

typedef struct { uint32_t lo, hi; const char *name; } PresRange;
/* v0.24.0-C1b2 F2 (C1B-R2-SPEC.md §2.3): the harness build keeps the P table writable for ONE control switch (h_pres_old); the plugin
 * build's table is const.
 * C1B3: (C1B-R3-SPEC.md §2.4) P is split into KEEP and POSE.
 *   KEEP = k_pres_p (put back by pres_keep; the lip-sync allowlist; PRES_K_P of the L4 guard): step-computed expression / voice state only,
 *          4 ranges per player (face_w, face_layers, lipsync_ctl, voice_ctl), 1,936 bytes (8 ranges, 3,872 bytes for both players).
 *   POSE = pres_pose_ranges(): the world-space face rows 62..96 of the three bank arrays (per-player bank base, pres_bank_off) and face_a /
 *          face_c / face_d. POSE rolls back like G (it is simply not KEEP), never kept; comparisons mask it through the frozen hash class
 *          k_pres_hashfx_v024 (the v0.24 P table byte for byte, shared 0x3610 base: no tier-2 hash changes).
 *   The binding rule: a field step s writes and step s+1 reads may be KEEP only if every reader is presentation code AND it does not encode a
 *   world-space pose derived from G state. Rows 60, 61, 97 are G (no table). */
#ifdef SC6RB_HARNESS
#define PRES_TBL_CONST
void pres_test_table(int which);     /* harness-only h_pres_old: 0 KEEP (default), 1 the v0.23 table (wind in P), 2 the v0.24 table */
void pres_test_old_face(int on);     /* the v0.24 entry point, kept (a no-op since C1B3: pres_test_table selects the table) */
#else
#define PRES_TBL_CONST const
#endif
extern PRES_TBL_CONST PresRange k_pres_p[];   /* KEEP (C1B3), sorted by lo, non-overlapping */
extern PRES_TBL_CONST int k_pres_np;
extern const PresRange k_pres_x[];   /* X, sorted by lo, non-overlapping */
extern const int k_pres_nx;

int    pres_p_index(uint64_t off);   /* index into k_pres_p (KEEP) of the range holding this PLAYER offset, or -1 */
int    pres_is_x(uint64_t off);      /* this PLAYER offset is in an X range */
size_t pres_p_bytes(void);           /* KEEP bytes of ONE player */

/* C1B3: POSE (C1B-R3-SPEC.md §2.4.1). The bank object is constructed at PLAYER+0x35a0 with its arrays at ((obj + 0xbf) & ~0x7f) (ctor
 * 0x14030b7d0: lea rdx,[rcx+0xbf]; and rdx,-0x80): PLAYER0 0x3610, PLAYER1 0x3620 (ASLR moves the image by multiples of 64 KiB, so the
 * offset is base-independent). Row = bone name index + 2: the face is rows 62..96. */
#define PRES_POSE_ROW_LO  62
#define PRES_POSE_ROW_HI  96      /* inclusive */
#define PRES_NPOSE        6
uint32_t pres_bank_off(int c);                    /* PLAYER-relative offset of player c's first bank array */
int      pres_pose_ranges(int c, PresRange out[PRES_NPOSE]);   /* POSE of player c (PLAYER-relative, sorted); returns PRES_NPOSE */
size_t   pres_pose_bytes(int c);                  /* POSE bytes of player c (14,184) */
int      pres_pose_index(int c, uint64_t off);    /* index of the POSE range of player c holding off, or -1 */
/* C1B3: the hash class FX (modes.c class_of_va), frozen at the v0.24 P table byte for byte (7 ranges, shared 0x3610 base) */
extern const PresRange k_pres_hashfx_v024[];
extern const int k_pres_nhashfx;
int      pres_hashfx_index(uint64_t off);

/* L4 guard: the kind of a preferred-base VA. PRES_K_P inside a P range of PLAYER 0 or 1, PRES_K_FX in the FX scratch
 * e9b0, PRES_K_OTHER elsewhere. *run_end (<= end) = the first VA after va where the kind changes, or end. */
enum { PRES_K_OTHER = 0, PRES_K_P = 1, PRES_K_FX = 2 };
int pres_va_kind(uint64_t va, uint64_t end, uint64_t *run_end);

/* Presentation keep-live. b[0..n) are memory blocks, live[] their bytes saved after the live pass (concatenated in
 * block order); memory holds the resim result. Every block whose bytes differ is put back to its live bytes, unless an
 * 8-byte word that differs (8-aligned in memory) holds an image or arena pointer in either version (is_ptr != 0: 1
 * image, 2 arena): then the block keeps the resim bytes and adopt() is called once for it (its first such word).
 * Returns 'A' when any block was adopted, else 'K' when any block was put back, else '-' (all equal). */
typedef struct { uintptr_t addr; uint32_t len; } PresBlk;
typedef int  (*pres_is_ptr_cb)(void *user, uint64_t v);
typedef void (*pres_adopt_cb)(void *user, int block, size_t off, uint64_t live_v, uint64_t resim_v, int kind);
typedef struct { int differ, kept, adopted; size_t bytes; } PresKeep;   /* blocks that differed / were put back / adopted; bytes put back */
char pres_keep_apply(const PresBlk *b, int n, const uint8_t *live, pres_is_ptr_cb is_ptr, void *pu, pres_adopt_cb adopt, void *au, PresKeep *out);

/* v0.21.1 pointer rule (ini pres_ptr_rule), applied to the is_ptr kind before pres_keep_apply sees it.
 * PRES_RULE_ARENA ("arena", default): only an ARENA pointer (an object in the plugin's arena, which the resim lineage may not
 *   have) keeps the resim bytes of its P range. An image pointer (a vtable or function pointer of an inline object, value inside
 *   the exe image) does not: the live bytes are put back. The S1 session showed why: at voice starts UE constructs the inline
 *   lip-sync controller at PLAYER+0x95ed0 (live = a vtable in the image, resim = 0), and adopting the resim threw it away.
 * PRES_RULE_IMAGE_ARENA ("image_arena"): image and arena pointers (the v0.21.0 behavior; the negative control). */
enum { PRES_RULE_ARENA = 0, PRES_RULE_IMAGE_ARENA = 1 };
int         pres_ptr_filter(int kind, int rule);   /* kind 1 image / 2 arena / 0 -> the kind that counts under the rule (0 = none) */
int         pres_rule_parse(const char *s);        /* "arena" -> 0, "image_arena" -> 1 (case-insensitive), anything else -> -1 */
const char *pres_rule_name(int rule);
