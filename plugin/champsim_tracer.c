/*
 * champsim_tracer.c — QEMU 9.2 TCG Plugin for ChampSim Trace Generation
 *
 * Captures raw instruction traces with memory values during TCG execution.
 * Output is zstd-compressed per-vCPU trace files.
 * An offline converter transforms the raw trace into ChampSim format.
 *
 * Requires: QEMU >= 9.1, libzstd
 *
 * Build:
 *   gcc -O2 -shared -fPIC \
 *       -I<qemu-src>/include \
 *       $(pkg-config --cflags glib-2.0) \
 *       -o champsim_tracer.so champsim_tracer.c \
 *       $(pkg-config --libs glib-2.0) -lzstd
 *
 * Usage:
 *   # Immediate tracing (starts on first instruction):
 *   qemu-system-x86_64 -accel tcg \
 *       -plugin ./champsim_tracer.so,outdir=/path,vcpus=0-3,limit=200000000 \
 *       ...
 *
 *   # Deferred tracing (waits for trigger file on host):
 *   qemu-system-x86_64 -accel tcg \
 *       -plugin ./champsim_tracer.so,outdir=/path,vcpus=0-3,limit=200000000,trigger=/tmp/trace_start \
 *       ...
 *   # Then on the host, when ready: touch /tmp/trace_start
 *
 * Output: <outdir>/trace_vcpu<N>.raw.zst (one per traced vCPU)
 *
 * ======================================================================
 * RAW TRACE FORMAT (inside the zstd stream)
 * ======================================================================
 *
 * File header (16 bytes, written once at start):
 *     magic:    4 bytes  "CSTF" (0x46545343 little-endian)
 *     version:  4 bytes  (2)
 *     vcpu_id:  4 bytes
 *     reserved: 4 bytes  (0)
 *
 * Per instruction record (variable length):
 *
 *   [header: 4 bytes, uint32_t]
 *     bits [3:0]   = vcpu_id       (0-15)
 *     bits [4]     = privilege      (0=user, 1=kernel)
 *     bits [8:5]   = instr_size    (1-15 bytes)
 *     bits [11:9]  = num_mem_ops   (0-7)
 *     bits [31:12] = reserved
 *
 *   [instruction pointer: 8 bytes, uint64_t]
 *
 *   [raw instruction bytes: <instr_size> bytes]
 *
 *   [memory ops: repeated <num_mem_ops> times]
 *     address:  8 bytes  (uint64_t, guest virtual address)
 *     size:     1 byte   (access width: 1,2,4,8,16,32,64)
 *     flags:    1 byte   (bit 0: 0=read 1=write, bit 1: has_value)
 *     value:    <size> bytes (only present if has_value is set)
 *               For accesses > 16 bytes, has_value is 0 (API limit).
 *               Values are in little-endian byte order.
 *
 * ======================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/stat.h>
#include <glib.h>
#include <zstd.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* ================================================================
 * Constants
 * ================================================================ */

#define PLUGIN_NAME "champsim_tracer"
#define TRACE_FORMAT_MAGIC 0x46545343 /* "CSTF" little-endian */
#define TRACE_FORMAT_VER 2            /* v2: memory values */

#define MAX_VCPUS 16
#define MAX_INSN_SIZE 15
#define MAX_MEM_OPS 7
#define MAX_VALUE_SIZE 16                 /* 128-bit max from API */
#define INPUT_BUF_SIZE (16 * 1024 * 1024) /* 16 MB uncompressed buffer */
#define FLUSH_THRESHOLD (INPUT_BUF_SIZE - 8192)
#define STAGING_BUF_SIZE 512

#define ZSTD_LEVEL 1 /* fast compression */

/* How often to check the trigger file (in instructions across all vCPUs).
 * 10M instructions at ~10 MIPS under TCG ≈ check once per second.
 * The check is a single access() syscall — negligible overhead. */
