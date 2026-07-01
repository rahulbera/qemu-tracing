/*
 * scylla_bench.c — Optimized ScyllaDB benchmark client for QEMU tracing
 *
 * Modes:
 *   load    — Async-pipelined inserts with prepared statements
 *   warmup  — Async-pipelined sequential reads with prepared statements
 *   run     — Sync Zipfian read/write mix with prepared statements
 *             (runs forever, designed for snapshot/restore survival)
 *
 * Key format: user0000001 .. userNNNNNNN (zero-padded 7 digits)
 *
 * Build:
 *   gcc -O2 -o scylla_bench scylla_bench.c -lcassandra -lpthread -lm
 *
 * Examples:
 *   ./scylla_bench --mode=load --records=5000000 --cpus=0,5,6
 *   ./scylla_bench --mode=warmup --records=5000000 --cpus=0,5,6
 *   nohup ./scylla_bench --mode=run --records=5000000 --cpus=5,6 \
 *       --read-ratio=95 --zipfian-skew=0.99 > /tmp/bench.log 2>&1 &
 */

#define _GNU_SOURCE
#include <cassandra.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ================================================================
 * Tuning constants
 * ================================================================ */
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9042
#define DEFAULT_RECORDS 5000000
#define DEFAULT_THREADS 2
#define DEFAULT_READ_RATIO 95
#define DEFAULT_SKEW 0.99
#define PIPELINE_DEPTH 256    /* async concurrency per thread   */
#define STATS_INTERVAL_SEC 10 /* heartbeat reporting interval   */
#define KEY_FMT "user%07ld"
#define KEY_BUF_LEN 32
#define NUM_FIELDS 10
#define FIELD_LEN 100
#define MAX_PIN_CPUS 16 /* max CPUs in --cpus= list       */

/* Latency histogram: 10 buckets with upper bounds in microseconds  */
#define HIST_BUCKETS 10
static const long HIST_BOUNDS[HIST_BUCKETS] = {
    250, 500, 1000, 2000, 5000, 10000, 50000, 100000, 500000, 1000000};

/* ================================================================
 * Types
 * ================================================================ */
typedef enum
{
    MODE_LOAD,
    MODE_WARMUP,
    MODE_RUN
} bench_mode_t;

/* Per-thread stats — only the owning thread writes; reporter reads */
typedef struct
{
    long reads_ok;
    long reads_err;
    long writes_ok;
    long writes_err;
    long lat_sum_us;
    long lat_count;
    long lat_max_us;
    long lat_min_us;
    long hist[HIST_BUCKETS + 1]; /* +1 for overflow bucket */
} thread_stats_t;

typedef struct
{
    int thread_id;
    int pin_cpu;
    long key_start;
    long key_end;
    thread_stats_t stats;
} thread_ctx_t;

/* Pending async operation */
typedef struct
{
    CassFuture *future;
    struct timespec t_submit;
    int is_write;
} pending_op_t;

/* ================================================================
 * Global state
 * ================================================================ */
static struct
{
    bench_mode_t mode;
    const char *host;
    int port;
    long records;
    int threads;
    int read_ratio;
    double skew;
    int cpus[MAX_PIN_CPUS];
    int ncpus;
} cfg;

static CassSession *g_session = NULL;
static const CassPrepared *g_prep_insert = NULL;
static const CassPrepared *g_prep_read = NULL;
static const CassPrepared *g_prep_update = NULL;

static volatile sig_atomic_t g_running = 1;
static thread_ctx_t *g_ctxs = NULL;
static int g_nctxs = 0;

/* ================================================================
 * Signal handlers
 * ================================================================ */
