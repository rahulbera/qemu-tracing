# FAISS Vector Database Instruction Trace Generation — Claude Code Task Spec

## Migration Context

This document specifies a task for Claude Code to execute on a machine with
direct access to the codebase and toolchain. It covers setting up FAISS,
writing a driver program with realistic query patterns, modifying an existing
Intel PIN tracer to support an extended trace format, and end-to-end testing.

**This task is part of a larger research project** building ChampSim-compatible
instruction traces from real server workloads. The QEMU-based pipeline
(documented in `qemu-tracing-methodology.md` in this project) handles
multi-threaded server workloads (Memcached, etc.). This task uses Intel PIN
instead, because the target workload (FAISS) is a standalone single-threaded
C++ program — PIN is faster, simpler, and sufficient here.

---

## 1. Background and Motivation

### What We're Doing
Generating single-threaded ChampSim instruction traces from FAISS vector
similarity search over production-scale datasets with realistic (Zipfian)
query distributions. These traces will be used for microarchitectural
simulation studies including:
- Cache/memory hierarchy behavior under different ANN index types
- Value prediction and memory renaming (requires memory values in traces)
- Data-dependent prefetching analysis
- Comparison of memory access patterns across index types (Flat vs HNSW
  vs IVF vs PQ)

### Why PIN (Not QEMU)
- FAISS is pure C++ — PIN handles it perfectly
- Single-threaded — no need for QEMU's full-system multi-vCPU tracing
- PIN ROI markers let us skip index building and trace only the search loop
- ~5-10x overhead vs QEMU's 50-150x TCG overhead
- No VM, no snapshots, no guest tuning — dramatically simpler pipeline

### Why FAISS (Not a Vector DB Server)
- Standalone C++ library — no server, no networking, no benchmark client
- Multiple index types (Flat, HNSW, IVF, PQ) with radically different
  memory access patterns from a single codebase
- Deterministic: same binary + same dataset + same queries = same trace
- Production-representative datasets available (Deep10M, MSTuring, etc.)

---

## 2. Existing Artifacts

### 2.1 PIN Tracer (To Be Modified)

We have an optimized multi-threaded sampled ChampSim PIN tracer
(`champsim_tracer_mt_sampled.cpp`) with these features:
- TRACE-granularity instrumentation (not INS-granularity) for lower overhead
- `PIN_RemoveInstrumentation()` on phase transitions for near-native
  fast-forward speed
- Per-thread state machine: INITIAL_SKIP → TRACING → INTER_SKIP → TRACING → ... → DONE
- Sampled trace output: skip N instructions, trace M instructions, repeat
- Multi-thread safe with per-thread output files

**For this FAISS task, we will use it in single-threaded mode.** The key
modification is extending it to support a new extended trace format that
includes memory load/store values.

> **⚠️ IMPORTANT — ASK THE USER:**
> The exact specification of the new extended ChampSim trace format
> (with memory values, privilege bits, etc.) was developed in a separate
> conversation that Claude Code does not have access to. Before starting
> Task 1, ask the user to provide:
> 1. The `trace_instruction.h` header with the extended `input_instr` struct
> 2. Or a description of the new fields (memory values, privilege bit, etc.)
> 3. The location of the current PIN tracer source file on this machine
>
> From prior conversations, the planned extensions include:
> - A privilege bit per instruction (user=0, kernel=1) — less relevant for
>   PIN (user-mode only) but keep for format compatibility
> - Memory load values (the value loaded from memory)
> - Memory store values (the value being stored to memory)
> - Values stored in little-endian byte order
> - Support for variable-width values (1/2/4/8 bytes for scalar ops)
>
> The user may have finalized this format since the last conversation.
> **Do not assume — ask.**

### 2.2 Project Documentation

- `qemu-tracing-methodology.md` — Full methodology doc (in this project)
- Past conversation history covers: QEMU raw format v2, offline converter
  design, kvmclock patches, Memcached workload setup

---

## 3. Task Breakdown

### Task 1: Modify the Intel PIN Tracer for Extended Trace Format

**Goal:** Extend the existing `champsim_tracer_mt_sampled.cpp` to emit traces
in the new extended format that includes memory values.

**Prerequisites (ask user for):**
- Location of current PIN tool source on this machine
- Intel PIN SDK installation path
- The new extended trace format specification (`trace_instruction.h`)
- Whether the user wants values for ALL memory ops or only loads

**What to modify:**
1. In the memory read callback (`RecordMemRead` or equivalent): capture the
   value at the accessed address using `PIN_SafeCopy()` into the trace record
2. In the memory write callback (`RecordMemWrite` or equivalent): capture the
   value being written using `PIN_SafeCopy()` from the source
3. Update the trace record emission to write the new extended format fields
4. Add a PIN knob (`-values 0/1`) to optionally disable value capture for
   backward compatibility with vanilla ChampSim
