# ScyllaDB Tracing Pipeline — Stage 1

## Installation, Data Loading, and Cache Warmup

This document captures the complete, tested procedure for setting up
ScyllaDB inside the QEMU guest VM, loading 5 million records, and
warming the in-memory cache. Every command listed here is the final
working version — earlier failed attempts and the lessons they taught
are documented in the "Lessons Learned" section at the end.

This guide assumes the QEMU infrastructure, guest VM (Ubuntu 24.04
cloud image), kvmclock patch, tracing plugin, and base guest tuning
from the Memcached methodology are already in place.

---

## 1. QEMU CPU Model Change

### Problem

ScyllaDB requires SSE4.2 and PCLMULQDQ instructions. The Memcached
pipeline used `-cpu host,-kvmclock` under KVM and `-cpu qemu64` under
TCG. The `qemu64` model lacks both features and ScyllaDB refuses to
start.

### Solution

Use a named CPU model that includes the required features and works
identically under both KVM and TCG. This also solves the snapshot
compatibility problem — KVM and TCG snapshots are interchangeable
because the guest sees the same CPU in both phases.

**Host CPU:** Intel Core i7-8700 (Coffee Lake, 8th gen). Coffee Lake
is architecturally Skylake without AVX-512, so `Skylake-Client` is
the correct QEMU model.

**Boot commands:**

```bash
# KVM (setup, data loading, snapshots)
-cpu Skylake-Client,-kvmclock

# TCG (tracing)
-cpu Skylake-Client
```

**Why not `-cpu max`:** Under KVM, `max` means "host CPU features."
Under TCG, `max` means "all features QEMU can emulate." These are
different sets, so snapshots taken under one may not load under the
other. A named model gives an explicit, frozen feature set.

**Why not `Haswell`:** Haswell also works (and is more conservative),
but `Skylake-Client` is closer to the actual host hardware, so
ScyllaDB's instruction sequences in traces better reflect real
Coffee Lake behavior. Either choice is valid.

**Impact on Memcached snapshots:** Existing Memcached snapshots were
taken with `-cpu host,-kvmclock` and are tied to that CPU model.
They will not load under `-cpu Skylake-Client`. This is fine — the
Memcached work is complete. Going forward, all ScyllaDB snapshots
must use the same named model.

---

## 2. Guest Kernel Parameters Update

### vCPU Layout Change

For ScyllaDB, we reserve vCPU 0 for bootstrap and OS work (avoiding
tracing of initialization code) and use vCPUs 1-4 for the four
ScyllaDB shards:

```
vCPU 0 ─── OS / bootstrap (NOT traced)
vCPU 1 ─── ScyllaDB shard 0 (traced)
vCPU 2 ─── ScyllaDB shard 1 (traced)
vCPU 3 ─── ScyllaDB shard 2 (traced)
vCPU 4 ─── ScyllaDB shard 3 (traced)
vCPU 5-6 ─ Benchmark client + OS (NOT traced)
```

### Update isolcpus

The Memcached setup had `isolcpus=0-3`. Update the guest kernel
command line to:

```
isolcpus=1-4
```

Edit the kernel command line (via `/etc/default/grub` or
`/boot/grub/grub.cfg` directly — verify with `cat /proc/cmdline`
after reboot, as cloud images may ignore `/etc/default/grub`).

Reboot the guest after changing the kernel parameters.

Full daemon disable list — do all of these before taking any snapshot:

```bash
# Crash reporting
sudo systemctl disable apport
sudo systemctl disable whoopsie  
sudo systemctl disable kerneloops
echo 'enabled=0' | sudo tee /etc/default/apport

# Snap - heavy background I/O and timers
sudo systemctl disable snapd
sudo systemctl disable snapd.socket
sudo systemctl disable snapd.seeded

# Package management background activity
sudo systemctl disable unattended-upgrades
sudo systemctl disable apt-daily.timer
sudo systemctl disable apt-daily-upgrade.timer
sudo systemctl disable dpkg-db-backup.timer

# Network/discovery services not needed in the VM
sudo systemctl disable avahi-daemon
sudo systemctl disable ModemManager
sudo systemctl disable multipathd

# Time sync - can behave oddly across snapshot/restore
sudo systemctl disable systemd-timesyncd

# IRQ balancing - conflicts with our manual CPU pinning
sudo systemctl disable irqbalance

# Ubuntu telemetry
sudo systemctl disable ua-timer
sudo systemctl disable ubuntu-advantage

sudo systemctl disable cron
sudo systemctl disable udisks2

# ScyllaDB - start manually, not on boot
sudo systemctl disable scylla-server
```

