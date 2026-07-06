# Online Raw-Trace Rotation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `rotate=N` plugin knob that closes the current per-vCPU `.raw.zst` and opens a fresh chunk every N traced instructions, producing `trace_vcpu<V>_c<KKKKK>.raw.zst` chunks plus a per-vCPU append-on-close manifest.

**Architecture:** Refactor the per-chunk open/close into `open_chunk`/`close_chunk` helpers (behavior-identical at `rotate=0`), then add rotation on top: a per-chunk instruction counter, a rotation check in `insn_exec_cb` after finalize + after the limit check, and a per-vCPU manifest. Default off; capture kit opts in at 100M. No reader changes — each chunk is a standalone v3 file.

**Tech Stack:** C (QEMU plugin API, zstd streaming), bash (capture kit), the existing `plugin/tests/smoke_capture.sh` BIOS harness.

**Spec:** `docs/superpowers/specs/2026-07-06-online-raw-trace-rotation-design.md` — normative.

## Global Constraints

- Knob `rotate=N` (file-scope `static uint64_t rotate_interval = 0;`), default 0 = off → **byte-identical** to the pre-feature plugin (single `trace_vcpu<V>.raw.zst`, no `_c`, no manifest).
- `rotate>0`: rotate every N **traced instructions per vCPU**, counted independently. Contiguous (no skip). Trailing partial chunk kept.
- Chunk filename `trace_vcpu<V>_c<KKKKK>.raw.zst`, `_c` token, `%05d` zero-pad, 0-indexed; every chunk incl. the first gets `_c` when rotating; plain name when off.
- Rotation site: in `insn_exec_cb`, **after** `finalize_pending_insn` **and after** the `limit=` check, before assembling the new pending instruction.
- `open_chunk` owns all per-chunk field init: sets `inbuf_pos=0` **before** the header write, `chunk_insn_count=0`, `chunk_start_insn=vs->insn_count`, `chunk_bytes_at_open=vs->bytes_compressed`.
- `close_chunk` = `flush_final` + `fclose` + `ZSTD_freeCCtx` + (append manifest line **only when** `rotate_interval>0 && manifest_fp && chunk_insn_count>0`, then `fflush`). `comp_bytes = bytes_compressed - chunk_bytes_at_open`.
- `finalize_pending_insn` increments `chunk_insn_count` alongside `insn_count`.
- One-time per-vCPU init (`memset`, `vcpu_id`, `inbuf`/`outbuf` `g_malloc`) stays in the install loop, NOT in `open_chunk`.
- Per-vCPU manifest `trace_vcpu<V>_manifest.txt`: header comment + one row `chunk file start_insn insn_count comp_bytes` per non-empty chunk. Only when `rotate>0`.
- Capture kit: always emit `,rotate=$ROTATE` (default 100000000) into `run_trace.sh`; add `ROTATE=$ROTATE` to the sidecar.
- Build: `make -C plugin plugin`. Test harness: `plugin/tests/smoke_capture.sh <outdir> [,extra-plugin-args]` (QEMU `~/qemu-custom/bin/qemu-system-x86_64`, arch auto-detects x86_64). Scratch: `/tmp/cstf-rot/`. `zstd` on PATH; `python3` at `~/anaconda3/bin/python3`.
- Commit after every task.

---

### Task 1: Refactor open/close into helpers (behavior-identical)

**Files:**
- Modify: `plugin/champsim_tracer.c` (add `open_chunk`/`close_chunk` above `insn_exec_cb`; call `open_chunk` from the install loop replacing the inline block ~1051–1088; call `close_chunk` from `plugin_atexit` replacing the inline finalize/flush/close ~826–839)

**Interfaces:**
- Produces: `static bool open_chunk(VcpuState *vs)` and `static void close_chunk(VcpuState *vs)`. At this task's end, rotation does not exist yet — `open_chunk` is called once per vCPU (install), `close_chunk` once per vCPU (atexit). Output is byte-identical to pre-refactor.

