/* Option A M2: the rbonline R-STACK thunks (sc6rb/M2-INTERFACES.md §3.2, §9). Part of the rollback core.
 *
 * M1's HOOK macro (src/nobs_thunks.S, proven by NOBS-CTX; live REGSEQ 0/47,577) with exactly two differences:
 *   1. the body call is `call rbo_dispatch` (src/rbo_body.c: nobs_dispatch first, then rbonline's body), and
 *   2. the final jump AND the contended-path jump go through rbo_hooktab[id].tgt (+24) instead of .orig (+0).
 * PASSIVE: tgt == orig, so each thunk is M1's thunk instruction for instruction (only the symbol names differ).
 * The XSAVE areas and private stacks are M1's (nobs_xarea[id], nobs_pstack_top(id); wired by rbo_install), so nobs_dispatch's REGSEQ
 * reads the areas these thunks filled. The per-hook ctx / lock array is this file's own rbo_hookpriv (NobsCtx layout, stride 0xc0,
 * inuse at +0x88). The XSAVE mask is M1's nobs_xmask (XCR0 without the AMX tile components).
 *
 * What the original sees (V by construction, as M1): identical GPRs, vector state (XRSTOR64 of the same XSAVE64), rsp and an untouched
 * game stack below rsp; status flags are NOT preserved (the lock's bts; M1's review VERIFIED no entry point reads one before writing it);
 * DF untouched. When tgt is rbo_step_armed (ARMED / DRAINED, IFC Step only), the jump lands in our C function with the game's registers,
 * rsp and return address: an ordinary call of the slot's signature (void (*)(iface, in)).
 *
 * Layout:
 *   rbo_hooktab[id]  = { orig +0, pstk +8, xarea +16, tgt +24 }   stride 32   (written by rbo_install before the slot is enabled)
 *   rbo_hookpriv[id] = NobsCtx { rax +0 .. r15 +0x78, rflags +0x80, inuse +0x88 }   stride 0xc0
 *
 * Harness-only (SC6RB_HARNESS) mutated copies of the SAME macro, for RBO-CTX's teeth (each must make the probe differ):
 *   variant 1 = XRSTOR64 skipped (the body's vector writes leak into the original),
 *   variant 2 = a push/pop of rax on the GAME stack before the lock (residue below the game rsp). */
    .intel_syntax noprefix

    .data
    .p2align 4
    .globl rbo_hooktab
rbo_hooktab:                         /* 9 x { orig +0, pstk +8, xarea +16, tgt +24 } */
    .space 9*32

    .bss
    .p2align 6
    .globl rbo_hookpriv
rbo_hookpriv:
    .space 9*0xc0

    .text

    .macro RHOOK name, id, variant
    .globl rbo_th_\name
    .p2align 4
rbo_th_\name:
    .if \variant == 2
    push    rax
    pop     rax
    .endif
    lock bts dword ptr [rip + rbo_hookpriv + \id*0xc0 + 0x88], 0
    jc      8f
    mov     qword ptr [rip + rbo_hookpriv + \id*0xc0 + 0x00], rax
    mov     qword ptr [rip + rbo_hookpriv + \id*0xc0 + 0x20], rsp
    mov     rsp, qword ptr [rip + rbo_hooktab + \id*32 + 8]
    lea     rax, [rip + rbo_hookpriv + \id*0xc0]
    mov     qword ptr [rax + 0x08], rcx
    mov     qword ptr [rax + 0x10], rdx
    mov     qword ptr [rax + 0x18], rbx
    mov     qword ptr [rax + 0x28], rbp
    mov     qword ptr [rax + 0x30], rsi
    mov     qword ptr [rax + 0x38], rdi
    mov     qword ptr [rax + 0x40], r8
    mov     qword ptr [rax + 0x48], r9
    mov     qword ptr [rax + 0x50], r10
    mov     qword ptr [rax + 0x58], r11
    mov     qword ptr [rax + 0x60], r12
    mov     qword ptr [rax + 0x68], r13
    mov     qword ptr [rax + 0x70], r14
    mov     qword ptr [rax + 0x78], r15
    pushfq
    pop     rcx
    mov     qword ptr [rax + 0x80], rcx
    mov     rcx, qword ptr [rip + rbo_hooktab + \id*32 + 16]
    mov     eax, dword ptr [rip + nobs_xmask]
    mov     edx, dword ptr [rip + nobs_xmask + 4]
    xsave64 [rcx]
    mov     ecx, \id
    lea     rdx, [rip + rbo_hookpriv + \id*0xc0]
    sub     rsp, 0x20
    call    rbo_dispatch
    add     rsp, 0x20
    mov     rcx, qword ptr [rip + rbo_hooktab + \id*32 + 16]
    mov     eax, dword ptr [rip + nobs_xmask]
    mov     edx, dword ptr [rip + nobs_xmask + 4]
    .if \variant != 1
    xrstor64 [rcx]
    .endif
    lea     rax, [rip + rbo_hookpriv + \id*0xc0]
    mov     rcx, qword ptr [rax + 0x80]
    push    rcx
    popfq
    mov     rdx, qword ptr [rax + 0x10]
    mov     rbx, qword ptr [rax + 0x18]
    mov     rbp, qword ptr [rax + 0x28]
    mov     rsi, qword ptr [rax + 0x30]
    mov     rdi, qword ptr [rax + 0x38]
    mov     r8,  qword ptr [rax + 0x40]
    mov     r9,  qword ptr [rax + 0x48]
    mov     r10, qword ptr [rax + 0x50]
    mov     r11, qword ptr [rax + 0x58]
    mov     r12, qword ptr [rax + 0x60]
    mov     r13, qword ptr [rax + 0x68]
    mov     r14, qword ptr [rax + 0x70]
    mov     r15, qword ptr [rax + 0x78]
    mov     rcx, qword ptr [rax + 0x08]
    mov     rsp, qword ptr [rip + rbo_hookpriv + \id*0xc0 + 0x20]
    mov     rax, qword ptr [rip + rbo_hookpriv + \id*0xc0 + 0x00]
    mov     dword ptr [rip + rbo_hookpriv + \id*0xc0 + 0x88], 0
    jmp     qword ptr [rip + rbo_hooktab + \id*32 + 24]
