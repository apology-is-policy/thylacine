---
id: fnd-pouchb0-r1-f3
type: fnd
title: "the 0033 "device-side pin" compares libc's literals with the prover's own copy of them, never with the kernel"
round: adt-pouchb0-r1
severity: P2
status: fixed
surface: [sub-pouch-thread, sub-kernel-exec]
threatens: []
fixed-by: chg-2026-09-21-pouch-b0-libc
regression: "pouch-hello-threads: the reported bounds must equal the single `stack` row of /proc/<pid>/maps, read at run time; the prover holds no literal. RED measured with a 2 MiB mirror linked ahead of libc.a: libc [0x7fe00000, 0x80000000) kernel [0x7ff00000, 0x80000000)"
created: 2026-09-21
---
## Prosecution

**File**: `usr/pouch-hello/pouch-hello-threads.c`; 0033's header; `kernel/include/thylacine/exec.h`
**Invariant**: a control must prove discrimination -- a mirror pinned to a copy of itself cannot go red
**Prosecution**:
1. 0033 states the main thread's stack as two constants that MIRROR `EXEC_USER_STACK_TOP` / `_SIZE`.
2. The prover's second half compared libc's answer with the same two literals typed again in the prover.
3. When the kernel's constant moves and libc's does not, both copies still agree: green, and wrong. Five sentences in the header, `exec.h`, the series and two dossiers claimed the kernel was pinned.
**Suggested fix**: read the kernel's own answer at run time.

## Disposition

Fixed: the prover parses the `stack` row of `/proc/<pid>/maps` (exactly one such row or it refuses) and compares; the five sentences corrected.
