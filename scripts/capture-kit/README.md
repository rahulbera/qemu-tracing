# AArch64 Capture Kit

Collaborator-facing entry point for generating raw traces from an
AArch64 guest VM using this repo's QEMU TCG plugin. Read this
top-to-bottom the first time; after that, sections 3–8 are the
step-by-step you'll actually run.

## 1. What this kit does

This kit drives `plugin/champsim_tracer.so` — a QEMU TCG plugin — to
capture per-vCPU instruction traces from an AArch64 guest. It emits
the **raw trace format v3**: a 16-byte file header (magic, version,
vCPU id, arch byte, capture-flags byte, value-cap byte) followed by
one variable-length record per executed instruction (IP, raw
instruction bytes, privilege bit, and per-memory-op address/size/value,
plus an optional guest physical address when PA capture is on). Format
v3 adds arch-awareness (so downstream tools know whether they're
looking at x86_64 or aarch64 bytes), physical-address capture, and a
wider value-capture ceiling — full byte-level layout is in
`docs/superpowers/specs/2026-07-06-aarch64-capture-kit-design.md`
and `plugin/README.md`.

Output is one zstd-compressed file per traced vCPU
(`trace_vcpu<N>.raw.zst`) in an output directory you choose, plus a
single provenance sidecar, `trace_metadata.txt`, written into that same
directory. The sidecar records every fact this kit could determine
about your guest and host (kernel version, VA_BITS, page size, SVE
presence, QEMU version/binary/CPU model, plugin git commit, and the
exact capture knobs used) so the traces are self-describing months
later — you should never need to reconstruct how a capture was taken
from memory.

This kit is capture-side only. It does not decode AArch64 instructions
or convert traces to ChampSim's format — `plugin/trace_filter` and
`converter/raw2champsim` currently refuse AArch64 v3 files outright
(offline AArch64 decode/conversion is the next project phase; see
section 9). Your job here is to produce clean, validated `.raw.zst`
files and the sidecar, then ship the whole output directory back.

## 2. Requirements

- **QEMU ≥ 9.1**, built with `--enable-plugins` (the plugin's value
  capture uses `qemu_plugin_mem_get_value`, added in 9.1).
  `qemu-system-aarch64 --version` must report at least that.
- **Build the plugin against your own QEMU source tree** — the plugin
  is a `.so` compiled against QEMU's plugin headers, so it must be
  built against the same tree you'll run:

  ```bash
  cd plugin
  ./build_plugin.sh /path/to/your/qemu-source
  ```

  This produces `plugin/champsim_tracer.so` and prints the exact
  `-plugin ...` string (`configure_tracer.sh`, next section, will
  generate that string for you automatically — you don't need to
  copy it by hand).
