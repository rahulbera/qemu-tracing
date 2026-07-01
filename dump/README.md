# dump/

## Goal

Scratch space for **on-host binary dumps** captured during debugging
of the pipeline — memory dumps from `pmemsave` or `memsave` in the
QEMU monitor, register dumps, extracted qcow2 slices, or any other
one-off binary artifact you want to keep around while chasing a bug.

Currently empty. Kept out of version control (`dump/` is in
`.gitignore`) because these files are large, ephemeral, and specific
to a particular investigation.

## How this fits into the repo

Nothing else in the pipeline depends on `dump/`. It exists so that
when you're debugging a trace mismatch, snapshot corruption, or a
tricky TCG bug, you have an obvious place to drop bytes without
polluting `traces/` or `images/`.

## Files

Empty at present. Anything you put here should be considered
throwaway.

## How to use

Common sources of binary dumps during pipeline work:

- **Guest memory dump** from the QEMU monitor:
  ```
  (qemu) pmemsave 0 0x40000000 dump/guest_ram.bin
  ```

- **Guest registers** (useful when a snapshot restore misbehaves):
  ```
  (qemu) info registers > dump/registers_before.txt
  ```

- **Extracted qcow2 slice** for offline inspection:
  ```
  qemu-img convert -O raw ~/qemu-tracing/images/ubuntu-guest.qcow2 \
      dump/guest_disk_raw.bin
  ```

- **Trace bytes for hand-decode** when the inspector reports
  something odd — take the first few KB with `zstd -d < trace.raw.zst |
  head -c 16384 > dump/head.raw`.

Delete the contents of `dump/` freely; nothing in the build depends
on them.
