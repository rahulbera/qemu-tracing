# AArch64 Capture Kit (raw v3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement raw trace format v3 (arch byte, PA capture, value knob) across the QEMU TCG plugin and all three readers, plus the collaborator-facing AArch64 capture kit (probe/configure scripts + README).

**Architecture:** One plugin, v3-always emission, arch auto-detected from QEMU's `target_name`. PA capture is per-file all-or-nothing (default on). All three readers (inspector, filter, converter) gain version+arch whitelists and header-flag-driven framing. Capture kit = guest probe → host configure → generated `run_trace.sh` + `trace_metadata.txt` sidecar.

**Tech Stack:** C (QEMU plugin API ≥9.1, zstd), POSIX sh (guest probe), bash (host configure), QEMU 9.2.4 source at `~/softwares/qemu-9.2.4`.

**Spec:** `docs/superpowers/specs/2026-07-06-aarch64-capture-kit-design.md` — normative for all format/behavior questions.

## Global Constraints

- Raw v3 header (16 B): magic `0x46545343` u32 | version `3` u32 | vcpu_id u32 | arch u8 (`0`=x86_64, `1`=aarch64) | flags u8 (bit0 `has_pa`, bit1 `has_values`) | value_cap u8 | reserved u8 `0`. Bytes 12–15 decoded as **individual u8s**, never as a u32.
- Per-mem-op v3: `VA u64 | [PA u64 iff file has_pa] | size u8 | opflags u8 | [value size-bytes iff opflags bit1]`. opflags: bit0 write, bit1 has_value, bit2 pa_valid, bit3 pa_is_io. All multi-byte fields little-endian memcpy.
- `VALUE_API_CAP = 16` gates value extraction (**never** call `qemu_plugin_mem_get_value` for size > 16 — it is `g_assert_not_reached()` in QEMU 9.2 and aborts the VM). `MAX_VALUE_SIZE = 64` sizes buffers only.
- `STAGING_BUF_SIZE = 1024`, `#ifndef`-guarded, with a `_Static_assert` that it holds the format-ceiling worst case (4+8+15+7×(8+8+1+1+64) = 601).
- Plugin knobs: `arch=auto|x86_64|aarch64` (default auto), `capture_pa=on|off|1|0` (default **on**), `values=on|off|1|0` (default **on**). `values=off` ⇒ header has_values=0 AND value_cap=0 AND no per-op has_value.
- Privilege thresholds: x86_64 `0xFFFF800000000000`, aarch64 `0xFF00000000000000`.
- Readers whitelist version {2,3} and (v3 only) arch {0,1}; any other value = clean error naming the value and supported set. Per-op has_value in a has_values=0 file = corrupt-file error.
- Every reader sizes record buffers from the 64-byte format ceiling, never from value_cap.
- Scripts degrade with `KEY=UNKNOWN=<reason>`, never abort on missing sources.
- QEMU source: `~/softwares/qemu-9.2.4` (CLAUDE.md's `~/qemu-9.2.4` is stale). Local x86 QEMU: `~/qemu-custom/bin/qemu-system-x86_64`. zstd: `~/local/bin/zstd` (on PATH).
- Commit after every task. Scratch artifacts live in `/tmp/cstf/` (never committed; `traces/`, `dump/` are gitignored anyway).

---

### Task 1: Smoke-capture script + v2 reference artifacts

**Files:**
- Create: `plugin/tests/smoke_capture.sh`
- Artifacts (not committed): `/tmp/cstf/ref_v2/trace_vcpu0.raw.zst`

**Interfaces:**
- Produces: `plugin/tests/smoke_capture.sh <outdir> [extra-plugin-args]` — boots BIOS-only x86 QEMU under TCG for ≤20 s with the *current working-tree plugin build* (`plugin/champsim_tracer.so`), `vcpus=0,limit=200000`, writes `<outdir>/trace_vcpu0.raw.zst`. Exit 0 iff the trace file exists and is >1 KB. All later tasks use this for fast capture cycles.
- Produces: `/tmp/cstf/ref_v2/trace_vcpu0.raw.zst` — a **v2** reference trace captured with the *unmodified* plugin, used by Tasks 3–5 for backward-compat regression.

- [ ] **Step 1: Write the smoke script**

```bash
mkdir -p plugin/tests
cat > plugin/tests/smoke_capture.sh <<'EOF'
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
if [ -f "$F" ] && [ "$(stat -c%s "$F")" -gt 1024 ]; then
    echo "OK: $F ($(stat -c%s "$F") bytes)"
    exit 0
fi
echo "FAIL: trace missing or too small; see $OUTDIR/plugin_stderr.log"
exit 1
EOF
chmod +x plugin/tests/smoke_capture.sh
```

- [ ] **Step 2: Build the current (unmodified) plugin and capture the v2 reference**

```bash
make -C plugin plugin
mkdir -p /tmp/cstf
plugin/tests/smoke_capture.sh /tmp/cstf/ref_v2
```
Expected: `OK: /tmp/cstf/ref_v2/trace_vcpu0.raw.zst (...)`. (timeout exiting 124 is fine — the script judges by the file.)

- [ ] **Step 3: Verify the reference is well-formed v2**

```bash
make -C plugin inspector
plugin/trace_inspector /tmp/cstf/ref_v2/trace_vcpu0.raw.zst | head -8
```
Expected: `Format version: 2`, `Total instructions: 200000` (in the summary; run without `head` to see it).

- [ ] **Step 4: Preserve the pre-change inspector/filter binaries for cross-checks**

```bash
make -C plugin filter
cp plugin/trace_inspector /tmp/cstf/inspector_v2bin
cp plugin/trace_filter    /tmp/cstf/filter_v2bin
```

- [ ] **Step 5: Commit**

```bash
git add plugin/tests/smoke_capture.sh
git commit -m "Add BIOS-only smoke-capture script for plugin testing"
```

---

### Task 2: Plugin v3 core (`plugin/champsim_tracer.c`)

**Files:**
- Modify: `plugin/champsim_tracer.c` (constants ~84–104, `MemOpRecord` 131–137, `finalize_pending_insn` 316–367, `mem_cb` 479–519, `insn_exec_cb` line 475, `parse_args` 590–630, `plugin_atexit` 636–734, `qemu_plugin_install` 740–885, header doc comment 31–63)

**Interfaces:**
- Consumes: `plugin/tests/smoke_capture.sh` (Task 1).
- Produces: v3 trace files per Global Constraints; three test artifacts for Tasks 3–5: `/tmp/cstf/v3_default/`, `/tmp/cstf/v3_paoff/`, `/tmp/cstf/v3_valoff/` (each containing `trace_vcpu0.raw.zst`). New globals other steps reference: `int trace_arch` (0/1), `bool capture_pa`, `bool capture_values`, `uint64_t kernel_addr_thresh`, `const char *plugin_commit_str`.

- [ ] **Step 1: Constants, flags, arch ids, static assert**

Replace lines 84–104 region as follows. Old:
```c
#define TRACE_FORMAT_VER 2            /* v2: memory values */
...
#define MAX_VALUE_SIZE 16                 /* 128-bit max from API */
...
#define STAGING_BUF_SIZE 512
```
New (keep `PLUGIN_NAME`, `TRACE_FORMAT_MAGIC`, `MAX_VCPUS`, `MAX_INSN_SIZE`, `MAX_MEM_OPS`, `INPUT_BUF_SIZE`, `FLUSH_THRESHOLD`, `ZSTD_LEVEL`, `TRIGGER_CHECK_INTERVAL` unchanged):
```c
#define TRACE_FORMAT_VER 3 /* v3: arch byte, optional PA, value_cap */

/* Format ceiling for stored values: buffers and readers are sized for
 * this. The ChampSim v2 record's value slots are 64 bytes. */
#define MAX_VALUE_SIZE 64

/* Effective value-extraction cap: qemu_plugin_mem_get_value() tops out
 * at U128 in QEMU 9.2 and calling it for a wider access is
 * g_assert_not_reached() (plugins/api.c) — a hard VM abort. NEVER gate
 * extraction on MAX_VALUE_SIZE. Written to the header value_cap byte. */
#define VALUE_API_CAP 16

/* #ifndef so tests can compile a deliberately small buffer. */
#ifndef STAGING_BUF_SIZE
#define STAGING_BUF_SIZE 1024
#endif

/* File-header arch byte */
#define TRACE_ARCH_X86_64  0
#define TRACE_ARCH_AARCH64 1

/* File-header flags byte */
#define FILE_FLAG_HAS_PA     0x1
#define FILE_FLAG_HAS_VALUES 0x2

/* Per-mem-op flags byte */
#define MEMOP_FLAG_WRITE     0x1
#define MEMOP_FLAG_HAS_VALUE 0x2
#define MEMOP_FLAG_PA_VALID  0x4
#define MEMOP_FLAG_PA_IS_IO  0x8

/* Kernel/user split (privilege heuristic), selected by arch at install:
 * both are "top half of the VA space" tests.
 * x86_64: canonical split. aarch64: TTBR0 (user) is the low half for
 * every VA_BITS config (39/48/52), so one threshold covers them all. */
#define KERNEL_ADDR_THRESH_X86_64  0xFFFF800000000000ULL
#define KERNEL_ADDR_THRESH_AARCH64 0xFF00000000000000ULL

/* Compile-time guarantee: the staging buffer holds the format-ceiling
 * worst-case record. Fires if anyone grows MAX_VALUE_SIZE/MAX_MEM_OPS
 * without resizing, or compiles a test buffer too small. */
_Static_assert(STAGING_BUF_SIZE >=
                   4 + 8 + MAX_INSN_SIZE +
                       MAX_MEM_OPS * (8 + 8 + 1 + 1 + MAX_VALUE_SIZE),
               "STAGING_BUF_SIZE cannot hold worst-case v3 record");

#ifndef CSTF_COMMIT_STR
#define CSTF_COMMIT_STR "CSTF_COMMIT=unknown"
#endif
```
Delete the old `#define KERNEL_ADDR_THRESH 0xFFFF800000000000ULL` (line 104).

- [ ] **Step 2: `MemOpRecord` gains `paddr`**

At lines 131–137, change:
```c
typedef struct
{
    uint64_t address;
    uint64_t paddr;   /* v3: guest physical address (0 if !pa_valid) */
    uint8_t size;
    uint8_t flags; /* bit0 write, bit1 has_value, bit2 pa_valid, bit3 pa_is_io */
    uint8_t value[MAX_VALUE_SIZE];
} MemOpRecord;
```

- [ ] **Step 3: New globals**

After the existing globals block (line ~197, after `trigger_skipped_insns`), add:
```c
/* v3 configuration (resolved once in qemu_plugin_install, before any
 * vCPU callback can fire — lazy init would race multi-threaded TCG) */
static int trace_arch = -1;               /* TRACE_ARCH_*; -1 = unresolved */
static int arch_override = -1;            /* from arch= knob; -1 = auto */
static bool capture_pa = true;            /* capture_pa= knob */
static bool capture_values = true;        /* values= knob */
static uint64_t kernel_addr_thresh = 0;   /* set from trace_arch */
static const char plugin_commit_str[] = CSTF_COMMIT_STR;

/* v3 stats */
static uint64_t pa_invalid_count = 0;     /* hwaddr lookup returned NULL */
static uint64_t pa_io_count = 0;          /* MMIO accesses */
static uint64_t memops_dropped = 0;       /* ops beyond MAX_MEM_OPS */
```

- [ ] **Step 4: Privilege uses the runtime threshold**

Line 475, change:
```c
    vs->pending_privilege = (meta->vaddr >= kernel_addr_thresh) ? 1 : 0;
```

- [ ] **Step 5: `mem_cb` — PA capture, drop counter, re-pinned value gate**

Replace the body from line 500 (`if (vs->pending_num_mem_ops >= MAX_MEM_OPS)`) to line 518 (`vs->pending_num_mem_ops++;`) with:
```c
    if (vs->pending_num_mem_ops >= MAX_MEM_OPS)
    {
        memops_dropped++;
        return;
    }

    int idx = vs->pending_num_mem_ops;
    MemOpRecord *mop = &vs->pending_mem_ops[idx];

    mop->address = vaddr;
    mop->paddr = 0;
    mop->size = 1 << qemu_plugin_mem_size_shift(meminfo);
    mop->flags = qemu_plugin_mem_is_store(meminfo) ? MEMOP_FLAG_WRITE : 0x0;

    if (capture_pa)
    {
        /* hwaddr pointer is only valid inside this callback */
        struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(meminfo, vaddr);
        if (hw)
        {
            mop->paddr = qemu_plugin_hwaddr_phys_addr(hw);
            mop->flags |= MEMOP_FLAG_PA_VALID;
            if (qemu_plugin_hwaddr_is_io(hw))
            {
                mop->flags |= MEMOP_FLAG_PA_IS_IO;
                pa_io_count++;
            }
        }
        else
        {
            pa_invalid_count++;
        }
    }

    /* Gate on VALUE_API_CAP, NOT MAX_VALUE_SIZE: a wider
     * qemu_plugin_mem_get_value() call aborts the VM (see constants). */
    if (capture_values && mop->size <= VALUE_API_CAP)
    {
        memset(mop->value, 0, mop->size);
        extract_mem_value(meminfo, mop);
    }

    vs->pending_num_mem_ops++;
```

- [ ] **Step 6: `finalize_pending_insn` emits PA**

In the mem-op loop (lines 341–360), after `pos += 8;` for the address (line 346), insert:
```c
        if (capture_pa)
        {
            memcpy(staging + pos, &mop->paddr, 8);
            pos += 8;
        }
```
Also change the value-flag test on line 351 from `mop->flags & 0x2` to `mop->flags & MEMOP_FLAG_HAS_VALUE` (behavior identical).

- [ ] **Step 7: `parse_args` — three new knobs + updated usage**

Add a helper above `parse_args` (after `parse_vcpus`, line ~588):
```c
static bool parse_onoff(const char *val, bool *out)
{
    if (!strcmp(val, "on") || !strcmp(val, "1"))  { *out = true;  return true; }
    if (!strcmp(val, "off") || !strcmp(val, "0")) { *out = false; return true; }
    return false;
}
```
Inside `parse_args`, before the final `else` (line 618), add:
```c
        else if (g_str_has_prefix(arg, "arch="))
        {
            const char *v = arg + 5;
            if (!strcmp(v, "auto"))          arch_override = -1;
            else if (!strcmp(v, "x86_64"))   arch_override = TRACE_ARCH_X86_64;
            else if (!strcmp(v, "aarch64"))  arch_override = TRACE_ARCH_AARCH64;
            else
            {
                fprintf(stderr, "[%s] arch= must be auto|x86_64|aarch64\n",
                        PLUGIN_NAME);
                return false;
            }
        }
        else if (g_str_has_prefix(arg, "capture_pa="))
        {
            if (!parse_onoff(arg + 11, &capture_pa))
            {
                fprintf(stderr, "[%s] capture_pa= must be on|off|1|0\n",
                        PLUGIN_NAME);
                return false;
            }
        }
        else if (g_str_has_prefix(arg, "values="))
        {
            if (!parse_onoff(arg + 7, &capture_values))
            {
                fprintf(stderr, "[%s] values= must be on|off|1|0\n",
                        PLUGIN_NAME);
                return false;
            }
        }
```
Update the usage string (lines 621–624) to:
```c
            fprintf(stderr, "Usage: -plugin %s.so"
                            "[,outdir=<path>][,vcpus=<range>][,limit=<N>]"
                            "[,trigger=<host-file>][,arch=auto|x86_64|aarch64]"
                            "[,capture_pa=on|off][,values=on|off]\n",
                    PLUGIN_NAME);
```

- [ ] **Step 8: `qemu_plugin_install` — arch resolution, threshold, v3 header, banner**

After the `parse_args` call succeeds (line ~766), insert arch resolution:
```c
    /* Resolve arch: auto-detect from target_name, allow explicit override */
    int detected = -1;
    if (info->target_name)
    {
        if (!strcmp(info->target_name, "x86_64"))
            detected = TRACE_ARCH_X86_64;
        else if (!strcmp(info->target_name, "aarch64"))
            detected = TRACE_ARCH_AARCH64;
    }

    if (arch_override >= 0)
    {
        trace_arch = arch_override;
        if (detected >= 0 && detected != arch_override)
        {
            fprintf(stderr, "[%s] WARNING: arch= override (%s) contradicts "
                            "QEMU target (%s) — honoring the override\n",
                    PLUGIN_NAME,
                    arch_override == TRACE_ARCH_AARCH64 ? "aarch64" : "x86_64",
                    info->target_name);
        }
    }
    else if (detected >= 0)
    {
        trace_arch = detected;
    }
    else
    {
        fprintf(stderr, "[%s] ERROR: cannot determine arch from QEMU target "
                        "'%s'; pass arch=x86_64 or arch=aarch64 explicitly\n",
                PLUGIN_NAME, info->target_name ? info->target_name : "?");
        return -1;
    }

    kernel_addr_thresh = (trace_arch == TRACE_ARCH_AARCH64)
                             ? KERNEL_ADDR_THRESH_AARCH64
                             : KERNEL_ADDR_THRESH_X86_64;
```
In the configuration banner (after line 795), add:
```c
    fprintf(stderr, "[%s] Arch: %s | capture_pa: %s | values: %s "
                    "(value_cap=%d) | %s\n",
            PLUGIN_NAME,
            trace_arch == TRACE_ARCH_AARCH64 ? "aarch64" : "x86_64",
            capture_pa ? "on" : "off",
            capture_values ? "on" : "off",
            capture_values ? VALUE_API_CAP : 0,
            plugin_commit_str);
```
Replace the header write (lines 853–862) with:
```c
        /* Write v3 file header (16 bytes; bytes 12-15 are individual u8s) */
        uint32_t magic = TRACE_FORMAT_MAGIC;
        uint32_t version = TRACE_FORMAT_VER;
        uint32_t vid = i;
        uint8_t hdr_tail[4];
        hdr_tail[0] = (uint8_t)trace_arch;                       /* arch */
        hdr_tail[1] = (capture_pa ? FILE_FLAG_HAS_PA : 0) |      /* flags */
                      (capture_values ? FILE_FLAG_HAS_VALUES : 0);
        hdr_tail[2] = capture_values ? VALUE_API_CAP : 0;        /* value_cap */
        hdr_tail[3] = 0;                                         /* reserved */

        buffer_append(vs, &magic, 4);
        buffer_append(vs, &version, 4);
        buffer_append(vs, &vid, 4);
        buffer_append(vs, hdr_tail, 4);
```

- [ ] **Step 9: `plugin_atexit` — v3 stats**

After the per-vCPU loop's totals print (line ~725), add:
```c
    if (capture_pa)
    {
        fprintf(stderr, "[%s] PA capture: %" PRIu64 " invalid lookups, "
                        "%" PRIu64 " MMIO accesses\n",
                PLUGIN_NAME, pa_invalid_count, pa_io_count);
    }
    if (memops_dropped > 0)
    {
        fprintf(stderr, "[%s] WARNING: %" PRIu64 " mem ops dropped "
                        "(insns with >%d memory operands)\n",
                PLUGIN_NAME, memops_dropped, MAX_MEM_OPS);
    }
```

- [ ] **Step 10: Update the format doc comment (lines 31–63)**

Rewrite the header-comment format section to describe v3 exactly as in Global Constraints (header table incl. arch/flags/value_cap bytes; mem-op layout with conditional PA; opflags bits 0–3; the VALUE_API_CAP note replacing "For accesses > 16 bytes"). Also add the three new knobs to the usage comment at lines 17–28.

- [ ] **Step 11: Build**

```bash
make -C plugin plugin
```
Expected: `Built: champsim_tracer.so`, no warnings about our changes.

- [ ] **Step 12: Capture the three v3 test artifacts**

```bash
plugin/tests/smoke_capture.sh /tmp/cstf/v3_default
plugin/tests/smoke_capture.sh /tmp/cstf/v3_paoff  ",capture_pa=off"
plugin/tests/smoke_capture.sh /tmp/cstf/v3_valoff ",values=off"
grep -h "Arch:" /tmp/cstf/v3_*/plugin_stderr.log
```
Expected: three `OK:` lines; each stderr log shows `Arch: x86_64 | capture_pa: ... | values: ...` matching its config.

- [ ] **Step 13: Verify the v3 headers byte-for-byte**

```bash
for d in v3_default v3_paoff v3_valoff; do
  echo "== $d =="; zstd -dc /tmp/cstf/$d/trace_vcpu0.raw.zst | head -c 16 | xxd
done
```
Expected output (byte 12 onward is the point):
```
== v3_default ==
00000000: 4353 5446 0300 0000 0000 0000 0003 1000
== v3_paoff ==
00000000: 4353 5446 0300 0000 0000 0000 0002 1000
== v3_valoff ==
00000000: 4353 5446 0300 0000 0000 0000 0001 0000
```
(`4353 5446` = "CSTF"; version 3; vcpu 0; then arch=00, flags=03/02/01, value_cap=10₁₆=16 or 00, reserved 00.)

- [ ] **Step 14: Verify the arch-override warning fires**

```bash
plugin/tests/smoke_capture.sh /tmp/cstf/v3_archwarn ",arch=aarch64"
grep "WARNING: arch=" /tmp/cstf/v3_archwarn/plugin_stderr.log
```
Expected: the contradiction warning, and `Arch: aarch64` in the banner (override honored). Delete `/tmp/cstf/v3_archwarn` afterwards — its privilege bits are nonsense by construction.

- [ ] **Step 15: Verify the static assert guards small buffers**

```bash
gcc -O2 -Wall -shared -fPIC -DSTAGING_BUF_SIZE=32 \
    -I"$HOME/qemu-custom/include" \
    $(pkg-config --cflags glib-2.0 libzstd) \
    -o /tmp/cstf/should_fail.so plugin/champsim_tracer.c \
    $(pkg-config --libs glib-2.0) ; echo "exit=$?"
```
Expected: compile **error** containing `STAGING_BUF_SIZE cannot hold worst-case v3 record`; `exit=1`. (This strengthens spec §8 test 4: the guard is compile-time, so a too-small buffer cannot even build.)

- [ ] **Step 16: Commit**

```bash
git add plugin/champsim_tracer.c
git commit -m "Plugin: raw v3 format — arch auto-detect, PA capture, values knob

VALUE_API_CAP(16) split from MAX_VALUE_SIZE(64): extraction is gated on
the API cap because qemu_plugin_mem_get_value aborts on >128-bit
accesses. STAGING_BUF_SIZE 512->1024 with a compile-time worst-case
assert. Per-arch privilege thresholds resolved at install."
```

---

### Task 3: Inspector v3 support (`plugin/trace_inspector.c`)

**Files:**
- Modify: `plugin/trace_inspector.c` (header parse 298–324, mem-op loop 405–461, summary 482–511)

**Interfaces:**
- Consumes: `/tmp/cstf/{ref_v2,v3_default,v3_paoff,v3_valoff}/trace_vcpu0.raw.zst` (Tasks 1–2).
- Produces: inspector output fields later tasks grep: `Arch: x86_64|aarch64`, `PA capture: yes|no`, `Format version: 2|3`, and for aarch64 `sanity: instr_size!=4 in N/M records (X.XX%)`.

- [ ] **Step 1: Header parse — byte-granular decode + whitelists**

Replace lines 298–324 with:
```c
  /* Read file header: three u32s + four individual bytes (v3 fields
     must be decoded as u8s, never as a u32 — see spec 3.1) */
  uint32_t magic, version, vcpu_id;
  uint8_t  hdr_tail[4];
  if (!reader_read_exact(r, &magic, 4) || !reader_read_exact(r, &version, 4) ||
      !reader_read_exact(r, &vcpu_id, 4) ||
      !reader_read_exact(r, hdr_tail, 4)) {
    fprintf(stderr, "ERROR: Cannot read file header\n");
    reader_close(r);
    return 1;
  }

  if (magic != TRACE_FORMAT_MAGIC) {
    fprintf(stderr,
            "ERROR: Bad magic: 0x%08X (expected 0x%08X)\n",
            magic,
            TRACE_FORMAT_MAGIC);
    reader_close(r);
    return 1;
  }

  if (version != 2 && version != 3) {
    fprintf(stderr,
            "ERROR: Unsupported format version %u (supported: 2, 3)\n",
            version);
    reader_close(r);
    return 1;
  }

  uint8_t arch          = 0;     /* v2 files are x86_64 by definition */
  bool    file_has_pa   = false;
  bool    file_has_vals = true;  /* v2: value capture always on */
  uint8_t value_cap     = 16;

  if (version == 3) {
    arch          = hdr_tail[0];
    file_has_pa   = (hdr_tail[1] & 0x1) != 0;
    file_has_vals = (hdr_tail[1] & 0x2) != 0;
    value_cap     = hdr_tail[2];
    if (arch != 0 && arch != 1) {
      fprintf(stderr,
              "ERROR: Unknown arch byte %u (supported: 0=x86_64, 1=aarch64)\n",
              arch);
      reader_close(r);
      return 1;
    }
  }

  printf("=== Trace File: %s ===\n", filename);
  printf("Compression: %s\n", r->is_zstd ? "zstd" : "none");
  printf("Format version: %u\n", version);
  printf("vCPU ID: %u\n", vcpu_id);
  printf("Arch: %s\n", arch == 1 ? "aarch64" : "x86_64");
  printf("PA capture: %s\n", file_has_pa ? "yes" : "no");
  printf("Value capture: %s (cap %u bytes)\n",
         file_has_vals ? "enabled" : "disabled",
         value_cap);
  printf("\n");
```

- [ ] **Step 2: Stats declarations**

After `uint64_t mem_size_dist[7] = {0};` (line ~339), add:
```c
  uint64_t pa_valid_count  = 0;
  uint64_t pa_io_count     = 0;
  uint64_t isz_not4_count  = 0;   /* aarch64 sanity: non-A64-width insns */
```
And inside the main loop, right after the `nmem` validity check (line ~369), add:
```c
    if (arch == 1 && isz != 4) {
      isz_not4_count++;
    }
```

- [ ] **Step 3: Mem-op loop — conditional PA read + corrupt-file check**

Replace the per-mem-op reads (lines 405–420) with:
```c
    for (int m = 0; m < nmem; m++) {
      uint64_t maddr;
      uint64_t mpaddr = 0;
      uint8_t  msize;
      uint8_t  mflags;

      if (!reader_read_exact(r, &maddr, 8)) goto trunc_memop;
      if (file_has_pa && !reader_read_exact(r, &mpaddr, 8)) goto trunc_memop;
      if (!reader_read_exact(r, &msize, 1) ||
          !reader_read_exact(r, &mflags, 1)) {
      trunc_memop:
        fprintf(stderr,
                "ERROR: truncated mem op at insn #%" PRIu64 "\n",
                total_insns);
        goto done;
      }

      bool is_write  = (mflags & 0x1) != 0;
      bool has_value = (mflags & 0x2) != 0;
      bool pa_valid  = (mflags & 0x4) != 0;
      bool pa_is_io  = (mflags & 0x8) != 0;

      if (has_value && !file_has_vals) {
        fprintf(stderr,
                "ERROR: corrupt file — mem op sets has_value but header "
                "has_values=0 (insn #%" PRIu64 ")\n",
                total_insns);
        goto done;
      }
      if (pa_valid) pa_valid_count++;
      if (pa_is_io) pa_io_count++;
```
In the verbose print for mem ops (line ~431–433), extend:
```c
      if (verbose) {
        printf("  %s[%uB]@0x%" PRIx64, is_write ? "W" : "R", msize, maddr);
        if (file_has_pa) {
          printf(" PA=0x%" PRIx64 "%s%s",
                 mpaddr,
                 pa_valid ? "" : "[invalid]",
                 pa_is_io ? "[io]" : "");
        }
      }
```
(The value-read block at 435–453 is unchanged — `MAX_VALUE_SIZE` here is already 64.)

- [ ] **Step 4: Summary additions**

After the `Avg mem ops/insn` line (line ~500), add:
```c
  if (file_has_pa) {
    printf("PA valid:               %" PRIu64 " (%.1f%%)\n",
           pa_valid_count,
           total_mem_ops ? 100.0 * pa_valid_count / total_mem_ops : 0);
    printf("PA is MMIO:             %" PRIu64 "\n", pa_io_count);
  }
  if (arch == 1) {
    printf("sanity: instr_size!=4 in %" PRIu64 "/%" PRIu64
           " records (%.2f%%)\n",
           isz_not4_count,
           total_insns,
           total_insns ? 100.0 * isz_not4_count / total_insns : 0);
  }
```

- [ ] **Step 5: Build and validate against all four artifacts**

```bash
make -C plugin inspector
plugin/trace_inspector /tmp/cstf/ref_v2/trace_vcpu0.raw.zst     | grep -E "version|Arch|PA capture"
plugin/trace_inspector /tmp/cstf/v3_default/trace_vcpu0.raw.zst | grep -E "version|Arch|PA capture|PA valid"
plugin/trace_inspector /tmp/cstf/v3_paoff/trace_vcpu0.raw.zst   | grep -E "version|PA capture"
plugin/trace_inspector /tmp/cstf/v3_valoff/trace_vcpu0.raw.zst  | grep -E "version|Value capture"
```
Expected: v2 → `version: 2`, `Arch: x86_64`, `PA capture: no`; v3_default → `version: 3`, `PA capture: yes`, a `PA valid:` line with a high percentage; v3_paoff → `PA capture: no`; v3_valoff → `Value capture: disabled (cap 0 bytes)`. All four must reach the summary with **no truncation errors** (framing proof). Also spot-check verbose PA output: `plugin/trace_inspector -v -n 5 /tmp/cstf/v3_default/trace_vcpu0.raw.zst` shows `PA=0x...` per mem op.

- [ ] **Step 6: Whitelist rejection tests (synthetic bad headers)**

```bash
zstd -dc /tmp/cstf/v3_default/trace_vcpu0.raw.zst > /tmp/cstf/edit.raw
printf '\x04' | dd of=/tmp/cstf/edit.raw bs=1 seek=4 conv=notrunc 2>/dev/null
plugin/trace_inspector /tmp/cstf/edit.raw ; echo "exit=$?"
printf '\x03' | dd of=/tmp/cstf/edit.raw bs=1 seek=4 conv=notrunc 2>/dev/null
printf '\x02' | dd of=/tmp/cstf/edit.raw bs=1 seek=12 conv=notrunc 2>/dev/null
plugin/trace_inspector /tmp/cstf/edit.raw ; echo "exit=$?"
```
Expected: first run `ERROR: Unsupported format version 4 (supported: 2, 3)`, `exit=1`; second run `ERROR: Unknown arch byte 2 (supported: 0=x86_64, 1=aarch64)`, `exit=1`. (Readers auto-detect uncompressed `.raw` input, so no recompression needed.)

- [ ] **Step 7: Commit**

```bash
git add plugin/trace_inspector.c
git commit -m "Inspector: v3 support — arch/flags decode, PA fields, whitelists"
```

---

### Task 4: Filter v3 framing (`plugin/trace_filter.c`)

**Files:**
- Modify: `plugin/trace_filter.c` (header 499–534, rec_buf 564–565, mem-op loop 620–649, final report ~793–837)

**Interfaces:**
- Consumes: same four artifacts + `/tmp/cstf/edit.raw` technique from Task 3.
- Produces: filter that round-trips v2 and v3-x86 (identical filtering semantics — HLT heuristic untouched), errors on v3-aarch64.

- [ ] **Step 1: Header decode + whitelists + A64 policy**

After the existing version/vcpu extraction (line 520–522), add:
```c
  if (version != 2 && version != 3) {
    fprintf(stderr,
            "ERROR: unsupported format version %u (supported: 2, 3)\n",
            version);
    reader_close(r);
    if (w) writer_close(w);
    return 1;
  }

  uint8_t arch        = 0;
  bool    file_has_pa = false;
  if (version == 3) {
    arch        = file_hdr[12];
    file_has_pa = (file_hdr[13] & 0x1) != 0;
    if (arch == 1) {
      fprintf(stderr,
              "ERROR: arch=aarch64 — idle-loop filtering for AArch64 is not "
              "yet supported (WFI/WFE semantics and the HLT->HLT idle "
              "heuristic need validation on real A64 traces; see "
              "docs/superpowers/specs/2026-07-06-aarch64-capture-kit-design.md).\n"
              "Refusing to run rather than silently filtering nothing.\n");
      reader_close(r);
      if (w) writer_close(w);
      return 1;
    }
    if (arch != 0) {
      fprintf(stderr,
              "ERROR: unknown arch byte %u (supported: 0=x86_64, 1=aarch64)\n",
              arch);
      reader_close(r);
      if (w) writer_close(w);
      return 1;
    }
  }
```
Extend the existing log line (524–529) to include the mode:
```c
  fprintf(stderr,
          "[trace_filter] input=%s version=%u vcpu=%u arch=x86_64 "
          "has_pa=%d idle-opcode=HLT(0xF4)%s\n",
          input_path, version, vcpu_id, file_has_pa ? 1 : 0,
          stats_only ? " [stats-only]" : "");
```
The `writer_append(w, file_hdr, 16)` at line 533 stays — add above it:
```c
  /* NOTE: the 16-byte header is ECHOED verbatim, not regenerated. If a
     future option strips PAs, the flags byte must be rewritten here. */
```

- [ ] **Step 2: rec_buf sized for the v3 format ceiling**

Replace lines 564–565:
```c
  /* Scratch buffer for one record's serialized bytes — sized for the v3
     format-ceiling worst case (601 B) so a future value_cap>16 file
     needs no reader change (spec 3.4/4.5). */
  uint8_t  rec_buf[4 + 8 + MAX_INSN_SIZE +
                  MAX_MEM_OPS * (8 + 8 + 1 + 1 + MAX_VALUE_SIZE)];
```

- [ ] **Step 3: Mem-op loop — conditional 18-byte framing**

Replace lines 622–643 (the fused `mhdr[10]` block) with:
```c
    for (int m = 0; m < nmem; m++) {
      /* v2: VA(8)+size(1)+flags(1) = 10 bytes.
         v3 has_pa: VA(8)+PA(8)+size(1)+flags(1) = 18 bytes.
         Copied verbatim into rec_buf either way — framing only. */
      uint8_t mhdr[18];
      size_t  mlen = file_has_pa ? 18 : 10;
      if (!reader_read_exact(r, mhdr, mlen)) {
        truncated = true;
        break;
      }
      uint8_t msize  = mhdr[mlen - 2];
      uint8_t mflags = mhdr[mlen - 1];
      memcpy(rec_buf + rec_pos, mhdr, mlen);
      rec_pos += mlen;

      if (mflags & 0x2) {
        uint8_t vbytes = (msize <= MAX_VALUE_SIZE) ? msize : MAX_VALUE_SIZE;
        uint8_t valbuf[MAX_VALUE_SIZE];
        if (!reader_read_exact(r, valbuf, vbytes)) {
          truncated = true;
          break;
        }
        memcpy(rec_buf + rec_pos, valbuf, vbytes);
        rec_pos += vbytes;
      }
    }
```
Everything downstream (HLT heuristic at 658, state machine, buffering, re-emission) is untouched — v3-x86 filtering semantics are byte-for-byte v2 semantics per spec §4.3.

- [ ] **Step 4: Build and validate**

```bash
make -C plugin filter
plugin/trace_filter --stats-only /tmp/cstf/ref_v2/trace_vcpu0.raw.zst 2>&1 | tail -3
plugin/trace_filter --stats-only /tmp/cstf/v3_default/trace_vcpu0.raw.zst 2>&1 | tail -3
plugin/trace_filter --stats-only /tmp/cstf/v3_paoff/trace_vcpu0.raw.zst 2>&1 | tail -3
plugin/trace_filter /tmp/cstf/v3_default/trace_vcpu0.raw.zst /tmp/cstf/v3_filtered.raw.zst 2>&1 | tail -2
plugin/trace_inspector /tmp/cstf/v3_filtered.raw.zst | grep -E "version|Arch|PA capture|Total instructions"
```
Expected: all three stats runs complete with **no truncation errors** and matching in-counts (200000); the full run produces an output whose inspector parse shows `version: 3`, `PA capture: yes`, and instructions = 200000 − filtered (see the filter's own final report for the filtered count — BIOS traces may legitimately contain HLT).

- [ ] **Step 5: Rejection tests**

```bash
zstd -dc /tmp/cstf/v3_default/trace_vcpu0.raw.zst > /tmp/cstf/edit.raw
printf '\x01' | dd of=/tmp/cstf/edit.raw bs=1 seek=12 conv=notrunc 2>/dev/null
plugin/trace_filter --stats-only /tmp/cstf/edit.raw ; echo "exit=$?"
printf '\x04' | dd of=/tmp/cstf/edit.raw bs=1 seek=4 conv=notrunc 2>/dev/null
plugin/trace_filter --stats-only /tmp/cstf/edit.raw ; echo "exit=$?"
```
Expected: first → the AArch64 refusal message, `exit=1`; second → unsupported version 4, `exit=1`.

- [ ] **Step 6: Commit**

```bash
git add plugin/trace_filter.c
git commit -m "Filter: v3 framing (conditional PA), ceiling-sized buffer, A64 hard-error"
```

---

### Task 5: Converter v3-x86 read + PA pass-through (`converter/raw2champsim.c`)

**Files:**
- Modify: `converter/raw2champsim.c` (header parse 669–691, RawMemOp + mem-op loop 754–795, slot mapping 923–958)

**Interfaces:**
- Consumes: same artifacts.
- Produces: `.champsim.zst` files where `input_instr_v2.source_memory_pa[]` / `destination_memory_pa[]` carry real PAs for v3 captures (`pa_valid && !pa_is_io`), 0 otherwise. ChampSim record layout byte-offsets used in validation: `destination_memory_pa` at record offset 64 (2×u64), `source_memory_pa` at 80 (4×u64).

- [ ] **Step 1: Header parse — byte-granular + whitelists**

Replace lines 670–691 with:
```c
  /* Read file header: three u32s + four individual bytes */
  uint32_t magic, version, vcpu_id;
  uint8_t  hdr_tail[4];
  if (!reader_read_exact(reader, &magic, 4) ||
      !reader_read_exact(reader, &version, 4) ||
      !reader_read_exact(reader, &vcpu_id, 4) ||
      !reader_read_exact(reader, hdr_tail, 4)) {
    fprintf(stderr, "ERROR: Cannot read file header\n");
    reader_close(reader);
    return 1;
  }

  if (magic != TRACE_FORMAT_MAGIC) {
    fprintf(stderr, "ERROR: Bad magic: 0x%08X (expected 0x%08X)\n",
            magic, TRACE_FORMAT_MAGIC);
    reader_close(reader);
    return 1;
  }

  if (version != 2 && version != 3) {
    fprintf(stderr, "ERROR: Unsupported format version %u (supported: 2, 3)\n",
            version);
    reader_close(reader);
    return 1;
  }

  uint8_t arch          = 0;
  bool    file_has_pa   = false;
  bool    file_has_vals = true;
  if (version == 3) {
    arch          = hdr_tail[0];
    file_has_pa   = (hdr_tail[1] & 0x1) != 0;
    file_has_vals = (hdr_tail[1] & 0x2) != 0;
    if (arch == 1) {
      fprintf(stderr,
              "ERROR: arch=aarch64 — this converter decodes x86 only "
              "(Zydis). AArch64 (Capstone) support is the next spec; see "
              "docs/superpowers/specs/2026-07-06-aarch64-capture-kit-design.md\n");
      reader_close(reader);
      return 1;
    }
    if (arch != 0) {
      fprintf(stderr,
              "ERROR: Unknown arch byte %u (supported: 0=x86_64, 1=aarch64)\n",
              arch);
      reader_close(reader);
      return 1;
    }
  }

  printf("Input:   %s\n", input_file);
  printf("Output:  %s\n", output_file);
  printf("Format:  v%u, vCPU %u, arch x86_64, PA %s\n",
         version, vcpu_id, file_has_pa ? "captured" : "absent");
  printf("Compression level: %d\n", ZSTD_COMP_LEVEL);
  printf("\n");
```

- [ ] **Step 2: RawMemOp + mem-op read loop**

In the `RawMemOp` typedef (lines 755–763), add after `flags`:
```c
      uint64_t paddr;
      bool     pa_valid;
      bool     pa_is_io;
```
Replace the read block (lines 769–795) with:
```c
    for (int m = 0; m < nmem; m++) {
      mem_ops[m].paddr    = 0;
      mem_ops[m].pa_valid = false;
      mem_ops[m].pa_is_io = false;

      if (!reader_read_exact(reader, &mem_ops[m].addr, 8)) goto trunc_memop;
      if (file_has_pa &&
          !reader_read_exact(reader, &mem_ops[m].paddr, 8)) goto trunc_memop;
      if (!reader_read_exact(reader, &mem_ops[m].size, 1) ||
          !reader_read_exact(reader, &mem_ops[m].flags, 1)) {
      trunc_memop:
        fprintf(stderr, "ERROR: truncated mem op at insn #%" PRIu64 "\n",
                stats.total_insns);
        goto done;
      }

      mem_ops[m].is_write  = (mem_ops[m].flags & 0x1) != 0;
      mem_ops[m].has_value = (mem_ops[m].flags & 0x2) != 0;
      mem_ops[m].pa_valid  = (mem_ops[m].flags & 0x4) != 0;
      mem_ops[m].pa_is_io  = (mem_ops[m].flags & 0x8) != 0;
      mem_ops[m].value_len = 0;

      if (mem_ops[m].has_value && !file_has_vals) {
        fprintf(stderr,
                "ERROR: corrupt file — has_value set but header "
                "has_values=0 (insn #%" PRIu64 ")\n",
                stats.total_insns);
        goto done;
      }

      if (mem_ops[m].has_value) {
        uint8_t vbytes = mem_ops[m].size;
        if (vbytes > RAW_MAX_VALUE_SIZE) vbytes = RAW_MAX_VALUE_SIZE;
        if (!reader_read_exact(reader, mem_ops[m].value, vbytes)) {
          fprintf(stderr, "ERROR: truncated value at insn #%" PRIu64 "\n",
                  stats.total_insns);
          goto done;
        }
        mem_ops[m].value_len = vbytes;
      }

      if (mem_ops[m].is_write) num_writes++;
      else                     num_reads++;
    }
```

- [ ] **Step 3: PA pass-through in the slot mapping**

At line 932, replace the comment `/* PA: zero for now (raw traces don't capture PA) */` with:
```c
          if (mem_ops[m].pa_valid && !mem_ops[m].pa_is_io) {
            rec.destination_memory_pa[dst_mem_idx] = mem_ops[m].paddr;
          }
```
And symmetrically in the read branch, after `rec.source_memory_size[src_mem_idx] = ...` (line 946), add:
```c
          if (mem_ops[m].pa_valid && !mem_ops[m].pa_is_io) {
            rec.source_memory_pa[src_mem_idx] = mem_ops[m].paddr;
          }
```
(`rec` is memset to 0, so the else-case is already 0.)

- [ ] **Step 4: Build and validate — v2 regression + v3 conversion + PA spot-check**

```bash
make -C converter
converter/raw2champsim /tmp/cstf/ref_v2/trace_vcpu0.raw.zst /tmp/cstf/ref_v2.champsim.zst | head -4
converter/raw2champsim /tmp/cstf/v3_default/trace_vcpu0.raw.zst /tmp/cstf/v3.champsim.zst | head -4
python3 - <<'EOF'
import subprocess, struct
def pa_stats(path):
    raw = subprocess.run(["zstd","-dc",path],capture_output=True).stdout
    n = len(raw)//512; nz = 0
    for i in range(n):
        rec = raw[i*512:(i+1)*512]
        pas = struct.unpack_from("<6Q", rec, 64)   # dst_pa[2] + src_pa[4] at offset 64
        if any(pas): nz += 1
    print(f"{path}: {n} records, {nz} with nonzero PA")
pa_stats("/tmp/cstf/ref_v2.champsim.zst")
pa_stats("/tmp/cstf/v3.champsim.zst")
EOF
```
Expected: both conversions succeed with no errors; `ref_v2...: N records, 0 with nonzero PA`; `v3...: N records, <large nonzero count> with nonzero PA`.

- [ ] **Step 5: Rejection tests**

```bash
zstd -dc /tmp/cstf/v3_default/trace_vcpu0.raw.zst > /tmp/cstf/edit.raw
printf '\x01' | dd of=/tmp/cstf/edit.raw bs=1 seek=12 conv=notrunc 2>/dev/null
converter/raw2champsim /tmp/cstf/edit.raw /tmp/cstf/nope.champsim.zst ; echo "exit=$?"
printf '\x04' | dd of=/tmp/cstf/edit.raw bs=1 seek=4 conv=notrunc 2>/dev/null
converter/raw2champsim /tmp/cstf/edit.raw /tmp/cstf/nope.champsim.zst ; echo "exit=$?"
```
Expected: AArch64 refusal (`exit=1`), then version-4 refusal (`exit=1`).

- [ ] **Step 6: Commit**

```bash
git add converter/raw2champsim.c
git commit -m "Converter: read raw v3 (x86_64) — PA pass-through into ChampSim record

v3/aarch64 and unknown versions/arch bytes are clean errors. PAs land
in source/destination_memory_pa when pa_valid && !pa_is_io."
```

---

### Task 6: Build-time git-commit embedding

**Files:**
- Modify: `plugin/build_plugin.sh` (gcc invocation, lines 36–41)
- Modify: `plugin/Makefile` (plugin rule, lines 56–72)

**Interfaces:**
- Produces: `champsim_tracer.so` containing the literal string `CSTF_COMMIT=<sha>[-dirty]`, printed in the install banner (Task 2 wired the `CSTF_COMMIT_STR` fallback). `configure_tracer.sh` (Task 10) recovers it via `strings ... | grep '^CSTF_COMMIT='`.

- [ ] **Step 1: build_plugin.sh**

In `plugin/build_plugin.sh`, before the `gcc` command (line 36), add:
```bash
GIT_COMMIT="$(git -C "$(dirname "$0")" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [ "$GIT_COMMIT" != "unknown" ] && ! git -C "$(dirname "$0")" diff --quiet 2>/dev/null; then
    GIT_COMMIT="${GIT_COMMIT}-dirty"
fi
```
And add to the gcc flags (same command):
```bash
    -DCSTF_COMMIT_STR="\"CSTF_COMMIT=${GIT_COMMIT}\"" \
```

- [ ] **Step 2: Makefile**

In `plugin/Makefile`, above the `$(PLUGIN)` rule add:
```make
GIT_COMMIT := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
GIT_DIRTY  := $(shell git diff --quiet 2>/dev/null || echo -dirty)
```
and add to the plugin compile line (inside the `$(PLUGIN)` recipe, after `$(CFLAGS)`):
```make
		-DCSTF_COMMIT_STR='"CSTF_COMMIT=$(GIT_COMMIT)$(GIT_DIRTY)"' \
```

- [ ] **Step 3: Verify**

```bash
make -C plugin plugin
strings plugin/champsim_tracer.so | grep '^CSTF_COMMIT='
bash plugin/build_plugin.sh ~/softwares/qemu-9.2.4 && strings plugin/champsim_tracer.so | grep '^CSTF_COMMIT='
plugin/tests/smoke_capture.sh /tmp/cstf/banner && grep CSTF_COMMIT /tmp/cstf/banner/plugin_stderr.log
```
Expected: `CSTF_COMMIT=<12-hex>[-dirty]` from all three (string in .so via both build paths; printed in banner).

- [ ] **Step 4: Commit**

```bash
git add plugin/build_plugin.sh plugin/Makefile
git commit -m "Embed CSTF_COMMIT=<sha> into plugin .so at build time (both build paths)"
```

---

### Task 7: Local AArch64 QEMU + guest environment

**Files:**
- Create (not committed; environment): `~/softwares/qemu-9.2.4/build-aarch64/`, `/mnt/sherlock/rahbera/qemu-tracing/images/arm64-test/`

**Interfaces:**
- Produces: `~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64` (plugins enabled); a bootable ARM64 guest disk `arm64-test/guest.qcow2` reachable at `ssh -p 2223 ubuntu@localhost` (password `test1234`); UEFI firmware vars file `arm64-test/efivars.fd`. Boot helper `/mnt/sherlock/rahbera/qemu-tracing/images/arm64-test/boot_arm64.sh <extra-qemu-args>`.

- [ ] **Step 1: Build qemu-system-aarch64 (separate build dir; does not disturb the x86 build)**

```bash
cd ~/softwares/qemu-9.2.4 && mkdir -p build-aarch64 && cd build-aarch64
../configure --target-list=aarch64-softmmu --enable-plugins --enable-slirp \
             --disable-docs --disable-werror
make -j$(nproc)
./qemu-system-aarch64 --version | head -1
```
Expected: builds cleanly; version line `QEMU emulator version 9.2.4`. (~10–20 min.)

- [ ] **Step 2: Fetch guest image + firmware**

```bash
D=/mnt/sherlock/rahbera/qemu-tracing/images/arm64-test; mkdir -p $D; cd $D
wget -q https://cloud-images.ubuntu.com/releases/noble/release/ubuntu-24.04-server-cloudimg-arm64.img
qemu-img create -f qcow2 -b ubuntu-24.04-server-cloudimg-arm64.img -F qcow2 guest.qcow2 10G
cp ~/qemu-custom/share/qemu/edk2-aarch64-code.fd code.fd   # pre-verified present
truncate -s 64M efivars.fd
```
Expected: files present; `code.fd` is 64M ROM.

- [ ] **Step 3: cloud-init seed (password login on serial + ssh)**

```bash
cd /mnt/sherlock/rahbera/qemu-tracing/images/arm64-test
cat > user-data <<'EOF'
#cloud-config
password: test1234
chpasswd: { expire: false }
ssh_pwauth: true
EOF
echo "instance-id: arm64-test" > meta-data
# Use the first ISO tool available:
( command -v cloud-localds >/dev/null && cloud-localds seed.iso user-data meta-data ) || \
( command -v genisoimage  >/dev/null && genisoimage -output seed.iso -volid cidata -joliet -rock user-data meta-data ) || \
( command -v mkisofs      >/dev/null && mkisofs -output seed.iso -volid cidata -joliet -rock user-data meta-data ) || \
( command -v xorriso      >/dev/null && xorriso -as mkisofs -output seed.iso -volid cidata -joliet -rock user-data meta-data ) || \
echo "NO ISO TOOL — fall back to Alpine (see fallback below)"
```
**Fallback if no ISO tool exists:** use Alpine instead — `wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-virt-3.20.3-aarch64.iso`, boot it with `-cdrom` instead of the seed; it gives a passwordless root serial console. The probe script is POSIX-sh and runs there; transfer files via `wget http://10.0.2.2:8000/...` against a host-side `python3 -m http.server`.

- [ ] **Step 4: Boot helper script**

```bash
cat > /mnt/sherlock/rahbera/qemu-tracing/images/arm64-test/boot_arm64.sh <<'EOF'
#!/bin/bash
# boot_arm64.sh [extra qemu args...] — local ARM64 TCG test guest
D="$(cd "$(dirname "$0")" && pwd)"
Q=~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64
exec "$Q" \
    -machine virt -accel tcg,thread=multi -cpu cortex-a72 -smp 2 -m 2G \
    -drive if=pflash,format=raw,file="$D/code.fd",readonly=on \
    -drive if=pflash,format=raw,file="$D/efivars.fd" \
    -drive file="$D/guest.qcow2",format=qcow2,if=virtio \
    -drive file="$D/seed.iso",format=raw,if=virtio,readonly=on \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2223-:22 \
    -nographic -serial mon:stdio \
    "$@"
EOF
chmod +x /mnt/sherlock/rahbera/qemu-tracing/images/arm64-test/boot_arm64.sh
```

- [ ] **Step 5: First boot — verify login works**

```bash
/mnt/sherlock/rahbera/qemu-tracing/images/arm64-test/boot_arm64.sh
# Wait for cloud-init to finish (~5-10 min under TCG). Log in on the serial
# console as ubuntu/test1234. Then from another shell:
ssh -p 2223 ubuntu@localhost 'uname -m'
```
Expected: `aarch64`. Shut the guest down (`sudo poweroff`) or leave running for Task 8. Nothing to commit (environment only) — note completion in the task tracker.

---

### Task 8: AArch64 smoke capture + full reader matrix

**Files:**
- Artifacts: `/tmp/cstf/v3_arm/trace_vcpu0.raw.zst`

**Interfaces:**
- Consumes: Task 7 environment, Task 2 plugin, Tasks 3–5 readers.
- Produces: verified v3/aarch64 trace; the artifact Tasks 9–12 reuse.

- [ ] **Step 1: Capture during guest boot (no login needed — boot alone exercises user+kernel)**

```bash
mkdir -p /tmp/cstf/v3_arm
timeout 300 /mnt/sherlock/rahbera/qemu-tracing/images/arm64-test/boot_arm64.sh \
    -plugin /home/rahbera/qemu-tracing/plugin/champsim_tracer.so,outdir=/tmp/cstf/v3_arm,vcpus=0,limit=5000000 \
    2> /tmp/cstf/v3_arm/plugin_stderr.log
grep -E "Arch:|vCPU 0" /tmp/cstf/v3_arm/plugin_stderr.log
```
Expected: `Arch: aarch64 | capture_pa: on | values: on (value_cap=16) | CSTF_COMMIT=...`; a vCPU-0 stats line with 5000000 insns.

- [ ] **Step 2: Header + inspector validation**

```bash
zstd -dc /tmp/cstf/v3_arm/trace_vcpu0.raw.zst | head -c 16 | xxd
plugin/trace_inspector /tmp/cstf/v3_arm/trace_vcpu0.raw.zst | grep -E "Arch|PA capture|PA valid|Kernel mode|User mode|sanity"
```
Expected: header byte 12 = `01`, byte 13 = `03`, byte 14 = `10`; inspector shows `Arch: aarch64`, both user and kernel percentages nonzero (UEFI/kernel boot = mostly kernel; user appears once init starts — with a 5M limit mid-boot, kernel-heavy is fine as long as **both the U and K counters are plausible**, i.e. privilege isn't stuck at one value), `PA valid:` high percentage, and the `sanity: instr_size!=4` line at or near 0.00%.

- [ ] **Step 3: WFI opcode visible (idle instruction ground truth)**

```bash
plugin/trace_inspector -v /tmp/cstf/v3_arm/trace_vcpu0.raw.zst 2>/dev/null | grep -m3 "7f2003d5"
```
Expected: up to three verbose lines whose instruction bytes are `7f2003d5` (WFI). If none: the 5M window ended before first idle — rerun step 1 with `limit=20000000`; WFI must appear once the kernel idles.

- [ ] **Step 4: Filter and converter refuse it cleanly**

```bash
plugin/trace_filter --stats-only /tmp/cstf/v3_arm/trace_vcpu0.raw.zst ; echo "exit=$?"
converter/raw2champsim /tmp/cstf/v3_arm/trace_vcpu0.raw.zst /tmp/cstf/nope.champsim.zst ; echo "exit=$?"
```
Expected: both print their AArch64-unsupported message and `exit=1`.

- [ ] **Step 5: Commit checkpoint**

No repo files changed; record results (paste the inspector summary) in the task tracker / commit message of Task 9.

---

### Task 9: `probe_guest.sh`

**Files:**
- Create: `scripts/capture-kit/probe_guest.sh`

**Interfaces:**
- Produces: `guest_config.txt` with exactly these keys (every value either real or `UNKNOWN=<reason>`): `PROBE_VERSION`, `ARCH`, `KERNEL_VERSION`, `VA_BITS`, `PAGE_SIZE`, `SVE`, `SVE_VL`, `NCPU`, `CPU_MODEL_GUEST`, `ACCEL`. Task 10 consumes these keys by name.

- [ ] **Step 1: Write the script (POSIX sh — must run on busybox/dash too)**

```bash
mkdir -p scripts/capture-kit
cat > scripts/capture-kit/probe_guest.sh <<'PROBE_EOF'
#!/bin/sh
# probe_guest.sh — run INSIDE the guest VM. Collects the facts the
# capture pipeline and the offline modeling track need. Never fails:
# every key gets a value or UNKNOWN=<reason>.
#
# Usage:  sh probe_guest.sh > guest_config.txt
# Transfer to the guest with scp, or serve from the host:
#   host$ python3 -m http.server 8000
#   guest$ wget http://10.0.2.2:8000/probe_guest.sh

emit() { printf '%s=%s\n' "$1" "$2"; }

emit PROBE_VERSION 1
emit ARCH "$(uname -m 2>/dev/null || echo 'UNKNOWN=uname-failed')"
emit KERNEL_VERSION "$(uname -r 2>/dev/null || echo 'UNKNOWN=uname-failed')"

# --- VA_BITS (arm64) ---
VA=""
CFG="/boot/config-$(uname -r 2>/dev/null)"
if [ -r "$CFG" ]; then
    VA=$(grep '^CONFIG_ARM64_VA_BITS=' "$CFG" 2>/dev/null | cut -d= -f2)
fi
if [ -z "$VA" ] && [ -r /proc/config.gz ]; then
    VA=$(zcat /proc/config.gz 2>/dev/null | grep '^CONFIG_ARM64_VA_BITS=' | cut -d= -f2)
fi
if [ -z "$VA" ]; then
    # Fallback: infer from the top of the process stack mapping.
    TOP=$(grep '\[stack\]' /proc/self/maps 2>/dev/null | head -1 \
          | cut -d' ' -f1 | cut -d- -f2)
    case "$TOP" in
        0000ffff*|ffff*) VA=48 ;;
        0000007f*|7f*)   VA=39 ;;
        000fffff*)       VA=52 ;;
        *) VA="UNKNOWN=no-kernel-config-and-unrecognized-stack-top-$TOP" ;;
    esac
fi
emit VA_BITS "$VA"

# --- Page size ---
PS=$(getconf PAGE_SIZE 2>/dev/null) || PS="UNKNOWN=getconf-missing"
emit PAGE_SIZE "${PS:-UNKNOWN=getconf-empty}"

# --- SVE ---
FEAT=$(grep -m1 '^Features' /proc/cpuinfo 2>/dev/null)
case "$FEAT" in
    *" sve"*|*" sve "*|*sve2*) emit SVE yes ;;
    "") emit SVE "UNKNOWN=no-Features-line" ;;
    *)  emit SVE no ;;
esac
if [ -r /proc/sys/abi/sve_default_vector_length ]; then
    emit SVE_VL "$(cat /proc/sys/abi/sve_default_vector_length)"
else
    emit SVE_VL "UNKNOWN=no-sve-sysctl"
fi

# --- CPUs ---
emit NCPU "$(nproc 2>/dev/null || grep -c '^processor' /proc/cpuinfo 2>/dev/null || echo 'UNKNOWN=no-nproc')"
MODEL=$(grep -m1 -E '^(model name|CPU part)' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')
emit CPU_MODEL_GUEST "${MODEL:-UNKNOWN=no-cpuinfo-model}"

# --- Accelerator (informational provenance only) ---
ACCEL=""
if command -v systemd-detect-virt >/dev/null 2>&1; then
    V=$(systemd-detect-virt 2>/dev/null)
    case "$V" in
        kvm)  ACCEL=KVM ;;
        qemu) ACCEL=TCG ;;
    esac
fi
if [ -z "$ACCEL" ]; then
    if dmesg 2>/dev/null | grep -qiE 'kvm-clock|Hypervisor detected: KVM'; then
        ACCEL=KVM
    fi
fi
emit ACCEL "${ACCEL:-UNKNOWN=no-systemd-detect-virt-and-no-dmesg-signal}"
PROBE_EOF
chmod +x scripts/capture-kit/probe_guest.sh
```

- [ ] **Step 2: Degradation test on the x86 host (wrong machine on purpose)**

```bash
sh scripts/capture-kit/probe_guest.sh
```
Expected: 10 `KEY=value` lines, exit 0. `ARCH=x86_64`, `VA_BITS` either 48 (x86 stack-top heuristic coincidence) or `UNKNOWN=...` — the point is **no crash and every key present**:
```bash
sh scripts/capture-kit/probe_guest.sh | cut -d= -f1 | sort | tr '\n' ' '
```
Expected exactly: `ACCEL ARCH CPU_MODEL_GUEST KERNEL_VERSION NCPU PAGE_SIZE PROBE_VERSION SVE SVE_VL VA_BITS`

- [ ] **Step 3: Real test inside the ARM guest**

```bash
# host, from scripts/capture-kit/:
python3 -m http.server 8000 --directory scripts/capture-kit &
ssh -p 2223 ubuntu@localhost 'wget -q http://10.0.2.2:8000/probe_guest.sh && sh probe_guest.sh' \
    > /tmp/cstf/guest_config.txt
kill %1
cat /tmp/cstf/guest_config.txt
```
Expected: `ARCH=aarch64`, `VA_BITS=48` (Ubuntu noble arm64 default), `PAGE_SIZE=4096`, `SVE=no` (cortex-a72 has none), `NCPU=2`, `ACCEL=TCG`.

- [ ] **Step 4: Commit**

```bash
git add scripts/capture-kit/probe_guest.sh
git commit -m "capture-kit: guest probe script (POSIX sh, graceful degradation)"
```

---

### Task 10: `configure_tracer.sh`

**Files:**
- Create: `scripts/capture-kit/configure_tracer.sh`

**Interfaces:**
- Consumes: `guest_config.txt` (Task 9 key set), plugin `.so` with `CSTF_COMMIT=` string (Task 6).
- Produces: `run_trace.sh` (TCG+plugin invocation, `SNAPSHOT=` variable for optional `-loadvm`, copies sidecar into outdir before launch) and `trace_metadata.txt` with keys: all 10 probe keys verbatim, plus `QEMU_BIN`, `QEMU_VERSION`, `HOST_ARCH`, `CPU_MODEL_QEMU`, `MACHINE_QEMU`, `TRACE_ARCH`, `CAPTURE_PA`, `VALUES`, `VALUE_CAP`, `LIMIT`, `VCPUS`, `TRIGGER`, `OUTDIR`, `PLUGIN_SO`, `PLUGIN_COMMIT`, `GENERATED_AT`.

- [ ] **Step 1: Write the script**

```bash
cat > scripts/capture-kit/configure_tracer.sh <<'CONF_EOF'
#!/bin/bash
# configure_tracer.sh — run on the HOST. Turns a guest_config.txt (from
# probe_guest.sh) into a ready-to-run run_trace.sh plus a
# trace_metadata.txt provenance sidecar.
#
# Usage:
#   ./configure_tracer.sh <guest_config.txt> [outdir]
# Options via env:
#   QEMU=<path>       qemu-system-aarch64 binary   (default: from PATH)
#   PLUGIN=<path>     champsim_tracer.so           (default: ../../plugin/champsim_tracer.so)
#   VCPUS=<range>     vCPUs to trace               (default: 0)
#   LIMIT=<N>         insns per vCPU               (default: 1000000000)
set -u

GUEST_CFG="${1:?usage: configure_tracer.sh <guest_config.txt> [outdir]}"
OUTDIR="${2:-$PWD/traces_out}"
KIT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="${PLUGIN:-$KIT_DIR/../../plugin/champsim_tracer.so}"
QEMU="${QEMU:-$(command -v qemu-system-aarch64 || true)}"
VCPUS="${VCPUS:-0}"
LIMIT="${LIMIT:-1000000000}"
TRIGGER="/tmp/trace_start"

die() { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARNING: $*" >&2; }

[ -r "$GUEST_CFG" ] || die "cannot read $GUEST_CFG"

# --- QEMU checks ---
[ -n "$QEMU" ] && [ -x "$QEMU" ] || die "qemu-system-aarch64 not found.
Set QEMU=/path/to/qemu-system-aarch64 (must be built with --enable-plugins)."
QV_RAW="$("$QEMU" --version | head -1)"
QV=$(echo "$QV_RAW" | grep -oE '[0-9]+\.[0-9]+' | head -1)
QMAJ=${QV%%.*}; QMIN=${QV##*.}
if [ "$QMAJ" -lt 9 ] || { [ "$QMAJ" -eq 9 ] && [ "$QMIN" -lt 1 ]; }; then
    die "QEMU $QV is too old: the plugin's value capture needs >= 9.1
(qemu_plugin_mem_get_value). Upgrade QEMU or rebuild from source."
fi

# --- Plugin checks ---
[ -r "$PLUGIN" ] || die "plugin not found at $PLUGIN.
Build it first: cd <repo>/plugin && ./build_plugin.sh <qemu-source-tree>"
PLUGIN_COMMIT=$(strings "$PLUGIN" | grep -m1 '^CSTF_COMMIT=' | cut -d= -f2)
PLUGIN_COMMIT="${PLUGIN_COMMIT:-UNKNOWN=no-embedded-commit-string}"

# --- Running-guest CPU/machine discovery (best effort) ---
MAPS=$(pgrep -a -f qemu-system-aarch64 2>/dev/null | grep -v configure_tracer || true)
NGUEST=$(echo "$MAPS" | grep -c . || true)
CPU_MODEL_QEMU="UNKNOWN=no-running-guest"
MACHINE_QEMU="UNKNOWN=no-running-guest"
if [ "${NGUEST:-0}" -ge 1 ]; then
    LINE=$(echo "$MAPS" | head -1)
    [ "$NGUEST" -gt 1 ] && warn "multiple qemu-system-aarch64 processes; using the first"
    CPU_MODEL_QEMU=$(echo "$LINE" | grep -oE '\-cpu [^ ]+' | cut -d' ' -f2)
    MACHINE_QEMU=$(echo "$LINE" | grep -oE '\-machine [^ ]+' | cut -d' ' -f2)
    CPU_MODEL_QEMU="${CPU_MODEL_QEMU:-UNKNOWN=no--cpu-flag-visible}"
    MACHINE_QEMU="${MACHINE_QEMU:-UNKNOWN=no--machine-flag-visible}"
fi

mkdir -p "$OUTDIR"

# --- Sidecar ---
META="$KIT_DIR/trace_metadata.txt"
{
    cat "$GUEST_CFG"
    echo "QEMU_BIN=$QEMU"
    echo "QEMU_VERSION=$QV_RAW"
    echo "HOST_ARCH=$(uname -m)"
    echo "CPU_MODEL_QEMU=$CPU_MODEL_QEMU"
    echo "MACHINE_QEMU=$MACHINE_QEMU"
    echo "TRACE_ARCH=aarch64"
    echo "CAPTURE_PA=on"
    echo "VALUES=on"
    echo "VALUE_CAP=16"
    echo "LIMIT=$LIMIT"
    echo "VCPUS=$VCPUS"
    echo "TRIGGER=$TRIGGER"
    echo "OUTDIR=$OUTDIR"
    echo "PLUGIN_SO=$PLUGIN"
    echo "PLUGIN_COMMIT=$PLUGIN_COMMIT"
    echo "GENERATED_AT=$(date -Iseconds)"
} > "$META"

# --- run_trace.sh ---
RUN="$KIT_DIR/run_trace.sh"
cat > "$RUN" <<RUNEOF
#!/bin/bash
# run_trace.sh — GENERATED by configure_tracer.sh $(date -Iseconds)
# Traces vCPUs $VCPUS for $LIMIT insns each. Tracing is DORMANT until
# you run:   touch $TRIGGER     (on this host)
set -u

# ── Two-phase vs single-phase ────────────────────────────────────────
# SNAPSHOT="name"  -> restore a KVM-taken snapshot under TCG (two-phase).
#                     Run the KVM smoke test in the README FIRST.
# SNAPSHOT=""      -> cold boot under TCG (single-phase; always works).
SNAPSHOT=""

OUTDIR="$OUTDIR"
mkdir -p "\$OUTDIR"
cp "$META" "\$OUTDIR/trace_metadata.txt"   # sidecar travels with the traces

LOADVM=()
[ -n "\$SNAPSHOT" ] && LOADVM=(-loadvm "\$SNAPSHOT")

"$QEMU" \\
    -machine virt \\
    -accel tcg,thread=multi \\
    -smp 4 -m 4G \\
    -nographic -serial mon:stdio \\
    -plugin "$PLUGIN",outdir="\$OUTDIR",vcpus=$VCPUS,limit=$LIMIT,trigger=$TRIGGER,capture_pa=on,values=on \\
    "\${LOADVM[@]}" \\
    #
    # ─── YOUR BOOT FLAGS GO HERE (replace this comment block) ────────
    # Add your usual -cpu, -drive, pflash firmware, -nic, cloud-init
    # seed, etc. — everything you normally boot this guest with, minus
    # any -accel/-machine flags (already set above; -cpu note: TCG
    # cannot use 'host'; use a named model, e.g. -cpu cortex-a76).
    # ─────────────────────────────────────────────────────────────────
RUNEOF
chmod +x "$RUN"

echo "OK: wrote $RUN"
echo "OK: wrote $META"
echo "Next: edit the boot-flags block in run_trace.sh, then see README.md"
CONF_EOF
chmod +x scripts/capture-kit/configure_tracer.sh
```

- [ ] **Step 2: Validate on the host**

```bash
QEMU=~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64 \
PLUGIN=plugin/champsim_tracer.so \
scripts/capture-kit/configure_tracer.sh /tmp/cstf/guest_config.txt /tmp/cstf/kit_out
bash -n scripts/capture-kit/run_trace.sh && echo "run_trace.sh syntax OK"
grep -cE '=' scripts/capture-kit/trace_metadata.txt
grep -E "PLUGIN_COMMIT|VA_BITS|TRACE_ARCH" scripts/capture-kit/trace_metadata.txt
```
Expected: two `OK:` lines + the Next hint; syntax OK; ≥26 key lines; `PLUGIN_COMMIT=<sha>`, `VA_BITS=48`, `TRACE_ARCH=aarch64`.

- [ ] **Step 3: Failure-path checks**

```bash
QEMU=/bin/false scripts/capture-kit/configure_tracer.sh /tmp/cstf/guest_config.txt 2>&1 | head -2 ; echo "exit=$?"
scripts/capture-kit/configure_tracer.sh /nonexistent 2>&1 | head -1 ; echo "exit=$?"
```
Expected: version-check/QEMU error with fix instruction, exit 1; unreadable-config error, exit 1. (`/bin/false --version` produces no parsable version → the too-old/not-found path fires; either error text is acceptable as long as it exits 1 with instructions.)

- [ ] **Step 4: Commit**

```bash
git add scripts/capture-kit/configure_tracer.sh
git commit -m "capture-kit: host configure script — emits run_trace.sh + metadata sidecar"
```

---

### Task 11: Capture-kit README

**Files:**
- Create: `scripts/capture-kit/README.md`

**Interfaces:**
- Consumes: everything above. This is the collaborator's entry point.

- [ ] **Step 1: Write the README**

Write `scripts/capture-kit/README.md` with exactly these sections (prose to be written out fully — the content requirements per section):

1. **What this kit does** — three-paragraph overview: raw v3 format, per-vCPU `.raw.zst` outputs, the sidecar; link to the spec and `plugin/README.md`.
2. **Requirements** — QEMU ≥ 9.1 built with `--enable-plugins`; how to build the plugin against his QEMU source tree (`cd plugin && ./build_plugin.sh /path/to/qemu-src`); libzstd/glib dev packages.
3. **Step 1 — probe the guest**: transfer `probe_guest.sh` in (scp example + the `python3 -m http.server` / `wget http://10.0.2.2:8000/...` fallback), run `sh probe_guest.sh > guest_config.txt`, copy the file back out.
4. **Step 2 — configure on the host**: `./configure_tracer.sh guest_config.txt /path/for/traces`, env overrides (QEMU/PLUGIN/VCPUS/LIMIT), then *edit the marked boot-flags block* in the generated `run_trace.sh`.
5. **Step 3 — choose your flow**:
   - **Recommended: two-phase (KVM setup → TCG trace)** with the **mandatory smoke test first**: boot under KVM with a *named* CPU model (NOT `-cpu host` — TCG cannot restore it; try `-cpu max` or a named core your KVM accepts), take a trivial snapshot (`savevm smoketest` in the monitor — give the monitor telnet flag verbatim), quit, set `SNAPSHOT="smoketest"` in `run_trace.sh`, run it. If it restores: proceed with real workload setup under KVM. If it fails on CPU-model or device-state (GIC/arch-timer) errors: use single-phase; both failure modes are known ARM analogs of our x86 kvmclock story (link `docs/pipeline/kvmclock-patch-details.md`).
   - **Fallback: single-phase TCG** — `SNAPSHOT=""`, cold boot, reach steady state inside the guest (slow but certain).
6. **Step 4 — start tracing**: `touch /tmp/trace_start` on the host; what the plugin banner and per-vCPU lines look like; roughly what capture rates to expect under TCG.
7. **Step 5 — validate**: `plugin/trace_inspector <outdir>/trace_vcpu0.raw.zst`; a pasted example of GOOD output (from Task 8's real run: `Arch: aarch64`, `PA capture: yes`, user/kernel split, `PA valid` ≥90%, `sanity: instr_size!=4 ... (0.00%)`); what each red flag means (sanity ≠ 0 → 32-bit EL0 code present; PA valid low → tell us; version/arch errors → mixed-up binaries).
8. **Step 6 — ship**: send the whole `outdir/` (traces + `trace_metadata.txt` — the sidecar is copied there automatically by `run_trace.sh`).
9. **Troubleshooting** — plugin won't load (QEMU without plugins / version); `arch=` warning; `values=off`/`capture_pa=off` as performance levers (what each costs in fidelity); where the knobs are documented (`plugin/README.md`).

- [ ] **Step 2: Verify all referenced paths exist**

```bash
grep -oE '(scripts/capture-kit|plugin|docs/pipeline|docs/superpowers)/[A-Za-z0-9._/-]+' scripts/capture-kit/README.md | sort -u | while read f; do [ -e "$f" ] || echo "MISSING: $f"; done
```
Expected: no `MISSING:` lines.

- [ ] **Step 3: Commit**

```bash
git add scripts/capture-kit/README.md
git commit -m "capture-kit: collaborator README (two-phase flow + smoke test + TCG fallback)"
```

---

### Task 12: Kit end-to-end on the local ARM guest

**Files:** none (validation only; fixes go to the scripts with their own commits)

- [ ] **Step 1: Full sequence exactly as the README instructs**

Using the Task 7 guest: probe inside guest (Task 9 step 3 already produced `/tmp/cstf/guest_config.txt` — regenerate it following the README's own wording to catch doc drift), configure on host into `/tmp/cstf/e2e_out`, edit `run_trace.sh`'s boot-flags block with the `boot_arm64.sh` flags (pflash pair, guest.qcow2, seed.iso, `-cpu cortex-a72`, hostfwd 2223), set `LIMIT=2000000` via env at configure time.

- [ ] **Step 2: Run it**

```bash
scripts/capture-kit/run_trace.sh &
sleep 120   # guest boots dormant; plugin banner says DORMANT
touch /tmp/trace_start
# wait for the per-vCPU limit lines in stderr, then poweroff/kill
```
Expected: banner `DORMANT` → `TRIGGER DETECTED` after the touch → vCPU stats on exit.

- [ ] **Step 3: Validate the shipped directory**

```bash
ls /tmp/cstf/e2e_out
plugin/trace_inspector /tmp/cstf/e2e_out/trace_vcpu0.raw.zst | grep -E "Arch|version|PA"
grep -c . /tmp/cstf/e2e_out/trace_metadata.txt
```
Expected: `trace_vcpu0.raw.zst` **and** `trace_metadata.txt` present together; inspector clean; sidecar ≥26 lines.

- [ ] **Step 4: Commit any fixes surfaced**

Each fix goes to its owning file with message prefix `capture-kit e2e fix:`.

---

### Task 13: Documentation updates

**Files:**
- Modify: `plugin/README.md`, `CLAUDE.md`, `converter/README.md`, `scripts/README.md`, `docs/pipeline/README.md`, `docs/pipeline/boot-commands.md`

- [ ] **Step 1: `plugin/README.md`** — replace the v2 format description with v3 (header table + mem-op layout from Global Constraints), document `arch=`/`capture_pa=`/`values=` with defaults, the `value_cap` semantics (effective cap 16 vs ceiling 64 + the VM-abort rationale for the split), a "v2 files remain readable" note, and the `CSTF_COMMIT` banner line.
- [ ] **Step 2: `CLAUDE.md`** — rewrite the "Raw Trace Format (v2)" section as v3 (same content as above, condensed); fix `~/qemu-9.2.4` → `~/softwares/qemu-9.2.4` everywhere it appears.
- [ ] **Step 3: `converter/README.md`** — replace the "physical addresses (zero-filled — we don't have PA under QEMU/PIN yet)" bullet with: PA populated from v3 `capture_pa=on` traces (`pa_valid && !pa_is_io`), zero otherwise; input support: v2 and v3/x86_64; v3/aarch64 errors pending the Capstone spec.
- [ ] **Step 4: `scripts/README.md`** — add a `capture-kit/` entry: what it is, who it's for, pointer to its README; note the probe script is the one guest-side exception to "scripts/ is host-side".
- [ ] **Step 5: `docs/pipeline/README.md`** — add the capture kit and the v3 format bump to the index (one short paragraph each, linking spec + kit README).
- [ ] **Step 6: `docs/pipeline/boot-commands.md`** — one line under the plugin section: "AArch64 guests: use `scripts/capture-kit/` — do not hand-assemble the plugin knobs."
- [ ] **Step 7: Cross-check and commit**

```bash
grep -rn "qemu-9.2.4" --include="*.md" . | grep -v softwares | grep -v superpowers
grep -rln "zero-filled" converter/
```
Expected: first grep only hits historical spec/plan docs (fine) — no live docs; second: no hits.
```bash
git add plugin/README.md CLAUDE.md converter/README.md scripts/README.md docs/pipeline/README.md docs/pipeline/boot-commands.md
git commit -m "Docs: raw v3 format, new plugin knobs, capture kit, PA pass-through"
```

---

## Execution order & parallelism

Strictly ordered: 1 → 2 → {3, 4, 5 in any order} → 6 → 7 → 8 → 9 → 10 → 11 → 12 → 13. Task 7 (QEMU build ~15 min + guest boot ~10 min) can be started in the background any time after Task 1.

## Plan self-review record

- Spec coverage: D1→Task 2, D2→Task 3, D3→Task 4, D4→Task 5, D5→Tasks 9–12, D6→Task 13; §8 test 1→Tasks 2–5 steps, test 2→Task 8, test 3→Task 12, test 4→Task 2 step 15 (strengthened to compile-time), test 5→Tasks 3–5 rejection steps. Gap check: spec §4.1 exit stats→Task 2 step 9; §5.2 commit embedding→Task 6; smoke-test README mandate→Task 11 section 5. No gaps found.
- Deviation from spec (flagged): §8 test 4 anticipated a runtime bounds trip; the plan implements a `_Static_assert` (compile-time, strictly stronger) — a too-small buffer cannot build at all.
- Type/name consistency: `trace_arch`/`capture_pa`/`capture_values`/`kernel_addr_thresh` (Task 2) are referenced by name in Tasks 3–5 validations only via observable output, not linkage — no cross-file symbol coupling. Header byte offsets (12/13/14) and flag bits consistent across Tasks 2–5. `guest_config.txt` keys in Task 9 match `configure_tracer.sh` consumption in Task 10 (verbatim `cat`).
