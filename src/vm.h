/* v0.32.0 (OPTA-M2-SPEC.md §21.2): the move-script VM's native dispatch, as verified statically in the installed exe (read-only) by
 * tools/vm_static.py (VM_STATIC PASS). v0.32.0 review fix (REVIEW_BUILD.md B1; OPTA-M2-SPEC.md §21.13): src/vm.c + src/vm_thunks.S now build
 * goals 2-5 on these facts: D-G (the bytecode dump, a pure read between steps), D-F (the VM trace), D-H (the argument-tail fill) and the
 * zero-tail fix switch. Everything is OFF by default; the dispatch site is patched only when vm_trace / vm_fill / vm_fix is on AND the mode is
 * record / replay / rbreplay (rbonline and netobserve return from the installer before it is reached: never in rbonline this round).
 *
 * THE SITE (V, vm_static VS1-VS5). The interpreter FUN_1402e5a30 (`sub rsp, 0x50`, the stack cookie at [rsp+0x40]) keeps a 16-short local
 * at [rsp+0x20..0x40) that no instruction of the function initializes or clears: the only accesses are the CALLCOND pop loop's store and
 * `lea r8, [rsp+0x20]`. Its CALLCOND opcode (0x0D) block at 0x1402e64af reads funcIdx = bytecode[+1] (ebp) and argc = bytecode[+2] (edx, a
 * byte, never bounded against 16, not written again up to the dispatch), pops argc shorts off the VM ring stack into args[argc-1] .. args[0],
 * then `lea r8, [rsp+0x20]; mov rcx, r13 (the PLAYER, the function's first argument); lea r9, [image base]; call [r9+rax*8+0x3e83a90]`
 * (a 39-entry native table in .rdata, every entry relocated, used by this one instruction only). A native is native(PLAYER, argc, args); its
 * return value (ax) goes on to `movsx r10d, ax`. 19 of the 39 distinct natives are leaves without .pdata; vm_static VS6 lists which natives
 * read argc and which argument slots they read (a first-block heuristic: the precondition of H-U, not its mechanism). V (review): the nested
 * script's own argument array is zeroed by RunBytecodeScript 0x1402e67b0 and ExecuteBankSlotScript's argc <= 1 path before argc shorts are
 * copied in: the only uninitialized array on the path is this CALLCOND local. */
#pragma once
#include <stdint.h>

#define VM_A_INTERP      0x1402e5a30ULL   /* FUN_1402e5a30, the interpreter (pdata 0x1402e5a30..0x1402e67a4; its opcode jump table ends it) */
#define VM_A_CALLCOND    0x1402e64afULL   /* the CALLCOND (0x0D) block: funcIdx, argc, the pop loop, the dispatch, the post-call cmp */
#define VM_A_DISPATCH    0x1402e6508ULL   /* call qword ptr [r9 + rax*8 + 0x3e83a90]   41 ff 94 c1 90 3a e8 03   (8-aligned) */
#define VM_A_POSTCALL    0x1402e6510ULL   /* cmp dword ptr [rip + 0x44278c9], r14d      44 39 35 c9 78 42 04      (16-aligned) */
#define VM_A_POSTNEXT    0x1402e6517ULL
#define VM_A_CMP_TARGET  0x14470dde0ULL   /* the post-call cmp's operand (0x1402e6517 + 0x44278c9) */
#define VM_A_TABLE       0x143e83a90ULL   /* the native table (.rdata, 39 relocated qwords) */
#define VM_NNATIVE       39
#define VM_TABLE_DISP    0x3e83a90u
#define VM_ARGS_RSP_OFF  0x20             /* the 16-short local at [interpreter rsp + 0x20] */
#define VM_NARGS         16
/* the exe's bytes (tools/vm_static.py VS1 compares these two strings with the installed exe; none of their bytes is a DIR64 relocation; src/vm.c
 * compares VM_SIG_CALLCOND with the running image before it patches, and refuses on any difference) */
#define VM_SIG_PROLOGUE  "48895c241855565741544155415641574883ec50488b051538e0034833c44889442440"   /* 0x1402e5a30, 35 bytes */
#define VM_SIG_CALLCOND  "8d43014863c88d43020fb62c394863c80fb614398d42ff4c63d085c078280f1f00490fbfc06641ffc0664523c1410fb70c43" \
                         "6642894c54204983ea0166448905df70420479db400fb6c54c8d442420498bcd4c8d0df89ad1ff41ff94c1903ae803443935c978" \
                         "42044c8b1dba704204440fb70dae704204440fb705a4704204440fbfd00f843afdffff"   /* 0x1402e64af, 137 bytes */
