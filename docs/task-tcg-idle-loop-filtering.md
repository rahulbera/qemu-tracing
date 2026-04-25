# Design Document: TCG Idle Loop Filtering for Raw Traces

## Problem Statement

When QEMU runs in TCG mode, the guest kernel's idle loop is emulated
rather than causing the vCPU to physically halt. This means the `HLT`
instruction and subsequent timer interrupt handling generate a continuous
stream of kernel-mode instructions even when the workload (Memcached) has
no pending requests. These idle loop instructions are a TCG artifact — they
would not execute on real hardware or under KVM, where `HLT` physically
suspends the CPU core.

In our raw traces, this manifests as ~80% kernel-mode instructions under
TCG, compared to ~50% under KVM for the same workload and snapshot. The
excess ~30% is purely idle loop churn that dilutes the useful trace data.

**Goal:** Build a post-processing filter that identifies and removes TCG
idle loop instructions from raw traces while preserving all genuine kernel
instructions (syscall handlers, network stack processing, scheduler
wake-ups, interrupt handling for real work).

---

## Background: How the Linux Kernel Idle Loop Works

When a CPU has nothing to run, the Linux scheduler calls the idle handler,
which on x86-64 executes the `HLT` instruction. On real hardware, `HLT`
stops instruction execution until an external interrupt arrives.

Under TCG, the sequence is:

```
1. HLT instruction executes (opcode 0xF4, single byte)
2. TCG immediately generates a timer interrupt
3. Kernel interrupt handler runs (~20-50 instructions)
4. Scheduler checks the run queue — nothing to schedule
5. Returns to idle handler
6. HLT again
7. Repeat
```

This loop generates approximately 50-200 kernel instructions per iteration
and repeats millions of times during idle periods.

**Genuine kernel work** (e.g., processing a Memcached request) looks like:

```
1. HLT instruction (Memcached's vCPU is idle, waiting for request)
2. Network interrupt arrives (memtier sent a GET/SET request)
3. Kernel network stack processes the packet (~hundreds of instructions)
4. Kernel wakes the Memcached thread from epoll_wait
5. Transition to USER MODE — Memcached hash lookup, slab access
6. Memcached calls write() syscall
7. Kernel network stack sends response
8. Memcached calls epoll_wait()
9. Kernel checks for more events
10. If no more events → HLT (back to idle)
```

The critical difference: genuine kernel work always leads to user-mode
execution before returning to HLT. Idle loop iterations go from HLT
back to HLT without ever entering user mode.

---

## Filtering Algorithm

### Core Principle

**An idle loop iteration is defined as a sequence of kernel-mode
instructions bounded by two HLT instructions with no user-mode
instructions in between.**

Any kernel instruction sequence that includes or leads to a user-mode
instruction is genuine work and must be preserved.

### State Machine

```
                    ┌──────────────────────────────────┐
                    │                                  │
                    ▼                                  │
              ┌──────────┐   HLT instruction     ┌────┴─────┐
              │          │ ──────────────────────►│          │
   start ───► │  ACTIVE  │                        │  IDLE    │
              │          │ ◄──────────────────────│ CANDIDATE│
              │          │   user-mode insn       │          │
              └──────────┘   (flush buffer)       └──────────┘
                    ▲                                  │
                    │                                  │
                    │         HLT instruction          │
                    │         (discard buffer)          │
                    └──────────────────────────────────┘
```

### Pseudocode

```
state = ACTIVE
idle_buffer = []
stats.idle_discarded = 0
stats.idle_sequences = 0

for each instruction in raw_trace:

    if state == ACTIVE:
        if is_hlt(instruction):
            # Potential start of idle loop
            state = IDLE_CANDIDATE
            idle_buffer = [instruction]
        else:
            emit(instruction)

    elif state == IDLE_CANDIDATE:
        if is_hlt(instruction):
            # HLT → HLT with no user-mode in between
            # The buffered instructions are an idle loop iteration
            stats.idle_discarded += len(idle_buffer)
            stats.idle_sequences += 1
            # Start a new idle candidate with this HLT
            idle_buffer = [instruction]

        elif instruction.privilege == USER:
            # Genuine wake-up! The kernel work leading here is real.
            # Flush all buffered instructions (they're part of the
            # real interrupt/wake path) and this user instruction.
            for buffered_insn in idle_buffer:
                emit(buffered_insn)
            idle_buffer = []
            emit(instruction)
            state = ACTIVE

        else:
            # Kernel instruction after HLT — could be idle loop or
            # genuine interrupt handling. Buffer it until we know.
            idle_buffer.append(instruction)

# End of trace: flush any remaining buffer
# (conservative: treat final buffer as genuine)
for buffered_insn in idle_buffer:
    emit(buffered_insn)
```