#define TRIGGER_CHECK_INTERVAL 10000000

/* x86-64 Linux kernel/user boundary */
#define KERNEL_ADDR_THRESH 0xFFFF800000000000ULL

/* ================================================================
 * Header encoding
 * ================================================================ */

static inline uint32_t encode_header(uint8_t vcpu_id, uint8_t privilege,
                                     uint8_t instr_size, uint8_t num_mem_ops)
{
    return ((uint32_t)(vcpu_id & 0xF)) | ((uint32_t)(privilege & 0x1) << 4) | ((uint32_t)(instr_size & 0xF) << 5) | ((uint32_t)(num_mem_ops & 0x7) << 9);
}

/* ================================================================
 * InsnMeta: translation-time metadata passed to exec callback
 * ================================================================ */

typedef struct
{
    uint64_t vaddr;
    uint8_t size;
    uint8_t bytes[MAX_INSN_SIZE];
} InsnMeta;

/* ================================================================
 * MemOpRecord: per memory operand, built at runtime
 * ================================================================ */

typedef struct
{
    uint64_t address;
    uint8_t size;
    uint8_t flags; /* bit 0: write, bit 1: has_value */
    uint8_t value[MAX_VALUE_SIZE];
} MemOpRecord;

/* ================================================================
 * Per-vCPU state
 * ================================================================ */

typedef struct
{
    /* Output file */
    FILE *outfile;

    /* Uncompressed input buffer (instructions are appended here) */
    uint8_t *inbuf;
    size_t inbuf_pos;

    /* Compressed output buffer (flushed to disk) */
    uint8_t *outbuf;
    size_t outbuf_size;

    /* zstd streaming compressor */
    ZSTD_CCtx *cctx;

    /* Identity */
    uint8_t vcpu_id;

    /* Pending instruction being assembled */
    bool has_pending;
    uint64_t pending_ip;
    uint8_t pending_insn_size;
    uint8_t pending_insn_bytes[MAX_INSN_SIZE];
    uint8_t pending_privilege;

    MemOpRecord pending_mem_ops[MAX_MEM_OPS];
    uint8_t pending_num_mem_ops;

    /* Statistics */
    uint64_t insn_count;
    uint64_t mem_op_count;
    uint64_t values_captured;
    uint64_t bytes_uncompressed;
    uint64_t bytes_compressed;
    bool active;
    bool limit_reached;
} VcpuState;

/* ================================================================
 * Global state
 * ================================================================ */

static VcpuState vcpu_state[MAX_VCPUS];
static bool trace_vcpu[MAX_VCPUS];
static uint64_t insn_limit = 0;
static char output_dir[4096] = ".";
static qemu_plugin_id_t plugin_id;

/* Trigger mechanism */
static bool tracing_enabled = true;        /* true = immediate start */
static bool use_trigger = false;           /* true = wait for trigger */
static char trigger_file[4096] = "";       /* host-side file path */
static uint64_t trigger_check_counter = 0; /* global instruction counter */
static uint64_t trigger_skipped_insns = 0; /* instructions skipped before trigger */

/* ================================================================
 * Trigger file check
 *
 * Called periodically from insn_exec_cb when tracing is dormant.
 * Checks if the trigger file exists on the host filesystem.
 * When found, enables tracing and deletes the file (so it doesn't
 * re-trigger on subsequent runs).
 * ================================================================ */

static void check_trigger(void)
{
    if (access(trigger_file, F_OK) == 0)
    {
        tracing_enabled = true;

        /* Remove the trigger file to avoid re-triggering */
        unlink(trigger_file);

        fprintf(stderr, "\n[%s] >>> TRIGGER DETECTED: %s <<<\n",
                PLUGIN_NAME, trigger_file);
        fprintf(stderr, "[%s] Tracing is now ENABLED "
                        "(skipped %" PRIu64 " instructions during dormant phase)\n",
                PLUGIN_NAME, trigger_skipped_insns);
    }
}