Verify nothing important broke:

```bash
# After rebooting, check what's still running
systemctl list-units --type=service --state=running
```

One sysctl addition — add to `/etc/sysctl.d/99-scylla.conf`:
```bash
echo "kernel.watchdog = 0" | sudo tee -a /etc/sysctl.d/99-scylla.conf
echo "kernel.hung_task_timeout_secs = 0" | sudo tee -a /etc/sysctl.d/99-scylla.conf
```

---

## 3. Install ScyllaDB

### 3.1 Add APT Repository

```bash
sudo apt-get update
sudo apt-get install -y curl gpg apt-transport-https

sudo mkdir -p /etc/apt/keyrings
sudo gpg --homedir /tmp \
    --no-default-keyring \
    --keyring /tmp/scylladb-temp.gpg \
    --keyserver hkp://keyserver.ubuntu.com:80 \
    --recv-keys c503c686b007f39e
sudo gpg --homedir /tmp \
    --no-default-keyring \
    --keyring /tmp/scylladb-temp.gpg \
    --export --armor c503c686b007f39e \
    | gpg --dearmor \
    | sudo tee /etc/apt/keyrings/scylladb.gpg > /dev/null

sudo wget -O /etc/apt/sources.list.d/scylla.list \
    https://downloads.scylladb.com/deb/debian/scylla-2026.1.list
```

**Note:** Check https://www.scylladb.com/download/ for the current
stable branch. Replace `2026.1` if a newer branch is available.

### 3.2 Install Packages

```bash
sudo apt-get update
sudo apt-get install -y scylla
```

### 3.3 Install Build Dependencies for scylla_bench

The native C benchmark client (`scylla_bench`) requires the DataStax
C/C++ driver for CQL communication. Install the build toolchain and
driver dependencies:

```bash
sudo apt-get install -y cmake g++ make libuv1-dev libssl-dev zlib1g-dev
```

---

## 4. Configure ScyllaDB

### 4.1 Understanding ScyllaDB's Systemd Configuration

ScyllaDB's systemd unit (`scylla-server.service`) launches the
binary with multiple environment variables:

```
ExecStart=/usr/bin/scylla $SCYLLA_ARGS $SEASTAR_IO $DEV_MODE $CPUSET $MEM_CONF
```

Each variable is defined in a separate file under `/etc/scylla.d/`
or in `/etc/default/scylla-server`. The key lesson: **all flags that
take values must use `=` syntax** (e.g., `--smp=4`, not `--smp 4`).
Space-separated flags break when systemd expands the environment
variables.

Also, **boolean flags like `--overprovisioned` are bare** — their
presence means "on", their absence means "off". They do not accept
`=0` or `=1`.

### 4.2 Set Configuration Files

**SCYLLA_ARGS** — general flags (`/etc/default/scylla-server`):

```bash
# Edit the file:
sudo nano /etc/default/scylla-server

# Set this line:
SCYLLA_ARGS="--log-to-syslog=0 --log-to-stdout=1 --default-log-level=info"
```

**CPUSET** — shard count and CPU pinning (`/etc/scylla.d/cpuset.conf`):

```bash
echo 'CPUSET="--cpuset=1-4 --smp=4"' | sudo tee /etc/scylla.d/cpuset.conf
```

**DEV_MODE** — bypass hardware checks (`/etc/scylla.d/dev.conf`):

```bash
echo 'DEV_MODE="--developer-mode=1"' | sudo tee /etc/scylla.d/dev.conf
```

