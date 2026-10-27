/* Option A M2: rbo_l3s_call_step(fn, iface, in, zero_bytes, ctx) -- the L3-stock call wrapper (OPTA-M2-SPEC.md §2 D-B;
 * sc6rb/M2-INTERFACES.md §3.6). Part of the rollback core.
 *
 * l1_stub.S's l3_call_step (UNCHANGED: every existing mode keeps its L3-zero context) with the vector context taken from the RboCtx at
 * the 5th argument instead of zeros:
 *   save the caller's nonvolatile GPRs and xmm6-15, its MXCSR and x87 control word;
 *   vzeroall when g_l3_vzero (AVX usable: every ymm upper half 0); rep-stosq-zero [rsp - zero_bytes, rsp) of this frame (the caller
 *   committed those pages with an alloca scrub, as for l3_call_step);
 *   xmm_lo[i] -> dword 0 of xmmI with `movd` (dwords 1..3 zero; the upper halves stay 0 from vzeroall), ldmxcsr ctx->mxcsr, fldcw ctx->fcw;
 *   every GPR zero except rcx = iface, rdx = in, rsp, r10 = fn (as l3_call_step); call fn(iface, in);
 *   restore xmm6-15, MXCSR, FCW and the GPRs.
 * The measured stock Step-entry context (V, OPTA-SA §2, 47,577 ticks): xmm7 = xmm9 = 0x47a8c000, xmm8 = 0x7fffffff, xmm6 = xmm13 =
 * 0x3c888888, every other xmm and every upper half 0, MXCSR 0x1f80, FCW 0x027f (src/rbo.h RBO_L3S_*; rbo_l3s_ctx in rbo_core.c).
 * RboCtx layout: xmm_lo[16] at +0 (u32 each), mxcsr +64, fcw +68.
 * Frame: 8 pushes + 0xb8 = 248 bytes; at entry the 5th argument is at [rsp+0x28], so after the prologue it is at [rsp+0x120]. */
    .intel_syntax noprefix
    .text
    .globl rbo_l3s_call_step
    .def rbo_l3s_call_step; .scl 2; .type 32; .endef
    .p2align 4
rbo_l3s_call_step:
    .seh_proc rbo_l3s_call_step
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
    sub rsp, 0xb8
    .seh_stackalloc 0xb8
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
    stmxcsr dword ptr [rsp + 0xa0]
    fnstcw word ptr [rsp + 0xa4]
    mov rbx, qword ptr [rsp + 0x120]        /* ctx (5th argument) */
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
    movd xmm0,  dword ptr [rbx + 0x00]
    movd xmm1,  dword ptr [rbx + 0x04]
    movd xmm2,  dword ptr [rbx + 0x08]
    movd xmm3,  dword ptr [rbx + 0x0c]
    movd xmm4,  dword ptr [rbx + 0x10]
    movd xmm5,  dword ptr [rbx + 0x14]
    movd xmm6,  dword ptr [rbx + 0x18]
    movd xmm7,  dword ptr [rbx + 0x1c]
    movd xmm8,  dword ptr [rbx + 0x20]
    movd xmm9,  dword ptr [rbx + 0x24]
    movd xmm10, dword ptr [rbx + 0x28]
    movd xmm11, dword ptr [rbx + 0x2c]
    movd xmm12, dword ptr [rbx + 0x30]
    movd xmm13, dword ptr [rbx + 0x34]
    movd xmm14, dword ptr [rbx + 0x38]
    movd xmm15, dword ptr [rbx + 0x3c]
    ldmxcsr dword ptr [rbx + 0x40]
    fldcw word ptr [rbx + 0x44]
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
    sub rsp, 0x20                           /* home space (already zero); rsp stays 16-byte aligned */
    xor eax, eax
    call r10
    add rsp, 0x20
    ldmxcsr dword ptr [rsp + 0xa0]
    fldcw word ptr [rsp + 0xa4]
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
    add rsp, 0xb8
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
