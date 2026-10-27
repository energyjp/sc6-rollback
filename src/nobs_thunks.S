/* Option A M1 (OPTA-M0.md §5 R-STACK, as fixed by the M1 review): naked pre-thunks for every game-thread hook.
 *
 * A patched vtable slot points here; on entry the registers are exactly what the game's indirect call set up and the game's
 * return address is at [rsp]. The thunk:
 *   1. takes the per-hook lock FIRST with `lock bts` on a rip-relative dword (no register is touched); if it was already held
 *      (reentry or a second thread), it jumps straight to the original with every register and rsp intact,
 *   2. saves rax into the per-hook ctx (only after the lock is held), saves the game rsp, switches to the private stack,
 *   3. saves every other GPR + RFLAGS into the ctx and the full extended state with XSAVE64 (mask nobs_xmask = XCR0 without the
 *      AMX tile components) into the per-hook 64-byte-aligned area,
 *   4. calls the short pre-only C body nobs_dispatch(id, ctx) on the private stack,
 *   5. XRSTOR64, restores RFLAGS and every GPR, reloads rsp and rax from the ctx, THEN releases the lock with a plain store, and
 *      jmps to the original through nobs_hooktab (constant after install).
 * Nothing is written below the game rsp. Status flags are NOT preserved: bts sets CF (= 0 on the taken path) and leaves
 * OF/SF/AF/PF undefined; the review VERIFIED (entryflags.py) that none of the 10 entry points reads a status flag before writing
 * it. DF and all other RFLAGS bits are untouched.
 *
 * Layout (mirrored by src/nobs.h):
 *   nobs_hooktab[id]  = { orig +0, pstk +8, xarea +16, pad +24 }   stride 32   (written by nobs_thunk_wire before enabling)
 *   nobs_hookpriv[id] = NobsCtx { rax +0 .. r15 +0x78, rflags +0x80, inuse +0x88 }   stride 0xc0
 *   nobs_xmask        = { edx:eax for XSAVE64/XRSTOR64 } (low dword, high dword) */
    .intel_syntax noprefix

    .data
    .p2align 4
    .globl nobs_hooktab
nobs_hooktab:
    .space 9*32
    .globl nobs_xmask
nobs_xmask:
    .long 3, 0                       /* overwritten at init with XCR0 & ~AMX; x87|SSE until then */

    .bss
    .p2align 6
    .globl nobs_hookpriv
nobs_hookpriv:
    .space 9*0xc0

    .text

/* HOOK name, id, variant.  variant 0 = the real thunk. Harness-only mutations of the SAME code (SC6RB_HARNESS):
 *   1 = XRSTOR64 skipped (the body's vector writes leak into the original), 2 = a push/pop of rax on the GAME stack before the
 *   lock (residue below the game rsp). NOBS-CTX must catch both.
 *   3 = the v0.28.0 ordering (review M2): rax stored into a shared slot BEFORE the lock (an xchg lock that needs eax), the
 *   reentry path reloading rax from that slot, and the lock released BEFORE rsp and rax are reloaded. NOBS-RACE must catch it. */
    .macro HOOK name, id, variant
    .globl nobs_th_\name
    .p2align 4
nobs_th_\name:
    .if \variant == 2
    push    rax
    pop     rax
    .endif
    .if \variant == 3
    mov     qword ptr [rip + nobs_h_raxsave_old + \id*8], rax
    mov     eax, 1
    xchg    dword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x88], eax
    test    eax, eax
    jnz     9f
    mov     rax, qword ptr [rip + nobs_h_raxsave_old + \id*8]
    .else
    lock bts dword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x88], 0
    jc      8f
    .endif
    mov     qword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x00], rax
    mov     qword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x20], rsp
    mov     rsp, qword ptr [rip + nobs_hooktab + \id*32 + 8]
    lea     rax, [rip + nobs_hookpriv + \id*0xc0]
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
    mov     rcx, qword ptr [rip + nobs_hooktab + \id*32 + 16]
    mov     eax, dword ptr [rip + nobs_xmask]
    mov     edx, dword ptr [rip + nobs_xmask + 4]
    xsave64 [rcx]
    mov     ecx, \id
    lea     rdx, [rip + nobs_hookpriv + \id*0xc0]
    sub     rsp, 0x20
    call    nobs_dispatch
    add     rsp, 0x20
    mov     rcx, qword ptr [rip + nobs_hooktab + \id*32 + 16]
    mov     eax, dword ptr [rip + nobs_xmask]
    mov     edx, dword ptr [rip + nobs_xmask + 4]
    .if \variant != 1
    xrstor64 [rcx]
    .endif
    lea     rax, [rip + nobs_hookpriv + \id*0xc0]
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
    .if \variant == 3
    mov     dword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x88], 0
    mov     rsp, qword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x20]
    mov     rax, qword ptr [rip + nobs_h_raxsave_old + \id*8]
    jmp     qword ptr [rip + nobs_hooktab + \id*32]
9:  mov     rax, qword ptr [rip + nobs_h_raxsave_old + \id*8]
    jmp     qword ptr [rip + nobs_hooktab + \id*32]
    .else
    mov     rsp, qword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x20]
    mov     rax, qword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x00]
    mov     dword ptr [rip + nobs_hookpriv + \id*0xc0 + 0x88], 0
    jmp     qword ptr [rip + nobs_hooktab + \id*32]
8:  jmp     qword ptr [rip + nobs_hooktab + \id*32]
    .endif
    .endm

    HOOK tick,         0, 0
    HOOK ifc_init,     1, 0
    HOOK ifc_shutdown, 2, 0
    HOOK ifc_step,     3, 0
    HOOK sendctl,      4, 0
    HOOK ssm,          5, 0
    HOOK ispaused,     6, 0
    HOOK setstate,     7, 0
    HOOK rreset,       8, 0

#ifdef SC6RB_HARNESS
    HOOK step_mut_noxrstor, 3, 1
    HOOK step_mut_pushres,  3, 2
    HOOK ssm_mut_oldorder,  5, 3

    .bss
    .p2align 3
    .globl nobs_h_raxsave_old
nobs_h_raxsave_old:
    .space 9*8
#endif
