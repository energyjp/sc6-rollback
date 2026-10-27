/* v0.32.0 review fix (REVIEW_BUILD.md B1; OPTA-M2-SPEC.md §21.13): the move-script VM dispatch hook (D-F trace, D-H fill, the zero-tail fix).
 * Win64, intel syntax. src/vm.c copies this template (vm_tpl_begin .. vm_tpl_end) into one page within +-2 GiB of the dispatch site, patches
 * the TLS markers, fills the embedded control block (vm_tpl_ctl) and then patches the site:
 *   0x1402e6508  call [r9+rax*8+0x3e83a90] (8 bytes)  ->  call rel32 vm_tpl_pre ; nop3        (one aligned 8-byte store)
 *   0x1402e6510  cmp dword [rip+..], r14d   (7 bytes)  ->  jmp rel32 vm_tpl_post ; nop2 (+ the next instruction's first byte, unchanged;
 *                                                           only with the trace on)
 * Everything inside the copied block is position independent (rip-relative references stay inside the block; the TLS slots are gs: offsets).
 *
 * WHAT THE NATIVE SEES (R-STACK): the stock rsp, the stock return address 0x1402e6510 at [rsp] (the `call rel32` pushes 0x1402e650d; the pre
 * thunk overwrites that one slot with the stock value before anything else can read it), rcx = the PLAYER, edx = argc, r8 = the argument array,
 * r9 = the image base, rax = funcIdx, r10 / r11 / every callee-saved register and the arithmetic flags as stock (rax, r11 and the flags go to
 * three TEB TLS slots, never to the game stack; flags with seto/lahf, restored with add al,0x7f / sahf), DF untouched. The native is reached by
 * `jmp [r9+rax*8+0x3e83a90]`: the stock table read. NOTHING is written below the game rsp except that return slot (stock writes it too).
 * The only other game memory written: the argument tail args[argc..15] when the fill / fix is on (flags bit 0), the interpreter's own frame.
 * The trace (flags bit 1) runs C only on the thread whose id is in the control block (the step thread inside a pass, src/modes.c run_step):
 * the game rsp goes to a TLS slot, rsp switches to a private stack, rcx rdx r8 r9 r10 rbx are pushed THERE, the extended state is saved with
 * XSAVE64 (mask 0xff & XCR0) on the private stack, the C body runs, XRSTOR64, everything is restored and rsp switches back.
 * The post thunk (0x1402e6510) runs the trace's return-value record on the same rule, then the stock `cmp [0x14470dde0], r14d` (through a
 * register: the flags are the cmp's, then mov / jmp keep them for the stock je at 0x1402e6532) and jumps to 0x1402e6517.
 *
 * Markers (patched by vm.c): 0x7EAD0101 TLS(rax), 0x7EAD0102 TLS(r11), 0x7EAD0103 TLS(flags), 0x7EAD0104 TLS(the game rsp): gs: disp32.
 * v0.32.0 finish review fix F5 (OPTA-M2-SPEC.md §21.14): each traced path reads the TSC after its pushes on the private stack and again after
 * its XRSTOR64, and adds the difference to C_TCYC (VMSTAT hook_us): the XSAVE / XRSTOR pairs and the C bodies are inside the measured span; the
 * few instructions before the stack switch and after it are not (about 7-11 ns per dispatch, test/vm_test.c costfill / cost).
 * HARNESS ONLY (SC6RB_HARNESS): vm_tpl_mut_* = the same template with `push rax ; pop rax` first in the pre thunk (a residue below the game
 * rsp): the control XVMSTK of `./build.sh vm` (test/vm_test.c) must catch it. */
    .intel_syntax noprefix
    .text

