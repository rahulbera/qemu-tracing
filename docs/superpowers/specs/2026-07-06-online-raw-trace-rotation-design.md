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
  `insn_exec_cb` after `finalize_pending_insn` has committed the previous
  instruction to the closing chunk **and after the `limit=` check** (so no
  chunk is opened once tracing has already stopped), and before the next
  pending instruction is assembled. This ordering is the single source of
  truth — §5.3 uses identical language. No record is ever split across chunks.

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

**Empty chunks are never recorded.** `close_chunk` appends a manifest line only
when `chunk_insn_count > 0`. The only chunk that can be empty is chunk 0 when
the trigger never fires (every post-rotation chunk receives at least the one
instruction assembled in the same callback that opened it). That header-only
chunk-0 file still exists on disk (finalized normally at exit) but is **absent
from the manifest** — a distinction directory-glob consumers should note (the
manifest lists only non-empty chunks; the directory may contain one extra
header-only file).

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

The **per-chunk** part of the setup — `ZSTD_createCCtx` + level/checksum params
(`champsim_tracer.c:1051–1060`), filename `snprintf` (1062–1064), `fopen`
(1066), and the 16-byte v3 header build + `buffer_append` writes (1074–1088) —
is factored into `open_chunk`, and the symmetric per-chunk teardown currently
inlined in `plugin_atexit` (`finalize`-then-`flush_final` + `fclose` +
`ZSTD_freeCCtx`) into `close_chunk`. **The one-time per-vCPU init stays in the
install loop and must NOT move into `open_chunk`:** the `memset(vs, 0, …)`
(1042), `vs->vcpu_id = i` (1043), and the `inbuf`/`outbuf` `g_malloc`s
(1045–1049) run once — re-running the `memset` per rotation would wipe the
cumulative `insn_count`/stats and the new `chunk_*` fields, and re-running the
`g_malloc`s would leak `inbuf`/`outbuf` every rotation.

- **`static bool open_chunk(VcpuState *vs)`** — set `inbuf_pos = 0` first, then
  create `cctx`, set level + checksum params, build the filename (with `_c<K>`
  when `rotate>0`, plain when off), `fopen`, write the v3 header (via the
  existing `buffer_append` path so it enters the zstd stream). Also initialize
  all per-chunk bookkeeping here, so `open_chunk` is the single owner of it:
  `chunk_insn_count = 0`, `chunk_start_insn = vs->insn_count` (0 at install,
  the running cumulative count at each rotation), `chunk_bytes_at_open =
  vs->bytes_compressed` (snapshot for the per-chunk byte delta). Returns false
  on open/cctx failure. The v3 header fields (arch, flags, value_cap) are
  identical for every chunk of a run.
- **`static void close_chunk(VcpuState *vs)`** — `flush_final(vs)` (flushes the
  input buffer and ends the zstd frame), `fclose(vs->outfile)`,
  `ZSTD_freeCCtx(vs->cctx)`, null both; then, when `rotate>0` **and**
  `manifest_fp != NULL` **and** `chunk_insn_count > 0`, append this chunk's
  manifest line (with `comp_bytes = vs->bytes_compressed - chunk_bytes_at_open`)
  and `fflush`. The `chunk_insn_count > 0` guard suppresses the sole
  empty-chunk case (chunk 0 when the trigger never fires — see §4.3/§9).

The install loop calls `open_chunk` for chunk 0 (and opens the manifest file
first, when rotating). `plugin_atexit` calls `finalize_pending_insn` then
`close_chunk` for the final chunk, then closes the manifest file — replacing the
inline finalize/close it does today.

### 5.2 VcpuState additions

`VcpuState` gains:

- `uint32_t chunk_index` — current chunk number (0-based).
- `uint64_t chunk_insn_count` — instructions written to the current chunk;
  reset to 0 in `open_chunk`; incremented in `finalize_pending_insn` alongside
  `insn_count`.
- `uint64_t chunk_start_insn` — cumulative instruction index at the current
  chunk's start (manifest `start_insn`), set in `open_chunk`.
- `uint64_t chunk_bytes_at_open` — snapshot of cumulative `bytes_compressed`
  taken in `open_chunk`; the per-chunk `comp_bytes` is
  `bytes_compressed - chunk_bytes_at_open`, computed in `close_chunk` after
  `flush_final`. This snapshot-and-subtract method is chosen (over a parallel
  per-chunk counter) precisely so `flush_buffer`/`flush_final` need **no**
  change — they keep incrementing only the cumulative `bytes_compressed`.
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
finalize_pending_insn(vs);                 /* commits prev insn into current chunk;
                                              increments insn_count AND chunk_insn_count */
