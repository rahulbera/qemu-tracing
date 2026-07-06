# CLAUDE.md — QEMU-Based Multi-Threaded Trace Generator for ChampSim

## Project Overview

We are building a trace generation pipeline that extracts multi-threaded
instruction traces from real-world workloads (starting with Memcached) to
feed into an extended ChampSim simulator for NUMA memory system research.

The pipeline uses QEMU's TCG (Tiny Code Generator) mode with a custom
plugin to capture per-vCPU instruction traces, including memory access
values. These traces will be converted offline into ChampSim-compatible
format for simulating a multi-socket, multi-node memory architecture.

## Research Goal

Simulate a 2-socket system (2 cores per socket, each socket with its own
DRAM node) to study data sharing patterns across sockets and evaluate
data placement/migration policies. The workload (Memcached under
memtier_benchmark load) has a large data footprint (~6 GB) and 4 worker
threads that share data structures (hash table, slab allocator), creating
realistic cross-socket sharing.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    QEMU VM (5 vCPUs)                     │
│                                                           │
│  vCPU 0-3: Memcached workers (pinned, traced)            │
│  vCPU 4:   memtier_benchmark + OS (pinned, NOT traced)   │
│                                                           │
│  Phase 1 (KVM): Boot, load data, take snapshot           │
│  Phase 2 (TCG+Plugin): Restore snapshot, trace           │
└─────────────────────────────────────────────────────────┘
         │                              │
         ▼                              ▼
  ┌──────────────┐           ┌────────────────────┐
  │ .raw.zst     │           │ Offline Converter   │
  │ per-vCPU     │ ────────► │ (x86 decode,        │
  │ trace files  │           │  register extract,  │
  │              │           │  branch classify)   │
  └──────────────┘           └────────────────────┘
                                       │
                                       ▼
                             ┌────────────────────┐
                             │ ChampSim Traces     │
                             │ (extended format    │
                             │  with values +      │
                             │  privilege bit)     │
                             └────────────────────┘
                                       │
                                       ▼
                             ┌────────────────────┐
                             │ Extended ChampSim   │
                             │ (2 sockets, NUMA)   │
                             └────────────────────┘