**MEM_CONF** — memory allocation (`/etc/scylla.d/mem.conf`):

```bash
echo 'MEM_CONF="--memory=8G"' | sudo tee /etc/scylla.d/mem.conf
```

**Why `--developer-mode=1`:** The guest VM uses ext4 (not XFS) and
hasn't run `iotune`. Developer mode bypasses these checks. This is
fine for tracing — we care about instruction-level behavior, not
peak I/O throughput.

**Why no `--overprovisioned`:** Omitting this flag enables CPU pinning
and polling optimizations, ensuring each shard thread is pinned to
its assigned vCPU — exactly matching our tracing model.

### 4.3 Set AIO Limits

ScyllaDB uses asynchronous I/O heavily. The default Linux AIO limit
is too low for 4 shards:

```bash
# First check current limit
cat /proc/sys/fs/aio-max-nr

# If is it less, then set it to a bigger value
sudo sysctl -w fs.aio-max-nr=1048576
echo "fs.aio-max-nr = 1048576" | sudo tee -a /etc/sysctl.d/99-scylla.conf
```

### 4.4 Override Systemd CPU Affinity

Because `isolcpus=1-4` removes those CPUs from the default scheduler
affinity, ScyllaDB's process would not have permission to use them.
We must explicitly grant access via a systemd override:

```bash
sudo systemctl edit scylla-server
```

Add the following in the editor that opens:

```ini
[Service]
CPUAffinity=1 2 3 4
```

Save and exit, then reload:

```bash
sudo systemctl daemon-reload
```

This sets the process affinity mask before ScyllaDB starts, so
Seastar sees vCPUs 1-4 as available and can pin its shards to them.

### 4.5 Configure scylla.yaml

```bash
sudo nano /etc/scylla/scylla.yaml
```

Verify or set:

```yaml
listen_address: 127.0.0.1
rpc_address: 127.0.0.1
native_transport_port: 9042

seed_provider:
  - class_name: org.apache.cassandra.locator.SimpleSeedProvider
    parameters:
      - seeds: "127.0.0.1"

hinted_handoff_enabled: false
```

---

## 5. Start and Verify ScyllaDB

### 5.1 Start the Service

```bash
sudo systemctl start scylla-server
sudo journalctl -u scylla-server -f
```

Wait for "Starting listening for CQL clients" in the log. This may
take 30-60 seconds on first boot.

### 5.2 Verify CQL Connectivity

```bash
cqlsh 127.0.0.1 -e "SELECT release_version FROM system.local;"
```

Expected output: a version string like `3.0.8`.

### 5.3 Verify Shard-to-CPU Pinning

```bash
SCYLLA_PID=$(pgrep -x scylla)
for tid in $(ls /proc/$SCYLLA_PID/task/); do
    mask=$(taskset -p $tid 2>/dev/null | awk '{print $NF}')
    echo "TID $tid → affinity mask $mask"
done
```

**Expected:** 4 shard reactor threads with masks:

| Mask (hex) | Binary  | vCPU |
|-----------|---------|------|
| 2         | 00010   | vCPU 1 (shard 0) |
| 4         | 00100   | vCPU 2 (shard 1) |
| 8         | 01000   | vCPU 3 (shard 2) |
| 10        | 10000   | vCPU 4 (shard 3) |

Additional threads will appear: 4 syscall helper threads (same masks
as their parent shards) and 1 management thread with mask `1e`
(floats across all shard CPUs). These are normal. Total thread count
should be 9.

---

## 6. Create Database Schema

### 6.1 Create Keyspace

```bash
cqlsh 127.0.0.1 -e "CREATE KEYSPACE ycsb WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};"
```

### 6.2 Create Table

```bash
cqlsh 127.0.0.1 -e "CREATE TABLE ycsb.usertable (y_id text PRIMARY KEY, field0 text, field1 text, field2 text, field3 text, field4 text, field5 text, field6 text, field7 text, field8 text, field9 text) WITH compaction = {'class': 'SizeTieredCompactionStrategy'} AND compression = {'sstable_compression': 'LZ4Compressor'};"
```

