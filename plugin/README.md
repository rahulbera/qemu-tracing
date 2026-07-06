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
top of the source file (canonical byte-level reference:
`docs/superpowers/specs/2026-07-06-aarch64-capture-kit-design.md`, §3);
this is a summary of **format v3**, current since the AArch64 capture
kit work.

**File header — 16 bytes, unchanged size from v2:**

```
Offset  Size  Field       Value / semantics
0       u32   magic       "CSTF" (0x46545343 little-endian)
4       u32   version     3
8       u32   vcpu_id
12      u8    arch        0 = x86_64, 1 = aarch64
13      u8    flags       bit0 has_pa      (mem-ops carry an 8-byte PA)
                          bit1 has_values  (value capture enabled)
                          bits2-7 reserved, written 0
14      u8    value_cap   effective value-capture cap in bytes (0 if
                          has_values=0; see "value_cap semantics" below)
15      u8    reserved    0
```

Bytes 12–15 replace v2's reserved u32. **Readers must decode them as
individual `uint8_t`s, not as bitfields of a little-endian u32** — this
is an easy endianness trap. The three u32 fields (magic, version,
vcpu_id) keep the existing host-little-endian memcpy convention.

**Per instruction (variable length):**

```
[header: 4 bytes, uint32_t]     bits[3:0] vcpu_id, bit[4] privilege,
                                bits[8:5] instr_size (1-15),
                                bits[11:9] num_mem_ops (0-7)
[IP: 8 bytes]
[instruction bytes: instr_size bytes]
[memory ops × num_mem_ops]:
    VA:      8 bytes  (uint64_t, guest virtual address)
    PA:      8 bytes  (uint64_t, guest physical address) — ONLY
             present when the file-header flags.has_pa bit is set.
             Per-file, all-or-nothing: when present, it appears in
             every mem-op regardless of pa_valid/pa_is_io; a failed
             hwaddr lookup writes PA=0.
    size:    1 byte   (access width in bytes)
    opflags: 1 byte   bit0 write
                      bit1 has_value
                      bit2 pa_valid   (hwaddr lookup succeeded)
                      bit3 pa_is_io   (MMIO: PA is device-relative,
                                       not RAM; only meaningful when
                                       has_pa/pa_valid are set)
                      bits4-7 reserved, 0
    value:   size bytes, present iff opflags.has_value = 1
```

**Plugin knobs** (passed via `-plugin champsim_tracer.so,knob=value,...`):

| Knob | Values | Default | Behavior |
|---|---|---|---|
| `outdir=<path>` | any path | *(required)* | where to write `trace_vcpu<N>.raw.zst` |
| `vcpus=<range>` | `0-3` or `0,1,2,3` | *(required)* | which vCPUs to trace |
| `limit=<N>` | integer | `0` = unlimited | instruction limit per vCPU |
| `trigger=<path>` | host path | none (immediate) | tracing is *deferred* until this file appears on the host filesystem — the guest keeps running under TCG, but no records are written until you `touch <path>` on the host |
| `arch=` | `auto` \| `x86_64` \| `aarch64` | `auto` | `auto` resolves from the QEMU target at install time; an explicit value overrides detection (a mismatch logs a prominent warning but honors the override). An unknown target under `auto` is a **fatal install error** — the plugin never guesses the privilege threshold |
| `capture_pa=` | `on` \| `off` \| `1` \| `0` | `on` | capture each mem-op's guest physical address via `qemu_plugin_get_hwaddr`. `off` genuinely skips the hwaddr call (it costs a TLB walk per access), not just the record bytes |
| `values=` | `on` \| `off` \| `1` \| `0` | `on` | capture memory values via `qemu_plugin_mem_get_value`. `off` means no mem-op ever sets `has_value`, and the header's `flags.has_values=0`/`value_cap=0` |
| `rotate=<N>` | integer | `0` = off | close the current per-vCPU chunk and open a fresh one every N traced instructions on that vCPU. See "Rotation" below |

