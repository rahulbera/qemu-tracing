# raw2champsim AArch64 Support — Design Spec

**Date:** 2026-07-07
**Status:** Approved (design walkthrough approved by project owner; revised
after a 3-reviewer adversarial pass — see §10)
**Scope:** `converter/raw2champsim.c` (+ a small decode module and a golden
test) — add AArch64 (A64) decode so raw v3 traces with `arch=1` convert to
ChampSim v2 records. Plugin, readers, and capture kit are unchanged. The
ChampSim-side AArch64 configuration (cache/uarch params, running the
simulator) is a separate, later phase.

---

## 1. Motivation

The v3 raw format tags each trace with an arch byte (0=x86_64, 1=aarch64),
and the plugin captures AArch64 guests correctly. But `raw2champsim`
decodes x86 only (Zydis) and today **refuses** `arch=1` files. This work
adds a Capstone-based A64 decode path.

The converter's ISA-bound surface is small. `branch_taken` (next-IP
lookahead using the record's `prev_instr_size`) and the memory operands
(VA/PA/size/value read *from the record*, not decoded) are already
ISA-agnostic. What is **not** ISA-neutral — and what the first draft of
this spec got wrong — is the register derivation, including the
branch-register handling: the existing x86 synthesis (`raw2champsim.c:944–971`)
encodes *x86* stack/flags semantics (push/pop touches RSP; every
conditional reads EFLAGS) that do **not** hold on A64.

### 1.1 How ChampSim actually consumes the record (the key fact)

Verified against `arishem/champsim/src/ooo_cpu.cc` and
`inc/commons.h`: ChampSim classifies branches from the record's
`is_branch` field **directly** — there is no SP-based return-address-stack
inference and no `reads_sp`/`writes_sp` anywhere. The
`source_registers`/`destination_registers` arrays feed only the **RAW
dependency scoreboard**. `instr_type` is present in the struct but is
**not read** by this ChampSim build (the v2 alias omits it) — it is
stats-only.

Two consequences that drive the corrected design:

1. The register arrays must carry the **true architectural read/write
   sets** so the scoreboard models real dependencies. For A64 calls/returns
   that means **LR (X30)** — the return-address register — *not* SP.
   Forcing SP would serialize every call/return through a phantom reg-6
   dependency while omitting the real LR chain.
2. `is_branch` must be correct (it is the only thing the frontend keys on).
   `PC` (ID 26) is added to a branch's destinations to mirror the x86
   record convention (RIP=26 as branch target), giving a uniform
   branch-writes-PC signal.

So the corrected approach: the AArch64 backend derives the **complete,
final** register sets from Capstone (explicit operands + implicit
registers), maps them through the frozen scheme (§3), and adds PC(26) on
branches — and does **not** run the x86 SP/FLAGS synthesis. Each backend
is self-complete; there is no shared post-decode synthesis step.

Governing principle: the register-ID scheme (§3) is **frozen**
(owner-approved). ChampSim stays **stock** — by hitting its reserved
reg IDs and its `is_branch` contract, not by imitating x86 stack
semantics.

---

## 2. Architecture: a self-complete decode module

### 2.1 Interface

Each backend returns the **final** register sets (no caller-side
synthesis):

```c
/* decode.h */
#define NUM_INSTR_SOURCES       4
#define NUM_INSTR_DESTINATIONS  2
#define INSTR_TYPE_INT   0
#define INSTR_TYPE_FP    1
#define INSTR_TYPE_SIMD  2
/* ChampSim reserved register IDs (macros → no external-linkage clashes) */
#define CS_REG_NONE      0
#define CS_REG_SP        6
#define CS_REG_FLAGS     25
#define CS_REG_PC        26

typedef struct {
  bool    ok;                                     /* decode succeeded */
  uint8_t is_branch;                              /* 0, or ChampSim type 1..7 */
  uint8_t instr_type;                             /* INSTR_TYPE_* (stats-only) */
  uint8_t src_regs[NUM_INSTR_SOURCES];   uint8_t n_src;
  uint8_t dst_regs[NUM_INSTR_DESTINATIONS]; uint8_t n_dst;
} decoded_regs_t;

decoded_regs_t decode_x86(const uint8_t *bytes, uint8_t size);      /* Zydis  */
decoded_regs_t decode_aarch64(const uint8_t *bytes, uint8_t size);  /* Capstone */
```