/* ================================================================
 * Compressed buffer flush
 * ================================================================ */

static void flush_buffer(VcpuState *vs)
{
    if (vs->inbuf_pos == 0 || !vs->outfile)
    {
        return;
    }

    ZSTD_inBuffer input = {
        .src = vs->inbuf,
        .size = vs->inbuf_pos,
        .pos = 0};

    while (input.pos < input.size)
    {
        ZSTD_outBuffer output = {
            .dst = vs->outbuf,
            .size = vs->outbuf_size,
            .pos = 0};

        size_t ret = ZSTD_compressStream2(vs->cctx, &output, &input,
                                          ZSTD_e_continue);
        if (ZSTD_isError(ret))
        {
            fprintf(stderr, "[%s] ZSTD error: %s\n",
                    PLUGIN_NAME, ZSTD_getErrorName(ret));
            break;
        }

        if (output.pos > 0)
        {
            fwrite(vs->outbuf, 1, output.pos, vs->outfile);
            vs->bytes_compressed += output.pos;
        }
    }

    vs->bytes_uncompressed += vs->inbuf_pos;
    vs->inbuf_pos = 0;
}

static void flush_final(VcpuState *vs)
{
    if (!vs->outfile || !vs->cctx)
    {
        return;
    }

    flush_buffer(vs);

    ZSTD_inBuffer input = {.src = NULL, .size = 0, .pos = 0};
    size_t ret;
    do
    {
        ZSTD_outBuffer output = {
            .dst = vs->outbuf,
            .size = vs->outbuf_size,
            .pos = 0};

        ret = ZSTD_compressStream2(vs->cctx, &output, &input, ZSTD_e_end);
        if (ZSTD_isError(ret))
        {
            fprintf(stderr, "[%s] ZSTD finalize error: %s\n",
                    PLUGIN_NAME, ZSTD_getErrorName(ret));
            break;
        }

        if (output.pos > 0)
        {
            fwrite(vs->outbuf, 1, output.pos, vs->outfile);
            vs->bytes_compressed += output.pos;
        }
    } while (ret > 0);
}

static inline void buffer_append(VcpuState *vs, const void *data, size_t len)
{
    if (vs->inbuf_pos + len > FLUSH_THRESHOLD)
    {
        flush_buffer(vs);
    }
    memcpy(vs->inbuf + vs->inbuf_pos, data, len);
    vs->inbuf_pos += len;
}

/* ================================================================
 * Finalize a pending instruction
 * ================================================================ */

static void finalize_pending_insn(VcpuState *vs)
{
    if (!vs->has_pending)
    {
        return;
    }

    uint32_t header = encode_header(
        vs->vcpu_id,
        vs->pending_privilege,
        vs->pending_insn_size,
        vs->pending_num_mem_ops);

    uint8_t staging[STAGING_BUF_SIZE];
    size_t pos = 0;

    memcpy(staging + pos, &header, 4);
    pos += 4;

    memcpy(staging + pos, &vs->pending_ip, 8);
    pos += 8;

    memcpy(staging + pos, vs->pending_insn_bytes, vs->pending_insn_size);
    pos += vs->pending_insn_size;

    for (int i = 0; i < vs->pending_num_mem_ops; i++)
    {
        MemOpRecord *mop = &vs->pending_mem_ops[i];

        memcpy(staging + pos, &mop->address, 8);
        pos += 8;

        staging[pos++] = mop->size;
        staging[pos++] = mop->flags;

        if (mop->flags & 0x2)
        {
            uint8_t value_bytes = (mop->size <= MAX_VALUE_SIZE)
                                      ? mop->size
                                      : MAX_VALUE_SIZE;
            memcpy(staging + pos, mop->value, value_bytes);
            pos += value_bytes;
            vs->values_captured++;
        }
    }

    buffer_append(vs, staging, pos);

    vs->insn_count++;
    vs->mem_op_count += vs->pending_num_mem_ops;
    vs->has_pending = false;
}