- [ ] **Step 1: Capture a pre-refactor baseline**

```bash
mkdir -p /tmp/cstf-rot
make -C plugin plugin
plugin/tests/smoke_capture.sh /tmp/cstf-rot/baseline
cp /tmp/cstf-rot/baseline/trace_vcpu0.raw.zst /tmp/cstf-rot/baseline_pre.raw.zst
zstd -dc /tmp/cstf-rot/baseline_pre.raw.zst | sha256sum > /tmp/cstf-rot/baseline_pre.sha
cat /tmp/cstf-rot/baseline_pre.sha
```
Expected: a sha256 of the decompressed baseline (used to prove Task 1 changes nothing).

- [ ] **Step 2: Add the two helpers**

Insert immediately above `static void insn_exec_cb(` (currently near line 530). The bodies below are lifted verbatim from the existing install (open) and atexit (close) code so behavior is identical; the rotation-specific pieces (`_c` naming, per-chunk fields, manifest) are written now but are dormant while `rotate_interval == 0` and `manifest_fp == NULL`.

```c
/* ================================================================
 * Chunk lifecycle: open_chunk / close_chunk
 *
 * Factored from the per-vCPU install loop and plugin_atexit so that
 * (Task 2) online rotation can reuse them. With rotate_interval==0 and
 * manifest_fp==NULL these behave exactly as the original inline code:
 * one chunk per vCPU named trace_vcpu<V>.raw.zst, no manifest.
 * ================================================================ */

static bool open_chunk(VcpuState *vs)
{
    /* Fresh compressed stream for this chunk. */
    vs->inbuf_pos = 0;

    vs->cctx = ZSTD_createCCtx();
    if (!vs->cctx)
    {
        fprintf(stderr, "[%s] ERROR: ZSTD_createCCtx failed (vcpu %u)\n",
                PLUGIN_NAME, vs->vcpu_id);
        return false;
    }
    ZSTD_CCtx_setParameter(vs->cctx, ZSTD_c_compressionLevel, ZSTD_LEVEL);
    ZSTD_CCtx_setParameter(vs->cctx, ZSTD_c_checksumFlag, 1);

    /* Filename: plain when not rotating, _c<KKKKK> when rotating. */
    char filepath[4200];
    if (rotate_interval > 0)
    {
        snprintf(filepath, sizeof(filepath),
                 "%s/trace_vcpu%u_c%05u.raw.zst",
                 output_dir, vs->vcpu_id, vs->chunk_index);
    }
    else
    {
        snprintf(filepath, sizeof(filepath), "%s/trace_vcpu%u.raw.zst",
                 output_dir, vs->vcpu_id);
    }

    vs->outfile = fopen(filepath, "wb");
    if (!vs->outfile)
    {
        fprintf(stderr, "[%s] ERROR: cannot open %s\n", PLUGIN_NAME, filepath);
        ZSTD_freeCCtx(vs->cctx);
        vs->cctx = NULL;
        return false;
    }

    /* Per-chunk bookkeeping (open_chunk is the single owner). */
    vs->chunk_insn_count    = 0;
    vs->chunk_start_insn    = vs->insn_count;
    vs->chunk_bytes_at_open = vs->bytes_compressed;

    /* Write the 16-byte v3 header into the zstd stream. */
    uint32_t magic = TRACE_FORMAT_MAGIC;
    uint32_t version = TRACE_FORMAT_VER;
    uint32_t vid = vs->vcpu_id;
    uint8_t hdr_tail[4];
    hdr_tail[0] = (uint8_t)trace_arch;
    hdr_tail[1] = (capture_pa ? FILE_FLAG_HAS_PA : 0) |
                  (capture_values ? FILE_FLAG_HAS_VALUES : 0);
    hdr_tail[2] = capture_values ? VALUE_API_CAP : 0;
    hdr_tail[3] = 0;

    buffer_append(vs, &magic, 4);
    buffer_append(vs, &version, 4);
    buffer_append(vs, &vid, 4);
    buffer_append(vs, hdr_tail, 4);

    return true;
}

static void close_chunk(VcpuState *vs)
{
    finalize_pending_insn(vs);   /* flush any pending insn into this chunk */
    flush_final(vs);             /* drain input buffer + end the zstd frame */

    if (vs->outfile)
    {
        fclose(vs->outfile);
        vs->outfile = NULL;
    }
    if (vs->cctx)
    {
        ZSTD_freeCCtx(vs->cctx);
        vs->cctx = NULL;
    }

    /* Manifest line: only when rotating, a manifest is open, and this
     * chunk actually received instructions (suppresses the empty chunk 0
     * when the trigger never fires). */
    if (rotate_interval > 0 && vs->manifest_fp && vs->chunk_insn_count > 0)
    {
        uint64_t comp = vs->bytes_compressed - vs->chunk_bytes_at_open;
        fprintf(vs->manifest_fp,
                "%u trace_vcpu%u_c%05u.raw.zst %" PRIu64 " %" PRIu64
                " %" PRIu64 "\n",
                vs->chunk_index, vs->vcpu_id, vs->chunk_index,
                vs->chunk_start_insn, vs->chunk_insn_count, comp);
        fflush(vs->manifest_fp);
    }
}
```

