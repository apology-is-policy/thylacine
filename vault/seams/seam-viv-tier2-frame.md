---
id: seam-viv-tier2-frame
type: seam
status: open
title: "viv_tier2 unions getdents64's 4.6 KiB of staging into EVERY Linux-phenotype syscall frame, putting that path at 86% of the kernel stack"
surface: [sub-kernel-syscall-abi, sub-kernel-vivarium]
opened-by: chg-2026-09-22-arch81-scripture
tracker: "ARCH 8.1 prerequisite"
created: 2026-09-22
updated: 2026-09-22
---
## Owed

Move the `VIV_GD_RAW` (2048) and `VIV_GD_ENC` (2560) staging arrays out of
`viv_tier2`'s frame -- a `noinline` helper for its `VIV_LINUX_GETDENTS64` case,
or per-thread scratch.

## Why

`viv_tier2` is one `switch`, and clang unions every case's locals into ONE
frame. `-fstack-usage` and the prologue agree: **4720 bytes allocated on EVERY
Linux-phenotype syscall** -- openat, read, write -- when only
`VIV_LINUX_GETDENTS64` touches the buffers.

Measured at `ca1c7030` with `-fstack-usage` over a DWARF-resolved call graph:
the deepest phenotype chain is **14112 B of 16384 = 86.1%**, against a native
worst case (execve -> `exec_load_into` -> `stalk_exec` -> `stalk_core` -> 9P) of
10640 B = 64.9%. 797 `blr` sites stay unresolved, so that is a LOWER bound.

Two reasons this is owed, not merely tidy:

1. **It is a live latent defect today.** At 86% with no IRQ frame involved, any
   deepening of the FS path -- a deeper union, a longer symlink chain, one more
   wrapper -- overflows into the guard. The guard means it FAULTS rather than
   corrupts (#214's memory), but it is a fault an unprivileged program can
   provoke.
2. **It is [[arc-arch81]]'s prerequisite.** That chunk puts an IRQ frame
   (+1728 B: a second exception context plus the handler chain) on the syscall
   stack, taking the phenotype path to 15840/16384 = 96.7%, 544 bytes free.
   With this fixed the worst case becomes the native path at 12368 B = 75.5%.

Nothing in the tree would have reported any of this: there is no runtime
kernel-stack high-water instrument (`stack_peak`, `kstack_high`, poison,
watermark -- zero hits across `kernel/` and `arch/`). [[arc-arch81]] adds one.