if (insn_limit > 0 && vs->insn_count >= insn_limit) { ...stop...; return; }
if (rotate_interval > 0 &&
    vs->chunk_insn_count >= rotate_interval) {
    close_chunk(vs);                       /* finalizes + manifest line for the closing chunk */
    vs->chunk_index++;
    open_chunk(vs);                        /* owns chunk_start_insn / chunk_insn_count / snapshot */
}
/* ...assemble new pending insn... */
```

(`chunk_start_insn` is set inside `open_chunk`, not here — `open_chunk` is the
single owner of per-chunk field initialization.)

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
  `100000000`** (100 M instructions/chunk). It **always** threads
  `,rotate=$ROTATE` into the generated `run_trace.sh` plugin knob string (a
  single clean heredoc insertion) and adds `ROTATE=$ROTATE` to the
  `trace_metadata.txt` sidecar heredoc (an additive key; the sidecar has no
  key-count-sensitive consumer — it is copied wholesale).
- Setting `ROTATE=0` at configure time emits `rotate=0`, which the plugin
  already treats as off (§2.1) — single-file behavior. We always emit the knob
  (rather than conditionally omitting it) to keep the generation a one-line
  insertion and the sidecar key set consistent.

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
  is carried through. **Caveat to document:** `raw2champsim` sets
  `branch_taken` by look-ahead to the next instruction's IP, and the last
  instruction of any file is written with `branch_taken=0` (no cross-file
  look-ahead). Converting chunks independently therefore mismarks a taken
  branch that lands on a chunk's final instruction — bounded to one instruction
  per chunk boundary (statistically negligible at 100 M/chunk). For exact
  parity, convert the concatenated stream.
- `CLAUDE.md` — a rotation note in the raw-format section.

**No changes to `trace_inspector`, `trace_filter`, or `raw2champsim` source** —
each chunk is a standalone v3 file. Two stateful-across-records caveats must be
documented (not code-fixed):

- **`trace_filter` is stateful (HLT→HLT idle detection).** It resets to ACTIVE
  at each file's start and conservatively keeps any in-flight idle candidate at
  EOF, so an idle iteration straddling a chunk boundary is not filtered — a
  bounded under-filter of at most one idle iteration per boundary (~1 per 100 M
  instructions at the default). No crash or corruption. For exact whole-stream
  filtering parity, filter the concatenated (un-rotated) stream.
- **`raw2champsim` branch look-ahead** — the per-boundary `branch_taken=0`
  caveat above.

Both are inherent to per-chunk (vs whole-stream) processing of a stateful
consumer; they are documented and accepted, not defects in the rotation
feature.

---

## 8. Testing plan (all local — BIOS smoke, no ARM guest needed)

1. **Rotation basic.** Capture a short BIOS run with `rotate=50000` (and a
   `limit` a few multiples above it, e.g. `limit=220000`). Expect multiple
   `trace_vcpu0_c<K>.raw.zst` files + a `trace_vcpu0_manifest.txt`. Each chunk:
   `trace_inspector` clean, `version 3`, ~50000 instructions (last one partial),
   valid standalone header. Manifest: one row per chunk, `insn_count` values sum
   to the total traced, `start_insn` values are the running sum starting at 0,
   and each `comp_bytes` **equals the chunk file's on-disk `stat` size exactly**
   (the zstd frame is fully finalized with `ZSTD_e_end` before the count is
   recorded, and `bytes_compressed` is the exact running sum of every `fwrite`
   `output.pos`; the only way they could diverge is an unchecked short write /
   `ENOSPC`).
2. **Lossless cut (self-contained).** Within the single rotated capture:
   decompress each chunk, **strip its 16-byte header**, concatenate the record
   bodies, and verify the result is a valid record stream whose instruction
   count equals the manifest total and whose IP sequence is monotone-consistent
   across boundaries (no split record: the concatenation length is an exact
   multiple of the per-instruction record framing at every boundary). This is
   self-contained — it does not diff two separate QEMU processes.
   **Optional exact-parity variant (requires determinism):** compare
   `strip_header(decompress(single rotate=0 file))` byte-for-byte against
   `concat(strip_header(decompress(chunk_i)))`. Both headers must be stripped
   (the rotate=0 file has its own). This diff is only valid under a
   deterministic replay (`-icount`); without it, TCG interrupt/timer timing can
   differ between the two processes and cause a false mismatch unrelated to
   rotation, so the self-contained check above is the primary gate.
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
| Trigger never fires | Chunk 0 opened (header only) at install; atexit finalizes the empty chunk-0 file (as today). The manifest contains **only its header comment** — `close_chunk`'s `chunk_insn_count > 0` guard omits the empty chunk. The header-only `_c00000` file exists on disk but is absent from the manifest (directory-glob vs manifest-driven consumers differ by that one file) |
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

## 11. Adversarial review record

Reviewed 2026-07-06 by three independent reviewers (consistency/ambiguity,
source-implementability, edge/downstream). 15 findings — 1 blocker (spurious
empty-chunk-0 manifest row), 7 should-fix, 7 nits — all incorporated:
`close_chunk` guards the manifest append on `chunk_insn_count > 0`; §2.2/§5.3
rotation-ordering language unified (after finalize *and* after the limit
check); `open_chunk` is the sole owner of `chunk_start_insn`/`chunk_insn_count`
init and resets `inbuf_pos` before the header write; per-chunk `comp_bytes` via
`bytes_compressed` snapshot-subtract (no `flush_*` change) and asserted equal
to the on-disk file size exactly; the factorable install region corrected to
~1051–1088 (excluding the one-time `memset`/`g_malloc`s); the lossless-cut test
made self-contained (with an `-icount`-gated exact-parity variant); and the
`trace_filter` cross-chunk idle-state and `raw2champsim` last-instruction
`branch_taken=0` boundary effects documented as bounded/accepted. Source facts
verified: `raw2champsim` output naming (`strstr(".raw.zst")`) carries `_c<K>`
through cleanly; `bytes_compressed` is the exact fwrite total.
