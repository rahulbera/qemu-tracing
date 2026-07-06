# docs/pipeline/

## Goal

Pipeline-wide references — the "how the QEMU-based ChampSim tracing
infrastructure works" documents. These are authoritative long-form
references for the infrastructure itself, independent of any specific
workload.

## How this fits into the repo

Read these before touching the code in `plugin/`, `converter/`, or
`scripts/`. The pipeline stage document (`pipeline-stages.md`) is the
entry point; the others are targeted references for a specific problem
(kvmclock, idle-loop filtering) or a quick lookup (boot commands).

## Files

### `pipeline-stages.md`

Six-stage summary of the whole pipeline:

- Stage 1: VM setup (guest OS, workload install)
- Stage 2: Snapshot creation (KVM, warm state)
- Stage 3: TCG tracing plugin (`plugin/champsim_tracer.c`)
- Stage 4: TCG tracing run (restore snapshot, capture traces)
- Stage 5: Offline converter (`converter/raw2champsim.c`)
- Stage 6: ChampSim integration

Each stage is marked ✅ complete, 🚫 blocked, or (not started), and
lists key decisions and outputs. Read this first if you want a mental
model of the whole pipeline.

### `boot-commands.md`

Quick reference for the exact QEMU invocations used at each stage:

- KVM boot (setup / snapshot creation)
- KVM boot + `-loadvm` (restore snapshot)
- TCG boot + `-plugin` + `-loadvm` (the tracing run)
- SSH into guest, QEMU monitor, kill QEMU
- List snapshots, inspect traces, rebuild QEMU/plugin

Reflects the 5-vCPU Memcached layout. The `scripts/` directory has
richer, ScyllaDB-oriented (7-vCPU) versions with named CPU models.
When in doubt, prefer `scripts/` — this doc is a hand-typed cheat
sheet.

### `kvmclock-patch-details.md`

Full technical writeup of the snapshot-under-TCG blocker and its fix.
The `kvmclock` device only instantiates under KVM, so its VMState
handler is missing when TCG tries to `loadvm` — QEMU aborts. This
document analyses the snapshot stream format, explains why we can't
skip the section, and walks through the QEMU patch that makes
`kvmclock` instantiable under TCG (skipping only the KVM-specific
runtime initialization).

Read this if you rebuild QEMU or if snapshot loading errors surprise
you.

### `task-tcg-idle-loop-filtering.md`

Design document for `plugin/trace_filter.c`. Explains why TCG traces
contain ~80% kernel-mode instructions (vs ~50% under KVM) — QEMU's TCG
emulates `HLT` rather than physically halting, so the guest kernel's
idle loop generates a continuous stream of useless kernel instructions.
Documents the detection heuristic (kernel-mode runs bounded by `HLT`
with no user-mode instructions between) and the filter's design.

Read this if you're modifying the filter or diagnosing why the trace
volume is unexpectedly high.

### Raw trace format v3 (AArch64 capture kit)

The raw trace format bumped from v2 to **v3**: a per-file arch byte
(x86_64/aarch64), optional guest physical-address capture, and a
`value_cap` byte that separates the format's 64-byte value-buffer
ceiling from the 16-byte cap QEMU's value-capture API actually
supports today. The full byte-level contract is frozen in
`docs/superpowers/specs/2026-07-06-aarch64-capture-kit-design.md`;
`plugin/README.md` has the reader-facing summary and knob reference.
Old v2 files remain readable forever — all three readers
(`trace_inspector`, `trace_filter`, `converter/raw2champsim`)
whitelist versions `{2, 3}`.

### `scripts/capture-kit/`

Not a doc in this directory but the operational counterpart to the
v3 format bump above: a self-contained kit that lets an **AArch64
collaborator** capture traces from their own guest without hand-
assembling plugin knobs — `probe_guest.sh` (runs inside the guest),
`configure_tracer.sh` (runs on the host, emits `run_trace.sh` +
a `trace_metadata.txt` provenance sidecar), and its own README with
the full step-by-step, including the mandatory KVM→TCG smoke test.
See `scripts/README.md` for how it fits alongside the x86 launcher
scripts.

## How to use

- New collaborator? Read `pipeline-stages.md` end-to-end.
- Producing traces? Use `scripts/boot_tcg_trace.sh` and refer to
  `boot-commands.md` when you need to modify a flag.
- Snapshot won't load under TCG? `kvmclock-patch-details.md`.
- Kernel-idle noise in traces? `task-tcg-idle-loop-filtering.md` plus
  `plugin/trace_filter`.
- Tracing an AArch64 guest? Skip straight to `scripts/capture-kit/` —
  don't hand-assemble the plugin knobs from `boot-commands.md`, which
  is x86-only.
