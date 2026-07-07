# raw2champsim AArch64 Support — Design Spec

**Date:** 2026-07-07
**Status:** Approved (design walkthrough approved by project owner)
**Scope:** `converter/raw2champsim.c` — add AArch64 (A64) decode so raw v3
traces with `arch=1` convert to ChampSim v2 records. Plugin, readers, and
capture kit are unchanged. The ChampSim-side AArch64 configuration
(cache/uarch params, running the simulator) is a separate, later phase.

---

## 1. Motivation

The v3 raw format already tags each trace with an arch byte (0=x86_64,
1=aarch64), and the plugin captures AArch64 guests correctly. But
`raw2champsim` decodes x86 only (Zydis) and today **refuses** `arch=1`
files with a clean error pointing at this spec. This work adds the
Capstone-based A64 decode path so the collaborator's AArch64 captures can
be converted to the 512-byte ChampSim v2 record.

The converter's ISA-bound surface is small. `branch_taken` (next-IP
lookahead using the record's `prev_instr_size`) and the memory operands
(VA/PA/size/value read *from the record*, not decoded) are already
ISA-agnostic. Critically, the **branch register-synthesis block**
(`raw2champsim.c:944–971`) that keeps ChampSim stock — stamping
FLAGS/SP/PC touches by ChampSim reserved IDs `25/6/26` and `is_branch`
types `1–7` — is already ISA-neutral. So only the *decode + operand →
register/branch/type derivation* differs per ISA.

Governing principle: **the register-ID scheme is frozen** (owner-approved,
§3). It is baked into every converted AArch64 trace; changing it later
means re-converting. ChampSim stays **stock** — no simulator patches — by
mapping SP/NZCV/PC to ChampSim's reserved IDs and synthesizing the same
branch register-touches the x86 path already does.

---

## 2. Architecture: an isolated decode module

Extract the ISA-specific decode behind one interface so the shared ~80%
of the converter (read loop, header parse, memory slot-mapping + PA
pass-through, `branch_taken` lookahead, the branch register-synthesis,
record write, stats) never forks.

### 2.1 Interface

```c
/* decode.h */
typedef struct {
  bool    ok;                                    /* decode succeeded */
  uint8_t is_branch;                             /* 0, or ChampSim type 1..7 */
  uint8_t instr_type;                            /* INSTR_TYPE_INT/FP/SIMD */
  uint8_t src_regs[NUM_INSTR_SOURCES];   uint8_t n_src;
  uint8_t dst_regs[NUM_INSTR_DESTINATIONS]; uint8_t n_dst;
} decoded_regs_t;

/* arch: 0=x86_64 (Zydis), 1=aarch64 (Capstone). Fills is_branch,
 * instr_type, and the explicit register read/write sets (including
 * memory-addressing base/index registers). Does NOT synthesize the
 * FLAGS/SP/PC branch touches — that stays shared in the caller. */
decoded_regs_t decode_instr(int arch, const uint8_t *bytes, uint8_t size);
```

### 2.2 File layout

- `converter/decode.h` — `decoded_regs_t`, `decode_instr` prototype, the
  ChampSim register-ID constants (§3), the INSTR_TYPE_* and NUM_INSTR_*
  constants shared with the record definition.
- `converter/decode_x86.c` — the Zydis backend: the existing
  `map_zydis_register`, `fixup_champsim_reg`, `classify_instr_type`,
  `is_branch_instruction`, `classify_branch`, and the operand→register
  extraction currently inlined in the main loop (`raw2champsim.c:901–940`),
  moved here behind `decode_instr` when `arch==0`.
- `converter/decode_aarch64.c` — the Capstone backend (§4), `decode_instr`
  when `arch==1`.
- `converter/raw2champsim.c` — keeps the shared scaffolding. Where it
  currently decodes inline, it calls `decode_instr(arch, insn_bytes, isz)`,
  copies the result into `rec` and `src_reg_idx`/`dst_reg_idx`, then runs
  the **unchanged** synthesis block (944–971), memory mapping, and write.

### 2.3 Shared synthesis stays put

After `decode_instr` returns, the caller copies `n_src`/`n_dst` register
arrays into `rec.source_registers`/`rec.destination_registers` and sets
`src_reg_idx=n_src`, `dst_reg_idx=n_dst`. The existing block then runs
identically for both arches: conditional (`is_branch==3`) → ensure
FLAGS(25) source; call/ret (`4/5/6`) → ensure SP(6) src+dst; any branch →
PC(26) dest. This is the single mechanism that keeps ChampSim stock, and
it is written once.