### 6.3 Verify

```bash
cqlsh 127.0.0.1 -e "DESCRIBE TABLE ycsb.usertable;"
```

**Warnings about SimpleStrategy and RF=1 are expected and safe to
ignore** — we're running a single-node instance for tracing.

The schema has 10 text fields of 100 bytes each, making each record
~1 KB. 5M records ≈ 5 GB data footprint.

| Record count | Approximate data size | Notes |
|--------------|-----------------------|-------|
| 1,000,000 | ~1 GB | Small, good for validation |
| 5,000,000 | ~5 GB | Medium, stresses L3/DRAM boundary |
| 10,000,000 | ~10 GB | Large, requires ≥12 GB ScyllaDB memory |

---

## 7. Build scylla_bench

`scylla_bench` is a custom native C benchmark client designed for
the QEMU tracing pipeline. It handles data loading, cache warmup,
and the Zipfian benchmark run phase — all with predictable text keys
(`user0000001` through `user5000000`) and built-in CPU pinning. It
uses the DataStax C/C++ driver for CQL, prepared statements for
minimal overhead, and async pipelining for high-throughput bulk
operations.

### 7.1 Build the DataStax C/C++ Driver

```bash
cd ~
git clone https://github.com/datastax/cpp-driver.git
cd cpp-driver
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

This installs `libcassandra.so` to `/usr/local/lib/` and headers
to `/usr/local/include/`.

**Note:** If github.com is not reachable from the guest, clone on
the host and SCP the directory in:

```bash
# From the host
git clone https://github.com/datastax/cpp-driver.git
tar czf cpp-driver.tar.gz cpp-driver/
scp -P 2222 cpp-driver.tar.gz user@localhost:~/
```

### 7.2 Compile scylla_bench

Copy `scylla_bench.c` into the guest (via SCP or paste), then:

```bash
cd ~
gcc -O2 -o scylla_bench scylla_bench.c -lcassandra -lpthread -lm -lz
```

If the linker cannot find `libcassandra`, specify the paths explicitly:

```bash
gcc -O2 -o scylla_bench scylla_bench.c \
    -I/usr/local/include -L/usr/local/lib \
    -lcassandra -lpthread -lm -lz
```

### 7.3 Verify

```bash
./scylla_bench --help
```

### 7.4 Quick Connectivity Test

```bash
./scylla_bench --mode=load --records=10 --cpus=0,5,6
```

Expected: 10 inserts, 0 errors. Verify the key format:

```bash
cqlsh 127.0.0.1 -e "SELECT y_id FROM ycsb.usertable LIMIT 5;"
```

Expected: keys like `user0000002`, `user0000007`, etc. (returned in
token order, not insertion order — this is normal for CQL).

Clean up the test data before the full load:

```bash
cqlsh 127.0.0.1 --request-timeout=300 -e "DROP TABLE ycsb.usertable;"
```

Then recreate the table (Section 6.2 command).

---

## 8. Load Data (5 Million Records)

```bash
./scylla_bench --mode=load --records=5000000 --threads=3 --cpus=0,5,6
```

The `--cpus=0,5,6` flag pins the 2 worker threads to non-traced
vCPUs (round-robin: thread 0 → CPU 0, thread 1 → CPU 5). The load
mode uses async pipelining (256 concurrent requests per thread) with
prepared statements for maximum throughput.

**Expected duration:** ~10-15 minutes under KVM.

**Expected heartbeat output (every 10 seconds):**

```
   time  total_ops   intv_ops    rd_ok    wr_ok   rd_err   wr_err     ops/s  ...  progr
  10s       48000      48000        0    48000        0        0    4800/s  ...   1.0%
  20s       98000      50000        0    50000        0        0    5000/s  ...   2.0%