Note: this references `VcpuState` fields (`chunk_index`, `chunk_insn_count`, `chunk_start_insn`, `chunk_bytes_at_open`, `manifest_fp`) and the global `rotate_interval` that Task 2 adds. **To keep Task 1 compiling and behavior-identical, add those field/global declarations now** (they stay zero/NULL/0 with no rotation): in `VcpuState` (after `bool limit_reached;`, line 258) add:
```c
    /* Rotation (Task 2) — inert while rotate_interval==0 */
    uint32_t chunk_index;
    uint64_t chunk_insn_count;
    uint64_t chunk_start_insn;
    uint64_t chunk_bytes_at_open;
    FILE *manifest_fp;
```
and near `static uint64_t insn_limit = 0;` (line 267) add `static uint64_t rotate_interval = 0;`. `finalize_pending_insn`'s `chunk_insn_count++` is added in Task 2; at `rotate_interval==0` it is unused, so leave it out here (adding it now is harmless but Task 2 owns it — either is fine as long as Task 1 stays behavior-identical, which it is since the field is written nowhere read).

- [ ] **Step 3: Replace the install inline block with `open_chunk`**

In `qemu_plugin_install`'s per-vCPU loop, the one-time init (`memset` 1042, `vcpu_id` 1043, `inbuf`/`outbuf` `g_malloc` 1045–1049) STAYS. Replace the block from the `ZSTD_createCCtx()` call (currently ~1051) through the last header `buffer_append(vs, hdr_tail, 4);` (currently ~1088) with:

```c
        if (!open_chunk(vs))
        {
            return -1;
        }
```

Keep the `vs->active = true;` and the `fprintf(... "vCPU %d -> %s")` that follow (adjust the fprintf if it references the now-local `filepath`; replace it with a generic `"[%s] vCPU %d tracing initialized\n"` line since the filename now lives inside `open_chunk`).

- [ ] **Step 4: Replace the atexit inline finalize/close with `close_chunk`**

In `plugin_atexit`, replace lines ~826–839 (the `finalize_pending_insn(vs); flush_final(vs); if(outfile)fclose...; if(cctx)ZSTD_freeCCtx...`) with:

```c
        close_chunk(vs);
```

Leave the subsequent `g_free(vs->inbuf)` / `g_free(vs->outbuf)` and the stats `fprintf` in place (those are one-time per-vCPU teardown, not per-chunk).

- [ ] **Step 5: Build**

```bash
make -C plugin plugin
```
Expected: `Built: champsim_tracer.so`, no warnings from our changes.

- [ ] **Step 6: Prove byte-identical output (the whole point of Task 1)**

