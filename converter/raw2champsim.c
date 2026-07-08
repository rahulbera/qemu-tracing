/*
 * raw2champsim.c — Convert raw QEMU TCG traces to ChampSim v2 format
 *
 * Input:  .raw.zst per-vCPU trace files (variable-length records)
 * Output: .champsim.zst per-vCPU trace files (512-byte fixed records)
 *
 * Build:
 *   gcc -O2 -o raw2champsim raw2champsim.c \
 *       -Izydis/include -Izydis/dependencies/zycore/include \
 *       -Izydis/build -Izydis/build/zycore \
 *       zydis/build/libZydis.a zydis/build/zycore/libZycore.a \
 *       $(pkg-config --cflags --libs libzstd) -lm
 *
 * Usage:
 *   ./raw2champsim [-v] [-n COUNT] <input.raw.zst> [output.champsim.zst]
 *
 *   -v         Verbose: detailed progress every 1M instructions
 *              (default heartbeat is every 10M instructions)
 *   -n COUNT   Convert only the first COUNT instructions
 *
 * If output is not specified, it is derived from input by replacing
 * .raw.zst with .champsim.zst
 */

#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "decode.h"

/* ================================================================
 * Constants
 * ================================================================ */

#define TRACE_FORMAT_MAGIC 0x46545343 /* "CSTF" */
#define MAX_INSN_SIZE      15
#define MAX_MEM_OPS        7
#define RAW_MAX_VALUE_SIZE 64

/* ChampSim v2 format constants */
#define NUM_INSTR_SOURCES      4
#define NUM_INSTR_DESTINATIONS 2
#define MAX_MEM_VALUE_SIZE     64

/* Compression */
#define ZSTD_COMP_LEVEL 19 /* high compression, decompression speed unaffected */

/* Buffer sizes */
#define READER_COMP_BUF_SIZE   (4 * 1024 * 1024)
#define READER_DECOMP_BUF_SIZE (8 * 1024 * 1024)
#define WRITER_BUF_SIZE        (8 * 1024 * 1024) /* uncompressed output buffer */

/* Instruction type classification */
#define INSTR_TYPE_INT  0
#define INSTR_TYPE_FP   1
#define INSTR_TYPE_SIMD 2

/* ================================================================
 * ChampSim v2 trace record (512 bytes, packed)
 * ================================================================ */

typedef struct __attribute__((packed)) {
  /* Block 1: Vanilla ChampSim layout (64 bytes) */
  uint64_t ip;
  uint8_t  is_branch;
  uint8_t  branch_taken;
  uint8_t  destination_registers[NUM_INSTR_DESTINATIONS];
  uint8_t  source_registers[NUM_INSTR_SOURCES];
  uint64_t destination_memory[NUM_INSTR_DESTINATIONS];
  uint64_t source_memory[NUM_INSTR_SOURCES];

  /* Block 2: Physical addresses + metadata (64 bytes) */
  uint64_t destination_memory_pa[NUM_INSTR_DESTINATIONS];
  uint64_t source_memory_pa[NUM_INSTR_SOURCES];
  uint8_t  source_memory_size[NUM_INSTR_SOURCES];
  uint8_t  destination_memory_size[NUM_INSTR_DESTINATIONS];
  uint8_t  privilege;
  uint8_t  instr_type;
  uint8_t  reserved[8];

  /* Block 3: Memory values (384 bytes) */
  uint8_t  source_memory_value[NUM_INSTR_SOURCES][MAX_MEM_VALUE_SIZE];
  uint8_t  destination_memory_value[NUM_INSTR_DESTINATIONS][MAX_MEM_VALUE_SIZE];
} input_instr_v2;

_Static_assert(sizeof(input_instr_v2) == 512, "input_instr_v2 must be 512 bytes");

/* ================================================================
 * Buffered zstd reader (from trace_inspector)
 * ================================================================ */

typedef struct {
  FILE *fp;
  bool  is_zstd;

  ZSTD_DCtx *dctx;
  uint8_t   *comp_buf;
  size_t     comp_buf_size;
  size_t     comp_buf_pos;
  size_t     comp_buf_filled;
  bool       eof_reached;

  uint8_t *decomp_buf;
  size_t   decomp_buf_size;
  size_t   decomp_buf_pos;
  size_t   decomp_buf_filled;
  bool     stream_done;
} TraceReader;

