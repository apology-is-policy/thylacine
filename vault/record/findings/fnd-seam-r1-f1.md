---
id: fnd-seam-r1-f1
type: fnd
round: adt-seam-r1
severity: P0
status: fixed
title: "The cancellable syscall path had no sentinel guard — a retargeted cancellation point issued svc with x8=0xFFFF"
surface: [sub-pouch-seam]
threatens: []
fixed-by: chg-2026-05-22-p6-syscall-seam
regression: "the `build_sysroot` grep for the guard in the patched `__syscall_cp.c`"
created: 2026-08-01
---
## Prosecution

1. The sentinel guards are written into `__syscall0..6` in
   `arch/aarch64/syscall_arch.h` — the NON-cancellable path.
2. musl's *cancellable* path is different code: `syscall_cp` ->
   `__syscall_cp` -> `__syscall_cp_asm`, a hand-written `.s` file that
   carries no guard and was not patched.
3. So a cancellation-point call retargeted to the sentinel — `nanosleep`,
   `read`, `open`, any of them — issued `svc` with `x8 = 0xFFFF` on a
   thread with cancellation enabled.
4. A foreign syscall number reaches the kernel: **P-1 broken**, on the
   path a threaded program actually uses.

## Fix

The same guard in `__syscall_cp` — the single C chokepoint every
cancellable call resolves through, upstream of both the weak
(`sccp` -> `__syscall`) and strong (`__syscall_cp_asm`) backends. P-1 is
now true on both paths, and the build greps the patched file for the
guard as a structural regression check.

The shape is the lesson: a guard placed on the path you are reading is
not a guard on the mechanism. Two paths existed; only one was obvious.
