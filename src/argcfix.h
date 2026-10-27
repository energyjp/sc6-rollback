/* v0.34.0 (OPTA-M2-SPEC.md §23; design_v0320/FINDINGS4.md §4.3, §9): the in-memory fix of SC6's own EvaluateIf argument-loop off-by-one.
 *
 * THE BUG (V, FINDINGS4 + an independent re-check). EvaluateIf 0x1403732f0 dispatches its sub-op (args[0]) through the
 * table at 0x140376568. Sub-op 0x25 ("newly pressed direction", the running player) at 0x1403739bc and its opponent twin 0x91 at 0x140373b2d loop
 * over args[1..argc] INCLUSIVE: `sub ebp,1 ; js` then `cmp r9,r10 ; jle`. Their siblings 0x24 (0x140373909) and 0x90 (0x140373a6e) use
 * `dec ebp ; test ebp,ebp ; jle` then `jl`: args[1..argc-1]. argc counts the sub-op word, so args[argc] is one past the arguments: whatever sits in
 * the interpreter's 16-short CALLCOND local there (on the deciding frames, bits 32..47 of `__security_cookie XOR rsp`, one value per launch and PC).
 *
 * THE FIX (FINDINGS4 §4.3; four bytes, in memory only, never on disk): the second opcode byte of four 6-byte jcc rel32 instructions:
 *   0x1403739bf  0f 88 05 1a 00 00  js  0x1403753ca  ->  0f 8e (jle)   sub 0x25: argc <= 1 returns 1 (the empty AND) as 0x24 does
 *   0x140373a5e  0f 8e 71 ff ff ff  jle 0x1403739d5  ->  0f 8c (jl)    sub 0x25: the loop stops at args[argc-1]
 *   0x140373b30  0f 88 94 18 00 00  js  0x1403753ca  ->  0f 8e (jle)   sub 0x91 (the opponent through r11 = [P+0x973e8])
 *   0x140373bdd  0f 8e 6d ff ff ff  jle 0x140373b50  ->  0f 8c (jl)    sub 0x91
 * All four are on one 4 KB page of .text (0x140373000, characteristics 0x60000020: not writable): VirtualProtect, the four one-byte stores,
 * the old protection back, FlushInstructionCache. The rel32 displacements are unchanged (no relocation touches these bytes: V, tools/argc_static.py).
 *
 * The pure core below takes the image base explicitly (g_base in the plugin; a code buffer in test/argc_test.c): the signature check of all four
 * sites before any write, all-or-nothing (a write that does not read back as fixed rolls every site back to stock), the restore (signature-checked
 * again: all four must read as fixed). The plugin's rules (which mode, when, the handshake) are in src/argcfix.c (rbreplay) and src/rbo.c
 * (rbonline). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ARGC_NSITES 4
#define ARGC_A_EVALIF    0x1403732f0ULL
#define ARGC_A_PAGE      0x140373000ULL
/* the four case entries (jump-table targets, V: tools/argc_static.py AS2) and the contiguous span 0x24 .. 0x91's end that test/argc_test.c runs */
#define ARGC_A_SUB24     0x140373909ULL
#define ARGC_A_SUB25     0x1403739bcULL
#define ARGC_A_SUB90     0x140373a6eULL
#define ARGC_A_SUB91     0x140373b2dULL
#define ARGC_A_SPAN_END  0x140373bedULL   /* the next case (sub 0x00) */
#define ARGC_A_RET0      0x1403733aaULL   /* movzx eax, bx (0): the failed AND */
#define ARGC_A_RET1      0x1403753caULL   /* mov eax, 1: the empty AND */
#define ARGC_A_EPI       0x140374bdfULL   /* the common epilogue */
/* the PLAYER fields the two sub-ops read (0x24 / 0x90 read +0x2170 / +0x215c / +0x2178 / +0x2164 at the same places) */
#define ARGC_P_C1        0x217c           /* class 0x1000: compared with the low 12 bits */
#define ARGC_P_C2        0x2168           /* class 0x2000: compared with the low 12 bits */
#define ARGC_P_C3        0x2180           /* class 0x3000: tested against the whole word */
#define ARGC_P_C4        0x216c           /* class 0x4000: tested against the whole word (the raw new-press bits) */
#define ARGC_P_OPP       0x973e8          /* r11 = [P + 0x973e8]: the opponent (sub 0x90 / 0x91) */

