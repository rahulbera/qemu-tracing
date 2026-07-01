/*
 * trace_filter.c — Remove TCG idle-loop kernel instructions from raw traces
 *
 * Reads a v2 raw trace (.raw or .raw.zst) and writes a filtered .raw.zst
 * with idle-loop sequences removed. An idle-loop iteration is defined as a
 * sequence of kernel-mode instructions bounded by two HLT instructions
 * with no user-mode instruction in between.
 *
 * Build:
 *   gcc -O2 -o trace_filter trace_filter.c $(pkg-config --libs --cflags libzstd)
 *
 * Usage:
 *   ./trace_filter input.raw.zst output.raw.zst
 *   ./trace_filter --stats-only input.raw.zst
 */

#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zstd.h>

#define TRACE_FORMAT_MAGIC 0x46545343
#define MAX_INSN_SIZE      15
#define MAX_MEM_OPS        7
#define MAX_VALUE_SIZE     64

/* Dev: level 1 (fast). Bump to 19 for production filtering. */
#define ZSTD_LEVEL 1

#define HEARTBEAT_INTERVAL 10000000ULL

#define IDLE_BUFFER_WARN_THRESHOLD 1000000ULL

/* ================================================================
 * Reader: zstd-aware buffered reader (mirrors trace_inspector.c)
 * ================================================================ */

#define READER_COMP_BUF_SIZE   (4 * 1024 * 1024)
#define READER_DECOMP_BUF_SIZE (8 * 1024 * 1024)

typedef struct {
  FILE      *fp;
  bool       is_zstd;
  ZSTD_DCtx *dctx;

  uint8_t *comp_buf;
  size_t   comp_buf_size;
  size_t   comp_buf_pos;
  size_t   comp_buf_filled;
  bool     eof_reached;

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
    r->comp_buf_size   = READER_COMP_BUF_SIZE;
    r->comp_buf        = malloc(r->comp_buf_size);
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
  }
  return r;
}

