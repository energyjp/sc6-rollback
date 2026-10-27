/* Generic forwarding thunks, see fwd.h. Win64 ABI.
 * Entry from a thunk: r10 = slot index, r11 = FwdFamily*.
 * Frame (rbp-relative): -0x08 slot, -0x10 family, -0x18 saved tid,
 *   -0x20 rcx, -0x28 rdx, -0x30 r8, -0x38 r9, -0x40..-0x58 xmm0..xmm3,
 *   -0x68 result rax, -0x60 result xmm0 (low 8 bytes).
 * Outgoing area: [rsp, rsp+0x80) = home space + 12 copied stack arguments.
 * Optional hooks (family +0x30 pre, +0x38 post), both called as
 *   int hook(int slot, uint64_t rdx, uint64_t *result, uint64_t caller_rsp)   (result[0]=rax, result[1]=xmm0)
 * pre returning nonzero skips the target and returns result[] instead (replay). */
    .intel_syntax noprefix
    .text

    .globl fwd_common
    .def fwd_common; .scl 2; .type 32; .endef
    .p2align 4
fwd_common:
    .seh_proc fwd_common
    push rbp
    .seh_pushreg rbp
    mov rbp, rsp
    .seh_setframe rbp, 0
    sub rsp, 0xf0
    .seh_stackalloc 0xf0
    .seh_endprologue
    mov [rbp-0x08], r10
    mov [rbp-0x10], r11
    mov [rbp-0x20], rcx
    mov [rbp-0x28], rdx
    mov [rbp-0x30], r8
    mov [rbp-0x38], r9
    movq [rbp-0x40], xmm0
    movq [rbp-0x48], xmm1
    movq [rbp-0x50], xmm2
    movq [rbp-0x58], xmm3
    /* counts[bank*64 + slot]++ */
    mov rax, [r11+0x20]
    mov eax, dword ptr [rax]
    shl eax, 6
    add eax, r10d
    mov r11, [r11+0x10]
    lock inc qword ptr [r11+rax*8]
    /* save and clear the tid variable */
    mov r11, [rbp-0x10]
    mov r11, [r11+0x28]
    xor eax, eax
    test r11, r11
    jz 1f
    mov eax, dword ptr [r11]
    mov dword ptr [r11], 0
1:
    mov [rbp-0x18], rax
    /* copy 12 stack argument slots */
    mov rax, [rbp+0x30]
    mov [rsp+0x20], rax
    mov rax, [rbp+0x38]
    mov [rsp+0x28], rax
    mov rax, [rbp+0x40]
    mov [rsp+0x30], rax
    mov rax, [rbp+0x48]
    mov [rsp+0x38], rax
    mov rax, [rbp+0x50]
    mov [rsp+0x40], rax
    mov rax, [rbp+0x58]
    mov [rsp+0x48], rax
    mov rax, [rbp+0x60]
    mov [rsp+0x50], rax
    mov rax, [rbp+0x68]
    mov [rsp+0x58], rax
    mov rax, [rbp+0x70]
    mov [rsp+0x60], rax
    mov rax, [rbp+0x78]
    mov [rsp+0x68], rax
    mov rax, [rbp+0x80]
    mov [rsp+0x70], rax
    mov rax, [rbp+0x88]
    mov [rsp+0x78], rax
    mov qword ptr [rbp-0x60], 0
    mov qword ptr [rbp-0x68], 0
    /* pre hook */
    mov r11, [rbp-0x10]
    mov rax, [r11+0x30]
    test rax, rax
    jz 2f
    mov ecx, dword ptr [rbp-0x08]
    mov rdx, [rbp-0x28]
    lea r8, [rbp-0x68]
    lea r9, [rbp+0x10]          /* the caller's rsp at the call: its outgoing area and locals start here */
    call rax
    test eax, eax
    jnz 7f
2:
    /* restore argument registers */
    mov rcx, [rbp-0x20]
    mov rdx, [rbp-0x28]
    mov r8, [rbp-0x30]
    mov r9, [rbp-0x38]
    movq xmm0, [rbp-0x40]
    movq xmm1, [rbp-0x48]
    movq xmm2, [rbp-0x50]
    movq xmm3, [rbp-0x58]
    /* optional this replacement */
    mov r11, [rbp-0x10]
    mov r10, [rbp-0x08]
    mov rax, [r11+0x08]
    test rax, rax
    jz 3f
    mov rax, [rax+r10*8]
    test rax, rax
    jz 3f
    mov rcx, rax