static bool detect_zstd(FILE *fp)
{
  uint8_t magic[4];
  if (fread(magic, 1, 4, fp) != 4) {
    rewind(fp);
    return false;
  }
  rewind(fp);
  return (magic[0] == 0x28 && magic[1] == 0xB5 &&
          magic[2] == 0x2F && magic[3] == 0xFD);
}

static TraceReader *reader_open(const char *filename)
{
  TraceReader *r = calloc(1, sizeof(TraceReader));
  if (!r) return NULL;

  r->fp = fopen(filename, "rb");
  if (!r->fp) { free(r); return NULL; }

  r->is_zstd = detect_zstd(r->fp);

  if (r->is_zstd) {
    r->dctx = ZSTD_createDCtx();
    if (!r->dctx) { fclose(r->fp); free(r); return NULL; }

    r->comp_buf_size = READER_COMP_BUF_SIZE;
    r->comp_buf      = malloc(r->comp_buf_size);
    r->decomp_buf_size = READER_DECOMP_BUF_SIZE;
    r->decomp_buf      = malloc(r->decomp_buf_size);

    if (!r->comp_buf || !r->decomp_buf) {
      ZSTD_freeDCtx(r->dctx);
      free(r->comp_buf); free(r->decomp_buf);
      fclose(r->fp); free(r);
      return NULL;
    }
  }

  return r;
}

static bool reader_refill_zstd(TraceReader *r)
{
  if (r->stream_done) return false;

  r->decomp_buf_pos    = 0;
  r->decomp_buf_filled = 0;

  while (r->decomp_buf_filled == 0) {
    if (r->comp_buf_pos >= r->comp_buf_filled) {
      if (r->eof_reached) { r->stream_done = true; return false; }
      r->comp_buf_filled = fread(r->comp_buf, 1, r->comp_buf_size, r->fp);
      r->comp_buf_pos    = 0;
      if (r->comp_buf_filled == 0) {
        r->eof_reached = true;
        r->stream_done = true;
        return false;
      }
    }

    ZSTD_inBuffer  input  = { r->comp_buf, r->comp_buf_filled, r->comp_buf_pos };
    ZSTD_outBuffer output = { r->decomp_buf, r->decomp_buf_size, 0 };

    size_t ret = ZSTD_decompressStream(r->dctx, &output, &input);
    if (ZSTD_isError(ret)) {
      fprintf(stderr, "ZSTD decompression error: %s\n", ZSTD_getErrorName(ret));
      r->stream_done = true;
      return false;
    }

    r->comp_buf_pos      = input.pos;
    r->decomp_buf_filled = output.pos;

    if (ret == 0 && r->decomp_buf_filled == 0) {
      if (r->comp_buf_pos >= r->comp_buf_filled && feof(r->fp)) {
        r->stream_done = true;
        return false;
      }
    }
  }
  return true;
}

static size_t reader_read(TraceReader *r, void *buf, size_t len)
{
  if (!r->is_zstd) return fread(buf, 1, len, r->fp);

  uint8_t *dst       = (uint8_t *)buf;
  size_t   remaining = len;

  while (remaining > 0) {
    size_t avail = r->decomp_buf_filled - r->decomp_buf_pos;
    if (avail == 0) {
      if (!reader_refill_zstd(r)) break;
      avail = r->decomp_buf_filled - r->decomp_buf_pos;
    }
    size_t to_copy = (remaining < avail) ? remaining : avail;
    memcpy(dst, r->decomp_buf + r->decomp_buf_pos, to_copy);
    r->decomp_buf_pos += to_copy;
    dst += to_copy;
    remaining -= to_copy;
  }
  return len - remaining;
}

static bool reader_read_exact(TraceReader *r, void *buf, size_t len)
{
  return reader_read(r, buf, len) == len;
}

