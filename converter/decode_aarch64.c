/*
 * decode_aarch64.c — AArch64 decode stub.
 *
 * Placeholder until the Capstone-based AArch64 decoder lands (Task 2).
 * raw2champsim.c currently refuses arch==1 files before this function
 * would ever be reached; it is stubbed here only so the build links.
 */

#include "decode.h"

decoded_regs_t decode_aarch64(const uint8_t *b, uint8_t s)
{
  (void)b;
  (void)s;
  decoded_regs_t d = {0};
  d.ok         = false;
  d.instr_type = INSTR_TYPE_INT;
  return d;
}