`decode_x86`/`decode_aarch64` each produce the complete
`is_branch`/`instr_type`/register sets a ChampSim record needs — including
the branch register touches (x86: FLAGS/RSP/RIP as today; A64: real LR + a
synthetic PC). The caller in `raw2champsim.c` selects by arch, copies the
arrays into the 512-byte record, and does **no** further register
synthesis.

### 2.2 File layout

- `converter/decode.h` — `decoded_regs_t`, the two prototypes, and the
  shared constants above. All constants are `#define`/`enum` (never plain
  file-scope `const`, which would multiply-define across TUs).
- `converter/decode_x86.c` — the Zydis backend. Moves the existing
  `map_zydis_register`, `fixup_champsim_reg`, `classify_instr_type`,
  `is_branch_instruction`, `classify_branch` (they stay `static`), the
  operand-extraction loop (`raw2champsim.c:901–940`), **and** the x86
  branch synthesis (`944–971`) — the loop and synthesis move together as
  one unit, rewritten to fill `decoded_regs_t` arrays instead of the inline
  `rec`/`src_reg_idx`/`dst_reg_idx`, preserving exact behavior (§5.3).
- `converter/decode_aarch64.c` — the Capstone backend (§4).
- `converter/raw2champsim.c` — keeps the shared scaffolding (read loop,
  header parse, memory slot-mapping + PA pass-through, `branch_taken`
  lookahead, record write, stats). Where it decodes inline today it calls
  `decode_x86`/`decode_aarch64` and copies the returned arrays into `rec`.
  The inline synthesis block is deleted (it now lives in `decode_x86.c`).

`input_instr_v2` stays in `raw2champsim.c` — the backends return
`decoded_regs_t`, never the 512-byte struct, so it need not move into the
header.

---

## 3. Frozen AArch64 register-ID scheme (owner-approved)

`uint8_t` ChampSim IDs. A trace is single-ISA (arch byte), so AArch64 IDs
need only be injective, reserve `0`=none, and hit ChampSim's reserved
`6/25/26`.

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

**Capstone enum is NOT contiguous** (verified in
`/usr/include/capstone/arm64.h`, 4.0.2): `ARM64_REG_X29`/`X30` are aliases
`ARM64_REG_FP`/`ARM64_REG_LR` placed near the *start* of the enum (below
`X0`), while `X0..X28` sit near the end; `W0..W30`, and each of
`B/H/S/D/Q 0..31`, and `V0..V31` are their own separate blocks; `SP`,
`WSP`, `XZR`, `WZR`, `NZCV` are individual values. Therefore
`map_arm64_register` **must not** use `64 + (reg - ARM64_REG_X0)` range
arithmetic across X0–X30. It handles the aliases explicitly
(`FP`/`X29`→93, `LR`/`X30`→94, `SP`/`WSP`→6, `XZR`/`WZR`→0, `NZCV`→25) and
maps the genuinely-contiguous sub-ranges arithmetically per family
(`X0..X28`→64+n, `W0..W28`→64+n, `V0..V31`→96+n, and each of
`B/H/S/D/Q 0..31`→96+n). The implementer verifies the exact enum values
against the installed header before writing the map. Capstone may report
X30 as either `ARM64_REG_X30` or `ARM64_REG_LR` — both resolve to 94.

---

## 4. AArch64 backend (Capstone 4.0.2, A64-only)

Capstone 4.0.2 is installed system-wide (`libcapstone-dev`,
`/usr/include/capstone/arm64.h`, `pkg-config --modversion capstone` =
4.0.2). Uses `CS_ARCH_ARM64` with `arm64_*` types (Capstone 5 renamed
these to `aarch64_*`; we target the installed 4.x). **The implementer must
confirm every Capstone symbol below against the installed 4.0.2 headers
before use** — the review flagged version-sensitive names.

### 4.1 Decoder lifecycle