static void on_shutdown(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ================================================================
 * Utility: elapsed microseconds since a timespec
 * ================================================================ */
static inline long elapsed_us(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000000L + (now.tv_nsec - start->tv_nsec) / 1000L;
}

/* ================================================================
 * Stats helpers
 * ================================================================ */
static void stats_init(thread_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->lat_min_us = 999999999L;
}

static inline void stats_record(thread_stats_t *s, long us,
                                int is_write, int is_error)
{
    if (is_error)
    {
        if (is_write)
            s->writes_err++;
        else
            s->reads_err++;
        return;
    }

    if (is_write)
        s->writes_ok++;
    else
        s->reads_ok++;

    s->lat_sum_us += us;
    s->lat_count++;
    if (us > s->lat_max_us)
        s->lat_max_us = us;
    if (us < s->lat_min_us)
        s->lat_min_us = us;

    int b;
    for (b = 0; b < HIST_BUCKETS; b++)
        if (us < HIST_BOUNDS[b])
            break;
    s->hist[b]++;
}

/* Approximate percentile from histogram */
static double hist_percentile(const long *hist, long total, double pct)
{
    if (total == 0)
        return 0.0;
    long target = (long)(total * pct / 100.0);
    long cum = 0;
    for (int b = 0; b <= HIST_BUCKETS; b++)
    {
        cum += hist[b];
        if (cum >= target)
        {
            if (b < HIST_BUCKETS)
                return HIST_BOUNDS[b] / 1000.0;
            return 1000.0;
        }
    }
    return 1000.0;
}

/* ================================================================
 * Stats reporter thread — prints heartbeat every STATS_INTERVAL_SEC
 *
 * Output format (similar to cassandra-stress):
 *   time  total_ops  interval  rd_ok  wr_ok  rd_err  wr_err  ops/s  avg  p95  p99  max
 * ================================================================ */
typedef struct
{
    long reads_ok, reads_err, writes_ok, writes_err;
    long lat_sum_us, lat_count, lat_max_us;
    long hist[HIST_BUCKETS + 1];
} snap_t;

static void *stats_reporter(void *arg)
{
    (void)arg;
    snap_t *prev = calloc(g_nctxs, sizeof(snap_t));
    int elapsed_sec = 0;

    fprintf(stderr,
            "\n%7s %10s %10s %8s %8s %8s %8s %9s %8s %8s %8s %8s",
            "time", "total_ops", "intv_ops", "rd_ok", "wr_ok",
            "rd_err", "wr_err", "ops/s",
            "avg_ms", "p95_ms", "p99_ms", "max_ms");
    if (cfg.mode != MODE_RUN)
        fprintf(stderr, " %8s", "progr");
    fprintf(stderr, "\n");

    fprintf(stderr,
            "─────── ────────── ────────── ──────── ──────── "
            "──────── ──────── ───────── "
            "──────── ──────── ──────── ────────");
    if (cfg.mode != MODE_RUN)
        fprintf(stderr, " ────────");
    fprintf(stderr, "\n");

    while (g_running)
    {
        sleep(STATS_INTERVAL_SEC);
        if (!g_running)
            break;
        elapsed_sec += STATS_INTERVAL_SEC;

        /* Aggregate current and compute deltas */
        long c_ro = 0, c_re = 0, c_wo = 0, c_we = 0;
        long c_ls = 0, c_lc = 0, c_lm = 0;
        long c_h[HIST_BUCKETS + 1];
        memset(c_h, 0, sizeof(c_h));

        long p_ro = 0, p_re = 0, p_wo = 0, p_we = 0;
        long p_ls = 0, p_lc = 0;
        long p_h[HIST_BUCKETS + 1];
        memset(p_h, 0, sizeof(p_h));

        for (int i = 0; i < g_nctxs; i++)
        {
            thread_stats_t *s = &g_ctxs[i].stats;
            c_ro += s->reads_ok;
            c_re += s->reads_err;
            c_wo += s->writes_ok;
            c_we += s->writes_err;
            c_ls += s->lat_sum_us;
            c_lc += s->lat_count;
            if (s->lat_max_us > c_lm)
                c_lm = s->lat_max_us;
            for (int b = 0; b <= HIST_BUCKETS; b++)
                c_h[b] += s->hist[b];

            p_ro += prev[i].reads_ok;
            p_re += prev[i].reads_err;
            p_wo += prev[i].writes_ok;
            p_we += prev[i].writes_err;
            p_ls += prev[i].lat_sum_us;
            p_lc += prev[i].lat_count;
            for (int b = 0; b <= HIST_BUCKETS; b++)
                p_h[b] += prev[i].hist[b];
        }

        long d_ro = c_ro - p_ro, d_re = c_re - p_re;
        long d_wo = c_wo - p_wo, d_we = c_we - p_we;
        long d_ls = c_ls - p_ls, d_lc = c_lc - p_lc;

        long d_h[HIST_BUCKETS + 1];
        for (int b = 0; b <= HIST_BUCKETS; b++)
            d_h[b] = c_h[b] - p_h[b];

        long total = c_ro + c_wo;
        long intv = d_ro + d_wo;
        long rate = intv / STATS_INTERVAL_SEC;
        double avg = d_lc > 0 ? (double)d_ls / d_lc / 1000.0 : 0;
        double p95 = hist_percentile(d_h, d_lc, 95.0);
        double p99 = hist_percentile(d_h, d_lc, 99.0);
        double mx = c_lm / 1000.0;

        fprintf(stderr,
                "%4ds   %10ld %10ld %8ld %8ld %8ld %8ld %8ld/s "
                "%7.2f  %7.2f  %7.2f  %7.1f",
                elapsed_sec, total, intv,
                d_ro, d_wo, d_re, d_we, rate, avg, p95, p99, mx);

        if (cfg.mode != MODE_RUN)
            fprintf(stderr, "  %5.1f%%",
                    100.0 * (double)total / (double)cfg.records);
        fprintf(stderr, "\n");

        /* Save snapshot */
        for (int i = 0; i < g_nctxs; i++)
        {
            thread_stats_t *s = &g_ctxs[i].stats;
            prev[i].reads_ok = s->reads_ok;
            prev[i].reads_err = s->reads_err;
            prev[i].writes_ok = s->writes_ok;
            prev[i].writes_err = s->writes_err;
            prev[i].lat_sum_us = s->lat_sum_us;
            prev[i].lat_count = s->lat_count;
            prev[i].lat_max_us = s->lat_max_us;
            memcpy(prev[i].hist, s->hist, sizeof(prev[i].hist));
        }
    }

    free(prev);
    return NULL;
}

/* ================================================================
 * Scrambled Zipfian generator (YCSB algorithm)
 * ================================================================ */
typedef struct
{
    long n;
    double s;
    double zeta_n;
    double zeta_2;
    double alpha;
    double eta;
    unsigned int rng;
} zipfian_t;

static double zeta(long n, double s)
{
    double sum = 0.0;
    for (long i = 1; i <= n; i++)
        sum += 1.0 / pow((double)i, s);
    return sum;
}

static void zipfian_init(zipfian_t *z, long n, double s, unsigned int seed)
{
    z->n = n;
    z->s = s;
    z->rng = seed;
    fprintf(stderr, "  Precomputing zeta(%ld, %.2f)...", n, s);
    fflush(stderr);
    z->zeta_2 = zeta(2, s);
    z->zeta_n = zeta(n, s);
    fprintf(stderr, " done\n");
    z->alpha = 1.0 / (1.0 - s);
    z->eta = (1.0 - pow(2.0 / (double)n, 1.0 - s)) / (1.0 - z->zeta_2 / z->zeta_n);
}

static long zipfian_next_raw(zipfian_t *z)
{
    double u = (double)rand_r(&z->rng) / (double)RAND_MAX;
    double uz = u * z->zeta_n;
    if (uz < 1.0)
        return 0;
    if (uz < 1.0 + pow(0.5, z->s))
        return 1;
    return (long)((double)z->n * pow(z->eta * u - z->eta + 1.0, z->alpha));
}

static uint64_t fnv1a_64(uint64_t val)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 8; i++)
    {
        h ^= (val & 0xff);
        h *= 0x100000001b3ULL;
        val >>= 8;
    }
    return h;
}