```bash
plugin/tests/smoke_capture.sh /tmp/cstf-rot/refactored
zstd -dc /tmp/cstf-rot/refactored/trace_vcpu0.raw.zst | sha256sum
cat /tmp/cstf-rot/baseline_pre.sha
```
Expected: the two sha256 values are **identical** (BIOS boot is deterministic enough for a byte match here since it is the same binary; if the shas differ, the refactor changed behavior — investigate before committing). Also confirm the output is still a single `trace_vcpu0.raw.zst` with no `_c` and no `_manifest.txt`:
```bash
ls /tmp/cstf-rot/refactored/
```
Expected: exactly `trace_vcpu0.raw.zst` (+ `plugin_stderr.log`).

- [ ] **Step 7: Commit**

```bash
git add plugin/champsim_tracer.c
git commit -m "Plugin: factor per-chunk open/close into open_chunk/close_chunk

Behavior-identical refactor (rotate_interval==0): one file per vCPU,
no manifest, byte-identical decompressed output. Prepares for online
rotation. Adds inert VcpuState rotation fields + rotate_interval global."
```

---

### Task 2: Rotation feature (knob, counter, rotation site, manifest)

**Files:**
- Modify: `plugin/champsim_tracer.c` (parse_args knob + usage; `finalize_pending_insn` counter; rotation site in `insn_exec_cb`; manifest open in install loop, close in atexit; install banner)

**Interfaces:**
- Consumes: `open_chunk`/`close_chunk` (Task 1).
- Produces: `rotate=N` behavior per Global Constraints. Test artifacts for later tasks: none required beyond validation.

- [ ] **Step 1: Parse the knob + usage string**

In `parse_args`, before the final `else` (unknown-arg), add:
```c
        else if (g_str_has_prefix(arg, "rotate="))
        {
            rotate_interval = strtoull(arg + 7, NULL, 10);
        }
```
Extend the usage `fprintf` to include `[,rotate=<N>]`.

- [ ] **Step 2: Count per-chunk instructions**

In `finalize_pending_insn`, next to `vs->insn_count++;` (line 463), add:
```c
    vs->chunk_insn_count++;
```

- [ ] **Step 3: Rotation site in `insn_exec_cb`**

After the existing `limit` stop block (lines 561–566, the `if (insn_limit > 0 && vs->insn_count >= insn_limit) { ...; return; }`) and before `InsnMeta *meta = ...` (line 568), insert:
```c
    if (rotate_interval > 0 && vs->chunk_insn_count >= rotate_interval)
    {
        close_chunk(vs);
        vs->chunk_index++;
        if (!open_chunk(vs))
        {
            vs->limit_reached = true;   /* fail safe: stop this vCPU, keep prior chunks */
            return;
        }
    }
```

- [ ] **Step 4: Open the manifest (install loop) when rotating**

In the install per-vCPU loop, **before** the `open_chunk(vs)` call (Task 1 Step 3), add:
```c
        if (rotate_interval > 0)
        {
            char mpath[4300];
            snprintf(mpath, sizeof(mpath), "%s/trace_vcpu%d_manifest.txt",
                     output_dir, i);
            vs->manifest_fp = fopen(mpath, "w");
            if (vs->manifest_fp)
            {
                fprintf(vs->manifest_fp,
                        "# vcpu %d rotation manifest: "
                        "chunk file start_insn insn_count comp_bytes\n", i);
                fflush(vs->manifest_fp);
            }
            else
            {
                fprintf(stderr, "[%s] WARNING: cannot open manifest %s\n",
                        PLUGIN_NAME, mpath);
            }
        }
```

- [ ] **Step 5: Close the manifest (atexit)**

In `plugin_atexit`, after `close_chunk(vs);` (Task 1 Step 4) and after the `g_free`s, add:
```c
        if (vs->manifest_fp)
        {
            fclose(vs->manifest_fp);
            vs->manifest_fp = NULL;
        }
```

- [ ] **Step 6: Banner line**

