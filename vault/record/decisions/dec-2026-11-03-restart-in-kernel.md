---
id: dec-2026-11-03-restart-in-kernel
type: dec
title: "SA_RESTART lives in the kernel, for Pouch and for Linux guests"
date: 2026-11-03
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-vivarium, sub-kernel-pipe]
created: 2026-11-03
---
## Fork

ARCH 8.8.3 and VIVARIUM 6.22 said musl re-issues a call whose handler had `SA_RESTART`. It does not: `SA_RESTART` only sets `__eintr_valid_flag`, which `__timedwait` reads to filter a futex `EINTR` (`third_party/musl/src/thread/__timedwait.c:58`), and no Pouch patch adds a loop. So nothing honoured the flag, and a Linux guest saw `EINTR` where Linux restarts, listed as DEGRADED.

Options: kernel rewind for Pouch and Linux guests; the same for Pouch only; a new `SYS_NOTED` argument by which Pouch's dispatcher asks for the re-issue. A libc loop was not offered: it would have to sit at every blocking call site, and the dispatcher cannot rewind, because the restore comes from the kernel's save.

## Decision

Vote 19 of 2026-11-03, as recommended: at delivery the kernel rewinds `pc` and restores `x0` when the call is restartable under signal(7) and the note's row carries restart, on a per-thread mark set when the wait really unwound. It never reads `x0` alone, so a 9P server's `Rlerror(4)` is not a restart. Linux's `do_signal` is the shape, and Starnix's per-task record the reason for the mark. Linux guests stop seeing those `EINTR`s, and that behaviour change owes its own witnesses.
