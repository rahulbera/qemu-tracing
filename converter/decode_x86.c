/*
 * decode_x86.c — x86 instruction decode (Zydis) -> ChampSim register sets
 *
 * Moved verbatim from raw2champsim.c as part of the AArch64 decode-module
 * split. Behavior must remain byte-identical to the pre-refactor inline
 * code: same operand iteration order, same WRITE-before-READ ordering,
 * same fixup_champsim_reg application, same base-before-index handling
 * (base skips RIP, index does not), same bound checks, and the same
 * FLAGS -> SP -> PC synthesis order.
 */

#include "decode.h"

#include <string.h>

#include <Zydis/Zydis.h>

/* ================================================================
 * x86 register mapping: Zydis register -> ChampSim register ID
 *
 * ChampSim uses uint8_t register IDs. We map x86 registers to a
 * compact ID space. Key registers:
 *   6  = RSP (REG_STACK_POINTER)
 *   25 = RFLAGS (REG_FLAGS)
 *   26 = RIP (REG_INSTRUCTION_POINTER)
 * ================================================================ */

static uint8_t map_zydis_register(ZydisRegister reg)
{
  /* Group: GPR 64-bit → IDs 1-16 */
  if (reg >= ZYDIS_REGISTER_RAX && reg <= ZYDIS_REGISTER_R15) {
    return (uint8_t)(reg - ZYDIS_REGISTER_RAX + 1);
    /* RAX=1, RCX=2, RDX=3, RBX=4, RSP=5, RBP=6, RSI=7, RDI=8, R8-R15=9-16 */
    /* Note: RSP maps to 5 here, but ChampSim expects RSP=6 */
  }

  /* Map 32-bit GPRs to their 64-bit counterparts */
  if (reg >= ZYDIS_REGISTER_EAX && reg <= ZYDIS_REGISTER_R15D) {
    return (uint8_t)(reg - ZYDIS_REGISTER_EAX + 1);
  }

  /* Map 16-bit GPRs */
  if (reg >= ZYDIS_REGISTER_AX && reg <= ZYDIS_REGISTER_R15W) {
    return (uint8_t)(reg - ZYDIS_REGISTER_AX + 1);
  }

  /* Map 8-bit GPRs (AL-R15B) */
  if (reg >= ZYDIS_REGISTER_AL && reg <= ZYDIS_REGISTER_R15B) {
    return (uint8_t)(reg - ZYDIS_REGISTER_AL + 1);
  }

  /* High-byte registers (AH, CH, DH, BH) */
  if (reg >= ZYDIS_REGISTER_AH && reg <= ZYDIS_REGISTER_BH) {
    return (uint8_t)(reg - ZYDIS_REGISTER_AH + 1);
  }

  /* RFLAGS */
  if (reg == ZYDIS_REGISTER_RFLAGS || reg == ZYDIS_REGISTER_FLAGS ||
      reg == ZYDIS_REGISTER_EFLAGS) {
    return 25; /* REG_FLAGS */
  }

  /* RIP */
  if (reg == ZYDIS_REGISTER_RIP || reg == ZYDIS_REGISTER_EIP ||
      reg == ZYDIS_REGISTER_IP) {
    return 26; /* REG_INSTRUCTION_POINTER */
  }

  /* RSP special case: ChampSim's REG_STACK_POINTER = 6 */
  /* In our mapping above, RSP gets 5 (RAX=1..RSP=5).
     ChampSim expects RSP=6. Let's fix this. */
  /* Actually, let's look at the Zydis register enum ordering.
     The mapping RAX=1,RCX=2,RDX=3,RBX=4,RSP=5 doesn't match ChampSim
     where RSP=6 has a special meaning (stack pointer detection).
     We'll apply a correction. */

  /* XMM/YMM/ZMM: IDs 32-63 */
  if (reg >= ZYDIS_REGISTER_XMM0 && reg <= ZYDIS_REGISTER_XMM31) {
    return (uint8_t)(32 + (reg - ZYDIS_REGISTER_XMM0));
  }
  if (reg >= ZYDIS_REGISTER_YMM0 && reg <= ZYDIS_REGISTER_YMM31) {
    return (uint8_t)(32 + (reg - ZYDIS_REGISTER_YMM0));
  }
  if (reg >= ZYDIS_REGISTER_ZMM0 && reg <= ZYDIS_REGISTER_ZMM31) {
    return (uint8_t)(32 + (reg - ZYDIS_REGISTER_ZMM0));
  }

  /* ST(0)-ST(7): FP registers, IDs 64-71 */
  if (reg >= ZYDIS_REGISTER_ST0 && reg <= ZYDIS_REGISTER_ST7) {
    return (uint8_t)(64 + (reg - ZYDIS_REGISTER_ST0));
  }

  /* MM0-MM7: MMX registers, IDs 72-79 */
  if (reg >= ZYDIS_REGISTER_MM0 && reg <= ZYDIS_REGISTER_MM7) {
    return (uint8_t)(72 + (reg - ZYDIS_REGISTER_MM0));
  }

  /* Segment registers, K-mask registers, etc: map to a generic ID */
  if (reg != ZYDIS_REGISTER_NONE) {
    return 80; /* generic "other register" */
  }

  return 0; /* no register */
}

