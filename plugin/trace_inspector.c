/*
 * trace_inspector.c — Validate and inspect raw trace files (v1/v2)
 *
 * Supports both compressed (.raw.zst) and uncompressed (.raw) input.
 * Auto-detects format from the first 4 bytes of the file.
 *
 * Build:
 *   gcc -O2 -o trace_inspector trace_inspector.c $(pkg-config --libs --cflags
 * libzstd)
 *
 * Usage:
 *   ./trace_inspector [-v] [-n COUNT] <trace_file.raw.zst|trace_file.raw>
 *
 *   -v         Verbose: print each instruction record with values
 *   -n COUNT   Process only the first COUNT instructions (default: all)
 */

#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define TRACE_FORMAT_MAGIC 0x46545343
#define MAX_INSN_SIZE      15
#define MAX_MEM_OPS        7
#define MAX_VALUE_SIZE     64

/* ================================================================
 * Buffered reader: abstracts over raw and zstd-compressed input
 *
 * For raw files: thin wrapper around fread.
 * For zstd files: maintains a compressed input buffer and a
 * decompressed output buffer, refilling as needed.
 * ================================================================ */

#define READER_COMP_BUF_SIZE   (4 * 1024 * 1024) /* 4 MB compressed */
#define READER_DECOMP_BUF_SIZE (8 * 1024 * 1024) /* 8 MB decompressed */

typedef struct {
  FILE *fp;
  bool  is_zstd;

  /* zstd streaming state */
  ZSTD_DCtx *dctx;

  uint8_t *comp_buf; /* compressed data read from file */
  size_t   comp_buf_size;
  size_t   comp_buf_pos;    /* current read position in comp_buf */
  size_t   comp_buf_filled; /* bytes of valid data in comp_buf */
  bool     eof_reached;     /* no more compressed data from file */

  uint8_t *decomp_buf; /* decompressed data available to caller */
  size_t   decomp_buf_size;
  size_t   decomp_buf_pos;    /* current read position in decomp_buf */
  size_t   decomp_buf_filled; /* bytes of valid decompressed data */
  bool     stream_done;       /* zstd stream fully consumed */
} TraceReader;

/* Detect if file starts with zstd magic number (0xFD2FB528) */
static bool detect_zstd(FILE *fp)
{
  uint8_t magic[4];
  if (fread(magic, 1, 4, fp) != 4) {
    rewind(fp);
    return false;
  }
  rewind(fp);
  /* zstd magic: 28 B5 2F FD (little-endian: 0xFD2FB528) */
  return (magic[0] == 0x28 && magic[1] == 0xB5 && magic[2] == 0x2F &&
          magic[3] == 0xFD);
}

static TraceReader *reader_open(const char *filename)
{
  TraceReader *r = calloc(1, sizeof(TraceReader));
  if (!r)
    return NULL;

  r->fp = fopen(filename, "rb");
  if (!r->fp) {
    free(r);
    return NULL;
  }

  r->is_zstd = detect_zstd(r->fp);

  if (r->is_zstd) {
    r->dctx = ZSTD_createDCtx();
    if (!r->dctx) {
      fclose(r->fp);
      free(r);
      return NULL;
    }

    r->comp_buf_size = READER_COMP_BUF_SIZE;
    r->comp_buf      = malloc(r->comp_buf_size);

    r->decomp_buf_size = READER_DECOMP_BUF_SIZE;
    r->decomp_buf      = malloc(r->decomp_buf_size);

    if (!r->comp_buf || !r->decomp_buf) {
      ZSTD_freeDCtx(r->dctx);
      free(r->comp_buf);
      free(r->decomp_buf);
      fclose(r->fp);
      free(r);
      return NULL;
    }

    r->comp_buf_pos      = 0;
    r->comp_buf_filled   = 0;
    r->decomp_buf_pos    = 0;
    r->decomp_buf_filled = 0;
    r->eof_reached       = false;
    r->stream_done       = false;
  }

  return r;
}

/*
 * Refill the decompressed buffer by reading compressed data from
 * the file and running it through zstd decompression.
 */
