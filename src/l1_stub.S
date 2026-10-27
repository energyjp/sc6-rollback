/* v0.19.0 L1 probe stub template and the L3 canonical-context step wrapper. Win64, intel syntax.
 *
 * L1 probe stub (copied per site into an executable buffer by l1.c, markers patched):
 *   - writes NOTHING to the game stack: rax and the flags go to two TEB TLS slots (gs:[0x1480+8*i]),
 *     the flags are saved with seto/lahf (no pushfq on the game stack) and restored with add al,0x7f/sahf;
 *   - bypass (other thread, or capture off): restore rax/flags, jump to the MinHook trampoline;
 *   - capture: switch rsp to a private stack, push all 16 GPRs + rflags there, XSAVE the full
 *     x87/SSE/AVX/AVX-512 state (components 0..7), call l1_handler(site, regs, xsave), XRSTOR,
 *     pop everything, switch rsp back, jump to the trampoline. The game sees bit-identical registers,
 *     flags, vector state and stack bytes.
 * Markers (patched at install): 0x7EAD0001 TLS(rax) disp32, 0x7EAD0002 TLS(flags) disp32,
 * 0x7EAD0003 TLS(rsp) disp32, 0x7EAD7EAD00000004 &enable_tid, 0x7EAD7EAD00000005 &private stack top,
 * 0x7EAD0006 site id, 0x7EAD7EAD00000007 handler, 0x7EAD7EAD00000008 trampoline (twice). */
    .intel_syntax noprefix
    .text
    .globl l1_tpl_begin
    .globl l1_tpl_restore
    .globl l1_tpl_end
    .p2align 4
l1_tpl_begin:
    mov qword ptr gs:[0x7EAD0001], rax
    seto al
    lahf
    mov qword ptr gs:[0x7EAD0002], rax
    .byte 0xa1                              /* mov eax, dword ptr [moffs64] : enable tid */
    .quad 0x7EAD7EAD00000004
    cmp eax, dword ptr gs:[0x48]            /* TEB ClientId.UniqueThread */
    jne 9f
    mov qword ptr gs:[0x7EAD0003], rsp
    .byte 0x48, 0xa1                        /* mov rax, qword ptr [moffs64] : private stack top */
    .quad 0x7EAD7EAD00000005
    mov rsp, rax
    mov rax, qword ptr gs:[0x7EAD0002]
    add al, 0x7f
    sahf
    mov rax, qword ptr gs:[0x7EAD0001]
    pushfq
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push qword ptr gs:[0x7EAD0003]
    push rbx
    push rdx
    push rcx
    push rax
    mov rbx, rsp                            /* rbx -> L1Gpr {rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15,rflags} */
    lea r12, [rsp - 0x4000]
    and r12, -64                            /* r12 -> XSAVE area (16 KB reserve) */
    xor eax, eax
    mov qword ptr [r12 + 0x200], rax
    mov qword ptr [r12 + 0x208], rax
    mov qword ptr [r12 + 0x210], rax
    mov qword ptr [r12 + 0x218], rax
    mov qword ptr [r12 + 0x220], rax
    mov qword ptr [r12 + 0x228], rax
    mov qword ptr [r12 + 0x230], rax
    mov qword ptr [r12 + 0x238], rax
    mov eax, 0xff
    xor edx, edx
    xsave64 [r12]
    lea rsp, [r12 - 0x40]
    mov ecx, 0x7EAD0006
    mov rdx, rbx
    mov r8, r12
    movabs rax, 0x7EAD7EAD00000007
    call rax
l1_tpl_restore:                             /* also the kill-switch resume point (rbx, r12 set by the VEH) */
    mov eax, 0xff
    xor edx, edx
    xrstor64 [r12]
    mov rsp, rbx
    pop rax
    pop rcx
    pop rdx
    pop rbx
    lea rsp, [rsp + 8]
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    popfq
    mov rsp, qword ptr gs:[0x7EAD0003]
    jmp qword ptr [rip + 0]
    .quad 0x7EAD7EAD00000008
9:
    mov rax, qword ptr gs:[0x7EAD0002]
    add al, 0x7f
    sahf
    mov rax, qword ptr gs:[0x7EAD0001]
    jmp qword ptr [rip + 0]
    .quad 0x7EAD7EAD00000008
l1_tpl_end:
    nop