/* Fix RSP mapping: Zydis RAX,RCX,RDX,RBX,RSP ordering gives RSP=5,
   but ChampSim needs RSP=6 for stack pointer detection.
   We'll apply a post-mapping fixup. */
static uint8_t fixup_champsim_reg(uint8_t reg_id)
{
  /* In our mapping: RAX=1,RCX=2,RDX=3,RBX=4,RSP=5,RBP=6,...
     ChampSim wants RSP=6. The simplest fix: swap 5 and 6. */
  if (reg_id == 5) return 6;  /* RSP -> 6 (REG_STACK_POINTER) */
  if (reg_id == 6) return 5;  /* RBP -> 5 */
  return reg_id;
}

/* ================================================================
 * Instruction type classification from Zydis
 * ================================================================ */

static uint8_t classify_instr_type(const ZydisDecodedInstruction *insn)
{
  ZydisISAExt ext = insn->meta.isa_ext;

  /* SIMD extensions */
  switch (ext) {
  case ZYDIS_ISA_EXT_SSE:
  case ZYDIS_ISA_EXT_SSE2:
  case ZYDIS_ISA_EXT_SSE3:
  case ZYDIS_ISA_EXT_SSSE3:
  case ZYDIS_ISA_EXT_SSE4:
  case ZYDIS_ISA_EXT_SSE4A:
  case ZYDIS_ISA_EXT_AVX:
  case ZYDIS_ISA_EXT_AVX2:
  case ZYDIS_ISA_EXT_AVX512EVEX:
  case ZYDIS_ISA_EXT_AVX512VEX:
    /* Could be FP or integer SIMD. Check category for FP. */
    break;
  default:
    break;
  }

  /* Check for x87 FP */
  if (ext == ZYDIS_ISA_EXT_X87) {
    return INSTR_TYPE_FP;
  }

  /* Check category for scalar FP (SSE/AVX scalar operations) */
  ZydisInstructionCategory cat = insn->meta.category;
  switch (cat) {
  case ZYDIS_CATEGORY_SSE:
  case ZYDIS_CATEGORY_AVX:
  case ZYDIS_CATEGORY_AVX2:
  case ZYDIS_CATEGORY_AVX512:
    return INSTR_TYPE_SIMD;

  case ZYDIS_CATEGORY_X87_ALU:
    return INSTR_TYPE_FP;

  default:
    break;
  }

  /* Fallback: check ISA extension for SIMD-family instructions */
  switch (ext) {
  case ZYDIS_ISA_EXT_SSE:
  case ZYDIS_ISA_EXT_SSE2:
  case ZYDIS_ISA_EXT_SSE3:
  case ZYDIS_ISA_EXT_SSSE3:
  case ZYDIS_ISA_EXT_SSE4:
  case ZYDIS_ISA_EXT_SSE4A:
  case ZYDIS_ISA_EXT_AVX:
  case ZYDIS_ISA_EXT_AVX2:
  case ZYDIS_ISA_EXT_AVX512EVEX:
  case ZYDIS_ISA_EXT_AVX512VEX:
    return INSTR_TYPE_SIMD;
  default:
    return INSTR_TYPE_INT;
  }
}

