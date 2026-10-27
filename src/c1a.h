/* v0.22.0-C1a (GATE-C-DESIGN §8 C1a, §5.1; audit/c1a/RECON.md): pure helpers for the cross-launch determinism recorder.
 * No game state, no log: shared by the plugin (c1a_run.c) and the unit test.
 *
 * Pointer normalization (design §5.1, review R15). One 8-byte word at an 8-aligned VA is replaced by a launch-independent
 * value when it points into a known range, checked in this order:
 *   arena  [arena_lo, arena_hi)          -> C1A_TAG_ARENA  | offset
 *   pin    a pinned heap block (sorted)  -> C1A_TAG_PIN    | len << 32 | offset   (the pin is named by its LENGTH: pin order
 *                                                                                  follows heap addresses, which differ per launch)
 *   image  [img_lo, img_hi)              -> C1A_TAG_IMAGE  | RVA
 *   region a non-image snapshot region   -> C1A_TAG_REGION | region index << 40 | offset  (WorkMemory, the plugin extras)
 *   stack  [stk_lo, stk_hi) (game thread)-> C1A_TAG_STACK  (offset dropped)
 *   else   raw. With heap_mode=1 a raw value that readable() calls a committed address becomes C1A_TAG_HEAP (value dropped):
 *          used for the tier-1 pointer blocks and the setup hash; v0.23.0-C1b: also for tier-2 with c1a_t2_heap=tag (the default;
 *          header t2rule=heaptag), since every C1a tier-2 difference across launches was a raw heap pointer. c1a_t2_heap=raw = the
 *          v0.22 rule ("else raw", header t2rule=raw or no key), a control.
 * kind codes (dump kind map): C1A_K_*. C1A_K_HEAP is reported whenever readable() is given and says yes. */
#pragma once
#include <stdint.h>
#include <stddef.h>
struct C1aView_;
typedef struct C1aView_ C1aView_;

#define C1A_FORMAT_VERSION 1
#define C1A_T2_EVERY       32
#define C1A_NCOMP          25

enum { C1A_K_RAW = 0, C1A_K_ARENA = 1, C1A_K_REGION = 2, C1A_K_PIN = 3, C1A_K_IMAGE = 4, C1A_K_STACK = 5, C1A_K_HEAP = 6 };
#define C1A_TAG_ARENA  0xA000000000000000ULL
#define C1A_TAG_REGION 0xB000000000000000ULL
#define C1A_TAG_PIN    0xC000000000000000ULL
#define C1A_TAG_IMAGE  0xD000000000000000ULL
#define C1A_TAG_STACK  0xE000000000000000ULL
#define C1A_TAG_HEAP   0xF000000000000000ULL

typedef struct { uintptr_t addr; uint32_t len; } C1aPin;            /* same layout as SPin */
typedef struct { uintptr_t lo; uint64_t len; int idx; } C1aRegion;   /* idx = snapshot region index */
typedef struct {
    uintptr_t arena_lo, arena_hi;
    const C1aPin *pins; int npin;      /* sorted by addr (snapset order) */
    uintptr_t img_lo, img_hi;
    const C1aRegion *regs; int nreg;   /* non-image snapshot regions */
    uintptr_t stk_lo, stk_hi;
    int (*readable)(uintptr_t v);      /* committed-address test for C1A_K_HEAP (NULL = never) */
    int heap_mode;                     /* 1: C1A_K_HEAP values become C1A_TAG_HEAP */
} C1aNorm;

uint64_t c1a_norm_qword(const C1aNorm *n, uint64_t v, int *kind);
/* In place over buf (a copy of memory at preferred-base-independent address va): every 8-byte word at an 8-aligned VA that lies
 * wholly inside [va, va+len) is normalized. kinds (may be NULL) gets one code per 8-byte unit [8i, 8i+8) of buf; units that are
 * not 8-aligned in VA or not whole are C1A_K_RAW. Returns the number of words changed. */
size_t   c1a_norm_buf(const C1aNorm *n, uint8_t *buf, uintptr_t va, size_t len, uint8_t *kinds);

