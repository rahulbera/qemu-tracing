# AArch64 Capture Kit — Raw Trace Format v3 — Design Spec

**Date:** 2026-07-06
**Status:** Approved (design walkthrough approved by project owner; ARM-host
amendment incorporated; revised after 4-reviewer adversarial pass — see §11)
**Scope:** Capture-side only. The offline track (Capstone-based AArch64
support in `raw2champsim`, ChampSim configuration) is a separate,
subsequent spec written against the v3 format frozen here.

---

## 1. Motivation and context

A collaborator is generating traces from an **AArch64 guest VM** (on an
**AArch64 host**, so KVM is available to him) using this repo's QEMU TCG
plugin. Two capability gaps and one provisioning decision drive this work:

1. **Arch awareness.** The plugin's only x86-specific logic is the
   kernel/user privilege threshold. AArch64 needs a different threshold,
   and downstream tools need to know which ISA a trace contains so they
   can pick the right decoder (Zydis vs Capstone) and idle-instruction
   semantics (HLT vs WFI).
2. **Physical-address capture.** The QEMU plugin API can report guest
   physical addresses per memory access (`qemu_plugin_get_hwaddr`).
   Project decision: provision PA capture now, **enabled by default**,
   for future TLB/page-conflict/NUMA studies — even though the current
   study may not need it. PA is capture-time-only information; it cannot
   be recovered offline, which is why it must be in the format before
   the collaborator captures at scale.
3. **Wider value provision.** The trace record's value field grows to a
   64-byte format ceiling (matches the ChampSim v2 record's 64-byte
   value slots) so the format never needs to change when wider captures
   become possible. See §3.4 for the honest semantics of what is
   actually captured today.