static void reader_close(TraceReader *r)
{
  if (!r) return;
  if (r->dctx)      ZSTD_freeDCtx(r->dctx);
  if (r->comp_buf)   free(r->comp_buf);
  if (r->decomp_buf) free(r->decomp_buf);
  if (r->fp)         fclose(r->fp);
  free(r);
}

/* ================================================================
 * Buffered zstd writer
 * ================================================================ */

typedef struct {
  FILE      *fp;
  ZSTD_CCtx *cctx;
  uint8_t   *in_buf;        /* uncompressed data to write */
  size_t     in_buf_size;
  size_t     in_buf_used;
  uint8_t   *out_buf;       /* compressed output */
  size_t     out_buf_size;
  uint64_t   total_written;  /* uncompressed bytes */
} TraceWriter;

static TraceWriter *writer_open(const char *filename)
{
  TraceWriter *w = calloc(1, sizeof(TraceWriter));
  if (!w) return NULL;

  w->fp = fopen(filename, "wb");
  if (!w->fp) { free(w); return NULL; }

  w->cctx = ZSTD_createCCtx();
  if (!w->cctx) { fclose(w->fp); free(w); return NULL; }

  ZSTD_CCtx_setParameter(w->cctx, ZSTD_c_compressionLevel, ZSTD_COMP_LEVEL);
  ZSTD_CCtx_setParameter(w->cctx, ZSTD_c_checksumFlag, 1);

  w->in_buf_size = WRITER_BUF_SIZE;
  w->in_buf      = malloc(w->in_buf_size);
  w->out_buf_size = ZSTD_compressBound(w->in_buf_size);
  w->out_buf      = malloc(w->out_buf_size);

  if (!w->in_buf || !w->out_buf) {
    ZSTD_freeCCtx(w->cctx);
    free(w->in_buf); free(w->out_buf);
    fclose(w->fp); free(w);
    return NULL;
  }

  return w;
}

static bool writer_flush(TraceWriter *w, bool final)
{
  if (w->in_buf_used == 0 && !final) return true;

  ZSTD_inBuffer input = { w->in_buf, w->in_buf_used, 0 };

  ZSTD_EndDirective mode = final ? ZSTD_e_end : ZSTD_e_continue;

  do {
    ZSTD_outBuffer output = { w->out_buf, w->out_buf_size, 0 };
    size_t remaining = ZSTD_compressStream2(w->cctx, &output, &input, mode);
    if (ZSTD_isError(remaining)) {
      fprintf(stderr, "ZSTD compression error: %s\n",
              ZSTD_getErrorName(remaining));
      return false;
    }
    if (output.pos > 0) {
      if (fwrite(w->out_buf, 1, output.pos, w->fp) != output.pos) {
        perror("fwrite");
        return false;
      }
    }
    if (mode == ZSTD_e_end && remaining == 0) break;
    if (mode == ZSTD_e_continue) break;
  } while (1);

  w->in_buf_used = 0;
  return true;
}

static bool writer_write(TraceWriter *w, const void *data, size_t len)
{
  w->total_written += len;

  /* If data fits in buffer, just copy */
  if (w->in_buf_used + len <= w->in_buf_size) {
    memcpy(w->in_buf + w->in_buf_used, data, len);
    w->in_buf_used += len;
    return true;
  }

  /* Flush and then write */
  if (!writer_flush(w, false)) return false;

  /* If still too large (unlikely with 8MB buffer), write directly */
  if (len > w->in_buf_size) {
    /* This shouldn't happen with 512-byte records */
    memcpy(w->in_buf, data, len);
    w->in_buf_used = len;
    return writer_flush(w, false);
  }

  memcpy(w->in_buf + w->in_buf_used, data, len);
  w->in_buf_used += len;
  return true;
}

static bool writer_finalize(TraceWriter *w)
{
  return writer_flush(w, true);
}

static void writer_close(TraceWriter *w)
{
  if (!w) return;
  if (w->cctx)    ZSTD_freeCCtx(w->cctx);
  if (w->in_buf)  free(w->in_buf);
  if (w->out_buf) free(w->out_buf);
  if (w->fp)      fclose(w->fp);
  free(w);
}

/* ================================================================
 * Raw trace header decoders
 * ================================================================ */