/* ================================================================
 * Branch classification
 * ================================================================ */

static bool is_branch_instruction(const ZydisDecodedInstruction *insn)
{
  switch (insn->meta.branch_type) {
  case ZYDIS_BRANCH_TYPE_NONE:
    return false;
  default:
    return true;
  }
}

static uint8_t classify_branch(const ZydisDecodedInstruction *insn)
{
  ZydisMnemonic mnem = insn->mnemonic;

  /* Conditional jumps */
  if (mnem >= ZYDIS_MNEMONIC_JB && mnem <= ZYDIS_MNEMONIC_JS) {
    return 3; /* BRANCH_CONDITIONAL */
  }
  /* JRCXZ, JECXZ, JCXZ */
  if (mnem == ZYDIS_MNEMONIC_JRCXZ) {
    return 3; /* BRANCH_CONDITIONAL */
  }
  /* LOOP variants */
  if (mnem == ZYDIS_MNEMONIC_LOOP || mnem == ZYDIS_MNEMONIC_LOOPE ||
      mnem == ZYDIS_MNEMONIC_LOOPNE) {
    return 3; /* BRANCH_CONDITIONAL */
  }

  /* JMP */
  if (mnem == ZYDIS_MNEMONIC_JMP) {
    /* Direct vs indirect */
    if (insn->operand_count_visible > 0) {
      /* Will check operand type later; for now, use branch type */
      if (insn->meta.branch_type == ZYDIS_BRANCH_TYPE_SHORT ||
          insn->meta.branch_type == ZYDIS_BRANCH_TYPE_NEAR) {
        /* Could still be indirect (jmp [mem] or jmp reg) */
        return 1; /* BRANCH_DIRECT_JUMP — will refine below */
      }
    }
    return 2; /* BRANCH_INDIRECT */
  }

  /* CALL */
  if (mnem == ZYDIS_MNEMONIC_CALL) {
    if (insn->meta.branch_type == ZYDIS_BRANCH_TYPE_SHORT ||
        insn->meta.branch_type == ZYDIS_BRANCH_TYPE_NEAR) {
      return 4; /* BRANCH_DIRECT_CALL — will refine below */
    }
    return 5; /* BRANCH_INDIRECT_CALL */
  }

  /* RET */
  if (mnem == ZYDIS_MNEMONIC_RET) {
    return 6; /* BRANCH_RETURN */
  }

  /* Other branches (INT, SYSCALL, etc.) */
  if (insn->meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
    return 7; /* BRANCH_OTHER */
  }

  return 0; /* NOT_BRANCH */
}

/* ================================================================
 * decode_x86 — public entry point
 * ================================================================ */

