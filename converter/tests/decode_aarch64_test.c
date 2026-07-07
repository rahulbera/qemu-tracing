/*
 * decode_aarch64_test.c — golden unit test for the AArch64 (A64) decode
 * backend, per docs/superpowers/specs/2026-07-07-raw2champsim-aarch64-design.md
 * S5.1.
 *
 * Links decode_aarch64.o + Capstone directly (no raw2champsim.c, no Zydis) --
 * the backend is self-complete (S2.1/S9), so calling decode_aarch64() alone
 * reaches the final decoded_regs_t fields the golden table asserts on.
 *
 * Each row hand-encodes a little-endian A64 instruction and asserts:
 *   - is_branch   (exact)
 *   - instr_type  (exact)
 *   - src/dst register ID sets (order-independent, exact membership + count)
 *
 * Encodings were generated and verified with the installed aarch64-linux-gnu
 * binutils toolchain (`aarch64-linux-gnu-as` + `objdump -d` + `objcopy -O
 * binary`) at authoring time -- see the disassembly transcript in
 * converter/tests/decode_aarch64_test.c's companion report
 * (.superpowers/sdd/task-3-report.md) for the exact commands and output.
 * RET (c0035fd6) and NOP (1f2003d5, unused here) match the spec's stated
 * encodings.
 *
 * Prints "PASS n/14" and exits 0 iff every row passes; on any mismatch
 * prints expected vs actual for the failing row(s) and exits nonzero.
 *
 * IMPORTANT: the golden table (spec S5.1) is the oracle. decode_aarch64()
 * is under test. If a row fails here, do NOT weaken this test to make it
 * pass -- the mapping/branch logic in decode_aarch64.c must be fixed
 * instead (per the brief and the plan's Step 2 acceptance gate).
 */

#include "../decode.h"

#include <stdio.h>
#include <string.h>

/* ================================================================
 * Golden table (spec S5.1)
 * ================================================================ */

typedef struct {
  const char   *name;
  uint8_t       bytes[4];
  uint8_t       exp_is_branch;
  uint8_t       exp_instr_type;
  const uint8_t *exp_src;
  uint8_t       exp_n_src;
  const uint8_t *exp_dst;
  uint8_t       exp_n_dst;
} golden_row_t;

/* Expected register sets (order does not matter -- checked as sets). */
static const uint8_t RET_src[]      = { 94 };                 /* LR */
static const uint8_t RET_dst[]      = { 26 };                 /* PC */

static const uint8_t BL_dst[]       = { 94, 26 };              /* LR, PC */

static const uint8_t BLR_X0_src[]   = { 64 };                  /* X0 */
static const uint8_t BLR_X0_dst[]   = { 94, 26 };               /* LR, PC */

static const uint8_t BR_X1_src[]    = { 65 };                  /* X1 */
static const uint8_t BR_X1_dst[]    = { 26 };                  /* PC */

static const uint8_t B_dst[]        = { 26 };                  /* PC */

static const uint8_t Beq_src[]      = { 25 };                  /* NZCV */
static const uint8_t Beq_dst[]      = { 26 };                  /* PC */

static const uint8_t CBZ_X2_src[]   = { 66 };                  /* X2 */
static const uint8_t CBZ_X2_dst[]   = { 26 };                  /* PC */

static const uint8_t ADD_src[]      = { 65, 66 };               /* X1, X2 */
static const uint8_t ADD_dst[]      = { 64 };                  /* X0 */

static const uint8_t LDR_src[]      = { 65, 66 };               /* X1, X2 */
static const uint8_t LDR_dst[]      = { 64 };                  /* X0 */

static const uint8_t STP_src[]      = { 64, 65, 6 };            /* X0, X1, SP */

/* Writeback rows (FIX 1 regression cover): a writeback base register is
 * read+write -- both src and dst. Only the base is written, never the
 * index (LDR has no index operand here; base X1 is the one that moves). */
static const uint8_t STP_WB_src[]   = { 64, 65, 6 };            /* X0, X1, SP */
static const uint8_t STP_WB_dst[]   = { 6 };                    /* SP (writeback) */

static const uint8_t LDR_WB_src[]   = { 65 };                   /* X1 */
static const uint8_t LDR_WB_dst[]   = { 64, 65 };               /* X0, X1 (writeback) */

static const uint8_t FADD_src[]     = { 97, 98 };               /* V1, V2 (S view) */
static const uint8_t FADD_dst[]     = { 96 };                  /* V0 (S view) */

static const uint8_t ADDV_src[]     = { 97, 98 };               /* V1, V2 (4S view) */
static const uint8_t ADDV_dst[]     = { 96 };                  /* V0 (4S view) */

/*
 * Encodings (little-endian bytes), verified via
 * `aarch64-linux-gnu-as` + `objdump -d` + `objcopy -O binary` (see report):
 *
 *   ret                        -> c0 03 5f d6
 *   bl _start                  -> 00 00 00 94
 *   blr x0                     -> 00 00 3f d6
 *   br x1                      -> 20 00 1f d6
 *   b _start                   -> 00 00 00 14
 *   b.eq _start                -> 00 00 00 54
 *   cbz x2, _start             -> 02 00 00 b4
 *   add x0, x1, x2             -> 20 00 02 8b
 *   ldr x0, [x1, x2]           -> 20 68 62 f8
 *   stp x0, x1, [sp, #16]      -> e0 07 01 a9
 *   fadd s0, s1, s2            -> 20 28 22 1e
 *   add v0.4s, v1.4s, v2.4s    -> 20 84 a2 4e
 *
 * Writeback rows (FIX 1), verified the same way:
 *   stp x0, x1, [sp, #16]!     -> e0 07 81 a9   (pre-index writeback)
 *   ldr x0, [x1], #8           -> 20 84 40 f8   (post-index writeback)
 */