In the install configuration banner, add a line so every run states its rotation config:
```c
    if (rotate_interval > 0)
        fprintf(stderr, "[%s] Rotation: every %" PRIu64 " insns/vCPU "
                        "(trace_vcpu<V>_c<K>.raw.zst + manifest)\n",
                PLUGIN_NAME, rotate_interval);
    else
        fprintf(stderr, "[%s] Rotation: off (single file per vCPU)\n",
                PLUGIN_NAME);
```

- [ ] **Step 7: Build**

```bash
make -C plugin plugin
```
Expected: clean build.

- [ ] **Step 8: Rotation basic + manifest correctness**

```bash
plugin/tests/smoke_capture.sh /tmp/cstf-rot/rot50k ",rotate=50000"
ls /tmp/cstf-rot/rot50k/
cat /tmp/cstf-rot/rot50k/trace_vcpu0_manifest.txt
for f in /tmp/cstf-rot/rot50k/trace_vcpu0_c*.raw.zst; do
  echo "== $f =="; plugin/trace_inspector "$f" | grep -E "version|Total instructions"
done
```
Expected: multiple `trace_vcpu0_c00000.raw.zst`, `_c00001`, … + `trace_vcpu0_manifest.txt`. Manifest: header comment then one row per chunk, `insn_count` ≈ 50000 for full chunks (last partial); `start_insn` = running sum from 0. Each chunk: inspector `version 3`, ~50000 instructions. Verify `comp_bytes` equals the file size exactly:
```bash
python3 - <<'EOF'
import os
d="/tmp/cstf-rot/rot50k"
rows=[l.split() for l in open(f"{d}/trace_vcpu0_manifest.txt") if not l.startswith("#")]
tot=0; ok=True; start=0
for r in rows:
    ch,fn,st,ic,cb=r
    sz=os.path.getsize(f"{d}/{fn}")
    if int(cb)!=sz: print("BYTES MISMATCH",fn,cb,sz); ok=False
    if int(st)!=start: print("START MISMATCH",fn,st,start); ok=False
    start+=int(ic); tot+=int(ic)
print("rows",len(rows),"total_insns",tot,"bytes_exact",ok,"start_contiguous",ok)
EOF
```
Expected: `bytes_exact True`, `start_contiguous True`, total_insns matching the plugin's stderr vCPU-0 count.

- [ ] **Step 9: Lossless cut (self-contained)**

```bash
python3 - <<'EOF'
import subprocess, glob, os
d="/tmp/cstf-rot/rot50k"
def body(p):
    raw=subprocess.run(["zstd","-dc",p],capture_output=True).stdout
    return raw[16:]              # strip 16-byte header
chunks=sorted(glob.glob(f"{d}/trace_vcpu0_c*.raw.zst"))
concat=b"".join(body(c) for c in chunks)
# stream must parse cleanly to an integer number of records with no split
# (reuse the same record-walk the inspector uses; here just assert nonzero
#  and that concatenation is deterministic across a re-run)
print("chunks",len(chunks),"concat_body_bytes",len(concat))
EOF
```
Expected: prints the chunk count and a nonzero concatenated body size. (A fuller record-walk is exercised by the inspector in Step 8; this step proves headers strip and bodies concatenate.)

- [ ] **Step 10: Backward compatibility (rotate=0 unchanged)**

```bash
plugin/tests/smoke_capture.sh /tmp/cstf-rot/off
ls /tmp/cstf-rot/off/
zstd -dc /tmp/cstf-rot/off/trace_vcpu0.raw.zst | sha256sum
cat /tmp/cstf-rot/baseline_pre.sha
```
Expected: single `trace_vcpu0.raw.zst`, no `_c`, no manifest; sha256 identical to the Task-1 baseline.

- [ ] **Step 11: Edge — rotate > total, and rotate with limit**