static long zipfian_next(zipfian_t *z)
{
    long raw = zipfian_next_raw(z);
    return 1 + (long)(fnv1a_64((uint64_t)raw) % (uint64_t)z->n);
}

/* ================================================================
 * Random field data
 * ================================================================ */
static void random_field(char *buf, int len, unsigned int *seed)
{
    static const char cs[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789";
    for (int i = 0; i < len; i++)
        buf[i] = cs[rand_r(seed) % (sizeof(cs) - 1)];
    buf[len] = '\0';
}

/* ================================================================
 * Pin calling thread to a CPU
 * ================================================================ */
static void pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        fprintf(stderr, "Warning: could not pin to CPU %d\n", cpu);
}

/* ================================================================
 * Drain one pending async operation
 * ================================================================ */
static void drain_pending(pending_op_t *op, thread_stats_t *st)
{
    cass_future_wait(op->future);
    long lat = elapsed_us(&op->t_submit);
    CassError rc = cass_future_error_code(op->future);
    cass_future_free(op->future);
    op->future = NULL;
    stats_record(st, lat, op->is_write, rc != CASS_OK);
}

/* ================================================================
 * Worker: LOAD (async pipelined inserts with prepared statements)
 * ================================================================ */
static void *worker_load(void *arg)
{
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    unsigned int seed = (unsigned int)(ctx->thread_id * 31337 + time(NULL));
    pin_to_cpu(ctx->pin_cpu);
    stats_init(&ctx->stats);

    pending_op_t pipeline[PIPELINE_DEPTH];
    memset(pipeline, 0, sizeof(pipeline));
    int head = 0, count = 0;

    for (long k = ctx->key_start; k < ctx->key_end && g_running; k++)
    {
        /* Drain oldest if pipeline full */
        if (count >= PIPELINE_DEPTH)
        {
            drain_pending(&pipeline[head], &ctx->stats);
            head = (head + 1) % PIPELINE_DEPTH;
            count--;
        }

        char key[KEY_BUF_LEN];
        char fields[NUM_FIELDS][FIELD_LEN + 1];
        snprintf(key, sizeof(key), KEY_FMT, k);
        for (int f = 0; f < NUM_FIELDS; f++)
            random_field(fields[f], FIELD_LEN, &seed);

        CassStatement *stmt = cass_prepared_bind(g_prep_insert);
        cass_statement_bind_string(stmt, 0, key);
        for (int f = 0; f < NUM_FIELDS; f++)
            cass_statement_bind_string(stmt, 1 + f, fields[f]);
        cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);

        int tail = (head + count) % PIPELINE_DEPTH;
        clock_gettime(CLOCK_MONOTONIC, &pipeline[tail].t_submit);
        pipeline[tail].future = cass_session_execute(g_session, stmt);
        pipeline[tail].is_write = 1;
        count++;

        cass_statement_free(stmt);
    }

    /* Drain remaining */
    while (count > 0)
    {
        drain_pending(&pipeline[head], &ctx->stats);
        head = (head + 1) % PIPELINE_DEPTH;
        count--;
    }
    return NULL;
}