3:
    mov rax, [r11]
    mov rax, [rax+r10*8]
    test rax, rax
    jz 4f
    call rax
    jmp 5f
4:
    xor eax, eax
    xorps xmm0, xmm0
5:
    mov [rbp-0x68], rax
    movq [rbp-0x60], xmm0
    /* post hook */
    mov r11, [rbp-0x10]
    mov rax, [r11+0x38]
    test rax, rax
    jz 6f
    mov ecx, dword ptr [rbp-0x08]
    mov rdx, [rbp-0x28]
    lea r8, [rbp-0x68]
    lea r9, [rbp+0x10]
    call rax
6:
7:
    /* optional after-call hook (stack scrub below this frame), then zero our outgoing area, so the
     * caller's later callees see the same stack bytes whether UE ran, was muted or was replayed */
    mov r11, [rbp-0x10]
    mov rax, [r11+0x40]
    test rax, rax
    jz 10f
    call rax
    xor eax, eax
    mov [rsp+0x00], rax
    mov [rsp+0x08], rax
    mov [rsp+0x10], rax
    mov [rsp+0x18], rax
    mov [rsp+0x20], rax
    mov [rsp+0x28], rax
    mov [rsp+0x30], rax
    mov [rsp+0x38], rax
    mov [rsp+0x40], rax
    mov [rsp+0x48], rax
    mov [rsp+0x50], rax
    mov [rsp+0x58], rax
    mov [rsp+0x60], rax
    mov [rsp+0x68], rax
    mov [rsp+0x70], rax
    mov [rsp+0x78], rax
    /* round 14: also zero 4 KB below this frame, after the hook returned: the hook's own frames,
     * return addresses and timing values do not survive into the caller's later callees */
    mov r11, [rbp-0x10]
    cmp qword ptr [r11+0x48], 0
    je 10f
    mov r10, rdi
    lea rdi, [rsp-0x1000]
    mov ecx, 0x200
    xor eax, eax
    cld
    rep stosq
    mov rdi, r10
10:
    mov rax, [rbp-0x68]
    movq xmm0, [rbp-0x60]
    /* nonzero-return census; preserve rax and xmm0 */
    mov r10, [rbp-0x08]
    mov r11, [rbp-0x10]
    test rax, rax
    jz 8f
    mov r11, [r11+0x18]
    lock inc qword ptr [r11+r10*8]
    mov r11, [rbp-0x10]
8:
    mov r11, [r11+0x28]
    test r11, r11
    jz 9f
    mov r10, [rbp-0x18]
    mov dword ptr [r11], r10d
9:
    mov rsp, rbp
    pop rbp
    ret
    .seh_endproc

    /* round 14: zero [rsp - rcx, rsp) of the caller without a frame of our own (rcx: bytes, multiple
     * of 8). Only rax, rcx, r8, r9 are touched; rdi is preserved in r8. The pages are already committed
     * by the alloca-based scrubs. */
    .globl zero_below_caller
    .def zero_below_caller; .scl 2; .type 32; .endef
    .p2align 4
zero_below_caller:
    mov r8, rdi
    mov r9, rcx
    lea rdi, [rsp]
    sub rdi, r9
    xor eax, eax
    mov rcx, r9
    shr rcx, 3
    cld
    rep stosq
    mov rdi, r8
    ret

    .macro FWD_THUNK fam, idx
    .globl \fam\()_thunk_\idx
    .p2align 4
\fam\()_thunk_\idx:
    mov r10d, \idx
    lea r11, [rip + \fam\()_family]
    jmp fwd_common
    .endm

    .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40
    FWD_THUNK lp, \i
    .endr

    .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36
    FWD_THUNK ifc, \i
    .endr

    .section .rdata,"dr"
    .p2align 3
    .globl lp_thunk_table
lp_thunk_table:
    .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40
    .quad lp_thunk_\i
    .endr

    .globl ifc_thunk_table
ifc_thunk_table:
    .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36
    .quad ifc_thunk_\i
    .endr
