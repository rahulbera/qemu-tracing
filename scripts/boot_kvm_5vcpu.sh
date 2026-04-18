#!/bin/bash
# Boot the guest VM with KVM — 5 vCPUs
# vCPU 0-3: Memcached worker threads
# vCPU 4:   YCSB + OS housekeeping

IMGDIR="$HOME/qemu-tracing/images"

/home/rahbera/qemu-custom/bin/qemu-system-x86_64 \
    -accel kvm \
    -cpu host,-kvmclock \
    -smp 5 \
    -m 12G \
    -drive file=${IMGDIR}/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -qmp tcp:127.0.0.1:4445,server,nowait
