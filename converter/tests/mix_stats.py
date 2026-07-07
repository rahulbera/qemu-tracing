#!/usr/bin/env python3
"""
mix_stats.py -- instruction-mix oracle checker for the AArch64 microbench
end-to-end validation (Task 4 / spec S5.4).

Reads a `.champsim`/`.champsim.zst` file produced by raw2champsim from the
converter/tests/microbench.S trace and checks the observed branch-type
histogram and mem/SIMD fractions against the KNOWN per-iteration mix
documented in the header comment of microbench.S:

    1 bl   (is_branch=4, BRANCH_DIRECT_CALL)
    1 ret  (is_branch=6, BRANCH_RETURN)
    1 cbnz (is_branch=3, BRANCH_CONDITIONAL)
    2 mem ops (1 ldr + 1 str)
    1 SIMD op (fadd, scalar FP folds into INSTR_TYPE_SIMD per spec S4.4)
    9 instructions/iteration total

This does not know in advance how many loop iterations were actually
*traced* (the plugin's limit= may truncate mid-loop) -- so instead of
comparing to a precomputed expected iteration count, it derives the
"expected" counts *from the observed data itself*: bl count is the
reference, and ret/cbnz counts must be within a small tolerance of it
(a truncated final iteration can only cause a difference of ~1).

Usage:
    python3 mix_stats.py <trace.champsim[.zst]>

Exits 0 and prints "MIX OK" if every check passes, else prints the
observed-vs-expected numbers, exits 1.
"""
import struct
import subprocess
import sys

REC_SIZE = 512
NUM_DST = 2
NUM_SRC = 4

OFF_IS_BRANCH   = 8
OFF_SRC_MEM_SIZE = 112
OFF_DST_MEM_SIZE = 116
OFF_INSTR_TYPE  = 119

INSTR_TYPE_SIMD = 2

BR_NONE          = 0
BR_DIRECT_JUMP   = 1
BR_INDIRECT      = 2
BR_CONDITIONAL   = 3
BR_DIRECT_CALL   = 4
BR_INDIRECT_CALL = 5
BR_RETURN        = 6
BR_OTHER         = 7

BR_NAMES = {
    0: "NOT_BRANCH", 1: "DIRECT_JUMP", 2: "INDIRECT", 3: "CONDITIONAL",
    4: "DIRECT_CALL", 5: "INDIRECT_CALL", 6: "RETURN", 7: "OTHER",
}

# Tolerance for call/return/conditional-branch balance: a truncated final
# iteration (plugin limit= firing mid-iteration) can shift these counts by
# at most a couple of instructions relative to each other.
BALANCE_TOLERANCE = 3

# Expected per-iteration fractions (9 instrs/iter: see microbench.S).
EXPECTED_BRANCH_FRACTION = 3.0 / 9.0
EXPECTED_MEM_FRACTION    = 2.0 / 9.0
EXPECTED_SIMD_FRACTION   = 1.0 / 9.0
FRACTION_TOLERANCE       = 0.02  # +/- 2 percentage points


