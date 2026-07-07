# raw2champsim AArch64 Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add a Capstone-based A64 decode path so raw v3 traces with `arch=1` convert to 512-byte ChampSim v2 records, behind a self-complete decode module; x86 conversion stays byte-identical.

**Architecture:** Extract decode behind `decode_x86()`/`decode_aarch64()`, each returning the *final* `decoded_regs_t` (is_branch, instr_type, complete source/dest register sets — including branch touches). `raw2champsim.c` dispatches on the arch byte and copies the result into the record; no caller-side register synthesis remains. AArch64 uses Capstone's explicit operands + implicit regs, with LR(94) as the call/return register (ChampSim has no SP-based RAS).

**Tech Stack:** C, Zydis (x86, git-fetched), Capstone 4.0.2 (A64, system `libcapstone-dev`), zstd. Golden test harness in C. AArch64 QEMU at `~/softwares/qemu-9.2.4/build-aarch64/`.

**Spec:** `docs/superpowers/specs/2026-07-07-raw2champsim-aarch64-design.md` — normative for every mapping/branch/build decision.

## Global Constraints

- `decode.h`: `decoded_regs_t {bool ok; uint8_t is_branch; uint8_t instr_type; uint8_t src_regs[4]; uint8_t n_src; uint8_t dst_regs[2]; uint8_t n_dst;}`; prototypes `decode_x86`/`decode_aarch64`; constants `NUM_INSTR_SOURCES 4`, `NUM_INSTR_DESTINATIONS 2`, `INSTR_TYPE_INT/FP/SIMD 0/1/2`, `CS_REG_NONE 0`, `CS_REG_SP 6`, `CS_REG_FLAGS 25`, `CS_REG_PC 26` — all `#define`/`enum` (never file-scope `const`).
- Both backends return the **final** register sets. x86 = existing operand loop + FLAGS/SP/PC synthesis, moved together into `decode_x86.c`. A64 = operands + implicit regs + PC-on-branch, per §4.
- **AArch64 register map (explicit; Capstone enum non-contiguous — verified: X29=350, X30=351, NZCV=352, SP=353, WSP=354, XZR=356, X0=548, FP=X29, LR=X30):** XZR/WZR→0; SP/WSP→6; NZCV→25; FP/X29→93; LR/X30→94; X0–X28→64+n; W0–W28→64+n; V0–V31 and each of B/H/S/D/Q 0–31→96+n; else→200. PC is never a Capstone operand — ID 26 is added synthetically on branches only.
- **A64 branch types:** B(uncond)→1, BR→2, B.cond/CBZ/CBNZ/TBZ/TBNZ→3, BL→4, BLR→5, RET→6, SVC/HVC/SMC/BRK/ERET/other→7.
- **A64 branch touches (NO SP synthesis):** any branch → PC(26) in dst, never evicted; BL/BLR → LR(94) in dst; RET → LR(94) in src; B.cond only → FLAGS(25) in src (CBZ/CBNZ/TBZ/TBNZ get no FLAGS).
- **A64 instr_type:** SIMD iff any mapped reg ID ∈ [96,127]; else INT (never FP). Stats-only (ChampSim doesn't read it); `type_fp` is always 0 for A64.
- Capstone: `cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &h)` (CS_MODE_ARM==0==default), `cs_option(h, CS_OPT_DETAIL, CS_OPT_ON)`; per record `cs_disasm(h, bytes, 4, addr, 1, &insn)` → `insn->detail->arm64`; free `cs_free(insn, 1)`; `cs_close(&h)` at exit. Per-operand `op->access & CS_AC_READ/CS_AC_WRITE` (field exists in 4.0.2). Implicit regs via `insn->detail->regs_read[0..regs_read_count]` / `regs_write[0..regs_write_count]`.
- x86 path byte-identical after refactor: preserve iterating `insn.operand_count` (all operands), WRITE-before-READ dual-append, per-reg `fixup_champsim_reg`, `mem.base` skips RIP but `mem.index` doesn't, base-before-index, bound checks, synthesis order FLAGS→SP→PC.
- Non-A64/size≠4/decode-fail → `ok=false` → existing decode-failure fallback.
- Build: multi-object (`decode_x86.o`, `decode_aarch64.o`, `raw2champsim.o`) then link; Zydis flags on x86 TU, `pkg-config capstone` flags on A64 TU + link; `check-capstone` + `decode_test` targets.
- Scratch: `/tmp/cstf-a64/`. AArch64 QEMU: `~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64`. `aarch64-linux-gnu-gcc`/`-as`/`objdump` for the microbench if available (else hand-encode). zstd + python3 on PATH.
- Commit after every task.

---

### Task 1: Extract x86 decode into a self-complete module (byte-identical)

**Files:**
- Create: `converter/decode.h`, `converter/decode_x86.c`
- Modify: `converter/raw2champsim.c` (remove the moved code; call `decode_x86`), `converter/Makefile` (multi-object)

**Interfaces:**
- Produces: `decode.h` (the interface + constants) and `decode_x86(bytes,size) -> decoded_regs_t` returning final x86 register sets. At this task's end AArch64 is still refused; x86 output is byte-identical to pre-refactor.

- [ ] **Step 1: Baseline sha of a representative x86 v3 trace**

```bash
mkdir -p /tmp/cstf-a64
make -C plugin plugin >/dev/null 2>&1 || true
# Capture a v3/x86 trace with branch+mem variety (BIOS exercises calls/rets/cond branches/RIP-rel):
plugin/tests/smoke_capture.sh /tmp/cstf-a64/x86src 2>/dev/null || true
make -C converter >/dev/null
converter/raw2champsim /tmp/cstf-a64/x86src/trace_vcpu0.raw.zst /tmp/cstf-a64/x86_pre.champsim.zst >/dev/null
zstd -dc /tmp/cstf-a64/x86_pre.champsim.zst | sha256sum > /tmp/cstf-a64/x86_pre.sha
cat /tmp/cstf-a64/x86_pre.sha
```
Expected: a sha256 of the pre-refactor x86 conversion (the regression oracle).

- [ ] **Step 2: Write `decode.h`**

```c
#ifndef DECODE_H
#define DECODE_H
#include <stdbool.h>
#include <stdint.h>

#define NUM_INSTR_SOURCES       4
#define NUM_INSTR_DESTINATIONS  2

#define INSTR_TYPE_INT   0
#define INSTR_TYPE_FP    1
#define INSTR_TYPE_SIMD  2

/* ChampSim reserved register IDs */
#define CS_REG_NONE   0
#define CS_REG_SP     6
#define CS_REG_FLAGS  25
#define CS_REG_PC     26

typedef struct {
  bool    ok;
  uint8_t is_branch;                            /* 0, or ChampSim 1..7 */
  uint8_t instr_type;                           /* INSTR_TYPE_* */
  uint8_t src_regs[NUM_INSTR_SOURCES];
  uint8_t n_src;
  uint8_t dst_regs[NUM_INSTR_DESTINATIONS];
  uint8_t n_dst;
} decoded_regs_t;

decoded_regs_t decode_x86(const uint8_t *bytes, uint8_t size);
decoded_regs_t decode_aarch64(const uint8_t *bytes, uint8_t size);

#endif
```

- [ ] **Step 3: Create `decode_x86.c` by moving the x86 logic**

Move verbatim (keep `static`): `map_zydis_register`, `fixup_champsim_reg`, `classify_instr_type`, `is_branch_instruction`, `classify_branch` (raw2champsim.c ~360–591), plus the `ZydisDecoder` (init once via a lazily-initialized static, or init per call — per-call `ZydisDecoderInit` is cheap and stateless). Implement `decode_x86` to: init a local `ZydisDecodedInstruction`/`operands[]`, `ZydisDecoderDecodeFull(bytes,size)`; on failure return `{.ok=false, .instr_type=INSTR_TYPE_INT}`. On success fill a local `decoded_regs_t out`:
- `out.is_branch = is_branch_instruction(&insn) ? classify_branch(&insn) : 0;`
- `out.instr_type = classify_instr_type(&insn);`
- the **operand loop** (was raw2champsim.c:901–940) writing into `out.src_regs`/`out.dst_regs` with `out.n_src`/`out.n_dst` — preserving every invariant in Global Constraints (operand_count all; WRITE-then-READ; fixup; base-skips-RIP/index-doesn't; base-before-index; bound checks).
- the **x86 synthesis** (was 944–971) rewritten against `out` arrays: is_branch==3 → ensure FLAGS(25) src; is_branch∈{4,5,6} → ensure SP(6) src+dst; is_branch!=0 → ensure PC(26) dst. Same FLAGS→SP→PC order.
- `out.ok = true; return out;`

Include `decode.h` and `<Zydis/Zydis.h>`. Provide the ZydisDecoder as a `static ZydisDecoder` initialized once (guard with a `static bool inited`).

- [ ] **Step 4: Rewire `raw2champsim.c`**

Delete the moved functions and the inline decode+operand-loop+synthesis (884–971). Delete the `#include <Zydis/Zydis.h>` and the `ZydisDecoderInit` in main (the decoder now lives in decode_x86.c). Where the record is built, replace with:
```c
decoded_regs_t d = (arch == 1) ? decode_aarch64(insn_bytes, isz)
                               : decode_x86(insn_bytes, isz);
if (d.ok) {
  rec.is_branch  = d.is_branch;
  rec.instr_type = d.instr_type;
  for (uint8_t k = 0; k < d.n_src; k++) rec.source_registers[k]      = d.src_regs[k];
  for (uint8_t k = 0; k < d.n_dst; k++) rec.destination_registers[k] = d.dst_regs[k];
  if (d.is_branch) stats.branch_insns++;
  switch (d.instr_type) { case INSTR_TYPE_FP: stats.type_fp++; break;
                          case INSTR_TYPE_SIMD: stats.type_simd++; break;
                          default: stats.type_int++; }
} else {
  stats.decode_failures++;
  rec.instr_type = INSTR_TYPE_INT;
}
```
`#include "decode.h"`. (For this task, `decode_aarch64` is not yet defined — provide a temporary stub in a new `decode_aarch64.c` that returns `{.ok=false}` so the build links; Task 2 replaces it. Keep the arch==1 header refusal in place for now so no aarch64 file reaches the stub — Task 2 removes it.) The memory slot-mapping, PA pass-through, branch_taken lookahead, and write stay unchanged.

- [ ] **Step 5: Makefile → multi-object + stub**

Add a minimal `converter/decode_aarch64.c` stub: `#include "decode.h"\n decoded_regs_t decode_aarch64(const uint8_t *b, uint8_t s){(void)b;(void)s;decoded_regs_t d={0};d.ok=false;d.instr_type=INSTR_TYPE_INT;return d;}`. Update the Makefile so `raw2champsim.o`, `decode_x86.o`, `decode_aarch64.o` compile separately (x86 TU gets `$(ZYDIS_INC)`; aarch64 TU will get `$(CAPSTONE_CFLAGS)` in Task 2 — harmless now) and link together with `$(ZYDIS_LIB) $(ZYCORE_LIB) $(ZSTD_LDFLAGS)`.

- [ ] **Step 6: Build + byte-identical regression**

```bash
make -C converter
converter/raw2champsim /tmp/cstf-a64/x86src/trace_vcpu0.raw.zst /tmp/cstf-a64/x86_post.champsim.zst >/dev/null
zstd -dc /tmp/cstf-a64/x86_post.champsim.zst | sha256sum
cat /tmp/cstf-a64/x86_pre.sha
```
Expected: the two sha256 values **identical**. If they differ, an invariant was not preserved in the move — fix before committing.

- [ ] **Step 7: Commit**

```bash
git add converter/decode.h converter/decode_x86.c converter/decode_aarch64.c converter/raw2champsim.c converter/Makefile
git commit -m "converter: extract self-complete decode module (x86 byte-identical)

Move x86 Zydis decode + operand extraction + FLAGS/SP/PC synthesis into
decode_x86.c behind decode_x86()->decoded_regs_t; raw2champsim dispatches
and copies. decode_aarch64 stubbed. x86 output sha256-identical."
```

---

### Task 2: AArch64 backend (Capstone) + dispatch

**Files:**
- Modify: `converter/decode_aarch64.c` (replace the stub), `converter/raw2champsim.c` (remove the arch==1 refusal), `converter/Makefile` (Capstone flags + check-capstone)

**Interfaces:**
- Consumes: `decode.h`, the Task-1 dispatch.
- Produces: `decode_aarch64(bytes,size)` returning final A64 register sets per the spec §3/§4.

- [ ] **Step 1: `map_arm64_register` (explicit — enum is non-contiguous)**

In `decode_aarch64.c`, include `<capstone/capstone.h>` and `<capstone/arm64.h>`. Implement:
```c
static uint8_t map_arm64_register(unsigned reg) {
  switch (reg) {
    case ARM64_REG_XZR: case ARM64_REG_WZR: return CS_REG_NONE; /* 0 */
    case ARM64_REG_SP:  case ARM64_REG_WSP: return CS_REG_SP;   /* 6 */
    case ARM64_REG_NZCV:                    return CS_REG_FLAGS; /* 25 */
    case ARM64_REG_FP:  /* == X29 */        return 93;
    case ARM64_REG_LR:  /* == X30 */        return 94;
    default: break;
  }
  if (reg >= ARM64_REG_X0  && reg <= ARM64_REG_X28) return (uint8_t)(64 + (reg - ARM64_REG_X0));
  if (reg >= ARM64_REG_W0  && reg <= ARM64_REG_W28) return (uint8_t)(64 + (reg - ARM64_REG_W0));
  if (reg >= ARM64_REG_V0  && reg <= ARM64_REG_V31) return (uint8_t)(96 + (reg - ARM64_REG_V0));
  if (reg >= ARM64_REG_Q0  && reg <= ARM64_REG_Q31) return (uint8_t)(96 + (reg - ARM64_REG_Q0));
  if (reg >= ARM64_REG_D0  && reg <= ARM64_REG_D31) return (uint8_t)(96 + (reg - ARM64_REG_D0));
  if (reg >= ARM64_REG_S0  && reg <= ARM64_REG_S31) return (uint8_t)(96 + (reg - ARM64_REG_S0));
  if (reg >= ARM64_REG_H0  && reg <= ARM64_REG_H31) return (uint8_t)(96 + (reg - ARM64_REG_H0));
  if (reg >= ARM64_REG_B0  && reg <= ARM64_REG_B31) return (uint8_t)(96 + (reg - ARM64_REG_B0));
  if (reg == ARM64_REG_INVALID)                     return CS_REG_NONE;
  return 200; /* other/system */
}
```
**Before writing, verify** each `ARM64_REG_*` sub-range is genuinely contiguous in the installed `/usr/include/capstone/arm64.h` (grep the enum); if W29/W30 are not contiguous with W0–W28 (as X29/X30 aren't with X0–X28), add explicit `case ARM64_REG_W29: return 93; case ARM64_REG_W30: return 94;`.

- [ ] **Step 2: small add-register helpers (with dedupe + bounds)**

```c
static void add_src(decoded_regs_t *o, uint8_t id){ if(!id)return;
  for(uint8_t i=0;i<o->n_src;i++) if(o->src_regs[i]==id) return;
  if(o->n_src<NUM_INSTR_SOURCES) o->src_regs[o->n_src++]=id; }
static void add_dst(decoded_regs_t *o, uint8_t id){ if(!id)return;
  for(uint8_t i=0;i<o->n_dst;i++) if(o->dst_regs[i]==id) return;
  if(o->n_dst<NUM_INSTR_DESTINATIONS) o->dst_regs[o->n_dst++]=id; }
```

- [ ] **Step 3: `decode_aarch64`**

Static Capstone handle, opened once:
```c
static csh cs_handle; static bool cs_inited=false;
static bool ensure_cs(void){ if(cs_inited) return true;
  if(cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &cs_handle)!=CS_ERR_OK) return false;
  cs_option(cs_handle, CS_OPT_DETAIL, CS_OPT_ON); cs_inited=true; return true; }
```
Body:
```c
decoded_regs_t decode_aarch64(const uint8_t *bytes, uint8_t size){
  decoded_regs_t o={0}; o.instr_type=INSTR_TYPE_INT;
  if(size!=4 || !ensure_cs()){ o.ok=false; return o; }
  cs_insn *insn=NULL;
  size_t n=cs_disasm(cs_handle, bytes, 4, 0x0, 1, &insn);
  if(n<1){ o.ok=false; if(insn)cs_free(insn,n); return o; }
  cs_arm64 *a=&insn->detail->arm64;

  /* 1. explicit operands */
  for(int i=0;i<a->op_count;i++){ cs_arm64_op *op=&a->operands[i];
    if(op->type==ARM64_OP_REG){ uint8_t id=map_arm64_register(op->reg);
      if(op->access & CS_AC_WRITE) add_dst(&o,id);
      if(op->access & CS_AC_READ)  add_src(&o,id); }
    else if(op->type==ARM64_OP_MEM){
      add_src(&o, map_arm64_register(op->mem.base));
      add_src(&o, map_arm64_register(op->mem.index)); } }

  /* 2. implicit regs (LR write of BL/BLR, NZCV read of B.cond, etc.) */
  for(uint8_t i=0;i<insn->detail->regs_read_count;i++)
    add_src(&o, map_arm64_register(insn->detail->regs_read[i]));
  for(uint8_t i=0;i<insn->detail->regs_write_count;i++)
    add_dst(&o, map_arm64_register(insn->detail->regs_write[i]));

  /* 3. branch classification */
  o.is_branch = classify_arm64_branch(insn);   /* helper below */

  /* 4. branch touches (NO SP synthesis) */
  if(o.is_branch==4 || o.is_branch==5) add_dst(&o, 94);          /* BL/BLR write LR */
  if(o.is_branch==6) add_src(&o, 94);                            /* RET reads LR   */
  if(is_bcond(insn)) add_src(&o, CS_REG_FLAGS);                  /* B.cond only    */
  if(o.is_branch){ /* PC(26) as dst, never evicted */
    bool have_pc=false; for(uint8_t i=0;i<o.n_dst;i++) if(o.dst_regs[i]==CS_REG_PC) have_pc=true;
    if(!have_pc){ if(o.n_dst>=NUM_INSTR_DESTINATIONS) o.n_dst=NUM_INSTR_DESTINATIONS-1; /* free a slot */
      o.dst_regs[o.n_dst++]=CS_REG_PC; } }

  /* 5. instr_type: SIMD iff any mapped reg in [96,127] */
  o.instr_type=INSTR_TYPE_INT;
  for(uint8_t i=0;i<o.n_src;i++) if(o.src_regs[i]>=96&&o.src_regs[i]<=127) o.instr_type=INSTR_TYPE_SIMD;
  for(uint8_t i=0;i<o.n_dst;i++) if(o.dst_regs[i]>=96&&o.dst_regs[i]<=127) o.instr_type=INSTR_TYPE_SIMD;

  o.ok=true; cs_free(insn,n); return o;
}
```
Note the PC-eviction guard: if both dst slots are full (e.g. a call already holding LR + something), drop the last non-PC dst to guarantee PC is present (spec §4.3: PC must never be evicted). For a BL, dst becomes {LR(94), PC(26)} — LR added in step 4, PC in the guard, exactly 2 slots.

- [ ] **Step 4: branch helpers**

```c
static uint8_t classify_arm64_branch(cs_insn *insn){
  switch(insn->id){
    case ARM64_INS_BR:  return 2;
    case ARM64_INS_BL:  return 4;
    case ARM64_INS_BLR: return 5;
    case ARM64_INS_RET: return 6;
    case ARM64_INS_CBZ: case ARM64_INS_CBNZ:
    case ARM64_INS_TBZ: case ARM64_INS_TBNZ: return 3;
    case ARM64_INS_B:
      return (insn->detail->arm64.cc==ARM64_CC_AL ||
              insn->detail->arm64.cc==ARM64_CC_INVALID) ? 1 : 3;
    case ARM64_INS_SVC: case ARM64_INS_HVC: case ARM64_INS_SMC:
    case ARM64_INS_BRK: case ARM64_INS_ERET: return 7;
    default: return 0;
  }
}
static bool is_bcond(cs_insn *insn){
  return insn->id==ARM64_INS_B &&
         insn->detail->arm64.cc!=ARM64_CC_AL &&
         insn->detail->arm64.cc!=ARM64_CC_INVALID;
}
```
Verify `ARM64_CC_AL`/`ARM64_CC_INVALID` and all `ARM64_INS_*` ids exist in the installed `arm64.h` before use.

- [ ] **Step 5: Remove the arch==1 refusal in raw2champsim.c**

Delete the `if (arch == 1) { fprintf(... refuse ...); return 1; }` block (the arch whitelist for {0,1} stays; arch other than 0/1 still errors). The dispatch from Task 1 now routes arch==1 to the real `decode_aarch64`.

- [ ] **Step 6: Makefile Capstone wiring**

Add `CAPSTONE_CFLAGS := $(shell pkg-config --cflags capstone)` / `CAPSTONE_LDFLAGS := $(shell pkg-config --libs capstone 2>/dev/null || echo -lcapstone)`; apply CFLAGS to the `decode_aarch64.o` compile; add `$(CAPSTONE_LDFLAGS)` to the final link; add a `check-capstone` prerequisite that errors with `apt install libcapstone-dev` if `pkg-config --exists capstone` fails.

- [ ] **Step 7: Build + smoke on the existing AArch64 chunk**

```bash
make -C converter
ls /tmp/cstf/v3_arm/trace_vcpu0.raw.zst 2>/dev/null || echo "NOTE: regenerate v3_arm (Task 4 also produces one)"
converter/raw2champsim /tmp/cstf/v3_arm/trace_vcpu0.raw.zst /tmp/cstf-a64/arm.champsim.zst 2>&1 | tail -5
```
Expected: converts without the refusal; prints `Format: v3, ... arch aarch64` and a decode-failure count ≈0. (If `/tmp/cstf/v3_arm` is gone, this smoke moves to Task 4's fresh capture — acceptable.)

- [ ] **Step 8: x86 regression still holds**

```bash
converter/raw2champsim /tmp/cstf-a64/x86src/trace_vcpu0.raw.zst /tmp/cstf-a64/x86_t2.champsim.zst >/dev/null
zstd -dc /tmp/cstf-a64/x86_t2.champsim.zst | sha256sum ; cat /tmp/cstf-a64/x86_pre.sha
```
Expected: identical sha (Task 2 didn't touch the x86 path).

- [ ] **Step 9: Commit**

```bash
git add converter/decode_aarch64.c converter/raw2champsim.c converter/Makefile
git commit -m "converter: AArch64 (A64) decode via Capstone; remove arch=1 refusal

Real read/write reg sets (operands + implicit regs), LR(94) the call/
return register (no SP synthesis), PC(26) synthetic on branches, B.cond-
only FLAGS, SIMD via 96-127 ID. Explicit map_arm64_register (enum non-
contiguous). x86 path unchanged (sha-identical)."
```

---

### Task 3: Golden unit test + property checks

**Files:**
- Create: `converter/tests/decode_aarch64_test.c`, `converter/tests/props.py`
- Modify: `converter/Makefile` (`decode_test` target)

**Interfaces:**
- Consumes: `decode_aarch64` (Task 2).
- Produces: a `decode_test` binary asserting the §5.1 golden table, and a property checker for a real chunk.

- [ ] **Step 1: Golden harness**

`converter/tests/decode_aarch64_test.c` links `decode_aarch64.o` + Capstone, and for each row of spec §5.1 calls `decode_aarch64(bytes,4)` and asserts `is_branch`, `instr_type`, and **set membership** of src/dst reg IDs (order-independent, exact count). Hand-encoded little-endian bytes (verify each with `python3 -c` or `objdump` at authoring time), e.g. `RET`=`{0xc0,0x03,0x5f,0xd6}`, `NOP`=`{0x1f,0x20,0x03,0xd5}`. Cover all 12 rows: RET, BL, BLR X0, BR X1, B, B.eq, CBZ X2, ADD X0/X1/X2, LDR X0,[X1,X2], STP X0,X1,[SP,#16], FADD S0,S1,S2, ADD V0.4S… Print `PASS n/12` and exit nonzero on any mismatch (printing expected vs actual for the failing row).

- [ ] **Step 2: `decode_test` Makefile target + run**

```bash
make -C converter decode_test
converter/tests/decode_aarch64_test
```
Expected: `PASS 12/12`, exit 0. Any failing row prints expected vs actual — fix the mapping/branch logic (not the test) until all pass. This is the freeze-once mapping's acceptance gate.

- [ ] **Step 3: Property checks on a real chunk**

`converter/tests/props.py` reads a `.champsim.zst`, walks 512-byte records, and asserts spec §5.2: every `is_branch!=0` has PC(26) in dsts; `is_branch∈{4,5}` has LR(94) in dsts; `is_branch==6` has LR(94) in srcs; register IDs ∈ {0,6,25,26,64–94,96–127,200}; decode-failure count (from converter stderr) ≈0; branch/mem fractions printed. Run against the Task-2 (or Task-4) AArch64 conversion:
```bash
zstd -dc /tmp/cstf-a64/arm.champsim.zst > /tmp/cstf-a64/arm.champsim 2>/dev/null && \
  python3 converter/tests/props.py /tmp/cstf-a64/arm.champsim
```
Expected: `ALL PROPERTIES HOLD` (or a specific violation to fix).

- [ ] **Step 4: Commit**

```bash
git add converter/tests/decode_aarch64_test.c converter/tests/props.py converter/Makefile
git commit -m "converter: AArch64 golden unit test (12 insns) + property checker"
```

---

### Task 4: End-to-end microbenchmark validation

**Files:**
- Create: `converter/tests/microbench.S` (or `.c`), `converter/tests/run_e2e.sh`

**Interfaces:** consumes the full pipeline (plugin → raw v3/aarch64 → converter).

- [ ] **Step 1: Freestanding A64 microbench with a known mix**

Write `converter/tests/microbench.S`: a `-nostdlib -static` A64 program whose `_start` runs a large counted loop (say 1e7 iterations) containing a **known instruction mix** — a `bl`/`ret` pair (a leaf function), a `cbz`/`b.ne` loop back-edge, a couple of `ldr`/`str`/`add`, and one `fadd`/NEON op — then exits (`mov x8,#93; svc #0` for exit, or a `b .` spin if bare-metal). Assemble with `aarch64-linux-gnu-gcc -nostdlib -static -o microbench microbench.S` (or hand-`as`+`ld`). Keep the loop body's instruction counts documented in a comment (they are the oracle).

- [ ] **Step 2: Capture under the AArch64 QEMU**

`run_e2e.sh`: prefer bare-metal — `~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64 -M virt -cpu cortex-a72 -nographic -kernel microbench -plugin plugin/champsim_tracer.so,outdir=/tmp/cstf-a64/mb,vcpus=0,limit=20000000,arch=aarch64` (arch auto-detects; `-kernel` runs the microbench as the only code, no Linux). If `-kernel` won't boot a bare ELF cleanly, fall back to running the static binary inside the ARM Linux guest (boot via the capture-kit `boot_arm64.sh`, scp the binary, run it, capture) and filter to `privilege==0` records in the check. Verify the plugin banner shows `Arch: aarch64`.

- [ ] **Step 3: Convert + validate against the known mix**

```bash
converter/raw2champsim /tmp/cstf-a64/mb/trace_vcpu0.raw.zst /tmp/cstf-a64/mb.champsim.zst 2>/tmp/cstf-a64/mb.conv.log
grep -E "decode_fail|Format" /tmp/cstf-a64/mb.conv.log
zstd -dc /tmp/cstf-a64/mb.champsim.zst > /tmp/cstf-a64/mb.champsim
python3 converter/tests/props.py /tmp/cstf-a64/mb.champsim
```
Assert: zero decode failures; property checks hold; and the aggregate matches the microbench oracle — call count ≈ return count (bl/ret balance), conditional-branch count ≈ loop trips, a nonzero SIMD fraction (the NEON/FP op), branch fraction in the expected band. Document the observed-vs-expected numbers in `run_e2e.sh`'s output.

- [ ] **Step 4: Commit**

```bash
git add converter/tests/microbench.S converter/tests/run_e2e.sh
git commit -m "converter: end-to-end AArch64 microbench validation (plugin->raw->converter)"
```

---

### Task 5: Documentation

**Files:** Modify `converter/README.md`, `CLAUDE.md`, `scripts/capture-kit/README.md`

- [ ] **Step 1: `converter/README.md`** — AArch64 now supported (v2, v3/x86_64, v3/aarch64 all convert); include the frozen §3 register-ID scheme table; A64-only scope; the LR-is-call/return and `ERET`→OTHER and CBZ-no-FLAGS conventions; note `instr_type` is stats-only (ChampSim doesn't consume it), so `type_fp`=0 for A64.
- [ ] **Step 2: `CLAUDE.md`** — update the Stage 5 line: the converter now reads v3/aarch64 (Capstone); ChampSim-side AArch64 config remains the next phase.
- [ ] **Step 3: `scripts/capture-kit/README.md`** — change the "raw2champsim refuses aarch64" caveat to "raw2champsim now converts aarch64; trace_filter is still x86-only (idle filtering)."
- [ ] **Step 4: Cross-check + commit**

```bash
grep -rl "capstone\|aarch64\|register-ID" converter/README.md
git add converter/README.md CLAUDE.md scripts/capture-kit/README.md
git commit -m "Docs: raw2champsim AArch64 support (register scheme, scope, conventions)"
```

---

## Execution order & parallelism

Strict: 1 → 2 → 3 → 4 → 5 (2 needs 1's module; 3 needs 2's backend; 4 needs 2+3; 5 documents all). Task 4's microbench binary can be authored any time after Task 1.

## Plan self-review record

- Spec coverage: §2 module → Task 1; §3 map + §4 backend → Task 2; §5.1 golden → Task 3 Step 1-2; §5.2 properties → Task 3 Step 3; §5.3 x86 regression → Task 1 Step 6 + Task 2 Step 8; §5.4 microbench → Task 4; §6 build → Tasks 1/2 Makefile; §7 docs → Task 5. No gaps.
- Consistency: `decoded_regs_t` field names, the reg-ID constants (6/25/26/64–94/96–127/200), branch-type numbers, and the PC-never-evicted rule are identical across decode.h, decode_x86.c, decode_aarch64.c, and the golden test. The A64 path emits final sets (no caller synthesis); x86 synthesis lives in decode_x86.c — both reached by `decode_instr`-style dispatch in raw2champsim.c.
- Verified before planning: `cs_arm64_op.access` exists (arm64.h:654); `CS_MODE_ARM==0`; enum non-contiguity (X29/X30/FP/LR/SP/WSP/NZCV/XZR near start, X0 at 548).
