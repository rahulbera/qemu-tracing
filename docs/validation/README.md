# Trace Sanity Checks

Quick health checks for a captured raw trace (`.raw.zst`), before trusting
it or converting it at scale. Each step: **what it verifies → command →
what a good vs. bad result looks like.**

Substitute your own paths:

```bash
TRACE=/path/to/trace_vcpu0.raw.zst        # one raw chunk (or the whole file)
DIR=$(dirname "$TRACE")
```

## Build the tools (once)

```bash
sudo apt install -y libzstd-dev libcapstone-dev
make -C plugin inspector        # -> plugin/trace_inspector
make -C converter               # -> converter/raw2champsim (fetches Zydis)
```

## Step 1 — Capture summary (did the plugin record full data?)

Verifies arch, and that physical addresses and memory values were actually
captured — not silently dropped.

```bash
plugin/trace_inspector -n 5000000 "$TRACE" | tail -25
```

- **Good:** `Arch` is correct; `PA capture: yes`; `Value capture: yes`;
  `With values captured` ≈ 100% of mem ops; `Avg mem ops/insn` is a
  plausible non-zero fraction (~0.2–0.5 typical); `PA valid` ≈ 100%.
- **Bad:** `PA capture: no` / `Value capture: no` when you expected them;
  `With values captured: 0%`; `Avg mem ops/insn: 0.00`.

## Step 2 — Real bytes (are addresses/values genuine, not zeros?)

Verifies the captured VAs/PAs/values are real data. This is the decisive
check after any QEMU/plugin/environment change — a broken value/PA path
often still *counts* mem ops but writes zeros.

```bash
plugin/trace_inspector -v -n 40 "$TRACE"
```

- **Good:** per-op lines `R|W[NB]@0x<VA> PA=0x<PA>=0x<value>` show varied,
  real addresses (kernel/user IPs and VAs that look like real pointers)
  and **non-zero, varied** trailing `=0x<value>` fields.
- **Bad:** the trailing `=0x…` values are all zero; PAs all `0x0` (or all
  `[invalid]`); VAs constant/degenerate.

## Step 3 — Decode + invariants (does the converter accept it?)

Verifies the trace decodes to ChampSim records with (near-)zero failures
and that the register/branch invariants hold on real output. Convert a
bounded prefix for speed.

```bash
converter/raw2champsim -n 20000000 "$TRACE" /tmp/health.champsim.zst
python3 converter/tests/props.py /tmp/health.champsim.zst
```

- **Good:** converter ends with `Decode failures: 0` (a handful is fine);
  `props.py` prints `ALL PROPERTIES HOLD`. Both stream with a live
  heartbeat, so a long run is progressing, not hung.
- **Bad:** a high `Decode failures` count; any `PROPERTY VIOLATION`.

## Step 4 — Manifest consistency (rotated captures only)

For a rotated capture (`rotate=N`, producing `trace_vcpu<V>_c<K>.raw.zst`
chunks + a `trace_vcpu<V>_manifest.txt`), verify chunks are contiguous and
on-disk sizes match the manifest.

```bash
cd "$DIR" && python3 - <<'EOF'
import os
exp = bad = 0
for l in open("trace_vcpu0_manifest.txt"):
    if l.startswith("#"): continue
    _, f, st, ic, cb = l.split(); st, ic, cb = int(st), int(ic), int(cb)
    sz = os.path.getsize(f) if os.path.exists(f) else -1
    if st != exp: print(f"GAP  {f}: start {st} != {exp}"); bad += 1
    if sz != cb: print(f"SIZE {f}: on-disk {sz} != manifest {cb}"); bad += 1
    exp += ic
print("OK" if not bad else f"{bad} PROBLEM(S)", "| total insns", exp)
EOF
```

- **Good:** `OK | total insns <N>`, matching the plugin's reported count.
- **Bad:** any `GAP` (non-contiguous) or `SIZE` (truncated/corrupt chunk).

## Note on size

A small *compressed* file is not by itself a problem: the raw stream is
verbose (VA+PA+value per mem-op), but zstd compresses repetitive workloads
(e.g. interpreter loops) heavily. Trust Steps 1–2 over the file size — if
values and PAs are real and present, a small `.raw.zst` is just good
compression, not lost data.