Opened once when the converter reads an `arch==1` header:
`cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle)` (confirm the correct A64
mode constant against `capstone.h` — likely `CS_MODE_ARM`/`0`, not a
"little-endian" flag), then `cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON)`.
Closed with `cs_close` at converter exit. Per record:
`cs_disasm(handle, bytes, 4, addr, 1, &insn)`; on success read
`insn->detail->arm64`; free with **`cs_free(insn, 1)`** (the 4.0.2
signature takes the count `cs_disasm` returned).

### 4.2 Registers — complete architectural read/write sets

The backend fills `src_regs`/`dst_regs` from **both**:

1. **Explicit operands** — iterate `detail->arm64.operands[0..op_count]`.
   For `ARM64_OP_REG`, map `op->reg` via `map_arm64_register` and place by
   the per-operand access flags (`op->access & CS_AC_WRITE` → dst,
   `CS_AC_READ` → src; a writeback base is read+write → both). For
   `ARM64_OP_MEM`, `op->mem.base` and `op->mem.index` are sources. **The
   implementer must confirm `cs_arm64_op.access` exists in 4.0.2**; if it
   does not, fall back to `cs_regs_access()` / `detail->regs_read` +
   `regs_write` for the full split (item 2 already covers implicit regs).
2. **Implicit registers** — iterate `detail->regs_read[0..regs_read_count]`
   into sources and `detail->regs_write[0..regs_write_count]` into
   destinations, mapped through §3, skipping any that map to `0`. This is
   how the LR write of `BL`/`BLR` and the NZCV read of `B.cond` are
   captured (they are implicit, not operand fields) — the review showed a
   literal operands-only port silently drops them.

Registers mapping to `0` (XZR/WZR) are skipped. Fill up to
`NUM_INSTR_SOURCES` (4) / `NUM_INSTR_DESTINATIONS` (2); dedupe (a register
already present is not re-added); excess is dropped.

**Robustness for the call/return chain (do not rely solely on implicit
regs):** in the branch step (§4.3), explicitly ensure `BL`/`BLR` carry
LR(94) as a destination and `RET` carries LR(94) as a source, so the
call→return dependency is present regardless of Capstone's implicit-reg
completeness in 4.0.2.

### 4.3 Branch classification + PC/LR/FLAGS touches

`is_branch` from Capstone insn id + condition:

| A64 | ChampSim | id / condition |
|---|---|---|
| `B` (unconditional, cc = AL/none) | 1 BRANCH_DIRECT_JUMP | `ARM64_INS_B` |
| `B.cond` (cc ∈ EQ/NE/…) | 3 BRANCH_CONDITIONAL | `ARM64_INS_B` + `detail->arm64.cc` |
| `CBZ`/`CBNZ`/`TBZ`/`TBNZ` | 3 BRANCH_CONDITIONAL | those ids |
| `BR` | 2 BRANCH_INDIRECT | `ARM64_INS_BR` |
| `BL` | 4 BRANCH_DIRECT_CALL | `ARM64_INS_BL` |
| `BLR` | 5 BRANCH_INDIRECT_CALL | `ARM64_INS_BLR` |
| `RET` | 6 BRANCH_RETURN | `ARM64_INS_RET` |
| `SVC`/`HVC`/`SMC`/`BRK`/`ERET`/other control transfers | 7 BRANCH_OTHER | remaining branch-group ids |
| everything else | 0 NOT_BRANCH | — |

Then the branch register touches (A64-correct, **no SP synthesis**):

- **Any `is_branch != 0`** → ensure PC(26) is in destinations. Reserve a
  destination slot for it so it is **never evicted** (add PC before any
  other synthetic dest, or last with a guaranteed slot) — the §5.2
  invariant "every branch has PC(26) in dsts" must hold even for calls that
  also write LR.
- **`BL`/`BLR` (4/5)** → ensure LR(94) in destinations (call writes the
  link register). No SP.
- **`RET` (6)** → ensure LR(94) in sources (return reads the link
  register). No SP.
