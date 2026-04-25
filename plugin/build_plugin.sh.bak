#!/bin/bash
#
# Build the ChampSim raw tracer plugin for QEMU 9.2 TCG
#
# Usage:
#   ./build_plugin.sh [path-to-qemu-source]
#
# If no path is given, defaults to ~/qemu-9.2.4
#

set -e

QEMU_SRC="${1:-$HOME/qemu-9.2.4}"
PLUGIN_SRC="champsim_tracer.c"
PLUGIN_OUT="champsim_tracer.so"

# Verify QEMU source exists
if [ ! -f "${QEMU_SRC}/include/qemu/qemu-plugin.h" ]; then
    echo "ERROR: Cannot find qemu-plugin.h at ${QEMU_SRC}/include/qemu/"
    echo "Usage: $0 [path-to-qemu-source]"
    exit 1
fi

# Verify plugin source exists
if [ ! -f "${PLUGIN_SRC}" ]; then
    echo "ERROR: Cannot find ${PLUGIN_SRC} in current directory"
    exit 1
fi

echo "Building ${PLUGIN_OUT}..."
echo "  QEMU source: ${QEMU_SRC}"
echo "  Plugin source: ${PLUGIN_SRC}"

gcc -O2 -Wall -Wextra -Wno-unused-parameter \
    -shared -fPIC \
    -I"${QEMU_SRC}/include/qemu" \
    $(pkg-config --cflags glib-2.0) \
    -o "${PLUGIN_OUT}" "${PLUGIN_SRC}" \
    $(pkg-config --libs glib-2.0)

echo "Built successfully: $(pwd)/${PLUGIN_OUT}"
echo ""
echo "Test with:"
echo "  qemu-system-x86_64 -accel tcg \\"
echo "      -plugin $(pwd)/${PLUGIN_OUT},outdir=/tmp/traces,vcpus=0-3,limit=1000 \\"
echo "      ..."
