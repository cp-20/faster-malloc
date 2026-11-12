/*
 * jit_stubs.S -- code template for the JIT-compiled allocation entry point.
 *
 * Hand-assembling x86-64 in C is error-prone: a wrong ModRM byte produces
 * code that faults only on some inputs.  Instead the whole dispatch is
 * assembled here, once, and the C code generator *copies* this template into
 * an executable page and patches a handful of immediates:
 *
 *   - the low bound and the span of each hot size class,
 *   - the thread-local displacement of each class's free list,
 *   - the address of the generic fallback,
 *   - the branch of each *unused* slot, so that it skips straight to the
 *     generic entry instead of testing a range that can never match.
 *
 * Everything else -- instruction selection, register allocation, calling
 * convention, and every branch target that is known at assembly time -- is
 * left to the assembler.
 *
 * Every branch inside the template is *relative*, so the template stays
 * valid when it is copied to a different address.  That is the whole reason
 * the code lives in an assembler file: a single RIP-relative reference
 * would silently break the moment the template is relocated.
 *
 * Register use:
 *   %rdi  the requested size (preserved across the whole dispatch)
 *   %rdx  scratch for the range test
 *   %rcx  scratch for the thread-local displacement
 *   %r11  scratch for an absolute address
 */

    .text

/*
 * One range test per hot size class:
 *
 *   lea  -lo(%rdi), %rdx ; patched
 *   cmp  $span, %rdx     ; patched
 *   jbe  .Lleaf_n        ; relative, resolved by the assembler
 *
 * The test is done in 64 bits so that a huge request cannot be truncated
 * into a small class: (size - lo) is computed modulo 2^64, and a request
 * below the range wraps to a huge value, which fails the unsigned compare.
 *
 * The branch target is exported so that an *unused* slot can be redirected
 * to the generic entry at run time.
 */
#define TESTCLASS(n)                        \
    .byte   0x48, 0x8D, 0x97;              \
    .globl  jit_psub_##n;                   \
    .hidden jit_psub_##n;                   \
jit_psub_##n:                               \
    .long   0;                             \
    .byte   0x48, 0x81, 0xFA;              \
    .globl  jit_pcmp_##n;                   \
    .hidden jit_pcmp_##n;                   \
jit_pcmp_##n:                               \
    .long   0;                             \
    .byte   0x0F, 0x86;                    \
    .globl  jit_pjbe_##n;                   \
    .hidden jit_pjbe_##n;                   \
jit_pjbe_##n:                               \
    .long   .Lleaf_##n - (. + 4)

/*
 * One leaf per hot size class: pop a block from the class's thread-local
 * free list.  The list address is a compile-time constant displacement, so
 * the leaf is one thread-local load plus one dependent load on the block:
 *
 *   mov  $tls_off + cls*8, %rcx
 *   mov  %fs:(%rcx), %rax
 *   test %rax, %rax
 *   jz   .Lgeneric
 *   mov  (%rax), %rdx
 *   mov  %rdx, %fs:(%rcx)
 *   ret
 */
#define LEAF(n)                             \
    .globl  jit_leaf_##n;                   \
    .hidden jit_leaf_##n;                   \
.Lleaf_##n:                                \
jit_leaf_##n:                               \
    .byte   0x48, 0xB9;                    \
    .globl  jit_pdisp_##n;                  \
    .hidden jit_pdisp_##n;                  \
jit_pdisp_##n:                              \
    .quad   0;                             \
    .byte   0x64, 0x48, 0x8B, 0x01;        \
    .byte   0x48, 0x85, 0xC0;              \
    .byte   0x0F, 0x84;                    \
    .long   .Lgeneric - (. + 4);           \
    .byte   0x48, 0x8B, 0x10;              \
    .byte   0x64, 0x48, 0x89, 0x11;        \
    .byte   0xC3

    .globl  jit_template
    .hidden jit_template
    .align  32
jit_template:
    TESTCLASS(0)
    TESTCLASS(1)
    TESTCLASS(2)
    TESTCLASS(3)
    TESTCLASS(4)
    TESTCLASS(5)
    TESTCLASS(6)
    TESTCLASS(7)

    .byte   0xE9                          /* jmp .Lgeneric */
    .long   .Lgeneric - (. + 4)

    LEAF(0)
    LEAF(1)
    LEAF(2)
    LEAF(3)
    LEAF(4)
    LEAF(5)
    LEAF(6)
    LEAF(7)

    /*
     * The generic fallback.  %rdi still holds the requested size.  The
     * address of the C implementation is patched in at copy time, so the
     * entry stays position independent.
     */
    .globl  jit_generic
    .hidden jit_generic
.Lgeneric:
jit_generic:
    .byte   0x49, 0xBB                    /* movabs $imm64, %r11 */
    .globl  jit_pgen
    .hidden jit_pgen
jit_pgen:
    .quad   0                             /* patched: &fm_generic_malloc */
    .byte   0x41, 0xFF, 0xE3              /* jmp *%r11 */

    .globl  jit_template_end
    .hidden jit_template_end
jit_template_end:
    .size   jit_template, jit_template_end - jit_template

    .section .note.GNU-stack,"",@progbits