```bash
# rotate larger than the whole run -> one chunk, 1-row manifest
plugin/tests/smoke_capture.sh /tmp/cstf-rot/big ",rotate=100000000"
ls /tmp/cstf-rot/big/ | grep -c "_c00000.raw.zst"     # expect 1
grep -vc '^#' /tmp/cstf-rot/big/trace_vcpu0_manifest.txt  # expect 1
# rotate with a limit boundary: smoke_capture hardcodes limit=200000; rotate=100000 -> 2 chunks
plugin/tests/smoke_capture.sh /tmp/cstf-rot/rl ",rotate=100000"
grep -vc '^#' /tmp/cstf-rot/rl/trace_vcpu0_manifest.txt   # expect 2 (200000/100000)
awk 'NR>1{s+=$4} END{print "sum_insns",s}' /tmp/cstf-rot/rl/trace_vcpu0_manifest.txt  # expect 200000
```
Expected: big → one `_c00000` chunk, 1 manifest row; rl → 2 rows summing to 200000.

- [ ] **Step 12: Commit**

```bash
git add plugin/champsim_tracer.c
git commit -m "Plugin: online raw-trace rotation (rotate=N) + per-vCPU manifest

rotate=N closes the current chunk and opens trace_vcpu<V>_c<KKKKK> every
N traced instructions per vCPU; append-on-close manifest records
chunk/file/start_insn/insn_count/comp_bytes (exact file size). Rotation
sits after finalize and after the limit check; empty chunk 0 (trigger
never fires) is omitted from the manifest. rotate=0 unchanged."
```

---

### Task 3: Capture-kit integration

**Files:**
- Modify: `scripts/capture-kit/configure_tracer.sh` (ROTATE env default + thread into run_trace.sh + sidecar key)

**Interfaces:**
- Consumes: the `rotate=` knob (Task 2).
- Produces: generated `run_trace.sh` with `,rotate=$ROTATE`; `trace_metadata.txt` with `ROTATE=<value>`.

- [ ] **Step 1: Add the ROTATE default**

In `scripts/capture-kit/configure_tracer.sh`, in the env-defaults block (near the `VCPUS`/`LIMIT` defaults), add:
```bash
ROTATE="${ROTATE:-100000000}"     # rotate raw chunks every N traced insns/vCPU (0=off)
```

- [ ] **Step 2: Thread into the plugin knob string**

In the generated `run_trace.sh` heredoc, in the `-plugin ...` knob string that currently ends `...,capture_pa=on,values=on`, append `,rotate=$ROTATE` so it reads `...,capture_pa=on,values=on,rotate=$ROTATE`. (Since the heredoc bakes `$ROTATE` at generation time, the literal value appears in the generated script.)

- [ ] **Step 3: Record it in the sidecar**

In the `trace_metadata.txt` heredoc/echo block (alongside `CAPTURE_PA`, `VALUES`, `LIMIT`), add:
```bash
    echo "ROTATE=$ROTATE"
```

- [ ] **Step 4: Validate**

```bash
cd scripts/capture-kit
QEMU=~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64 \
PLUGIN=../../plugin/champsim_tracer.so \
./configure_tracer.sh /tmp/cstf/guest_config.txt /tmp/cstf-rot/kit_out
grep -o 'rotate=[0-9]*' run_trace.sh
grep '^ROTATE=' trace_metadata.txt
bash -n run_trace.sh && echo "run_trace.sh syntax OK"
# ROTATE=0 path
ROTATE=0 QEMU=~/softwares/qemu-9.2.4/build-aarch64/qemu-system-aarch64 \
PLUGIN=../../plugin/champsim_tracer.so \
./configure_tracer.sh /tmp/cstf/guest_config.txt /tmp/cstf-rot/kit_out0
grep -o 'rotate=[0-9]*' run_trace.sh   # expect rotate=0
cd ../..
```
Expected: default run → `rotate=100000000` in both `run_trace.sh` and (as `ROTATE=100000000`) the sidecar; syntax OK; `ROTATE=0` run → `rotate=0`. (`/tmp/cstf/guest_config.txt` exists from the prior feature; if absent, generate a 2-line stub `ARCH=aarch64` / `VA_BITS=48` — configure only needs it to copy through.)

- [ ] **Step 5: Commit**

```bash
git add scripts/capture-kit/configure_tracer.sh
git commit -m "capture-kit: default rotate=100M into generated run_trace.sh + sidecar"
```

