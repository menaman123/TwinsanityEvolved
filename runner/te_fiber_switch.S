// te_fiber_switch.S -- the coroutine switch (see te_fiber.h). Stores the callee-saved registers of the running fiber
// into *save (x0 / rdi) and loads the fiber in *load (x1 / rsi). The register layout is te_fiber_ctx's.
#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

#if defined(__aarch64__)

    .text
    .balign 4
    .globl SYM(te_fiber_switch)
SYM(te_fiber_switch):
    mov     x2, sp
    str     x2, [x0, #0]
    stp     x19, x20, [x0, #8]
    stp     x21, x22, [x0, #24]
    stp     x23, x24, [x0, #40]
    stp     x25, x26, [x0, #56]
    stp     x27, x28, [x0, #72]
    stp     x29, x30, [x0, #88]
    stp     d8,  d9,  [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    ldr     x2, [x1, #0]
    mov     sp, x2
    ldp     x19, x20, [x1, #8]
    ldp     x21, x22, [x1, #24]
    ldp     x23, x24, [x1, #40]
    ldp     x25, x26, [x1, #56]
    ldp     x27, x28, [x1, #72]
    ldp     x29, x30, [x1, #88]
    ldp     d8,  d9,  [x1, #104]
    ldp     d10, d11, [x1, #120]
    ldp     d12, d13, [x1, #136]
    ldp     d14, d15, [x1, #152]
    ret

    // First entry of a prepared fiber: x19 = fn, x20 = arg (te_fiber_prepare). fn never returns.
    .globl SYM(te_fiber_trampoline)
SYM(te_fiber_trampoline):
    mov     x0, x20
    blr     x19
    brk     #0

#elif defined(__x86_64__)

    .text
    .balign 16
    .globl SYM(te_fiber_switch)
SYM(te_fiber_switch):
    movq    (%rsp), %rax          /* the return address */
    movq    %rax, 56(%rdi)
    leaq    8(%rsp), %rax         /* the caller's rsp after our return */
    movq    %rax, 0(%rdi)
    movq    %rbx, 8(%rdi)
    movq    %rbp, 16(%rdi)
    movq    %r12, 24(%rdi)
    movq    %r13, 32(%rdi)
    movq    %r14, 40(%rdi)
    movq    %r15, 48(%rdi)
    movq    0(%rsi), %rsp
    movq    8(%rsi), %rbx
    movq    16(%rsi), %rbp
    movq    24(%rsi), %r12
    movq    32(%rsi), %r13
    movq    40(%rsi), %r14
    movq    48(%rsi), %r15
    movq    56(%rsi), %rax
    jmpq    *%rax

    // First entry: r12 = fn, r13 = arg; rsp is 16-aligned here, the call aligns it as the ABI wants at fn's entry.
    .globl SYM(te_fiber_trampoline)
SYM(te_fiber_trampoline):
    movq    %r13, %rdi
    callq   *%r12
    ud2

#elif defined(__arm__) && !defined(__vita__)   /* the Vita runs the game on a kernel thread instead (te_fiber.c) */

    // AAPCS32 (the PlayStation Vita: armv7-a, Thumb-2, VFPv3-D32 + NEON). Callee-saved: r4-r11, lr, d8-d15; sp is 8-byte
    // aligned at a function's entry. te_fiber_ctx: sp at 0, r4..r11 + lr at 4..36 (9 words), d8..d15 at 40.
    .syntax unified
    .thumb
    .text
    .balign 4
    .globl SYM(te_fiber_switch)
    .type SYM(te_fiber_switch), %function
    .thumb_func
SYM(te_fiber_switch):
    str     sp, [r0, #0]
    add     r2, r0, #4
    stmia   r2!, {r4-r11, lr}
    add     r2, r0, #40
    vstmia  r2, {d8-d15}
    ldr     sp, [r1, #0]
    add     r2, r1, #4
    ldmia   r2!, {r4-r11, lr}
    add     r2, r1, #40
    vldmia  r2, {d8-d15}
    bx      lr
    .size SYM(te_fiber_switch), .-SYM(te_fiber_switch)

    // First entry of a prepared fiber: r4 = fn, r5 = arg (te_fiber_prepare). fn never returns.
    .globl SYM(te_fiber_trampoline)
    .type SYM(te_fiber_trampoline), %function
    .thumb_func
SYM(te_fiber_trampoline):
    mov     r0, r5
    blx     r4
    udf     #0
    .size SYM(te_fiber_trampoline), .-SYM(te_fiber_trampoline)

#endif

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