### HLT Detection

The `HLT` instruction on x86-64 is a single-byte opcode: `0xF4`.

In our raw trace format, each instruction record contains the raw
instruction bytes. Detection is:

```c
bool is_hlt(instruction) {
    return instruction.instr_size == 1 &&
           instruction.instr_bytes[0] == 0xF4;
}
```

**Edge case:** `HLT` with a `REP` prefix (`0xF3 0xF4`) or `LOCK` prefix
would have size > 1. These don't occur in practice in the Linux idle loop,
but the check on size == 1 handles it safely — prefixed variants won't
false-match.

**Edge case:** `HLT` in user mode would be a segfault (it's a privileged
instruction). So we can further constrain:

```c
bool is_idle_hlt(instruction) {
    return instruction.instr_size == 1 &&
           instruction.instr_bytes[0] == 0xF4 &&
           instruction.privilege == KERNEL;
}
```

---

## Implementation Plan

### Option A: Standalone Filter Tool (Recommended)

Build a standalone C tool `trace_filter` that reads a `.raw.zst` trace,
applies the idle loop filter, and writes a filtered `.raw.zst` trace.

```
trace_vcpu1.raw.zst → trace_filter → trace_vcpu1.filtered.raw.zst
```

**Advantages:**
- Raw traces remain untouched (archival, reproducibility)
- Can be re-run with different filter parameters
- Composable with other filters (e.g., user-only, address range)
- Independent of the offline ChampSim converter

**Usage:**

```bash
./trace_filter input.raw.zst output.raw.zst
./trace_filter -v input.raw.zst output.raw.zst    # verbose: print stats
./trace_filter --stats-only input.raw.zst          # just report, no output
```

### Option B: Integrated into Offline Converter

Apply the filter as the first pass of the raw-to-ChampSim converter.
Simpler pipeline but couples the filter to the converter.

**Recommendation:** Option A. Build it standalone first. It can be
called from scripts and composed with the converter later.

---

## Input/Output Format

### Input

Raw trace file (`.raw.zst` or `.raw`) in our v2 format:

```
File header: 16 bytes (magic, version, vcpu_id, reserved)
Per instruction: variable length
  [header: 4 bytes] [IP: 8 bytes] [insn bytes: N] [mem ops: variable]
```

See CLAUDE.md for the complete format specification.

### Output

Same format, same version. The filter is transparent — it produces a
valid raw trace that any downstream tool (trace_inspector, offline
converter) can consume without modification.

The only difference: idle loop instruction records are omitted.

### Statistics Output (stderr)

```
=== Idle Loop Filter Report ===
Input instructions:      200,000,000
Output instructions:     142,000,000
Idle instructions removed: 58,000,000 (29.0%)
Idle loop iterations:    1,203,847
Avg idle loop length:    48.2 instructions
User-mode instructions:  40,000,000 (20.0% of input, 28.2% of output)
Kernel-mode instructions: 102,000,000 (remaining after filter)
```

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                  trace_filter                        │
│                                                      │
│  ┌──────────┐   ┌──────────────┐   ┌──────────┐    │
│  │  Reader   │──►│ Idle Loop    │──►│  Writer   │   │
│  │ (zstd     │   │ State Machine│   │ (zstd     │   │
│  │  decomp)  │   │              │   │  comp)    │   │
│  └──────────┘   └──────────────┘   └──────────┘    │
│                         │                            │
│                    ┌────▼────┐                       │
│                    │ Stats   │                       │
│                    │ Counter │                       │
│                    └─────────┘                       │
└─────────────────────────────────────────────────────┘
```

### Reader Module

Reuse the `TraceReader` abstraction from `trace_inspector.c`. It handles
both `.raw.zst` and `.raw` input transparently via auto-detection of the
zstd magic number.

### Writer Module

Mirror of the reader: maintains a zstd compression context, buffers
output, and writes compressed data. Reuse the `buffer_append`,
`flush_buffer`, and `flush_final` patterns from `champsim_tracer.c`.

### State Machine Module

Implements the idle loop detection algorithm from the pseudocode above.
Maintains:
- Current state (ACTIVE or IDLE_CANDIDATE)
- Idle instruction buffer (dynamically sized)
- Statistics counters

---

## Edge Cases and Considerations

### 1. Nested interrupts during idle

Rarely, a timer interrupt during idle handling could be interrupted by a
higher-priority interrupt (e.g., network). If that network interrupt leads
to user-mode execution, the algorithm correctly identifies it as genuine
work (because a user-mode instruction appears before the next HLT).

### 2. Very long idle sequences

If the workload is idle for an extended period, the idle buffer could grow
large. In practice, each idle loop iteration is short (50-200 instructions),
and the buffer is flushed (discarded) on every HLT-to-HLT transition.
Maximum buffer size is bounded by the longest single idle loop iteration.

### 3. Kernel-only work between HLTs

Some genuine kernel work may occur between two HLTs without entering user
mode. Examples: RCU callback processing, kernel timer functions, workqueue
items. These would be incorrectly discarded by the basic algorithm.

**Mitigation:** These are rare for Memcached's traced vCPUs (especially
with `rcu_nocbs` which offloads RCU work to other CPUs). The error is
small and conservative (we lose a few genuine kernel instructions rather
than keeping false idle instructions).

**Advanced mitigation (optional):** Instead of just checking for user-mode
instructions, also check for specific interrupt vectors that indicate real
work (network interrupt vs timer interrupt). This requires decoding the
interrupt entry point addresses, which is more complex.

### 4. MWAIT instead of HLT

Some Linux kernel configurations use `MWAIT` instead of `HLT` for idle.
Under QEMU with `-cpu qemu64`, `MWAIT` may not be available, so the kernel
falls back to `HLT`. Verify by checking the first few idle sequences in
a verbose trace dump. If `MWAIT` is used, its opcode is `0x0F 0x01 0xC9`
(3 bytes) — add it to the detection logic.

### 5. First and last instructions in the trace

The trace may start or end in the middle of an idle sequence. The algorithm
handles this correctly:
- Start: if the first instruction is kernel-mode (not HLT), we're in
  ACTIVE state and emit normally.
- End: any buffered instructions are flushed (conservative — treat them
  as genuine).

---

## Testing Plan

### Test 1: Basic correctness

Take a small trace (1M instructions) from a known-idle vCPU (one where
Memcached has no work). Almost all instructions should be filtered out.

```bash
./trace_filter -v idle_trace.raw.zst filtered.raw.zst
# Expected: >95% of instructions removed
```

### Test 2: Active workload preservation

Take a small trace (1M instructions) from an active vCPU during heavy
memtier load. The user/kernel ratio after filtering should improve
(more user-mode percentage) but total instructions should decrease
only modestly.

```bash
# Before filtering
./trace_inspector active_trace.raw.zst
# Expect: ~20% user, ~80% kernel

