---
id: dec-2026-09-22-arch81-design
type: dec
title: "ARCH 8.12: the in-syscall marker is its own field, the unmask stays inside the SVC body, and the kstack measurement pulls viv_tier2's frame forward"
date: 2026-09-22
status: standing
decided-by: autonomous
affects: [sub-kernel-sched, sub-kernel-syscall-abi, sub-kernel-poll]
created: 2026-09-22
---
## Observation

[[dec-2026-09-22-point-now-model-next]] ratified BUILDING ARCH 8.1 as written --
syscall bodies with interrupts on, still non-preemptible -- before the browser
arc's kernel work. This note records the DESIGN that followed, and the three
measurements that forced its shape. Each replaced an assumption I had been
carrying.

**The lock sweep is a clean negative.** The feared work -- converting every
plain lock shared between a syscall path and a same-CPU IRQ handler to
`irqsave` -- has ZERO sites. A read-only sweep at `ca1c7030` built the IRQ-taken
lock set from every `gic_attach` registration and cross-checked ~300 plain
`spin_lock(&...)` sites; all 27 plain acquisitions of an IRQ-reachable lock are
already nested inside an enclosing mask. So the discipline already holds and
what dies is its stated ARGUMENT: a doc sweep, not a code sweep.

**`preempt_count` cannot carry non-preemptibility.** Three live assertions
forbid a syscall-wide count, not one: `sched.c:1246` (lock-across-sleep -- every
blocking syscall calls `sched()`), `sched.c:2653` (the point; moot), and the
decisive one, `proc.c:4441` + `sched.c:241` (#361), where `el0_return_die_check`
extincts on "counted spinlock leaked to EL0 return" -- a syscall-wide count is
definitionally that leak. I had found only the first and had to be shown the
third by reading the code.

**The kernel stack was unmeasured, and the measurement moved the chunk.**

## Decision

- **A SEPARATE per-thread marker**, set at SVC entry, cleared before the
  EL0-return tail. `preempt_check_irq` gains one early return on it; `sched()`
  ignores it. REJECTED: deciding from the interrupted frame (Linux
  `user_mode(regs)`) -- it over-applies, and kthreads must stay preemptible or
  #810 is lost.
- **Unmask inside the SVC body ONLY; re-mask before `.Lel0_sync_return`.**
  `vectors.S:126-131` (KERNEL_EXIT) sets ELR/SPSR and `eret`s under an
  INHERITED mask -- the one surviving #713-class window that does not mask
  locally. An unmask that leaks into the return tail resurrects #713.
- **`viv_tier2`'s getdents64 staging buffers move out of its frame, as a
  prerequisite pulled forward.** Measured: the Linux-phenotype syscall chain is
  14112 B of 16384 (86.1%) because clang unions every switch case's locals into
  one frame, so all 4720 B are allocated on every phenotype syscall when only
  getdents64 uses `raw[2048]` + `enc[2560]`. Adding the IRQ frame (+1728 B)
  takes it to 96.7%, 544 bytes free. With the fix the worst case becomes the
  native execve path: 12368 B = 75.5%. See [[seam-viv-tier2-frame]].
- **The kernel stack stays 16 KiB.** Growing it to 32 KiB would double
  per-thread kernel memory against an I-32 bound to buy headroom the
  measurement says is not needed. Reversible if the watermark disagrees.
- **Two instruments land with the chunk**, because both premises are
  load-bearing only in prose: a debug assert on interrupt state (zero such
  asserts exist anywhere in the tree) and a runtime kernel-stack watermark
  (zero such instruments exist either -- nothing would have reported 86% before
  it was asked).

## Consequence

`sched_preempt_point` and poll's call to it are deleted, and with them
`specs/poll_cpu.tla` and its four cfgs -- that module's stated premise IS the
masked syscall, so it becomes VACUOUS rather than false. Five kthread-flag spin
loops (`loom.c:322`, `irqfwd.c:288`, `pci_irq.c:480`, `proc.c:5431`,
`gic.c:239`) stop being latent single-CPU hangs, which was not what the chunk
was aimed at. ARCH 8.11's rationale must be rewritten WITH the code, not before
it: the two-tier "masked spinners behind a preemptible holder" model collapses
to one tier, and the argument has to be rebuilt on `preempt_count` alone.

The spec obligation is NOT the lock sweep. It is "no involuntary switch inside
a syscall body" plus "the re-mask precedes the eret window".