- **`libzstd-dev` and `glib2.0-dev`** (or your distro's equivalents) —
  required to build the plugin and the two reader tools
  (`trace_inspector`, `trace_filter`). `build_plugin.sh` checks for
  `libzstd` via `pkg-config` and fails loudly with the install command
  if it's missing.

## 3. Step 1 — probe the guest

`probe_guest.sh` is POSIX `sh` and runs **inside** the AArch64 guest.
It collects ten facts (arch, kernel version, VA_BITS, page size, SVE
support, CPU count/model, accelerator in use) and never aborts — every
fact is printed as `KEY=value` or `KEY=UNKNOWN=<reason>`.

Get it into the guest, e.g. by `scp`:

```bash
scp scripts/capture-kit/probe_guest.sh guest:/tmp/
ssh guest 'sh /tmp/probe_guest.sh > /tmp/guest_config.txt'
scp guest:/tmp/guest_config.txt .
```

If the guest has no SSH/scp path yet (e.g. still on the serial
console), serve it from the host instead:

```bash
# host, in the directory containing probe_guest.sh:
python3 -m http.server 8000
# guest:
wget http://10.0.2.2:8000/probe_guest.sh
sh probe_guest.sh > guest_config.txt
# then copy guest_config.txt back out by whatever channel you have
```

(`10.0.2.2` is the default SLIRP gateway address for the host, from
the guest's point of view — adjust if your networking differs.)

You should end up with `guest_config.txt` on the host, next to (or
wherever you'll run) `configure_tracer.sh`.

## 4. Step 2 — configure on the host

```bash
cd scripts/capture-kit
./configure_tracer.sh guest_config.txt /path/for/traces
```

This reads `guest_config.txt`, gathers a few host facts itself (QEMU
binary path/version, host arch, the `-cpu`/`-machine` of a
currently-running `qemu-system-aarch64` process if one exists), and
writes two files next to itself:

- **`run_trace.sh`** — a ready-to-run QEMU command with the plugin
  wired up correctly. You still need to edit it (see below).
- **`trace_metadata.txt`** — the provenance sidecar described in
  section 1.

Environment variables override the defaults if the auto-detected
values are wrong or you want something different:

| Variable | Meaning | Default |
|---|---|---|
| `QEMU=` | path to `qemu-system-aarch64` | first match on `$PATH` |
| `PLUGIN=` | path to `champsim_tracer.so` | `../../plugin/champsim_tracer.so` |
| `VCPUS=` | which vCPUs to trace (`0`, `0-3`, `0,2`) | `0` |
| `LIMIT=` | instructions to capture per traced vCPU | `1000000000` |
| `ROTATE=` | instructions per chunk before rotating to a fresh file (`0` disables) | `100000000` |

Example: `VCPUS=0-3 LIMIT=20000000 ./configure_tracer.sh guest_config.txt /data/traces`

### Rotation is on by default

`configure_tracer.sh` always threads `,rotate=$ROTATE` into the
generated `run_trace.sh` and records `ROTATE=$ROTATE` in
`trace_metadata.txt` — at the default `100000000`, a capture at this
kit's scale (potentially hundreds of billions of instructions) lands as
a series of `trace_vcpu<N>_c00000.raw.zst`, `_c00001.raw.zst`, ... files
instead of one monolith. This is deliberate, not incidental: it bounds
each file's size, bounds the blast radius of a corrupted chunk or a
process killed mid-capture to that one chunk instead of the whole run,
and lets post-processing start on early chunks while later ones are
still being written. Set `ROTATE=0` to disable and get the old
single-`trace_vcpu<N>.raw.zst`-per-vCPU behavior, or `ROTATE=<N>` for a
different chunk size. Full naming and manifest format:
`plugin/README.md`.

Each traced vCPU also gets a `trace_vcpu<N>_manifest.txt` listing every
non-empty chunk's `start_insn`/`insn_count`/`comp_bytes` — it ships
alongside the chunks and `trace_metadata.txt` in the output directory.
To assemble a ChampSim run of a given size (a typical run uses
~500–600 M instructions) out of a 100 M-per-chunk capture, pick
contiguous chunks from the manifest whose `insn_count` sums to your
target:

```
# vcpu 0 rotation manifest: chunk file start_insn insn_count comp_bytes
0 trace_vcpu0_c00000.raw.zst 0 100000000 2807123
1 trace_vcpu0_c00001.raw.zst 100000000 100000000 2799881
2 trace_vcpu0_c00002.raw.zst 200000000 100000000 2812004
3 trace_vcpu0_c00003.raw.zst 300000000 100000000 2795511
4 trace_vcpu0_c00004.raw.zst 400000000 100000000 2803347
5 trace_vcpu0_c00005.raw.zst 500000000 100000000 2431901
```

Chunks 0–4 (`start_insn=0` through `500000000`, 500 M instructions) or
0–5 (600 M) are both contiguous ranges starting at instruction 0 — feed
those `.raw.zst` files to `trace_filter`/`raw2champsim` for your run
(see `converter/README.md` for the independent-vs-concatenated
conversion tradeoff).

`configure_tracer.sh` hard-checks QEMU ≥ 9.1 and that the plugin `.so`
exists before writing anything, and tells you exactly how to fix it if
either check fails (upgrade QEMU; `cd plugin && ./build_plugin.sh
<qemu-src>`).

**Now edit the generated `run_trace.sh`.** Near the bottom of the QEMU
invocation, immediately after the `"${LOADVM[@]}" \` line, you'll find
this verbatim (quoted from `configure_tracer.sh`'s template):

```
    "${LOADVM[@]}" \
    #
    # ─── YOUR BOOT FLAGS GO HERE (replace this comment block) ────────
    # Add your usual -cpu, -drive, pflash firmware, -nic, cloud-init
    # seed, etc. — everything you normally boot this guest with, minus
    # any -accel/-machine/-smp/-m flags (all four are already set above —
    # -smp/-m via the SMP=/MEM= variables near the top of this script,
    # -accel/-machine inline; -cpu note: TCG cannot use 'host'; use a
    # named model, e.g. -cpu cortex-a76).
    # ─────────────────────────────────────────────────────────────────
```

**Replace everything from the bare `#` line through the end of the
comment block** (all eight lines shown above, starting with the bare
`#` immediately under `"${LOADVM[@]}" \` and ending at the closing
`# ───...` separator) **with your boot flags.** That bare `#` line is
easy to miss — it has no marker text, so it looks like leftover
whitespace rather than part of the block — but it's exactly what
terminates the QEMU command via shell backslash-continuation. If you
leave it in place and only replace the decorated
`# ─── YOUR BOOT FLAGS GO HERE ...` lines underneath it, your pasted
flags become detached shell comments *after* the command has already
ended, and QEMU launches with none of them.

Keep the **trailing backslash on the `"${LOADVM[@]}" \` line intact** —
that's what continues the command into whatever you paste in its
place. Give each of your own flag lines a trailing ` \` too so the
command keeps continuing, except the very last one (no trailing
backslash on the final line, since it's the end of the script and
nothing follows it).

Add `-cpu`, `-drive`/pflash firmware, `-nic`, cloud-init seed, etc. —
everything you normally boot this guest with. Do **not** add `-accel`,
`-machine`, `-smp`, or `-m`: all four are already set by the template —
`-machine virt -accel tcg,thread=multi` inline, plus
`-smp "$SMP" -m "$MEM"`, where `SMP=4` and `MEM=4G` are ordinary shell
variables near the top of the generated script, right next to
`SNAPSHOT=`. If your guest normally boots with a different vCPU count
or RAM size, edit those two variables there — don't paste a second
`-smp`/`-m` into the boot-flags block below (QEMU accepts duplicate
`-smp`/`-m` flags silently, last one wins, and you'll end up tracing
the wrong vCPU count without any error telling you so). One hard
constraint: **`-cpu host` will not work under TCG** — TCG cannot
emulate a passthrough host CPU, so you need a named model (e.g.
`-cpu cortex-a76`, `-cpu max`). This matters most for the two-phase
flow below, where the same `-cpu` has to work under both KVM and TCG.

## 5. Step 3 — choose your flow

### Recommended: two-phase (KVM setup → TCG trace)

Since KVM is available on your (AArch64) host, the fast path is: do
all the slow setup (boot, install, load data, warm up) under KVM, take
a snapshot, then restore that snapshot under TCG with the plugin
attached — you skip re-doing the slow setup under emulation.

**Run the mandatory smoke test before investing in real workload setup.**
Two things can silently break a KVM→TCG snapshot restore on ARM (CPU
model mismatch, device state that only exists under KVM), and finding
that out *after* hours of workload warm-up is a waste of your time.

1. Boot your guest under KVM with a **named** CPU model — not `-cpu
   host`, which TCG cannot restore. Try `-cpu max` first (it usually
   aliases to the host CPU's features under KVM while still being a
   named model), or a specific named core your KVM accepts. Include a
   monitor so you can issue `savevm` from the QEMU console:

   ```bash
   -monitor telnet:127.0.0.1:4444,server,nowait
   ```

2. Once the guest is up (even at just a login prompt — this is a
   smoke test, not a real capture), connect to the monitor and take a
   trivial snapshot:

   ```bash
   telnet 127.0.0.1 4444
   (qemu) savevm smoketest
   (qemu) quit
   ```

3. Edit `run_trace.sh`: set `SNAPSHOT="smoketest"` at the top (it
   controls the `-loadvm` flag the script passes to TCG).

4. Run `./run_trace.sh`. Two outcomes:
   - **It restores cleanly** (no "Unknown savevm section" or CPU-model
     errors, guest resumes) → two-phase works on your hardware. Go
     back under KVM, do your real workload setup, `savevm` your real
     checkpoint, and repeat with that snapshot name.
   - **It fails** — either on a CPU-model error (TCG can't instantiate
     the CPU features the snapshot recorded) or an unknown-section /
     device-state error naming something like the GIC or the ARM
     architected timer — this is the ARM analog of this project's own
     x86 `kvmclock` problem: a device whose state is only meaningful
     under KVM, so TCG has no handler to read it back. See
     `docs/pipeline/kvmclock-patch-details.md` for the full story and
     debugging pattern (same shape of problem, different device); a
     fix would mean patching QEMU's ARM device models the same way we
     patched `hw/i386/kvm/clock.c`. If you don't want to chase that,
     fall back to single-phase below — nothing else in this kit
     depends on which flow you use.

### Fallback: single-phase TCG

Always works, no snapshot dependency, just slower to reach steady
state:

1. In `run_trace.sh`, set `SNAPSHOT=""` (empty — the default the
   generator writes).
2. Run `./run_trace.sh`. This cold-boots the guest entirely under TCG,
   using whatever boot flags you pasted into the marked block in
   section 4.
3. Wait for the guest to reach the state you actually want to trace
   (see section 6 for why this can take a while and how to know when
   you're there).

## 6. Step 4 — start tracing

Tracing does **not** start automatically. `run_trace.sh` always launches
the plugin with `trigger=/tmp/trace_start`, so the plugin is **dormant**
from the moment QEMU starts — it counts instructions but writes
nothing to disk — until you `touch /tmp/trace_start` **on the host**.
The whole job of this step is deciding *when* to do that.

**Do not touch the trigger right after boot.** On a real ARM64 Ubuntu
guest under TCG, UEFI firmware plus kernel boot takes on the order of
**tens of billions of instructions** before you even reach a login
prompt — a validation run on this project's own test guest found that
on the order of 20 billion instructions elapse before the guest is even
past login (that run's specific measurement was taken shortly after the
login prompt appeared, not at the exact instant of it, so treat it as
qualitative, not a precise cutoff). Any fixed "trace the first N
instructions" recipe will only ever capture firmware, never your
workload. Instead:

1. Launch `./run_trace.sh` (or the two-phase `-loadvm` restore).
2. Watch the console (or SSH in once networking comes up) and wait
   until the guest has reached the state you actually want traced —
   at an absolute minimum, **past the login prompt**; ideally past
   that, once your actual workload is running and warmed up (steady
   state). Give it extra settle time after login (tens of seconds is
   reasonable) so you're not capturing boot-tail noise like cloud-init
   or package-manager activity.
3. Only then, on the **host**:

   ```bash
   touch /tmp/trace_start
   ```

The plugin's stderr tells you exactly what state it's in. At startup,
before the trigger fires, you'll see something like:

```
[champsim_tracer] Arch: aarch64 | capture_pa: on | values: on (value_cap=16) | CSTF_COMMIT=<sha>
[champsim_tracer] Output: /path/for/traces
[champsim_tracer] Tracing vCPUs: 0
[champsim_tracer] Limit: 1000000000 insns/vCPU
[champsim_tracer] Trigger: WAITING for file '/tmp/trace_start'
[champsim_tracer]   >>> Tracing is DORMANT <<<
[champsim_tracer]   To start tracing, run on the host:
[champsim_tracer]     touch /tmp/trace_start
[champsim_tracer] vCPU 0 -> /path/for/traces/trace_vcpu0.raw.zst
[champsim_tracer] Initialized. Plugin is DORMANT — waiting for: touch /tmp/trace_start
```

The instant you `touch` the trigger file, you'll see:

```
[champsim_tracer] >>> TRIGGER DETECTED: /tmp/trace_start <<<
[champsim_tracer] Tracing is now ENABLED (skipped <N> instructions during dormant phase)
```

and, once a vCPU hits its instruction `limit` **and** you shut the
guest down, a summary line per traced vCPU:

```
[champsim_tracer] vCPU 0: 20000000 insns, 5898007 mem ops (5898007 with values), 459.5 MB raw -> 63.8 MB zstd (7.2x) [limit]
```

**Important: that summary line only ever prints when QEMU exits — it
does not print live the moment a vCPU hits its `limit`.** A vCPU that
reaches its instruction cap stops writing to its `.raw.zst` file
immediately, but the plugin stays silent about it until the whole QEMU
process exits (clean shutdown or otherwise); this project's own
validation run confirmed a vCPU can sit at its cap for 20+ minutes with
zero stderr output before the summary finally appears at shutdown. So
you cannot use "wait for the summary line" as your cue to shut down —
by the time you see it, the guest is already gone. Instead:

1. After `touch`-ing the trigger, just wait out the expected capture
   duration rather than watching stderr for a completion signal. As a
   rule of thumb from this project's own validation runs, budget on the
   order of a few minutes of wall-clock time per 2 million instructions
   per traced vCPU under TCG on a reasonably fast host — scale that
   linearly for whatever `LIMIT` you configured (expect TCG to run
   single-digit millions of guest instructions per wall-clock second,
   not the billions KVM gives you; multi-threaded TCG, `thread=multi`,
   already set in the template, will still be far behind KVM).
2. Optionally, corroborate that estimate by watching the output
   directory instead of stderr:
   ```bash
   watch -n 5 ls -l <outdir>
   ```
   Growing `.raw.zst` sizes mean capture is still active; a size that's
   stopped changing for well longer than your estimate from step 1
   means every traced vCPU has likely already hit its `limit`.
3. Shut the guest down (`ssh <guest> sudo poweroff`, or QEMU
   monitor `quit`/SIGTERM) once you're confident capture is done. The
   per-vCPU `[limit]` summary above will appear in stderr at that point
   — that's your authoritative, after-the-fact confirmation of what was
   actually captured, not a thing to wait for beforehand.

Prefer a smaller `LIMIT` for your first real run so you can validate
(section 7) before committing to a long capture.

## 7. Step 5 — validate

As soon as you have at least one `.raw.zst` file, before trusting the
capture, run:

```bash
plugin/trace_inspector <outdir>/trace_vcpu0.raw.zst
```

Here is a real, known-good example (a 20M-instruction AArch64 capture,
triggered after login + settle time — this project's own validation
run):

```
=== Trace File: /tmp/cstf/v3_arm/trace_vcpu0.raw.zst ===
Compression: zstd
Format version: 3
vCPU ID: 0
Arch: aarch64
PA capture: yes
Value capture: enabled (cap 16 bytes)


=== Summary ===
Total instructions:     20000000
  User mode:            2909031 (14.5%)
  Kernel mode:          17090969 (85.5%)
Total memory ops:       5898007
  Reads:                3755652
  Writes:               2142355
  With values captured: 5898007 (100.0%)
Taken branches (est):   2042057 (10.2%)
Avg mem ops/insn:       0.29
PA valid:               5898007 (100.0%)
PA is MMIO:             4697
sanity: instr_size!=4 in 0/20000000 records (0.00%)

Memory access size distribution:
   1B:     195133 (  3.3%)
   2B:      26222 (  0.4%)
   4B:    1040783 ( 17.6%)
   8B:    2861556 ( 48.5%)
  16B:    1774313 ( 30.1%)
```

What each red flag means, and what to do next:

- **`Arch:` is not `aarch64`, or a `version`/`arch` error at the top**
  ("ERROR: Unsupported format version...", "ERROR: Unknown arch
  byte...") — you're most likely pointing the inspector at the wrong
  file, or the plugin `.so` and inspector binary came from mismatched
  builds/commits. Rebuild both from the same checkout
  (`cd plugin && make clean && make`, or `./build_plugin.sh`) and
  re-run.
- **`sanity: instr_size!=4` is nonzero** — some instructions in the
  file are not 4 bytes wide, which on AArch64 means 32-bit (T32/A32)
  EL0 code is present (A64 is fixed-width 4 bytes; only compat-mode
  32-bit userspace code varies). Not necessarily wrong, but note it
  when you ship the trace — the (future) offline decoder needs to know.
- **`PA valid` is well below 100%** — a meaningful fraction of memory
  accesses failed the guest-physical-address lookup
  (`qemu_plugin_get_hwaddr` returned nothing). Some MMIO/failed
  lookups are expected (see `PA is MMIO` in the summary), but if this
  is unexpectedly low, tell us before shipping the trace — don't try
  to self-diagnose the QEMU internals here.
- **User/kernel split is ~100% user, ~0% kernel** (or vice versa in a
  way that looks implausible for your workload) — see the note below;
  this is the single most common false alarm.

**Important: the privilege split is a virtual-address heuristic, not
an exception-level probe.** The plugin classifies an instruction as
"kernel" purely by whether its virtual address falls in the top half
of the address space (VA_BITS-independent — see the spec, §3.5); it
does not read the CPU's current exception level. During pre-boot UEFI
firmware execution, code runs identity-mapped at low virtual addresses
regardless of its actual privilege, so **everything classifies as
"user"** during that window — this is expected and harmless, not a
bug. If `trace_inspector` shows ~100% user with near-zero kernel, the
most likely explanation is that `touch /tmp/trace_start` fired too
early — you're still capturing firmware, not your guest OS or
workload. Go back to section 6, let the guest get further into (or
past) boot, and re-capture.

## 8. Step 6 — ship

Send the **entire output directory** (`<outdir>` from section 4) back
— not just the `.raw.zst` files. `run_trace.sh` automatically copies
`trace_metadata.txt` into `<outdir>` before launching QEMU, so a
correctly shipped directory looks like:

```
<outdir>/
├── trace_vcpu0.raw.zst
├── trace_vcpu1.raw.zst        (if you traced more than one vCPU)
├── ...
└── trace_metadata.txt
```

The sidecar is not optional — it's how the offline conversion track
configures itself (arch, VA_BITS, capture knobs, plugin commit)
without asking you to reconstruct how the capture was taken.

## 9. Troubleshooting

- **Plugin won't load / QEMU rejects `-plugin`.** Either your QEMU
  wasn't built with `--enable-plugins`, or it's older than 9.1 (the
  plugin needs `qemu_plugin_mem_get_value`, added in 9.1).
  `configure_tracer.sh` checks the version up front and refuses to
  generate `run_trace.sh` if it's too old — if you're invoking QEMU
  by hand instead, check `qemu-system-aarch64 --version` and rebuild
  with `--enable-plugins` if needed (see section 2).
- **`WARNING: arch= override (...) contradicts QEMU target (...)`.**
  You (or the generated command) passed an explicit `arch=` knob that
  disagrees with what QEMU itself reports as its target. The plugin
  honors your override and keeps running — but double-check you
  pointed `configure_tracer.sh`/`run_trace.sh` at the AArch64 QEMU
  binary and not an x86_64 one.
- **`values=off` / `capture_pa=off` as performance levers.** Both are
  `on` by default in the generated `run_trace.sh`. Turning `values=off`
  skips the `qemu_plugin_mem_get_value` call entirely (no memory
  values in the trace — address/size are still captured). Turning
  `capture_pa=off` skips the guest-physical-address lookup (a TLB walk
  per memory access) entirely, not just the output bytes — no PA in
  the trace, but a real per-access performance win during a slow TCG
  capture. Only flip these if the offline track has confirmed it
  doesn't need values or PAs for your run — both are captured by
  default because they cannot be recovered after the fact. Full knob
  semantics (defaults, exact behavior, header-flag interaction) are in
  `plugin/README.md`.
- **`plugin/trace_filter` or `converter/raw2champsim` refuse your
  file.** Both intentionally hard-error on AArch64 v3 traces today
  (`ERROR: arch=aarch64 — idle-loop filtering for AArch64 is not yet
  supported` / `ERROR: arch=aarch64 — this converter decodes x86 only
  (Zydis)`) — offline AArch64 decode and idle-loop filtering are the
  next phase of this project, not yet built. This is expected; ship
  the raw `.raw.zst` + `trace_metadata.txt` as-is (section 8).