The governing principle (owner's words): **commit once**. The raw format
is the interface contract between the collaborator's capture track and
our offline conversion/modeling track. This spec freezes that contract;
after the collaborator captures at scale, format changes mean expensive
re-captures.

A second principle: **make the collaborator's life easy**. He should not
need to understand VA_BITS, privilege thresholds, or QEMU plugin knobs.
A probe script collects facts; a configure script turns them into a
ready-to-run command and a provenance sidecar.

---

## 2. Deliverables

| # | Deliverable | Where |
|---|---|---|
| D1 | Plugin: v3 emission, arch auto-detect, PA capture, value knob, sizing fixes | `plugin/champsim_tracer.c` |
| D2 | `trace_inspector`: v3 + arch support, version/arch whitelist | `plugin/trace_inspector.c` |
| D3 | `trace_filter`: v3 framing, A64 hard-error, version/arch whitelist | `plugin/trace_filter.c` |
| D4 | `raw2champsim`: v3/x86_64 read support (framing + PA pass-through); clean error on v3/aarch64 | `converter/raw2champsim.c` |
| D5 | Capture kit: `probe_guest.sh`, `configure_tracer.sh`, README | `scripts/capture-kit/` |
| D6 | Doc updates | `plugin/README.md`, `CLAUDE.md`, `converter/README.md`, `scripts/README.md`, `docs/pipeline/README.md`, `docs/pipeline/boot-commands.md` |

Out of scope (next spec): AArch64 decode in `raw2champsim` (Capstone),
ChampSim AArch64 configuration and register/branch conventions, AArch64
idle-loop **filtering** (the HLT→HLT heuristic does not transfer blindly
to WFI/WFE/WFIT and needs validation against real A64 traces).

---

## 3. The frozen contract: raw trace format v3

### 3.1 File header (16 bytes, unchanged size)

```
Offset  Size  Field       Value / semantics
0       u32   magic       "CSTF" (0x46545343 little-endian)
4       u32   version     3
8       u32   vcpu_id
12      u8    arch        0 = x86_64, 1 = aarch64
13      u8    flags       bit0 has_pa      (mem-ops carry an 8-byte PA)
                          bit1 has_values  (value capture enabled; see below)
                          bits2-7 reserved, written 0
14      u8    value_cap   effective value-capture cap in bytes (see 3.4)
15      u8    reserved    0
```

Bytes 12–15 replace v2's reserved u32. Readers MUST decode them as
individual `uint8_t`s, not as bitfields of a u32 (endianness trap flagged
during design review). The three u32 fields keep the existing
host-little-endian memcpy convention.

**`has_pa`** mirrors the `capture_pa=` knob. **`has_values`** mirrors the
`values=` knob (§4.1). Invariants:

- When `has_values = 0`: `value_cap` MUST be written 0, and no mem-op in
  the file may set `opflags.has_value`. Readers MUST treat a per-op
  `has_value = 1` in such a file as a corrupt-file error.
- When `has_values = 1`: `value_cap` records the effective capture cap
  (16 with today's QEMU API — §3.4).

### 3.2 Per-instruction record

Unchanged from v2 except the per-mem-op layout:

```
[header u32]  bits[3:0] vcpu_id, bit[4] privilege, bits[8:5] instr_size,
              bits[11:9] num_mem_ops           (encoding unchanged)
[IP u64]
[instr bytes: instr_size bytes]     (A64: always 4; T32 EL0 code yields 2 —
                                     see §4.2 sanity line)
[mem ops x num_mem_ops]:
    VA      u64
    PA      u64      ONLY PRESENT when file-header flags.has_pa = 1
    size    u8
    opflags u8       bit0 write
                     bit1 has_value
                     bit2 pa_valid   (hwaddr lookup succeeded)
                     bit3 pa_is_io   (MMIO: PA is device-relative, not RAM)
    value   size bytes, present iff opflags.has_value
```

**Framing invariants (readers depend on these):**

- All multi-byte record fields (IP, VA, PA) are written with the same
  little-endian memcpy convention as the header u32s. PA occupies the 8
  bytes immediately following VA, before the size byte.
- `has_pa` is **per-file, all-or-nothing**. When set, the 8 PA bytes are
  present in *every* mem-op regardless of `pa_valid`/`pa_is_io`. Framing
  never depends on per-op bits. Failed hwaddr lookups write PA=0 with
  `pa_valid=0`.
- When file-header `flags.has_pa = 0`, opflags bits 2 and 3 MUST be
  written 0 and MUST be ignored by readers.
- When `opflags.has_value = 1`, exactly `size` value bytes follow the
  opflags byte; when `has_value = 0`, zero value bytes follow.
  `has_value` is only ever set when `size <= value_cap` (file header
  byte 14). A reader may size its buffers from the format ceiling (64)
  and validate against `value_cap`.
- Mem ops beyond `num_mem_ops`'s 3-bit cap (7) are dropped at capture
  (pre-existing v2 behavior, now counted — see §4.1 exit stats). SVE
  gather/scatter and LD64B/ST64B may plausibly hit this; QEMU is
  expected to report per-element callbacks for SVE, but the invariant
  holds regardless.

### 3.3 Version compatibility matrix

| File | plugin emits | inspector | filter | raw2champsim |
|---|---|---|---|---|
| v2 (existing x86 traces) | — (no longer emitted) | reads | reads (filters) | reads (converts) |
| v3, arch=x86_64 | yes (always, incl. x86) | reads | reads (filters, §4.3 semantics) | **reads (converts, PA passed through — §4.4)** |
| v3, arch=aarch64 | yes | reads | **clean error** (framing OK, filtering unsupported) | **clean error** (Capstone decode is next spec) |
| any other version | — | clean error | clean error | clean error |
| any other arch byte | — | clean error | clean error | clean error |

Rationale for v3-always (including x86): readers must learn v3 anyway;
a v2-emission compat mode would only add a test matrix. Existing v2
files on disk remain readable forever. **Because the plugin emits
v3-only from the moment it is rebuilt, `raw2champsim` gains v3/x86_64
read support in this spec (D4) — otherwise the owner's own x86
capture→convert pipeline would break until the next spec lands.**

### 3.4 `value_cap` semantics — the honest provision

`value_cap` records the **effective value-capture cap of the capturing
build**: the guarantee that no value in this file exceeds it.

- QEMU's `qemu_plugin_mem_get_value` API tops out at 128 bits (U128), so
  today's plugin writes `value_cap = 16`.
- The **format ceiling** is 64 bytes: `MAX_VALUE_SIZE`, reader buffers,
  and the ChampSim v2 record's value slots are all sized for 64. When a
  wider QEMU API exists, the plugin starts writing a larger `value_cap`
  — **no format change, no reader change** (readers are ceiling-sized;
  see §4.3 for the filter's buffer rule).
- Consequence for AArch64/SVE: accesses wider than 16 bytes currently
  get `has_value=0` (address and size still captured). If QEMU reports
  SVE element-wise (expected), elements are ≤8/16 bytes and values are
  captured normally.

Traceability: the owner-approved decision "`max_value_bytes = 64`" is
realized as the **format ceiling** (`MAX_VALUE_SIZE = 64` in all writers
and readers); the header `value_cap` byte separately records the
effective cap of the capturing build (16 today).

### 3.5 Privilege bit

Derived from the instruction VA's address-space half, selected by arch:

- x86_64: `vaddr >= 0xFFFF800000000000`
- aarch64: `vaddr >= 0xFF00000000000000`

The AArch64 test is **VA_BITS-independent**: user space (TTBR0) occupies
the low half from 0 on every 39/48/52-bit configuration; kernel (TTBR1)
the high half. The maximum user VA (0x000FFFFFFFFFFFFF at 52-bit) and
minimum kernel base (0xFFF0000000000000 at 52-bit) never approach the
threshold; the region between is a non-canonical hole where nothing
executes. Both tests are "top-half" checks (equivalently `vaddr >> 63`),
kept as named per-arch constants for documentation value. Note this is a
heuristic on both arches (correct for standard Linux; exotic mappings
excepted) — unchanged in spirit from v2.

---

## 4. Component design

### 4.1 Plugin (`plugin/champsim_tracer.c`)

Anchors verified against source (885 lines) during design review.

**New knobs** (added to `parse_args`, lines 590–630; usage string updated):

| Knob | Values | Default | Behavior |
|---|---|---|---|
| `arch=` | `auto` \| `x86_64` \| `aarch64` | `auto` | `auto` resolves from `qemu_info_t->target_name` at install (already available, line ~749). Explicit value overrides; mismatch with `target_name` logs a prominent warning. Unknown `target_name` under `auto` is a **fatal install error** (never guess the privilege threshold). |
| `capture_pa=` | `on` \| `off` \| `1` \| `0` | `on` | When off, the hwaddr call is **genuinely skipped** (it costs a TLB walk per access), not just the record bytes. Header `flags.has_pa` mirrors it. |
| `values=` | `on` \| `off` \| `1` \| `0` | `on` | When off, `qemu_plugin_mem_get_value` is never called, no mem-op sets `has_value`, header `flags.has_values = 0` and `value_cap = 0`. Provides a performance lever symmetric with the PIN tracer's `-values` knob. |

Existing knobs (`outdir=`, `vcpus=`, `limit=`, `trigger=`) and the
trigger/limit/vcpu mechanics (lines 98–101, 186–197, 208–223, 441–467,
482–503) are untouched.

**Header emission** (lines 853–862): version constant 2→3; the fourth
u32 (`reserved`) becomes four individual bytes: arch, flags,
value_cap, 0. `flags.has_pa` mirrors `capture_pa=`; `flags.has_values`
mirrors `values=`; `value_cap` = 16 when values on, 0 when off. Format
doc comment (lines 35–61) rewritten.

**PA capture** (in `mem_cb`, lines 479–519 — `meminfo` and `vaddr` are
in scope, which is exactly what the API needs):

```c
struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(meminfo, vaddr);
if (hw) {
    mop->paddr   = qemu_plugin_hwaddr_phys_addr(hw);
    mop->flags  |= MEMOP_FLAG_PA_VALID;
    if (qemu_plugin_hwaddr_is_io(hw)) mop->flags |= MEMOP_FLAG_PA_IS_IO;
} else {
    mop->paddr = 0;   /* pa_valid stays 0 */
}
```

The `hwaddr` pointer is only valid inside the callback — value is copied
out immediately.

**Value-extraction guard (CRITICAL — do not conflate with buffer size).**
The gate at line 512 currently reads `if (mop->size <= MAX_VALUE_SIZE)`,
where `MAX_VALUE_SIZE` (16) has been doing double duty as the API-cap
guard. With `MAX_VALUE_SIZE` growing to 64, that comparison MUST be
re-pinned to a new constant `VALUE_API_CAP = 16`: calling
`qemu_plugin_mem_get_value` on an access wider than 16 bytes executes
`g_assert_not_reached()` in QEMU 9.2 (`plugins/api.c:359-383`) — a hard
VM abort mid-capture, not a skipped value. QEMU's MemOp enum defines
sizes up to 128 bytes, so >16-byte accesses are representable and will
occur. After this change, `MAX_VALUE_SIZE` sizes buffers only;
`VALUE_API_CAP` gates extraction and is what gets written to the
header's `value_cap`.

**Privilege** (lines 103–104, 475): `KERNEL_ADDR_THRESH` becomes a
runtime `uint64_t kernel_addr_thresh` set once in
`qemu_plugin_install` after arch resolution — **before** any vCPU
callback can fire (multi-threaded TCG races out lazy init).

**Sizing (mandatory future-proofing; overflow is latent, not live):**

- `MAX_VALUE_SIZE` 16 → 64 (line 91) — buffer/format ceiling only, per
  the extraction-guard rule above.
- `STAGING_BUF_SIZE` 512 → 1024 (line 94), **plus a compile-time
  `_Static_assert`** guaranteeing the buffer holds the format-ceiling
  worst-case record (implemented as compile-time assert; stronger than
  the runtime check originally specified here — a build whose
  worst-case record exceeds `STAGING_BUF_SIZE` cannot compile at all,
  rather than failing at run time inside `finalize_pending_insn`). The
  v3 format-ceiling worst case is 4 + 8 + 15 + 7×(8+8+1+1+64) = **601
  bytes > 512**. With today's `VALUE_API_CAP = 16` the real worst case
  is 265 bytes, so the assert is satisfied by the shipped
  `STAGING_BUF_SIZE` — it exists to catch the day `value_cap` grows
  past 16 (or someone raises the cap without the buffer) at build time
  instead of at run time. Growing the buffer and adding the assert now,
  while we are staring at it, is the point.
- `MemOpRecord` (lines 131–137) gains `uint64_t paddr`; `value[]` grows
  to 64. Hot-path `memset` of the value buffer (line ~514) clears only
  `size` bytes, not the full 64.
- `STAGING_BUF_SIZE` becomes `#ifndef`-guarded so the test plan (§8
  test 4) can compile a deliberately small buffer to exercise the
  assert.

**Record emission** (`finalize_pending_insn`, lines 316–367): 8 PA bytes
copied immediately after the VA (line ~345) when `capture_pa` is on.

**New exit stats:** dropped-mem-ops counter (ops beyond `MAX_MEM_OPS`),
PA-invalid count, PA-is-IO count — printed at plugin exit alongside the
existing per-vCPU stats.

**Install-time banner:** log resolved arch + `target_name` +
`capture_pa` + `values` + `value_cap` + embedded git commit (§5.2) so
every run's stderr states the exact capture configuration.

### 4.2 `trace_inspector` (`plugin/trace_inspector.c`)

Today it validates magic only and parses everything as v2 — a v3 file
**silently desyncs** into plausible-looking garbage stats. Changes:

- Header parse (lines 298–324): decode version; accept 2 and 3, hard
  error otherwise. For v3, decode bytes 12/13/14 as u8s; whitelist the
  arch byte (§4.5); print arch (name, not number), has_pa, has_values,
  value_cap in the summary.
- Mem-op parse (lines 405–461): when file-level `has_pa`, read the
  8-byte PA between VA and size; verbose mode prints
  `PA=0x... [valid|invalid] [io]` per op.
- Corrupt-file check: per-op `has_value = 1` in a file with header
  `has_values = 0` is an error (§3.1 invariant).
- Summary additions: PA-valid percentage, IO-access count. For
  arch=aarch64 **only**, one sanity line in the fixed format
  `sanity: instr_size!=4 in N/M records (X.XX%)` — nonzero indicates
  T32/A32 EL0 code, worth knowing before conversion. No sanity line for
  x86_64 (no equivalent invariant exists).
- `MAX_VALUE_SIZE` is already 64 here (pre-existing inconsistency with
  the plugin's 16 — v3 makes them consistent).

### 4.3 `trace_filter` (`plugin/trace_filter.c`)

Same silent-desync exposure, plus a stack-overflow trap of its own:

- Header (lines 499–534): version whitelist {2,3}; arch whitelist
  (§4.5); decode arch/flags. The header is already echoed verbatim to
  output (line 533) which is correct — v3 bytes pass through unchanged.
  Add a comment stating the header is *echoed, not regenerated* (if a
  future option ever strips PAs, the flags byte must be rewritten — out
  of scope, documented).
- Record loop (lines 562–649): the fused 10-byte mem-op read
  (`mhdr`, size/flags at indices 8/9) becomes conditional — 18 bytes
  with size/flags at 16/17 when `has_pa`. `rec_buf` (line 564) is a
  fixed stack array sized for v2; it is resized to the **v3
  format-ceiling worst case (601 bytes)**, not merely v2+PA — matching
  the plugin's `STAGING_BUF_SIZE` rationale and §3.4's promise that a
  future `value_cap > 16` file needs no reader change. Records are
  copied and re-emitted byte-for-byte, so once parsing consumes the
  right byte count, output framing is automatically preserved for both
  has_pa states.
- **v3/x86_64 filtering semantics are byte-for-byte identical to v2:**
  the same HLT idle-loop heuristic (`isz==1 && byte==0xF4`, line 658),
  applied to the same fields; `has_pa` affects only record framing
  (bytes consumed and re-emitted), never a filtering decision. Given
  the same instruction stream, a v2 file and its v3-x86 equivalent must
  produce the same set of filtered records.
- **AArch64 policy:** if header arch = aarch64, exit with a clear error:
  idle-loop filtering for A64 is not yet supported (WFI = 0xD503207F,
  plus WFE/WFIT variants, and the HLT→HLT idle-iteration pattern itself
  needs validation on real A64 traces). Today's behavior on such a file
  — the x86 HLT test never fires on fixed-4-byte A64 — would be to
  "succeed" while filtering nothing, which is worse than an error.
- Final report prints which arch/idle-opcode mode was active.

### 4.4 `raw2champsim` (`converter/raw2champsim.c`)

Expanded from a pure version guard (adversarial-review finding: with the
plugin emitting v3-only, a guard alone would break the owner's existing
x86 capture→convert pipeline). Changes, all in the reader layer — Zydis
decode is untouched:

- Version whitelist {2, 3} inserted immediately after the magic-check
  block (lines 680–685, before the Format printf at line 689). Other
  versions: clean error.
- v3 header decode (bytes 12–14 as u8s). arch = aarch64: clean error
  naming this spec and the pending Capstone work. arch outside {0,1}:
  clean error (§4.5).
- v3 mem-op framing: read the 8-byte PA between VA and size when
  `has_pa`.
- **PA pass-through:** when `pa_valid = 1` and `pa_is_io = 0`, the PA is
  copied into the ChampSim v2 record's `source_memory_pa[]` /
  `destination_memory_pa[]` slots (which v2-era conversion zero-filled);
  otherwise 0 is written as before. This is the first time the
  converter's PA fields carry real data — `converter/README.md` updated
  accordingly (§6).
- Corrupt-file check per §3.1 (has_value in a has_values=0 file).

### 4.5 Shared reader discipline

All readers (inspector, filter, converter) use the same rules:

- magic → **version whitelist** → **arch whitelist** (values other than
  0/x86_64 and 1/aarch64 are a clean error naming the value and the
  supported set, exactly like an unknown version) → byte-granular
  header decode → framing driven *only* by header flags.
- No reader may infer layout from record content.
- All readers size record buffers from the format ceiling
  (`MAX_VALUE_SIZE = 64`), never from `value_cap`.

---

## 5. Capture kit (`scripts/capture-kit/`)

New subdirectory (the probe runs *inside the guest*, which breaks the
existing "scripts/ is host-side launchers" convention; a subdir with its
own README keeps that boundary clean).

### 5.1 `probe_guest.sh` — runs inside the AArch64 guest

Collects, with graceful degradation (every probe prints a value or
`UNKNOWN=<reason>`; the script never aborts on a missing source):

| Fact | Sources (in order) |
|---|---|
| VA_BITS | `/boot/config-$(uname -r)`, `/proc/config.gz`, `[stack]` top in `/proc/self/maps` |
| Page size | `getconf PAGE_SIZE` |
| SVE + vector length | `Features` in `/proc/cpuinfo`; `/proc/sys/abi/sve_default_vector_length` |
| Kernel version | `uname -r` |
| CPU count / model | `nproc`, `/proc/cpuinfo` |
| Accel currently in use | see below |

Accel detection (informational provenance only — nothing branches on
it): try `systemd-detect-virt` first (`kvm` → ACCEL=KVM, `qemu` →
ACCEL=TCG); fall back to grepping `dmesg` for `kvm-clock` /
"Hypervisor detected: KVM" (KVM if found; dmesg may be
permission-restricted); else `ACCEL=UNKNOWN=<reason>`.
`configure_tracer.sh` copies the field into the sidecar verbatim.

Output: `guest_config.txt`, key=value, one per line, transferable by
scp/paste.

### 5.2 `configure_tracer.sh` — runs on his host

Input: path to `guest_config.txt`. Gathers host facts itself: QEMU
binary path + version (`qemu-system-aarch64 --version`), host arch, and
the `-cpu`/`-machine` of a running guest via `ps` — if no
qemu-system-aarch64 process is running, the template's boot-flags block
is left as placeholder comments and the sidecar records
`CPU_MODEL=UNKNOWN=no-running-guest`; if multiple match, the first is
used and a warning lists the others.

Validation (fail loud, with fix instructions):
- QEMU ≥ 9.1 (required by `qemu_plugin_mem_get_value`); error otherwise.
- Plugin `.so` exists / points at build instructions if not.

Output:

1. **`run_trace.sh`** — contains the **TCG+plugin invocation only**,
   with the exact
   `-plugin champsim_tracer.so,outdir=...,vcpus=...,limit=...,trigger=/tmp/trace_start,capture_pa=on,values=on`
   string spliced into an annotated QEMU command template with a clearly
   marked block for his boot flags (machine/cpu/drive/nic). A
   `SNAPSHOT=` variable at the top controls an optional `-loadvm` flag:
   set it for the two-phase flow, leave empty for single-phase cold
   boot. The KVM setup phase reuses the collaborator's own boot command;
   the README gives the `savevm` monitor commands verbatim rather than
   generating a second script. All ports/paths parameterized at the top.
   Before launching QEMU, `run_trace.sh` copies `trace_metadata.txt`
   into the plugin `outdir`, so the shipped directory is
   `outdir/` containing `*.raw.zst` plus exactly one
   `trace_metadata.txt` — the offline track looks it up by that fixed
   name in the same directory as the traces.
2. **`trace_metadata.txt`** — the provenance sidecar, written next to
   `run_trace.sh` (and copied into `outdir` at run time as above): every
   probed guest fact + host facts + the full capture configuration
   (arch, capture_pa, values, value_cap, limit, vcpus, plugin git
   commit). This is how the offline track configures ChampSim months
   later without archaeology.

**Plugin git commit — build-time embedding:** `build_plugin.sh` bakes
`CSTF_COMMIT=<git rev-parse HEAD>[-dirty]` into the `.so` as a string
constant; the plugin prints it in the install banner;
`configure_tracer.sh` recovers it from the `.so` via `strings` (grep for
the `CSTF_COMMIT=` prefix) and writes `UNKNOWN=<reason>` if absent —
matching the probe script's degradation convention. Recording the commit
at configure time from a checkout would be wrong whenever the `.so` was
built from a different commit (or from a tarball with no `.git`).

### 5.3 `README.md` — the collaborator's document

Structure (his host is ARM, so KVM is available):

1. **Build the plugin** against his QEMU tree (`build_plugin.sh` /
   `make`), with the ≥9.1 requirement stated.
2. **Probe** (in guest) → **configure** (on host) → inspect the
   generated `run_trace.sh`.
3. **Recommended flow: two-phase (KVM setup → TCG trace)** — but with a
   **mandatory early smoke test**, before any workload investment:
   take a trivial snapshot under KVM (monitor `savevm` commands given
   verbatim), attempt the restore by setting `SNAPSHOT=` in
   `run_trace.sh`. Two known-unknowns on ARM, called out explicitly:
   - **CPU model:** KVM-on-ARM typically requires `-cpu host`, which
     TCG cannot restore. He must find a named model his KVM accepts
     (`-cpu max` aliases host; named models under KVM are
     hardware/QEMU-version dependent) or the two-phase flow is off the
     table. This is the ARM analog of our x86 Skylake-Client fix, but
     harder.
   - **Device state:** GIC/arch-timer snapshot sections may not restore
     under TCG (the ARM analog of our kvmclock problem; see
     `docs/pipeline/kvmclock-patch-details.md` for the x86 story and
     debugging pattern).
4. **Fallback flow: single-phase TCG** — boot the guest entirely under
   TCG (`SNAPSHOT=` empty), reach steady state (slow but certain),
   `touch /tmp/trace_start`. Works unconditionally; documented fully so
   a smoke-test failure never blocks him.
5. **Validate the capture:** `trace_inspector` on the first `.raw.zst`;
   what good output looks like (arch=aarch64, user/kernel mix, PA-valid
   percentage, instr_size uniformly 4 — and what a nonzero sanity line
   means).
6. **Ship the `outdir/`** (traces + `trace_metadata.txt` land there
   together automatically).

### 5.4 Kit design rules

- He should never need to hand-write a plugin knob.
- Every failure mode he can hit prints what to do next, not just what
  went wrong.
- Everything the offline track will need later must land in the
  metadata sidecar, and the sidecar must travel with the traces by
  construction (the `outdir` copy in §5.2), not by convention.

---

## 6. Documentation updates

| File | Change |
|---|---|
| `plugin/README.md` | v3 layout (replacing v2 as the current format), new knobs (`arch=`, `capture_pa=`, `values=`), value_cap semantics, v2-compat note |
| `CLAUDE.md` | Raw-format section rewritten for v3; note arch field. Also fix the stale `~/qemu-9.2.4` path (actual: `~/softwares/qemu-9.2.4`) |
| `converter/README.md` | Replace "PA zero-filled — we don't have PA" with: PA populated for v3 captures with `capture_pa=on`; v3/x86_64 supported, AArch64 pending (next spec) |
| `scripts/README.md` | Add capture-kit subdir entry |
| `docs/pipeline/README.md` | Index the capture kit; note v3 |
| `docs/pipeline/boot-commands.md` | One-line pointer to the capture kit for AArch64 |

---

## 7. Error handling summary

| Failure | Behavior |
|---|---|
| Unknown `target_name` under `arch=auto` | Fatal at plugin install (never guess a privilege threshold) |
| `arch=` override contradicts `target_name` | Prominent warning, honor the override |
| Memory access wider than `VALUE_API_CAP` (16) | `has_value=0`; extraction NOT attempted (a >16B `qemu_plugin_mem_get_value` call aborts the VM — §4.1) |
| `qemu_plugin_get_hwaddr` returns NULL | PA=0, `pa_valid=0`; counted, reported at exit |
| MMIO access | PA recorded as returned, `pa_is_io=1`; counted |
| Mem ops > 7 on one instruction | Dropped (pre-existing), now counted and reported |
| Staged record exceeds staging buffer | Compile-time `_Static_assert` failure (implemented as compile-time assert; stronger than the runtime check originally specified here — a build whose worst-case record exceeds the staging buffer cannot compile; was: silent stack smash) |
| Reader meets unknown version or arch byte | Clean error naming the value and the supported set |
| Reader meets v3 where unsupported (filter/A64, converter/A64) | Clean error naming this spec |
| Reader meets per-op has_value in a has_values=0 file | Corrupt-file error |
| Probe can't determine a fact | `UNKNOWN=<reason>` in output; never aborts |
| Configure finds QEMU < 9.1 | Error with upgrade instructions |

---

## 8. Testing plan (all local — no dependency on the collaborator)

1. **x86 regression.** Rebuild plugin; capture short v3 traces against
   the existing x86 setup in **three configurations**: defaults
   (`capture_pa=on, values=on`), `capture_pa=off`, and `values=off`.
   Verify: updated inspector parses all three (arch=0; PAs nonzero with
   `pa_valid` when on; header flags correct; no PA column when off);
   **old v2 files still parse** in inspector, filter, and converter;
   filter round-trips v2 and all three v3-x86 variants byte-identically
   outside filtered regions; **converter converts v3-x86 and the PA
   fields land in the ChampSim record** (spot-check nonzero
   `*_memory_pa` slots), and still converts v2 with zero-filled PA.
2. **AArch64 smoke, locally.** Add `aarch64-softmmu` to the
   `~/softwares/qemu-9.2.4` build (note: CLAUDE.md's `~/qemu-9.2.4`
   path is stale); boot an Ubuntu ARM64 cloud image under TCG on this
   x86 host; attach the plugin. Verify: arch byte=1; plausible
   user/kernel privilege mix; PA-valid on RAM accesses; WFI
   (`7F 20 03 D5`) visible in verbose inspector output; instr_size
   uniformly 4 (sanity line 0%); filter and converter error cleanly on
   the file.
3. **Kit end-to-end.** Run `probe_guest.sh` inside the local ARM guest;
   `configure_tracer.sh` on the host; execute the generated
   `run_trace.sh`; confirm the trace validates, and `outdir/` contains
   the traces plus `trace_metadata.txt` with every field populated or
   explicitly `UNKNOWN`.
4. **Overflow guard.** Compile a debug plugin with
   `-DSTAGING_BUF_SIZE=256` (the constant is `#ifndef`-guarded for this
   purpose — §4.1). This build is expected to **fail compilation** with
   the `_Static_assert` message ("STAGING_BUF_SIZE cannot hold
   worst-case v3 record") — that compile failure is the test passing
   (implemented as compile-time assert; stronger than the runtime check
   originally specified here, which would have required running a real
   capture to trip a bounds check at run time). (With the production
   cap of `VALUE_API_CAP=16`, real records max at 265 bytes, so the
   shipped `STAGING_BUF_SIZE=1024` compiles fine — the assert only
   fires for a deliberately undersized build like this test's 256.)
5. **Reader whitelists.** Hex-edit a copy of a v3 header to version 4
   and to arch 2; confirm all three readers produce clean errors naming
   the value and supported set. Run one capture with a deliberately
   mismatching `arch=` override and confirm the warning fires and the
   override is honored.

What we cannot test locally: KVM-on-ARM snapshot→TCG restore (needs his
hardware). That is exactly why the README's step 3 makes him smoke-test
it first at trivial cost.

---

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Two-phase KVM flow fails on his host (CPU model and/or GIC/timer state) | Mandatory early smoke test; fully documented single-phase fallback; nothing else in the kit depends on which flow he uses |
| SVE values wider than 16B silently uncaptured | By design: `value_cap` in header is the truth; addresses/sizes always captured; §3.4 documents it. Extraction guard (§4.1) makes wide accesses safe, not fatal |
| Per-access hwaddr lookup slows TCG | `capture_pa=off` genuinely skips the call; overhead noted in kit README; default stays on per project decision |
| Three readers drift out of lockstep | §4.5 shared discipline; testing plan steps 1/2/5 exercise all three on the same files |
| Collaborator's QEMU too old / missing plugin support | configure_tracer.sh hard checks; README build section states requirements |
| 32-bit EL0 (T32/A32) code in the guest | instr_size 2/4 both representable; inspector's sanity line surfaces it; converter-track concern otherwise |

---

## 10. Decisions log (for traceability)

Owner decisions (from design conversation):

- v3 16-byte header with arch/flags/value_cap bytes in the old reserved
  word — approved as presented.
- Format value ceiling 64 bytes ("max_value_bytes=64") — realized per
  §3.4 traceability note.
- `capture_pa` **default on** (owner decision, future-proofing).
- Per-file all-or-nothing PA framing; per-op `pa_valid`/`pa_is_io` bits.
- Arch auto-detected from `target_name`; knob is an override only.
- Two scripts (guest probe + host configure); collaborator never
  hand-writes a knob.
- Collaborator host is AArch64 (KVM available): README leads two-phase
  with mandatory smoke test, single-phase TCG documented as fallback.
- Capture-kit-only scope; converter/ChampSim AArch64 work is the next
  spec.

Engineering additions made during spec writing (flagged for owner
visibility; each has rationale in the body):

- **v3-always emission** (including x86) — and, as its consequence,
  **`raw2champsim` v3/x86_64 read support with PA pass-through in this
  spec** (§3.3, §4.4): without it, rebuilding the plugin would break
  the owner's own x86 capture→convert pipeline.
- **`values=` knob** (§4.1): makes header bit1 well-defined instead of
  a dead provision; symmetric with the PIN tracer's `-values`.
- **`VALUE_API_CAP` split from `MAX_VALUE_SIZE`** (§4.1): mandatory —
  the alternative is a `g_assert_not_reached()` VM abort on the first
  >16-byte access.
- **Filter hard-error on A64** (§4.3): silently filtering nothing would
  be worse than an error.
- **Reader arch whitelist** (§4.5), **ceiling-sized reader buffers**
  (§4.3, §4.5), **sidecar-travels-with-traces mechanism** (§5.2),
  **build-time git-commit embedding** (§5.2).

## 11. Adversarial review record

Reviewed 2026-07-06 by four independent reviewers (internal consistency,
decision fidelity, source implementability, ambiguity). 28 findings — 2
blockers (the `VALUE_API_CAP` VM-abort trap; undefined `has_values`
semantics), 15 should-fix, 11 nits — all incorporated in this revision.
Notable source-verified facts: `qemu_plugin_mem_get_value` asserts on
>16-byte accesses (QEMU 9.2 `plugins/api.c:359-383`); all plugin line
anchors confirmed against source; `raw2champsim` version-guard insertion
point confirmed at `raw2champsim.c:680-689`.