/* v0.24.0-C1b2 F1 (C1B-R2-SPEC.md §2.2): the tier-1 pointer MAP rule for the ai / ctl blocks. offs = the mapped 8-byte units (offsets from
 * va, 8-aligned in VA, sorted). An unmapped unit stays RAW (kind 0) whatever it holds; a mapped unit holding 0 stays raw 0; a mapped unit
 * holding an arena / pin / image / region / stack pointer is normalized as c1a_norm_qword does; any other mapped nonzero value becomes
 * C1A_TAG_HEAP (kind 6) WITHOUT calling readable(). The v0.23 rule (heaptag: every committed value is a heap tag) is the control.
 * Returns the number of units changed. */
size_t   c1a_norm_buf_map(const C1aNorm *n, uint8_t *buf, uintptr_t va, size_t len, const uint32_t *offs, int noffs, uint8_t *kinds);
#define C1A_T1_AI     0x14470f390ULL     /* the tier-1 ai block (both CPU AI blocks, side * 0x3038) */
#define C1A_T1_AI_LEN 0x6070u
#define C1A_T1_CTL    0x144715400ULL     /* the tier-1 ctl block (= C1A_T1_AI + C1A_T1_AI_LEN) */
#define C1A_T1_CTL_LEN 0xc0u
#define C1A_T1_NAI    32
#define C1A_T1_NCTL   6
extern const uint32_t k_t1_ai_ptr[C1A_T1_NAI];     /* the 16 pointer units of a side (div/ptrmap.py census), side 1 at +0x3038 */
extern const uint32_t k_t1_ctl_ptr[C1A_T1_NCTL];
uint64_t c1a_t1map_hash(void);                     /* FNV-1a 64 over both offset lists (u32 LE, ai first): the header t1map= */
int      c1a_t1_mapped(const uint32_t *offs, int noffs, uint32_t off);
/* the tier-1 ai / ctl component from a dump view (offline: test/c1b_offline.c t1map_rec). rule 0 = map, 1 = heaptag. The dump holds the
 * tier-2 rule (arena/pin/image/region/stack tags applied, heap values raw with kind 6): heaptag = kind 6 -> C1A_TAG_HEAP; map = mapped
 * units as above, unmapped units raw (*exact = 0 when an unmapped unit is tagged in the dump: its raw value is gone). */
uint64_t c1a_t1_view_hash(const C1aView_ *v, const uint32_t *offs, int noffs, int rule, uint64_t seed, int *exact);

/* the tier-1 components, in record order (names as written in the F lines) */
extern const char *const k_c1a_comp[C1A_NCOMP];

/* the input the step got (StepIn: p1, p2 words, the 0x18-byte third block; ff bits 1 p1, 2 p2, 4 third block present) */
typedef struct { uint64_t p1, p2; uint8_t x[0x18]; uint8_t ff; } C1aIn;

#define C1A_IFC_CAP 160
typedef struct {
    uint32_t f, fr;          /* battle step (1 = the battle's first step), the core frame counter after the step */
    C1aIn in;
    uint8_t rp, pt;          /* game replay flag 0x1448463f8 after the step; 1 = vanilla passthrough step (outside the Tick) */
    uint64_t t1;
    uint64_t c[C1A_NCOMP];
    char ifc[C1A_IFC_CAP];   /* UE->core non-query IBattleCore calls since the previous step: "e8:1,88:1" or "-" */
    uint32_t q;              /* query/factory calls since the previous step (not compared) */
} C1aFrame;

/* "F <f> fr=... ifc=... q=...\n"; returns the length (0 if it did not fit) */
size_t c1a_format_frame(char *out, size_t cap, const C1aFrame *fr);
/* 1 = parsed an F line; 0 = not an F line or malformed (every field is required) */
int    c1a_parse_frame(const char *line, C1aFrame *fr);

