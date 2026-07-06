# Online Raw-Trace Rotation — Design Spec

**Date:** 2026-07-06
**Status:** Approved (design walkthrough approved by project owner)
**Scope:** QEMU TCG plugin only (`plugin/champsim_tracer.c`) plus capture-kit
and doc integration. Reader tools are unchanged by construction.

---

## 1. Motivation

The collaborator's AArch64 captures will run for a very large number of guest
instructions, but a single `.raw.zst` per vCPU has three problems at that
scale:

1. **Balloon / corruption blast radius** — one multi-hundred-GB file; a single
   corruption (or a killed process mid-write) can jeopardize the whole capture.
2. **Post-processing time** — the offline converter must chew through the
   entire monolith before any of it is usable.
3. **Mismatch with how traces are consumed** — a ChampSim run uses only
   ~500–600 M instructions. There is no reason to keep the rest coupled in one
   file.

The project already does all "trace cutting" **online** — the PIN tracer emits
one file per sample window via its skip/trace state machine. There is no
standalone cutter tool anywhere in the project. This feature brings the same
online idiom to the QEMU raw side: rotate to a fresh chunk file every N traced
instructions per vCPU.

Governing principle (owner): keep the feature **isolated and
backward-compatible** — default-off in the plugin, opt-in, and no changes to
any reader.

---

## 2. Rotation knob and semantics

### 2.1 The knob

`rotate=N` — a new plugin knob parsed alongside the existing
`outdir=/vcpus=/limit=/trigger=/arch=/capture_pa=/values=`.

- **Default `0` = rotation off.** With `rotate=0` (or the knob absent) the
  plugin behaves exactly as today: one `trace_vcpu<V>.raw.zst` per vCPU, no
  chunk index in the name, no manifest. Byte-for-byte identical output to the
  pre-feature plugin.
- **`N>0`**: close the current chunk and open a fresh one every **N traced
  instructions on each vCPU**, counted independently per vCPU.

### 2.2 Semantics

- **Unit = traced instructions per vCPU.** Not bytes, not wall-clock. Each
  vCPU counts its own traced instructions; chunk boundaries need not align
  across vCPUs (files are already per-vCPU, so this introduces no new coupling).
- **Contiguous chunks, no inter-chunk skip.** This is size-bounding, not
  PIN-style sampling. Instruction K of the capture lands in exactly one chunk;
  concatenating the chunks' record streams reproduces the un-rotated stream.
- **Orthogonal to `limit=`.** `limit=M` continues to mean "stop tracing at M
  instructions per vCPU (total)." `rotate=N` only sets chunk size. Combinations:
  - `rotate=N, limit=0` → unbounded chunks until the guest stops / plugin exit.
  - `rotate=N, limit=M` → chunks of N instructions until M reached; the final
    chunk is partial (M mod N instructions, or a full chunk if M is a multiple).
  - `rotate=0, limit=M` → today's behavior (single file, stops at M).
- **Orthogonal to `trigger=`.** Rotation counts only *traced* instructions.
  The dormant pre-trigger phase consumes no chunk: chunk 0's file is created
  (with header) at install, but stays empty until the trigger fires and
  instructions begin flowing.
- **Trailing partial chunk is kept.** The last chunk (< N instructions) is a
  valid file, finalized at plugin exit; its true length is recorded in the
  manifest.
- **Boundary is strictly between instructions.** Rotation is evaluated in
  `insn_exec_cb` immediately after `finalize_pending_insn` has committed the
  previous instruction to the closing chunk, and before the next pending
  instruction is assembled. No record is ever split across chunks.

---

## 3. Chunk file naming

When `rotate > 0`, **every** chunk (including the first) is named:

```
trace_vcpu<V>_c<KKKKK>.raw.zst
```

- `<V>` — vCPU id, unchanged from the existing per-vCPU prefix.
- `_c` — "contiguous chunk" token. Deliberately distinct from the PIN tracer's
  `_s<sid>` (whose samples have inter-sample skip gaps); `_c` signals a
  contiguous cut with no gap.