- **`B.cond` only** (id `B` with a real cc) → ensure FLAGS(25) in sources.
  **`CBZ`/`CBNZ`/`TBZ`/`TBNZ` do NOT** get FLAGS — they test a GPR (already
  captured as a real operand), not the condition flags. (This is more
  precise than the x86 path, which uniformly flags all conditionals; using
  Capstone's real reg sets makes the distinction free.)

`ERET` is BRANCH_OTHER (7), never RETURN — its target is not a normal
return address, so keeping it out of type-6 avoids polluting any RAS-like
consumer.

Slot budget: for a `BL`, destinations are {LR(94), PC(26)} — exactly the 2
available slots; sources empty. For `BLR X0`: src {X0(64)}, dst {LR(94),
PC(26)}. For `RET`: src {LR(94)}, dst {PC(26)}. For `B.eq`: src {NZCV(25)},
dst {PC(26)}. For `CBZ X2`: src {X2(66)}, dst {PC(26)}.

### 4.4 Instruction type (INT/FP/SIMD, stats-only)

After register mapping, if **any** mapped src/dst register ID is in
`[96,127]` (the SIMD/FP block) → `INSTR_TYPE_SIMD`; else `INSTR_TYPE_INT`.
Driving off the folded ID (not a single `ARM64_REG_V0` range) correctly
catches scalar-FP `S/D/H` and vector `V/Q` operands, which Capstone reports
as distinct enum blocks. `INSTR_TYPE_FP` is never emitted for A64 (scalar
FP lumps into SIMD, matching the PIN v3 convention). Since ChampSim does
not consume `instr_type` (§1.1), this is a stats-only choice; the
converter's `type_fp` counter is simply always 0 for A64 traces — no
downstream check may assert `type_fp > 0`.

### 4.5 branch_taken and memory — unchanged/shared

`branch_taken` uses the existing next-IP lookahead with the record's
`prev_instr_size` (4 for A64). Memory operands come from the raw record
(VA, optional PA, size, value) via the shared slot-mapping + PA
pass-through. The AArch64 backend does not touch either.

### 4.6 Non-A64 / decode failure

`size != 4` or `cs_disasm` decodes 0 → `ok = false`; the caller uses the
existing decode-failure fallback (`instr_type=INT`, no registers, record
keeps IP/privilege/memory, `stats.decode_failures++`). A64-only is the
agreed scope.

---

## 5. Validation

### 5.1 Golden unit test (strongest per-instruction signal)

A harness links `decode_aarch64.c` and calls `decode_aarch64(bytes, 4)` on
hand-encoded A64 instructions, asserting the **exact** `decoded_regs_t`
(the backend is self-complete, so no separate synthesis call is needed).
Little-endian encodings verified against a disassembler at authoring time.
Corrected expectations (no SP on calls/returns; LR is the call/return
register; PC always on branches):

| instruction | is_branch | instr_type | src (ids) | dst (ids) |
|---|---|---|---|---|
| `RET` (`c0035fd6`) | 6 | INT | LR 94 | PC 26 |
| `BL #x` | 4 | INT | — | LR 94, PC 26 |
| `BLR X0` | 5 | INT | X0 64 | LR 94, PC 26 |
| `BR X1` | 2 | INT | X1 65 | PC 26 |
| `B #x` | 1 | INT | — | PC 26 |
| `B.eq #x` | 3 | INT | NZCV 25 | PC 26 |
| `CBZ X2,#x` | 3 | INT | X2 66 | PC 26 |
| `ADD X0,X1,X2` | 0 | INT | X1 65, X2 66 | X0 64 |
| `LDR X0,[X1,X2]` | 0 | INT | X1 65, X2 66 | X0 64 |
| `STP X0,X1,[SP,#16]` | 0 | INT | X0 64, X1 65, SP 6 | — |
| `FADD S0,S1,S2` | 0 | SIMD | V1 97, V2 98 | V0 96 |
| `ADD V0.4S,V1.4S,V2.4S` | 0 | SIMD | V1 97, V2 98 | V0 96 |

Every field hand-computed and asserted exactly. (Source/dest *ordering*
within a slot array is not asserted — only membership and count — since
Capstone operand/implicit-reg ordering is an internal detail; the harness
checks set membership.)

### 5.2 Property checks on a real captured chunk

Run the converter over an AArch64 chunk (the `v3_arm` capture from the
capture-kit work, or a fresh microbench trace) and assert on the 512-byte
output records: every `is_branch != 0` has PC(26) in destinations;
`is_branch ∈ {4,5}` has LR(94) in destinations; `is_branch == 6` has LR(94)
in sources; **no** record has SP(6) unless it explicitly encodes SP;
`is_branch == 3` from a `B.cond` has FLAGS(25) in sources; all register IDs
∈ {0,6,25,26,64–94,96–127,200}; A64 decode-failure rate ≈ 0; branch and
mem-op fractions plausible.

### 5.3 x86 regression (behavior-preserving refactor)

A v3/x86 trace converts **byte-identically** before vs after the
decode-module extraction. The relocation of the operand loop + synthesis
into `decode_x86.c` must preserve these load-bearing invariants (flagged by
the review):

- iterate `insn.operand_count` (ALL operands, incl. implicit), not
  `operand_count_visible`;
- within a register operand, append WRITE (dst) **before** READ (src) so a
  read-write reg lands in both, in that order;
- apply `fixup_champsim_reg` per register after `map_zydis_register`;
- `op->mem.base` skips `ZYDIS_REGISTER_RIP`, but `op->mem.index` does not;
  base appended before index;
- the `< NUM_INSTR_SOURCES/DESTINATIONS` bound checks gate insertion;
- synthesis order FLAGS → SP → PC, unchanged.

Regression corpus must exercise these: include RW-register instructions,
RIP-relative memory operands, `call`/`ret`, and conditional branches — a
trivial trace won't catch a mis-relocation. Gate: `sha256` of the
`.champsim.zst` output identical before/after.

### 5.4 End-to-end microbenchmark (owner-requested)

The plugin's only ROI gate is `trigger=` (a host-side file, polled every
10 M instructions, start-only) — there is **no** magic-NOP / marker
mechanism in the plugin. So precise hot-loop bracketing is not available;
the microbench is instead made to **dominate** the traced window:

- Build a **freestanding, static** AArch64 microbench (`-nostdlib
  -static`, a `_start` that runs a large loop with a known mix: a
  call/return pair, a conditional branch (loop back-edge), a few
  loads/stores/adds, and a NEON/FP op, then exits) so no CRT/libc
  instructions pollute the stream.
- Run it under the AArch64 QEMU built for the capture kit
  (`~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64`). Preferred
  for a clean oracle: **bare-metal via `-kernel`** on `-M virt` (the
  microbench is the only thing executing — no Linux, no other userspace),
  traced from the first instruction with a `limit=`. Fallback: run the
  static binary inside the existing ARM Linux guest and filter the
  converted records to **user-mode** (`privilege == 0`) so kernel/boot
  noise is excluded.
- Convert with the new backend and assert the aggregate matches the known
  mix: call/return counts balance; conditional-branch count tracks the loop
  trip count; branch / mem-op / SIMD fractions match the source; zero
  decode failures; all register IDs valid (§5.2). This exercises plugin →
  raw v3/aarch64 → converter end-to-end on emulated execution.

The ChampSim *simulator* run (records → extended simulator, MPKI/RAS
stats) remains the separate ChampSim-bring-up phase, out of scope here.

---

## 6. Build

`converter/Makefile`:
- `CAPSTONE_CFLAGS := $(shell pkg-config --cflags capstone)`,
  `CAPSTONE_LDFLAGS := $(shell pkg-config --libs capstone 2>/dev/null || echo -lcapstone)`.
- Compile `decode_x86.c`, `decode_aarch64.c`, and `raw2champsim.c` to
  objects, then link — the current one-shot `gcc src→bin` invocation must
  become multi-object so Zydis flags reach the x86 TU and Capstone flags
  reach the A64 TU, and both reach the final link. Keep the Zydis git-fetch.
- `check-capstone` target: error with `apt install libcapstone-dev` if
  pkg-config can't find capstone.
- A `decode_test` target builds the §5.1 golden harness (links
  `decode_aarch64.o` + Capstone).

---

## 7. Documentation