/* tier-2 cells: key = class char << 24 | (preferred-base VA >> 16) (24 bits), written "G14471=<16 hex>" */
typedef struct { uint32_t key; uint64_t h; } C1aCell;
#define C1A_MAXCELL 128
size_t c1a_format_t2(char *out, size_t cap, uint32_t f, const C1aCell *c, int n);
/* v0.32.0 (OPTA-M2-SPEC.md §21.6, D-B): the raw clash / aif bytes of both players, written into the .rec / .play as an A line whenever they
 * change (c1a_clash_bytes=1). Spans per player (FINDINGS3 §1.1): the aif spans +0x16d8 (0x28) and +0x4412a (8), and the clash records
 * +0x1968..+0x1b38 (0x1d0: the u16 vars from +0x197c, var 0x40 = +0x19fc, var 0x41 = +0x19fe; the aif span +0x19a0 (0x60) lies inside).
 * The A line: "A <f> p<i>+<off>=<hex> ..." with one item per run of changed 8-byte units (offsets absolute from the PLAYER), every unit of
 * every span when prev is NULL (the battle's first frame). Readers that do not know the tag skip it (ckcmp: other_tags; the plugin's own
 * .rec reader: F / T2 / D 0 only). */
#define C1A_AB_NSPAN 3
#define C1A_AB_LEN   0x200
typedef struct { uint32_t off, len, at; } C1aAbSpan;   /* PLAYER offset, bytes, offset in the per-player buffer */
extern const C1aAbSpan k_c1a_ab[C1A_AB_NSPAN];
size_t c1a_format_ab(char *out, size_t cap, uint32_t f, const uint8_t cur[2][C1A_AB_LEN], const uint8_t prev[2][C1A_AB_LEN]);   /* 0 = nothing changed */
int    c1a_parse_t2(const char *line, uint32_t *f, C1aCell *c, int max, int *n);
void   c1a_cell_name(uint32_t key, char out[16]);

/* value of " key=" (or "key=" at the line start) up to the next space or end of line; 1 = found */
int    c1a_kv(const char *line, const char *key, char *out, size_t cap);
uint64_t c1a_kv_u64(const char *line, const char *key, int *ok);   /* decimal, or hex with 0x */

/* setup categories (one hash each); the default refusal set is every category except catch, uecam and wind.
 * v0.22.1: + wind (the wind manager 0x144100b60, information only; a v0.22.0 recording has no s_wind: compared from its f0 dump) */
enum { C1A_S_EXE = 0, C1A_S_PLUGIN, C1A_S_SIMCFG, C1A_S_RULES, C1A_S_FLAGS, C1A_S_PL0, C1A_S_PL1, C1A_S_CTL, C1A_S_AI, C1A_S_STAGE,
       C1A_S_ASSETS, C1A_S_CATCH, C1A_S_UECAM, C1A_S_WIND, C1A_S_CONTENT /* C1B3: (C1B-R3-SPEC.md §2.1.1) */, C1A_NSETUP };
extern const char *const k_c1a_setup[C1A_NSETUP];
#define C1A_REFUSE_DEFAULT ((1u << C1A_S_CATCH) - 1u)   /* exe .. assets */
/* "exe,ai,..." | "none" | "default" | "" (default) -> bit mask; *bad = 1 when a name is unknown (ignored) */
uint32_t c1a_refuse_parse(const char *s, int *bad);
size_t   c1a_refuse_str(uint32_t mask, char *out, size_t cap);

/* ================================================================== v0.22.1-C1a setup injection (C1A-AUDIT.md C1-C6)
 * The one non-pointer setup difference of the two real launches (C1A-AUDIT §1, §4) is FLuxBattlePlayerParam.CpuPersonality, a UE input
 * (like the character choice) that the core copies and derives from:
 *   D1 SetMatchRule per-player block +0x10  0x14470f1d0 + side*0x58 + 0x10          (FUN_1402dafe0, +0x48)
 *   D4 Setup's rule copy +0x40              0x144843ef0 + 0x40 + side*0x58, <= 99    (FUN_1402fa9f0(0x144843ef0, 0x14470f1d0), +0x60)
 *   D3 AI block +0x2c0c                     0x14470f390 + side*0x3038 + 0x2c0c       (PlayerInfoHandler +0x1f8 = FUN_1402d4af0)
 *   D2 AI block +0x38..+0x987               the personality tables (FUN_140394fe0 via [AI+0xbb0] = AI+0x38) and the PSNL entry +
 *                                           mirror (FUN_140364bc0: +0x948..+0x987): a pure function of (personality, AI data)
 * mode=replay with c1a_setup_inject=rules,ai: at the first step the live bytes of three ranges are compared with the recording's
 * battle-start dump (_f0.dump); ONLY the personality-derived bytes of a side whose recorded personality differs may differ (C2), and
 * only those are written (the recording's values; pointer units never). Anything else that differs refuses (SETUP_MISMATCH). */
