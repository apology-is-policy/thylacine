---
id: chg-2026-10-20-thread-model-c2
type: chg
title: "Thread-model C2: the kernel stack cache; the guard toggles invalidate by address"
date: 2026-10-20
arc: arc-phase6-pouch
commits: ["*(pending)*"]
touched:
  - sub-kernel-thread
  - sub-kernel-mmu
  - sub-kernel-mm-phys
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-20
---
The second chunk of the thread-model overhaul (`docs/thread-model-review.html`).
**E-1:** every thread create and every free flushed every CPU's whole TLB
(`tlbi vmalle1is`) to toggle the kernel stack's guard; **E-2:** each create
wrote 48 KiB (the zero-fill, guard included, then the poison). A freed stack
now waits in a bounded LIFO of 32 with its guard still no-access, and the next
create takes it ([[sub-kernel-thread]]): on that path nothing writes a PTE or
invalidates anything. Every stack handed out is poisoned whole, which resets
the watermark and is the only scrub between two owners; the stack is no longer
zero-filled. The toggles that remain -- a miss, a release past the bound --
invalidate the four guard pages by address instead of flushing the TLB, the
operand masked so a kernel address carries nothing into the FEAT_TTL hint
([[sub-kernel-mmu]]). The cache is 1 MiB of the TCB reserve at most
([[sub-kernel-mm-phys]]). Measured: Measured on the Mac under HVF (`--config ci`, the suite's `thread.kstack_create_free_cost`), a create+free cycle costs 924-1018 ns from the cache and 1417-1570 ns from the buddy, against 2802 ns before C2.. E-3, the guard being half of each
thread's kernel memory, needs a kernel-stack VA arena and is its own chunk.
Audit round 1 (Fable 5.1 on Opus 5.5): Fable 5.1, round 1 (Fable reviewing Opus): 0 P0 / 0 P1 / 0 P2 / 4 P3, all closed. F1 fixed: `kstack_release` extincts when the guard restore fails instead of freeing pages that would fault their next user (no regression test: only a corrupted direct map makes the walk fail). F2 fixed: `directmap.guard_toggle_invalidates_a_warm_entry`, the one red test with the invalidate removed (2023/2024). F3 fixed: the three reuse tests drain the cache first, and the first asserts that the free cached exactly one stack. F4 closed: the dossiers ride the landing commit; nothing operator-visible changed, so the manual is untouched..
