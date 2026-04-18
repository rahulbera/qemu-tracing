# Boot Commands Quick Reference

## KVM Boot (normal, for setup and snapshot creation)

```bash
~/qemu-custom/bin/qemu-system-x86_64 \
    -accel kvm \
    -cpu host,-kvmclock \
    -smp 5 \
    -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -qmp tcp:127.0.0.1:4445,server,nowait
```

## KVM Boot + Load Snapshot

```bash
# Add: -loadvm roi_running (or roi_ready)
~/qemu-custom/bin/qemu-system-x86_64 \
    -accel kvm \
    -cpu host,-kvmclock \
    -smp 5 -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -loadvm roi_running
```

## TCG Boot + Plugin (THE TRACING RUN)

```bash
~/qemu-custom/bin/qemu-system-x86_64 \
    -accel tcg,thread=multi \
    -cpu qemu64 \
    -smp 5 \
    -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -plugin $HOME/qemu-tracing/plugin/champsim_tracer.so,outdir=$HOME/qemu-tracing/traces,vcpus=0-3,limit=1000000 \
    -loadvm roi_running
```

## SSH Into Guest

```bash
ssh -o ConnectTimeout=120 -p 2222 researcher@localhost
```

## QEMU Monitor (from host)

```bash
telnet localhost 4444
# Commands: savevm <name>, loadvm <name>, info snapshots, quit
# EXIT TELNET: Ctrl+] then type quit (DO NOT type quit at (qemu) prompt!)
```

## Kill QEMU (from serial console)

```
Ctrl+A then X
```

## List Snapshots

```bash
qemu-img snapshot -l ~/qemu-tracing/images/ubuntu-guest.qcow2
```

## Inspect Traces

```bash
# Summary
~/qemu-tracing/plugin/trace_inspector traces/trace_vcpu0.raw.zst

# Verbose first 20 instructions
~/qemu-tracing/plugin/trace_inspector -v -n 20 traces/trace_vcpu0.raw.zst
```

## Rebuild QEMU After Patching

```bash
cd ~/qemu-9.2.4/build
make -j$(nproc)
make install
```

## Rebuild Plugin

```bash
cd ~/qemu-tracing/plugin
bash build_plugin.sh ~/qemu-9.2.4
```
