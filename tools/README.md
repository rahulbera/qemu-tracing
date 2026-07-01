# tools/

## Goal

Workload-side helpers meant to run **inside the QEMU guest VM** (or, in
some cases, on a machine that talks to the guest). Everything under
this directory is code you install and execute in the guest as part of
setting up a workload — not code that runs on the host as part of the
tracing infrastructure.

## How this fits into the repo

The host-side pipeline is in `plugin/`, `converter/`, and `scripts/`.
Those tools run on the host and don't know anything about the workload
inside the guest. To exercise the workload — load data, generate a
realistic request stream, warm the cache before you take a snapshot —
you need a *client* running inside (or against) the guest, and that's
what lives here.

Each subdirectory under `tools/` is one workload client. The client is
usually a native C program (Java clients start a JVM at 30–50× under
TCG, which is a waste of tracing budget) with support for:

- deterministic random / Zipfian data generation,
- CPU pinning of its worker threads,
- separate load / warmup / run modes so the same binary can populate
  the DB, prime the cache, and drive the ROI.

## Files and subdirectories

### `scylla_bench/`

Native-C benchmark client for ScyllaDB using the DataStax
`libcassandra` driver. Modes:

- `--mode=load` — bulk-insert records via `INSERT ... VALUES ...`.
- `--mode=warmup` — sequential-scan read pass to prime the cache.
- `--mode=run` — steady-state read/write mix with configurable
  read ratio and Zipfian skew.

Key format is `user0000001..user5000000` (7-digit zero-padded), which
is the same convention adopted by the RocksDB driver — so any Zipfian
analysis tooling works across both workloads. See
`scylla_bench/scylla_bench_build.md` for build steps inside the guest
and full CLI reference.

The Zipfian generator implementation (`zipfian_init`, `zipfian_next`
with FNV-1a scrambling, YCSB-style) that lives inside
`scylla_bench.c` is the reference implementation reused by other
workloads.

## How to use

The intended flow for a new workload client:

1. Add a subdirectory here for the workload (e.g. `tools/redis_bench/`).
2. Write the client as a native C or C++ program. Do not add a Java
   or Python client here — the whole point of `tools/` is to keep
   the guest-side workload driver lightweight enough to survive TCG.
3. Include a build doc (`<workload>_build.md`) that documents
   inside-guest install steps and CLI examples.
4. Commit the client's Zipfian implementation next to it. Cross-tool
   consistency matters more than deduplication here — a bare guest VM
   pulling from disparate git repos is friction.
5. Reference the client from the workload's setup doc in
   `docs/workloads/`.

Currently there is one client; new clients can be added freely.