8:  jmp     qword ptr [rip + rbo_hooktab + \id*32 + 24]
    .endm

    RHOOK tick,         0, 0
    RHOOK ifc_init,     1, 0
    RHOOK ifc_shutdown, 2, 0
    RHOOK ifc_step,     3, 0
    RHOOK sendctl,      4, 0
    RHOOK ssm,          5, 0
    RHOOK ispaused,     6, 0
    RHOOK setstate,     7, 0
    RHOOK rreset,       8, 0

#ifdef SC6RB_HARNESS
    RHOOK step_mut_noxrstor, 3, 1
    RHOOK step_mut_pushres,  3, 2
#endif

/* ==================================================================================================================================
 * v0.29.1 (review B1): rbonline's ALLOCATOR FRONT DOORS (src/rbo_core.c owns the cells; src/mem.c installs the doors once, at the first
 * ARM, into the GMalloc vtable slots +0x10 / +0x18 / +0x20 / +0x30 and the four CRT heap IAT entries, and never removes them).
 *   index: 0 GMalloc Malloc(self, n, al)  1 Realloc(self, p, n, al)  2 Free(self, p)  3 GetAllocationSize(self, p, out)
 *          4 CRT malloc(n)                5 free(p)                   6 realloc(p, n)   7 calloc(n, size)
 *   rbo_md_door<i>   the slot's target: `jmp qword ptr [rip + cell<i>]` -- touches no register, no flag and no stack byte.
 *   cell<i>          = rbo_md_hk[i] (mem.c's C hook) while a battle is armed (ARM .. END);
 *                    = rbo_md_orig[i] (UE's / the CRT's original) otherwise, so a passive battle's allocator calls reach the original with
 *                      the game's exact registers, rsp and stack (R-STACK), whatever happened in an earlier armed battle of the process;
 *                    = rbo_md_range<i> for the pointer entries (1, 2, 3, 5, 6) while the arena still holds live blocks after an armed
 *                      battle: a pointer inside [rbo_md_alo, rbo_md_ahi) (an arena block UE kept) goes to the C hook (only it can free,
 *                      size or move an arena block); every other pointer jumps to the original. The two compares change the status flags
 *                      only (no register, no stack byte; the flags are dead at a call boundary: the same standard as M1's thunks, whose
 *                      `lock bts` changes them too).
 * The pointer argument: rdx for the GMalloc methods (rcx = the GMalloc object), rcx for CRT free / realloc. */
    .data
    .p2align 4
    .globl rbo_md_cell, rbo_md_orig, rbo_md_hk, rbo_md_alo, rbo_md_ahi
rbo_md_cell:  .space 8*8
rbo_md_orig:  .space 8*8
rbo_md_hk:    .space 8*8
rbo_md_alo:   .quad 0
rbo_md_ahi:   .quad 0

    .text
    .macro MDOOR i
    .globl rbo_md_door\i
    .p2align 4
rbo_md_door\i:
    jmp     qword ptr [rip + rbo_md_cell + \i*8]
    .endm
    .macro MRANGE i, reg
    .globl rbo_md_range\i
    .p2align 4
rbo_md_range\i:
    cmp     \reg, qword ptr [rip + rbo_md_alo]
    jb      1f
    cmp     \reg, qword ptr [rip + rbo_md_ahi]
    jae     1f
    jmp     qword ptr [rip + rbo_md_hk + \i*8]
1:  jmp     qword ptr [rip + rbo_md_orig + \i*8]
    .endm
    MDOOR 0
    MDOOR 1
    MDOOR 2
    MDOOR 3
    MDOOR 4
    MDOOR 5
    MDOOR 6
    MDOOR 7
    MRANGE 1, rdx
    MRANGE 2, rdx
    MRANGE 3, rdx
    MRANGE 5, rcx
    MRANGE 6, rcx