static bool reader_refill_zstd(TraceReader *r)
{
  if (r->stream_done) {
    return false;
  }

  r->decomp_buf_pos    = 0;
  r->decomp_buf_filled = 0;

  while (r->decomp_buf_filled == 0) {
    /* Refill compressed buffer if exhausted */
    if (r->comp_buf_pos >= r->comp_buf_filled) {
      if (r->eof_reached) {
        r->stream_done = true;
        return false;
      }
      r->comp_buf_filled = fread(r->comp_buf, 1, r->comp_buf_size, r->fp);
      r->comp_buf_pos    = 0;
      if (r->comp_buf_filled == 0) {
        r->eof_reached = true;
        r->stream_done = true;
        return false;
      }
    }

    ZSTD_inBuffer input = {.src  = r->comp_buf,
                           .size = r->comp_buf_filled,
                           .pos  = r->comp_buf_pos};

    ZSTD_outBuffer output = {.dst  = r->decomp_buf,
                             .size = r->decomp_buf_size,
                             .pos  = 0};

    size_t ret = ZSTD_decompressStream(r->dctx, &output, &input);
    if (ZSTD_isError(ret)) {
      fprintf(stderr, "ZSTD decompression error: %s\n", ZSTD_getErrorName(ret));
      r->stream_done = true;
      return false;
    }

    r->comp_buf_pos      = input.pos;
    r->decomp_buf_filled = output.pos;

    /* ret == 0 means end of a frame; check if there's more data */
    if (ret == 0 && r->decomp_buf_filled == 0) {
      if (r->comp_buf_pos >= r->comp_buf_filled && feof(r->fp)) {
        r->stream_done = true;
        return false;
      }
    }
  }

  return true;
}

/*
 * Read exactly `len` bytes from the trace.
 * Returns the number of bytes actually read.
 */