- `<KKKKK>` — 0-indexed chunk counter, **zero-padded to 5 digits** (`%05d`), so
  `ls`/glob sort correctly up to 100 000 chunks (`_c00000`, `_c00001`, …). The
  index is always the last component before the `.raw.zst` extension.

When `rotate = 0`, the name is the unchanged `trace_vcpu<V>.raw.zst` (no `_c`
suffix). The rule is simple: rotation on ⇒ always `_c<K>`; rotation off ⇒ plain
name.

`raw2champsim` maps each chunk by its existing suffix replacement
(`…_c00000.raw.zst → …_c00000.champsim.zst`) with no code change — it operates
on a path argument and never parses the chunk index.

---

## 4. Per-vCPU manifest

When `rotate > 0`, each vCPU writes a manifest listing its chunks and their
instruction boundaries.

### 4.1 One manifest per vCPU

`trace_vcpu<V>_manifest.txt`, one file per traced vCPU. Per-vCPU (not a single
shared manifest) because:

- It matches the plugin's existing "everything is per-vCPU, no shared mutable
  state" architecture.
- It **avoids introducing the plugin's first lock.** A single shared manifest
  would be appended concurrently by multiple vCPU threads under
  `-accel tcg,thread=multi`, requiring a mutex. Per-vCPU manifests have a single
  writer each — no lock needed.

### 4.2 Append-on-close (crash resilience)

A manifest line for chunk K is appended **only after** chunk K's zstd stream is
finalized and its file `fclose`d. Therefore a line's presence guarantees that
chunk is complete and valid — which is exactly the resilience the feature is
for. `fflush` follows each append so completed chunks survive a later crash.

### 4.3 Format

Plain text, one header comment line, then one whitespace-delimited row per
completed chunk:

```
# vcpu 0 rotation manifest: chunk file start_insn insn_count comp_bytes
0 trace_vcpu0_c00000.raw.zst 0 100000000 2807123
1 trace_vcpu0_c00001.raw.zst 100000000 100000000 2799881
2 trace_vcpu0_c00002.raw.zst 200000000 43112900 1204551
```

Columns:

- `chunk` — chunk index (matches the `_c<K>` in the filename, unpadded here).
- `file` — the chunk's basename (no directory).
- `start_insn` — cumulative traced-instruction index at chunk start (chunk 0
  starts at 0; chunk K starts at the sum of prior chunks' `insn_count`).
- `insn_count` — instructions in this chunk (N for full chunks; less for the
  final partial chunk).
- `comp_bytes` — compressed size written for this chunk (its `bytes_compressed`).

The final (partial) chunk's line is appended when it is closed at plugin exit.
A run whose trigger never fires produces a manifest with only the header
comment (chunk 0 was opened but never received an instruction, so it is not
recorded as a completed chunk). Manifest is only produced when `rotate > 0`.

---

## 5. Code structure

### 5.1 Refactor: `open_chunk` / `close_chunk`

The file-open + cctx-create + 16-byte-header-write block currently inlined in
the `qemu_plugin_install` per-vCPU loop (`champsim_tracer.c:1041–1084`), and the
symmetric finalize + `fclose` + `ZSTD_freeCCtx` currently inlined in
`plugin_atexit`, are factored into two helpers reused by install, rotation, and
atexit:

- **`static bool open_chunk(VcpuState *vs)`** — create `cctx`, set level +
  checksum params, build the filename (with `_c<K>` when `rotate>0`, plain when
  off), `fopen`, write the v3 header (via the existing `buffer_append` path so
  it enters the zstd stream), reset `inbuf_pos=0`, `chunk_insn_count=0`,
  `chunk_bytes_compressed=0`. Returns false on open/cctx failure. The v3 header
  fields (arch, flags, value_cap) are identical for every chunk of a run.
- **`static void close_chunk(VcpuState *vs)`** — `flush_final(vs)` (flushes the
  input buffer and ends the zstd frame), `fclose(vs->outfile)`,
  `ZSTD_freeCCtx(vs->cctx)`, null both; then, when `rotate>0` and a manifest is
  open, append this chunk's manifest line and `fflush`.

The install loop calls `open_chunk` for chunk 0 (and opens the manifest file
first, when rotating). `plugin_atexit` calls `finalize_pending_insn` then
`close_chunk` for the final chunk, then closes the manifest file — replacing the
inline finalize/close it does today.

### 5.2 VcpuState additions

`VcpuState` gains:

- `uint32_t chunk_index` — current chunk number (0-based).
- `uint64_t chunk_insn_count` — instructions written to the current chunk;
  resets to 0 in `open_chunk`.
- `uint64_t chunk_start_insn` — cumulative instruction index at the current
  chunk's start (for the manifest `start_insn` column).
- `uint64_t chunk_bytes_compressed` — compressed bytes for the current chunk
  (for the manifest `comp_bytes` column). Note `flush_buffer`/`flush_final`
  already accumulate `bytes_compressed` cumulatively; the per-chunk counter is
  derived (e.g. snapshot cumulative at chunk open, subtract at close) or tracked
  in parallel — implementation detail, either is acceptable.
- `FILE *manifest_fp` — the vCPU's manifest file (NULL when `rotate=0`).

The existing cumulative `insn_count`, `mem_op_count`, `values_captured`,
`bytes_uncompressed`, `bytes_compressed` are unchanged and still drive the
`limit=` check and the exit stats.

### 5.3 Rotation site

In `insn_exec_cb`, the existing sequence is: early-returns (vcpu filter,
dormant, `limit_reached`) → `finalize_pending_insn(vs)` → `limit` check → set up
new pending instruction. `finalize_pending_insn` increments the instruction
counters. Rotation is inserted **after** `finalize_pending_insn` and its
counter bump, and **after** the `limit` check (so a rotation is not opened when
tracing has just stopped), and before the new pending instruction:

```c
finalize_pending_insn(vs);                 /* commits prev insn into current chunk */
if (insn_limit > 0 && vs->insn_count >= insn_limit) { ...stop...; return; }
if (rotate_interval > 0 &&
    vs->chunk_insn_count >= rotate_interval) {
    close_chunk(vs);
    vs->chunk_index++;
    vs->chunk_start_insn = vs->insn_count;
    open_chunk(vs);
}
/* ...assemble new pending insn... */
```

`finalize_pending_insn` increments `chunk_insn_count` alongside `insn_count`
(one added line). Because `finalize` has already appended the previous
instruction to the closing chunk's buffer, `close_chunk`'s `flush_final` writes
that instruction into the closing chunk — the boundary is exactly between two
instructions.

### 5.4 Globals

A file-scope `static uint64_t rotate_interval = 0;` mirrors the parsed knob,
alongside the existing `insn_limit`. Parsed in `parse_args` with the other
knobs; the usage string is extended.

---

## 6. Capture-kit integration

Because this feature exists for the collaborator's large captures, the kit opts
in by default so it reaches him without extra steps (the plugin knob itself
stays default-off, preserving existing single-file x86 users):

- `configure_tracer.sh` gains a `ROTATE` env override, **default
  `100000000`** (100 M instructions/chunk). It threads `rotate=$ROTATE` into the
  generated `run_trace.sh` plugin knob string and records `ROTATE=<value>` in
  `trace_metadata.txt`.
- Setting `ROTATE=0` at configure time omits `rotate=` (or emits `rotate=0`),
  giving single-file behavior.

100 M is a deliberate default: 5–6 chunks compose the ~500–600 M a ChampSim run
uses, and each chunk is a few GB at v3 density — small enough to post-process
incrementally and to bound corruption blast radius.

---

## 7. Documentation

- `plugin/README.md` — document `rotate=N` (default 0), the `_c<K>` chunk
  naming, and the per-vCPU manifest format; note that rotation is off by default
  and every chunk is a standalone v3 file readable by all three tools.
- `scripts/capture-kit/README.md` — the `ROTATE` default (100 M), how to choose
  a chunk size, and how to assemble a ~500–600 M ChampSim run from chunks using
  the manifest (`start_insn`/`insn_count` columns).
- `converter/README.md` — one line: rotated chunks are converted independently
  (`raw2champsim <chunk>.raw.zst` → `<chunk>.champsim.zst`); the `_c<K>` suffix
  is carried through.
- `CLAUDE.md` — a rotation note in the raw-format section.

No changes to `trace_inspector`, `trace_filter`, or `raw2champsim` source.

---

## 8. Testing plan (all local — BIOS smoke, no ARM guest needed)

1. **Rotation basic.** Capture a short BIOS run with `rotate=50000` (and a
   `limit` a few multiples above it, e.g. `limit=220000`). Expect multiple
   `trace_vcpu0_c<K>.raw.zst` files + a `trace_vcpu0_manifest.txt`. Each chunk:
   `trace_inspector` clean, `version 3`, ~50000 instructions (last one partial),
   valid standalone header. Manifest: one row per chunk, `insn_count` values sum
   to the total, `start_insn` values are the running sum, `comp_bytes` match the
   on-disk file sizes within the compressor's flush granularity.
2. **Lossless cut.** Decompress and concatenate the chunk record streams (strip
   each chunk's 16-byte header, concatenate the record bodies) and compare
   byte-for-byte against a `rotate=0` single-file capture of the *same* run
   (same seed/inputs, deterministic BIOS boot). They must be identical — proves
   the cut is lossless and lands on a record boundary.
3. **Backward compatibility.** `rotate=0` (and knob absent) → a single
   `trace_vcpu0.raw.zst`, no `_c` suffix, no manifest, byte-identical to the
   pre-feature plugin on the same run.
4. **Edge — rotate larger than total.** `rotate=100000000` on a 200 k-insn BIOS
   run → exactly one chunk `trace_vcpu0_c00000.raw.zst` + a 1-row manifest.
5. **Edge — rotate with limit boundary.** `rotate=100 limit=250` → chunks
   `_c00000` (100), `_c00001` (100), `_c00002` (50, partial); 3-row manifest;
   tracing stops at 250.
6. **Multi-vCPU independence.** Trace two vCPUs with different instruction rates
   → each has its own `_c<K>` series and its own manifest, with independent
   chunk counts.
7. **Kit integration.** `configure_tracer.sh` with default `ROTATE` →
   generated `run_trace.sh` contains `rotate=100000000`; `trace_metadata.txt`
   records `ROTATE=100000000`. `ROTATE=0` → no `rotate=` knob (single file).

---

## 9. Edge cases

| Case | Behavior |
|---|---|
| `rotate=0` / knob absent | Exact current single-file behavior; no `_c`, no manifest |
| Trigger never fires | Chunk 0 opened (header only) at install; manifest has header line only; atexit finalizes the empty chunk 0 file (as today) |
| Final partial chunk | Closed at `plugin_atexit` via `close_chunk`; its true `insn_count` recorded; exactly one manifest line, no double-append |
| `rotate` > total insts | Single chunk `_c00000` + 1-row manifest |
| `rotate` with `limit` | Rotation checked after the limit check, so no empty chunk is opened once tracing has stopped |
| Multi-threaded TCG | Per-vCPU manifests → single writer each → no lock introduced |
| `open_chunk` failure mid-run (fopen/cctx) | Log error, mark vCPU `limit_reached`/inactive to stop writing (fail safe, do not crash the guest); prior chunks already flushed and manifested remain valid |
| chunk_index overflow (>99999) | `%05d` widens naturally (6+ digits); sort order degrades only past 100 k chunks — acceptable, documented |

---

## 10. Decisions log

- Rotate trigger = **per-vCPU traced instruction count** (owner decision).
- Chunk naming = **`trace_vcpu<V>_c<KKKKK>.raw.zst`**, `_c` token, 5-digit
  zero-pad (owner decision).
- **Per-run manifest = yes**, realized as **per-vCPU** files, append-on-close,
  to avoid a lock (owner decision + engineering realization).
- Plugin knob default **off**; capture-kit default **on at 100 M** (engineering
  choice so the feature reaches the collaborator without changing plugin
  defaults for existing users).
- No reader changes — each chunk is a standalone v3 file by construction.
- Contiguous cut, trailing partial kept, boundary strictly between instructions.
