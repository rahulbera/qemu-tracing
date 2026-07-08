#!/usr/bin/env python3
"""
props.py — property checker for converted AArch64 ChampSim traces.

Reads a `.champsim` or `.champsim.zst` file produced by raw2champsim from a
v3/aarch64 raw trace, walks its 512-byte `input_instr_v2` records, and
asserts the AArch64 output invariants from
docs/superpowers/specs/2026-07-07-raw2champsim-aarch64-design.md S5.2:

  - every record with is_branch != 0 has PC(26) in destination_registers
  - is_branch in {4,5} (BL/BLR, direct/indirect call) has LR(94) in
    destination_registers
  - is_branch == 6 (RET) has LR(94) in source_registers
  - no call/return record (is_branch in {4,5,6}) has SP(6) in either
    register array -- BL/BLR/RET never take an explicit SP operand, so
    this is the checkable proxy for "no record has SP(6) unless it
    explicitly encodes SP" (i.e. no synthetic call/return SP touch is
    ever introduced by the backend; see spec S1.1/S4.3)
  - every register ID present (all src/dst slots, including the 0 = "no
    register" filler) is in the frozen scheme's valid set
    {0,6,25,26,64-94,96-127,200} (spec S3)

Also prints branch and memory-op fractions for a plausibility sanity
check. Exits nonzero (after printing every violation, capped) on any
property violation; prints "ALL PROPERTIES HOLD" and exits 0 otherwise.

Usage:
    python3 props.py <trace.champsim>       # already-decompressed
    python3 props.py <trace.champsim.zst>   # decompressed via `zstd -dc`
"""
import struct
import subprocess
import sys

# ================================================================
# input_instr_v2 layout (converter/raw2champsim.c) -- confirmed by
# reading the struct definition (__attribute__((packed)), 512 bytes)
# and the write path (raw2champsim.c ~line 630-694) that fills it.
# ================================================================

REC_SIZE = 512

NUM_DST = 2  # NUM_INSTR_DESTINATIONS
NUM_SRC = 4  # NUM_INSTR_SOURCES

# Block 1: vanilla ChampSim layout (64 bytes)
OFF_IP           = 0    # uint64_t
OFF_IS_BRANCH    = 8    # uint8_t
OFF_BRANCH_TAKEN = 9    # uint8_t
OFF_DST_REGS     = 10   # uint8_t[NUM_DST]
OFF_SRC_REGS     = 12   # uint8_t[NUM_SRC]
OFF_DST_MEM      = 16   # uint64_t[NUM_DST]
OFF_SRC_MEM      = 32   # uint64_t[NUM_SRC]
# Block 1 ends at 64.

# Block 2: physical addresses + metadata (64 bytes, starts at 64)
OFF_DST_MEM_PA   = 64   # uint64_t[NUM_DST]
OFF_SRC_MEM_PA   = 80   # uint64_t[NUM_SRC]
OFF_SRC_MEM_SIZE = 112  # uint8_t[NUM_SRC]
OFF_DST_MEM_SIZE = 116  # uint8_t[NUM_DST]
OFF_PRIVILEGE    = 118  # uint8_t
OFF_INSTR_TYPE   = 119  # uint8_t
# Block 2 ends at 128 (reserved[8] fills 120-127).

# Block 3 (memory values, 384 bytes) is not needed for these checks.

# Frozen AArch64 register-ID scheme (spec S3).
VALID_IDS = {0, 6, 25, 26} | set(range(64, 95)) | set(range(96, 128)) | {200}

CS_REG_SP = 6
CS_REG_PC = 26
CS_REG_LR = 94

BR_DIRECT_CALL   = 4
BR_INDIRECT_CALL = 5
BR_RETURN        = 6

MAX_PRINTED_VIOLATIONS = 50
MAX_COLLECTED_VIOLATIONS = 200  # stop scanning after this many


READ_BLOCK = 1 << 22   # 4 MiB reads; bounded memory regardless of trace size
HEARTBEAT_RECORDS = 5_000_000   # progress line every 5M records


def iter_records(path):
    """Yield the trace's 512-byte records one at a time, STREAMING.

    A converted 500M-instruction chunk is 500M x 512 B = ~256 GB
    decompressed, so the trace must never be held in memory whole. We
    read the `zstd -dc` (or file) stream in bounded blocks, buffering
    only a partial trailing record across block boundaries. Any trailing
    bytes that don't form a full record are reported and dropped, matching
    the previous whole-file behavior.
    """
    if path.endswith(".zst"):
        proc = subprocess.Popen(["zstd", "-dc", path], stdout=subprocess.PIPE)
        stream = proc.stdout
    else:
        proc = None
        stream = open(path, "rb")

    buf = bytearray()
    eof = False
    try:
        while True:
            block = stream.read(READ_BLOCK)
            if not block:
                eof = True
                break
            buf.extend(block)
            nfull = len(buf) // REC_SIZE
            if nfull:
                off = nfull * REC_SIZE
                mv = memoryview(buf)
                for k in range(nfull):
                    yield bytes(mv[k * REC_SIZE:(k + 1) * REC_SIZE])
                del mv
                del buf[:off]
        if buf:
            print(f"WARNING: ignoring trailing {len(buf)} bytes "
                  f"(not a multiple of {REC_SIZE})", file=sys.stderr)
    finally:
        # If the consumer stops early (e.g. violation cap), we may not have
        # drained the stream — kill the decompressor rather than mistaking
        # its SIGPIPE for a real failure. Only a clean EOF checks the code.
        if proc is not None:
            try:
                stream.close()
            except BrokenPipeError:
                pass
            if eof:
                rc = proc.wait()
                if rc != 0:
                    print(f"ERROR: `zstd -dc {path}` failed (exit {rc})",
                          file=sys.stderr)
                    sys.exit(1)
            else:
                proc.kill()
                proc.wait()
        else:
            stream.close()


