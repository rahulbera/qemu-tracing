# DLRM / FBGEMM Embedding Workload Instruction Trace Generation — Claude Code Task Spec

## Migration Context

This document specifies a task for Claude Code to execute on a machine
with direct access to the codebase and toolchain. It covers setting up
FBGEMM, downloading and preparing one day of the Criteo Terabyte click
log dataset, writing a single-threaded C++ driver that exercises
FBGEMM's embedding-bag kernel with realistic per-table configurations
and Zipfian index distributions, and end-to-end testing through the
existing ChampSim PIN tracer.

**This task is part of a larger research project** building
ChampSim-compatible instruction traces from real server workloads. The
QEMU-based pipeline (documented in `qemu-tracing-methodology.md` in
this project) handles multi-threaded server workloads (Memcached,
ScyllaDB, etc.). The FAISS work (`faiss-tracing-task.md`) established
the PIN-based single-threaded pattern. **This DLRM/FBGEMM task follows
the FAISS pattern**: standalone C++ driver, PIN tracer, single thread,
ROI-marked search loop.

A subsequent multi-threaded task using the QEMU pipeline will build on
this work to study inter-query embedding-row sharing for multi-node
memory system modeling. That is explicitly out of scope here — this
task delivers the single-threaded baseline.

---

## 1. Background and Motivation

### What We're Doing
Generating single-threaded ChampSim instruction traces from DLRM-style
embedding-bag inference. The target kernel is FBGEMM's
`EmbeddingSpMDM` (the same kernel Meta runs in production). The
workload exercises the canonical recommendation-system access pattern:
a small number of tables, each looked up many times per batch with
indices drawn from skewed (Zipfian) distributions, with the fetched
rows pooled (summed) per sample.

These traces will be used for microarchitectural simulation studies
including:
- Cache and memory hierarchy behavior across embedding table size
  regimes (cache-resident, LLC-pressured, memory-bound)
- Value prediction opportunity in pooled embedding sums
- Data-dependent prefetching analysis on indexed gathers
- Working-set characterization across realistic per-table heterogeneity

### Why FBGEMM (Not PyTorch DLRM)
- The PyTorch reference DLRM is dominated by the framework's
  dispatcher, autograd, and tensor metadata machinery. Tracing it
  produces traces of PyTorch, not of DLRM.
- FBGEMM's standalone benchmarks drive the production embedding kernel
  directly with no framework. This is the kernel Meta actually runs.
- Pure C++ with no Python in the hot path. PIN handles it cleanly.
- Single executable, single thread, deterministic. Trivial to
  reproduce and to ROI-mark.

### Why Single-Threaded First
- Establishes a clean per-thread baseline of the embedding access
  pattern with no cross-thread interference.
- The questions of cache behavior across table sizes, value-prediction
  opportunity in pooled sums, and prefetcher behavior on indexed
  gathers are all answerable from single-threaded traces.
- Multi-threaded inter-query sharing studies require the QEMU pipeline
  and a different experimental design (see "Out of Scope" below).

### Why Criteo Terabyte (One Day)
- Real production ad-click logs with 26 categorical features — the
  canonical DLRM dataset, used by MLPerf.
- One day (`day_0`) is ~15 GB raw, ~150-200M rows, fully sufficient to
  source millions of realistic index sequences without downloading the
  full multi-day archive.
- Per-table cardinalities and pooling factors observed in the data are
  inherently heterogeneous — some features are single-valued
  (pooling=1), others are multi-valued (pooling 30+). We use this
  natural heterogeneity rather than synthesizing it.

### Out of Scope (For This Task)
- Multi-threaded tracing for inter-query sharing analysis (future
  QEMU-based task)
- The dense MLPs (bottom MLP on dense features, interaction layer, top
  MLP). We focus exclusively on the embedding lookup phase, which is
  the memory-bound part and where the interesting microarchitectural
  behavior lives.
- Online updates / write traffic to embedding tables (inference is
  read-only).
- int8 / quantized embeddings. We use full FP32 to maximize working
  set pressure and to provide a clean baseline.
- GPU paths in FBGEMM (CPU-only build).

---

## 2. Existing Artifacts

### 2.1 ChampSim PIN Tracer (Reused As-Is)

The existing PIN tracer used for the FAISS workload already supports:
- TRACE-granularity instrumentation with `PIN_RemoveInstrumentation()`
  on phase transitions for fast fast-forward
