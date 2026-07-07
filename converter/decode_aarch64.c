/*
 * decode_aarch64.c — AArch64 (A64) instruction decode (Capstone) -> ChampSim
 * register sets.
 *
 * Frozen register-ID scheme and semantics per
 * docs/superpowers/specs/2026-07-07-raw2champsim-aarch64-design.md (S3/S4):
 *
 *   - Capstone's ARM64_REG_* enum is NOT contiguous: X29/X30 are aliases
 *     (FP/LR) placed near the start of the enum, below X0; SP/WSP/XZR/WZR/
 *     NZCV are individual values. map_arm64_register is therefore explicit
 *     for the aliases/singletons and only range-maps the sub-families that
 *     are genuinely contiguous in the installed header (verified against
 *     /usr/include/capstone/arm64.h, Capstone 4.0.2).
 *   - This backend is self-complete: it derives the *final* read/write
 *     register sets from Capstone (explicit operands + implicit regs),
 *     classifies branches, and adds the A64-correct branch touches
 *     (LR the call/return register, PC synthetic on branches, FLAGS only
 *     on B.cond). There is NO SP synthesis on calls/returns — ChampSim has
 *     no SP-based RAS, unlike the x86 backend's push/pop/call/ret handling.
 */

#include "decode.h"

#include <capstone/capstone.h>
#include <capstone/arm64.h>

/* ================================================================
 * arm64 register -> ChampSim register ID
 *
 * See decode.h for the shared CS_REG_* constants and the spec S3 table:
 *   0        = none (XZR/WZR)
 *   6        = SP/WSP
 *   25       = NZCV (FLAGS)
 *   26       = PC (synthetic only, never a real operand -- not mapped here)
 *   64-92    = X0-X28 / W0-W28
 *   93       = X29/W29 (FP)
 *   94       = X30/W30 (LR)
 *   96-127   = V/Q/D/S/H/B 0-31 (one SIMD/FP reg per index, all views fold)
 *   200      = other/system register
 * ================================================================ */

static uint8_t map_arm64_register(unsigned reg)
{
  switch (reg) {
    case ARM64_REG_XZR: case ARM64_REG_WZR: return CS_REG_NONE; /* 0 */
    case ARM64_REG_SP:  case ARM64_REG_WSP: return CS_REG_SP;   /* 6 */
    case ARM64_REG_NZCV:                    return CS_REG_FLAGS; /* 25 */
    case ARM64_REG_FP:  /* == X29 */        return 93;
    case ARM64_REG_LR:  /* == X30 */        return 94;
    /* W29/W30 are distinct enum values from X29/X30 (not aliases) and are
     * NOT covered by the W0..W28 range below (verified: the installed
     * header lists W0..W30 contiguously, i.e. W29/W30 sit right after
     * W28 but before the W0-W28 range check's upper bound) -- special-case
     * them here so they fold to the same FP/LR IDs as their 64-bit views. */
    case ARM64_REG_W29:                     return 93;
    case ARM64_REG_W30:                     return 94;
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

/* ================================================================
 * Add-register helpers: skip id 0 (no register), dedupe, bound to the
 * ChampSim slot counts.
 * ================================================================ */

static void add_src(decoded_regs_t *o, uint8_t id)
{
  if (!id) return;
  for (uint8_t i = 0; i < o->n_src; i++)
    if (o->src_regs[i] == id) return;
  if (o->n_src < NUM_INSTR_SOURCES) o->src_regs[o->n_src++] = id;
}

static void add_dst(decoded_regs_t *o, uint8_t id)
{
  if (!id) return;
  for (uint8_t i = 0; i < o->n_dst; i++)
    if (o->dst_regs[i] == id) return;
  if (o->n_dst < NUM_INSTR_DESTINATIONS) o->dst_regs[o->n_dst++] = id;
}

/* ================================================================
 * Branch classification
 * ================================================================ */

static uint8_t classify_arm64_branch(cs_insn *insn)
{
  switch (insn->id) {
    case ARM64_INS_BR:  return 2; /* BRANCH_INDIRECT */
    case ARM64_INS_BL:  return 4; /* BRANCH_DIRECT_CALL */
    case ARM64_INS_BLR: return 5; /* BRANCH_INDIRECT_CALL */
    case ARM64_INS_RET: return 6; /* BRANCH_RETURN */
    case ARM64_INS_CBZ: case ARM64_INS_CBNZ:
    case ARM64_INS_TBZ: case ARM64_INS_TBNZ: return 3; /* BRANCH_CONDITIONAL */
    case ARM64_INS_B:
      return (insn->detail->arm64.cc == ARM64_CC_AL ||
              insn->detail->arm64.cc == ARM64_CC_INVALID) ? 1 : 3;
    case ARM64_INS_SVC: case ARM64_INS_HVC: case ARM64_INS_SMC:
    case ARM64_INS_BRK: case ARM64_INS_ERET:
    case ARM64_INS_HLT: case ARM64_INS_DCPS1: case ARM64_INS_DCPS2:
    case ARM64_INS_DCPS3: case ARM64_INS_DRPS: return 7; /* BRANCH_OTHER */
    default: return 0; /* NOT_BRANCH */
  }
}

static bool is_bcond(cs_insn *insn)
{
  return insn->id == ARM64_INS_B &&
         insn->detail->arm64.cc != ARM64_CC_AL &&
         insn->detail->arm64.cc != ARM64_CC_INVALID;
}

/* ================================================================
 * Capstone handle: opened once, lazily, on first use.
 * ================================================================ */

static csh cs_handle;
static bool cs_inited = false;

static bool ensure_cs(void)
{
  if (cs_inited) return true;
  if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &cs_handle) != CS_ERR_OK) return false;
  cs_option(cs_handle, CS_OPT_DETAIL, CS_OPT_ON);
  cs_inited = true;
  return true;
}