static size_t reader_read(TraceReader *r, void *buf, size_t len)
{
  if (!r->is_zstd) {
    return fread(buf, 1, len, r->fp);
  }

  uint8_t *dst       = (uint8_t *)buf;
  size_t   remaining = len;

  while (remaining > 0) {
    /* Check if decompressed buffer has data */
    size_t avail = r->decomp_buf_filled - r->decomp_buf_pos;
    if (avail == 0) {
      if (!reader_refill_zstd(r)) {
        break; /* no more data */
      }
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

/* Convenience: read exactly N bytes, return true on success */
static bool reader_read_exact(TraceReader *r, void *buf, size_t len)
{
  return reader_read(r, buf, len) == len;
}

static void reader_close(TraceReader *r)
{
  if (!r)
    return;
  if (r->dctx)
    ZSTD_freeDCtx(r->dctx);
  if (r->comp_buf)
    free(r->comp_buf);
  if (r->decomp_buf)
    free(r->decomp_buf);
  if (r->fp)
    fclose(r->fp);
  free(r);
}

/* ================================================================
 * Header field decoders
 * ================================================================ */

static inline uint8_t hdr_vcpu_id(uint32_t h)
{
  return h & 0xF;
}

static inline uint8_t hdr_privilege(uint32_t h)
{
  return (h >> 4) & 0x1;
}

static inline uint8_t hdr_instr_size(uint32_t h)
{
  return (h >> 5) & 0xF;
}

static inline uint8_t hdr_num_mem_ops(uint32_t h)
{
  return (h >> 9) & 0x7;
}

/* ================================================================
 * Main
 * ================================================================ */

int main(int argc, char **argv)
{
  bool     verbose   = false;
  uint64_t max_insns = 0;

  int opt;
  while ((opt = getopt(argc, argv, "vn:")) != -1) {
    switch (opt) {
    case 'v':
      verbose = true;
      break;
    case 'n':
      max_insns = strtoull(optarg, NULL, 10);
      break;
    default:
      fprintf(stderr, "Usage: %s [-v] [-n COUNT] <trace_file>\n", argv[0]);
      return 1;
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "Usage: %s [-v] [-n COUNT] <trace_file>\n", argv[0]);
    return 1;
  }

  const char  *filename = argv[optind];
  TraceReader *r        = reader_open(filename);
  if (!r) {
    perror(filename);
    return 1;
  }

  /* Read file header: three u32s + four individual bytes (v3 fields
     must be decoded as u8s, never as a u32 — see spec 3.1) */
  uint32_t magic, version, vcpu_id;
  uint8_t  hdr_tail[4];
  if (!reader_read_exact(r, &magic, 4) || !reader_read_exact(r, &version, 4) ||
      !reader_read_exact(r, &vcpu_id, 4) ||
      !reader_read_exact(r, hdr_tail, 4)) {
    fprintf(stderr, "ERROR: Cannot read file header\n");
    reader_close(r);
    return 1;
  }

  if (magic != TRACE_FORMAT_MAGIC) {
    fprintf(stderr,
            "ERROR: Bad magic: 0x%08X (expected 0x%08X)\n",
            magic,
            TRACE_FORMAT_MAGIC);
    reader_close(r);
    return 1;
  }

  if (version != 2 && version != 3) {
    fprintf(stderr,
            "ERROR: Unsupported format version %u (supported: 2, 3)\n",
            version);
    reader_close(r);
    return 1;
  }

  uint8_t arch          = 0;     /* v2 files are x86_64 by definition */
  bool    file_has_pa   = false;
  bool    file_has_vals = true;  /* v2: value capture always on */
  uint8_t value_cap     = 16;

  if (version == 3) {
    arch          = hdr_tail[0];
    file_has_pa   = (hdr_tail[1] & 0x1) != 0;
    file_has_vals = (hdr_tail[1] & 0x2) != 0;
    value_cap     = hdr_tail[2];
    if (arch != 0 && arch != 1) {
      fprintf(stderr,
              "ERROR: Unknown arch byte %u (supported: 0=x86_64, 1=aarch64)\n",
              arch);
      reader_close(r);
      return 1;
    }
  }

  printf("=== Trace File: %s ===\n", filename);
  printf("Compression: %s\n", r->is_zstd ? "zstd" : "none");
  printf("Format version: %u\n", version);
  printf("vCPU ID: %u\n", vcpu_id);
  printf("Arch: %s\n", arch == 1 ? "aarch64" : "x86_64");
  printf("PA capture: %s\n", file_has_pa ? "yes" : "no");
  printf("Value capture: %s (cap %u bytes)\n",
         file_has_vals ? "enabled" : "disabled",
         value_cap);
  printf("\n");

  /* Statistics */
  uint64_t total_insns    = 0;
  uint64_t total_mem_ops  = 0;
  uint64_t user_insns     = 0;
  uint64_t kernel_insns   = 0;
  uint64_t mem_reads      = 0;
  uint64_t mem_writes     = 0;
  uint64_t values_present = 0;
  uint64_t branch_count   = 0;
  uint64_t prev_ip        = 0;
  uint8_t  prev_size      = 0;
  bool     has_prev       = false;

  uint64_t mem_size_dist[7] = {0};
  uint64_t pa_valid_count  = 0;
  uint64_t pa_io_count     = 0;
  uint64_t isz_not4_count  = 0;   /* aarch64 sanity: non-A64-width insns */

  while (1) {
    if (max_insns > 0 && total_insns >= max_insns) {
      break;
    }

    uint32_t header;
    if (!reader_read_exact(r, &header, 4)) {
      break;
    }

    uint8_t vid  = hdr_vcpu_id(header);
    uint8_t priv = hdr_privilege(header);
    uint8_t isz  = hdr_instr_size(header);
    uint8_t nmem = hdr_num_mem_ops(header);

    if (isz == 0 || isz > MAX_INSN_SIZE) {
      fprintf(stderr,
              "ERROR at insn #%" PRIu64 ": invalid instr_size=%u\n",
              total_insns,
              isz);
      break;
    }
    if (nmem > MAX_MEM_OPS) {
      fprintf(stderr,
              "ERROR at insn #%" PRIu64 ": invalid num_mem_ops=%u\n",
              total_insns,
              nmem);
      break;
    }

    if (arch == 1 && isz != 4) {
      isz_not4_count++;
    }

    uint64_t ip;
    if (!reader_read_exact(r, &ip, 8)) {
      fprintf(stderr,
              "ERROR: truncated record (IP) at insn #%" PRIu64 "\n",
              total_insns);
      break;
    }

    uint8_t insn_bytes[MAX_INSN_SIZE];
    if (!reader_read_exact(r, insn_bytes, isz)) {
      fprintf(stderr,
              "ERROR: truncated record (bytes) at insn #%" PRIu64 "\n",
              total_insns);
      break;
    }

    /* Detect taken branch by IP discontinuity */
    if (has_prev && ip != prev_ip + prev_size) {
      branch_count++;
    }

    if (verbose) {
      printf("[%8" PRIu64 "] %s IP=0x%016" PRIx64 " sz=%2u mem=%u  ",
             total_insns,
             priv ? "K" : "U",
             ip,
             isz,
             nmem);
      for (int j = 0; j < isz; j++) {
        printf("%02x", insn_bytes[j]);
      }
    }

    /* Read memory operations */
    for (int m = 0; m < nmem; m++) {
      uint64_t maddr;
      uint64_t mpaddr = 0;
      uint8_t  msize;
      uint8_t  mflags;

      if (!reader_read_exact(r, &maddr, 8)) goto trunc_memop;
      if (file_has_pa && !reader_read_exact(r, &mpaddr, 8)) goto trunc_memop;
      if (!reader_read_exact(r, &msize, 1) ||
          !reader_read_exact(r, &mflags, 1)) {
      trunc_memop:
        fprintf(stderr,
                "ERROR: truncated mem op at insn #%" PRIu64 "\n",
                total_insns);
        goto done;
      }

      bool is_write  = (mflags & 0x1) != 0;
      bool has_value = (mflags & 0x2) != 0;
      bool pa_valid  = (mflags & 0x4) != 0;
      bool pa_is_io  = (mflags & 0x8) != 0;

      if (has_value && !file_has_vals) {
        fprintf(stderr,
                "ERROR: corrupt file — mem op sets has_value but header "
                "has_values=0 (insn #%" PRIu64 ")\n",
                total_insns);
        goto done;
      }
      if (pa_valid) pa_valid_count++;
      if (pa_is_io) pa_io_count++;

      /* Size distribution */
      int     size_idx = 0;
      uint8_t s        = msize;
      while (s > 1 && size_idx < 6) {
        s >>= 1;
        size_idx++;
      }
      mem_size_dist[size_idx]++;

      if (verbose) {
        printf("  %s[%uB]@0x%" PRIx64, is_write ? "W" : "R", msize, maddr);
        if (file_has_pa) {
          printf(" PA=0x%" PRIx64 "%s%s",
                 mpaddr,
                 pa_valid ? "" : "[invalid]",
                 pa_is_io ? "[io]" : "");
        }
      }

      if (has_value) {
        uint8_t vbytes = (msize <= MAX_VALUE_SIZE) ? msize : MAX_VALUE_SIZE;
        uint8_t valbuf[MAX_VALUE_SIZE];
        if (!reader_read_exact(r, valbuf, vbytes)) {
          fprintf(stderr,
                  "ERROR: truncated value at insn #%" PRIu64 "\n",
                  total_insns);
          goto done;
        }

        if (verbose) {
          printf("=0x");
          for (int b = vbytes - 1; b >= 0; b--) {
            printf("%02x", valbuf[b]);
          }
        }

        values_present++;
      }

      total_mem_ops++;
      if (is_write) {
        mem_writes++;
      } else {
        mem_reads++;
      }
    }

    if (verbose) {
      printf("\n");
    }

    total_insns++;
    if (priv) {
      kernel_insns++;
    } else {
      user_insns++;
    }

    prev_ip   = ip;
    prev_size = isz;
    has_prev  = true;
  }

done:
  reader_close(r);

  printf("\n=== Summary ===\n");
  printf("Total instructions:     %" PRIu64 "\n", total_insns);
  printf("  User mode:            %" PRIu64 " (%.1f%%)\n",
         user_insns,
         total_insns ? 100.0 * user_insns / total_insns : 0);
  printf("  Kernel mode:          %" PRIu64 " (%.1f%%)\n",
         kernel_insns,
         total_insns ? 100.0 * kernel_insns / total_insns : 0);
  printf("Total memory ops:       %" PRIu64 "\n", total_mem_ops);
  printf("  Reads:                %" PRIu64 "\n", mem_reads);
  printf("  Writes:               %" PRIu64 "\n", mem_writes);
  printf("  With values captured: %" PRIu64 " (%.1f%%)\n",
         values_present,
         total_mem_ops ? 100.0 * values_present / total_mem_ops : 0);
  printf("Taken branches (est):   %" PRIu64 " (%.1f%%)\n",
         branch_count,
         total_insns ? 100.0 * branch_count / total_insns : 0);
  printf("Avg mem ops/insn:       %.2f\n",
         total_insns ? (double)total_mem_ops / total_insns : 0);

  if (file_has_pa) {
    printf("PA valid:               %" PRIu64 " (%.1f%%)\n",
           pa_valid_count,
           total_mem_ops ? 100.0 * pa_valid_count / total_mem_ops : 0);
    printf("PA is MMIO:             %" PRIu64 "\n", pa_io_count);
  }
  if (arch == 1) {
    printf("sanity: instr_size!=4 in %" PRIu64 "/%" PRIu64
           " records (%.2f%%)\n",
           isz_not4_count,
           total_insns,
           total_insns ? 100.0 * isz_not4_count / total_insns : 0);
  }

  printf("\nMemory access size distribution:\n");
  const char *size_labels[] = {"1B", "2B", "4B", "8B", "16B", "32B", "64B"};
  for (int i = 0; i < 7; i++) {
    if (mem_size_dist[i] > 0) {
      printf("  %3s: %10" PRIu64 " (%5.1f%%)\n",
             size_labels[i],
             mem_size_dist[i],
             total_mem_ops ? 100.0 * mem_size_dist[i] / total_mem_ops : 0);
    }
  }

  return 0;
}