- ROI markers (`__pin_roi_begin()` / `__pin_roi_end()`) recognized by
  the tool — start/stop tracing on these symbols
- Multi-threaded tracing with per-thread output files (we will use it
  in single-threaded mode here, but the multi-thread support is
  inherited)
- The extended ChampSim trace format with memory load/store values
  and privilege bits (developed during the FAISS task)

**No tracer changes are required for this task.** We use the tool
exactly as it stands after the FAISS work.

> **⚠️ ASK THE USER:**
> Before starting, ask the user to provide:
> 1. The path to the existing ChampSim PIN tracer source/binary on
>    this machine (e.g., `~/pin-tracing/champsim_tracer_extended.so`)
> 2. The Intel PIN SDK installation path (e.g., `~/pin-3.30/`)
> 3. Confirmation that the tracer's ROI markers are still
>    `__pin_roi_begin` / `__pin_roi_end` (or what the current symbol
>    names are)
> 4. The PIN command-line flags currently used for output file naming,
>    instruction limits, and any sampling knobs that should be set or
>    cleared for this workload
>
> **Do not assume these from the FAISS task spec — confirm with the
> user.** The user may have renamed symbols or changed flag names
> during the FAISS work.

### 2.2 Project Documentation

- `qemu-tracing-methodology.md` — Multi-threaded QEMU pipeline (not
  used here, but provides context)
- `faiss-tracing-task.md` — Sister task spec for the FAISS workload.
  This DLRM task follows the same overall pattern; refer to it for
  the established conventions on driver structure, ROI marking, and
  PIN invocation.

---

## 3. Task Breakdown

### Task 1: Install Build Dependencies and FBGEMM

**Goal:** Build FBGEMM from source as a static library and verify its
standalone embedding benchmark runs.

**Steps:**

```bash
# 1. System dependencies
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build git \
    libomp-dev pkg-config

# 2. Clone FBGEMM (needed for the EmbeddingSpMDM kernel)
cd ~
git clone --recursive https://github.com/pytorch/FBGEMM.git
cd FBGEMM

# 3. Build FBGEMM (CPU only)
mkdir build && cd build
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DFBGEMM_BUILD_TESTS=OFF \
    -DFBGEMM_BUILD_BENCHMARKS=ON \
    -DFBGEMM_BUILD_DOCS=OFF \
    ..
ninja -j$(nproc)

# 4. Verify with the shipped embedding benchmark
./bench/EmbeddingSpMDMBenchmark
```

**Expected:** the benchmark prints throughput numbers across various
configurations without errors.

**Note on the cloned tree:** We do **not** install FBGEMM
system-wide. The driver in Task 4 will link directly against the
in-tree static library and headers. Record the FBGEMM source path
(e.g., `~/FBGEMM`) — the driver Makefile will reference it.

**If the user already has FBGEMM built:** Skip this task; just record
the existing build and source paths.

---

### Task 2: Download and Prepare Criteo Day-0

**Goal:** Obtain one day of the Criteo Terabyte click log and convert
it to a fast binary format the driver can mmap.

#### 2.1 Download

The Criteo Terabyte dataset is hosted by Criteo AI Lab with a
click-through agreement. Files are named `day_0.gz` through
`day_23.gz`. We need only `day_0`.