decoded_regs_t decode_x86(const uint8_t *bytes, uint8_t size)
{
  static ZydisDecoder decoder;
  static bool         inited = false;
  if (!inited) {
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    inited = true;
  }

  decoded_regs_t out;
  memset(&out, 0, sizeof(out));

  ZydisDecodedInstruction insn;
  ZydisDecodedOperand     operands[ZYDIS_MAX_OPERAND_COUNT];

  ZyanStatus status = ZydisDecoderDecodeFull(
    &decoder, bytes, size, &insn, operands);

  if (!ZYAN_SUCCESS(status)) {
    out.ok         = false;
    out.instr_type = INSTR_TYPE_INT;
    return out;
  }

  /* Branch classification */
  out.is_branch = is_branch_instruction(&insn) ? classify_branch(&insn) : 0;

  /* Instruction type */
  out.instr_type = classify_instr_type(&insn);

  /* Extract registers from operands */
  int src_reg_idx = 0;
  int dst_reg_idx = 0;

  for (int i = 0; i < insn.operand_count; i++) {
    const ZydisDecodedOperand *op = &operands[i];

    /* Skip hidden/implicit memory operands for register extraction,
       but DO process implicit register operands */
    if (op->type == ZYDIS_OPERAND_TYPE_REGISTER) {
      uint8_t reg_id = map_zydis_register(op->reg.value);
      reg_id = fixup_champsim_reg(reg_id);

      if (reg_id == 0) continue;

      if (op->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) {
        if (dst_reg_idx < NUM_INSTR_DESTINATIONS) {
          out.dst_regs[dst_reg_idx++] = reg_id;
        }
      }
      if (op->actions & ZYDIS_OPERAND_ACTION_MASK_READ) {
        if (src_reg_idx < NUM_INSTR_SOURCES) {
          out.src_regs[src_reg_idx++] = reg_id;
        }
      }
    } else if (op->type == ZYDIS_OPERAND_TYPE_MEMORY) {
      /* Extract base and index registers as source registers */
      if (op->mem.base != ZYDIS_REGISTER_NONE &&
          op->mem.base != ZYDIS_REGISTER_RIP) {
        uint8_t reg_id = map_zydis_register(op->mem.base);
        reg_id = fixup_champsim_reg(reg_id);
        if (reg_id != 0 && src_reg_idx < NUM_INSTR_SOURCES) {
          out.src_regs[src_reg_idx++] = reg_id;
        }
      }
      if (op->mem.index != ZYDIS_REGISTER_NONE) {
        uint8_t reg_id = map_zydis_register(op->mem.index);
        reg_id = fixup_champsim_reg(reg_id);
        if (reg_id != 0 && src_reg_idx < NUM_INSTR_SOURCES) {
          out.src_regs[src_reg_idx++] = reg_id;
        }
      }
    }
  }

  /* For branch instructions, ensure FLAGS is a source register
     (needed for conditional branches) */
  if (out.is_branch == 3 /* BRANCH_CONDITIONAL */) {
    bool has_flags = false;
    for (int i = 0; i < src_reg_idx; i++) {
      if (out.src_regs[i] == CS_REG_FLAGS) { has_flags = true; break; }
    }
    if (!has_flags && src_reg_idx < NUM_INSTR_SOURCES) {
      out.src_regs[src_reg_idx++] = CS_REG_FLAGS;
    }
  }

  /* For CALL/RET, ensure RSP is both source and destination */
  if (out.is_branch == 4 || out.is_branch == 5 || out.is_branch == 6) {
    /* CALL or RET modifies RSP */
    bool has_rsp_src = false, has_rsp_dst = false;
    for (int i = 0; i < src_reg_idx; i++)
      if (out.src_regs[i] == CS_REG_SP) has_rsp_src = true;
    for (int i = 0; i < dst_reg_idx; i++)
      if (out.dst_regs[i] == CS_REG_SP) has_rsp_dst = true;
    if (!has_rsp_src && src_reg_idx < NUM_INSTR_SOURCES)
      out.src_regs[src_reg_idx++] = CS_REG_SP;
    if (!has_rsp_dst && dst_reg_idx < NUM_INSTR_DESTINATIONS)
      out.dst_regs[dst_reg_idx++] = CS_REG_SP;
  }

  /* For branch instructions, RIP is a destination */
  if (out.is_branch && dst_reg_idx < NUM_INSTR_DESTINATIONS) {
    out.dst_regs[dst_reg_idx++] = CS_REG_PC;
  }

  out.n_src = (uint8_t)src_reg_idx;
  out.n_dst = (uint8_t)dst_reg_idx;

  out.ok = true;
  return out;
}