```

## Host Machine

- **CPU:** Intel i7-8700 (6 cores / 12 threads)
- **RAM:** 32 GB
- **OS:** Ubuntu 24.04 LTS
- **QEMU:** 9.2.4 (built from source with `--enable-kvm --enable-plugins`)
- **QEMU source:** `~/softwares/qemu-9.2.4/`
- **QEMU install:** `~/qemu-custom/bin/qemu-system-x86_64`

## Guest VM Configuration

- **OS:** Ubuntu Server 24.04, kernel 6.8.0-107-generic
- **vCPUs:** 5 (`-smp 5`)
- **RAM:** 12 GB (`-m 12G`)
- **Disk:** `~/qemu-tracing/images/ubuntu-guest.qcow2` (qcow2, 40 GB)
- **Networking:** SLIRP user-mode, port forwards: 2222→22 (SSH), 11211→11211 (Memcached)
- **Guest tuning:** ASLR disabled, swap off, THP disabled, unnecessary services disabled

## Workload Setup (Inside Guest)

- **Memcached:** 4 worker threads (`-t 4`), 8 GB memory (`-m 8192`), workers pinned to vCPUs 0-3
- **Data:** ~2.25M records loaded via YCSB, ~6 GB footprint
- **Benchmark:** memtier_benchmark (native C), pinned to vCPU 4
  - Default: `--ratio=1:1` (50/50 GET:SET), `--key-maximum=2250000`, Gaussian distribution
  - Duration set to 86400s (effectively infinite for tracing)
- **Snapshots:**
  - `roi_ready`: Memcached warm with 6 GB data, idle, threads pinned
  - `roi_running`: Same as roi_ready but memtier_benchmark actively sending requests

## Pipeline Stages Completed

### Stage 1: VM Setup ✅
- QEMU installed with KVM
- Guest VM created with cloud-init (headless, no GUI)
- Memcached and memtier_benchmark installed and configured
- YCSB installed (used only for data loading phase)

### Stage 2: Snapshot Creation ✅
- YCSB loads 2.25M records (~6 GB) into Memcached under KVM (fast)
- `roi_ready` snapshot: warm Memcached, idle
- `roi_running` snapshot: warm Memcached + memtier actively running
- Snapshots created under KVM with `-cpu host,-kvmclock`

### Stage 3: TCG Tracing Plugin ✅
- Plugin: `~/qemu-tracing/plugin/champsim_tracer.so`
- Captures: IP, raw instruction bytes, privilege level, memory addresses/sizes/values
- Output: zstd-compressed per-vCPU trace files (`.raw.zst`)
- Value capture via QEMU 9.2 `qemu_plugin_mem_get_value()` API
- Inspector tool: `~/qemu-tracing/plugin/trace_inspector`

### Stage 4: TCG Tracing Run — READY ✅
**kvmclock blocker resolved** — QEMU patched to instantiate the kvmclock device under TCG.

## Current Blocker: kvmclock Snapshot Incompatibility

### The Problem

Snapshots taken under KVM contain a `kvmclock` device state section.
When loading under TCG, QEMU can't find a registered handler for
`kvmclock` (the device is only instantiated under KVM) and aborts:

```
qemu-system-x86_64: Unknown savevm section or instance 'kvmclock' 0.
Make sure that your current VM setup matches your saved VM setup,
including any hotplugged devices
qemu-system-x86_64: Error -22 while loading VM state
```

### What We've Already Tried (Failed)

1. **`-cpu host,-kvmclock`**: Prevents exposing kvmclock CPUID feature but
   QEMU's KVM accelerator still registers the kvmclock save handler
   regardless of CPU flags.

2. **Guest kernel cmdline `no-kvmclock clocksource=tsc tsc=reliable`**:
   Changes the guest clocksource but doesn't prevent QEMU from saving
   kvmclock state at the hypervisor level.

3. **Both combined**: Still fails. The kvmclock VMState handler is registered
   by QEMU's KVM code unconditionally when KVM is the accelerator.

### Why Simple "Skip" Won't Work

The snapshot stream format has no per-section length field. Each section's
data is written by `vmstate_save_state()` according to the device's
`VMStateDescription`. Without knowing the VMState format, we can't skip
the correct number of bytes — the stream position would be wrong and
every subsequent section would fail to load.

### The Fix: Patch QEMU to Create kvmclock Under TCG

**Approach:** Modify `hw/i386/kvm/clock.c` to:
1. Create the kvmclock device even when KVM is not the accelerator
2. In `kvmclock_realize()`, skip KVM-specific initialization under TCG

This way, the VMState handler exists under TCG, QEMU reads the kvmclock
data from the snapshot correctly (and discards it), and the rest of the
snapshot loads normally.

**The kvmclock VMState format is simple:**
```c
// Main state: single uint64_t
.fields = (const VMStateField[]) {
    VMSTATE_UINT64(clock, KVMClockState),
    VMSTATE_END_OF_LIST()
},
// Optional subsection: single bool
.subsections = (const VMStateDescription * const []) {
    &kvmclock_reliable_get_clock,  // contains VMSTATE_BOOL
    NULL
}
```

### What Needs to Be Done

1. Find the exact `kvmclock_create()` function in `~/softwares/qemu-9.2.4/hw/i386/kvm/clock.c`
   and identify where the `kvm_enabled()` guard prevents device creation under TCG.

2. Find where `kvmclock_create()` is called from (likely `hw/i386/pc.c` or similar
   machine init code).

3. Patch `kvmclock_realize()` to return early (skip KVM init) when `!kvm_enabled()`.

4. Patch the call site or `kvmclock_create()` to remove/relax the `kvm_enabled()` guard.

5. Rebuild QEMU: `cd ~/softwares/qemu-9.2.4/build && make -j$(nproc) && make install`

6. Test: Load `roi_running` snapshot under TCG with the tracing plugin.

### Key Source Files

- `~/softwares/qemu-9.2.4/hw/i386/kvm/clock.c` — kvmclock device implementation
- `~/softwares/qemu-9.2.4/migration/savevm.c` — snapshot loading (error originates here)
- `~/softwares/qemu-9.2.4/hw/i386/pc.c` or `~/softwares/qemu-9.2.4/hw/i386/x86.c` — likely calls `kvmclock_create()`

### Build Configuration

```bash
cd ~/softwares/qemu-9.2.4/build
../configure \
    --target-list=x86_64-softmmu \
    --enable-kvm \
    --enable-plugins \
    --enable-slirp \
    --prefix=$HOME/qemu-custom \
    --disable-docs \
    --disable-werror
