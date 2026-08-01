---
id: adt-stube2-r1
type: adt
title: "P5-stratumd-stub-bringup e1 + e2 (SYS_WALK_OPEN + SYS_CHROOT) focused round"
date: 2026-05-21
scope: [sub-kernel-territory, sub-kernel-stalk, sub-kernel-ninep-attach]
reviewer: opus
model-start: "claude-opus"
model-end: "claude-opus"
verdict: dirty
counts: {p0: 1, p1: 1, p2: 2, p3: 3}
findings: [fnd-stube2-r1-f1, fnd-stube2-r1-f2, fnd-stube2-r1-f5, fnd-stube2-r1-f6]
round-of: chg-2026-05-21-p5-chroot
created: 2026-08-01
---
## Scope

The first walk-and-open syscall plus the root pivot, prosecuted
together: `sys_walk_open_handler` (including the FROM_ROOT path),
`sys_chroot_handler`, `territory_chroot` / `territory_clone` /
`territory_unref`'s root_spoor handling, the struct asserts, and the e1
`dev9p_walk` failure-path aux discipline carried in as a preamble.

## Convergence

A DIRTY close — one P0 and one P1, both on the newly-reachable walk
path rather than on the territory bookkeeping the chunk was about.

The P0 ([[fnd-stube2-r1-f1]]) was a kernel-stack information leak: a
name buffer sized exactly to the maximum, with a CONDITIONAL NUL write
that skipped at exactly the maximum length, so `dev9p_walk`'s strlen
scan ran off the scratch into adjacent stack until it found a zero byte
— and shipped the discovered length over the wire. Its root cause was a
comment ([[fnd-stube2-r1-f6]]) asserting the terminator was
"defense-in-depth" when the Dev vtable has no length array and the
terminator is REQUIRED. A wrong comment about why something is optional
is how it becomes optional.

The P1 ([[fnd-stube2-r1-f2]]) was the walked-Spoor UAF the previous
review series had DEFERRED to "the SYS_WALK chunk" — this one. It
reproduced through a pure syscall sequence, because `proc_free` runs
`territory_unref` then walks the handle table ASCENDING, so the attach
fd closes before the walked fd and frees the client the walked Spoor
still points at. Deferred hazards come due at the chunk that makes them
reachable.

The territory-facing finding is [[fnd-stube2-r1-f5]] — asserting a
struct's SIZE while leaving its field offsets unpinned. The chroot
refcount discipline itself (bump-before-swap, clunk-after, idempotent
same-pointer) came through unbroken and is what the extended
`MountRefcountConsistency` now pins.
