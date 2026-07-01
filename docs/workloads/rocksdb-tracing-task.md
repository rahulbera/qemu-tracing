# RocksDB Key-Value Store Instruction Trace Generation — Claude Code Task Spec

## Migration Context

This document specifies a task for Claude Code to execute on a machine with
direct access to the codebase and toolchain. It covers installing RocksDB,
writing a driver program with realistic workload patterns, and end-to-end
tracing using an existing Intel PIN tool.

**This task is part of a larger research project** building ChampSim-compatible
instruction traces from real server workloads. The QEMU-based pipeline
(documented in `qemu-tracing-methodology.md` in this project) handles
multi-threaded server workloads (Memcached). This task uses Intel PIN instead
because RocksDB is a standalone C++ library — PIN is faster, simpler, and
avoids all QEMU/TCG snapshot complexity.

---

## 1. Background and Motivation

### What We're Doing

Generating multi-threaded ChampSim instruction traces from RocksDB key-value
operations with realistic (Zipfian) access distributions. These traces are
used for microarchitectural simulation studies including:

- Cache/memory hierarchy behavior under LSM-tree database workloads
- Value prediction and memory renaming (requires memory values in traces)
- Data-dependent prefetching analysis
- NUCA (Non-Uniform Cache Architecture) simulation — data sharing between
  foreground worker threads operating on a shared block cache

### Why PIN (Not QEMU)

- RocksDB is pure C++ — PIN handles it perfectly
- PIN ROI markers skip the data loading phase and trace only the steady-state
  Zipfian query loop
- ~5-10x overhead vs QEMU's 50-150x TCG overhead
- No VM, no snapshots, no guest tuning required
- The existing PIN tracer already supports multi-threaded tracing, ROI
  markers, and sampled tracing — no modifications needed

### Why RocksDB

- Standalone C++ library — no server, no networking, no benchmark client
- LSM-tree architecture with a configurable block cache creates realistic
  memory hierarchy pressure
- Production-representative: used as storage engine in Meta, Netflix, Uber,
  CockroachDB, MySQL (MyRocks), Ceph
- Deterministic with compaction disabled: same binary + same seed = same trace
- RocksDB is Facebook's production-hardened fork of LevelDB, with explicit
  multi-threaded foreground operations and a large shared block cache — ideal
  for studying inter-thread cache sharing patterns

### Design Decision: Foreground Threads Only, No Background Compaction

Background compaction threads are explicitly disabled during the ROI using
`db->DisableAutoCompactions()`. This is a first-class RocksDB API — the DB
remains fully readable and writable (memtables flush to L0 normally), but no
background compaction kthreads wake up.

**Rationale:** At the target trace scale (8B instructions per thread), even a
pre-settled DB will trigger compaction from 5% write traffic during the trace
window. Disabling compaction gives fully deterministic, reproducible traces
of the foreground read/write hot path — the most research-relevant behavior
for cache hierarchy and NUCA studies.

---

## 2. Existing Artifacts

### 2.1 PIN Tracer (Already Available — No Modifications Needed)

The existing `champsim_tracer_mt_sampled.cpp` supports:
- Multi-threaded tracing with per-thread output files
- ROI markers: `__pin_roi_begin()` and `__pin_roi_end()` to bracket the
  region of interest
- TRACE-granularity instrumentation for low overhead
- Sampled trace output: skip N instructions, trace M instructions, repeat
  for K samples
- Per-thread state machine for phase management

> **⚠️ ASK THE USER before starting:**
> 1. The location of the compiled PIN tracer `.so` on this machine
> 2. The Intel PIN SDK installation path (`$PIN_ROOT`)
> 3. The exact PIN knob names for: skip count, trace count, sample count
>    (the tracer uses `-i`, `-t`, `-n` but confirm the exact names)
> 4. Whether the ROI mechanism uses `__pin_roi_begin`/`__pin_roi_end` symbol
>    names or a different approach (e.g., address-based, magic instruction)
>
> Do not proceed with Task 2 until these are confirmed.

### 2.2 Related Project Files

- `qemu-tracing-methodology.md` — Full QEMU pipeline methodology
- `faiss-tracing-task.md` — FAISS tracing task (same PIN infrastructure)
- `scylla_bench.c` — ScyllaDB benchmark client (reuse Zipfian generator
  and key format from this file)

---

## 3. Task Breakdown

### Task 1: Install RocksDB