/* ================================================================
 * Worker: WARMUP (async pipelined sequential reads)
 * ================================================================ */
static void *worker_warmup(void *arg)
{
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    pin_to_cpu(ctx->pin_cpu);
    stats_init(&ctx->stats);

    pending_op_t pipeline[PIPELINE_DEPTH];
    memset(pipeline, 0, sizeof(pipeline));
    int head = 0, count = 0;

    for (long k = ctx->key_start; k < ctx->key_end && g_running; k++)
    {
        if (count >= PIPELINE_DEPTH)
        {
            drain_pending(&pipeline[head], &ctx->stats);
            head = (head + 1) % PIPELINE_DEPTH;
            count--;
        }

        char key[KEY_BUF_LEN];
        snprintf(key, sizeof(key), KEY_FMT, k);

        CassStatement *stmt = cass_prepared_bind(g_prep_read);
        cass_statement_bind_string(stmt, 0, key);
        cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);

        int tail = (head + count) % PIPELINE_DEPTH;
        clock_gettime(CLOCK_MONOTONIC, &pipeline[tail].t_submit);
        pipeline[tail].future = cass_session_execute(g_session, stmt);
        pipeline[tail].is_write = 0;
        count++;

        cass_statement_free(stmt);
    }

    while (count > 0)
    {
        drain_pending(&pipeline[head], &ctx->stats);
        head = (head + 1) % PIPELINE_DEPTH;
        count--;
    }
    return NULL;
}