#define C1A_INJ_RULES   0x14470f1d0ULL   /* SetMatchRule output .. 0x14470f30b (the rules hash range) */
#define C1A_INJ_RULESTR 0x58ULL
#define C1A_INJ_PERS    0x10             /* CpuPersonality in a per-player rule block */
#define C1A_INJ_GAME    0x144843ef0ULL   /* CBattleGameClass: Setup's clamped rule copy at +0x30.. (per player 0x58) */
#define C1A_INJ_PCOPY   0x40             /* the personality in the copy (side 0 +0x40, side 1 +0x98) */
#define C1A_INJ_AI      0x14470f390ULL   /* the CPU AI blocks, side*0x3038 */
#define C1A_INJ_AISTR   0x3038ULL
#define C1A_INJ_PAI     0x2c0c           /* the personality in the AI block */
#define C1A_INJ_TAB_LO  0x38             /* derived tables + PSNL entry + mirror: AI +0x38 .. +0x987 */
#define C1A_INJ_TAB_HI  0x988
/* v0.22.2: the CPU difficulty (C1a session-2 retry refusal 2026-10-05: rule_player1+0x8/+0xc 02/03 vs 01/01, the copy +0x90/+0x94,
 * AI1 +0x1a28/+0x2bf8/+0x2c00): FLuxBattlePlayerParam.CpuRankLevel/CpuWazaLevel (payload +4/+8, C1A-AUDIT.md) at rule block +0x8/+0xc,
 * the copy 8/4 bytes before the personality, the AI block +0x2bf8 (rank) and +0x2c00/+0x1a28 (waza, clamped 0..12 by PlayerInfoHandler +0x1e8) */
#define C1A_INJ_RRANK   0x8
#define C1A_INJ_RWAZA   0xc
#define C1A_INJ_ARANK   0x2bf8
#define C1A_INJ_AWAZA   0x2c00
#define C1A_INJ_ALVL    0x1a28
#define C1A_DEMO_SEED   0x765u           /* the default rule ctor's seed (FUN_1402dcf30): the title/menu demo Init (C4) */
enum { C1A_R_RULES = 0, C1A_R_COPY, C1A_R_AI, C1A_NRANGE };
typedef struct { uint64_t lo, hi; const char *name; int cat; } C1aRange;   /* [lo, hi), 8-aligned; cat = C1A_S_RULES | C1A_S_AI */
extern const C1aRange k_c1a_range[C1A_NRANGE];
#define C1A_INJECT_DEFAULT ((1u << C1A_S_RULES) | (1u << C1A_S_AI))
/* c1a_setup_inject= "rules,ai" (default) | "none" | a subset; any other name sets *bad (ignored) */
uint32_t c1a_inject_parse(const char *s, int *bad);

/* a C1ADUMP1 file (c1a_run.c dump_write); the sections point into the caller's buffer */
typedef struct { uint64_t va; uint32_t len; char name[17]; const uint8_t *data, *cls, *kind; } C1aDumpSec;
typedef struct { uint32_t version, frame, battle; int nsect; uint64_t module_base, arena_base, flags; C1aDumpSec s[8]; } C1aDump;
int c1a_dump_parse(const uint8_t *buf, size_t n, C1aDump *d, const char **why);   /* 1 = ok; *why = the first problem */
/* bytes and per-8-byte-unit kinds of [va, va+len) (va 8-aligned); 1 = one section covers it */
struct C1aView_ { uint64_t lo; size_t len; const uint8_t *b, *k; };
typedef struct C1aView_ C1aView;
int c1a_dump_view(const C1aDump *d, uint64_t va, size_t len, C1aView *v);