---

### Task 4: Documentation

**Files:**
- Modify: `plugin/README.md`, `scripts/capture-kit/README.md`, `converter/README.md`, `CLAUDE.md`

**Interfaces:** consumes everything above.

- [ ] **Step 1: `plugin/README.md`**

Add `rotate=` to the knob table (default 0 = off) and a short "Rotation" subsection: `rotate=N` closes the current chunk every N traced instructions/vCPU, chunks named `trace_vcpu<V>_c<KKKKK>.raw.zst` (5-digit, contiguous — distinct from PIN's skip-gapped `_s`), plus a per-vCPU `trace_vcpu<V>_manifest.txt` (columns `chunk file start_insn insn_count comp_bytes`, one row per non-empty chunk, `comp_bytes` = exact file size). Note every chunk is a standalone v3 file readable by all three tools, and the two stateful-consumer caveats (trace_filter cross-chunk idle under-filter ≤1 iteration/boundary; raw2champsim last-insn `branch_taken=0` per chunk) — for exact whole-stream parity, concatenate first.

- [ ] **Step 2: `scripts/capture-kit/README.md`**

Add a short subsection: rotation defaults ON at 100M insts/chunk (set `ROTATE=0` to disable, or another N via `ROTATE=`), why (bounded file size / corruption blast radius / incremental post-processing), and how to assemble a ~500–600M ChampSim run from chunks using the manifest's `start_insn`/`insn_count` columns (pick contiguous chunks summing to your target).

- [ ] **Step 3: `converter/README.md`**

One line: rotated chunks convert independently (`raw2champsim <chunk>.raw.zst` → `<chunk>.champsim.zst`, `_c<K>` carried through), with the bounded per-chunk-boundary `branch_taken=0` caveat (last instruction of each chunk; convert the concatenated stream for exact parity).

- [ ] **Step 4: `CLAUDE.md`**

One paragraph in the raw-format section: optional `rotate=N` online chunking, `_c<KKKKK>` naming + per-vCPU manifest, default off; point to `plugin/README.md`.

- [ ] **Step 5: Cross-check + commit**

```bash
grep -rl "rotate" plugin/README.md scripts/capture-kit/README.md converter/README.md CLAUDE.md
git add plugin/README.md scripts/capture-kit/README.md converter/README.md CLAUDE.md
git commit -m "Docs: online raw-trace rotation (rotate=N, _c<K> chunks, manifest)"
```

---

## Execution order & parallelism

Strictly ordered: 1 → 2 → 3 → 4 (Task 2 depends on Task 1's helpers; 3 depends on 2's knob; 4 documents all). No parallelism within the plugin file. Task 4 could overlap Task 3.

## Plan self-review record

- Spec coverage: §2 knob/semantics → Task 2; §3 naming → Task 1 (open_chunk filename) + Task 2; §4 manifest → Task 2 Steps 4/5 + close_chunk (Task 1); §5 refactor/fields/site → Task 1 + Task 2 Steps 2/3; §6 kit → Task 3; §7 docs → Task 4; §8 tests → Task 1 Step 6, Task 2 Steps 8–11, Task 3 Step 4; §9 edges → Task 2 Step 11 + close_chunk guard (Task 1). No gaps.
- Consistency: `open_chunk` owns `inbuf_pos=0` (before header), `chunk_start_insn`, `chunk_insn_count=0`, `chunk_bytes_at_open` — matches spec §5.1. `close_chunk` manifest guard `chunk_insn_count>0` — matches §4.3/§9 (kills the blocker). Rotation site after finalize AND after limit check — matches §2.2/§5.3. `comp_bytes = bytes_compressed - chunk_bytes_at_open`, no `flush_*` change — matches §5.2.
- Field/global names used identically across Task 1 (declares) and Task 2 (uses): `chunk_index`, `chunk_insn_count`, `chunk_start_insn`, `chunk_bytes_at_open`, `manifest_fp`, `rotate_interval`.
