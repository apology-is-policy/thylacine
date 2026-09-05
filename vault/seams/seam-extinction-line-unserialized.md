---
id: seam-extinction-line-unserialized
type: seam
title: "The fix protected the banner and left the string 14 of 15 consumers actually match"
status: closed
surface: abi-boot-banner
opened-by: chg-2026-08-16-cons-writer-set
tracker: "unfiled -- yip to main 2026-08-16"
created: 2026-08-16
updated: 2026-08-16
closed-by: chg-2026-08-18-extinction-ring-claim
---
## Owed

A delivery guarantee for the crash-line literal, or a recorded decision that it
does not get one.

The console tearing defect — a kernel emitter holding no writer role, interleaved
byte-wise by a peer writing the same device — was closed for the boot-success
line by enrolling that emitter in the role. The crash path was **deliberately
excluded**: it runs on a dying machine and must stay lock-free and bounded, so a
primitive that can park is exactly wrong there.

That reasoning is correct. The consequence was not written down, and it inverts
the fix's value.

**Classified by which literal each of the fifteen declared mirrors actually
matches**: eight match the now-serialized success line; **fourteen match the
still-unserialized crash prefix**; one matches only the base-address line. The
string that received the guarantee has roughly half the readership of the string
that did not.

The crash emitter uses the same lock-free byte-at-a-time path the banner used,
does **not** halt peer processors before printing, and its pre-emit ring flush is
a bounded try-lock that *skips* when a peer holds the lock — so the one case it
declines to handle is precisely a peer mid-write.

## What closes it

A **try**-acquire of the writer role rather than a park: take it if free, emit
unserialized if not. That preserves every property the exclusion was protecting
— no parking, bounded, non-recursing — while covering the common case where the
peer is not actually inside a write. It is the same shape the pre-emit flush
already uses, in the same file.

Or a deliberate record that a torn crash line is accepted, with the two costs
below stated, so the next reader is not surprised by them.

**Not a vault edit.** Both files are on the implementation branch.

### CLOSED 2026-08-18 by `7dd5be19` — and the prescribed remedy was wrong in one specific

**The prescription above named the wrong lock.** The writer role serializes
whole `cons_output_write` calls, but the **drain never consults the role**
(main#144, stated in `cons.h`): bytes a peer had already pushed still pop into
the FIFO from cpu0's TX IRQ or a peer's `cons_tx_kick`, landing inside the
banner while the role sits held. A prescribed remedy is a hypothesis.

What owns the wire is the **ring lock** — every producer pushes its unit under
`g_cons_tx.lock` and every ring→FIFO drain pops under it. So the winner takes
that (`cons_tx_claim_for_dump`): IRQs masked first, a **bounded raw try-spin**
(20 ms + a `1<<20` backstop), the whole pre-crash ring flushed under it, and
then **held forever** — every path through `extinction()` ends in `_torpor`, and
a release would let the next push land mid-banner. Past the bound it emits
unserialized and reports the miss after the dump. Raw, not counted, because the
counted variants deref `current_thread()`.

**Two things this seam's own framing did not reach:**

1. **A second emitter of the literal.** `arch/arm64/exception.c::
   el1_sync_runaway` prints `EXTINCTION: el1-sync recursion …` without going
   through `extinction()`, and was in *neither* serializer — nor in this
   surface's `mirrors` set, which is why `quaestor owner` reports it as
   matching the literal from outside the set. It now takes both. It surfaced
   from deleting the old symbol and letting the build fail, not from a grep
   census: **a rename is a census that cannot lie.**
2. **The old flush was already breaking the crash path, silently.** Its
   *counted* `spin_trylock` derefs `current_thread()`, and
   `test-fault.sh recursive_kernel_fault` installs a wild `TPIDR_EL1` on
   purpose — so on the base tree that variant emitted **nothing at all** for
   about a month (fault inside the extinction path → nested EL1-sync faults →
   the depth>MAX arm parks silently). Measured by stash, with a
   counted-trylock control reproducing it byte-identically. main#244.

**Residual, so the close is not read as total:** the ring lock reaches only
writers that go *through* the ring. Steady-state kernel diagnostics that still
call `uart_puts` directly (`sched.c`, `syscall.c`, `exec.c`, `9p_client.c`) sit
outside it — main#243. Source 3 (`IPI_HALT`) would subsume them. And
`test-fault.sh`, the gate that would have caught #244, is wired into no gate at
all — main#245.

## Risk while open

Two costs, different in kind, and neither degrades gracefully.

**A torn prefix loses a corruption verdict.** Every consumer checks the
start-of-line prefix first on every poll, and the multi-boot classifier keys its
corruption class on it. A tear does not produce a missing result — it produces
the **unclassified** bucket, which is the classifier's most expensive verdict and
the one it was redesigned to stop over-producing. So a real corruption is
demoted to "unexplained", from outside the classifier, by a mechanism nothing in
the classifier can see.

**A torn message body inverts a fault-injection result.** The fault gate matches
seven full crash-message strings — seventeen matches in that one file. A torn one
reports that a protection **did not fire**, on a run where it fired correctly.
That is a false negative on a safety mechanism, which is the worst direction a
gate can fail in.

Both are rare and neither leaves a trace distinguishable from the real failure
it imitates, which is what makes this worth a record rather than a note in
passing.

## Why the existing machinery could not catch it

The mirror check on this surface is derived, fails rather than warns, and carries
a positive control. It reasons entirely about **who reads the literal** — and is
right to, since that is what a value contract needs.

Nothing in it, or in any stricter version of it, addresses the string *arriving
intact*. **A contract on a value is silent about its delivery**, and the surface
note stated the delivery requirement in its first paragraph ("must appear on a
line by itself") while carrying no obligation that would produce it — because
that sentence reads as a property of the emitter and is actually a joint property
of the emitter and every concurrent writer of the device.

This was found only because the mirror rule forced an enumeration that had no
other reason to happen.
