#!/usr/bin/env bash
#
# run_e2e.sh -- end-to-end AArch64 microbench validation of the
# raw2champsim pipeline (Task 4 / docs/superpowers/specs/2026-07-07-
# raw2champsim-aarch64-design.md S5.4; brief: .superpowers/sdd/task-4-brief.md).
#
# Exercises the FULL pipeline on real emulated execution of a known
# instruction mix:
#
#   QEMU (AArch64, bare-metal -kernel, no OS) + champsim_tracer.so plugin
#     -> raw v3/aarch64 trace
#     -> converter/raw2champsim (Capstone A64 backend)
#     -> ChampSim v2 records
#     -> converter/tests/props.py   (S5.2 register-scheme invariants)
#     -> converter/tests/mix_stats.py (known-mix oracle from microbench.S)
#
# Re-runnable: every step (re)creates its own scratch output.
#
# Usage: converter/tests/run_e2e.sh
#
# Env overrides (all optional):
#   QEMU_AARCH64   path to qemu-system-aarch64 built for aarch64-softmmu
#                  (default: ~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64)
#   SCRATCH        scratch directory (default: /tmp/cstf-a64)
#   INSN_LIMIT     plugin limit= knob, instructions traced on vCPU 0
#                  (default: 2000000 -- ~222k loop iterations, an
#                  overwhelming statistical signal for the aggregate mix,
#                  while keeping the re-runnable end-to-end time to a
#                  couple of minutes and the validators' decompressed
#                  footprint near 1 GB. The design spec S5.4 example and
#                  the authoritative run recorded in task-4-report.md use
#                  INSN_LIMIT=20000000; set that here to reproduce it --
#                  it converts in ~5 min and decompresses to ~10 GB, so
#                  props.py/mix_stats.py then need ~10 GB of free RAM.)
#   QEMU_TIMEOUT_S wall-clock timeout for the QEMU capture step (default: 120)
#   CONV_TIMEOUT_S wall-clock timeout for the converter step (default: 900;
#                  the per-instruction Capstone decode + 512-byte record
#                  write is the pipeline's slow step, ~14 us/instr, so
#                  ~30 s at the 2 M default and ~5 min at 20 M -- a
#                  converter throughput characteristic, not a correctness
#                  concern for this test)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TESTS_DIR="$SCRIPT_DIR"

QEMU_AARCH64="${QEMU_AARCH64:-$HOME/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64}"
SCRATCH="${SCRATCH:-/tmp/cstf-a64}"
INSN_LIMIT="${INSN_LIMIT:-2000000}"
QEMU_TIMEOUT_S="${QEMU_TIMEOUT_S:-120}"
CONV_TIMEOUT_S="${CONV_TIMEOUT_S:-900}"

PLUGIN_SO="$REPO_ROOT/plugin/champsim_tracer.so"
CONVERTER_BIN="$REPO_ROOT/converter/raw2champsim"

MICROBENCH_SRC="$TESTS_DIR/microbench.S"
MICROBENCH_BIN="$SCRATCH/microbench"
TRACE_DIR="$SCRATCH/mb"
RAW_TRACE="$TRACE_DIR/trace_vcpu0.raw.zst"
QEMU_STDERR_LOG="$SCRATCH/qemu_stderr.log"
SERIAL_LOG="$SCRATCH/serial.log"
CHAMPSIM_ZST="$SCRATCH/mb.champsim.zst"
CHAMPSIM_RAW="$SCRATCH/mb.champsim"
CONV_LOG="$SCRATCH/mb.conv.log"

# Load address for the bare-metal ELF: must land inside the `-M virt`
# RAM region (RAM base 0x40000000). Mirrors the common Linux Image
# text_offset convention (RAM base + 0x80000); any address in RAM works.
LOAD_ADDR=0x40080000

PASS=1
declare -a RESULTS

note()  { echo "== $* =="; }
check() {
  # check <label> <status: 0=pass>
  local label="$1" status="$2"
  if [ "$status" -eq 0 ]; then
    RESULTS+=("PASS  $label")
    echo "[PASS] $label"
  else
    RESULTS+=("FAIL  $label")
    echo "[FAIL] $label"
    PASS=0
  fi
}

# ================================================================
# Step 0: prerequisites + scratch dir
# ================================================================
note "Step 0: prerequisites"

if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  echo "BLOCKED: aarch64-linux-gnu-gcc not found on PATH" >&2
  exit 2
fi
if [ ! -x "$QEMU_AARCH64" ]; then
  echo "BLOCKED: AArch64 QEMU not found/executable at $QEMU_AARCH64" >&2
  echo "  (override with QEMU_AARCH64=...)" >&2
  exit 2
fi

mkdir -p "$SCRATCH"
rm -rf "$TRACE_DIR"
mkdir -p "$TRACE_DIR"

echo "Building plugin (make -C plugin plugin)..."
make -C "$REPO_ROOT/plugin" plugin >"$SCRATCH/plugin_build.log" 2>&1
if [ ! -f "$PLUGIN_SO" ]; then
  echo "BLOCKED: plugin build failed, see $SCRATCH/plugin_build.log" >&2
  exit 2
fi

echo "Building converter (make -C converter)..."
make -C "$REPO_ROOT/converter" >"$SCRATCH/converter_build.log" 2>&1
if [ ! -x "$CONVERTER_BIN" ]; then
  echo "BLOCKED: converter build failed, see $SCRATCH/converter_build.log" >&2
  exit 2
fi
echo "OK: plugin and converter built."
echo

# ================================================================
# Step 1: assemble the freestanding, static AArch64 microbench
# ================================================================
note "Step 1: assemble microbench.S (known instruction mix, see header comment)"

