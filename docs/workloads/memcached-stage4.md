# Stage 2: Restoring VM Checkpoint in TCG Mode and Start Tracing

## Overview

In this stage we will:
1. First restore the VM checkpoint in KVM mode
1. Then restore the VM checkpoint in TCG mode
2. Verify that both memcached and memtier survived the TCG restore
3. Verify that memcached is indeed processing requests and pinned
4. Signal the TCG plugin to start tracing

---

## Part 1: Restoring the Checkpoint in KVM Mode

Simply use the restore_kvm.sh script

```bash
cd ~/qemu-tracing/script
./restore_kvm.sh <checkpoint name>
```

Verify that both memcached and memtier survives the restore by SSH-ing to the VM and doing `htop`. Since this is still running in KVM mode, it will run in native speed.

Ensure that memcached is still pinned by running

```bash
MCPID=$(pgrep -x memcached)
for TID in $(sudo ls /proc/$MCPID/task/); do
    AFF=$(sudo taskset -p $TID 2>/dev/null | awk '{print $NF}')
    echo "Thread $TID affinity mask: $AFF"
done
```

---

## Part 2: Restoring the Checkpoint in TCG Mode

Simply use the boot_tcg_trace.sh script

```bash
cd ~/qemu-tracing/script
./boot_tcg_trace.sh <insts to trace> <checkpoint name>
```

Note that, simply booting in TCG mode will *NOT* start the tracing immediately. Instead, the plugin will start in a dormant mode. The plugin checks for the existence of the file `/tmp/trace_start` in the **HOST** to start the tracing. So the idea is to first restore the checkpoint in TCG mode, first verify that both memtier and memcached survived the restore, and then when everything looks good, create the `/tmp/trace_start` file in the host to send the signal to the TCG plugin to start tracing.

Do the following to ensure that memcached and memtier survived once you have booted in TCG mode

```bash
# Terminal 2: Wait ~30 seconds for VM to resume, then verify from host
echo "stats" | nc -q 1 localhost 11211 | grep -E "cmd_get|cmd_set"
sleep 5
echo "stats" | nc -q 1 localhost 11211 | grep -E "cmd_get|cmd_set"
# Numbers increasing? Good — trigger tracing:
touch /tmp/trace_start
```