/* ================================================================
 * Worker: RUN (sync Zipfian read/write mix, loops forever)
 * ================================================================ */
static void *worker_run(void *arg)
{
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    unsigned int seed = (unsigned int)(ctx->thread_id * 7919 + time(NULL));
    pin_to_cpu(ctx->pin_cpu);
    stats_init(&ctx->stats);

    zipfian_t zipf;
    zipfian_init(&zipf, cfg.records, cfg.skew, seed ^ 0xDEADBEEF);

    while (g_running)
    {
        long key_id = zipfian_next(&zipf);
        int is_write = (rand_r(&seed) % 100) >= cfg.read_ratio;

        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        CassError rc;
        if (is_write)
        {
            char key[KEY_BUF_LEN], val[FIELD_LEN + 1];
            snprintf(key, sizeof(key), KEY_FMT, key_id);
            random_field(val, FIELD_LEN, &seed);

            CassStatement *stmt = cass_prepared_bind(g_prep_update);
            cass_statement_bind_string(stmt, 0, val);
            cass_statement_bind_string(stmt, 1, key);
            cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);

            CassFuture *f = cass_session_execute(g_session, stmt);
            cass_future_wait(f);
            rc = cass_future_error_code(f);
            cass_future_free(f);
            cass_statement_free(stmt);
        }
        else
        {
            char key[KEY_BUF_LEN];
            snprintf(key, sizeof(key), KEY_FMT, key_id);

            CassStatement *stmt = cass_prepared_bind(g_prep_read);
            cass_statement_bind_string(stmt, 0, key);
            cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);

            CassFuture *f = cass_session_execute(g_session, stmt);
            cass_future_wait(f);
            rc = cass_future_error_code(f);
            cass_future_free(f);
            cass_statement_free(stmt);
        }

        long lat = elapsed_us(&t0);
        stats_record(&ctx->stats, lat, is_write, rc != CASS_OK);
    }
    return NULL;
}

/* ================================================================
 * Prepare CQL statements (done once at startup)
 * ================================================================ */
static const CassPrepared *prepare_one(const char *query)
{
    CassFuture *f = cass_session_prepare(g_session, query);
    cass_future_wait(f);
    if (cass_future_error_code(f) != CASS_OK)
    {
        const char *msg;
        size_t len;
        cass_future_error_message(f, &msg, &len);
        fprintf(stderr, "Prepare failed: %.*s\nQuery: %s\n",
                (int)len, msg, query);
        cass_future_free(f);
        return NULL;
    }
    const CassPrepared *p = cass_future_get_prepared(f);
    cass_future_free(f);
    return p;
}