make -j$(nproc)
make install
```

Binary installs to: `~/qemu-custom/bin/qemu-system-x86_64`

### Testing the Patch

After rebuilding, test with:

```bash
# Test 1: Does the patched QEMU still boot normally under KVM?
~/qemu-custom/bin/qemu-system-x86_64 \
    -accel kvm -cpu host,-kvmclock -smp 5 -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22 \
    -nographic -serial mon:stdio \
    -loadvm memcached_loaded

# Test 2: Does it load the KVM snapshot under TCG? (THE CRITICAL TEST)
~/qemu-custom/bin/qemu-system-x86_64 \
    -accel tcg,thread=multi -cpu qemu64 -smp 5 -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22 \
    -nographic -serial mon:stdio \
    -loadvm memcached_loaded

# Test 3: Does it load under TCG with the tracing plugin?
~/qemu-custom/bin/qemu-system-x86_64 \
    -accel tcg,thread=multi -cpu qemu64 -smp 5 -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic -serial mon:stdio \
    -plugin $HOME/qemu-tracing/plugin/champsim_tracer.so,outdir=$HOME/qemu-tracing/traces,vcpus=0-3,limit=1000000 \
    -loadvm memcached_loaded
```

**Expected success criteria for Test 2:**
- No "Unknown savevm section" error
- VM resumes (you see console output or can SSH on port 2222)
- Guest is functional (Memcached running, memtier sending requests)

**If additional unknown sections appear** (e.g., `kvm-tpr-opt`, `apic-msi`),
the same approach applies: find the device, make it instantiable under TCG.

## File Locations

```
~/qemu-tracing/
├── images/ubuntu-guest.qcow2          # VM disk + snapshots
├── plugin/
│   ├── champsim_tracer.c              # TCG tracing plugin source
│   ├── champsim_tracer.so             # Compiled plugin
│   ├── build_plugin.sh                # Plugin build script
│   ├── trace_inspector.c              # Trace validation tool source
│   └── trace_inspector                # Compiled inspector
├── traces/                            # Output directory for traces
├── snapshots/roi_ready_metadata.txt
└── scripts/
    ├── boot_kvm.sh                    # Stage 1 KVM boot (4 vCPU)
    ├── boot_kvm_5vcpu.sh              # Stage 2+ KVM boot (5 vCPU)
    └── boot_tcg_trace.sh              # Stage 4 TCG+plugin boot

