/*
 * microbench.S — freestanding, static AArch64 microbenchmark for
 * end-to-end validation of the raw2champsim AArch64 pipeline
 * (QEMU plugin -> raw v3/aarch64 -> converter -> ChampSim record).
 *
 * See docs/superpowers/specs/2026-07-07-raw2champsim-aarch64-design.md
 * S5.4 and .superpowers/sdd/task-4-brief.md.
 *
 * This is NOT a Linux userspace binary: it has no libc, no CRT, and
 * expects to run as the *only* code on the CPU (booted bare-metal via
 * QEMU `-M virt -kernel microbench`, no OS). It sets up its own stack,
 * runs a large counted loop with a KNOWN instruction mix, then powers
 * the machine off via a PSCI HVC call so QEMU exits cleanly (which is
 * what flushes/finalizes the plugin's zstd trace file -- see
 * plugin/champsim_tracer.c:plugin_atexit()/close_chunk()).
 *
 * ============================================================
 * ORACLE: per-iteration instruction mix (this is what run_e2e.sh
 * validates the converted trace against)
 * ============================================================
 *
 * One trip around `.Lloop` executes exactly 9 instructions:
 *
 *   1. bl   leaf              call            (is_branch=4 BRANCH_DIRECT_CALL)
 *   2.   add x3, x3, #1       [inside leaf]   int
 *   3.   ret                  return          (is_branch=6 BRANCH_RETURN)
 *   4. ldr  x2, [x19]         load            mem-op (read)
 *   5. add  x2, x2, #1        int
 *   6. str  x2, [x19]         store           mem-op (write)
 *   7. fadd s2, s0, s1        scalar FP       SIMD (Capstone/converter
 *                                             folds scalar FP into
 *                                             INSTR_TYPE_SIMD, spec S4.4)
 *   8. subs x21, x21, #1      int (sets NZCV, but not consumed by a
 *                                  FLAGS-reading instruction here)
 *   9. cbnz x21, .Lloop       conditional     (is_branch=3 BRANCH_CONDITIONAL;
 *                                             CBNZ tests a GPR, not NZCV,
 *                                             so it carries no FLAGS source
 *                                             per spec S4.3)
 *
 * Expected aggregate relationships over any prefix of the trace that
 * covers whole iterations:
 *   - count(is_branch==4)  (bl / call)      == count(is_branch==6) (ret)
 *   - count(is_branch==3)  (cbnz)           == count(is_branch==4) (bl)
 *     (one call, one return, one conditional back-edge per iteration)
 *   - branch fraction   ~= 3/9  = 33.3%   (bl + ret + cbnz)
 *   - mem-op fraction   ~= 2/9  = 22.2%   (ldr + str)
 *   - SIMD fraction     ~= 1/9  = 11.1%   (fadd)
 *   - decode failures   == 0  (every instruction is a plain A64 opcode)
 *
 * A handful of one-time setup instructions run before the loop (stack
 * pointer + buffer pointer setup, register init, two `fmov` immediate
 * loads) and are traced too; they are a negligible fraction against
 * millions of loop iterations and are not part of the oracle above.
 *
 * ITERS below is intentionally larger than the plugin's `limit=`
 * knob used by run_e2e.sh, so the run exercises the plugin's
 * instruction-limit truncation path too: tracing stops mid-loop,
 * the (untraced) CPU keeps running to completion, then shuts down
 * cleanly via PSCI so the trace file is properly finalized.
 */

    .text
    .global _start

    .equ ITERS, 10000000        /* loop trip count (documented above) */
    .equ PSCI_SYSTEM_OFF, 0x84000008

_start:
    /* ---- one-time setup (not part of the per-iteration oracle) ---- */
    adrp    x0, stack_top
    add     x0, x0, :lo12:stack_top
    mov     sp, x0

    adrp    x19, databuf
    add     x19, x19, :lo12:databuf
    mov     x20, #0
    str     x20, [x19]

    mov     x3, #0               /* leaf()'s accumulator, initialized once */

    /* Bare-metal gotcha: at EL1 reset, CPACR_EL1.FPEN traps all FP/SIMD
     * instructions (synchronous exception -> VBAR_EL1+0x200, which is
     * 0 since we never set up a vector table -> infinite fault loop on
     * the all-zero "instruction" there). Enable FP/SIMD access for
     * EL0+EL1 before the first `fmov`. */
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)   /* FPEN = 0b11: no trapping */
    msr     cpacr_el1, x0
    isb

    fmov    s0, #1.0
    fmov    s1, #2.0

    mov     x21, #(ITERS & 0xffff)
    movk    x21, #(ITERS >> 16), lsl #16

    /* ---- hot loop: 9 instructions/iteration, see oracle above ---- */
.Lloop:
    bl      leaf                 /* 1: call  (is_branch=4)            */

    ldr     x2, [x19]            /* 4: load                            */
    add     x2, x2, #1           /* 5: int add                         */
    str     x2, [x19]            /* 6: store                           */

    fadd    s2, s0, s1           /* 7: scalar FP -> SIMD instr_type    */

    subs    x21, x21, #1         /* 8: int, decrement + set flags      */
    cbnz    x21, .Lloop          /* 9: conditional back-edge (is_branch=3) */

    /* ---- loop finished: clean shutdown via PSCI SYSTEM_OFF ---- */
    mov     w0, #0x0008
    movk    w0, #0x8400, lsl #16 /* w0 = PSCI_SYSTEM_OFF (0x84000008) */
    hvc     #0

.Lhang:
    b       .Lhang               /* fallback spin if PSCI didn't stop us */

    /* ---- leaf: trivial 2-instruction call/return pair ---- */
leaf:
    add     x3, x3, #1           /* 2: int (body of the leaf function) */
    ret                          /* 3: return (is_branch=6)            */

    .section .bss
    .align 4
databuf:
    .skip 8
    .align 4
stack_bottom:
    .skip 0x4000
stack_top:

    .section .note.GNU-stack,"",%progbits