```

**Expected final result:**
- Total ops: 5,000,000
- Writes OK: 5,000,000
- Errors: 0

**Verify after loading:**

```bash
nodetool cfstats ycsb.usertable
```

Also verify compaction is settled:

```bash
nodetool compactionstats
```

Check:
- `Number of partitions (estimate)` ≈ 4-5M
- `Space used (total)` ≈ 4.5-5 GB
- `Write Count` = 5,000,000

**Do not use `SELECT COUNT(*)` for verification** — it scans the
entire table and times out on millions of records. Use `nodetool`
instead.

---

## 9. Warm the Cache

Read all data into ScyllaDB's in-memory cache. This is critical —
without cache warmth, traces under TCG would be dominated by disk
I/O wait instructions rather than actual query processing:

```bash
./scylla_bench --mode=warmup --records=5000000 --threads=3 --cpus=0,5,6
```

The warmup mode does a sequential read of all 5M records using async
pipelining. Every key from `user0000001` to `user5000000` is read
exactly once, ensuring the full working set is pulled into cache.

**Expected duration:** ~10-20 minutes under KVM.

**Expected heartbeat output:**

```
   time  total_ops   intv_ops    rd_ok    wr_ok   rd_err   wr_err     ops/s  ...  progr
  10s       95000      95000    95000        0        0        0    9500/s  ...   1.9%
```

**Expected final result:**
- Total ops: 5,000,000
- Reads OK: 5,000,000
- Errors: 0

**Verify after warmup:**

```bash
nodetool cfstats ycsb.usertable
```

Check:
- `Local read count` = 5,000,000
- `Local read latency` < 1 ms (sub-millisecond = served from memory)
- `Space used (live)` = `Space used (total)` (no dead data)

Also verify compaction is settled:

```bash
nodetool compactionstats
```

Expected: `pending tasks: 0`.

---

## 10. Final State Summary

After completing all steps, the guest VM state should be:

| Component | Value |
|-----------|-------|
| ScyllaDB version | 2026.1.1 |
| Shard count | 4 (vCPUs 1-4) |
| CPU model | Skylake-Client |
| Keyspace | `ycsb` (SimpleStrategy, RF=1) |
| Table | `usertable` (10 text fields, ~1 KB/record) |
| Records loaded | 5,000,000 |
| Data size | ~4.6 GB |
| SSTable count | 4 (one per shard) |
| Cache state | Warm (read latency < 1 ms) |
| Pending compaction | 0 |
| Pending flushes | 0 |

This state is ready for taking the `roi_warm` snapshot.

---

## 11. Lessons Learned

### CPU Model

**Problem:** ScyllaDB requires SSE4.2 and PCLMULQDQ. The `qemu64`
CPU model used for Memcached tracing lacks these features.

**Failed attempt:** Using `-cpu max` for both KVM and TCG. `max`
resolves to different feature sets under each accelerator, breaking
snapshot compatibility.

**Solution:** Use a named model (`Skylake-Client`) that matches the
host hardware and works identically under both KVM and TCG.

### ScyllaDB Systemd Configuration

**Problem:** ScyllaDB's systemd unit splits configuration across
multiple environment variables (`$SCYLLA_ARGS`, `$CPUSET`,
`$DEV_MODE`, `$MEM_CONF`, `$SEASTAR_IO`). Putting all flags into
`$SCYLLA_ARGS` conflicts with the unit's `ExecStart` line.

**Failed attempt 1:** Using space-separated flags
(`--smp 4 --cpuset 0-3`). Systemd's variable expansion splits the
values into positional arguments. Seastar's parser rejects them.

**Failed attempt 2:** Using `--overprovisioned=0`. This flag is a
bare boolean — it doesn't accept any value. Its presence means "on";
omit it entirely to disable.

**Solution:** Use `=` syntax for all value-taking flags
(`--smp=4`, `--cpuset=1-4`, `--memory=8G`, `--developer-mode=1`).
Place them in the correct config files (`cpuset.conf`, `dev.conf`,
`mem.conf`). Omit boolean-only flags like `--overprovisioned` to
disable them.

### isolcpus and Systemd CPUAffinity

**Problem:** `isolcpus=1-4` removes those CPUs from the default
scheduler affinity. When ScyllaDB starts, its process affinity mask
excludes the isolated CPUs. Seastar detects this and refuses to pin
shards to unavailable CPUs.

**Solution:** Add a systemd override with `CPUAffinity=1 2 3 4` to
explicitly grant ScyllaDB access to the isolated CPUs. This is the
intended workflow for `isolcpus`: isolate CPUs from the general
scheduler, then manually assign them to the workload you care about.

### Why Not YCSB or cassandra-stress

**Problem 1:** YCSB 0.17.0's `cassandra-cql` binding has compatibility
issues with ScyllaDB 2026.1 on the read path. All 5M reads returned
`NOT_FOUND` or `READ-FAILED`. Loading worked (with retries for
backpressure), but the read phase was broken.

**Problem 2:** `cassandra-stress` is no longer bundled with ScyllaDB.
It must be downloaded separately and requires Java. Its user-mode
key generation produces binary blobs rather than predictable text
keys, making it impossible for a native C benchmark client to
generate matching keys independently.

**Problem 3:** Both tools are Java-based. Java clients die on QEMU
snapshot/restore due to NIO timeout expiry and clock jumps — a lesson
learned from the Memcached pipeline with YCSB.

**Solution:** Write a custom native C benchmark client (`scylla_bench`)
using the DataStax C/C++ driver. This single tool handles loading,
warmup, and the benchmark run phase with predictable text keys
(`user0000001` through `user5000000`), prepared statements, async
pipelining, and built-in CPU pinning. It ignores SIGHUP for
snapshot/nohup survival and uses plain TCP sockets that survive
snapshot/restore cleanly.

### CQL Timeouts on Large Operations

**Problem:** `SELECT COUNT(*)` and `DROP TABLE` on tables with
millions of records time out with default `cqlsh` settings. The
operations may actually succeed server-side while the client reports
a timeout.

**Lesson:** Use `nodetool cfstats` instead of `SELECT COUNT(*)` for
record count verification. For `DROP TABLE`, use
`cqlsh --request-timeout=300`. If a timed-out `DROP TABLE` is
re-executed and reports "table does not exist", the first drop
actually succeeded — the timeout was client-side only.

---

## 12. Next Steps

With Stage 1 complete, proceed to:

1. **Take `roi_warm` snapshot** via QEMU monitor
2. **Start Zipfian benchmark** with `scylla_bench` in run mode:

```bash
nohup ./scylla_bench --mode=run --records=5000000 \
    --threads=3 --cpus=5,6 --read-ratio=95 --zipfian-skew=0.99 \
    > /tmp/scylla_bench.log 2>&1 &
