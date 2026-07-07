# converter/

## Goal

Convert the pipeline's **raw** trace format (variable-length records
produced by the QEMU TCG plugin) into the **ChampSim v2** trace format
(fixed 512-byte records that the extended ChampSim simulator consumes).

The conversion is a per-instruction decode: the raw record gives us IP,
instruction bytes, privilege bit, and memory addresses/values; an
arch-specific backend fills in the register reads/writes, identifies
branches, and classifies the instruction as INT/FP/SIMD. x86_64 traces
decode via [Zydis](https://github.com/zyantific/zydis); AArch64 (A64)
traces decode via [Capstone](http://www.capstone-engine.org/) 4.0.2.
The backend is selected per-file from the raw header's `arch` byte — see
"Input support" below. Output is streaming-zstd compressed at level 19.

## How this fits into the repo

```
plugin/champsim_tracer.so   ─►  traces/trace_vcpu*.raw.zst
                                          │
                                          ▼
               converter/raw2champsim ─►  traces/trace_vcpu*.champsim.zst
                                          │
                                          ▼
                                   extended ChampSim
```

This is Stage 5 of the pipeline described in
`docs/pipeline/pipeline-stages.md`. The 512-byte ChampSim v2 layout
implemented here matches the format the PIN-based tracer
(`arishem/champsim/tracer/champsim_tracer_mt_roi_v3.cpp`) also emits,
so traces from either tracer can be fed to the same simulator
unchanged.

## Files

### `raw2champsim.c` → `raw2champsim`

Reads a `.raw.zst`, decodes each instruction, and writes a
`.champsim.zst`. Register decode lives in a separate per-arch backend
(see the next section) — `raw2champsim.c` keeps the shared scaffolding:
the read loop, header parse, memory slot-mapping + PA pass-through, the
`branch_taken` next-IP lookahead, record write, and stats. The 512-byte
v2 record layout is defined near the top of the source; a summary:

- **Block 1** (64 B) — vanilla ChampSim layout: IP, branch flags,
  source/destination registers (4 src, 2 dst), source/destination
  memory virtual addresses.
- **Block 2** (64 B) — v2 additions: source/destination physical
  addresses, per-operand access sizes, privilege bit, instruction type
  (INT/FP/SIMD), reserved bytes.
- **Block 3** (384 B) — memory values: up to 64 bytes per source
  memory op × 4 + up to 64 bytes per destination memory op × 2.

**Input support:** reads raw trace format v2, v3/x86_64, **and
v3/aarch64** — the backend (Zydis vs. Capstone) is selected
automatically from the raw v3 header's `arch` byte (0=x86_64,
1=aarch64); v2 files (no `arch` byte) are always treated as x86_64. For
a raw v3 trace captured with `capture_pa=on`, the converter populates
the ChampSim record's `source_memory_pa[]`/`destination_memory_pa[]`
slots with the real guest physical address whenever a mem-op's
`pa_valid` bit is set and `pa_is_io` is not (MMIO addresses are
device-relative, not RAM, so they're left zero); a failed hwaddr lookup
or a v2 input (which has no PA at all) also leaves the slot at zero,
same as before this converter had any PA source. This memory-operand
handling (VA/PA/size/value, taken straight from the raw record) and the
`branch_taken` lookahead are architecture-agnostic and shared by both
backends — only register derivation and branch classification differ
per arch.

**Rotated chunks convert independently.** A raw capture taken with the
plugin's `rotate=N` (see `plugin/README.md`) produces
`trace_vcpu<V>_c<K>.raw.zst` chunk files instead of one monolith; each
converts on its own with no code change, and the `_c<K>` suffix is
carried through to the output name (`…_c00000.raw.zst` →
`…_c00000.champsim.zst`) by the same `.raw.zst`→`.champsim.zst`
suffix replacement used for un-rotated files. Caveat: `branch_taken` is
set by looking ahead to the next instruction's IP, so the last
instruction of each chunk is written `branch_taken=0` (no cross-file
look-ahead) — bounded to at most one instruction per chunk boundary;
convert the concatenated (un-rotated) stream instead for exact parity.

An unknown `arch` byte (neither 0 nor 1) is still a clean error — that
part of the format is not open-ended.

Usage:

```bash
# Default: derive output name from input (.raw.zst → .champsim.zst)
./raw2champsim traces/trace_vcpu1.raw.zst

# Explicit output
./raw2champsim traces/trace_vcpu1.raw.zst /tmp/vcpu1.champsim.zst

# Verbose progress (every 1 M instructions)
./raw2champsim -v traces/trace_vcpu1.raw.zst

# Convert only the first N instructions
./raw2champsim -n 1000000 traces/trace_vcpu1.raw.zst
```

Zstd level is hard-coded to **19** (`ZSTD_COMP_LEVEL`) — this is the
long-term-storage compression setting. Decompression cost is
unaffected, so the ChampSim simulator pays no penalty.

### `decode.h`, `decode_x86.c`, `decode_aarch64.c` — the decode module

Register/branch decode is factored out of `raw2champsim.c` into two
self-complete backends sharing one interface:

```c
decoded_regs_t decode_x86(const uint8_t *bytes, uint8_t size);      /* Zydis */
decoded_regs_t decode_aarch64(const uint8_t *bytes, uint8_t size);  /* Capstone */
```

`decode.h` defines the shared `decoded_regs_t` struct (`ok`, `is_branch`,
`instr_type`, and the source/destination register-ID arrays) and
ChampSim's reserved register-ID constants (`CS_REG_NONE=0`,
`CS_REG_SP=6`, `CS_REG_FLAGS=25`, `CS_REG_PC=26`). Each backend returns
the **complete, final** register read/write sets for one instruction —
`raw2champsim.c` picks the backend by the raw header's `arch` byte and
copies the arrays straight into the 512-byte record; there is no
further register synthesis in the caller. `decode_x86.c` carries the
existing x86 behavior (Zydis register mapping plus the FLAGS→SP→PC
branch-register synthesis), unchanged from before this split — a v3/x86
trace converts byte-identically to the pre-split converter.

#### AArch64 (A64) backend — scope

`decode_aarch64.c` decodes **A64 only** (AArch64's fixed-width,
64-bit-mode instruction set) via [Capstone](http://www.capstone-engine.org/)
4.0.2 (`CS_ARCH_ARM64`). Anything that isn't a cleanly-decodable 4-byte
A64 instruction — a 2-byte T32/compat-mode instruction, or any other
non-decodable input — takes the existing decode-failure fallback:
`ok=false`, `instr_type=INT`, no registers, the record keeps its
IP/privilege/memory fields, and `stats.decode_failures` is incremented.
SVE (Z/P registers) is out of scope entirely; the register-ID scheme
below reserves IDs for it but the backend never emits them.

#### Frozen AArch64 register-ID scheme

`uint8_t` ChampSim IDs. A trace is single-ISA (per the `arch` byte), so
these IDs only need to be injective and hit ChampSim's reserved `6`
(stack pointer), `25` (flags), `26` (instruction pointer):

| AArch64 register | ChampSim ID | Rationale |
|---|---|---|
| XZR / WZR (zero register) | `0` | carries no dependency — same as "no register" |
| SP / WSP | `6` | = `REG_STACK_POINTER` |
| NZCV (condition flags) | `25` | = `REG_FLAGS` |
| PC | `26` | = `REG_INSTRUCTION_POINTER` — **synthetic** (added on branches; A64 has no PC register operand) |
| X0–X28 / W0–W28 | `64`–`92` | GPR block; W is the 32-bit view of the same reg |
| X29 / W29 (FP, frame ptr) | `93` | (= 64+29) |
| X30 / W30 (LR, link reg) | `94` | (= 64+30) — the call/return register |
| V0–V31, and their B/H/S/D/Q views | `96`–`127` | one SIMD/FP reg; every view folds to `96+n` |
| SVE Z0–Z31 / P0–P15 | reserved `128`–`175` | not emitted now; reserved so a future SVE build needs no scheme change |
| other / system registers | `200` | generic "other" bucket |

The scheme is owner-approved and frozen — see
`docs/superpowers/specs/2026-07-07-raw2champsim-aarch64-design.md` (§3)
for the full derivation, including why Capstone's `ARM64_REG_*` enum
(non-contiguous — X29/X30 sit near the start of the enum as the
`FP`/`LR` aliases, below X0) forces `map_arm64_register` to be explicit
rather than range arithmetic.

#### Conventions worth knowing before reading a converted trace

- **LR (94), not SP, is the call/return register.** `BL`/`BLR` write
  LR as a destination; `RET` reads LR as a source. This deliberately
  does **not** mirror the x86 backend's SP-based call/ret/push/pop
  synthesis: ChampSim classifies branches from the record's `is_branch`
  field directly and has no SP-based return-address-stack inference, so
  forcing SP here would add a phantom dependency while omitting the
  real LR chain.
- **Writeback base registers are read+write.** A base register in a
  writeback addressing mode (`STP X0,X1,[SP,#16]!`, `LDR X0,[X1],#8`)
  is captured as both a source and a destination — the store/load and
  the base-pointer update.
- **PC (26) is a synthetic branch destination.** A64 has no PC register
  operand; the backend adds PC(26) to every branch's destinations to
  mirror the x86 record convention (RIP=26 as branch target), and it is
  never evicted even under destination-slot pressure (e.g. a `BL`'s two
  destination slots are exactly `{LR 94, PC 26}`).
- **FLAGS (25) is a source only for `B.cond`.** `CBZ`/`CBNZ`/`TBZ`/`TBNZ`
  are conditional branches too, but they test a GPR operand (already
  captured as a real register), not NZCV — so they carry no FLAGS
  source. This is more precise than the x86 backend, which flags every
  conditional branch uniformly.
- **`ERET` and other privileged control transfers classify as
  `BRANCH_OTHER` (7), never `BRANCH_RETURN` (6).** `ERET`'s target is
  an exception return address, not a normal call/return address, so
  keeping it out of type 6 avoids polluting any consumer that treats
  BRANCH_RETURN as a return-address-stack pop. `SVC`/`HVC`/`SMC`/`BRK`/
  `HLT`/`DCPS{1,2,3}`/`DRPS` classify the same way.
- **`instr_type` is stats-only.** ChampSim's build does not read
  `instr_type` from the record — it only feeds the converter's own
  `type_int`/`type_fp`/`type_simd` counters. Scalar FP folds into
  `INSTR_TYPE_SIMD` for A64 (driven by whether any mapped register ID
  falls in the `[96,127]` SIMD/FP block), so `type_fp` is always `0` for
  AArch64 traces — that is expected, not a bug.

### `Makefile`

Builds `raw2champsim` from `raw2champsim.c`, `decode_x86.c`, and
`decode_aarch64.c` as separate objects, linked together. **Fetches and
builds Zydis automatically** on first use — Zydis is not committed to
this repo. Requires `libzstd-dev` and `libcapstone-dev` on the system
(`make` fails fast with the exact `apt`/`dnf`/`brew` install command if
either is missing — see the `check-zstd`/`check-capstone` targets).

```bash
make                # build the converter (clones Zydis if not present)
make decode_test    # build + run the AArch64 golden unit test (tests/)
make clean          # remove build artifacts (keeps Zydis)
make distclean      # remove Zydis too
```

Zydis version is pinned via `ZYDIS_VERSION = v4.1.1` in the Makefile.
Capstone is a system dependency (`pkg-config capstone`, version 4.0.2 at
the time this backend was written) — not vendored, since it's
universally packaged.

### `tests/`

Validation for the AArch64 backend, per
`docs/superpowers/specs/2026-07-07-raw2champsim-aarch64-design.md` (§5):

- **`decode_aarch64_test.c`** — the golden unit test (`make
  decode_test`). Links `decode_aarch64.o` directly (no Zydis, no
  `raw2champsim.o`) and asserts the exact `decoded_regs_t` — `is_branch`,
  `instr_type`, and source/destination register-ID sets — for 14
  hand-encoded A64 instructions covering calls, returns, indirect/
  conditional branches, loads/stores (incl. writeback), and scalar/
  vector FP. Prints `PASS n/14`.
- **`props.py`** — a property checker that runs over a converted
  `.champsim(.zst)` file and asserts the register-scheme invariants hold
  on real output (e.g. every branch record has PC(26) in its
  destinations, every `is_branch∈{4,5}` has LR(94) in destinations,
  every `is_branch==6` has LR(94) in sources, all register IDs fall in
  the frozen scheme).
- **`microbench.S`** + **`mix_stats.py`** + **`run_e2e.sh`** — an
  end-to-end validation: a freestanding, static A64 microbenchmark with
  a known instruction mix (call/return pair, conditional back-edge,
  loads/stores, a scalar-FP op) run bare-metal under AArch64 QEMU
  (`-kernel` on `-M virt`, no OS), captured with the plugin, converted
  with this backend, and checked against `mix_stats.py`'s oracle for
  that mix. `run_e2e.sh` drives the whole chain (assemble → capture →
  convert → `props.py` → `mix_stats.py`) and is re-runnable. The
  recorded validation run matched the oracle to 4 decimal places with
  **zero** decode failures and every register ID in the frozen scheme.

### `zydis/`

The Zydis submodule, fetched by `make`. Do not edit — it's a checkout
of the upstream repo. Listed in `.gitignore` for the same reason. If
`git status` shows this as untracked, that's expected; `make
distclean` removes it entirely.

## How to use

```bash
# 1. Build (first time: pulls Zydis, takes ~1 minute).
cd converter/
make

# 2. Convert. If the raw trace was idle-loop-filtered upstream, point
#    at the filtered file — the converter doesn't do filtering.
./raw2champsim ../traces/trace_vcpu1.filtered.raw.zst

# 3. Confirm the output size looks sane. At 512 B/record × N records,
#    a 1 B-instruction trace is 512 GB uncompressed but tends to hit
#    ~5–10 GB after zstd-19 for typical workloads.
ls -lh ../traces/trace_vcpu1.filtered.champsim.zst
```

The output `.champsim.zst` is ready to hand to the extended ChampSim
simulator that lives at `champsim/` (symlink into
`arishem/champsim/`).
