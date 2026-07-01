# snapshots/

## Goal

Intended landing spot for **VM snapshot metadata** that isn't
automatically stored inside the qcow2 image. Currently unused —
snapshots produced by QEMU's `savevm` command are embedded in the
qcow2 file itself (see `images/ubuntu-guest.qcow2`), so this directory
is empty.

## How this fits into the repo

`savevm`-managed snapshots (the common case) live inside the qcow2
image. You list them with:

```bash
qemu-img snapshot -l ~/qemu-tracing/images/ubuntu-guest.qcow2
```

and reference them by name from the QEMU monitor (`loadvm <name>`) or
via the `-loadvm <name>` command-line flag in
`scripts/restore_kvm.sh` and `scripts/boot_tcg_trace.sh`.

This directory exists so that anything *auxiliary* to a snapshot —
notes on when it was taken, what data it contains, the exact QEMU
command line used, a matching guest-side script — has an obvious home.

Historically the project used files named `roi_ready_metadata.txt`
(referenced in the top-level `CLAUDE.md`) as this kind of sidecar.
If you take a named snapshot and want to record its provenance for
other collaborators, drop a `<snapshot_name>_metadata.md` here.

## Files

Empty at present.

## How to use

When you `savevm <name>` a snapshot that you plan to hand off to
another collaborator or use across many runs, drop a companion
markdown file here:

```
snapshots/<name>_metadata.md
```

Suggested content:

- Date taken.
- Full `-cpu`, `-smp`, `-m`, and `-drive` flags at the time of save.
- Workload state (what was running, what data was loaded).
- Which host `scripts/` invocation restores it.
- Any known incompatibilities (e.g., "requires the kvmclock patch —
  see `docs/pipeline/kvmclock-patch-details.md`").

The sidecars are just for humans; QEMU doesn't read them.
