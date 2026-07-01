# converter/

## Goal

Convert the pipeline's **raw** trace format (variable-length records
produced by the QEMU TCG plugin) into the **ChampSim v2** trace format
(fixed 512-byte records that the extended ChampSim simulator consumes).

The conversion is a per-instruction x86 decode using
[Zydis](https://github.com/zyantific/zydis): the raw record gives us
IP, instruction bytes, privilege bit, and memory addresses/values;
Zydis fills in the register reads/writes, identifies branches, and
classifies the instruction as INT/FP/SIMD. Output is streaming-zstd
compressed at level 19.

## How this fits into the repo

```
plugin/champsim_tracer.so   ─►  traces/trace_vcpu*.raw.zst
                                          │
                                          ▼
               converter/raw2champsim ─►  traces/trace_vcpu*.champsim.zst
                                          │
                                          ▼
                                   extended ChampSim
```

This is Stage 5 of the pipeline described in
`docs/pipeline/pipeline-stages.md`. The 512-byte ChampSim v2 layout
implemented here matches the format the PIN-based tracer
(`arishem/champsim/tracer/champsim_tracer_mt_roi_v3.cpp`) also emits,
so traces from either tracer can be fed to the same simulator
unchanged.

## Files

### `raw2champsim.c` → `raw2champsim`

Single-file converter. Reads a `.raw.zst`, decodes each instruction
with Zydis, and writes a `.champsim.zst`. The 512-byte v2 record
layout is defined near the top of the source; a summary:

- **Block 1** (64 B) — vanilla ChampSim layout: IP, branch flags,
  source/destination registers (4 src, 2 dst), source/destination
  memory virtual addresses.
- **Block 2** (64 B) — v2 additions: source/destination physical
  addresses (zero-filled — we don't have PA under QEMU/PIN yet),
  per-operand access sizes, privilege bit, instruction type
  (INT/FP/SIMD), reserved bytes.
- **Block 3** (384 B) — memory values: up to 64 bytes per source
  memory op × 4 + up to 64 bytes per destination memory op × 2.

Usage:

```bash
# Default: derive output name from input (.raw.zst → .champsim.zst)
./raw2champsim traces/trace_vcpu1.raw.zst

# Explicit output
./raw2champsim traces/trace_vcpu1.raw.zst /tmp/vcpu1.champsim.zst

# Verbose progress (every 1 M instructions)
./raw2champsim -v traces/trace_vcpu1.raw.zst

# Convert only the first N instructions
./raw2champsim -n 1000000 traces/trace_vcpu1.raw.zst
```

Zstd level is hard-coded to **19** (`ZSTD_COMP_LEVEL`) — this is the
long-term-storage compression setting. Decompression cost is
unaffected, so the ChampSim simulator pays no penalty.

### `Makefile`

Builds `raw2champsim`. **Fetches and builds Zydis automatically** on
first use — Zydis is not committed to this repo. Requires `libzstd-dev`
on the system.

```bash
make                # build the converter (clones Zydis if not present)
make clean          # remove the built binary
make distclean      # remove Zydis too
```

Zydis version is pinned via `ZYDIS_VERSION = v4.1.1` in the Makefile.

### `zydis/`

The Zydis submodule, fetched by `make`. Do not edit — it's a checkout
of the upstream repo. Listed in `.gitignore` for the same reason. If
`git status` shows this as untracked, that's expected; `make
distclean` removes it entirely.

## How to use

```bash
# 1. Build (first time: pulls Zydis, takes ~1 minute).
cd converter/
make

# 2. Convert. If the raw trace was idle-loop-filtered upstream, point
#    at the filtered file — the converter doesn't do filtering.
./raw2champsim ../traces/trace_vcpu1.filtered.raw.zst

# 3. Confirm the output size looks sane. At 512 B/record × N records,
#    a 1 B-instruction trace is 512 GB uncompressed but tends to hit
#    ~5–10 GB after zstd-19 for typical workloads.
ls -lh ../traces/trace_vcpu1.filtered.champsim.zst
```

The output `.champsim.zst` is ready to hand to the extended ChampSim
simulator that lives at `champsim/` (symlink into
`arishem/champsim/`).