typedef struct { uint64_t va; uint8_t stock[6]; uint8_t fixed[6]; const char *what; } ArgcSite;
extern const ArgcSite k_argc_sites[ARGC_NSITES];

enum { ARGC_SITE_STOCK = 0, ARGC_SITE_FIXED = 1, ARGC_SITE_OTHER = 2, ARGC_SITE_UNREADABLE = 3 };
extern const char *const k_argc_site_nm[4];          /* "stock", "fixed", "other", "unreadable" */
enum { ARGC_W_OK = 0, ARGC_W_ALREADY, ARGC_W_SIG, ARGC_W_PROTECT, ARGC_W_VERIFY, ARGC_W_N };
extern const char *const k_argc_why[ARGC_W_N];       /* "ok", "already", "signature", "protect", "verify" */

typedef struct {
    int ok;            /* 1 = the four sites are in the wanted state now */
    int why;           /* ARGC_W_* */
    int first_bad;     /* the first site that blocked a write (-1 = none) */
    int st[ARGC_NSITES];   /* the four sites' states after the call */
    int written;       /* bytes this call wrote (rollback included) */
    int rolled_back;   /* the verify failed and every site was put back */
} ArgcRes;

int  argc_site_state(uintptr_t base, int i);                 /* ARGC_SITE_* (VirtualQuery-guarded read) */
int  argc_site_state_raw(uintptr_t base, int i);             /* the same without VirtualQuery: only after a check said the sites are readable */
void argc_states_str(const int st[ARGC_NSITES], char *out, size_t cap);   /* "stock,stock,stock,stock" */
int  argc_apply(uintptr_t base, ArgcRes *r);                 /* stock -> fixed, all or nothing; returns r->ok */
int  argc_restore(uintptr_t base, ArgcRes *r);               /* fixed -> stock, all or nothing; returns r->ok */

/* The D-I census's C copy of the predicate (sub-ops 0x25 / 0x91; FINDINGS4 §1, test/argc_test.c runs it against the real bytes): the AND over
 * args[1..last] with last = argc (stock) or argc - 1 (fixed). Class 0x1000 / 0x2000: low 12 bits 0 = the field is 0, 5 = do not care, 0xa = the
 * field is not 0, else the field equals them; 0x3000 / 0x4000: (field & word) != 0; any other class is skipped. */
typedef struct { uint16_t c1, c2, c3, c4; } ArgcIn;          /* +0x217c, +0x2168, +0x2180, +0x216c of the evaluated player */
int  argc_pred(const ArgcIn *in, const uint16_t *args, int argc, int fixed);

#ifdef SC6RB_HARNESS
/* fault plants (test/argc_test.c and the harness; 0 / -1 = none): a store that does not take (site k), VirtualProtect refusing, and the two
 * controls (the rollback off: XROLL; the pre-write signature check off: XSIGPRE) */
extern int argc_h_skip_write, argc_h_vp_fail, argc_h_no_rollback, argc_h_no_precheck;
#endif

/* ------------------------------------------------------------------ the plugin (src/argcfix.c; g_base) */
extern volatile int g_argc_cap;     /* rbonline: argc_fix=1 AND the four sites were stock and readable at install (the HELLO advertises it) */
extern volatile int g_argc_broken;  /* a restore was refused (the bytes are not as this plugin left them): never advertise again this process */
void argc_install(void);            /* src/main.c generic path (after vm_install): rbreplay applies for the whole process when argc_fix=1 */
void argc_rbo_install(void);        /* src/rbo_install.c: the capability check (no write) */
void argc_detach(void);             /* src/main.c detach (rbreplay): the restore */
void argc_battle_line(uint64_t battle);   /* src/c1a_run.c c1a_battle_end (rbreplay): ARGC b= state= ... sites= */
