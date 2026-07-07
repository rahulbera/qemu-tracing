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