def parse_record(rec):
    ip, = struct.unpack_from("<Q", rec, OFF_IP)
    return {
        "ip": ip,
        "is_branch": rec[OFF_IS_BRANCH],
        "branch_taken": rec[OFF_BRANCH_TAKEN],
        "dst_regs": list(rec[OFF_DST_REGS:OFF_DST_REGS + NUM_DST]),
        "src_regs": list(rec[OFF_SRC_REGS:OFF_SRC_REGS + NUM_SRC]),
        "src_mem_size": list(rec[OFF_SRC_MEM_SIZE:OFF_SRC_MEM_SIZE + NUM_SRC]),
        "dst_mem_size": list(rec[OFF_DST_MEM_SIZE:OFF_DST_MEM_SIZE + NUM_DST]),
        "privilege": rec[OFF_PRIVILEGE],
        "instr_type": rec[OFF_INSTR_TYPE],
    }


def check_record(i, r, violations):
    is_branch = r["is_branch"]
    src = r["src_regs"]
    dst = r["dst_regs"]

    if is_branch != 0 and CS_REG_PC not in dst:
        violations.append(
            f"record {i}: is_branch={is_branch} missing PC(26) in "
            f"destination_registers={dst}")

    if is_branch in (BR_DIRECT_CALL, BR_INDIRECT_CALL) and CS_REG_LR not in dst:
        violations.append(
            f"record {i}: is_branch={is_branch} (call) missing LR(94) in "
            f"destination_registers={dst}")

    if is_branch == BR_RETURN and CS_REG_LR not in src:
        violations.append(
            f"record {i}: is_branch=6 (RET) missing LR(94) in "
            f"source_registers={src}")

    if is_branch in (BR_DIRECT_CALL, BR_INDIRECT_CALL, BR_RETURN):
        if CS_REG_SP in dst or CS_REG_SP in src:
            violations.append(
                f"record {i}: is_branch={is_branch} call/return record has "
                f"SP(6) present (BL/BLR/RET never explicitly encode SP) "
                f"src={src} dst={dst}")

    for rid in src + dst:
        if rid not in VALID_IDS:
            violations.append(
                f"record {i}: register id {rid} is not in the valid "
                f"AArch64 scheme {{0,6,25,26,64-94,96-127,200}} "
                f"(src={src} dst={dst})")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <trace.champsim[.zst]>", file=sys.stderr)
        sys.exit(2)

    path = sys.argv[1]

    violations = []
    n = 0
    n_branch = 0
    n_mem = 0
    n_simd = 0

    records = iter_records(path)
    for rec in records:
        r = parse_record(rec)

        if r["is_branch"] != 0:
            n_branch += 1
        if any(s > 0 for s in r["src_mem_size"]) or any(s > 0 for s in r["dst_mem_size"]):
            n_mem += 1
        if r["instr_type"] == 2:  # INSTR_TYPE_SIMD
            n_simd += 1

        check_record(n, r, violations)
        n += 1

        if n % HEARTBEAT_RECORDS == 0:
            print(f"[props] {n // 1_000_000}M records checked "
                  f"({len(violations)} violation(s) so far)...",
                  file=sys.stderr)
            sys.stderr.flush()

        if len(violations) > MAX_COLLECTED_VIOLATIONS:
            violations.append("... (further violations suppressed)")
            records.close()   # stop the decompressor; don't scan the rest
            break

    if n == 0:
        print("ERROR: no complete 512-byte records found", file=sys.stderr)
        sys.exit(1)

    print(f"records:          {n}")
    print(f"branch fraction:  {n_branch}/{n} = {100.0 * n_branch / n:.4f}%")
    print(f"mem-op fraction:  {n_mem}/{n} = {100.0 * n_mem / n:.4f}%")
    print(f"SIMD fraction:    {n_simd}/{n} = {100.0 * n_simd / n:.4f}%")

    if violations:
        print(f"\n{len(violations)} PROPERTY VIOLATION(S):", file=sys.stderr)
        for v in violations[:MAX_PRINTED_VIOLATIONS]:
            print(f"  {v}", file=sys.stderr)
        if len(violations) > MAX_PRINTED_VIOLATIONS:
            print(f"  ... ({len(violations) - MAX_PRINTED_VIOLATIONS} more, "
                  f"not shown)", file=sys.stderr)
        sys.exit(1)

    print("\nALL PROPERTIES HOLD")
    sys.exit(0)


if __name__ == "__main__":
    main()