static int prepare_all(void)
{
    fprintf(stderr, "Preparing CQL statements...\n");

    g_prep_insert = prepare_one(
        "INSERT INTO ycsb.usertable "
        "(y_id, field0, field1, field2, field3, field4, "
        " field5, field6, field7, field8, field9) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!g_prep_insert)
        return -1;

    g_prep_read = prepare_one(
        "SELECT * FROM ycsb.usertable WHERE y_id = ?");
    if (!g_prep_read)
        return -1;

    g_prep_update = prepare_one(
        "UPDATE ycsb.usertable SET field0 = ? WHERE y_id = ?");
    if (!g_prep_update)
        return -1;

    fprintf(stderr, "Statements prepared (INSERT, SELECT, UPDATE)\n");
    return 0;
}

/* ================================================================
 * Connect to ScyllaDB
 * ================================================================ */
static CassSession *connect_db(const char *host, int port)
{
    CassCluster *cluster = cass_cluster_new();
    CassSession *sess = cass_session_new();

    cass_cluster_set_contact_points(cluster, host);
    cass_cluster_set_port(cluster, port);
    cass_cluster_set_num_threads_io(cluster, cfg.threads < 2 ? 1 : 2);
    cass_cluster_set_core_connections_per_host(cluster, 2);
    cass_cluster_set_queue_size_io(cluster, 32768);

    /* Token-aware routing: connect with keyspace */
    CassFuture *f = cass_session_connect_keyspace(sess, cluster, "ycsb");
    cass_future_wait(f);

    if (cass_future_error_code(f) != CASS_OK)
    {
        const char *msg;
        size_t len;
        cass_future_error_message(f, &msg, &len);
        fprintf(stderr, "Connect failed: %.*s\n", (int)len, msg);
        cass_future_free(f);
        cass_cluster_free(cluster);
        cass_session_free(sess);
        return NULL;
    }

    fprintf(stderr, "Connected to ScyllaDB at %s:%d (keyspace: ycsb)\n",
            host, port);
    cass_future_free(f);
    cass_cluster_free(cluster);
    return sess;
}

/* ================================================================
 * Usage & argument parsing
 * ================================================================ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "scylla_bench — Optimized native C benchmark for ScyllaDB\n\n"
            "Usage: %s --mode=MODE [options]\n\n"
            "Modes:\n"
            "  load     Async-pipelined insert of --records records\n"
            "  warmup   Async-pipelined sequential read of all records\n"
            "  run      Sync Zipfian read/write mix (runs forever)\n\n"
            "Options:\n"
            "  --host=HOST           Contact point    (default: %s)\n"
            "  --port=PORT           CQL port         (default: %d)\n"
            "  --records=N           Record count     (default: %d)\n"
            "  --threads=N           Worker threads   (default: %d)\n"
            "  --cpus=C1,C2,...      CPUs to pin to   (default: 5,6)\n"
            "  --read-ratio=N        Read %% 0-100    (default: %d)\n"
            "  --zipfian-skew=F      Zipfian param    (default: %.2f)\n\n"
            "Key format: user0000001 .. user%07d\n\n"
            "Threads round-robin over the --cpus list. If --threads exceeds\n"
            "the CPU count, multiple threads share CPUs.\n\n"
            "Examples:\n"
            "  %s --mode=load --records=5000000 --cpus=0,5,6\n"
            "  %s --mode=warmup --records=5000000 --cpus=0,5,6\n"
            "  nohup %s --mode=run --records=5000000 --cpus=5,6 \\\n"
            "      --read-ratio=95 --zipfian-skew=0.99 > /tmp/bench.log 2>&1 &\n",
            prog, DEFAULT_HOST, DEFAULT_PORT, DEFAULT_RECORDS,
            DEFAULT_THREADS, DEFAULT_READ_RATIO, DEFAULT_SKEW,
            DEFAULT_RECORDS, prog, prog, prog);
    exit(1);
}

static void parse_args(int argc, char *argv[])
{
    cfg.host = DEFAULT_HOST;
    cfg.port = DEFAULT_PORT;
    cfg.records = DEFAULT_RECORDS;
    cfg.threads = DEFAULT_THREADS;
    cfg.read_ratio = DEFAULT_READ_RATIO;
    cfg.skew = DEFAULT_SKEW;
    cfg.mode = (bench_mode_t)-1;
    cfg.cpus[0] = 5;
    cfg.cpus[1] = 6;
    cfg.ncpus = 2;

    for (int i = 1; i < argc; i++)
    {
        char *a = argv[i];
        if (strncmp(a, "--mode=", 7) == 0)
        {
            const char *m = a + 7;
            if (strcmp(m, "load") == 0)
                cfg.mode = MODE_LOAD;
            else if (strcmp(m, "warmup") == 0)
                cfg.mode = MODE_WARMUP;
            else if (strcmp(m, "run") == 0)
                cfg.mode = MODE_RUN;
            else
            {
                fprintf(stderr, "Unknown mode: %s\n", m);
                exit(1);
            }
        }
        else if (strncmp(a, "--host=", 7) == 0)
            cfg.host = a + 7;
        else if (strncmp(a, "--port=", 7) == 0)
            cfg.port = atoi(a + 7);
        else if (strncmp(a, "--records=", 10) == 0)
            cfg.records = atol(a + 10);
        else if (strncmp(a, "--threads=", 10) == 0)
            cfg.threads = atoi(a + 10);
        else if (strncmp(a, "--read-ratio=", 13) == 0)
            cfg.read_ratio = atoi(a + 13);
        else if (strncmp(a, "--zipfian-skew=", 15) == 0)
            cfg.skew = atof(a + 15);
        else if (strncmp(a, "--cpus=", 7) == 0)
        {
            /* Parse comma-separated CPU list, e.g. --cpus=5,6 or --cpus=0,5,6 */
            cfg.ncpus = 0;
            char *list = strdup(a + 7);
            char *tok = strtok(list, ",");
            while (tok && cfg.ncpus < MAX_PIN_CPUS)
            {
                cfg.cpus[cfg.ncpus++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
            free(list);
            if (cfg.ncpus == 0)
            {
                fprintf(stderr, "Error: --cpus= requires at least one CPU\n");
                exit(1);
            }
        }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
            usage(argv[0]);
        else
        {
            fprintf(stderr, "Unknown option: %s\n", a);
            usage(argv[0]);
        }
    }

    if ((int)cfg.mode == -1)
    {
        fprintf(stderr, "Error: --mode is required\n\n");
        usage(argv[0]);
    }
    if (cfg.read_ratio < 0 || cfg.read_ratio > 100)
    {
        fprintf(stderr, "Error: --read-ratio must be 0-100\n");
        exit(1);
    }
}

