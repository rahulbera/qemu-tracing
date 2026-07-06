#!/bin/bash
#
# Build the ChampSim raw tracer plugin for QEMU 9.2 TCG
# Requires: libzstd-dev
#
# Usage:
#   ./build_plugin.sh [path-to-qemu-source]
#

set -e

QEMU_SRC="${1:-$HOME/softwares/qemu-9.2.4}"
PLUGIN_SRC="champsim_tracer.c"
PLUGIN_OUT="champsim_tracer.so"

if [ ! -f "${QEMU_SRC}/include/qemu/qemu-plugin.h" ]; then
    echo "ERROR: Cannot find qemu-plugin.h at ${QEMU_SRC}/include/qemu/"
    echo "Usage: $0 [path-to-qemu-source]"
    exit 1
fi

if [ ! -f "${PLUGIN_SRC}" ]; then
    echo "ERROR: Cannot find ${PLUGIN_SRC} in current directory"
    exit 1
fi

if ! pkg-config --exists libzstd 2>/dev/null; then
    echo "ERROR: libzstd not found. Install with: sudo apt install libzstd-dev"
    exit 1
fi

echo "Building ${PLUGIN_OUT}..."
echo "  QEMU source: ${QEMU_SRC}"
echo "  zstd: $(pkg-config --modversion libzstd)"

GIT_COMMIT="$(git -C "$(dirname "$0")" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [ "$GIT_COMMIT" != "unknown" ] && ! git -C "$(dirname "$0")" diff --quiet 2>/dev/null; then
    GIT_COMMIT="${GIT_COMMIT}-dirty"
fi

gcc -O2 -Wall -Wextra -Wno-unused-parameter \
    -shared -fPIC \
    -I"${QEMU_SRC}/include/qemu" \
    $(pkg-config --cflags glib-2.0 libzstd) \
    -DCSTF_COMMIT_STR="\"CSTF_COMMIT=${GIT_COMMIT}\"" \
    -o "${PLUGIN_OUT}" "${PLUGIN_SRC}" \
    $(pkg-config --libs glib-2.0 libzstd)

echo "Built successfully: $(pwd)/${PLUGIN_OUT}"
echo ""
echo "Usage modes:"
echo ""
echo "  Immediate tracing:"
echo "    -plugin $(pwd)/${PLUGIN_OUT},outdir=traces,vcpus=0-3,limit=200000000"
echo ""
echo "  Deferred tracing (with trigger):"
echo "    -plugin $(pwd)/${PLUGIN_OUT},outdir=traces,vcpus=0-3,limit=200000000,trigger=/tmp/trace_start"
echo "    # Then on the host when ready: touch /tmp/trace_start"
