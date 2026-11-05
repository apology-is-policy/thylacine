---
id: chg-2026-11-03-c10a-kstack-range
type: chg
title: "Each thread's kernel stack in a range of its own, its guard costing no memory; the 0x200 vector reads SP"
date: 2026-11-03
arc: arc-phase6-pouch
commits: [0a5d6e513, d0a545836]
touched:
  - sub-kernel-thread
  - sub-kernel-mmu
  - sub-kernel-exception
  - sub-kernel-sched-smp
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-03
---
NOT LANDED, and nothing here has been built past the object files or booted. Thread-model C10 (E-3): half of every thread's kernel memory was guard, because a guard had to be whole pages of a direct-map block. A stack is now 32 KiB of a range of its own, the low half unmapped and the high half four order-0 pages, so a thread costs five pages where it cost nine and a fragmented buddy refuses no thread.

The plan said one guard page. It stays 16 KiB of address space: that costs the same nothing, keeps the guard larger than any frame, and a span twice the stack, aligned to itself, is what lets the vector tell a guard from a stack by one bit. That is the geometry of Linux's arm64 `VMAP_STACK`.

Pulled forward, as a dependency: the 0x200 vector's SP test and a per-CPU overflow stack, the hardening `g_exception_stacks` had been reserved for. Today an overflow's recursive frame stores nothing until SP re-enters mapped memory, and the direct map guarantees it soon does, so the C handler runs and prints. In a sparse range SP may fall through unmapped address space for as long as it lasts: a silent spin in place of the extinction. The gate's `kstack_overflow` now expects the body only the vector path prints, since a stack with a mapped slot below would otherwise pass by descending into it. `overflow_stack_guard` is new.

`abi-boot-banner` gains a line on those two expectations. No consumer changes what it matches: every body keeps the `kernel stack overflow` prefix, and the boot and idle bodies are as they were.

Census: every `kva_to_pa` in `kernel/`, `arch/` and `mm/`. None takes a stack address but the release that this replaces. Read, not run.