5. Add PIN ROI support: recognize `__pin_roi_begin()` and `__pin_roi_end()`
   markers in the instrumented binary to start/stop tracing

**Value capture approach:**
```
For loads:  Read memory at accessed address AFTER the load executes
            → gives the loaded value
For stores: Read the value being stored
            → use PIN_GetContextRegval() or read from source register
```

**Key concern:** `PIN_SafeCopy()` handles unmapped/guard pages gracefully
(returns bytes copied, doesn't crash). Always check return value.

**Testing:** Compile a trivial C program that does known loads/stores, trace
it, verify values in the trace match expected values.

---

### Task 2: Set Up FAISS

**Goal:** Install FAISS C++ library from source on the machine, ready for
the driver to link against.

**Steps:**

```bash
# 1. Install dependencies
sudo apt-get update
sudo apt-get install -y cmake build-essential libopenblas-dev liblapack-dev

# 2. Clone and build FAISS (CPU only — no GPU needed for tracing)
git clone https://github.com/facebookresearch/faiss.git
cd faiss
cmake -B build \
    -DFAISS_ENABLE_GPU=OFF \
    -DFAISS_ENABLE_PYTHON=OFF \
    -DBUILD_TESTING=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON
cmake --build build -j$(nproc)
sudo cmake --install build
```

**Verify installation:**
```bash
# Should find libfaiss.so and headers
ls /usr/local/lib/libfaiss*
ls /usr/local/include/faiss/IndexFlat.h
```

**If the user already has FAISS installed:** Skip this task, just verify
the install path and header availability.

---

### Task 3: Download a Production-Scale Dataset

**Goal:** Obtain a real-world vector dataset large enough to stress the
memory hierarchy (multi-GB footprint).

**Recommended datasets (pick based on available disk/RAM):**

| Dataset | Vectors | Dims | Raw Size | Download |
|---------|---------|------|----------|----------|
| SIFT-1M | 1M | 128 | ~0.5 GB | http://corpus-texmex.irisa.fr/  (ANN_SIFT1M) |
| Deep-1M | 1M | 96 | ~0.4 GB | Subset of Deep1B |
| Deep-10M | 10M | 96 | ~3.6 GB | Yandex Deep1B subsets |
| GIST-1M | 1M | 960 | ~3.6 GB | http://corpus-texmex.irisa.fr/ |

**Start with SIFT-1M for validation** (small, fast to download, well-understood),
then scale to Deep-10M or GIST-1M for production traces.

**File formats:**
- `.fvecs` / `.bvecs` / `.ivecs`: Standard ANN benchmark format
  - 4-byte int (dimension), then `dim` floats/bytes/ints, repeated per vector
- `.fbin`: Raw binary (4-byte header with num_vectors and dims, then flat float array)

**The driver needs a small I/O helper to read these formats.** This is
standard and well-documented in FAISS's own benchmark code.

---

### Task 4: Write the FAISS Driver with Zipfian Query Support

**Goal:** A standalone C++ program that loads a dataset, builds a FAISS index,
generates Zipfian-distributed queries, and runs a search loop with PIN ROI
markers around the hot section.

**Architecture:**

```
Phase 1 (before ROI): Load dataset, build index, generate query distribution
Phase 2 (ROI):        Tight search loop — this is what gets traced
Phase 3 (after ROI):  Cleanup
```

**Key design requirements:**

1. **PIN ROI markers.** The driver must include markers so PIN traces only
   the search loop:
   ```cpp
   // Define markers as weak symbols (PIN recognizes these)
   extern "C" {
       void __attribute__((noinline)) __pin_roi_begin() {
           asm volatile("" ::: "memory");
       }
       void __attribute__((noinline)) __pin_roi_end() {
           asm volatile("" ::: "memory");
       }
   }
   ```
   > **Note:** Verify which ROI mechanism our PIN tool uses. The existing
   > sampled tracer uses skip/trace counts (`-i`, `-t` knobs), not ROI
   > markers. If adding ROI marker support to the PIN tool is too invasive,
   > the alternative is to calibrate `-i` (skip count) to land at the start
   > of the search loop. ROI markers are cleaner, though.

2. **Zipfian query distribution over vector space.** The approach:
   - Cluster the dataset into K clusters using FAISS k-means (K=100 default)
   - Assign Zipfian popularity weights to clusters:
     `P(cluster i) ∝ 1/(i+1)^alpha` where alpha is the skew parameter
   - To generate a query: sample a cluster by Zipfian weight, pick a random
     vector from that cluster, add small Gaussian noise (sigma ~= 0.01 * avg
     inter-vector distance)
   - This models production behavior where certain regions of embedding space
     are queried far more than others

3. **Configurable parameters (command-line args):**

   | Parameter | Flag | Default | Description |
   |-----------|------|---------|-------------|
   | Dataset path | `-d` | required | Path to .fvecs/.bvecs file |
   | Index type | `-x` | `HNSW32` | FAISS index_factory string |
   | Num queries | `-n` | 1000000 | Queries in the search loop |
   | Top-k | `-k` | 10 | Nearest neighbors per query |
   | Zipf alpha | `-a` | 0.99 | Skew parameter (0=uniform) |
   | Num clusters | `-c` | 100 | Clusters for query generation |
   | Seed | `-r` | 42 | RNG seed for reproducibility |
   | Query file (optional) | `-q` | none | Pre-generated query vectors |

4. **Multiple index types.** The same driver supports different index types
   via the `-x` flag using FAISS's `index_factory()`:
   - `Flat` — brute-force scan, sequential access, cache-friendly baseline
   - `HNSW32` — graph traversal, pointer-chasing, cache-hostile
   - `IVF4096,Flat` — inverted file with 4096 clusters
   - `IVF4096,PQ16` — inverted file + product quantization (SIMD-heavy)
   - `OPQ16,IVF4096,PQ16` — with optimized product quantization

   Each produces a radically different memory access pattern — this is a
   major advantage over tracing a server where you'd only get one index type.

5. **Deterministic execution.** Fixed RNG seed means the same command
   produces the same query sequence → reproducible traces.

**Pseudocode:**

```cpp
int main(int argc, char** argv) {
    // Parse args
    auto args = parse_args(argc, argv);

    // Phase 1: Load dataset
    auto [data, n, d] = load_fvecs(args.dataset_path);

    // Phase 1: Build index
    auto index = faiss::index_factory(d, args.index_type);
    index->train(n, data);
    index->add(n, data);

    // Phase 1: Build Zipfian query generator
    auto query_gen = ZipfianQueryGenerator(data, n, d,
        args.num_clusters, args.alpha, args.seed);

    // Allocate result buffers (outside ROI to avoid tracing allocation)
    std::vector<float> distances(args.k);
    std::vector<faiss::idx_t> labels(args.k);
    std::vector<float> query(d);

    // Phase 2: Search loop (ROI)
    __pin_roi_begin();
    for (int i = 0; i < args.num_queries; i++) {
        query_gen.next_query(query.data());
        index->search(1, query.data(), args.k,
                      distances.data(), labels.data());
    }
    __pin_roi_end();

    return 0;
}
```

**Build:**
```bash
g++ -O2 -o faiss_driver faiss_driver.cpp \
    -I/usr/local/include -L/usr/local/lib -lfaiss -lopenblas -lpthread
```

> **Important:** Build with `-O2`, not `-O0`. We want to trace optimized code
> that represents what a production system actually runs. `-O0` produces
> artificially bad memory access patterns with excessive stack spills.

---

### Task 5: End-to-End Testing

**Goal:** Verify the entire pipeline works: driver runs, PIN traces it,
traces are valid ChampSim inputs.

#### 5.1 Test the Driver Standalone (No PIN)

```bash
# Small dataset, few queries, verify it runs
./faiss_driver -d sift1m/sift_base.fvecs -x Flat -n 1000 -k 10 -a 0.99

# Verify different index types work
./faiss_driver -d sift1m/sift_base.fvecs -x HNSW32 -n 1000 -k 10
./faiss_driver -d sift1m/sift_base.fvecs -x "IVF256,Flat" -n 1000 -k 10
```

**What to check:**
- No crashes or assertion failures
- Prints summary (queries executed, avg search time, recall if ground truth available)
- Memory footprint is as expected (check with `/usr/bin/time -v`)

#### 5.2 Test PIN Tracing with the Driver

```bash
# Trace with small query count first
pin -t obj-intel64/champsim_tracer.so \
    -o traces/faiss_hnsw \
    -t 10000000 \
    -- ./faiss_driver -d sift1m/sift_base.fvecs -x HNSW32 -n 100000 -k 10 -a 0.99
```

**What to check in the trace:**
1. Trace file is non-empty and of expected size (~64 bytes × instruction count
   for vanilla format, larger for extended format with values)
2. If we have a trace inspector tool: verify instruction count, memory op
   percentage, branch rate
3. IP addresses fall within the FAISS library's text segment:
   ```bash
   nm -D /usr/local/lib/libfaiss.so | grep search
   objdump -d faiss_driver | grep -A5 "search"
   ```

#### 5.3 Trace Different Index Types and Compare

Generate traces for each index type on the same dataset and query sequence:

```bash
for idx in Flat HNSW32 "IVF256,Flat" "IVF256,PQ16"; do
    pin -t obj-intel64/champsim_tracer.so \
        -o "traces/faiss_${idx// /_}" \
        -t 200000000 \
        -- ./faiss_driver -d deep10m/deep10m_base.fvecs \
           -x "$idx" -n 10000000 -k 10 -a 0.99 -r 42
done
```

**Expected behavioral differences in traces:**

| Index Type | Memory Pattern | Expected Branch Rate | Notes |
|------------|---------------|---------------------|-------|
| Flat | Sequential scan | Low (~10%) | Cache-friendly, streaming |
| HNSW32 | Random graph walk | High (~25%) | Pointer-chasing, cache-hostile |
| IVF,Flat | Clustered access | Medium (~15%) | Access concentrated in probed clusters |
| IVF,PQ | SIMD + lookup tables | Medium (~15%) | Compact, compute-heavy |

#### 5.4 Validate Zipfian Distribution

Run the driver with `alpha=0` (uniform) and `alpha=0.99` (skewed) on HNSW,
and compare the traces. The skewed version should show:
- Higher instruction-cache hit rate (same graph nodes traversed repeatedly)
- Different data-cache behavior (hot region fits in cache vs cold misses)
- Potentially different branch prediction accuracy (repeated paths)

This can be verified by running both traces through ChampSim and comparing
cache statistics, or by analyzing unique memory addresses in the traces.

---

## 4. File Organization

```
~/faiss-tracing/
├── driver/
│   ├── faiss_driver.cpp           # Main driver with Zipfian queries
│   ├── fvecs_io.h                 # .fvecs/.bvecs file I/O helpers
│   ├── zipfian_query_gen.h        # Zipfian query generator
│   ├── Makefile                   # Build driver
│   └── README.md                  # Usage instructions
├── pintool/
│   ├── champsim_tracer_extended.cpp  # Modified PIN tracer
│   ├── makefile.rules             # PIN build rules
│   └── trace_instruction_v2.h     # Extended trace format header
├── datasets/
│   ├── sift1m/                    # SIFT-1M (validation)
│   └── deep10m/                   # Deep-10M (production traces)
├── traces/                        # Output traces
│   ├── faiss_Flat_*.champsim
│   ├── faiss_HNSW32_*.champsim
│   └── ...
├── scripts/
│   ├── download_datasets.sh       # Dataset download helper
│   ├── run_all_traces.sh          # Batch tracing script
│   └── validate_traces.sh         # Trace validation checks
└── docs/
    └── faiss-tracing-task.md      # This document
```

---

## 5. Dependencies and Prerequisites

| Dependency | Purpose | Install |
|------------|---------|---------|
| Intel PIN SDK | Instruction tracing | Manual download from Intel |
| FAISS (C++) | Vector search library | Build from source (Task 2) |
| OpenBLAS | FAISS linear algebra backend | `apt install libopenblas-dev` |
| CMake ≥ 3.24 | Building FAISS | `apt install cmake` |
| GCC ≥ 11 | C++17 for driver | System default on Ubuntu 22.04+ |
| wget/curl | Dataset download | System package |

---

## 6. Known Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| FAISS HNSW index build is slow for 10M vectors | Wastes time during development | Use SIFT-1M for development, scale to Deep-10M only for final traces |
| PIN overhead makes 10M+ queries take hours | Long trace generation times | Start with 1M queries for validation; use `-t` to cap instruction count |
| Extended trace format not finalized | Blocks Task 1 | Ask user for format spec before starting; implement vanilla format as fallback |
| FAISS uses SIMD (AVX2/AVX-512) that PIN may handle slowly | Even higher PIN overhead | Acceptable — traces are still correct, just slower to generate |
| Large dataset doesn't fit in RAM | OOM during index build | Check available RAM first; fall back to SIFT-1M or use IVF (doesn't need all vectors in RAM for search) |

---

## 7. Order of Operations

```
Step 0: Ask user for extended trace format spec and PIN tool location
        ↓
Step 1: Task 2 — Install FAISS (can proceed without trace format)
        ↓
Step 2: Task 3 — Download SIFT-1M dataset (parallel with FAISS build)
        ↓
Step 3: Task 4 — Write and test FAISS driver standalone
        ↓
Step 4: Task 1 — Modify PIN tracer (once format spec is available)
        ↓
Step 5: Task 5 — End-to-end PIN + driver testing
        ↓
Step 6: Download Deep-10M and generate production traces
```

Tasks 1 and 2-4 are independent and can proceed in parallel once the
format spec is obtained.

---

## 8. Success Criteria

- [ ] FAISS builds and installs on the machine
- [ ] SIFT-1M dataset downloaded and loadable by the driver
- [ ] Driver runs successfully with all four index types (Flat, HNSW, IVF, IVF+PQ)
- [ ] Driver supports Zipfian query distribution with configurable alpha
- [ ] PIN tracer produces valid traces from the driver
- [ ] Extended trace format includes memory values (once format is specified)
- [ ] Traces from different index types show measurably different characteristics
- [ ] At least one production-scale trace (200M+ instructions on Deep-10M) generated