- `converter/README.md` — AArch64 now supported (v2, v3/x86_64,
  v3/aarch64); the frozen register-ID scheme table; A64-only scope; the
  `is_branch`/LR conventions, `ERET`→OTHER, and the CBZ-has-no-FLAGS and
  `instr_type` stats-only notes.
- `CLAUDE.md` — Stage 5 line: converter reads v3/aarch64 (Capstone);
  ChampSim-side config remains the next phase.
- `scripts/capture-kit/README.md` — "raw2champsim now converts aarch64;
  trace_filter is still x86-only (idle filtering)."

---

## 8. Edge cases

| Case | Behavior |
|---|---|
| v2 / v3/x86_64 | Zydis path, byte-identical to today |
| v3/aarch64 | Capstone path (new) |
| Unknown arch byte | Existing clean error |
| 2-byte T32 / non-decodable | `ok=false` → decode-failure fallback, counted |
| XZR/WZR operand | maps to 0, skipped |
| Explicit SP/WSP operand (frame setup, `STP [SP]!`) | maps to 6 via operand loop; no call/return SP synthesis to double it |
| `ERET`/exceptions | is_branch=7 |
| `BL` slot pressure | dst = {LR 94, PC 26} exactly fills 2 slots; PC guaranteed not evicted |
| Capstone reports X30 as `LR` alias | resolves to 94 (same as `X30`) |
| `cs_arm64_op.access` absent in 4.0.2 | fall back to `regs_read`/`regs_write` for the full split |
| Capstone not installed | `make` fails at `check-capstone` with apt hint |

---

## 9. Decisions log

- Register-ID scheme frozen per §3 (owner-approved); `map_arm64_register`
  is explicit (enum non-contiguous), FP/X29→93, LR/X30→94, SP/WSP→6,
  XZR/WZR→0, NZCV→25, V-views→96–127.
- ChampSim stays **stock** by hitting reserved IDs + the `is_branch`
  contract — **not** by imitating x86 stack semantics. **No SP synthesis
  on A64 calls/returns**; the load-bearing call/return register is LR(94).
- Backends are **self-complete** (return final register sets); the x86
  synthesis moves into `decode_x86.c`; no shared caller-side synthesis. This
  makes the golden test reach the final fields by linking one TU.
- Registers derived from Capstone **explicit operands + implicit
  regs_read/regs_write** (with explicit LR/FLAGS reinforcement on
  branches); `CBZ`/`CBNZ`/`TBZ`/`TBNZ` carry no FLAGS.
- Decoder = Capstone 4.0.2 system install, `CS_ARCH_ARM64`; A64-only; SVE
  reserved but not emitted.
- `instr_type` is stats-only (ChampSim doesn't consume it); FP/NEON→SIMD;
  `type_fp` is always 0 for A64.
- Validation = golden unit test + property checks + x86 byte-identical
  regression + end-to-end freestanding microbench through the AArch64 QEMU
  (bare-metal `-kernel` preferred, user-mode-filtered Linux run as
  fallback); ChampSim simulator run is the separate next phase.

## 10. Adversarial review record

Reviewed 2026-07-07 by three reviewers (Capstone-4.0.2 API correctness vs
installed headers, ChampSim-mapping soundness, consistency/refactor/
validation realism). 14 findings — 3 blockers, 6 should-fix, 5 nits — all
incorporated. Most consequential: (1) ChampSim has no SP-based RAS, so the
x86 SP synthesis is wrong for A64 — LR(94) is the real call/return
register, and each backend is now self-complete (no shared synthesis),
which also fixes the golden-test-reachability blocker; (2) the Capstone
4.0.2 `ARM64_REG_*` enum is non-contiguous (X29/X30 = FP/LR aliases below
X0; W and B/H/S/D/Q are separate blocks), so `map_arm64_register` is
explicit, not range arithmetic; (3) `BL`'s LR write is an implicit register
(read `regs_write`, not operands), and the corrected golden `BL` row is
dst={LR,PC}. Source facts verified against `/usr/include/capstone/arm64.h`,
`arishem/champsim/src/ooo_cpu.cc`, and `raw2champsim.c`.