---

## 3. Frozen AArch64 register-ID scheme (owner-approved)

`uint8_t` ChampSim register IDs. A given trace is single-ISA (arch byte),
so AArch64 IDs need not avoid x86 IDs — only be injective, reserve `0` =
no register, and hit ChampSim's reserved `6/25/26` for SP/flags/PC.

| AArch64 register | ChampSim ID | Rationale |
|---|---|---|
| XZR / WZR (zero register) | `0` | carries no dependency — same as "no register" |
| SP | `6` | = `REG_STACK_POINTER` → ChampSim RAS / call-return |
| NZCV (condition flags) | `25` | = `REG_FLAGS` → conditional-branch dependency |
| PC | `26` | = `REG_INSTRUCTION_POINTER` → branch destination |
| X0–X30 (LR = X30) | `64`–`94` | contiguous GPR block; clears reserved 6/25/26 |
| V0–V31 (NEON/FP; B/H/S/D/Q views alias the same reg) | `96`–`127` | SIMD/FP block |
| SVE Z0–Z31 / P0–P15 | reserved `128`–`175` | not emitted now; reserved so a future SVE build needs no scheme change |
| other / system registers | `200` | generic "other" bucket (mirrors x86 path's catch-all) |

W-registers (W0–W30) are the 32-bit views of X0–X30 and map to the same
`64–94` IDs (a dependency on W3 is a dependency on X3). Likewise the FP/
SIMD sub-register views (Bn/Hn/Sn/Dn/Qn) map to their Vn ID in `96–127`.

---

## 4. AArch64 backend (Capstone 4.0.2, A64-only)

Capstone 4.0.2 is installed system-wide (`libcapstone-dev`,
`/usr/include/capstone/arm64.h`, `pkg-config --modversion capstone` =
4.0.2). Uses `CS_ARCH_ARM64` with `arm64_*` types (Capstone 5 renamed
these to `aarch64_*`; we target the installed 4.x).

### 4.1 Decoder lifecycle

Opened once when the converter reads an `arch==1` header:
`cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &handle)` then
`cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON)`. Closed at converter exit.
Per record: `cs_disasm(handle, bytes, 4, addr, 1, &insn)`; on success read
`insn->detail->arm64`; `cs_free` the insn.

### 4.2 Registers (explicit read/write sets)

Iterate `detail->arm64.operands[0..op_count]`:

- `ARM64_OP_REG` → map `op->reg` via §3 (`map_arm64_register`), then place
  by the per-operand access flags: `op->access & CS_AC_WRITE` → dst,
  `op->access & CS_AC_READ` → src (writeback base registers are marked
  read+write and land in both, mirroring the x86 operand-action loop).
- `ARM64_OP_MEM` → `op->mem.base` and `op->mem.index` are sources (address
  computation), same as the x86 memory base/index extraction.
- Registers that map to `0` (XZR/WZR) are skipped (no dependency).

NZCV, SP-as-implicit, and PC are **not** extracted here — the shared
synthesis (§2.3) adds FLAGS/SP/PC for branches, exactly as the x86 path
does (which synthesizes rather than reading EFLAGS/RSP/RIP). Instructions
that *explicitly* operate on SP (e.g. `ADD SP, SP, #16`, `STP … [SP,#…]!`)
carry SP as a real operand and map to `6` through the operand loop.

Fill up to `NUM_INSTR_SOURCES` (4) / `NUM_INSTR_DESTINATIONS` (2); excess
operands are dropped (as the x86 path does).

### 4.3 Branch classification (`is_branch`, ChampSim 1–7)

By Capstone insn id + condition code:

| A64 | ChampSim | id / condition |
|---|---|---|
| `B` (unconditional) | 1 BRANCH_DIRECT_JUMP | `ARM64_INS_B`, cc = `ARM64_CC_AL`/invalid |
| `B.cond` | 3 BRANCH_CONDITIONAL | `ARM64_INS_B`, cc ∈ EQ/NE/… |
| `CBZ`/`CBNZ`/`TBZ`/`TBNZ` | 3 BRANCH_CONDITIONAL | `ARM64_INS_CBZ/CBNZ/TBZ/TBNZ` |
| `BR` (indirect) | 2 BRANCH_INDIRECT | `ARM64_INS_BR` |
| `BL` (direct call) | 4 BRANCH_DIRECT_CALL | `ARM64_INS_BL` |
| `BLR` (indirect call) | 5 BRANCH_INDIRECT_CALL | `ARM64_INS_BLR` |
| `RET` | 6 BRANCH_RETURN | `ARM64_INS_RET` |
| `SVC`/`HVC`/`SMC`/`BRK`/`ERET` and other control transfers | 7 BRANCH_OTHER | remaining branch-group insns |
| everything else | 0 NOT_BRANCH | — |

`ERET` is classified BRANCH_OTHER (7): it is an exception return whose
target is not a normal RAS entry, so mapping it to RETURN(6) would corrupt
ChampSim's return-address stack — keep it out of the RAS. (Documented
choice; revisit only if kernel-heavy traces show RAS pressure from it.)