/* spot checks of the table (V, vm_static VS4; FINDINGS3 §1.1) */
#define VM_A_N_EVALIF    0x1403732f0ULL   /* [0] [1] EvaluateIfOpcode (args[0] = the sub-op, stored at PLAYER+0x1c6a) */
#define VM_A_N_BANKSLOT  0x1402fcc30ULL   /* [13] = 0x0D ExecuteBankSlotScript (args[0] = the slot; runs a nested script; P+0x1c68 = the slot meanwhile) */
#define VM_A_N_SETVAR    0x1402fda30ULL   /* [20] = 0x14 SetCharaVar: mov word [P + args[0]*2 + 0x197c], args[1] */
/* the PLAYER fields a trace reads (FINDINGS3 §1.1, §1.2; research3 fn_fc930 / fn_fe1a0) */
#define VM_P_LANE        0x1c58           /* i32, the lane of the running script */
#define VM_P_BANKSEL     0x1c60           /* i32 */
#define VM_P_SLOT        0x1c68           /* i16, the slot of the running script (ExecuteBankSlotScript sets it to the helper slot while it runs) */
#define VM_P_VARS        0x197c           /* u16 vars[]; var 0x40 = +0x19fc (the clash code), var 0x41 = +0x19fe (the clash round) */
#define VM_P_BANK        0x455c0          /* the move bank (heap): ExecuteBankSlotScript `mov rbx, [rcx+0x455c0]`, then 0x1402fc400 (V) */
#define VM_A_SLOT_LOOKUP 0x1402fc400ULL   /* (bank, slot) -> bank + ((u16 [bank + 0x1c + 4*b] + (slot & 0x7ff)) * 9 + 6) * 8, b = (slot >> 12) & 0xf < 4,
                                           * (slot & 0x7ff) < u16 [bank + 0x1e + 4*b]; the bytecode = bank + i32 [record + 0x38] (-1 = none) */
#define VM_REC_LEN       0x48             /* one slot record (9 qwords) */

/* ------------------------------------------------------------------ src/vm.c (review fix B1) */
typedef struct {   /* one native dispatch of a traced pass (D-F) */
    uint8_t fi, argc, depth, p;          /* funcIdx, argc (the byte), nesting depth (0 = outermost), player (0, 1; 2 = rcx is not a PLAYER) */
    int32_t lane;                        /* [P + 0x1c58] at the dispatch */
    uint16_t slot, ret;                  /* [P + 0x1c68] at the dispatch; ax after the native */
    uint8_t has_ret, pad[3];
    uint16_t a[16];                      /* args[0..15] as the native sees them (after the fill / fix) */
} VmRec;
void vm_install(void);                   /* main.c, generic installer path (after the version guard): the D-G flag, then the patch when a VM key is on */
extern volatile int g_vm_trace_on;       /* the trace is installed: the D-F records / windows run */
extern volatile int g_vm_body_on;        /* v0.34.0 (OPTA-M2-SPEC.md §23.2): the pre-thunk C body is installed (trace OR census): run_step brackets every step pass */
extern volatile int g_vm_census_on;      /* v0.34.0: the D-I census (sub 0x25 / 0x91 stock vs fixed return) runs in vm_pre_c */
void vm_step_begin(void);                /* src/modes.c run_step, before the step: this thread's dispatches are traced until vm_step_end */
void vm_step_end(void);
extern volatile int g_vm_on;             /* any VM feature (trace, fill, fix, bytecode) is on in this launch (record / replay / rbreplay only) */
void vm_battle_begin(uint64_t battle);   /* src/c1a_run.c c1a_battle_begin */
void vm_battle_end(void);                /* src/c1a_run.c c1a_battle_end: VMSTAT, the side file closed */
void vm_pass_end(uint32_t f, uint16_t cv[2][2]);   /* src/c1a_run.c c1a_pass_compute: the clash vars after the pass; the pass's records -> frame f */
void vm_confirm(uint32_t f, char ps, const uint16_t cv[2][2]);   /* src/c1a_run.c c1a_confirm: the clash arm, the frame's VMT lines, D-G */
/* v0.32.0 finish review fix F4 (OPTA-M2-SPEC.md §21.14): src/c1a_run.c cmp_frame, after vm_confirm of frame f, at a tier-1 difference from the
 * recording in a component other than ctl: the battle's first one opens a window (the ring's frames before it, vm_trace_diff_pre) */
void vm_t1_diff(uint32_t f, const char *comps);
#ifdef SC6RB_HARNESS
typedef struct { uint64_t ret; uint32_t flags, tid; uint16_t fillv, pad16[3]; uint64_t n16, pstk, prefn, postfn, cmpaddr, next, nother, nfill, t0, tcyc; } VmCtlView;
const VmCtlView *vm_h_ctl(void);         /* the installed control block (NULL = not installed) */
const VmRec *vm_h_scratch(uint32_t *n);  /* the current pass's records */
uint64_t vm_h_unmatched(void);
void vm_h_cycles(uint64_t *body, uint64_t *hook, uint64_t *passes, uint64_t *disp);   /* VMSTAT's sums: the C bodies, the thunks (F5), passes, dispatches */
#endif