def open_records(path):
    if path.endswith(".zst"):
        proc = subprocess.run(["zstd", "-dc", path], stdout=subprocess.PIPE)
        if proc.returncode != 0:
            print(f"ERROR: `zstd -dc {path}` failed (exit {proc.returncode})",
                  file=sys.stderr)
            sys.exit(1)
        return proc.stdout
    with open(path, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <trace.champsim[.zst]>", file=sys.stderr)
        sys.exit(2)

    data = open_records(sys.argv[1])
    if len(data) == 0 or len(data) % REC_SIZE != 0:
        print(f"ERROR: bad file size {len(data)} (not a nonzero multiple of "
              f"{REC_SIZE})", file=sys.stderr)
        sys.exit(1)

    n = len(data) // REC_SIZE
    branch_hist = {k: 0 for k in BR_NAMES}
    n_mem = 0
    n_simd = 0

    for i in range(n):
        rec = data[i * REC_SIZE:(i + 1) * REC_SIZE]
        is_branch = rec[OFF_IS_BRANCH]
        branch_hist[is_branch] = branch_hist.get(is_branch, 0) + 1
        src_sizes = rec[OFF_SRC_MEM_SIZE:OFF_SRC_MEM_SIZE + NUM_SRC]
        dst_sizes = rec[OFF_DST_MEM_SIZE:OFF_DST_MEM_SIZE + NUM_DST]
        if any(s > 0 for s in src_sizes) or any(s > 0 for s in dst_sizes):
            n_mem += 1
        if rec[OFF_INSTR_TYPE] == INSTR_TYPE_SIMD:
            n_simd += 1

    n_call = branch_hist[BR_DIRECT_CALL]
    n_ret = branch_hist[BR_RETURN]
    n_cbnz = branch_hist[BR_CONDITIONAL]
    n_branch_total = sum(v for k, v in branch_hist.items() if k != BR_NONE)

    branch_frac = n_branch_total / n
    mem_frac = n_mem / n
    simd_frac = n_simd / n

    print(f"records:              {n}")
    print("branch-type histogram:")
    for k in sorted(BR_NAMES):
        print(f"  {k} {BR_NAMES[k]:<14} {branch_hist.get(k, 0)}")
    print()
    print(f"call (bl,  is_branch=4):        {n_call}")
    print(f"return (ret, is_branch=6):      {n_ret}  "
          f"(expected ~= call count {n_call}, tolerance {BALANCE_TOLERANCE})")
    print(f"conditional (cbnz, is_branch=3): {n_cbnz}  "
          f"(expected ~= call count {n_call}, tolerance {BALANCE_TOLERANCE})")
    print()
    print(f"branch fraction: {n_branch_total}/{n} = {100*branch_frac:.4f}%  "
          f"(expected ~= {100*EXPECTED_BRANCH_FRACTION:.4f}%)")
    print(f"mem-op fraction: {n_mem}/{n} = {100*mem_frac:.4f}%  "
          f"(expected ~= {100*EXPECTED_MEM_FRACTION:.4f}%)")
    print(f"SIMD fraction:   {n_simd}/{n} = {100*simd_frac:.4f}%  "
          f"(expected ~= {100*EXPECTED_SIMD_FRACTION:.4f}%)")

    failures = []
    if abs(n_ret - n_call) > BALANCE_TOLERANCE:
        failures.append(
            f"call/return imbalance: call={n_call} ret={n_ret} "
            f"(diff {abs(n_ret - n_call)} > tolerance {BALANCE_TOLERANCE})")
    if abs(n_cbnz - n_call) > BALANCE_TOLERANCE:
        failures.append(
            f"call/conditional imbalance: call={n_call} cbnz={n_cbnz} "
            f"(diff {abs(n_cbnz - n_call)} > tolerance {BALANCE_TOLERANCE})")
    if n_simd == 0:
        failures.append("SIMD fraction is zero (expected nonzero from fadd)")
    if abs(branch_frac - EXPECTED_BRANCH_FRACTION) > FRACTION_TOLERANCE:
        failures.append(
            f"branch fraction {100*branch_frac:.4f}% outside expected band "
            f"{100*EXPECTED_BRANCH_FRACTION:.4f}% +/- {100*FRACTION_TOLERANCE:.1f}pp")
    if abs(mem_frac - EXPECTED_MEM_FRACTION) > FRACTION_TOLERANCE:
        failures.append(
            f"mem-op fraction {100*mem_frac:.4f}% outside expected band "
            f"{100*EXPECTED_MEM_FRACTION:.4f}% +/- {100*FRACTION_TOLERANCE:.1f}pp")
    if abs(simd_frac - EXPECTED_SIMD_FRACTION) > FRACTION_TOLERANCE:
        failures.append(
            f"SIMD fraction {100*simd_frac:.4f}% outside expected band "
            f"{100*EXPECTED_SIMD_FRACTION:.4f}% +/- {100*FRACTION_TOLERANCE:.1f}pp")

    print()
    if failures:
        print(f"{len(failures)} MIX CHECK FAILURE(S):", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        sys.exit(1)

    print("MIX OK")
    sys.exit(0)


if __name__ == "__main__":
    main()