aarch64-linux-gnu-gcc -nostdlib -static -no-pie \
    -Wl,-Ttext="$LOAD_ADDR" -Wl,--build-id=none \
    -o "$MICROBENCH_BIN" "$MICROBENCH_SRC" 2>"$SCRATCH/asm.log"
ASM_RC=$?
check "assemble microbench.S -> static ELF" "$ASM_RC"
if [ "$ASM_RC" -ne 0 ]; then
  cat "$SCRATCH/asm.log" >&2
  echo "BLOCKED: could not assemble microbench.S" >&2
  exit 2
fi

ENTRY=$(aarch64-linux-gnu-readelf -h "$MICROBENCH_BIN" | awk '/Entry point/{print $NF}')
echo "microbench ELF entry point: $ENTRY (expect $LOAD_ADDR)"
echo

# ================================================================
# Step 2: capture under the AArch64 QEMU (bare-metal, -kernel, no OS)
# ================================================================
note "Step 2: capture trace (bare-metal QEMU virt board, plugin limit=$INSN_LIMIT)"
echo "QEMU: $QEMU_AARCH64"
echo "command:"
cat <<EOF
  $QEMU_AARCH64 -M virt -cpu cortex-a72 -m 128M -smp 1 \\
      -display none -monitor none -serial file:$SERIAL_LOG \\
      -kernel $MICROBENCH_BIN -no-reboot \\
      -plugin $PLUGIN_SO,outdir=$TRACE_DIR,vcpus=0,limit=$INSN_LIMIT,arch=aarch64
EOF

: > "$SERIAL_LOG"
timeout "${QEMU_TIMEOUT_S}s" "$QEMU_AARCH64" \
    -M virt -cpu cortex-a72 -m 128M -smp 1 \
    -display none -monitor none -serial "file:$SERIAL_LOG" \
    -kernel "$MICROBENCH_BIN" -no-reboot \
    -plugin "$PLUGIN_SO,outdir=$TRACE_DIR,vcpus=0,limit=$INSN_LIMIT,arch=aarch64" \
    2>"$QEMU_STDERR_LOG"
QEMU_RC=$?

echo "--- plugin banner / summary (stderr) ---"
cat "$QEMU_STDERR_LOG"
echo "----------------------------------------"

check "QEMU exited cleanly (rc=0, PSCI SYSTEM_OFF shutdown)" "$([ "$QEMU_RC" -eq 0 ] && echo 0 || echo 1)"
if [ "$QEMU_RC" -eq 124 ]; then
  echo "  QEMU was killed after ${QEMU_TIMEOUT_S}s (timeout) -- PSCI shutdown" >&2
  echo "  likely did not fire; see $SERIAL_LOG / $QEMU_STDERR_LOG" >&2
fi

grep -q "Arch: aarch64" "$QEMU_STDERR_LOG"
check "plugin banner shows Arch: aarch64" "$?"

[ -s "$RAW_TRACE" ]
check "raw trace file exists and is non-empty ($RAW_TRACE)" "$?"
echo

if [ ! -s "$RAW_TRACE" ]; then
  echo "BLOCKED: no raw trace produced; cannot continue to conversion." >&2
  exit 2
fi

# ================================================================
# Step 3: convert raw v3/aarch64 -> ChampSim v2 records
# ================================================================
note "Step 3: convert (converter/raw2champsim)"

timeout "${CONV_TIMEOUT_S}s" "$CONVERTER_BIN" "$RAW_TRACE" "$CHAMPSIM_ZST" \
    >"$CONV_LOG" 2>&1
CONV_RC=$?
check "converter exited 0" "$([ "$CONV_RC" -eq 0 ] && echo 0 || echo 1)"

echo "--- converter output ---"
cat "$CONV_LOG"
echo "-------------------------"

grep -q "^Format:.*arch aarch64" "$CONV_LOG"
check "converter reports 'Format: ... arch aarch64'" "$?"

DECODE_FAILURES=$(awk -F': *' '/Decode failures:/{print $2}' "$CONV_LOG" | tr -d '[:space:]')
echo "decode failures reported: ${DECODE_FAILURES:-<not found>}"
check "decode failures == 0" "$([ "${DECODE_FAILURES:-1}" = "0" ] && echo 0 || echo 1)"
echo

if [ ! -s "$CHAMPSIM_ZST" ]; then
  echo "BLOCKED: converter produced no output; cannot continue to validation." >&2
  exit 2
fi

zstd -f -q -d "$CHAMPSIM_ZST" -o "$CHAMPSIM_RAW"
check "decompress .champsim.zst" "$?"
echo

# ================================================================
# Step 4: validate against the S5.2 property checker + the known mix
# ================================================================
note "Step 4a: converter/tests/props.py (S5.2 register-scheme invariants)"
python3 "$TESTS_DIR/props.py" "$CHAMPSIM_RAW"
check "props.py: ALL PROPERTIES HOLD" "$?"
echo

note "Step 4b: converter/tests/mix_stats.py (microbench known-mix oracle)"
python3 "$TESTS_DIR/mix_stats.py" "$CHAMPSIM_RAW"
check "mix_stats.py: MIX OK" "$?"
echo

# ================================================================
# Verdict
# ================================================================
note "VERDICT"
for r in "${RESULTS[@]}"; do
  echo "  $r"
done

if [ "$PASS" -eq 1 ]; then
  echo
  echo "RESULT: PASS -- end-to-end AArch64 pipeline (plugin -> raw v3/aarch64 ->"
  echo "        converter -> ChampSim record) validated on real emulated"
  echo "        execution of a known instruction mix."
  exit 0
else
  echo
  echo "RESULT: FAIL -- see [FAIL] lines above."
  exit 1
fi