/* L3: void l3_call_step(void *fn, void *iface, void *in, size_t zero_bytes)
 * Canonical call context for every step pass: saves our callee-saved registers, zeroes
 * [rsp - zero_bytes, rsp) of this frame (the caller committed those pages with an alloca scrub),
 * then calls fn(iface, in) with EVERY other register zero: rbx rbp rsi rdi r8-r15 rax, xmm0-15,
 * flags from xor (DF clear). The only nonzero values the step sees are rcx=iface, rdx=in, rsp,
 * r10=fn (the game's thunk address, identical in every pass) and the return address (ours, constant). */
    .globl l3_call_step
    .def l3_call_step; .scl 2; .type 32; .endef
    .p2align 4
l3_call_step:
    .seh_proc l3_call_step
    push rbp
    .seh_pushreg rbp
    push rbx
    .seh_pushreg rbx
    push rsi
    .seh_pushreg rsi
    push rdi
    .seh_pushreg rdi
    push r12
    .seh_pushreg r12
    push r13
    .seh_pushreg r13
    push r14
    .seh_pushreg r14
    push r15
    .seh_pushreg r15
    sub rsp, 0xa8
    .seh_stackalloc 0xa8
    movaps [rsp + 0x00], xmm6
    .seh_savexmm xmm6, 0x00
    movaps [rsp + 0x10], xmm7
    .seh_savexmm xmm7, 0x10
    movaps [rsp + 0x20], xmm8
    .seh_savexmm xmm8, 0x20
    movaps [rsp + 0x30], xmm9
    .seh_savexmm xmm9, 0x30
    movaps [rsp + 0x40], xmm10
    .seh_savexmm xmm10, 0x40
    movaps [rsp + 0x50], xmm11
    .seh_savexmm xmm11, 0x50
    movaps [rsp + 0x60], xmm12
    .seh_savexmm xmm12, 0x60
    movaps [rsp + 0x70], xmm13
    .seh_savexmm xmm13, 0x70
    movaps [rsp + 0x80], xmm14
    .seh_savexmm xmm14, 0x80
    movaps [rsp + 0x90], xmm15
    .seh_savexmm xmm15, 0x90
    .seh_endprologue
    /* v0.20: vzeroall (all of ymm0-15, including the upper halves the xorps below cannot reach), only
     * when modes_init found AVX in CPUID and the OS saving YMM state (XGETBV): no #UD on other CPUs.
     * xmm6-15 were saved above; the arguments are still in rcx, rdx, r8, r9. */
    cmp byte ptr [rip + g_l3_vzero], 0
    je 11f
    vzeroall
11:
    mov r10, rcx                            /* fn */
    mov r11, rdx                            /* iface */
    mov rsi, r8                             /* in */
    lea rdi, [rsp]
    sub rdi, r9
    mov rcx, r9
    shr rcx, 3
    xor eax, eax
    cld
    rep stosq                               /* zero [rsp - zero_bytes, rsp) */
    mov rcx, r11
    mov rdx, rsi
    xor eax, eax
    xor ebx, ebx
    xor ebp, ebp
    xor esi, esi
    xor edi, edi
    xor r8d, r8d
    xor r9d, r9d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    xorps xmm0, xmm0
    xorps xmm1, xmm1
    xorps xmm2, xmm2
    xorps xmm3, xmm3
    xorps xmm4, xmm4
    xorps xmm5, xmm5
    xorps xmm6, xmm6
    xorps xmm7, xmm7
    xorps xmm8, xmm8
    xorps xmm9, xmm9
    xorps xmm10, xmm10
    xorps xmm11, xmm11
    xorps xmm12, xmm12
    xorps xmm13, xmm13
    xorps xmm14, xmm14
    xorps xmm15, xmm15
    sub rsp, 0x20                           /* home space (already zero); rsp stays 16-byte aligned */
    xor eax, eax
    call r10
    add rsp, 0x20
    movaps xmm6, [rsp + 0x00]
    movaps xmm7, [rsp + 0x10]
    movaps xmm8, [rsp + 0x20]
    movaps xmm9, [rsp + 0x30]
    movaps xmm10, [rsp + 0x40]
    movaps xmm11, [rsp + 0x50]
    movaps xmm12, [rsp + 0x60]
    movaps xmm13, [rsp + 0x70]
    movaps xmm14, [rsp + 0x80]
    movaps xmm15, [rsp + 0x90]
    add rsp, 0xa8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    ret
    .seh_endproc