/* ================================================================
 * Main
 * ================================================================ */
int main(int argc, char *argv[])
{
    parse_args(argc, argv);

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_shutdown);
    signal(SIGTERM, on_shutdown);

    const char *mode_str =
        cfg.mode == MODE_LOAD ? "load" : cfg.mode == MODE_WARMUP ? "warmup"
                                                                 : "run";

    /* Build CPU list string for display */
    char cpus_str[128] = "";
    for (int i = 0; i < cfg.ncpus; i++)
    {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%s%d", i > 0 ? "," : "", cfg.cpus[i]);
        strcat(cpus_str, tmp);
    }

    fprintf(stderr,
            "\n"
            "╔══════════════════════════════════════════════╗\n"
            "║           scylla_bench starting              ║\n"
            "╠══════════════════════════════════════════════╣\n"
            "║  mode         : %-28s║\n"
            "║  host         : %-24s:%d ║\n"
            "║  records      : %-28ld║\n"
            "║  threads      : %-28d║\n"
            "║  cpus         : %-28s║\n"
            "║  read_ratio   : %-27d%%║\n"
            "║  zipfian_skew : %-28.2f║\n"
            "║  pipeline     : %-28d║\n"
            "║  key_range    : user0000001..user%07ld   ║\n"
            "╚══════════════════════════════════════════════╝\n\n",
            mode_str, cfg.host, cfg.port, cfg.records,
            cfg.threads, cpus_str, cfg.read_ratio, cfg.skew,
            (cfg.mode == MODE_RUN) ? 1 : PIPELINE_DEPTH,
            cfg.records);

    /* Connect */
    g_session = connect_db(cfg.host, cfg.port);
    if (!g_session)
        return 1;

    /* Prepare statements */
    if (prepare_all() != 0)
        return 1;

    /* Thread contexts */
    g_nctxs = cfg.threads;
    g_ctxs = calloc(g_nctxs, sizeof(thread_ctx_t));
    pthread_t *tids = calloc(g_nctxs, sizeof(pthread_t));

    long per = cfg.records / cfg.threads;
    for (int i = 0; i < g_nctxs; i++)
    {
        g_ctxs[i].thread_id = i;
        g_ctxs[i].pin_cpu = cfg.cpus[i % cfg.ncpus];
        g_ctxs[i].key_start = 1 + (long)i * per;
        g_ctxs[i].key_end = (i == g_nctxs - 1)
                                ? cfg.records + 1
                                : 1 + (long)(i + 1) * per;
        stats_init(&g_ctxs[i].stats);
    }

    void *(*fn)(void *) =
        cfg.mode == MODE_LOAD ? worker_load : cfg.mode == MODE_WARMUP ? worker_warmup
                                                                      : worker_run;

    /* Stats reporter (runs in all modes) */
    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_reporter, NULL);

    /* Launch workers */
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int i = 0; i < g_nctxs; i++)
        pthread_create(&tids[i], NULL, fn, &g_ctxs[i]);

    for (int i = 0; i < g_nctxs; i++)
        pthread_join(tids[i], NULL);

    g_running = 0;
    pthread_join(stats_tid, NULL);

    /* Final summary */
    long tot_ro = 0, tot_re = 0, tot_wo = 0, tot_we = 0;
    long tot_ls = 0, tot_lc = 0;
    for (int i = 0; i < g_nctxs; i++)
    {
        thread_stats_t *s = &g_ctxs[i].stats;
        tot_ro += s->reads_ok;
        tot_re += s->reads_err;
        tot_wo += s->writes_ok;
        tot_we += s->writes_err;
        tot_ls += s->lat_sum_us;
        tot_lc += s->lat_count;
    }

    long total_ops = tot_ro + tot_wo;
    long total_errs = tot_re + tot_we;
    double elapsed_s = elapsed_us(&t_start) / 1e6;
    double avg_ms = tot_lc > 0 ? (double)tot_ls / tot_lc / 1000.0 : 0;

    fprintf(stderr,
            "\n"
            "══════════════════════════════════════════════\n"
            "  FINAL RESULTS\n"
            "══════════════════════════════════════════════\n"
            "  Mode:           %s\n"
            "  Elapsed:        %.1f s\n"
            "  Total ops:      %ld\n"
            "  Reads OK:       %ld\n"
            "  Reads failed:   %ld\n"
            "  Writes OK:      %ld\n"
            "  Writes failed:  %ld\n"
            "  Throughput:     %.0f ops/s\n"
            "  Avg latency:    %.2f ms\n"
            "══════════════════════════════════════════════\n",
            mode_str, elapsed_s, total_ops,
            tot_ro, tot_re, tot_wo, tot_we,
            elapsed_s > 0 ? total_ops / elapsed_s : 0, avg_ms);

    /* Cleanup */
    if (g_prep_insert)
        cass_prepared_free(g_prep_insert);
    if (g_prep_read)
        cass_prepared_free(g_prep_read);
    if (g_prep_update)
        cass_prepared_free(g_prep_update);

    CassFuture *cf = cass_session_close(g_session);
    cass_future_wait(cf);
    cass_future_free(cf);
    cass_session_free(g_session);

    free(tids);
    free(g_ctxs);
    return (total_errs > 0) ? 1 : 0;
}
