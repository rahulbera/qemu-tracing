#!/bin/bash
#
# Boot VM under TCG with the ChampSim tracing plugin.
# Restores from the roi_ready snapshot.
#
# Usage:
#   ./boot_tcg_trace.sh [instruction_limit]
#
# Examples:
#   ./boot_tcg_trace.sh 1000000      # 1M instructions per vCPU (test)
#   ./boot_tcg_trace.sh 200000000    # 200M instructions per vCPU
#   ./boot_tcg_trace.sh 0            # unlimited (run until VM shutdown)
#   ./boot_tcg_trace.sh              # default: 1M instructions (test)
#

LIMIT="${1:-1000000}"
CKPTNAME="${2}"
IMGDIR="$HOME/qemu-tracing/images"
PLUGINDIR="$HOME/qemu-tracing/plugin"
TRACEDIR="$HOME/qemu-tracing/traces"
QEMUDIR="$HOME/qemu-custom/bin"

# Clean previous traces
rm -f ${TRACEDIR}/trace_vcpu*.raw.zst

echo "=========================================="
echo "  QEMU TCG Tracing Run"
echo "=========================================="
echo "  QEMU:        ${QEMUDIR}/qemu-system-x86_64"
echo "  Mode:        TCG (software emulation)"
echo "  Snapshot:    roi_ready"
echo "  Plugin:      champsim_tracer.so"
echo "  Tracing:     vCPUs 0-3"
echo "  Checkpoint:  ${CKPTNAME}"
echo "  Limit:       ${LIMIT} instructions/vCPU"
echo "  Output:      ${TRACEDIR}/"
echo "=========================================="
echo ""
echo "NOTE: Everything will be slow. This is expected under TCG."
echo "      SSH may take 10-30 seconds to respond after boot."
echo ""

${QEMUDIR}/qemu-system-x86_64 \
    -accel tcg,thread=multi \
    -cpu qemu64 \
    -smp 7 \
    -m 12G \
    -drive file=${IMGDIR}/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -plugin ${PLUGINDIR}/champsim_tracer.so,outdir=${TRACEDIR},vcpus=0-3,limit=${LIMIT} \
    -loadvm ${CKPTNAME}

echo ""
echo "QEMU has exited. Traces are in: ${TRACEDIR}/"
ls -lh ${TRACEDIR}/trace_vcpu*.raw.zst 2>/dev/null
