---
id: moc-kernel-boot
type: moc
title: "Boot — establishing what everything after it assumes"
parent: moc-kernel
created: 2026-08-02
updated: 2026-08-02
---
The path from the bootloader's first instruction to `Thylacine boot OK`: the
image header and the pre-C stub ([[sub-kernel-boot-entry]]), the randomized
kernel base ([[sub-kernel-kaslr]]), the hardware description everything else is
derived from ([[sub-kernel-dtb]]), the boot-time rewrite of the atomic baseline
([[sub-kernel-alternatives]]), and the ordering contract that composes them
([[sub-kernel-boot-sequence]]).

## The organizing fact

**Boot is the region where the tools' assumptions are not yet true.** Everywhere
else in the kernel, four things can be taken for granted; here, each one has to
be established, and every local oddity in this area is one of them being worked
around.

| The assumption | Where it becomes true | What the boot path does instead |
|---|---|---|
| An address means what the linker said | after the long branch to high VA | resolves everything PC-relative, caches load-PA bounds in `volatile` storage |
| Memory is Normal, cacheable, unaligned-tolerant | after `mmu_enable` | constrains every DTB read to a `volatile` 4-byte load |
| A constant can be shared by `#include` | never, across asm / C / linker | duplicates the value and freezes the C side with a `_Static_assert` |
| A test suite exists to run | after ~40 init calls succeed | is itself the test — the suite running at all is the evidence |

The first two produced worked failures where **the compiler defeated the code**,
both fixed with `volatile`, both because a rule the compiler is entitled to
assume is false in this window. Under PIE, clang treats `&_kernel_start` as a
link-time constant and folded a store-then-load into a boolean plus that
constant — correct everywhere except before the MMU, where PC-relative
addressing yields the load PA and the two differ. And pre-MMU kernel data
accesses are Device-nGnRnE, which mandates natural alignment; clang was observed
fusing two adjacent 4-byte DTB reads into one 8-byte load, which faults.

Neither is a compiler bug. Both are the same shape: **this code runs on a
machine the compiler is not modelling.**

## Sequencing is the contract

[[sub-kernel-boot-sequence]] is unusual for a dossier because its subject is an
*order*, not a mechanism. Roughly forty initializers run in a sequence where
almost every position is load-bearing, and the dependency is recorded beside each
call rather than in a graph. The sharpest ones:

- The instruction patcher runs strictly **before** secondaries exist, so nothing
  can execute a half-rewritten site.
- The identity map is retired strictly **after** they exist, because their
  trampoline runs through it.
- The wall-clock anchor is written **before** SMP, which is what makes a single
  unsynchronized `u64` sufficient.
- The test suite runs with cross-CPU wakeups and secondary preemption **off**,
  and they are enabled immediately after — so the suite is deliberately UP-like
  and [[gate-smp]] is the compensating control.

## What is single-CPU, and why it matters

Almost everything here runs before `smp_init`, which is not incidental — it is
the enabling premise for several designs that would otherwise need locks. The
patcher's scratch mapping needs no lock. The feature word needs no publication
barrier. The DTB parser's cached state is written once and read forever. Each of
those is sound *because* of where it sits in the sequence, so **moving a call in
[[sub-kernel-boot-sequence]] can invalidate a concurrency argument made in
another file**, with nothing local to catch it.

## Cross-links

Secondary CPU bring-up — the PSCI trampoline, `per_cpu_main`, and the
online/alive handshake — is owned by [[sub-kernel-sched-smp]]; this area covers
the primary path only. The MMU tables the stub enables, and the W^X enforcement
the patcher must not violate, belong to the memory area (unswept).