disown $!
```

3. **Verify steady state** with `tail -f /tmp/scylla_bench.log`
4. **Take `roi_running` snapshot** with benchmark actively sending
   requests
5. **Trace under TCG** with the plugin (`vcpus=1-4`)
6. **Validate traces** with `trace_inspector`

The tracing plugin arguments for ScyllaDB:

```bash
-plugin champsim_tracer.so,outdir=traces/,vcpus=1-4,limit=200000000
```

---

## 13. Directory Structure (Inside Guest)

```
~/
├── scylla_bench.c              # Benchmark client source
├── scylla_bench                # Compiled benchmark binary
├── cpp-driver/                 # DataStax C/C++ driver (build artifact)
│   └── build/
└── (ScyllaDB data lives in /var/lib/scylla/)
```

**ScyllaDB configuration files:**

```
/etc/default/scylla-server      # SCYLLA_ARGS
/etc/scylla/scylla.yaml         # Main config (listen address, ports, etc.)
/etc/scylla.d/
├── cpuset.conf                 # CPUSET="--cpuset=1-4 --smp=4"
├── dev.conf                    # DEV_MODE="--developer-mode=1"
└── mem.conf                    # MEM_CONF="--memory=8G"
```

**Systemd override:**

```
/etc/systemd/system/scylla-server.service.d/override.conf
    [Service]
    CPUAffinity=1 2 3 4
```