**Goal:** Build and install the RocksDB C++ library from source.

```bash
# Install build dependencies
sudo apt-get update
sudo apt-get install -y \
    cmake build-essential \
    libgflags-dev libsnappy-dev zlib1g-dev \
    libbz2-dev liblz4-dev libzstd-dev

# Clone RocksDB
git clone https://github.com/facebook/rocksdb.git
cd rocksdb

# Build as shared library (Release mode)
mkdir build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DROCKSDB_BUILD_SHARED=ON \
    -DWITH_GFLAGS=ON \
    -DWITH_SNAPPY=ON \
    -DWITH_LZ4=ON \
    -DWITH_ZSTD=ON \
    -DWITH_TESTS=OFF \
    -DWITH_TOOLS=OFF \
    -DWITH_BENCHMARK_TOOLS=OFF
cmake --build . -j$(nproc)
sudo cmake --install .
sudo ldconfig
```

**Verify installation:**

```bash
ls /usr/local/lib/librocksdb*
ls /usr/local/include/rocksdb/db.h
```

**If RocksDB is already installed:** Skip the build, verify the install
path and header availability, and proceed to Task 2.

---

### Task 2: Write the RocksDB Driver

**Goal:** A standalone C++ program that:
1. Pre-populates a RocksDB instance with N key-value pairs (load phase)
2. Runs a full manual compaction to settle the DB (pre-ROI settling)
3. Warms the block cache with a sequential read pass (warmup phase)
4. Runs another manual compaction to settle any post-warmup activity
5. Disables auto-compaction
6. Enters the PIN ROI: N foreground threads run a Zipfian read/write loop

#### 2.1 Architecture

```
Phase 1 — Load:
  Sequential WriteBatch inserts, num_records total
  Progress printed every 100K records

Phase 2 — Compaction settle:
  db->CompactRange(CompactRangeOptions(), nullptr, nullptr)
  Block until complete
  Verify: rocksdb.num-files-at-level0 == 0

Phase 3 — Warmup:
  Sequential iterator scan over all keys (pulls everything into block cache)
  Print cache hit rate after completion

Phase 4 — Post-warmup settle:
  db->CompactRange(CompactRangeOptions(), nullptr, nullptr)
  Block until complete

Phase 5 — Disable compaction:
  db->DisableAutoCompactions()

Phase 6 — ROI (traced by PIN):
  __pin_roi_begin()
  Launch N worker threads, each running Zipfian read/write loop
  Join threads after roi_ops total operations
  __pin_roi_end()

Phase 7 — Cleanup:
  db->EnableAutoCompactions()
  delete db
```

#### 2.2 Key Design Requirements

**PIN ROI Markers**

Define as weak noinline symbols (verify exact names with user first):

```cpp
extern "C" {
    void __attribute__((noinline)) __pin_roi_begin() {
        asm volatile("" ::: "memory");
    }
    void __attribute__((noinline)) __pin_roi_end() {
        asm volatile("" ::: "memory");
    }
}
```

**Key Format**

Use the same format as `scylla_bench.c` in this project for consistency
across workloads:

```cpp
// Zero-padded 7-digit decimal: user0000001 through user5000000
char key_buf[32];
snprintf(key_buf, sizeof(key_buf), "user%07ld", key_id);
rocksdb::Slice key(key_buf, strlen(key_buf));
```

**Zipfian Generator**

Reuse the scrambled Zipfian implementation from `scylla_bench.c` in this
project (YCSB algorithm with FNV-1a scrambling). Copy the `zipfian_t`
struct, `zeta()`, `zipfian_init()`, `zipfian_next()` functions directly
into the driver or extract into a shared header.

**Value Size**

Default 1024 bytes per value to match ScyllaDB's usertable record size.
Values are random alphanumeric data generated once per write.

**Worker Thread Structure**

Each worker thread:
1. Gets its own `zipfian_t` instance (different seed per thread)
2. Pins itself to the assigned CPU via `pthread_setaffinity_np`
3. Runs a tight loop: sample Zipfian key → read or write → repeat
4. Counts operations and signals completion after `roi_ops / num_threads`

All threads are created before `__pin_roi_begin()`. Use a barrier
(`pthread_barrier_t`) to synchronize thread start inside the ROI:

```cpp
// Main thread:
__pin_roi_begin();
pthread_barrier_wait(&start_barrier);  // release all workers
for (int i = 0; i < nthreads; i++) pthread_join(tids[i], NULL);
__pin_roi_end();

// Each worker thread:
pthread_barrier_wait(&start_barrier);  // wait for ROI start
while (ops_done < ops_per_thread && g_running) {
    long key_id = zipfian_next(&zipf);
    if (is_write) rocksdb_put(db, key_id);
    else          rocksdb_get(db, key_id);
    ops_done++;
}
```

**RocksDB Configuration**

```cpp
rocksdb::Options options;
options.create_if_missing = true;

// Explicit multi-threading for foreground operations
options.increase_parallelism(num_threads);

// Large block cache to stress memory hierarchy
rocksdb::BlockBasedTableOptions table_options;
table_options.block_cache = rocksdb::NewLRUCache(
    (size_t)cache_gb * 1024 * 1024 * 1024);
table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
options.table_factory.reset(
    rocksdb::NewBlockBasedTableFactory(table_options));

// Realistic compression
options.compression = rocksdb::kSnappyCompression;

// Large memtable to reduce flush frequency during load
options.write_buffer_size = 256 * 1024 * 1024;  // 256 MB
options.max_write_buffer_number = 4;
```

**Compaction Verification**

After each `CompactRange` call, verify it actually settled:

```cpp
void wait_for_compaction(rocksdb::DB* db) {
    std::string pending;
    while (true) {
        db->GetProperty("rocksdb.compaction-pending", &pending);
        if (pending == "0") break;
        fprintf(stderr, "  Waiting for compaction...\n");
        sleep(2);
    }
    // Also verify L0 is empty
    std::string l0_files;
    db->GetProperty("rocksdb.num-files-at-level0", &l0_files);
    fprintf(stderr, "  L0 files after compaction: %s\n", l0_files.c_str());
}
```

**Progress Reporting**

Print clear progress for each phase to stderr (not stdout):

```
[load]    500000 / 5000000 (10.0%)  ops/s: 48321
[load]    1000000 / 5000000 (20.0%) ops/s: 51204
...
[compact] Running full compaction (L0 -> Lmax)...
[compact] Done. L0 files: 0
[warmup]  Sequential scan: 500000 / 5000000 (10.0%)
...
[warmup]  Done. Cache hit rate: 94.3%
[compact] Post-warmup compaction...
[compact] Done.
[roi]     Auto-compaction disabled.
[roi]     Launching 4 worker threads...
[roi]     ROI started.
```

#### 2.3 Command-Line Parameters

| Flag | Default | Description |
|------|---------|-------------|
| `-d PATH` | `/tmp/rocksdb_trace` | RocksDB database directory |
| `-n N` | `5000000` | Number of key-value pairs to load |
| `-v N` | `1024` | Value size in bytes |
| `-o N` | `100000000` | Total ROI operations across all threads |
| `-r N` | `95` | Read percentage 0-100 |
| `-a F` | `0.99` | Zipfian skew (0.0 = uniform) |
| `-t N` | `4` | Foreground worker thread count |
| `--cpus=C1,C2,...` | required | CPUs to pin threads to (round-robin) |
| `-c N` | `4` | Block cache size in GB |
| `-s N` | `42` | RNG seed |
| `--no-warmup` | false | Skip warmup phase (for testing) |
| `--no-compact` | false | Skip compaction phases (for testing) |

#### 2.4 Build

```bash
g++ -O2 -std=c++17 -o rocksdb_driver rocksdb_driver.cpp \
    -I/usr/local/include \
    -L/usr/local/lib \
    -lrocksdb -lpthread -lsnappy -lz -lbz2 -llz4 -lzstd -ldl
```

Build with `-O2`, not `-O0`. Tracing optimized code is essential for
realistic memory access patterns.

---

### Task 3: End-to-End Testing and Trace Generation

#### 3.1 Validate Driver Standalone (No PIN)

```bash
# Small dataset, fast test — verify all phases run correctly
./rocksdb_driver \
    -d /tmp/rocksdb_test \
    -n 100000 \
    -o 1000000 \
    -r 95 -a 0.99 \
    -t 4 --cpus=0,1,2,3
```

**What to verify:**
- All 4 phases print completion messages
- Compaction shows L0 files = 0 after each phase
- Cache hit rate after warmup > 90%
- ROI completes 1M operations with 0 errors
- Memory footprint is reasonable: `sudo /usr/bin/time -v ./rocksdb_driver ...`

#### 3.2 Validate Key Format

After the small test load:

```bash
# Use RocksDB's ldb tool to spot-check keys
/usr/local/bin/ldb --db=/tmp/rocksdb_test scan --max_keys=5
```

Expected output: keys like `user0000001`, `user0000042`, etc. If ldb is
not installed, verify via a small custom reader or add a `--dump-keys`
flag to the driver.

#### 3.3 First PIN Trace (Small Scale)

```bash
# Validate PIN + driver integration with small instruction count
pin -t /path/to/champsim_tracer.so \
    -i 1000000 -t 1000000 \
    -- ./rocksdb_driver \
       -d /tmp/rocksdb_pin_test \
       -n 100000 -o 10000000 \
       -r 95 -a 0.99 \
       -t 4 --cpus=0,1,2,3

# Verify trace files created (one per traced thread)
ls -lh traces/
```

**What to verify:**
- Trace files are created and non-empty
- Instruction counts match the `-t 1000000` limit
- IP addresses fall within RocksDB's text segment:

```bash
nm -D /usr/local/lib/librocksdb.so | grep -i " Get\| Put\| Seek" | head -20
```

#### 3.4 Production Traces — Full Scale

**Target scale per configuration:**
- 3 samples per thread
- 2B instructions traced per sample
- 1B instructions skipped between samples
- 4 foreground worker threads

Confirm the exact PIN knob names for sample count with the user before
running. The assumed interface is:

```bash
pin -t champsim_tracer.so \
    -i 1000000000 \    # skip 1B instructions between samples
    -t 2000000000 \    # trace 2B instructions per sample
    -n 3 \             # 3 samples
    -- ./rocksdb_driver ...
```

**Configuration A — Read-heavy, Zipfian (primary workload):**

```bash
pin -t /path/to/champsim_tracer.so \
    -i 1000000000 -t 2000000000 -n 3 \
    -- ./rocksdb_driver \
       -d /tmp/rocksdb_prod_read95 \
       -n 5000000 -o 500000000 \
       -r 95 -a 0.99 \
       -t 4 --cpus=0,1,2,3 -c 4
```

**Configuration B — Write-heavy, Zipfian (stress write path):**

```bash
pin -t /path/to/champsim_tracer.so \
    -i 1000000000 -t 2000000000 -n 3 \
    -- ./rocksdb_driver \
       -d /tmp/rocksdb_prod_rw50 \
       -n 5000000 -o 500000000 \
       -r 50 -a 0.99 \
       -t 4 --cpus=0,1,2,3 -c 4
```

**Configuration C — Read-heavy, Uniform (baseline comparison):**

```bash
pin -t /path/to/champsim_tracer.so \
    -i 1000000000 -t 2000000000 -n 3 \
    -- ./rocksdb_driver \
       -d /tmp/rocksdb_prod_uniform \
       -n 5000000 -o 500000000 \
       -r 95 -a 0.0 \
       -t 4 --cpus=0,1,2,3 -c 4
```

**Wall time estimate per configuration:**

| Phase | Estimated time |
|-------|---------------|
| Load 5M records | 5-10 min |
| Full compaction | 10-20 min |
| Warmup (5M reads) | 5-10 min |
| Post-warmup compaction | 5-10 min |
| PIN trace (3×2B insns, 4 threads) | 2-6 hours |
| **Total per config** | **~3-7 hours** |

Run configurations in background with `nohup`:

```bash
nohup ./run_config_A.sh > logs/config_A.log 2>&1 &
```

#### 3.5 Validate Production Traces

```bash
# Check trace file sizes (should be large)
ls -lh traces/

# Verify per-thread instruction counts match expectations
# (use trace_inspector from the QEMU pipeline if available)

# Check branch rates are in expected range (15-25% for database code)
# Check memory op percentage (should be 30-50% for LSM-tree traversal)
```

**Expected trace characteristics for read-heavy Zipfian:**
- Branch rate: 15-25% (many conditional paths in RocksDB block cache and
  bloom filter lookups)
- Memory op rate: 30-50% (block cache traversal + SSTable reads)
- Hot address range: small set of block cache entries repeatedly accessed
  (Zipfian creates clear hot/cold separation)

**Comparison between Zipfian and Uniform:**
- Zipfian: hot block cache entries — higher cache hit rate, tighter
  instruction footprint, more branch prediction regularity
- Uniform: cold misses dominate — lower cache hit rate, more diverse
  memory access patterns

---

## 4. File Organization