**`value_cap` semantics — the honest provision.** Header byte 14
records the *effective* value-capture cap of the build that wrote the
file: the guarantee that no captured value exceeds it. Today that's
**16**, not the format's 64-byte ceiling (`MAX_VALUE_SIZE`), because
`qemu_plugin_mem_get_value()` tops out at U128 (16 bytes) in QEMU
9.2 — calling it on a wider access is a hard `g_assert_not_reached()`
**VM abort**, not a skipped value. So an access wider than 16 bytes
(some AArch64 SVE loads/stores, for example) gets its address and size
captured with `has_value=0`; the plugin never attempts extraction on
it. The format ceiling stays 64 bytes (matching the ChampSim v2
record's value slots) precisely so that when a wider QEMU API exists,
the plugin can start writing a larger `value_cap` with **no format
change and no reader change** — readers size their buffers from the
64-byte ceiling and validate each value against `value_cap`, never the
other way around.

**v2 files remain readable.** The plugin emits v3 only — there is no
v2-emission mode — but `trace_inspector`, `trace_filter`, and
`converter/raw2champsim` all whitelist versions `{2, 3}` and continue
to read existing v2 files on disk forever (decoded as `arch=x86_64`,
no PA/value_cap fields). You do not need to re-capture or convert old
traces after upgrading the plugin.

**Rotation (`rotate=N`).** Off by default (`rotate=0`, or the knob
absent) — behavior is byte-for-byte identical to the pre-rotation
plugin: one `trace_vcpu<V>.raw.zst` per traced vCPU, no chunk index, no
manifest. With `rotate=N` (N>0), the plugin closes the current chunk
and opens a fresh one every N *traced* instructions on that vCPU,
counted independently per vCPU (chunk boundaries need not line up
across vCPUs). Rotation is orthogonal to `limit=` (which still means
"stop tracing at M instructions total") and to `trigger=` (the dormant
pre-trigger phase consumes no chunk instructions).

- **Naming.** When rotating, every chunk — including the first — is
  named `trace_vcpu<V>_c<KKKKK>.raw.zst`: `_c` is a "contiguous chunk"
  token, deliberately distinct from the PIN tracer's skip-gapped
  `_s<sid>` samples (rotation cuts a contiguous stream with no
  inter-chunk gap; PIN's `_s` samples skip instructions between them).
  `<KKKKK>` is the 0-indexed chunk counter, zero-padded to 5 digits
  (`%05u`), so a directory listing sorts correctly up to 100 000
  chunks. When `rotate=0`, the filename is the unchanged plain
  `trace_vcpu<V>.raw.zst` — no `_c` suffix ever appears in non-rotated
  output.
- **Manifest.** When rotating, each traced vCPU also gets a
  `trace_vcpu<V>_manifest.txt`: a header comment line
  (`# vcpu <V> rotation manifest: chunk file start_insn insn_count
  comp_bytes`) followed by one row per **non-empty** chunk, columns
  `chunk file start_insn insn_count comp_bytes`. `comp_bytes` is the
  chunk file's exact on-disk (compressed) size. A line is appended only
  after its chunk's zstd stream is finalized and the file closed, so a
  manifest line guarantees that chunk is complete and valid. The manifest
  is only written when `rotate>0`. The one exception: if the trigger
  never fires, chunk 0's file exists on disk (header only, finalized at
  exit) but is *not* listed in the manifest — directory-glob consumers
  will see one more file than manifest-driven consumers.
- **Every chunk is a standalone v3 file.** No changes were needed in
  `trace_inspector`, `trace_filter`, or `converter/raw2champsim` — each
  chunk carries its own valid 16-byte v3 header and can be inspected,
  filtered, or converted exactly like an un-rotated `trace_vcpu<V>.raw.zst`.
- **Two stateful-consumer caveats.** Both tools below process a single
  file's record stream with a small amount of cross-record state, and
  that state resets at each chunk boundary instead of carrying across
  chunks. Neither is a correctness bug in rotation; both are bounded
  and documented here rather than "fixed" (fixing them would mean
  teaching the readers about chunk sequences, which the design
  deliberately avoids):
  - `trace_filter`'s HLT→HLT idle-loop detector resets to ACTIVE at the
    start of every file, so an idle iteration that straddles a chunk
    boundary is not filtered — an under-filter of at most one idle
    iteration per boundary (~1 per 100 M instructions at the capture
    kit's default chunk size). For exact whole-stream filtering parity,
    concatenate the chunks first, then filter.
  - `raw2champsim` sets `branch_taken` by looking ahead to the next
    instruction's IP, so the *last* instruction of every chunk is
    written with `branch_taken=0` (there is no next record to look
    ahead to within that file). This mismarks a taken branch that lands
    on a chunk's final instruction — bounded to at most one instruction
    per chunk boundary. For exact parity, convert the concatenated
    (un-rotated) stream instead of converting chunks independently.

**Install-time banner.** Every run prints its exact capture
configuration to stderr, including the plugin's own build identity:

```
[champsim_tracer] Arch: aarch64 | capture_pa: on | values: on (value_cap=16) | CSTF_COMMIT=<sha>
```

`CSTF_COMMIT` is `git rev-parse --short=12 HEAD` (plus a `-dirty`
suffix for an uncommitted tree) baked into the `.so` as a string
constant at build time by both `build_plugin.sh` and the `Makefile`.
It tells you exactly which commit of `champsim_tracer.c` produced a
given trace — `scripts/capture-kit/configure_tracer.sh` recovers it
from the `.so` via `strings` for the provenance sidecar it writes.

### `trace_inspector.c` → `trace_inspector`

Offline validator/inspector for `.raw` and `.raw.zst` files.
Auto-detects the format from the first four bytes, and reads both v2
and v3 (any unknown version or arch byte is a clean error naming the
value and the supported set — it never silently misparses a newer
format). For v3 files it decodes and prints arch, `has_pa`,
`has_values`, `value_cap`, a PA-valid percentage and IO-access count,
and — for `arch=aarch64` only — a sanity line
(`sanity: instr_size!=4 in N/M records (X.XX%)`) flagging non-4-byte
instructions, which on AArch64 means T32/A32 EL0 code is present.

Usage:

```bash
# Summary (record count, header validity, first/last IP, memory-op stats)
./trace_inspector traces/trace_vcpu0.raw.zst

# Verbose — print each instruction record with decoded memory values
# (and, for v3 files with PA capture on, each op's PA/valid/io state)
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

Reads v2 and v3 (version and arch whitelisted like the inspector); v3
framing is conditional on the file header's `has_pa` bit (8 extra PA
bytes per mem-op when set), and filtering semantics for `arch=x86_64`
are byte-for-byte identical to v2 — `has_pa` only changes record
framing, never a filtering decision. **`arch=aarch64` files are a hard
error**: the HLT-based idle heuristic does not transfer to AArch64's
`WFI`/`WFE`/`WFIT` idle instructions without validation against real
A64 traces, and silently filtering nothing would be worse than
refusing to run (see
`docs/superpowers/specs/2026-07-06-aarch64-capture-kit-design.md`,
§4.3).

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
make QEMU_PREFIX=~/softwares/qemu-9.2.4/build

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
./build_plugin.sh                          # uses default path
./build_plugin.sh ~/softwares/qemu-9.2.4   # explicit source path
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
