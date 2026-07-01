# Pipeline Stages Summary

## Stage 1: VM Setup ✅ COMPLETE

**Goal:** QEMU VM with Memcached + YCSB + memtier_benchmark installed.

**Key decisions made:**
- Used Ubuntu Server 24.04 cloud image (headless, no GUI installer needed)
- Memcached instead of Redis (natively multi-threaded workers)
- memtier_benchmark for run phase (native C, instant startup vs Java YCSB)
- YCSB kept only for data loading phase

**Guest tuning applied:**
- ASLR disabled (`kernel.randomize_va_space=0`)
- Swap disabled
- Transparent Huge Pages disabled
- Unnecessary services disabled (snapd, unattended-upgrades, etc.)

## Stage 2: Snapshot Creation ✅ COMPLETE

**Goal:** Golden VM snapshots at the beginning of the region of interest.

**Key decisions made:**
- 5 vCPUs: 4 for Memcached workers (pinned), 1 for benchmark + OS
- YCSB loads 2.25M records (~6 GB) under KVM (fast)
- Two snapshots: `roi_ready` (idle) and `roi_running` (benchmark active)
- `roi_running` eliminates human timing dependency — benchmark already running
- Snapshots taken with `-cpu host,-kvmclock` (attempted fix, insufficient)

**Data footprint:** 2,250,000 records × 10 fields × 400 bytes ≈ 6 GB

## Stage 3: TCG Tracing Plugin ✅ COMPLETE

**Goal:** QEMU TCG plugin that dumps raw instruction traces with memory values.

**Key decisions made:**
- Two-stage architecture: lean online capture + rich offline conversion
- Online captures only runtime-observable info (IP, bytes, privilege, mem addr/size/value)
- Offline derives registers, branch info, ChampSim format (not yet written)
- zstd streaming compression at level 1 (reduces I/O, net performance win)
- Memory values via QEMU 9.2 `qemu_plugin_mem_get_value()` (up to 128-bit)
- Privilege inferred from IP (>= 0xFFFF800000000000 = kernel)
- Per-vCPU state, no locking, 16 MB uncompressed buffers

**Plugin features:**
- Configurable vCPU selection (`vcpus=0-3`)
- Configurable instruction limit per vCPU (`limit=200000000`)
- Configurable output directory (`outdir=/path`)
- Format version 2 (with values)
- File magic: `CSTF`

**Tools built:**
- `champsim_tracer.so` — the tracing plugin
- `trace_inspector` — validates and inspects raw traces (supports .raw and .raw.zst)

## Stage 4: TCG Tracing Run 🚫 BLOCKED

**Goal:** Load snapshot under TCG with plugin, generate traces.

**Blocker:** KVM snapshot contains `kvmclock` device state that TCG can't load.
See `docs/kvmclock-patch-details.md` for full analysis and fix plan.

**Current task:** Patch QEMU to allow kvmclock device instantiation under TCG.

## Stage 5: Offline Converter (NOT STARTED)

**Goal:** Convert raw traces (.raw.zst) to extended ChampSim trace format.

**Will need:**
- x86-64 instruction decoder (extract registers, identify branches)
- Branch taken/not-taken detection from IP sequence analysis
- Register ID mapping (consistent scheme for dependency tracking)
- Extended ChampSim trace format (privilege bit, memory values)
- Kernel instruction filtering (configurable include/exclude)
- Per-vCPU output files in ChampSim format

## Stage 6: ChampSim Integration (NOT STARTED)

**Goal:** Feed generated traces into extended ChampSim for NUMA simulation.

**Will need:**
- Extended trace reader in ChampSim
- 4-core, 2-socket configuration
- vCPU-to-socket mapping (vCPU 0,1 → socket 0; vCPU 2,3 → socket 1)
- NUMA-aware memory node modeling