/* the control block's layout (mirrored by src/vm.c VmCtl) */
#define C_RET      0x00    /* u64 the stock return address (VA 0x1402e6510) */
#define C_FLAGS    0x08    /* u32 bit0 fill / fix, bit1 trace */
#define C_TID      0x0c    /* u32 the step thread while it is inside a pass (0 otherwise) */
#define C_FILLV    0x10    /* u16 the fill value (0 = the fix) */
#define C_N16      0x18    /* u64 dispatches with argc >= 16 (nothing written) */
#define C_PSTK     0x20    /* u64 the private stack's top (16-aligned) */
#define C_PREFN    0x28    /* u64 vm_pre_c */
#define C_POSTFN   0x30    /* u64 vm_post_c */
#define C_CMPADDR  0x38    /* u64 VA 0x14470dde0 (the stock post-call cmp's operand) */
#define C_NEXT     0x40    /* u64 VA 0x1402e6517 */
#define C_NOTHER   0x48    /* u64 traced-mode dispatches outside a pass / on another thread (passed straight through) */
#define C_NFILL    0x50    /* u64 dispatches whose tail was filled */
#define C_T0       0x58    /* u64 finish review fix F5: the TSC when the traced path entered its private stack (step thread only) */
#define C_TCYC     0x60    /* u64 finish review fix F5: the traced paths' own TSC cycles (XSAVE64 / XRSTOR64 and the C body included) */

    .macro VMTPL sfx, mut
    .globl vm_tpl_begin\sfx
    .globl vm_tpl_pre\sfx
    .globl vm_tpl_post\sfx
    .globl vm_tpl_ctl\sfx
    .globl vm_tpl_end\sfx
    .p2align 6
vm_tpl_begin\sfx:
vm_tpl_pre\sfx:
    .if \mut == 1
    push    rax
    pop     rax
    .endif
    mov     qword ptr gs:[0x7EAD0101], rax
    mov     qword ptr gs:[0x7EAD0102], r11
    seto    al
    lahf
    mov     qword ptr gs:[0x7EAD0103], rax
    mov     r11, qword ptr [rip + vm_tpl_ctl\sfx + C_RET]
    mov     qword ptr [rsp], r11                          /* the stock return address in the one slot the stock call writes */
    mov     eax, dword ptr [rip + vm_tpl_ctl\sfx + C_FLAGS]
    test    al, 1
    jz      2f
    cmp     edx, 16
    jae     1f
    mov     eax, edx
    movzx   r11d, word ptr [rip + vm_tpl_ctl\sfx + C_FILLV]
3:  mov     word ptr [r8 + rax*2], r11w                   /* args[argc..15] = the fill value (the interpreter's own frame) */
    inc     eax
    cmp     eax, 16
    jb      3b
    lock inc qword ptr [rip + vm_tpl_ctl\sfx + C_NFILL]
    jmp     2f
1:  lock inc qword ptr [rip + vm_tpl_ctl\sfx + C_N16]
2:  mov     eax, dword ptr [rip + vm_tpl_ctl\sfx + C_FLAGS]
    test    al, 2
    jz      8f
    mov     eax, dword ptr gs:[0x48]                      /* TEB ClientId.UniqueThread */
    cmp     eax, dword ptr [rip + vm_tpl_ctl\sfx + C_TID]
    je      4f
    lock inc qword ptr [rip + vm_tpl_ctl\sfx + C_NOTHER]
    jmp     8f
4:  mov     qword ptr gs:[0x7EAD0104], rsp
    mov     rsp, qword ptr [rip + vm_tpl_ctl\sfx + C_PSTK]
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    rbx
    mov     rbx, rsp                                      /* [rbx] rbx, +8 r10, +16 r9, +24 r8, +32 rdx, +40 rcx */
    rdtsc                                                 /* F5: rax / rdx are saved (TLS / the private stack); the flags are restored at 8 */
    shl     rdx, 32
    or      rax, rdx
    mov     qword ptr [rip + vm_tpl_ctl\sfx + C_T0], rax
    sub     rsp, 0x4000
    and     rsp, -64
    xor     eax, eax
    mov     qword ptr [rsp + 0x200], rax
    mov     qword ptr [rsp + 0x208], rax
    mov     qword ptr [rsp + 0x210], rax
    mov     qword ptr [rsp + 0x218], rax
    mov     qword ptr [rsp + 0x220], rax
    mov     qword ptr [rsp + 0x228], rax
    mov     qword ptr [rsp + 0x230], rax
    mov     qword ptr [rsp + 0x238], rax
    mov     eax, 0xff
    xor     edx, edx
    xsave64 [rsp]
    sub     rsp, 0x30
    mov     rax, qword ptr gs:[0x7EAD0104]
    mov     qword ptr [rsp + 0x20], rax                   /* 5th: the game rsp at the thunk (the native's rsp) */
    mov     rcx, qword ptr [rbx + 40]
    mov     rdx, qword ptr [rbx + 32]
    mov     r8, qword ptr [rbx + 24]
    mov     r9, qword ptr gs:[0x7EAD0101]                 /* funcIdx (the dispatch's rax) */
    call    qword ptr [rip + vm_tpl_ctl\sfx + C_PREFN]
    add     rsp, 0x30
    mov     eax, 0xff
    xor     edx, edx
    xrstor64 [rsp]
    rdtsc                                                 /* F5: the traced pre path's cycles -> C_TCYC (only the step thread runs here) */
    shl     rdx, 32
    or      rax, rdx
    sub     rax, qword ptr [rip + vm_tpl_ctl\sfx + C_T0]
    add     qword ptr [rip + vm_tpl_ctl\sfx + C_TCYC], rax
    mov     rsp, rbx
    pop     rbx
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    mov     rsp, qword ptr gs:[0x7EAD0104]
8:  mov     rax, qword ptr gs:[0x7EAD0103]
    add     al, 0x7f
    sahf
    mov     rax, qword ptr gs:[0x7EAD0101]
    mov     r11, qword ptr gs:[0x7EAD0102]
    jmp     qword ptr [r9 + rax*8 + 0x3e83a90]            /* the stock table read; the native returns to 0x1402e6510 */

    .p2align 4