### 4.4 Instruction type (INT/FP/SIMD)

If any operand references a V/Q/D/S/H/B register (the NEON/FP register
file, IDs `96–127` under §3) → `INSTR_TYPE_SIMD`; otherwise
`INSTR_TYPE_INT`. Scalar FP (e.g. `FADD S0,S1,S2`) uses S/D registers and
is therefore tagged SIMD — matching the PIN v3 tracer's documented
convention of lumping scalar SSE/AVX into SIMD, so the two tracers'
`instr_type` distributions stay comparable. (A scalar-FP → `INSTR_TYPE_FP`
refinement is possible later via the FP-vs-vector operand shape; not done
now.)

### 4.5 branch_taken and memory — unchanged/shared

`branch_taken` is computed by the existing next-IP lookahead using the
record's `prev_instr_size` (4 for A64). Memory operands come from the raw
record (VA, optional PA, size, value) via the shared slot-mapping and PA
pass-through — the AArch64 backend does **not** touch them. No change.

### 4.6 Non-A64 / decode failure

If `size != 4` (e.g. a 2-byte T32 instruction under a 32-bit EL0 app) or
`cs_disasm` decodes 0 instructions, return `ok=false`. The caller uses the
existing decode-failure fallback: `instr_type=INT`, no registers, the
record still carries its IP/privilege/memory, and `stats.decode_failures`
is incremented. A64-only is the agreed scope; his guest is 64-bit
userspace with a pure-A64 stream, so this path should stay ≈0.

---

## 5. Validation (self-contained + end-to-end)

### 5.1 Golden unit test (strongest per-instruction signal)

A test harness links the decode module and calls
`decode_instr(1, bytes, 4)` on a curated set of **hand-encoded** A64
instructions (no cross-assembler dependency), asserting the exact
`decoded_regs_t` and, after the shared synthesis, the exact ChampSim
register/branch/type fields. Curated mix (with the little-endian
encodings verified against a disassembler at authoring time):

- `RET` (`c0 03 5f d6`) → is_branch=6, src has SP(6) + LR(X30=94), dst has SP(6)+PC(26)
- `BL #x` → is_branch=4, dst PC(26)+SP(6), src SP(6); LR(94) written
- `BLR X0` → is_branch=5, src X0(64)+SP(6), dst PC(26)+SP(6)
- `BR X1` → is_branch=2, src X1(65), dst PC(26)
- `B #x` → is_branch=1, dst PC(26)
- `B.eq #x` → is_branch=3, src FLAGS(25), dst PC(26)
- `CBZ X2,#x` → is_branch=3, src X2(66)+FLAGS(25), dst PC(26)
- `ADD X0,X1,X2` → is_branch=0, INT, dst X0(64), src X1(65)+X2(66)
- `LDR X0,[X1,X2]` → INT, dst X0(64), src X1(65)+X2(66); (memory access itself is record-driven, not asserted here)
- `STP X0,X1,[SP,#16]` → INT, src X0(64)+X1(65)+SP(6)
- `FADD S0,S1,S2` → SIMD, dst V0(96), src V1(97)+V2(98)
- a NEON vector op (e.g. `ADD V0.4S,V1.4S,V2.4S`) → SIMD

Each expected field is hand-computed and asserted exactly. This directly
validates the frozen mapping and the branch synthesis.

### 5.2 Property checks on a real captured chunk

Run the converter over the `v3_arm` AArch64 chunk captured during the
capture-kit work (regenerate if absent) and assert invariants on the
512-byte output records: every `is_branch≠0` has PC(26) in destinations;
`is_branch==3` has FLAGS(25) in sources; `is_branch∈{4,5,6}` has SP(6) in
src and dst; all register IDs ∈ {0,6,25,26,64–94,96–127,200}; the A64
decode-failure rate ≈0; branch-instruction fraction and mem-op fraction
are in plausible ranges.

