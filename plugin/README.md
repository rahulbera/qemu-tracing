# plugin/

## Goal

Everything that produces or reads the **raw** per-vCPU trace format:

- The QEMU TCG plugin that writes `.raw.zst` files during a tracing
  run.
- Two offline helpers that read those `.raw.zst` files — one to
  validate/inspect and one to filter out TCG idle-loop noise.

The raw format lives in this directory's source; the ChampSim v2
conversion happens one stage later, in `converter/`.

## How this fits into the repo

Producing traces (see `docs/pipeline/pipeline-stages.md` for the whole
picture):

```
scripts/boot_tcg_trace.sh
        │  loads plugin via QEMU's -plugin flag
        ▼
plugin/champsim_tracer.so  ────►  traces/trace_vcpu*.raw.zst
                                          │
                            [inspect]     │       [filter]
                                ▼         ▼          ▼
              plugin/trace_inspector   plugin/trace_filter
                                          │
                                          ▼
                          converter/raw2champsim  →  .champsim.zst
                                                      │
                                                      ▼
                                                extended ChampSim
```

The raw format is the source of truth in this stage. Both `converter/`
and any downstream analysis tool assume the format defined in the
header comment of `champsim_tracer.c`.

## Files

### `champsim_tracer.c` → `champsim_tracer.so`

The **QEMU TCG plugin** itself. Registers `qemu_plugin_install()` and
per-vCPU/per-TB callbacks with QEMU, and writes a zstd-compressed raw
trace file per traced vCPU. The full raw format is documented at the
top of the source file; a summary:

- 16-byte file header (magic `CSTF`, version `2`, vCPU id).
- Per instruction: 4-byte header (encodes vCPU id, privilege bit,
  instruction size 1–15, num memory ops 0–7) + 8-byte IP + raw
  instruction bytes + per memory op (address, size, flags, optional
  value up to 16 bytes).

Plugin knobs (passed via `-plugin champsim_tracer.so,knob=value,...`):

- `outdir=<path>` — where to write `trace_vcpu<N>.raw.zst`.
- `vcpus=0-3` (or `0,1,2,3`) — which vCPUs to trace.
- `limit=<N>` — instruction limit per vCPU (`0` = unlimited).
- `trigger=<path>` — if set, tracing is *deferred* until this file
  appears on the host filesystem. Enables "start tracing after the
  benchmark reaches steady state" — the guest keeps running under TCG,
  but no records are written until you `touch <path>` on the host.

### `trace_inspector.c` → `trace_inspector`

Offline validator/inspector for `.raw` and `.raw.zst` files.
Auto-detects the format from the first four bytes.

Usage:

```bash
# Summary (record count, header validity, first/last IP, memory-op stats)
./trace_inspector traces/trace_vcpu0.raw.zst

# Verbose — print each instruction record with decoded memory values
./trace_inspector -v -n 20 traces/trace_vcpu0.raw.zst
```

Use this as the first sanity check after a tracing run — confirms the
plugin wrote sensible data before you commit hours to conversion and
simulation.

### `trace_filter.c` → `trace_filter`

Offline filter that strips **TCG idle-loop kernel instruction runs**
from a raw trace. Under TCG the guest kernel's idle loop emulates
`HLT` in software rather than actually halting the vCPU, so raw traces
end up ~80% kernel-mode when only ~50% is real work. The filter
detects runs of kernel-mode instructions bounded by two `HLT`s with
no user-mode instruction in between and removes them.

Usage:

```bash
# Actually filter (writes new .raw.zst)
./trace_filter input.raw.zst output.raw.zst

# Just report how much would be filtered
./trace_filter --stats-only input.raw.zst
```

Full design in `docs/pipeline/task-tcg-idle-loop-filtering.md`.

### `Makefile`

Builds all three targets. Usage:

```bash
# Default: builds plugin, inspector, and filter using ~/qemu-custom as
# the QEMU install prefix.
make

# Point at a QEMU source tree instead of an install
make QEMU_PREFIX=~/qemu-9.2.4/build

# Individual targets
make plugin       # only champsim_tracer.so
make inspector    # only trace_inspector
make filter       # only trace_filter
make clean        # remove built binaries
```

Dependencies checked automatically:
- `glib-2.0` (via `pkg-config`) — QEMU plugin ABI headers.
- `libzstd` (via `pkg-config`) — compression for `.raw.zst`.

Auto-detects whether `qemu-plugin.h` sits under
`$(QEMU_PREFIX)/include/qemu-plugin.h` (install layout) or
`$(QEMU_PREFIX)/include/qemu/qemu-plugin.h` (source-tree layout).

### `build_plugin.sh`

Stand-alone build script for **only the plugin** (useful when working
inside a container or when the Makefile's install-vs-source detection
misfires). Takes an optional path to the QEMU source tree; defaults to
`~/softwares/qemu-9.2.4`.

```bash
./build_plugin.sh                    # uses default path
./build_plugin.sh ~/qemu-9.2.4       # explicit source path
```

Prints the exact `-plugin ...` string you'd feed to `qemu-system-x86_64`
after a successful build.

### `.bak` files (`champsim_tracer.c.bak`, `build_plugin.sh.bak`)

Snapshots kept from before the last major rewrite. Kept for
reference; safe to ignore.

## How to use

Typical flow (see `docs/pipeline/pipeline-stages.md` for the full
sequence):

```bash
# 1. Build.
cd plugin/
make                                     # or: bash build_plugin.sh

# 2. Launch a traced VM (see scripts/boot_tcg_trace.sh).
../scripts/boot_tcg_trace.sh 200000000 roi_running

# 3. Validate output.
./trace_inspector ../traces/trace_vcpu1.raw.zst

# 4. Strip idle-loop noise (optional but usually desirable under TCG).
./trace_filter ../traces/trace_vcpu1.raw.zst \
               ../traces/trace_vcpu1.filtered.raw.zst

# 5. Hand off to the converter.
../converter/raw2champsim ../traces/trace_vcpu1.filtered.raw.zst
```