vm_tpl_post\sfx:
    mov     qword ptr gs:[0x7EAD0101], rax
    mov     qword ptr gs:[0x7EAD0102], r11
    mov     eax, dword ptr gs:[0x48]
    cmp     eax, dword ptr [rip + vm_tpl_ctl\sfx + C_TID]
    jne     8f
    mov     qword ptr gs:[0x7EAD0104], rsp
    mov     rsp, qword ptr [rip + vm_tpl_ctl\sfx + C_PSTK]
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    rbx
    mov     rbx, rsp
    rdtsc                                                 /* F5 (as in the pre thunk) */
    shl     rdx, 32
    or      rax, rdx
    mov     qword ptr [rip + vm_tpl_ctl\sfx + C_T0], rax
    sub     rsp, 0x4000
    and     rsp, -64
    xor     eax, eax
    mov     qword ptr [rsp + 0x200], rax
    mov     qword ptr [rsp + 0x208], rax
    mov     qword ptr [rsp + 0x210], rax
    mov     qword ptr [rsp + 0x218], rax
    mov     qword ptr [rsp + 0x220], rax
    mov     qword ptr [rsp + 0x228], rax
    mov     qword ptr [rsp + 0x230], rax
    mov     qword ptr [rsp + 0x238], rax
    mov     eax, 0xff
    xor     edx, edx
    xsave64 [rsp]
    sub     rsp, 0x30
    mov     rcx, qword ptr gs:[0x7EAD0101]                /* the native's return value */
    mov     rdx, qword ptr gs:[0x7EAD0104]                /* the interpreter's rsp after the return */
    call    qword ptr [rip + vm_tpl_ctl\sfx + C_POSTFN]
    add     rsp, 0x30
    mov     eax, 0xff
    xor     edx, edx
    xrstor64 [rsp]
    rdtsc                                                 /* F5 */
    shl     rdx, 32
    or      rax, rdx
    sub     rax, qword ptr [rip + vm_tpl_ctl\sfx + C_T0]
    add     qword ptr [rip + vm_tpl_ctl\sfx + C_TCYC], rax
    mov     rsp, rbx
    pop     rbx
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    mov     rsp, qword ptr gs:[0x7EAD0104]
8:  mov     rax, qword ptr gs:[0x7EAD0101]
    mov     r11, qword ptr [rip + vm_tpl_ctl\sfx + C_CMPADDR]
    cmp     dword ptr [r11], r14d                          /* the stock post-call cmp: its flags reach the stock je at 0x1402e6532 */
    mov     r11, qword ptr gs:[0x7EAD0102]
    jmp     qword ptr [rip + vm_tpl_ctl\sfx + C_NEXT]

    .p2align 6
vm_tpl_ctl\sfx:
    .space 0x80
vm_tpl_end\sfx:
    .endm

    VMTPL , 0
#ifdef SC6RB_HARNESS
    VMTPL _mut, 1
#endif
