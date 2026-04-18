#!/bin/bash
# Boot the guest VM with KVM acceleration
# Used for: setup, warmup, fast-forwarding to ROI

IMGDIR="$HOME/qemu-tracing/images"

qemu-system-x86_64 \
    -accel kvm \
    -cpu host \
    -smp 4 \
    -m 12G \
    -drive file=${IMGDIR}/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -qmp tcp:127.0.0.1:4445,server,nowait