/* ================================================================
 * Extract memory value from QEMU 9.x API
 * ================================================================ */

static inline void extract_mem_value(qemu_plugin_meminfo_t meminfo,
                                     MemOpRecord *mop)
{
    qemu_plugin_mem_value val = qemu_plugin_mem_get_value(meminfo);

    switch (val.type)
    {
    case QEMU_PLUGIN_MEM_VALUE_U8:
        mop->value[0] = val.data.u8;
        mop->flags |= 0x2;
        break;

    case QEMU_PLUGIN_MEM_VALUE_U16:
    {
        uint16_t v = val.data.u16;
        memcpy(mop->value, &v, 2);
        mop->flags |= 0x2;
        break;
    }

    case QEMU_PLUGIN_MEM_VALUE_U32:
    {
        uint32_t v = val.data.u32;
        memcpy(mop->value, &v, 4);
        mop->flags |= 0x2;
        break;
    }

    case QEMU_PLUGIN_MEM_VALUE_U64:
    {
        uint64_t v = val.data.u64;
        memcpy(mop->value, &v, 8);
        mop->flags |= 0x2;
        break;
    }

    case QEMU_PLUGIN_MEM_VALUE_U128:
    {
        uint64_t lo = val.data.u128.low;
        uint64_t hi = val.data.u128.high;
        memcpy(mop->value, &lo, 8);
        memcpy(mop->value + 8, &hi, 8);
        mop->flags |= 0x2;
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 * Plugin callbacks
 * ================================================================ */

static void insn_exec_cb(unsigned int vcpu_index, void *userdata)
{
    if (vcpu_index >= MAX_VCPUS || !trace_vcpu[vcpu_index])
    {
        return;
    }

    /*
     * Trigger check: when tracing is dormant, we just count instructions
     * and periodically check for the trigger file. No recording, no
     * buffering, no I/O — just an integer increment and an occasional
     * access() syscall. Overhead is negligible.
     */
    if (!tracing_enabled)
    {
        trigger_skipped_insns++;
        trigger_check_counter++;
        if (trigger_check_counter >= TRIGGER_CHECK_INTERVAL)
        {
            trigger_check_counter = 0;
            check_trigger();
        }
        return;
    }

    VcpuState *vs = &vcpu_state[vcpu_index];

    if (vs->limit_reached)
    {
        return;
    }

    finalize_pending_insn(vs);

    if (insn_limit > 0 && vs->insn_count >= insn_limit)
    {
        vs->limit_reached = true;
        flush_buffer(vs);
        return;
    }

    InsnMeta *meta = (InsnMeta *)userdata;

    vs->has_pending = true;
    vs->pending_ip = meta->vaddr;
    vs->pending_insn_size = meta->size;
    memcpy(vs->pending_insn_bytes, meta->bytes, meta->size);
    vs->pending_privilege = (meta->vaddr >= KERNEL_ADDR_THRESH) ? 1 : 0;
    vs->pending_num_mem_ops = 0;
}

static void mem_cb(unsigned int vcpu_index, qemu_plugin_meminfo_t meminfo,
                   uint64_t vaddr, void *userdata)
{
    if (vcpu_index >= MAX_VCPUS || !trace_vcpu[vcpu_index])
    {
        return;
    }

    /* Skip memory recording when tracing is dormant */
    if (!tracing_enabled)
    {
        return;
    }

    VcpuState *vs = &vcpu_state[vcpu_index];

    if (vs->limit_reached || !vs->has_pending)
    {
        return;
    }

    if (vs->pending_num_mem_ops >= MAX_MEM_OPS)
    {
        return;
    }

    int idx = vs->pending_num_mem_ops;
    MemOpRecord *mop = &vs->pending_mem_ops[idx];

    mop->address = vaddr;
    mop->size = 1 << qemu_plugin_mem_size_shift(meminfo);
    mop->flags = qemu_plugin_mem_is_store(meminfo) ? 0x1 : 0x0;

    if (mop->size <= MAX_VALUE_SIZE)
    {
        memset(mop->value, 0, MAX_VALUE_SIZE);
        extract_mem_value(meminfo, mop);
    }

    vs->pending_num_mem_ops++;
}

/* ================================================================
 * Translation block callback
 * ================================================================ */

static void tb_trans_cb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n_insns = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n_insns; i++)
    {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        InsnMeta *meta = g_new(InsnMeta, 1);
        meta->vaddr = qemu_plugin_insn_vaddr(insn);
        meta->size = qemu_plugin_insn_size(insn);

        if (meta->size > MAX_INSN_SIZE)
        {
            meta->size = MAX_INSN_SIZE;
        }

        qemu_plugin_insn_data(insn, meta->bytes, meta->size);

        qemu_plugin_register_vcpu_insn_exec_cb(
            insn, insn_exec_cb, QEMU_PLUGIN_CB_NO_REGS, meta);

        qemu_plugin_register_vcpu_mem_cb(
            insn, mem_cb, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW, NULL);
    }
}

/* ================================================================
 * Argument parsing
 * ================================================================ */

static void parse_vcpus(const char *str)
{
    memset(trace_vcpu, 0, sizeof(trace_vcpu));

    char *s = g_strdup(str);
    char *saveptr = NULL;
    char *token = strtok_r(s, ",", &saveptr);

    while (token)
    {
        char *dash = strchr(token, '-');
        if (dash)
        {
            *dash = '\0';
            int start = atoi(token);
            int end = atoi(dash + 1);
            for (int i = start; i <= end && i < MAX_VCPUS; i++)
            {
                if (i >= 0)
                    trace_vcpu[i] = true;
            }
        }
        else
        {
            int id = atoi(token);
            if (id >= 0 && id < MAX_VCPUS)
                trace_vcpu[id] = true;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    g_free(s);
}

static bool parse_args(int argc, char **argv)
{
    parse_vcpus("0-3");

    for (int i = 0; i < argc; i++)
    {
        char *arg = argv[i];

        if (g_str_has_prefix(arg, "outdir="))
        {
            strncpy(output_dir, arg + 7, sizeof(output_dir) - 1);
            output_dir[sizeof(output_dir) - 1] = '\0';
        }
        else if (g_str_has_prefix(arg, "vcpus="))
        {
            parse_vcpus(arg + 6);
        }
        else if (g_str_has_prefix(arg, "limit="))
        {
            insn_limit = strtoull(arg + 6, NULL, 10);
        }
        else if (g_str_has_prefix(arg, "trigger="))
        {
            strncpy(trigger_file, arg + 8, sizeof(trigger_file) - 1);
            trigger_file[sizeof(trigger_file) - 1] = '\0';
            use_trigger = true;
            tracing_enabled = false; /* start dormant */
        }
        else
        {
            fprintf(stderr, "[%s] Unknown argument: %s\n", PLUGIN_NAME, arg);
            fprintf(stderr, "Usage: -plugin %s.so"
                            "[,outdir=<path>][,vcpus=<range>][,limit=<N>]"
                            "[,trigger=<host-file>]\n",
                    PLUGIN_NAME);
            return false;
        }
    }

    return true;
}

/* ================================================================
 * Initialization and cleanup
 * ================================================================ */

static void plugin_atexit(qemu_plugin_id_t id, void *userdata)
{
    uint64_t total_insns = 0;
    uint64_t total_mem = 0;
    uint64_t total_values = 0;
    uint64_t total_raw = 0;
    uint64_t total_comp = 0;

    fprintf(stderr, "\n[%s] Finalizing traces...\n", PLUGIN_NAME);

    if (use_trigger && !tracing_enabled)
    {
        fprintf(stderr, "[%s] WARNING: Trigger was never activated! "
                        "No trace data recorded.\n",
                PLUGIN_NAME);
        fprintf(stderr, "[%s] Skipped %" PRIu64 " instructions while dormant.\n",
                PLUGIN_NAME, trigger_skipped_insns);
    }

    for (int i = 0; i < MAX_VCPUS; i++)
    {
        VcpuState *vs = &vcpu_state[i];

        if (!vs->active)
        {
            continue;
        }

        finalize_pending_insn(vs);
        flush_final(vs);

        if (vs->outfile)
        {
            fclose(vs->outfile);
            vs->outfile = NULL;
        }

        if (vs->cctx)
        {
            ZSTD_freeCCtx(vs->cctx);
            vs->cctx = NULL;
        }

        if (vs->inbuf)
        {
            g_free(vs->inbuf);
            vs->inbuf = NULL;
        }

        if (vs->outbuf)
        {
            g_free(vs->outbuf);
            vs->outbuf = NULL;
        }

        double ratio = vs->bytes_compressed > 0
                           ? (double)vs->bytes_uncompressed / vs->bytes_compressed
                           : 0;

        fprintf(stderr, "[%s] vCPU %d: %" PRIu64 " insns, "
                        "%" PRIu64 " mem ops (%" PRIu64 " with values), "
                        "%.1f MB raw -> %.1f MB zstd (%.1fx)",
                PLUGIN_NAME, i,
                vs->insn_count, vs->mem_op_count, vs->values_captured,
                vs->bytes_uncompressed / (1024.0 * 1024.0),
                vs->bytes_compressed / (1024.0 * 1024.0),
                ratio);
        if (vs->limit_reached)
        {
            fprintf(stderr, " [limit]");
        }
        fprintf(stderr, "\n");

        total_insns += vs->insn_count;
        total_mem += vs->mem_op_count;
        total_values += vs->values_captured;
        total_raw += vs->bytes_uncompressed;
        total_comp += vs->bytes_compressed;
    }

    double total_ratio = total_comp > 0
                             ? (double)total_raw / total_comp
                             : 0;

    fprintf(stderr, "[%s] Total: %" PRIu64 " insns, %" PRIu64 " mem ops, "
                    "%" PRIu64 " values, %.1f MB -> %.1f MB (%.1fx)\n",
            PLUGIN_NAME, total_insns, total_mem, total_values,
            total_raw / (1024.0 * 1024.0),
            total_comp / (1024.0 * 1024.0),
            total_ratio);

    if (use_trigger)
    {
        fprintf(stderr, "[%s] Dormant phase: %" PRIu64 " instructions skipped\n",
                PLUGIN_NAME, trigger_skipped_insns);
    }

    fprintf(stderr, "[%s] Done.\n", PLUGIN_NAME);
}

/* ================================================================
 * Plugin entry point
 * ================================================================ */

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    plugin_id = id;

    fprintf(stderr, "[%s] ChampSim raw trace plugin v%d "
                    "(values + zstd compression)\n",
            PLUGIN_NAME, TRACE_FORMAT_VER);
    fprintf(stderr, "[%s] QEMU target: %s, plugin API: %d (min %d)\n",
            PLUGIN_NAME,
            info->target_name ? info->target_name : "unknown",
            info->version.cur, info->version.min);
    fprintf(stderr, "[%s] zstd version: %s\n",
            PLUGIN_NAME, ZSTD_versionString());

    if (!info->system_emulation)
    {
        fprintf(stderr, "[%s] ERROR: requires system emulation mode\n",
                PLUGIN_NAME);
        return -1;
    }

    if (!parse_args(argc, argv))
    {
        return -1;
    }

    /* If trigger is specified, remove any stale trigger file from previous runs */
    if (use_trigger)
    {
        unlink(trigger_file);
    }

    mkdir(output_dir, 0755);

    /* Print configuration */
    fprintf(stderr, "[%s] Output: %s\n", PLUGIN_NAME, output_dir);
    fprintf(stderr, "[%s] Tracing vCPUs:", PLUGIN_NAME);
    for (int i = 0; i < MAX_VCPUS; i++)
    {
        if (trace_vcpu[i])
            fprintf(stderr, " %d", i);
    }
    fprintf(stderr, "\n");
    if (insn_limit > 0)
    {
        fprintf(stderr, "[%s] Limit: %" PRIu64 " insns/vCPU\n",
                PLUGIN_NAME, insn_limit);
    }
    else
    {
        fprintf(stderr, "[%s] Limit: unlimited\n", PLUGIN_NAME);
    }
    fprintf(stderr, "[%s] Compression: zstd level %d\n",
            PLUGIN_NAME, ZSTD_LEVEL);

    if (use_trigger)
    {
        fprintf(stderr, "[%s] Trigger: WAITING for file '%s'\n",
                PLUGIN_NAME, trigger_file);
        fprintf(stderr, "[%s]   >>> Tracing is DORMANT <<<\n", PLUGIN_NAME);
        fprintf(stderr, "[%s]   To start tracing, run on the host:\n",
                PLUGIN_NAME);
        fprintf(stderr, "[%s]     touch %s\n", PLUGIN_NAME, trigger_file);
    }
    else
    {
        fprintf(stderr, "[%s] Trigger: immediate (no trigger file)\n",
                PLUGIN_NAME);
    }

    /* Initialize per-vCPU state */
    for (int i = 0; i < MAX_VCPUS; i++)
    {
        if (!trace_vcpu[i])
        {
            continue;
        }

        VcpuState *vs = &vcpu_state[i];
        memset(vs, 0, sizeof(VcpuState));
        vs->vcpu_id = i;

        vs->inbuf = g_malloc(INPUT_BUF_SIZE);
        vs->inbuf_pos = 0;

        vs->outbuf_size = ZSTD_CStreamOutSize();
        vs->outbuf = g_malloc(vs->outbuf_size);

        vs->cctx = ZSTD_createCCtx();
        if (!vs->cctx)
        {
            fprintf(stderr, "[%s] ERROR: ZSTD_createCCtx failed\n",
                    PLUGIN_NAME);
            return -1;
        }

        ZSTD_CCtx_setParameter(vs->cctx, ZSTD_c_compressionLevel, ZSTD_LEVEL);
        ZSTD_CCtx_setParameter(vs->cctx, ZSTD_c_checksumFlag, 1);

        char filepath[4200];
        snprintf(filepath, sizeof(filepath), "%s/trace_vcpu%d.raw.zst",
                 output_dir, i);

        vs->outfile = fopen(filepath, "wb");
        if (!vs->outfile)
        {
            fprintf(stderr, "[%s] ERROR: cannot open %s\n",
                    PLUGIN_NAME, filepath);
            return -1;
        }

        /* Write file header */
        uint32_t magic = TRACE_FORMAT_MAGIC;
        uint32_t version = TRACE_FORMAT_VER;
        uint32_t vid = i;
        uint32_t reserved = 0;

        buffer_append(vs, &magic, 4);
        buffer_append(vs, &version, 4);
        buffer_append(vs, &vid, 4);
        buffer_append(vs, &reserved, 4);

        vs->active = true;
        fprintf(stderr, "[%s] vCPU %d -> %s\n", PLUGIN_NAME, i, filepath);
    }

    /* Register callbacks */
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans_cb);
    qemu_plugin_register_atexit_cb(id, plugin_atexit, NULL);

    if (use_trigger)
    {
        fprintf(stderr, "[%s] Initialized. Plugin is DORMANT — "
                        "waiting for: touch %s\n",
                PLUGIN_NAME, trigger_file);
    }
    else
    {
        fprintf(stderr, "[%s] Initialized. Tracing begins on first insn exec.\n",
                PLUGIN_NAME);
    }

    return 0;
}
