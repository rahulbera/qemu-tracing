# docs/workloads/

## Goal

Per-workload playbooks and design specs. Each document either walks you
through setting up one target workload and producing traces from it, or
specifies the design for a workload driver so that a collaborator (human
or agent) can implement it end-to-end.

## How this fits into the repo

Once you understand the pipeline (`docs/pipeline/`), pick a workload
here and follow that document. Two kinds of workloads live in this
directory, and they differ in **which tracer they use**:

| Tracer | Why | Workloads here |
|---|---|---|
| **QEMU TCG** (this repo's pipeline) | Guest kernel work is on the hot path (network stack, syscalls). PIN would miss it. | Memcached, ScyllaDB |
| **Intel PIN** (in `arishem/champsim/tracer/`) | Standalone C++ program, mostly user-mode. QEMU's 50–150× slowdown is not worth paying. | FAISS, DLRM, RocksDB |

The PIN workloads are documented here because the trace *format* (v2:
512-byte records, memory values, privilege bit) is shared with the QEMU
pipeline. The actual PIN tracer source and workload drivers for
FAISS/DLRM/RocksDB live outside this repo (on `/mnt/sherlock/rahbera/workloadzoo/` and `arishem/champsim/tracer/`).

## Files

### QEMU-traced workloads

#### `memcached-stage1.md`

Full step-by-step for setting up Memcached inside the QEMU guest VM,
including host QEMU install, guest OS setup (cloud-init or GUI paths),
guest tuning (ASLR off, THP off, swap off), Memcached install with
thread pinning, YCSB install, and memtier_benchmark install.

#### `memcached-stage2.md`

Identifying the region of interest and creating the golden VM
snapshot. Covers the 5-vCPU layout, YCSB-load-only / memtier-run split,
snapshot creation and validation, and the `roi_ready` / `roi_running`
snapshot pair.

#### `memcached-stage4.md`

Restoring the snapshot under TCG mode and starting the tracing run.
Uses the deferred-trigger mode of the plugin so tracing starts when
`touch /tmp/trace_start` is issued from the host.

*(There is no `stage3` document — Stage 3 is the plugin implementation
itself, covered by the source in `plugin/champsim_tracer.c` and the
design notes in `docs/pipeline/`.)*

#### `scyllaDB-stage1.md`

Analogous Stage-1 doc for ScyllaDB. Introduces the 7-vCPU layout (vCPU
0 bootstrap, vCPUs 1–4 ScyllaDB shards, vCPUs 5–6 client + OS) and
switches to a named QEMU CPU model (`Skylake-Client`) so that the same
snapshot loads under both KVM and TCG. Covers ScyllaDB install and
tuning inside the guest. The workload driver referenced here
(`scylla_bench.c`) lives in `tools/scylla_bench/`.

### PIN-traced workloads (design specs)

#### `faiss-tracing.md`

Task spec for the FAISS vector-similarity-search tracing pipeline.
Covers PIN tracer modifications for extended format (memory values),
FAISS install, dataset download (SIFT-1M, Deep-10M), driver design
with Zipfian query distribution, and multi-index-type comparison. The
actual driver lives at `/mnt/sherlock/rahbera/workloadzoo/faiss-driver/`.

#### `dlrm-tracing-task.md`

Task spec for tracing FBGEMM embedding-bag kernels (the DLRM inner
loop). Covers Criteo dataset preparation, per-table
Zipfian-distributed embedding lookups, and single-threaded tracing
through the PIN tracer. Uses the same tracer as FAISS. The driver
lives at `/mnt/sherlock/rahbera/workloadzoo/dlrm-driver/`.

#### `rocksdb-tracing-task.md`

Task spec for tracing RocksDB key-value operations with realistic
Zipfian access. Covers RocksDB install, driver design (load / compact
/ warmup / disable-auto-compactions / ROI), and the multi-threaded PIN
tracer. Reuses the Zipfian generator from
`tools/scylla_bench/scylla_bench.c`. The actual driver lives at
`/mnt/sherlock/rahbera/workloadzoo/rocksdb-driver/`.

## How to use

- **Reproducing an existing workload's traces?** Follow the file for
  that workload from top to bottom. The QEMU workloads have complete
  step-by-step commands; the PIN specs point at driver source that
  already exists.
- **Adding a new workload?** Decide QEMU or PIN based on the
  kernel-mode share of the workload, then use the closest existing
  document as a template.
- **Cross-referencing what the trace format looks like?** The v2 record
  layout is defined in `plugin/champsim_tracer.c` (raw format) and
  `converter/raw2champsim.c` (ChampSim v2 struct). Every workload doc
  points at these files.