/* ================================================================
 * decode_aarch64 — public entry point
 * ================================================================ */

decoded_regs_t decode_aarch64(const uint8_t *bytes, uint8_t size)
{
  decoded_regs_t o = {0};
  o.instr_type = INSTR_TYPE_INT;

  if (size != 4 || !ensure_cs()) {
    o.ok = false;
    return o;
  }

  cs_insn *insn = NULL;
  size_t n = cs_disasm(cs_handle, bytes, 4, 0x0, 1, &insn);
  if (n < 1) {
    o.ok = false;
    if (insn) cs_free(insn, n);
    return o;
  }
  cs_arm64 *a = &insn->detail->arm64;

  /* 1. explicit operands */
  for (int i = 0; i < a->op_count; i++) {
    cs_arm64_op *op = &a->operands[i];
    if (op->type == ARM64_OP_REG) {
      uint8_t id = map_arm64_register(op->reg);
      if (op->access & CS_AC_WRITE) add_dst(&o, id);
      if (op->access & CS_AC_READ)  add_src(&o, id);
    } else if (op->type == ARM64_OP_MEM) {
      add_src(&o, map_arm64_register(op->mem.base));
      add_src(&o, map_arm64_register(op->mem.index));
      /* Writeback addressing (e.g. STP [SP,#16]!, LDR [X1],#8) also WRITES
       * the base register -- Capstone 4.0.2 does not report this in
       * regs_write, so it must be derived from insn->detail->arm64.writeback
       * (confirmed field: `bool writeback` in struct cs_arm64,
       * /usr/include/capstone/arm64.h). Only the base is updated, never the
       * index. Per spec S4.2: "a writeback base is read+write -> both." */
      if (a->writeback)
        add_dst(&o, map_arm64_register(op->mem.base));
    }
  }

  /* 2. implicit regs (LR write of BL/BLR, NZCV read of B.cond, etc.) */
  for (uint8_t i = 0; i < insn->detail->regs_read_count; i++)
    add_src(&o, map_arm64_register(insn->detail->regs_read[i]));
  for (uint8_t i = 0; i < insn->detail->regs_write_count; i++)
    add_dst(&o, map_arm64_register(insn->detail->regs_write[i]));

  /* 3. branch classification */
  o.is_branch = classify_arm64_branch(insn);

  /* 4. branch touches (NO SP synthesis) */
  if (o.is_branch == 4 || o.is_branch == 5) add_dst(&o, 94); /* BL/BLR write LR */
  if (o.is_branch == 6) add_src(&o, 94);                     /* RET reads LR   */
  if (is_bcond(insn)) add_src(&o, CS_REG_FLAGS);             /* B.cond only    */
  if (o.is_branch) { /* PC(26) as dst, never evicted */
    bool have_pc = false;
    for (uint8_t i = 0; i < o.n_dst; i++)
      if (o.dst_regs[i] == CS_REG_PC) have_pc = true;
    if (!have_pc) {
      if (o.n_dst >= NUM_INSTR_DESTINATIONS) o.n_dst = NUM_INSTR_DESTINATIONS - 1; /* free a slot */
      o.dst_regs[o.n_dst++] = CS_REG_PC;
    }
  }

  /* 5. instr_type: SIMD iff any mapped reg in [96,127] */
  o.instr_type = INSTR_TYPE_INT;
  for (uint8_t i = 0; i < o.n_src; i++)
    if (o.src_regs[i] >= 96 && o.src_regs[i] <= 127) o.instr_type = INSTR_TYPE_SIMD;
  for (uint8_t i = 0; i < o.n_dst; i++)
    if (o.dst_regs[i] >= 96 && o.dst_regs[i] <= 127) o.instr_type = INSTR_TYPE_SIMD;

  o.ok = true;
  cs_free(insn, n);
  return o;
}