~/softwares/qemu-9.2.4/                # QEMU source tree
~/qemu-custom/                         # QEMU install prefix
```

## Guest-Side Scripts

```
~/run_experiment_v2.sh     # Load data into Memcached + prepare for snapshot
~/start_benchmark.sh       # Launch memtier_benchmark (run after restore)
~/start_memcached_pinned.sh # Start Memcached with thread pinning
```

## Raw Trace Format (v3)

**File:** `.raw.zst` (zstd compressed). Current since the AArch64
capture kit work; full byte-level contract in
`docs/superpowers/specs/2026-07-06-aarch64-capture-kit-design.md`
(§3). Old v2 files remain readable forever — `trace_inspector`,
`trace_filter`, and `converter/raw2champsim` all whitelist versions
`{2, 3}` — but the plugin itself now emits v3 only.

**Header (16 bytes, same size as v2):** magic `CSTF`, version `3`,
vcpu_id, then four individual `uint8_t` fields (not a packed u32,
readers must decode byte-by-byte) replacing v2's reserved word:
`arch` (0=x86_64, 1=aarch64), `flags` (bit0 `has_pa`, bit1
`has_values`), `value_cap` (effective value-capture cap in bytes,
0 if `has_values=0`), `reserved` (0).

**Per instruction (variable length):**
```
[header: 4 bytes]
  bits [3:0]   = vcpu_id (0-15)
  bits [4]     = privilege (0=user, 1=kernel)
  bits [8:5]   = instr_size (1-15)
  bits [11:9]  = num_mem_ops (0-7)

[IP: 8 bytes]
[instruction bytes: instr_size bytes]
[memory ops × num_mem_ops:]
  VA:      8 bytes
  PA:      8 bytes (ONLY present when file header flags.has_pa=1;
           per-file all-or-nothing; failed hwaddr lookups write 0)
  size:    1 byte
  opflags: 1 byte (bit0 write, bit1 has_value, bit2 pa_valid,
           bit3 pa_is_io — bits 2-3 meaningful only when has_pa=1)
  value:   size bytes (only if has_value; only ever set when
           size <= value_cap)
```

**`value_cap`:** today's plugins write `value_cap = 16` because
`qemu_plugin_mem_get_value()` tops out at U128 (16 bytes) and asserts
(VM abort) on wider accesses — that hard API cap is why `value_cap` is
tracked separately from the format's 64-byte value-buffer ceiling
(`MAX_VALUE_SIZE`), which exists so a future wider QEMU API needs no
format or reader change.

New plugin knobs beyond v2's `outdir=`/`vcpus=`/`limit=`/`trigger=`:
`arch=auto|x86_64|aarch64` (default `auto`, resolved from the QEMU
target), `capture_pa=on|off` (default `on`), `values=on|off` (default
`on`). Full knob and format reference: `plugin/README.md`.

**Online rotation (`rotate=N`, optional, default off).** The plugin
can also chunk its own output as it captures: with `rotate=N` (N>0), it
closes the current per-vCPU chunk and opens a fresh one every N traced
instructions on that vCPU, counted independently per vCPU. Chunk files
are named `trace_vcpu<V>_c<KKKKK>.raw.zst` (`_c` = contiguous chunk,
0-indexed, zero-padded to 5 digits) instead of the plain
`trace_vcpu<V>.raw.zst` used when rotation is off, and each traced vCPU
gets a companion `trace_vcpu<V>_manifest.txt` recording every non-empty
chunk's start instruction, instruction count, and exact compressed
size. Every chunk is a standalone v3 file — no changes needed in
`trace_inspector`, `trace_filter`, or `converter/raw2champsim`. This
exists for the AArch64 capture kit's very large captures (the kit
defaults it on, at 100 M instructions/chunk); the plugin's own default
stays off so existing single-file x86_64 usage is unaffected. Full
naming/manifest details and the two bounded stateful-consumer caveats
(idle-filter reset and last-instruction `branch_taken` at chunk
boundaries): `plugin/README.md`.

## ChampSim Trace Format (Target)

Vanilla ChampSim `input_instr` struct (~64 bytes per instruction):
- `ip`: instruction pointer (uint64_t)
- `is_branch`, `branch_taken`: branch info (uint8_t each)
- `source_registers[4]`: source register IDs (uint8_t each)
- `destination_registers[2]`: destination register IDs (uint8_t each)
- `source_memory[4]`: source memory addresses (uint64_t each)
- `destination_memory[2]`: destination memory addresses (uint64_t each)

We plan to extend this with: privilege bit, memory values.

The offline converter (Stage 5, not yet written) will decode x86 instructions
from the raw trace bytes to extract registers, branch info, etc.