./trace_filter -v active_trace.raw.zst filtered.raw.zst

# After filtering
./trace_inspector filtered.raw.zst
# Expect: ~50-70% user, ~30-50% kernel (matching KVM-observed ratios)
```

### Test 3: Consistency across vCPUs

Filter all 4 vCPU traces from the same run. The filtered traces should
have similar user/kernel ratios and instruction counts (since Memcached
distributes work roughly evenly across workers).

### Test 4: Round-trip integrity

Verify the filtered trace is a valid raw trace:

```bash
./trace_filter input.raw.zst output.raw.zst
./trace_inspector output.raw.zst
# Should parse cleanly with no errors
```

### Test 5: Comparison with KVM baseline

Compare the user/kernel ratio of filtered TCG traces against KVM-mode
observation (htop during KVM restore). They should be in the same
ballpark (~50/50 for Memcached under moderate load).

---

## File Locations

### Source to create

```
~/qemu-tracing/plugin/trace_filter.c     # The filter tool
```

### Existing code to reuse

```
~/qemu-tracing/plugin/trace_inspector.c  # TraceReader abstraction (zstd decompression)
~/qemu-tracing/plugin/champsim_tracer.c  # Writer patterns (zstd compression, buffer management)
```

### Build command

```bash
gcc -O2 -o trace_filter trace_filter.c $(pkg-config --libs --cflags libzstd)
```

### Usage in the pipeline

```bash
# Step 1: Generate raw traces (existing)
~/qemu-tracing/scripts/boot_tcg_trace.sh 200000000

# Step 2: Filter idle loops (new)
for f in ~/qemu-tracing/traces/trace_vcpu*.raw.zst; do
    out="${f%.raw.zst}.filtered.raw.zst"
    ~/qemu-tracing/plugin/trace_filter -v "$f" "$out"
done

# Step 3: Inspect filtered traces
for f in ~/qemu-tracing/traces/trace_vcpu*.filtered.raw.zst; do
    ~/qemu-tracing/plugin/trace_inspector "$f"
done

# Step 4: Convert to ChampSim format (future, uses filtered traces)
```

---

## Success Criteria

1. The filter correctly identifies and removes idle loop sequences
   (HLT → kernel-only → HLT transitions)
2. All user-mode instructions are preserved (zero false positives
   on user-mode instructions)
3. Genuine kernel work (syscall handling, network stack, scheduler
   wake-ups) is preserved
4. Output is a valid raw trace readable by trace_inspector
5. Filtered user/kernel ratio approximately matches KVM-observed ratio
6. Filter runs at native speed (much faster than trace generation)