static inline uint8_t hdr_vcpu_id(uint32_t h)     { return h & 0xF; }
static inline uint8_t hdr_privilege(uint32_t h)    { return (h >> 4) & 0x1; }
static inline uint8_t hdr_instr_size(uint32_t h)   { return (h >> 5) & 0xF; }
static inline uint8_t hdr_num_mem_ops(uint32_t h)  { return (h >> 9) & 0x7; }

/* ================================================================
 * Conversion statistics
 * ================================================================ */

typedef struct {
  uint64_t total_insns;
  uint64_t user_insns;
  uint64_t kernel_insns;
  uint64_t branch_insns;
  uint64_t mem_insns;
  uint64_t decode_failures;
  uint64_t mem_overflow;  /* instructions with >6 mem ops */
  uint64_t type_int;
  uint64_t type_fp;
  uint64_t type_simd;
} ConvertStats;

/* ================================================================
 * Main conversion logic
 * ================================================================ */

int main(int argc, char **argv)
{
  bool     verbose   = false;
  uint64_t max_insns = 0;

  int opt;
  while ((opt = getopt(argc, argv, "vn:")) != -1) {
    switch (opt) {
    case 'v': verbose = true; break;
    case 'n': max_insns = strtoull(optarg, NULL, 10); break;
    default:
      fprintf(stderr, "Usage: %s [-v] [-n COUNT] <input.raw.zst> [output.champsim.zst]\n",
              argv[0]);
      return 1;
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "Usage: %s [-v] [-n COUNT] <input.raw.zst> [output.champsim.zst]\n",
            argv[0]);
    return 1;
  }

  const char *input_file = argv[optind];
  char output_file_buf[1024];
  const char *output_file;

  if (optind + 1 < argc) {
    output_file = argv[optind + 1];
  } else {
    /* Derive output from input: replace .raw.zst with .champsim.zst */
    strncpy(output_file_buf, input_file, sizeof(output_file_buf) - 1);
    output_file_buf[sizeof(output_file_buf) - 1] = '\0';

    char *raw_ext = strstr(output_file_buf, ".raw.zst");
    if (raw_ext) {
      strcpy(raw_ext, ".champsim.zst");
    } else {
      char *dot = strrchr(output_file_buf, '.');
      if (dot) {
        strcpy(dot, ".champsim.zst");
      } else {
        strcat(output_file_buf, ".champsim.zst");
      }
    }
    output_file = output_file_buf;
  }

  /* Open input */
  TraceReader *reader = reader_open(input_file);
  if (!reader) {
    perror(input_file);
    return 1;
  }

  /* Read file header: three u32s + four individual bytes */
  uint32_t magic, version, vcpu_id;
  uint8_t  hdr_tail[4];
  if (!reader_read_exact(reader, &magic, 4) ||
      !reader_read_exact(reader, &version, 4) ||
      !reader_read_exact(reader, &vcpu_id, 4) ||
      !reader_read_exact(reader, hdr_tail, 4)) {
    fprintf(stderr, "ERROR: Cannot read file header\n");
    reader_close(reader);
    return 1;
  }

  if (magic != TRACE_FORMAT_MAGIC) {
    fprintf(stderr, "ERROR: Bad magic: 0x%08X (expected 0x%08X)\n",
            magic, TRACE_FORMAT_MAGIC);
    reader_close(reader);
    return 1;
  }

  if (version != 2 && version != 3) {
    fprintf(stderr, "ERROR: Unsupported format version %u (supported: 2, 3)\n",
            version);
    reader_close(reader);
    return 1;
  }

  uint8_t arch          = 0;
  bool    file_has_pa   = false;
  bool    file_has_vals = true;
  if (version == 3) {
    arch          = hdr_tail[0];
    file_has_pa   = (hdr_tail[1] & 0x1) != 0;
    file_has_vals = (hdr_tail[1] & 0x2) != 0;
    if (arch != 0 && arch != 1) {
      fprintf(stderr,
              "ERROR: Unknown arch byte %u (supported: 0=x86_64, 1=aarch64)\n",
              arch);
      reader_close(reader);
      return 1;
    }
  }

  printf("Input:   %s\n", input_file);
  printf("Output:  %s\n", output_file);
  printf("Format:  v%u, vCPU %u, arch %s, PA %s\n",
         version, vcpu_id, (arch == 1) ? "aarch64" : "x86_64",
         file_has_pa ? "captured" : "absent");
  printf("Compression level: %d\n", ZSTD_COMP_LEVEL);
  printf("\n");

  /* Open output */
  TraceWriter *writer = writer_open(output_file);
  if (!writer) {
    perror(output_file);
    reader_close(reader);
    return 1;
  }

  ConvertStats stats = {0};
  int          exit_status = 0; /* set to 1 on any truncated/corrupt record */

  /* We need to look ahead one instruction to determine branch_taken.
   * Strategy: decode current instruction, but defer writing until we
   * know the next IP. */

  input_instr_v2 prev_record;
  bool           has_prev        = false;
  uint64_t       prev_ip         = 0;
  uint8_t        prev_instr_size = 0;

  while (1) {
    if (max_insns > 0 && stats.total_insns >= max_insns) break;

    /* Read raw record header */
    uint32_t header;
    if (!reader_read_exact(reader, &header, 4)) break;

    uint8_t priv = hdr_privilege(header);
    uint8_t isz  = hdr_instr_size(header);
    uint8_t nmem = hdr_num_mem_ops(header);

    if (isz == 0 || isz > MAX_INSN_SIZE) {
      fprintf(stderr, "ERROR at insn #%" PRIu64 ": invalid instr_size=%u\n",
              stats.total_insns, isz);
      exit_status = 1;
      break;
    }
    if (nmem > MAX_MEM_OPS) {
      fprintf(stderr, "ERROR at insn #%" PRIu64 ": invalid num_mem_ops=%u\n",
              stats.total_insns, nmem);
      exit_status = 1;
      break;
    }

    /* Read IP */
    uint64_t ip;
    if (!reader_read_exact(reader, &ip, 8)) {
      fprintf(stderr, "ERROR: truncated record (IP) at insn #%" PRIu64 "\n",
              stats.total_insns);
      exit_status = 1;
      break;
    }

    /* Read instruction bytes */
    uint8_t insn_bytes[MAX_INSN_SIZE];
    if (!reader_read_exact(reader, insn_bytes, isz)) {
      fprintf(stderr, "ERROR: truncated record (bytes) at insn #%" PRIu64 "\n",
              stats.total_insns);
      exit_status = 1;
      break;
    }

    /* Read memory operations */
    typedef struct {
      uint64_t addr;
      uint64_t paddr;
      uint8_t  size;
      uint8_t  flags;
      bool     is_write;
      bool     has_value;
      bool     pa_valid;
      bool     pa_is_io;
      uint8_t  value[RAW_MAX_VALUE_SIZE];
      uint8_t  value_len;
    } RawMemOp;

    RawMemOp mem_ops[MAX_MEM_OPS];
    int      num_reads  = 0;
    int      num_writes = 0;

    for (int m = 0; m < nmem; m++) {
      mem_ops[m].paddr    = 0;
      mem_ops[m].pa_valid = false;
      mem_ops[m].pa_is_io = false;

      if (!reader_read_exact(reader, &mem_ops[m].addr, 8)) goto trunc_memop;
      if (file_has_pa &&
          !reader_read_exact(reader, &mem_ops[m].paddr, 8)) goto trunc_memop;
      if (!reader_read_exact(reader, &mem_ops[m].size, 1) ||
          !reader_read_exact(reader, &mem_ops[m].flags, 1)) {
      trunc_memop:
        fprintf(stderr, "ERROR: truncated mem op at insn #%" PRIu64 "\n",
                stats.total_insns);
        exit_status = 1;
        goto done;
      }

      mem_ops[m].is_write  = (mem_ops[m].flags & 0x1) != 0;
      mem_ops[m].has_value = (mem_ops[m].flags & 0x2) != 0;
      mem_ops[m].pa_valid  = (mem_ops[m].flags & 0x4) != 0;
      mem_ops[m].pa_is_io  = (mem_ops[m].flags & 0x8) != 0;
      mem_ops[m].value_len = 0;

      if (mem_ops[m].has_value && !file_has_vals) {
        fprintf(stderr,
                "ERROR: corrupt file — has_value set but header "
                "has_values=0 (insn #%" PRIu64 ")\n",
                stats.total_insns);
        exit_status = 1;
        goto done;
      }

      if (mem_ops[m].has_value) {
        uint8_t vbytes = mem_ops[m].size;
        if (vbytes > RAW_MAX_VALUE_SIZE) vbytes = RAW_MAX_VALUE_SIZE;
        if (!reader_read_exact(reader, mem_ops[m].value, vbytes)) {
          fprintf(stderr, "ERROR: truncated value at insn #%" PRIu64 "\n",
                  stats.total_insns);
          exit_status = 1;
          goto done;
        }
        mem_ops[m].value_len = vbytes;
      }

      if (mem_ops[m].is_write) num_writes++;
      else                     num_reads++;
    }

    /* Determine branch_taken for PREVIOUS instruction */
    if (has_prev) {
      /* A branch is "taken" if the next IP is not prev_ip + prev_instr_size */
      if (prev_record.is_branch) {
        prev_record.branch_taken =
          (ip != prev_ip + prev_instr_size) ? 1 : 0;
      }
      /* Write previous record */
      if (!writer_write(writer, &prev_record, sizeof(prev_record))) {
        fprintf(stderr, "ERROR: write failed\n");
        exit_status = 1;
        goto done;
      }
    }

    /* Build current v2 record */
    input_instr_v2 rec;
    memset(&rec, 0, sizeof(rec));

    rec.ip        = ip;
    rec.privilege = priv;

    /* Decode instruction (x86 via Zydis, AArch64 via Capstone) and copy
       the resulting register sets into the record. */
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

    /* Map memory operations to source/destination slots */
    int src_mem_idx = 0;
    int dst_mem_idx = 0;

    for (int m = 0; m < nmem; m++) {
      if (mem_ops[m].is_write) {
        if (dst_mem_idx < NUM_INSTR_DESTINATIONS) {
          rec.destination_memory[dst_mem_idx]      = mem_ops[m].addr;
          rec.destination_memory_size[dst_mem_idx]  = mem_ops[m].size;
          if (mem_ops[m].pa_valid && !mem_ops[m].pa_is_io) {
            rec.destination_memory_pa[dst_mem_idx] = mem_ops[m].paddr;
          }
          if (mem_ops[m].has_value && mem_ops[m].value_len > 0) {
            uint8_t copy_len = mem_ops[m].value_len;
            if (copy_len > MAX_MEM_VALUE_SIZE) copy_len = MAX_MEM_VALUE_SIZE;
            memcpy(rec.destination_memory_value[dst_mem_idx],
                   mem_ops[m].value, copy_len);
          }
          dst_mem_idx++;
        } else {
          stats.mem_overflow++;
        }
      } else {
        if (src_mem_idx < NUM_INSTR_SOURCES) {
          rec.source_memory[src_mem_idx]      = mem_ops[m].addr;
          rec.source_memory_size[src_mem_idx]  = mem_ops[m].size;
          if (mem_ops[m].pa_valid && !mem_ops[m].pa_is_io) {
            rec.source_memory_pa[src_mem_idx] = mem_ops[m].paddr;
          }
          if (mem_ops[m].has_value && mem_ops[m].value_len > 0) {
            uint8_t copy_len = mem_ops[m].value_len;
            if (copy_len > MAX_MEM_VALUE_SIZE) copy_len = MAX_MEM_VALUE_SIZE;
            memcpy(rec.source_memory_value[src_mem_idx],
                   mem_ops[m].value, copy_len);
          }
          src_mem_idx++;
        } else {
          stats.mem_overflow++;
        }
      }
    }

    if (src_mem_idx > 0 || dst_mem_idx > 0) stats.mem_insns++;

    /* Track stats */
    stats.total_insns++;
    if (priv) stats.kernel_insns++;
    else      stats.user_insns++;

    /* Save for next iteration (to determine branch_taken) */
    prev_record     = rec;
    prev_ip         = ip;
    prev_instr_size = isz;
    has_prev        = true;

    /* Progress heartbeat. Coarse (every 10M insns) by DEFAULT so long
       chunks show a steady sign of life instead of looking hung; fine
       (every 1M) with -v. Always to stderr — never touches the record
       stream or the stdout summary — and flushed so it appears promptly
       in a redirected log. */
    uint64_t hb_interval = verbose ? 1000000ULL : 10000000ULL;
    if (stats.total_insns > 0 && (stats.total_insns % hb_interval) == 0) {
      fprintf(stderr, "[raw2champsim] %"PRIu64"M insns | "
              "user %.1f%% kern %.1f%% | "
              "branch %.1f%% mem %.1f%% | "
              "decode_fail %"PRIu64" memop_overflow %"PRIu64" | "
              "INT %"PRIu64" FP %"PRIu64" SIMD %"PRIu64"\n",
              stats.total_insns / 1000000,
              100.0 * stats.user_insns / stats.total_insns,
              100.0 * stats.kernel_insns / stats.total_insns,
              100.0 * stats.branch_insns / stats.total_insns,
              100.0 * stats.mem_insns / stats.total_insns,
              stats.decode_failures,
              stats.mem_overflow,
              stats.type_int, stats.type_fp, stats.type_simd);
      fflush(stderr);
    }
  }

  /* Write the last instruction (assume branch not taken for final) */
  if (has_prev) {
    if (!writer_write(writer, &prev_record, sizeof(prev_record))) {
      fprintf(stderr, "ERROR: write failed (final)\n");
    }
  }

done:
  if (verbose) fprintf(stderr, "\n");

  /* Finalize output */
  if (!writer_finalize(writer)) {
    fprintf(stderr, "ERROR: failed to finalize compressed output\n");
  }

  /* Get compressed size */
  fflush(writer->fp);
  long compressed_size = ftell(writer->fp);

  reader_close(reader);
  writer_close(writer);

  /* Print statistics */
  printf("=== Conversion Complete ===\n");
  printf("Total instructions:     %" PRIu64 "\n", stats.total_insns);
  printf("  User mode:            %" PRIu64 " (%.1f%%)\n",
         stats.user_insns,
         stats.total_insns ? 100.0 * stats.user_insns / stats.total_insns : 0);
  printf("  Kernel mode:          %" PRIu64 " (%.1f%%)\n",
         stats.kernel_insns,
         stats.total_insns ? 100.0 * stats.kernel_insns / stats.total_insns : 0);
  printf("  Branches:             %" PRIu64 " (%.1f%%)\n",
         stats.branch_insns,
         stats.total_insns ? 100.0 * stats.branch_insns / stats.total_insns : 0);
  printf("  Memory instructions:  %" PRIu64 " (%.1f%%)\n",
         stats.mem_insns,
         stats.total_insns ? 100.0 * stats.mem_insns / stats.total_insns : 0);
  printf("  Decode failures:      %" PRIu64 "\n", stats.decode_failures);
  printf("  Mem op overflows:     %" PRIu64 "\n", stats.mem_overflow);
  printf("  Type INT:             %" PRIu64 "\n", stats.type_int);
  printf("  Type FP:              %" PRIu64 "\n", stats.type_fp);
  printf("  Type SIMD:            %" PRIu64 "\n", stats.type_simd);
  printf("\n");
  printf("Output size:  %.2f MB (compressed, level %d)\n",
         compressed_size / (1024.0 * 1024.0), ZSTD_COMP_LEVEL);
  printf("Uncompressed: %.2f MB (%zu bytes/instr x %" PRIu64 " instrs)\n",
         (double)(stats.total_insns * sizeof(input_instr_v2)) / (1024.0 * 1024.0),
         sizeof(input_instr_v2), stats.total_insns);
  if (compressed_size > 0 && stats.total_insns > 0) {
    double ratio = (double)(stats.total_insns * sizeof(input_instr_v2)) / compressed_size;
    printf("Compression ratio: %.1f:1 (%.1f bytes/instr on disk)\n",
           ratio, (double)compressed_size / stats.total_insns);
  }

  return exit_status;
}
