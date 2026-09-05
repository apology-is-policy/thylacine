---
id: chg-2026-08-18-extinction-ring-claim
type: chg
title: "The prescription named the wrong lock, and the old one was already breaking the crash path"
date: 2026-08-18
arc: arc-vault
commits: ["7dd5be19"]
touched: [sub-kernel-cons, abi-boot-banner, sub-kernel-exception]
established: []
closed: [seam-extinction-line-unserialized]
opened: []
mirrors-checked:
  - "tools/test.sh"
  - "tools/smp-multiboot.sh"
  - "tools/test-cross-reboot.sh"
  - "tools/test-fault.sh"
  - "tools/ci-idle-gate.sh"
  - "tools/np3-bench.sh"
  - "tools/verify-kaslr.sh"
  - "tools/warp/boot-probe.sh"
  - "tools/interactive/lib.exp"
  - "tools/interactive/dap-nora.exp"
  - "tools/interactive/flood-174.exp"
  - "tools/interactive/freeze-172.exp"
  - "tools/interactive/ls-gfx-font.exp"
  - "tools/warp/quarry-wedge.exp"
  - "tools/stall-watch.py"
depth: rich
created: 2026-08-18
---
`seam-extinction-line-unserialized` asked for a delivery guarantee on the crash
literal, and prescribed one: a **try**-acquire of the console **writer role**.
The guarantee landed. The prescription did not — it named the wrong lock, and
the seam's framing missed two things that were larger than the tear it opened
for.

## The prescribed remedy was a hypothesis

The writer role serializes whole `cons_output_write` calls. But **the drain
never consults the role** — that is main#144, and `cons.h` already said so.
Bytes a peer had *already pushed* still pop into the FIFO from cpu0's TX IRQ or
from a peer's `cons_tx_kick`, so they land inside the banner while the role sits
held. Taking the role would have produced a guarantee that reads correct and is
not.

What owns the wire is the **ring lock**: every steady-state producer pushes its
unit under `g_cons_tx.lock`, and every ring→FIFO drain pops under the same lock.
So the winner takes that and never lets go — IRQs masked first (its own TX IRQ
arm would otherwise self-deadlock on the held lock), a **bounded raw try-spin**,
the whole pre-crash ring flushed under it, then held through the banner and the
dump to `_torpor`. Past the bound it emits unserialized and says so afterwards.

Every property is the inverse of the console-word claim one file over, for one
reason — **who holds the thing you are waiting for**. That word's holder is a
dying peer that never releases, so try-once is right. The ring's holder is a
*healthy* peer that releases in microseconds, so a bounded spin is right and a
try-once fails in exactly the case it exists for.

## The surface had a second emitter, and the mirror set did not know

`arch/arm64/exception.c::el1_sync_runaway` prints `EXTINCTION: el1-sync
recursion …` **without going through `extinction()`**. It was in neither
serializer — not the 2026-08-16 console-word fix, and not this surface's
`mirrors` set, which is why `quaestor owner` reports it as matching the literal
from *outside* the set. It now takes both.

It surfaced by deleting the old symbol and letting the build fail — not by the
grep census that ran first and missed it. **A rename is a census that cannot
lie.**

## The old flush was already breaking the crash path, in silence

The predecessor took the ring lock with the **counted** `spin_trylock`, whose
`spin_preempt_inc` dereferences `current_thread()` through TPIDR_EL1. The
`recursive_kernel_fault` fault variant installs a wild TPIDR **on purpose** —
that is its entire premise. So the flush faulted *inside* the extinction path;
the nested EL1-sync faults climbed to the runaway, which called the same flush
and faulted again; and the depth-past-max arm parks silently.

On the base tree that variant emitted **nothing at all**, and had done for about
a month. Measured by stashing the fix and running it, with a counted-trylock
control reproducing the symptom byte-identically. Filed main#244.

The rule: **a dying-machine path may not call a primitive that reads state the
crash may have destroyed.** The counted-spinlock retrofit inherited every
existing `spin_trylock` caller, including one on the extinction path, and nobody
re-asked whether that caller could survive the new work.

## The mirror re-check, and why every one of the fifteen is unaffected

**No literal changed.** The change emits the same `"EXTINCTION: "` prefix and
the same seven message bodies; `git show 7dd5be19 --stat` touches **no file
under `tools/`** at all. The two lines it adds (`console-ring: held …` /
`console-ring: NOT held …`) are deliberately worded to the existing peer-report
rules — no `EXTINCTION` token, nothing at column 0 — and print *after* the
banner and the dump, so nothing anchored on the line can see them. Grepping all
fifteen mirrors for `console-ring` returns nothing: no consumer reads them.

Each mirror was re-grepped for the three literals and still matches (hit counts
in the same order as the list: 10, 6, 5, 21, 2, 3, 7, 4, 9, 1, 1, 1, 1, 1, 2).
`tools/test-fault.sh`'s twenty-one include the seven full MESSAGE bodies this
surface warns about; none is reworded here.

What the change *does* alter for these consumers is **delivery, not value** —
which is the seam's own lesson, arriving from the other side: the banner is now
serialized against every ring writer, so the tear that could demote a real
extinction to the classifier's unclassified bucket is gone for that class. The
one consumer whose behaviour materially changes is `tools/test-fault.sh`, and it
changes from **failing** to **passing** on `recursive_kernel_fault` (main#244).

## What this close does not cover

The ring lock reaches only writers that go *through* the ring. Steady-state
kernel diagnostics that still call `uart_puts` directly (`sched.c`,
`syscall.c`, `exec.c`, `9p_client.c`) sit outside it — main#243. `IPI_HALT`
(a commented-out reservation) would subsume them and both closed sources.

And the gate that would have caught main#244 is **wired into no gate at all**:
`tools/test-fault.sh` is manual-only, grep-proven across the Makefile,
`ci-smp-gate.sh`, `test.sh`, `test-interactive.sh` and `.github`. It is the only
runtime witness that W^X, BTI, the stack guards and the #806 guard actually
fire. main#245.
