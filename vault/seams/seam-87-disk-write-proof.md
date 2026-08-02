---
id: seam-87-disk-write-proof
type: seam
title: "A reused disk.img disarms virtio-blk-rw's write proof"
status: open
surface: [sub-substrate-interactive, sub-substrate-gates]
opened-by: chg-2026-08-01-substrate-sweep
tracker: "#87"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A guest-side fix that makes `usr/virtio-blk-rw`'s write proof independent of
the fixture's prior contents — a fresh pattern per boot (derived from a
counter, a timestamp, or the boot's own entropy) rather than a fixed
pattern B.

## The failure

The probe runs pass A read-verify / pass B write / pass C read-back, and
pass C is the only proof the write landed.

- On a **fresh** disk the write region holds pattern A, so a silently
  dropped pass B makes pass C read A != B and FAIL. Correct.
- On a **reused** disk the region already holds pattern B from the previous
  boot, so the same dropped write reads a stale B and **PASSES**.

Reusing the fixture therefore disarms the proof entirely — the assertion is
satisfiable by the broken system ([[haz-harness-fail-open]]).

## What partially closes it today

LS-CI restores `disk.img` from a pristine twin per attempt, making every
LS-CI boot a "boot 1" so the leg can fail again there. That is a
**mitigation, not the fix**: `tools/test.sh` and `tools/smp-multiboot.sh`
share the same fixture and stay exposed. Unlike the pool there is no
`build.sh`-maintained twin, so LS-CI mints one with `mkdisk.py`
(deterministic, ~0.45 s) and clones from it thereafter.

## Risk while open

A dropped or mis-addressed block write in the virtio-blk path passes the SMP
gate and the default boot silently, on every boot after the first. The
exposure is exactly the gates that run the most.