/* the injection plan: rec = the recording's f0 views, now = this launch's normalized views (tier-2 rule + kinds), per range */
#define C1A_INJ_MAXSPAN 1024
typedef struct { uint64_t va; uint32_t len; uint8_t range, bad, ptr; } C1aSpan;
enum { C1A_INJ_EQUAL = 0, C1A_INJ_OK = 1, C1A_INJ_REFUSE = 2 };
typedef struct {
    int verdict;
    const char *why;                       /* OK: cpu_setup; REFUSE: personality_copies_disagree | cpu_level_copies_disagree | not_cpu_setup | pointer_differs */
    int32_t pers_rec[2][3], pers_now[2][3]; /* [side][rule, copy, ai] */
    int32_t lvl_rec[2][7], lvl_now[2][7];   /* v0.22.2 [side][rule rank, rule waza, copy rank, copy waza, ai rank +0x2bf8, ai waza +0x2c00, ai +0x1a28] */
    int side_ovr[2];                       /* this side's recorded CPU setup (personality or difficulty) differs: its CPU-setup bytes may be written */
    int lvl_cons_rec, lvl_cons_now;        /* v0.22.2: the difficulty copies agree in every overridden side */
    int side_pd[2], side_ld[2];            /* v0.22.2: per side, the personality / the difficulty differs (each group is writable only then) */
    int cons_rec, cons_now;                /* the three copies agree (copy = min(rule, 99)) in every side */
    uint32_t diff_bytes, wr_bytes[C1A_NRANGE], wr_spans[C1A_NRANGE], bad_bytes, bad_spans, ptr_bytes;
    int nspan; C1aSpan span[C1A_INJ_MAXSPAN];   /* write and bad spans in VA order (stored up to the cap; the counts are exact) */
} C1aPlan;
void c1a_inj_plan(const C1aView rec[C1A_NRANGE], const C1aView now[C1A_NRANGE], uint32_t inject, C1aPlan *p);
/* writes the planned bytes (the recording's) into dst[r] (the base of range r: live memory or a copy); returns the bytes written */
uint32_t c1a_inj_apply(const C1aView rec[C1A_NRANGE], const C1aView now[C1A_NRANGE], const C1aPlan *p, uint8_t *const dst[C1A_NRANGE]);

/* the setup hashes the plugin computes on live memory (c1a_run.c setup_compute, heap pointers dropped), recomputed from views of
 * dump-normalized bytes: rules from the C1A_R_RULES view, ai from the C1A_R_AI view. *exact = 0 when a partial unit is not RAW. */
uint64_t c1a_hash_rules_view(const C1aView *r, int *exact);
uint64_t c1a_hash_ai_view(const C1aView *ai, int *exact);
/* v0.24.0-C1b2 (offline setup_rehash): c1a_run.c nh() with heap_mode=1 on [va, va+len) of a dump view, any alignment (whole 8-aligned units
 * normalized; a partial unit raw, exact only if RAW in the dump); raw = 1: c1a_run.c rh() (the bytes as they are; exact only if every
 * touched unit is RAW) */
uint64_t c1a_hash_va_view(const C1aView *v, uint64_t va, size_t len, uint64_t h, int raw, int *exact);
uint64_t c1a_plugin_hash(const char *version);           /* the s_plugin value of a build string */
const char *c1a_plugin_compat(uint64_t rec_plugin_hash); /* a recorded build this build replays (step path unchanged), or NULL */
const char *c1a_plugin_compat2(uint64_t rec_plugin_hash); /* v0.23.0-C1b: c1a_plugin_compat + 0.22.2-gateC-C1a (the C1a session recording) */

