#!/bin/bash
# smoke_capture.sh — fast plugin smoke: BIOS-only x86 boot under TCG.
# No disk, no OS: SeaBIOS alone executes >200k instructions, which is
# enough to exercise the record path and produce a valid trace file.
#
# Usage: smoke_capture.sh <outdir> [extra-plugin-args]
#   e.g. smoke_capture.sh /tmp/cstf/t1 ,capture_pa=off
set -u
OUTDIR="${1:?usage: smoke_capture.sh <outdir> [extra-plugin-args]}"
EXTRA="${2:-}"
QEMU="${QEMU:-$HOME/qemu-custom/bin/qemu-system-x86_64}"
PLUGIN_DIR="$(cd "$(dirname "$0")/.." && pwd)"

mkdir -p "$OUTDIR"
rm -f "$OUTDIR"/trace_vcpu*.raw.zst

timeout 20 "$QEMU" \
    -accel tcg -display none -nodefaults -machine pc -m 256 \
    -plugin "$PLUGIN_DIR/champsim_tracer.so,outdir=$OUTDIR,vcpus=0,limit=200000$EXTRA" \
    2> "$OUTDIR/plugin_stderr.log"

F="$OUTDIR/trace_vcpu0.raw.zst"
[ -f "$F" ] || F=$(ls "$OUTDIR"/trace_vcpu0_c*.raw.zst 2>/dev/null | head -1)
if [ -f "$F" ] && [ "$(stat -c%s "$F")" -gt 1024 ]; then
    echo "OK: $F ($(stat -c%s "$F") bytes)"
    exit 0
fi
echo "FAIL: trace missing or too small; see $OUTDIR/plugin_stderr.log"
exit 1