static const golden_row_t GOLDEN[] = {
  { "RET",                     {0xc0,0x03,0x5f,0xd6}, 6, INSTR_TYPE_INT,
    RET_src, 1, RET_dst, 1 },
  { "BL #x",                   {0x00,0x00,0x00,0x94}, 4, INSTR_TYPE_INT,
    NULL, 0, BL_dst, 2 },
  { "BLR X0",                  {0x00,0x00,0x3f,0xd6}, 5, INSTR_TYPE_INT,
    BLR_X0_src, 1, BLR_X0_dst, 2 },
  { "BR X1",                   {0x20,0x00,0x1f,0xd6}, 2, INSTR_TYPE_INT,
    BR_X1_src, 1, BR_X1_dst, 1 },
  { "B #x",                    {0x00,0x00,0x00,0x14}, 1, INSTR_TYPE_INT,
    NULL, 0, B_dst, 1 },
  { "B.eq #x",                 {0x00,0x00,0x00,0x54}, 3, INSTR_TYPE_INT,
    Beq_src, 1, Beq_dst, 1 },
  { "CBZ X2,#x",                {0x02,0x00,0x00,0xb4}, 3, INSTR_TYPE_INT,
    CBZ_X2_src, 1, CBZ_X2_dst, 1 },
  { "ADD X0,X1,X2",             {0x20,0x00,0x02,0x8b}, 0, INSTR_TYPE_INT,
    ADD_src, 2, ADD_dst, 1 },
  { "LDR X0,[X1,X2]",           {0x20,0x68,0x62,0xf8}, 0, INSTR_TYPE_INT,
    LDR_src, 2, LDR_dst, 1 },
  { "STP X0,X1,[SP,#16]",       {0xe0,0x07,0x01,0xa9}, 0, INSTR_TYPE_INT,
    STP_src, 3, NULL, 0 },
  { "STP X0,X1,[SP,#16]!",      {0xe0,0x07,0x81,0xa9}, 0, INSTR_TYPE_INT,
    STP_WB_src, 3, STP_WB_dst, 1 },
  { "LDR X0,[X1],#8",           {0x20,0x84,0x40,0xf8}, 0, INSTR_TYPE_INT,
    LDR_WB_src, 1, LDR_WB_dst, 2 },
  { "FADD S0,S1,S2",            {0x20,0x28,0x22,0x1e}, 0, INSTR_TYPE_SIMD,
    FADD_src, 2, FADD_dst, 1 },
  { "ADD V0.4S,V1.4S,V2.4S",    {0x20,0x84,0xa2,0x4e}, 0, INSTR_TYPE_SIMD,
    ADDV_src, 2, ADDV_dst, 1 },
};

#define NUM_GOLDEN (sizeof(GOLDEN) / sizeof(GOLDEN[0]))

/* ================================================================
 * Set-membership helpers (order-independent, exact count)
 * ================================================================ */

static int contains(const uint8_t *arr, uint8_t n, uint8_t v)
{
  for (uint8_t i = 0; i < n; i++)
    if (arr[i] == v) return 1;
  return 0;
}

/* True iff {a[0..na)} == {b[0..nb)} as sets (no duplicates assumed in
 * either -- decode_aarch64's add_src/add_dst dedupe, and the golden
 * arrays above are hand-written without duplicates). */
static int set_equal(const uint8_t *a, uint8_t na, const uint8_t *b, uint8_t nb)
{
  if (na != nb) return 0;
  for (uint8_t i = 0; i < na; i++)
    if (!contains(b, nb, a[i])) return 0;
  return 1;
}

static void print_set(const uint8_t *a, uint8_t n)
{
  printf("{");
  for (uint8_t i = 0; i < n; i++) printf("%s%u", i ? "," : "", a[i]);
  printf("}");
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
  int npass = 0;
  int nfail = 0;

  for (size_t i = 0; i < NUM_GOLDEN; i++) {
    const golden_row_t *g = &GOLDEN[i];
    decoded_regs_t d = decode_aarch64(g->bytes, 4);

    int row_ok = 1;

    if (!d.ok) {
      printf("FAIL [%s]: decode_aarch64 returned ok=false (expected a "
             "successful decode)\n", g->name);
      row_ok = 0;
    }
    if (d.ok && d.is_branch != g->exp_is_branch) {
      printf("FAIL [%s]: is_branch expected=%u actual=%u\n",
             g->name, g->exp_is_branch, d.is_branch);
      row_ok = 0;
    }
    if (d.ok && d.instr_type != g->exp_instr_type) {
      printf("FAIL [%s]: instr_type expected=%u actual=%u\n",
             g->name, g->exp_instr_type, d.instr_type);
      row_ok = 0;
    }
    if (d.ok && !set_equal(d.src_regs, d.n_src, g->exp_src, g->exp_n_src)) {
      printf("FAIL [%s]: src_regs expected=", g->name);
      print_set(g->exp_src, g->exp_n_src);
      printf(" actual=");
      print_set(d.src_regs, d.n_src);
      printf("\n");
      row_ok = 0;
    }
    if (d.ok && !set_equal(d.dst_regs, d.n_dst, g->exp_dst, g->exp_n_dst)) {
      printf("FAIL [%s]: dst_regs expected=", g->name);
      print_set(g->exp_dst, g->exp_n_dst);
      printf(" actual=");
      print_set(d.dst_regs, d.n_dst);
      printf("\n");
      row_ok = 0;
    }

    if (row_ok) npass++;
    else        nfail++;
  }

  printf("PASS %d/%d\n", npass, (int)NUM_GOLDEN);
  return (nfail == 0 && npass == (int)NUM_GOLDEN) ? 0 : 1;
}