> **⚠️ ASK THE USER:**
> The Criteo Terabyte download URL requires a click-through agreement
> and the link format may change. Ask the user to:
> 1. Confirm the current download URL for `day_0.gz` (typically
>    obtained from https://ailab.criteo.com/download-criteo-1tb-click-logs-dataset/)
> 2. Confirm whether they have already downloaded it locally and its
>    path on this machine, in which case skip the download

If downloading from scratch:

```bash
mkdir -p ~/criteo-1t/raw
cd ~/criteo-1t/raw
# URL provided by user
wget -c '<USER-PROVIDED-URL>/day_0.gz'
gunzip -k day_0.gz
# Result: ~/criteo-1t/raw/day_0  (~150-200M lines, ~15 GB)
```

#### 2.2 Schema

Each line of `day_0` is tab-separated:

```
<label>  <int_1> ... <int_13>  <cat_1> ... <cat_26>
```

- `label` is 0 or 1 (clicked or not)
- `int_1` .. `int_13` are integer dense features (may be empty)
- `cat_1` .. `cat_26` are 8-character lowercase hex strings (may be
  empty), each representing a hashed categorical value

Empty fields are denoted by adjacent tab characters.

#### 2.3 Preprocessing into a Driver-Friendly Binary Format

The hex categorical strings are inconvenient for the driver. Convert
them to dense integer indices per table, and write a compact binary
file the driver can mmap.

Write a small Python (or C++) preprocessing tool
`scripts/preprocess_criteo.py` that:

1. Streams `day_0` line by line
2. For each of the 26 categorical columns, maintains a hash-string to
   integer-id dictionary (assigning a new ID on first sight)
3. Emits a binary file `criteo_day0.bin` with the following layout:

```
Header (64 bytes, padded):
    uint32_t magic         = 0x43524954  ("CRIT")
    uint32_t version       = 1
    uint64_t num_samples
    uint32_t num_cat_tables = 26
    uint32_t reserved_pad[12]

Per-table cardinality table (26 × uint64_t):
    cardinality[0..25]   # number of distinct IDs observed per table

Per-sample records (num_samples × 26 × uint32_t):
    cat_id[sample][table]   # -1 (0xFFFFFFFF) if missing in source
```

For this task we deliberately ignore the label and the dense features
— we only need the categorical IDs to drive embedding lookups.

4. Also emits a side text file `criteo_day0_stats.txt` containing per
   table:
   - Cardinality (distinct IDs observed)
   - Total non-missing occurrences
   - Top-10 most frequent IDs and their counts (useful for sanity
     checking the Zipfian skew later)

**Why preprocess at all:** parsing 15 GB of tab-separated text per
trace run is wasteful. The binary file is a few GB and mmap-friendly,
and the cardinalities are the input to sizing the embedding tables.

**Memory note:** Building 26 hash-string-to-id dictionaries over
~150-200M rows fits comfortably in 16-32 GB RAM. If the host is
memory-constrained, the script can do two passes (first to learn IDs,
second to emit the binary).

#### 2.4 Verification

After preprocessing:

```bash
ls -lh ~/criteo-1t/criteo_day0.bin     # expect a few GB
cat ~/criteo-1t/criteo_day0_stats.txt  # 26 tables of stats
```

Per-table cardinalities should range from very small (a handful, e.g.
device type) to very large (millions, e.g. user-side hashes). The
heterogeneity is exactly what makes the workload interesting.

---

### Task 3: Establish Per-Table Configuration from the Data

**Goal:** Derive the embedding table sizing, pooling factors, and
Zipfian skew parameters from the actual Criteo statistics rather than
inventing them.

**Why this matters:** The whole point of using a real dataset is that
the per-table heterogeneity (cardinality, frequency distribution,
multi-valuedness) reflects reality. We extract those properties from
day-0 and use them to configure the driver's embedding tables and
query generator.

**Note on pooling in raw Criteo:** The raw Criteo Terabyte dataset has
**one categorical value per feature per sample** (pooling factor = 1
for every table in the source data). MLPerf's "multi-hot" variant of
the workload synthesizes per-feature pooling factors on top of this
raw single-hot data, because production DLRM features are typically
multi-valued and a pooling factor of 1 understates the embedding
access intensity.

We follow the MLPerf approach: take the per-table cardinalities from
the actual data, but **assign per-table pooling factors from a
realistic, heterogeneous set** when generating queries. Suggested
defaults (configurable):

| Feature class      | Tables (example) | Pooling factor |
|--------------------|------------------|----------------|
| Single-valued      | 6 tables         | 1              |
| Lightly pooled     | 10 tables        | 5–20           |
| Heavily pooled     | 8 tables         | 30–80          |
| Very heavily pooled| 2 tables         | 100–200        |

Total assignment to all 26 tables. The driver should accept a config
file (`config/table_config.json` or similar) that lists per-table
`(cardinality, pooling_factor, zipf_alpha)`. A default config is
generated by `scripts/build_table_config.py` from
`criteo_day0_stats.txt` using the policy above (pooling factor
assignment can be deterministic by table-id ordering of cardinality:
larger tables get heavier pooling).

**Per-table Zipfian skew:** Default `alpha = 1.0` for all tables. This
matches the empirical heavy-tail observed in click-log categorical
features. Override per-table if a particular feature shows a
different skew in the stats file.

---

### Task 4: Write the FBGEMM Driver

**Goal:** A single-threaded C++ driver that builds 26 embedding
tables, generates batches of Zipfian-distributed indices per table,
calls FBGEMM's `EmbeddingSpMDM` kernel inside a ROI-marked loop, and
produces deterministic, reproducible behavior.

#### 4.1 Architecture

```
Phase 1 (before ROI): Parse args, load table config, allocate and
                       initialize 26 embedding tables (FP32, dim=128),
                       initialize per-table Zipfian generators with
                       fixed seed. Pre-generate ALL batches (indices
                       and offsets arrays) so the ROI loop contains
                       no RNG work.

Phase 2 (ROI):         Tight loop over pre-generated batches. For
                       each batch, call EmbeddingSpMDM 26 times (one
                       per table), accumulating outputs into a
                       per-batch result buffer.

Phase 3 (after ROI):   Print summary: total batches, total lookups,
                       wall-clock time, throughput. Free resources.
```

The pre-generation in Phase 1 is important. We do not want index
generation, RNG state updates, or `rand()` calls in the ROI — those
are not part of the workload we are tracing.

#### 4.2 Embedding Tables

- **Number of tables:** 26 (matches Criteo)
- **Dimension:** 128 (configurable, default 128 — gives 512 bytes
  per row = exactly 2 cache lines on 64-byte-line systems)
- **Precision:** FP32 (configurable, default FP32)
- **Per-table row count:** taken from the table config (derived from
  Criteo cardinalities)
- **Initialization:** small random FP32 values, seeded deterministically.
  The actual values do not affect access patterns but they affect
  pooled output values, which matter if the extended trace format
  captures memory load values (it does).
- **Layout:** plain row-major 2D arrays per table, allocated with
  `aligned_alloc` to 64-byte boundary. No fancy NUMA-aware allocation
  — single-threaded, single-socket assumed.

**Memory budget:** Each table contributes `cardinality × 128 × 4`
bytes. With Criteo's largest tables in the millions, total embedding
memory is in the multi-GB range. Driver should print the total
allocated table memory at startup so the user can sanity-check
against available RAM.

#### 4.3 Query Generation

For each batch:
- For each of the 26 tables:
  - Sample `pooling_factor[table]` indices from `Zipf(alpha[table],
    cardinality[table])`
  - Append to the table's `indices[]` array
  - Record the offset for this sample in the table's `offsets[]` array

Indices and offsets are laid out per-table per-batch, exactly in the
shape FBGEMM's `EmbeddingSpMDM` kernel expects.

**Zipfian implementation:** Use the same scrambled-Zipfian algorithm
already implemented in `scylla_bench.c` (in this project) — port the
generator to C++. Precompute zeta(N, alpha) once per table at
startup.

**Determinism:** Single global seed parameter. Each per-table
generator is seeded from the global seed plus the table index, so the
exact index sequence is reproducible across runs.

**Pre-generation memory:** With batch=128, pooling sums to a few
thousand indices per batch across all tables. For `num_batches`
batches, indices arrays total `num_batches × few_thousand × 4` bytes.
For 100k batches this is a few hundred MB — fits in RAM. For 1M
batches consider streaming generation in chunks of, say, 10k batches,
with each chunk pre-generated outside ROI and consumed inside ROI in
sequence.

#### 4.4 ROI Loop

```cpp
__pin_roi_begin();
for (uint64_t b = 0; b < num_batches; ++b) {
    for (int t = 0; t < 26; ++t) {
        // FBGEMM kernel signature (paraphrased):
        //   bool EmbeddingSpMDM<float, int32_t>::operator()(
        //       int64_t output_size,    // = batch_size
        //       int64_t index_size,     // = total indices in this batch for this table
        //       int64_t data_size,      // = cardinality[t]
        //       const float* input,     // = embedding table[t]
        //       const int32_t* indices, // = pre-generated indices[t][b]
        //       const int* offsets_or_lengths,
        //       const float* weights,   // nullptr for unweighted sum
        //       float* output           // = output_buffer[t]
        //   );
        kernel[t](batch_size, batch_index_count[t][b], cardinality[t],
                  table[t], indices_ptr(t, b), offsets_ptr(t, b),
                  nullptr, output_ptr(t, b));
    }
}
__pin_roi_end();
```

**Important:** Construct the FBGEMM kernel objects (`GenerateEmbeddingSpMDM`)
**outside** the ROI. Those constructors JIT-generate code; we don't
want to trace the JIT. Inside the ROI, only the operator() invocations
run.

#### 4.5 Command-Line Arguments

| Flag                  | Default     | Description                                 |
|-----------------------|-------------|---------------------------------------------|
| `--criteo-bin=PATH`   | required    | Path to `criteo_day0.bin`                   |
| `--table-config=PATH` | required    | Path to per-table config JSON               |
| `--num-batches=N`     | 100000      | Number of batches in ROI loop               |
| `--batch-size=N`      | 128         | Samples per batch                           |
| `--embedding-dim=N`   | 128         | Embedding row dimension                     |
| `--seed=N`            | 42          | Global RNG seed                             |
| `--output-prefix=STR` | `dlrm_run`  | Used in summary printing                    |
| `--dry-run`           | off         | Build everything, print plan, skip ROI loop |

**Note:** The `--criteo-bin` path is currently used only to size the
tables (via the cardinalities recorded during preprocessing) and to
provide the popularity distribution per table for Zipfian
parameterization in a future enhancement. The indices used inside the
ROI are generated by the Zipfian generators using the per-table
cardinalities — not by replaying the actual sequence from the file.
This is intentional: it preserves the data's statistical properties
(cardinality, skew) while giving us full control over the access
pattern for reproducibility and parameter sweeps.

If a future task wants to replay the literal index sequence from
day-0 (rather than re-sampling), the `criteo_day0.bin` binary already
contains everything needed to do so — no preprocessing changes
required.

#### 4.6 Build

```bash
g++ -O2 -std=c++17 -fopenmp \
    -I${FBGEMM_SRC}/include \
    -I${FBGEMM_SRC}/third_party/asmjit/src \
    -I${FBGEMM_SRC}/third_party/cpuinfo/include \
    -o dlrm_driver dlrm_driver.cpp \
    ${FBGEMM_SRC}/build/libfbgemm.a \
    ${FBGEMM_SRC}/build/asmjit/libasmjit.a \
    ${FBGEMM_SRC}/build/cpuinfo/libcpuinfo.a \
    -lpthread
```

(Exact link line will need adjustment based on the actual FBGEMM
build artifact layout — check `${FBGEMM_SRC}/build/` after Task 1.)

**Build with `-O2`, not `-O0`.** Production code is what we want to
trace. `-O0` produces artificially register-spill-heavy code with
unrepresentative memory traffic.

---

### Task 5: End-to-End Testing

#### 5.1 Standalone Driver Test (No PIN)

```bash
# Quick correctness test
./dlrm_driver \
    --criteo-bin=$HOME/criteo-1t/criteo_day0.bin \
    --table-config=$HOME/dlrm-tracing/config/table_config.json \
    --num-batches=1000 \
    --batch-size=128 \
    --dry-run

# Real short run
./dlrm_driver \
    --criteo-bin=$HOME/criteo-1t/criteo_day0.bin \
    --table-config=$HOME/dlrm-tracing/config/table_config.json \
    --num-batches=1000 \
    --batch-size=128
```

**What to check:**
- No crashes, no asserts
- Total embedding-table memory printed at startup matches expectation
  (sum across tables of `cardinality × dim × 4`)
- Total lookups in the ROI = `num_batches × sum_over_tables(batch_size
  × pooling_factor[t])`
- Throughput is sane (millions of lookups per second on modern x86)

#### 5.2 PIN-Traced Run

```bash
$PIN_ROOT/pin -t $TRACER_PATH \
    -o traces/dlrm_day0 \
    -- ./dlrm_driver \
        --criteo-bin=$HOME/criteo-1t/criteo_day0.bin \
        --table-config=$HOME/dlrm-tracing/config/table_config.json \
        --num-batches=10000 \
        --batch-size=128 \
        --seed=42
```

(Substitute the actual tracer path and ROI/limit flag names per the
user's earlier answers in §2.1.)

**Trace validation:**
- Trace file is non-empty, of the expected size given the format
- IPs in the trace fall within the FBGEMM library text segment (use
  `nm` or `objdump` on the driver binary and on any FBGEMM `.a`
  archives that were linked in to confirm)
- Memory access intensity is high — DLRM embedding lookups should
  generate one memory op per few instructions, not the typical 30%
- Branch rate is moderate (the inner loop is straight-line gather)
- If the extended trace format with values is in use, verify the
  loaded values appear plausible (random-looking FP32 patterns from
  the embedding table init)

#### 5.3 Sweep Across Operating Points (Optional, Recommended)

Generate three traces representing the canonical regimes:

| Regime              | How to set up                                                                  |
|---------------------|--------------------------------------------------------------------------------|
| Cache-resident      | Override table config to cap all cardinalities at e.g. 4096. Total ~5 MB.      |
| LLC-pressured       | Cap cardinalities at e.g. 1M (largest tables). Total ~hundreds of MB to GB.    |
| Memory-bound        | Use the unmodified Criteo cardinalities. Total in multi-GB range.              |
|                     | (May be limited by the available RAM on the host.)                             |

Same seed across all three. Each gives a qualitatively different
microarchitectural picture; the comparison is the scientifically
interesting result.

---

## 4. File Organization

```
~/dlrm-tracing/
├── driver/
│   ├── dlrm_driver.cpp          # Main driver
│   ├── zipfian.h                # Ported from scylla_bench.c
│   ├── criteo_io.h              # Reader for criteo_day0.bin
│   ├── table_config.h           # JSON config loader
│   └── Makefile                 # Build driver against FBGEMM
├── config/
│   ├── table_config.json        # Per-table sizing/pooling/skew
│   └── table_config_small.json  # Cache-resident sweep config
├── scripts/
│   ├── preprocess_criteo.py     # day_0 -> criteo_day0.bin
│   ├── build_table_config.py    # stats -> table_config.json
│   └── run_traces.sh            # Batch tracing across regimes
├── traces/                      # Output traces
│   ├── dlrm_day0_*.champsim
│   └── ...
└── docs/
    └── dlrm-fbgemm-tracing-task.md   # This document

~/criteo-1t/
├── raw/
│   └── day_0                    # ~15 GB raw text
├── criteo_day0.bin              # Preprocessed binary
└── criteo_day0_stats.txt        # Per-table statistics

~/FBGEMM/                        # FBGEMM source tree
└── build/                       # Static libs
```

---

## 5. Order of Operations

```
Step 0: Ask user for ChampSim PIN tracer path, PIN SDK path,
        ROI symbol names, PIN flags. Also Criteo download URL or
        existing Criteo path.
        ↓
Step 1: Task 1 — Build FBGEMM, run shipped EmbeddingSpMDMBenchmark
                  to confirm the build is sane.
        ↓
Step 2: Task 2 — Download (if needed) and preprocess Criteo day_0.
                  Long-running but parallelizable with Task 1.
        ↓
Step 3: Task 3 — Generate table_config.json from Criteo stats.
        ↓
Step 4: Task 4 — Write and unit-test the dlrm_driver.
        ↓
Step 5: Task 5 — Standalone driver test, then PIN-traced runs.
        ↓
Step 6: (Optional) Cardinality sweeps for the three regimes.
```

Tasks 1 and 2 are independent and can run in parallel. Task 3 depends
on the stats file produced by Task 2. Task 4 needs Task 1. Task 5
needs everything.

---

## 6. Known Risks and Mitigations

| Risk                                                  | Impact                              | Mitigation                                                                                  |
|-------------------------------------------------------|-------------------------------------|---------------------------------------------------------------------------------------------|
| Criteo download URL changes or requires new agreement | Blocks Task 2                       | Ask user upfront. If user has the file locally, skip the download.                          |
| Preprocessing 15 GB on a low-RAM host runs out of memory | Stalls Task 2                    | Use the two-pass version of the preprocessor (assign IDs first, emit binary second).        |
| Largest Criteo tables exceed host RAM when allocated as FP32 embeddings | OOM at driver startup | Print total allocation at startup; provide `table_config_small.json` that caps cardinalities. |
| FBGEMM kernel signature changes between FBGEMM versions | Build break in driver             | Pin to a specific FBGEMM commit (record SHA in this doc once chosen).                       |
| PIN ROI marker symbol names differ from FAISS task    | Tracing covers wrong region         | Confirm symbol names with user in Step 0; verify with a small trace and grep for ROI IPs.   |
| FBGEMM JIT runs inside ROI by accident                | Trace polluted with codegen         | Construct kernel objects outside `__pin_roi_begin()`. Verify by inspecting the trace's IP histogram. |

---

## 7. Success Criteria

- [ ] FBGEMM builds and shipped benchmark runs cleanly
- [ ] One day of Criteo Terabyte downloaded and converted to binary
- [ ] Per-table config generated from actual Criteo cardinalities
- [ ] Driver builds and runs standalone with multi-GB embedding tables
- [ ] Driver is deterministic: same seed produces identical lookup
      sequence across runs
- [ ] PIN-traced run produces a valid ChampSim trace
- [ ] At least one full-cardinality memory-bound trace generated (e.g.
      100k batches, batch size 128)
- [ ] Optional: three traces across the cache-resident /
      LLC-pressured / memory-bound regimes for comparative analysis