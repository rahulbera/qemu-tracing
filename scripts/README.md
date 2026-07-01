# scripts/

## Goal

Top-of-pipeline **bash launchers** for the QEMU-based tracing flow.
Three scripts that cover the three ways you'll invoke QEMU in this
project: boot fresh under KVM, restore a snapshot under KVM, and
restore a snapshot under TCG *with the tracing plugin attached*.

## How this fits into the repo

These scripts are what you actually run on the host to move a workload
through the pipeline:

```
scripts/boot_kvm.sh          →  fresh guest VM under KVM (setup, install workload)
                                     │
                                     ▼   (savevm from QEMU monitor)
                                 snapshot named e.g. "scylla_run"
                                     │
        ┌────────────────────────────┴─────────────────────────────┐
        ▼                                                          ▼
scripts/restore_kvm.sh <ckpt>                         scripts/boot_tcg_trace.sh <limit> <ckpt>
    (KVM, fast — verify workload)                        (TCG + plugin, the actual tracing run)
                                                              │
                                                              ▼
                                                    plugin writes .raw.zst
```

All three scripts share a common set of QEMU flags (CPU model,
disable-kvm-features, port forwards, monitor/QMP sockets) so snapshots
taken under `boot_kvm.sh` load cleanly under either `restore_kvm.sh`
or `boot_tcg_trace.sh`.

## Files

### `boot_kvm.sh`

Boots the guest VM under KVM from scratch (no `-loadvm`). Use this
when setting up a new workload, installing packages, or reaching a
warm state you plan to snapshot.

Layout (7 vCPUs — ScyllaDB style):
- vCPU 0: OS / bootstrap (not traced)
- vCPUs 1–4: ScyllaDB shards (traced under TCG)
- vCPUs 5–6: benchmark client + OS housekeeping (not traced)

QEMU CPU model: `Haswell` with a long list of KVM-specific features
disabled (`kvmclock=off`, `kvm-asyncpf=off`, etc.). This is
intentional — features that only KVM implements would leave TCG
unable to restore the snapshot. Read `docs/pipeline/kvmclock-patch-details.md`
for why kvmclock in particular is worth its own document.

Port forwards: `2222→22` (SSH), `9042→9042` (CQL / ScyllaDB).

Monitor: telnet `127.0.0.1:4444`. QMP: TCP `127.0.0.1:4445`.

Usage:

```bash
./boot_kvm.sh
```

No arguments. Take snapshots from the QEMU monitor (`savevm <name>`).

### `restore_kvm.sh`

Loads a named snapshot under KVM. Same QEMU flags as `boot_kvm.sh`;
adds `-loadvm $1`. Use this to sanity-check that a snapshot is intact
before spending hours running it under TCG.

Usage:

```bash
./restore_kvm.sh scylla_run
```

### `boot_tcg_trace.sh`

**The actual tracing run.** Loads a snapshot under TCG multi-threaded
mode, attaches `plugin/champsim_tracer.so`, and writes per-vCPU
`.raw.zst` files under `~/qemu-tracing/traces/`. Uses the
plugin's `trigger=/tmp/trace_start` mode — tracing does not begin
until the file appears on the host, letting you defer the start of
tracing until the workload reaches steady state inside the (slow)
TCG-restored VM.

Layout: same 7-vCPU model, `-cpu Haswell` (with `hle/rtm/pcid/invpcid/tsc-deadline` off for TCG compatibility).

Cleans previous traces in `~/qemu-tracing/traces/` before starting.

Usage:

```bash
# Signature: ./boot_tcg_trace.sh [instruction_limit_per_vcpu] [checkpoint_name]

./boot_tcg_trace.sh 1000000 scylla_run     # 1 M insns per vCPU (smoke test)
./boot_tcg_trace.sh 200000000 scylla_run   # 200 M insns per vCPU (production)
./boot_tcg_trace.sh 0 scylla_run           # unlimited (until VM shutdown)
./boot_tcg_trace.sh                        # defaults: 1 M, no checkpoint (won't work)
```

To actually start tracing after the VM has settled:

```bash
touch /tmp/trace_start
```

The plugin polls once every 10 M instructions across all vCPUs
(≈once per wall-clock second under TCG). Traces land at
`~/qemu-tracing/traces/trace_vcpu<N>.raw.zst`.

## How to use

Full pipeline flow — pick up the pieces you need:

```bash
# --- One-time setup: bring up a fresh VM and install the workload. ---
cd scripts/
./boot_kvm.sh
#   (inside the VM: install workload, load data, warm up)
#   (from QEMU monitor on 127.0.0.1:4444: savevm scylla_run)

# --- Later: verify snapshot loads cleanly under KVM. ---
./restore_kvm.sh scylla_run
#   (inside the VM: confirm workload responds normally)
#   quit QEMU

# --- The tracing run: same snapshot under TCG with the plugin. ---
./boot_tcg_trace.sh 200000000 scylla_run
#   (inside the guest — via SSH on port 2222 — wait for workload steady state)
#   (from another host shell:)
touch /tmp/trace_start
#   (QEMU exits when the per-vCPU instruction limit is reached, then:)
ls -lh ~/qemu-tracing/traces/
```

Then hand off to `plugin/trace_inspector` (validate), optionally
`plugin/trace_filter` (strip idle-loop noise), and finally
`converter/raw2champsim` (produce the ChampSim v2 file).

## Notes on scripts vs docs/pipeline/boot-commands.md

`docs/pipeline/boot-commands.md` is a hand-typed cheat sheet that
predates these scripts and reflects the older 5-vCPU Memcached
layout. When something in the two disagrees, **the scripts are
authoritative** — they're what actually gets executed. The doc is
kept because it's a useful compact reference and shows the deferred-
tracing pattern (`trigger=/tmp/trace_start`) explicitly.