```
~/rocksdb-tracing/
├── driver/
│   ├── rocksdb_driver.cpp      # Main driver
│   ├── zipfian.h               # Zipfian generator (from scylla_bench.c)
│   └── Makefile
├── traces/
│   ├── config_A_read95_zipf/   # Per-thread trace files, 3 samples each
│   ├── config_B_rw50_zipf/
│   └── config_C_read95_uniform/
├── logs/
│   ├── config_A.log
│   ├── config_B.log
│   └── config_C.log
├── scripts/
│   ├── run_config_A.sh
│   ├── run_config_B.sh
│   ├── run_config_C.sh
│   └── validate_traces.sh
└── docs/
    └── rocksdb-tracing-task.md
```

---

## 5. Dependencies and Prerequisites

| Dependency | Purpose | Install |
|------------|---------|---------|
| Intel PIN SDK | Instruction tracing | Already installed — confirm path |
| RocksDB (C++) | Key-value store | Build from source (Task 1) |
| libgflags | RocksDB dependency | `apt install libgflags-dev` |
| libsnappy | Compression | `apt install libsnappy-dev` |
| liblz4 | Compression | `apt install liblz4-dev` |
| libzstd | Compression | `apt install libzstd-dev` |
| CMake ≥ 3.16 | Building RocksDB | `apt install cmake` |
| GCC ≥ 9 | C++17 for driver | System default on Ubuntu 20.04+ |

---

## 6. Known Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Compaction fires during ROI despite DisableAutoCompactions | Non-deterministic traces | `DisableAutoCompactions()` is authoritative — memtable flushes still happen but compaction threads do not wake up. Verify with `rocksdb.compaction-pending = 0` during ROI. |
| Block cache too small for working set | Cache thrashing, I/O dominated traces | Default 4 GB cache for 5M × 1 KB records (~5 GB uncompressed). Adjust `-c` flag if needed. Monitor `rocksdb.block-cache-hit` vs `rocksdb.block-cache-miss` stats. |
| PIN overhead + long trace = many hours | Impractical wall time | Use `-n 1` and short `-t` for iteration; run production configs in background with `nohup`. Budget 3-7 hours per configuration. |
| Write-heavy config causes L0 buildup during ROI | Read amplification increases during trace | Auto-compaction is disabled so L0 builds up. Acceptable — the trace captures realistic write-pressure behavior. For cleaner writes, reduce write ratio. |
| DB directory from previous run exists | Stale data contaminates new run | Driver should check for existing DB and warn/exit if `-d` path exists and is non-empty. Add `--overwrite` flag to delete and recreate. |
| CPU pinning fails silently | Threads migrate, traces mix CPUs | Check `pthread_setaffinity_np` return code; abort if it fails. Verify with `taskset -p <tid>` after launch. |

---

## 7. Order of Operations

```
Step 0: Ask user for PIN tracer .so path, PIN SDK path, and knob names
        ↓
Step 1: Task 1 — Install RocksDB from source
        ↓
Step 2: Task 2 — Write rocksdb_driver.cpp
        (extract Zipfian from scylla_bench.c, implement all phases)
        ↓
Step 3: Task 3.1 — Validate driver standalone with small dataset
        Verify: all phases complete, cache warmup > 90%, 0 errors
        ↓
Step 4: Task 3.2 — Verify key format with ldb
        ↓
Step 5: Task 3.3 — First PIN trace (small scale: 1M insns)
        Verify: trace files created, IPs in RocksDB text segment
        ↓
Step 6: Task 3.4 — Production traces (3 configs, background nohup runs)
        ↓
Step 7: Task 3.5 — Validate traces (branch rate, memory op rate, sizes)
```

---

## 8. Success Criteria

- [ ] RocksDB builds and installs cleanly
- [ ] Driver loads 5M records and completes full compaction
- [ ] Block cache hit rate > 90% after warmup
- [ ] Auto-compaction disabled before ROI, verified via
      `rocksdb.compaction-pending = 0`
- [ ] PIN produces per-thread trace files with correct instruction counts
- [ ] Trace IPs fall within RocksDB library text segment
- [ ] Branch rate 15-25%, memory op rate 30-50% in production traces
- [ ] All 3 configurations (read-heavy Zipfian, write-heavy Zipfian,
      uniform) traced with 3 samples × 2B instructions × 4 threads
- [ ] Zipfian vs uniform traces show measurably different cache behavior