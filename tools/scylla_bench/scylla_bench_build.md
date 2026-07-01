# Building scylla_bench

## Prerequisites: Install DataStax C/C++ Driver

The benchmark client uses the DataStax C/C++ driver for CQL
communication with ScyllaDB. Build it from source inside the guest:

```bash
# Install build dependencies
sudo apt-get install -y cmake g++ make libuv1-dev libssl-dev

# Clone and build the driver
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

## Build scylla_bench

```bash
cd ~
gcc -O2 -o scylla_bench scylla_bench.c -lcassandra -lpthread -lm
```

If the linker can't find libcassandra, you may need:

```bash
gcc -O2 -o scylla_bench scylla_bench.c \
    -I/usr/local/include \
    -L/usr/local/lib \
    -lcassandra -lpthread -lm
```

## Verify

```bash
./scylla_bench --help
```

## Usage

### Stage 1: Load (replaces cassandra-stress)

```bash
# Ensure ycsb.usertable exists (create via cqlsh first)
./scylla_bench --mode=load --records=5000000 --threads=2 --cpus=0,5,6
```

### Stage 1: Warmup

```bash
./scylla_bench --mode=warmup --records=5000000 --threads=2 --cpus=0,5,6
```

### Stage 2: Benchmark (detached for snapshot survival)

```bash
nohup ./scylla_bench --mode=run \
    --records=5000000 \
    --read-ratio=95 \
    --zipfian-skew=0.99 \
    --threads=2 \
    --cpus=5,6 \
    > /tmp/scylla_bench.log 2>&1 &

# Verify it's running
ps aux | grep scylla_bench
tail -f /tmp/scylla_bench.log
```

### CPU Pinning

The `--cpus=` flag specifies which CPUs to pin worker threads to.
Threads round-robin over the list. For example, `--threads=4 --cpus=5,6`
pins thread 0 to CPU 5, thread 1 to CPU 6, thread 2 to CPU 5, thread 3
to CPU 6. This replaces the need for external `taskset` invocations.

### Key Format

Keys are `user0000001` through `user5000000` (zero-padded 7 digits).
Both load and run modes generate keys with the same format, so
there are no key mismatch issues.