### 5.3 x86 regression (behavior-preserving refactor)

A v3/x86 trace converts **byte-identically** before vs after the
decode-module extraction (the x86 path is only relocated, not changed).
sha256 the two `.champsim.zst` outputs.

### 5.4 End-to-end microbenchmark (owner-requested)

Using the AArch64 QEMU built for the capture-kit work
(`~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64`): compile a
small static AArch64 microbenchmark with a **known instruction mix** (a
loop with a call/return pair, a conditional branch, a few
loads/stores/adds, and a NEON/FP op), boot it in the ARM guest under the
plugin (trigger-gated around the hot loop), capture a v3/aarch64 raw
trace, and run it through the new converter. Because the microbenchmark's
instruction mix is known, the converted trace's aggregate characteristics
are **predictable and checkable**: the call/return counts balance, the
conditional-branch count tracks the loop trip count, the branch/mem/SIMD
fractions match the source, and no decode failures occur. This exercises
the entire path — plugin → raw v3/aarch64 → converter → ChampSim record —
on real hardware-emulated execution, not just synthetic bytes.

The ChampSim *simulator* run (feeding the records to the extended
simulator, checking MPKI/RAS stats) remains the separate ChampSim-bring-up
phase and is out of scope here.

---

## 6. Build

`converter/Makefile`:
- Add `CAPSTONE_CFLAGS := $(shell pkg-config --cflags capstone)` and
  `CAPSTONE_LDFLAGS := $(shell pkg-config --libs capstone 2>/dev/null || echo -lcapstone)`.
- Compile `decode_x86.c` and `decode_aarch64.c` into the target; add their
  headers to the prerequisites. Keep the existing Zydis git-fetch.
- A `check-capstone` step errors with an install hint
  (`apt install libcapstone-dev`) if pkg-config can't find it.

---

## 7. Documentation

- `converter/README.md` — AArch64 is now supported (v2, v3/x86_64, and
  v3/aarch64 all convert); include the frozen register-ID scheme table,
  the A64-only scope, and the `instr_type` scalar-FP→SIMD and `ERET`→OTHER
  conventions.
- `CLAUDE.md` — update the Stage 5 line: the converter reads v3/aarch64
  (Capstone); ChampSim-side AArch64 config remains the next phase.
- `scripts/capture-kit/README.md` — the "filter and raw2champsim refuse
  aarch64" caveat becomes "raw2champsim now converts aarch64; trace_filter
  is still x86-only (idle filtering)."

---

## 8. Edge cases

| Case | Behavior |
|---|---|
| v2 or v3/x86_64 file | Zydis path, unchanged (byte-identical to today) |
| v3/aarch64 file | Capstone path (new) |
| Unknown arch byte (not 0/1) | Existing clean error, unchanged |
| 2-byte T32 / non-decodable A64 | `ok=false` → decode-failure fallback, counted |
| XZR/WZR operand | Mapped to 0, skipped (no dependency) |
| Explicit SP operand (frame setup, LDP/STP writeback) | Maps to 6 via operand loop; synthesis is idempotent (won't double-add) |
| `ERET` / exception instructions | is_branch=7 (OTHER), never RETURN — keeps RAS clean |
| >4 source or >2 dest registers | Excess dropped (as x86 path) |
| Capstone not installed | `make` fails at `check-capstone` with an apt hint |

---

## 9. Decisions log

- Register-ID scheme frozen per the §3 table (owner-approved).
- ChampSim stays **stock** — SP/NZCV/PC → 6/25/26 + shared branch
  synthesis; no simulator patches.
- Decoder = **Capstone 4.0.2** system install (no vendoring), `CS_ARCH_ARM64`.
- Dispatch = **in-place, arch-selected decode module** (`decode.h` +
  `decode_x86.c` + `decode_aarch64.c`), not a separate binary — the shared
  scaffolding is the bulk.
- Scope = **A64-only**; SVE register IDs reserved but not emitted; non-A64
  records use the decode-failure fallback.
- `instr_type`: FP/NEON → SIMD (matches PIN v3 convention); `ERET` → OTHER.
- Validation = **golden unit test + property checks + x86 regression +
  end-to-end AArch64 microbenchmark** (owner-requested); the ChampSim
  simulator run is the separate next phase.
