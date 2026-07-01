# tools/scylla_bench/

## Goal

`scylla_bench` is a native-C benchmark client for **ScyllaDB** that
runs inside the QEMU guest VM. It handles the three phases the
QEMU tracing pipeline needs from a workload driver:

- **Load** — bulk-insert records into a keyspace (replaces
  `cassandra-stress` load mode; much faster startup than YCSB).
- **Warmup** — sequential read pass to prime the cache before the
  ROI.
- **Run** — steady-state read/write mix with configurable read
  ratio and Zipfian skew. This is what generates the requests being
  traced.

Design constraint: **must be a native C program**. Java YCSB / Python
`cassandra-driver` start their runtimes at 30–50× under TCG and would
generate hundreds of millions of useless bytecode/interpreter traces
before reaching a single CQL request. Native C is single-digit-percent
overhead across the mode transition.

## How this fits into the repo

Referenced from:

- `docs/workloads/scyllaDB-stage1.md` — ScyllaDB setup and Stage-1
  procedure that calls this client.
- The RocksDB driver at
  `/mnt/sherlock/rahbera/workloadzoo/rocksdb-driver/zipfian.h` reuses
  the Zipfian generator implementation from here verbatim (see the
  header of `scylla_bench.c`), so if you touch the FNV-scrambled
  Zipfian here, keep the RocksDB copy in sync.

## Files

### `scylla_bench.c`

Single-file client. Compiles to `scylla_bench`. Contains:

- **Zipfian generator** (`zipfian_init`, `zipfian_next`, `zeta`,
  `fnv1a_64`). YCSB-style scrambled Zipfian — the FNV-1a scramble
  breaks up the natural key ordering so the hot subset is spread
  across the keyspace, not clustered in the low-numbered keys.
  Suitable for reuse in any workload that wants Zipfian.
- **Key format** — `user0000001..user5000000` (see `#define KEY_FMT
  "user%07ld"`). Zero-padded seven-digit decimal. Any downstream
  Zipfian access-pattern analysis assumes this format.
- **Load mode** — round-robin threads inserting into `ycsb.usertable`
  using prepared statements.
- **Warmup mode** — sequential-scan reads that pull every record
  through the cache.
- **Run mode** — per-thread Zipfian read/write mix, with each thread
  pinned to a specified CPU via `pthread_setaffinity_np`. Runs
  effectively-forever (default 86400 s) so that the QEMU tracing
  window is bounded by the *plugin's* instruction limit, not by the
  workload finishing early.

CLI options:

| Flag | Default | Meaning |
|---|---|---|
| `--mode=` | (required) | `load`, `warmup`, or `run` |
| `--records=` | 5000000 | Number of records |
| `--read-ratio=` | 95 | Read % in `run` mode (0..100) |
| `--zipfian-skew=` | 0.99 | Zipfian α (0 = uniform) |
| `--threads=` | 2 | Worker thread count |
| `--cpus=` | (required) | Comma list of CPUs to round-robin pin to |
| `--duration=` | 86400 | Run-mode duration seconds (effectively infinite) |
| `--host=` | 127.0.0.1 | ScyllaDB coordinator |

### `scylla_bench_build.md`

Build and usage doc for inside-guest use. Covers:

- Installing the DataStax C/C++ driver (`cmake`, `libuv1-dev`,
  `libssl-dev` → `libcassandra.so`).
- Compiling `scylla_bench.c` with the DataStax driver.
- Verifying the build (`./scylla_bench --help`).
- Full worked examples for each mode.
- Explanation of `--cpus=` pinning semantics.
- Note on key-format convention.

## How to use

### Inside the guest

Copy this directory into the guest (via SSH `scp` or a shared filesystem):

```bash
# From the host
scp -P 2222 tools/scylla_bench/scylla_bench.c researcher@localhost:~/
```

Then inside the guest, follow `scylla_bench_build.md`:

```bash
# 1. Install the DataStax driver (one-time).
sudo apt-get install -y cmake g++ make libuv1-dev libssl-dev
git clone https://github.com/datastax/cpp-driver.git
cd cpp-driver && mkdir build && cd build && cmake .. && \
    make -j$(nproc) && sudo make install && sudo ldconfig

# 2. Build the client.
cd ~
gcc -O2 -o scylla_bench scylla_bench.c -lcassandra -lpthread -lm

# 3. Load.
./scylla_bench --mode=load --records=5000000 --threads=2 --cpus=0,5,6

# 4. Warmup.
./scylla_bench --mode=warmup --records=5000000 --threads=2 --cpus=0,5,6

# 5. Run (detached, so it survives the KVM→TCG snapshot boundary).
nohup ./scylla_bench --mode=run \
    --records=5000000 --read-ratio=95 --zipfian-skew=0.99 \
    --threads=2 --cpus=5,6 \
    > /tmp/scylla_bench.log 2>&1 &
```

At step 5 you snapshot the VM (from the host QEMU monitor) with the
benchmark actively running — that's the `roi_running` snapshot the
tracer will restore under TCG later.

### Extending

The Zipfian implementation in this file is the project's canonical
reference. When adding a new workload client to `tools/`, copy the
`zipfian_t` struct and the `zeta` / `zipfian_init` / `zipfian_next_raw`
/ `fnv1a_64` / `zipfian_next` functions verbatim so that access-pattern
comparisons across workloads are apples-to-apples.