/* C1B3: (C1B-R3-SPEC.md §2.1) the content gate. Each player's move-record tree std::multimap<uint32_t, record*> at PLAYER+0x455d0 (head node
 * pointer; MSVC _Tree: the sentinel's _Left = leftmost, _Parent = root, _Right = rightmost) with its size at PLAYER+0x455d8. 48-byte nodes,
 * key at node+0x20 (VERIFIED, GATE-B.md:1164); _Left +0, _Parent +8, _Right +0x10, _Isnil byte +0x19 (HYPOTHESIS: a bad walk is loud). */
#define C1A_CONTENT_HEAD 0x455d0
#define C1A_CONTENT_SIZE 0x455d8
/* the sizes PLAYER0/1+0x455d8 from a dump (a legacy recording's f0 dump: sizes only); 1 = both read (the units are RAW in the dump) */
int c1a_content_sizes_from_dump(const C1aDump *d, uint64_t n[2]);
/* C1B3: (C1B-R3-SPEC.md §2.6) the address-independent tier-2 heap map (src/t2map.inc, test/gen_t2map.py): core_bss offsets (from the
 * preferred VA 0x14470d000) of the class-G units that held a per-launch heap pointer in the census. The harness build adds its own heap units
 * (SC6RB_HARNESS only; c1a_t2map_hash covers the generated table only). */
#define C1A_T2MAP_BASE 0x14470d000ULL
int      c1a_t2rule_parse(const char *hdr);   /* a header's t2rule=: absent or raw 0, heaptag 1, map1 / map2 2 (C2), anything else -1 */
int      c1a_t2rule_ver(const char *hdr);     /* C2: the map version of a header's t2rule (map1 1, map2 2, else 0) */
extern int g_c1a_t2map_ver;                   /* C2: the active map (2 = map2, the default; 1 = map1, set when replaying a map1 recording) */
int      c1a_t2map_units(void);               /* C2: the active map's unit count (786 map1, 787 map2) */
extern const uint32_t k_t2map[];
extern const int k_t2map_n;
uint64_t c1a_t2map_hash(void);                   /* FNV-1a 64 of the sorted offsets as u32 LE (the generator's hash=) */
int      c1a_t2map_has(uint64_t pref_va);        /* this preferred-base VA starts a mapped unit */
/* the map rule over buf (a copy of memory at runtime VA va = preferred VA pref): every 8-aligned unit is normalized by the address-range tags
 * of c1a_norm_qword (arena, pin, image, region, stack; readable() is never asked); a value none of them claims becomes C1A_TAG_HEAP (kind 6)
 * when the unit is mapped and nonzero, else stays raw. Returns the units changed. */
size_t   c1a_norm_buf_t2map(const C1aNorm *n, uint8_t *buf, uintptr_t va, uint64_t pref, size_t len, uint8_t *kinds);

/* the wind manager 0x144100b60 ([0x14470e038], FUN_140333fd0): +0x00 list head, +0x18 two slot arrays of 8 callbacks, +0x98 slot
 * index (flipped by every update), +0x9c queued count, +0xa0, +0xa4 (spawned), +0xb0..+0xbf spawn parameters (TT800 at Setup),
 * +0xc0..+0xef the per-update output vectors (recomputed by every update) */
#define C1A_WIND     0x144100b60ULL
#define C1A_WIND_LEN 0x100
typedef struct { int ok; uint32_t idx, a0, a4; int32_t cnt; uint64_t head; uint8_t head_kind; int nq; uint64_t q[8]; uint8_t prm[16]; uint8_t outv[48]; } C1aWind;
void c1a_wind_view(const C1aView *v, C1aWind *w);
/* behavior-relevant equality: the queued callbacks of the ACTIVE slot array, the count, +0xa0/+0xa4, the spawn parameters and the
 * list head's null-ness; the slot index itself only selects the physical array (FUN_140333fd0 runs slot[idx][0..cnt), then flips) */
int c1a_wind_equal(const C1aWind *a, const C1aWind *b, char *why, size_t cap);
size_t c1a_wind_str(const C1aWind *w, char *out, size_t cap);
