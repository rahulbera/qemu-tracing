#!/bin/bash
# Restores the guest VM checkpoint with KVM — 7 vCPUs
# vCPU 0:   OS / bootstrap (NOT traced)
# vCPU 1-4: ScyllaDB shards (traced)
# vCPU 5-6: Benchmark client + OS housekeeping

IMGDIR="$HOME/qemu-tracing/images"

/home/rahbera/qemu-custom/bin/qemu-system-x86_64 \
    -accel kvm \
    -cpu Haswell,\
kvmclock=off,\
kvmclock-stable-bit=off,\
kvm-asyncpf=off,\
kvm-steal-time=off,\
kvm-pv-eoi=off,\
kvm-pv-unhalt=off,\
kvm-poll-control=off,\
kvm-pv-ipi=off,\
kvm-pv-sched-yield=off,\
kvm-pv-tlb-flush=off,\
kvm-asyncpf-int=off,\
hle=off,\
rtm=off,\
pcid=off,\
invpcid=off,\
tsc-deadline=off \
    -smp 7 \
    -m 12G \
    -drive file=${IMGDIR}/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::9042-:9042 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -loadvm $1