static bool reader_refill_zstd(TraceReader *r)
{
  if (r->stream_done)
    return false;

  r->decomp_buf_pos    = 0;
  r->decomp_buf_filled = 0;

  while (r->decomp_buf_filled == 0) {
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

    ZSTD_inBuffer  input  = {.src  = r->comp_buf,
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
  if (!r->is_zstd) {
    return fread(buf, 1, len, r->fp);
  }
  uint8_t *dst       = (uint8_t *)buf;
  size_t   remaining = len;
  while (remaining > 0) {
    size_t avail = r->decomp_buf_filled - r->decomp_buf_pos;
    if (avail == 0) {
      if (!reader_refill_zstd(r))
        break;
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
  if (!r)
    return;
  if (r->dctx)
    ZSTD_freeDCtx(r->dctx);
  free(r->comp_buf);
  free(r->decomp_buf);
  if (r->fp)
    fclose(r->fp);
  free(r);
}

/* ================================================================
 * Writer: zstd-compressed output (mirrors champsim_tracer.c)
 * ================================================================ */

#define WRITER_INBUF_SIZE  (4 * 1024 * 1024)
#define WRITER_OUTBUF_SIZE (4 * 1024 * 1024)

typedef struct {
  FILE      *fp;
  ZSTD_CCtx *cctx;
  uint8_t   *inbuf;
  size_t     inbuf_size;
  size_t     inbuf_pos;
  uint8_t   *outbuf;
  size_t     outbuf_size;
} TraceWriter;

static TraceWriter *writer_open(const char *filename, int level)
{
  TraceWriter *w = calloc(1, sizeof(TraceWriter));
  if (!w)
    return NULL;
  w->fp = fopen(filename, "wb");
  if (!w->fp) {
    free(w);
    return NULL;
  }
  w->cctx = ZSTD_createCCtx();
  if (!w->cctx) {
    fclose(w->fp);
    free(w);
    return NULL;
  }
  ZSTD_CCtx_setParameter(w->cctx, ZSTD_c_compressionLevel, level);

  w->inbuf_size  = WRITER_INBUF_SIZE;
  w->inbuf       = malloc(w->inbuf_size);
  w->outbuf_size = WRITER_OUTBUF_SIZE;
  w->outbuf      = malloc(w->outbuf_size);
  if (!w->inbuf || !w->outbuf) {
    ZSTD_freeCCtx(w->cctx);
    free(w->inbuf);
    free(w->outbuf);
    fclose(w->fp);
    free(w);
    return NULL;
  }
  return w;
}

static void writer_flush(TraceWriter *w)
{
  if (w->inbuf_pos == 0)
    return;
  ZSTD_inBuffer input = {.src = w->inbuf, .size = w->inbuf_pos, .pos = 0};
  while (input.pos < input.size) {
    ZSTD_outBuffer output = {
        .dst = w->outbuf, .size = w->outbuf_size, .pos = 0};
    size_t ret =
        ZSTD_compressStream2(w->cctx, &output, &input, ZSTD_e_continue);
    if (ZSTD_isError(ret)) {
      fprintf(stderr, "ZSTD error: %s\n", ZSTD_getErrorName(ret));
      break;
    }
    if (output.pos > 0) {
      fwrite(w->outbuf, 1, output.pos, w->fp);
    }
  }
  w->inbuf_pos = 0;
}

static void writer_finish(TraceWriter *w)
{
  if (!w || !w->fp)
    return;
  writer_flush(w);
  ZSTD_inBuffer input = {.src = NULL, .size = 0, .pos = 0};
  size_t        ret;
  do {
    ZSTD_outBuffer output = {
        .dst = w->outbuf, .size = w->outbuf_size, .pos = 0};
    ret = ZSTD_compressStream2(w->cctx, &output, &input, ZSTD_e_end);
    if (ZSTD_isError(ret)) {
      fprintf(stderr, "ZSTD finalize error: %s\n", ZSTD_getErrorName(ret));
      break;
    }
    if (output.pos > 0) {
      fwrite(w->outbuf, 1, output.pos, w->fp);
    }
  } while (ret > 0);
}

static void writer_close(TraceWriter *w)
{
  if (!w)
    return;
  if (w->cctx)
    ZSTD_freeCCtx(w->cctx);
  free(w->inbuf);
  free(w->outbuf);
  if (w->fp)
    fclose(w->fp);
  free(w);
}

static inline void writer_append(TraceWriter *w, const void *data, size_t len)
{
  if (w->inbuf_pos + len > w->inbuf_size) {
    writer_flush(w);
    if (len > w->inbuf_size) {
      /* Single chunk larger than buffer — write straight through */
      ZSTD_inBuffer input = {.src = data, .size = len, .pos = 0};
      while (input.pos < input.size) {
        ZSTD_outBuffer output = {
            .dst = w->outbuf, .size = w->outbuf_size, .pos = 0};
        size_t ret =
            ZSTD_compressStream2(w->cctx, &output, &input, ZSTD_e_continue);
        if (ZSTD_isError(ret)) {
          fprintf(stderr, "ZSTD error: %s\n", ZSTD_getErrorName(ret));
          return;
        }
        if (output.pos > 0)
          fwrite(w->outbuf, 1, output.pos, w->fp);
      }
      return;
    }
  }
  memcpy(w->inbuf + w->inbuf_pos, data, len);
  w->inbuf_pos += len;
}

/* ================================================================
 * Helpers
 * ================================================================ */

static int cmp_u32(const void *a, const void *b)
{
  uint32_t x = *(const uint32_t *)a;
  uint32_t y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

/* ================================================================
 * Header field decoders
 * ================================================================ */

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
 * Idle buffer: stores raw serialized record bytes for buffered
 * IDLE_CANDIDATE instructions. Variable-length records, so we keep
 * a flat byte buffer plus an array of (offset, length) per record.
 * ================================================================ */

typedef struct {
  uint8_t *bytes;
  size_t   bytes_size;
  size_t   bytes_pos;

  size_t  *offsets;
  size_t  *lengths;
  size_t   slots_size;
  size_t   slots_used;

  uint64_t kernel_count;
} IdleBuffer;

static void idle_buffer_init(IdleBuffer *b)
{
  b->bytes_size = 64 * 1024;
  b->bytes      = malloc(b->bytes_size);
  b->bytes_pos  = 0;
  b->slots_size = 1024;
  b->offsets    = malloc(b->slots_size * sizeof(size_t));
  b->lengths    = malloc(b->slots_size * sizeof(size_t));
  b->slots_used = 0;
  b->kernel_count = 0;
}

static void idle_buffer_free(IdleBuffer *b)
{
  free(b->bytes);
  free(b->offsets);
  free(b->lengths);
}

static void idle_buffer_reset(IdleBuffer *b)
{
  b->bytes_pos   = 0;
  b->slots_used  = 0;
  b->kernel_count = 0;
}

static void idle_buffer_append(IdleBuffer *b, const uint8_t *data, size_t len,
                               bool is_kernel)
{
  if (b->bytes_pos + len > b->bytes_size) {
    while (b->bytes_pos + len > b->bytes_size)
      b->bytes_size *= 2;
    b->bytes = realloc(b->bytes, b->bytes_size);
  }
  if (b->slots_used + 1 > b->slots_size) {
    b->slots_size *= 2;
    b->offsets = realloc(b->offsets, b->slots_size * sizeof(size_t));
    b->lengths = realloc(b->lengths, b->slots_size * sizeof(size_t));
  }
  memcpy(b->bytes + b->bytes_pos, data, len);
  b->offsets[b->slots_used] = b->bytes_pos;
  b->lengths[b->slots_used] = len;
  b->slots_used++;
  b->bytes_pos += len;
  if (is_kernel)
    b->kernel_count++;
}

/* ================================================================
 * Main filter
 * ================================================================ */

static void print_usage(const char *prog)
{
  fprintf(stderr,
          "Usage:\n"
          "  %s <input.raw.zst> <output.raw.zst>\n"
          "  %s --stats-only <input.raw.zst>\n",
          prog,
          prog);
}

int main(int argc, char **argv)
{
  bool stats_only = false;

  static struct option long_opts[] = {
      {"stats-only", no_argument, NULL, 's'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "sh", long_opts, NULL)) != -1) {
    switch (opt) {
    case 's':
      stats_only = true;
      break;
    case 'h':
    default:
      print_usage(argv[0]);
      return (opt == 'h') ? 0 : 1;
    }
  }

  const char *input_path  = NULL;
  const char *output_path = NULL;

  if (stats_only) {
    if (optind != argc - 1) {
      print_usage(argv[0]);
      return 1;
    }
    input_path = argv[optind];
  } else {
    if (optind != argc - 2) {
      print_usage(argv[0]);
      return 1;
    }
    input_path  = argv[optind];
    output_path = argv[optind + 1];
  }

  TraceReader *r = reader_open(input_path);
  if (!r) {
    perror(input_path);
    return 1;
  }

  TraceWriter *w = NULL;
  if (!stats_only) {
    w = writer_open(output_path, ZSTD_LEVEL);
    if (!w) {
      perror(output_path);
      reader_close(r);
      return 1;
    }
  }

  /* File header: 16 bytes */
  uint8_t file_hdr[16];
  if (!reader_read_exact(r, file_hdr, 16)) {
    fprintf(stderr, "ERROR: cannot read file header\n");
    reader_close(r);
    if (w)
      writer_close(w);
    return 1;
  }
  uint32_t magic;
  memcpy(&magic, file_hdr, 4);
  if (magic != TRACE_FORMAT_MAGIC) {
    fprintf(stderr,
            "ERROR: bad magic 0x%08X (expected 0x%08X)\n",
            magic,
            TRACE_FORMAT_MAGIC);
    reader_close(r);
    if (w)
      writer_close(w);
    return 1;
  }
  uint32_t version, vcpu_id;
  memcpy(&version, file_hdr + 4, 4);
  memcpy(&vcpu_id, file_hdr + 8, 4);

  fprintf(stderr,
          "[trace_filter] input=%s version=%u vcpu=%u%s\n",
          input_path,
          version,
          vcpu_id,
          stats_only ? " [stats-only]" : "");
  if (w) {
    fprintf(stderr, "[trace_filter] output=%s zstd_level=%d\n", output_path,
            ZSTD_LEVEL);
    writer_append(w, file_hdr, 16);
  }

  /* Filter state */
  enum { ACTIVE, IDLE_CANDIDATE } state = ACTIVE;
  IdleBuffer idle;
  idle_buffer_init(&idle);
  bool warned_oversize = false;

  /* Stats */
  uint64_t in_total      = 0;
  uint64_t in_user       = 0;
  uint64_t in_kernel     = 0;
  uint64_t out_total     = 0;
  uint64_t out_user      = 0;
  uint64_t out_kernel    = 0;
  uint64_t idle_discarded = 0;
  uint64_t idle_iters    = 0;
  uint64_t real_wakeups  = 0;
  uint64_t hlt_seen      = 0;

  uint64_t idle_len_min = UINT64_MAX;
  uint64_t idle_len_max = 0;
  uint64_t idle_len_sum = 0;

  /* Reservoir-ish: collect samples for median. Cap to 1M to bound memory. */
  size_t   sample_cap = 1 << 20;
  uint32_t *idle_samples = malloc(sample_cap * sizeof(uint32_t));
  size_t   sample_used = 0;

  /* Scratch buffer for one record's serialized bytes */
  uint8_t  rec_buf[4 + 8 + MAX_INSN_SIZE +
                  MAX_MEM_OPS * (8 + 1 + 1 + MAX_VALUE_SIZE)];

  struct timespec t_start;
  clock_gettime(CLOCK_MONOTONIC, &t_start);
  uint64_t next_heartbeat = HEARTBEAT_INTERVAL;

  while (1) {
    size_t rec_pos = 0;

    uint32_t header;
    if (!reader_read_exact(r, &header, 4)) {
      break;
    }
    memcpy(rec_buf + rec_pos, &header, 4);
    rec_pos += 4;

    uint8_t priv = hdr_privilege(header);
    uint8_t isz  = hdr_instr_size(header);
    uint8_t nmem = hdr_num_mem_ops(header);

    if (isz == 0 || isz > MAX_INSN_SIZE) {
      fprintf(stderr,
              "ERROR at insn #%" PRIu64 ": invalid instr_size=%u\n",
              in_total,
              isz);
      break;
    }
    if (nmem > MAX_MEM_OPS) {
      fprintf(stderr,
              "ERROR at insn #%" PRIu64 ": invalid num_mem_ops=%u\n",
              in_total,
              nmem);
      break;
    }

    uint64_t ip;
    if (!reader_read_exact(r, &ip, 8)) {
      fprintf(stderr,
              "ERROR: truncated record (IP) at insn #%" PRIu64 "\n",
              in_total);
      break;
    }
    memcpy(rec_buf + rec_pos, &ip, 8);
    rec_pos += 8;

    uint8_t insn_bytes[MAX_INSN_SIZE];
    if (!reader_read_exact(r, insn_bytes, isz)) {
      fprintf(stderr,
              "ERROR: truncated record (bytes) at insn #%" PRIu64 "\n",
              in_total);
      break;
    }
    memcpy(rec_buf + rec_pos, insn_bytes, isz);
    rec_pos += isz;

    /* Memory ops */
    bool truncated = false;
    for (int m = 0; m < nmem; m++) {
      uint8_t mhdr[10];
      if (!reader_read_exact(r, mhdr, 10)) {
        truncated = true;
        break;
      }
      uint8_t msize  = mhdr[8];
      uint8_t mflags = mhdr[9];
      memcpy(rec_buf + rec_pos, mhdr, 10);
      rec_pos += 10;

      if (mflags & 0x2) {
        uint8_t vbytes = (msize <= MAX_VALUE_SIZE) ? msize : MAX_VALUE_SIZE;
        uint8_t valbuf[MAX_VALUE_SIZE];
        if (!reader_read_exact(r, valbuf, vbytes)) {
          truncated = true;
          break;
        }
        memcpy(rec_buf + rec_pos, valbuf, vbytes);
        rec_pos += vbytes;
      }
    }
    if (truncated) {
      fprintf(stderr,
              "ERROR: truncated mem op at insn #%" PRIu64 "\n",
              in_total);
      break;
    }

    in_total++;
    if (priv)
      in_kernel++;
    else
      in_user++;

    /* HLT detection (loose: size==1 && bytes[0]==0xF4) */
    bool is_hlt = (isz == 1 && insn_bytes[0] == 0xF4);
    if (is_hlt)
      hlt_seen++;

    /* State machine */
    if (state == ACTIVE) {
      if (is_hlt) {
        state = IDLE_CANDIDATE;
        idle_buffer_reset(&idle);
        idle_buffer_append(&idle, rec_buf, rec_pos, priv != 0);
      } else {
        if (w)
          writer_append(w, rec_buf, rec_pos);
        out_total++;
        if (priv)
          out_kernel++;
        else
          out_user++;
      }
    } else { /* IDLE_CANDIDATE */
      if (is_hlt) {
        /* HLT → HLT with no user-mode in between: discard buffered run */
        uint64_t discarded_now = idle.kernel_count;
        idle_discarded += discarded_now;
        idle_iters++;
        idle_len_sum += discarded_now;
        if (discarded_now < idle_len_min)
          idle_len_min = discarded_now;
        if (discarded_now > idle_len_max)
          idle_len_max = discarded_now;
        if (sample_used < sample_cap) {
          idle_samples[sample_used++] = (uint32_t)(
              discarded_now > UINT32_MAX ? UINT32_MAX : discarded_now);
        }

        /* Start a new candidate with this HLT */
        idle_buffer_reset(&idle);
        idle_buffer_append(&idle, rec_buf, rec_pos, priv != 0);
      } else if (priv == 0) {
        /* Genuine wake-up: flush buffer + this user-mode insn */
        for (size_t i = 0; i < idle.slots_used; i++) {
          if (w)
            writer_append(w,
                          idle.bytes + idle.offsets[i],
                          idle.lengths[i]);
          out_total++;
          /* Buffered records' privilege would need re-decoding for
             accurate counts; simpler: assume kernel since IDLE_CANDIDATE
             only buffers kernel insns (HLT itself is kernel). */
          out_kernel++;
        }
        real_wakeups++;
        idle_buffer_reset(&idle);
        if (w)
          writer_append(w, rec_buf, rec_pos);
        out_total++;
        out_user++;
        state = ACTIVE;
      } else {
        /* Kernel insn after HLT — keep buffering */
        idle_buffer_append(&idle, rec_buf, rec_pos, true);
        if (!warned_oversize && idle.slots_used >= IDLE_BUFFER_WARN_THRESHOLD) {
          fprintf(stderr,
                  "[trace_filter] WARNING: idle candidate buffer exceeded "
                  "%llu instructions at input insn #%" PRIu64
                  " — possible long kernel-only sequence\n",
                  (unsigned long long)IDLE_BUFFER_WARN_THRESHOLD,
                  in_total);
          warned_oversize = true;
        }
      }
    }

    /* Heartbeat */
    if (in_total >= next_heartbeat) {
      struct timespec t_now;
      clock_gettime(CLOCK_MONOTONIC, &t_now);
      double elapsed =
          (t_now.tv_sec - t_start.tv_sec) +
          (t_now.tv_nsec - t_start.tv_nsec) / 1e9;
      double mips = elapsed > 0 ? (in_total / 1e6) / elapsed : 0.0;
      fprintf(
          stderr,
          "[trace_filter] in=%" PRIu64 " (U=%" PRIu64 "/%.1f%%, K=%" PRIu64
          "/%.1f%%) | out=%" PRIu64 " | filtered=%" PRIu64
          " (%.1f%%) | iters=%" PRIu64 " | %.1fs %.1f Minsn/s\n",
          in_total,
          in_user,
          in_total ? 100.0 * in_user / in_total : 0,
          in_kernel,
          in_total ? 100.0 * in_kernel / in_total : 0,
          out_total,
          idle_discarded,
          in_kernel ? 100.0 * idle_discarded / in_kernel : 0,
          idle_iters,
          elapsed,
          mips);
      next_heartbeat += HEARTBEAT_INTERVAL;
    }
  }

  /* End-of-trace: flush any remaining buffer (conservative-keep) */
  if (idle.slots_used > 0) {
    for (size_t i = 0; i < idle.slots_used; i++) {
      if (w)
        writer_append(w, idle.bytes + idle.offsets[i], idle.lengths[i]);
      out_total++;
      out_kernel++;
    }
    fprintf(stderr,
            "[trace_filter] EOF: flushed %zu buffered kernel insns "
            "(no terminating HLT)\n",
            idle.slots_used);
  }

  if (w) {
    writer_finish(w);
    writer_close(w);
  }
  reader_close(r);

  /* Compute median idle-loop length */
  uint32_t median = 0;
  if (sample_used > 0) {
    qsort(idle_samples, sample_used, sizeof(uint32_t), cmp_u32);
    median = idle_samples[sample_used / 2];
  }
  free(idle_samples);

  struct timespec t_end;
  clock_gettime(CLOCK_MONOTONIC, &t_end);
  double total_elapsed =
      (t_end.tv_sec - t_start.tv_sec) +
      (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

  fprintf(stderr, "\n=== Idle Loop Filter Report ===\n");
  fprintf(stderr,
          "Input instructions:        %" PRIu64 "\n  user:                    %" PRIu64
          " (%.1f%%)\n  kernel:                  %" PRIu64 " (%.1f%%)\n",
          in_total,
          in_user,
          in_total ? 100.0 * in_user / in_total : 0,
          in_kernel,
          in_total ? 100.0 * in_kernel / in_total : 0);
  fprintf(stderr,
          "Output instructions:       %" PRIu64 "\n  user:                    %" PRIu64
          " (%.1f%% of output)\n  kernel:                  %" PRIu64
          " (%.1f%% of output)\n",
          out_total,
          out_user,
          out_total ? 100.0 * out_user / out_total : 0,
          out_kernel,
          out_total ? 100.0 * out_kernel / out_total : 0);
  fprintf(stderr,
          "Idle insns removed:        %" PRIu64 " (%.1f%% of input, %.1f%% of input kernel)\n",
          idle_discarded,
          in_total ? 100.0 * idle_discarded / in_total : 0,
          in_kernel ? 100.0 * idle_discarded / in_kernel : 0);
  fprintf(stderr,
          "HLT instructions seen:     %" PRIu64 "\n",
          hlt_seen);
  fprintf(stderr,
          "Idle iterations:           %" PRIu64 "\n",
          idle_iters);
  fprintf(stderr,
          "Real wake-ups (HLT→user):  %" PRIu64 "\n",
          real_wakeups);
  if (idle_iters > 0) {
    fprintf(stderr,
            "Idle iter length: avg=%.1f min=%" PRIu64 " max=%" PRIu64
            " median=%u\n",
            (double)idle_len_sum / idle_iters,
            idle_len_min,
            idle_len_max,
            median);
  }
  fprintf(stderr,
          "Elapsed: %.2fs (%.2f Minsn/s)\n",
          total_elapsed,
          total_elapsed > 0 ? (in_total / 1e6) / total_elapsed : 0);

  idle_buffer_free(&idle);
  return 0;
}
