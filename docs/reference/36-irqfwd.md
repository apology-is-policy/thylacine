# 36 — IRQ forwarding [ABSORBED INTO THE VAULT]

This document was absorbed at the interrupt-and-time sweep
(`chg-2026-08-02-devices-interrupt-time-sweep`). Its content now lives,
code-verified and current, in the dossier:

    vault/system/kernel/devices/sub-kernel-irqfwd.md

(the exclusive claim bitmap and why it exists, the kernel's own reserved
interrupt numbers, the arrival hook's lock-then-wake ordering, the saturating
count and the sentinel it must not collide with, the single-waiter refusal, the
interactive promotion, the read-and-zero window, and the three-part teardown
against a live interrupt.)

**This was the best-preserved of the three documents absorbed in this batch** —
it has a real caveats section, and the code cites it by name for its
"stale-fire safety" discussion, which is genuinely there. What follows is
therefore not a stale-document catalogue so much as two specific defects.

**It contradicts itself about the same line of code, twenty lines apart.** The
stale-fire section says, correctly, that `gic_attach(intid, NULL, NULL)` "is
rejected by the gic API (the slot retains `kobj_irq_dispatch` + `arg=k`)". The
test-reuse section immediately below says "The destroy path clears the handler
slot, so subsequent tests can re-attach." The slot is never cleared. What
actually allows the next create to succeed is the **claim bitmap being
released**, a different mechanism in a different file.

That section's failure prediction is then wrong twice over: it says that if a
test fails without destroying, "the next test fails on `gic_attach`". Attaching
over a live slot does not fail — it silently overwrites, which is the exact
hazard the claim bitmap exists to prevent. The next create would fail at the
*claim*, before ever reaching attach. A reader debugging a test failure here is
pointed at the wrong call, expecting the wrong error, for the wrong reason.

**It defers a guarantee the code now has.** The stale-fire section closes with
"A stronger guarantee (synchronous drain-pending in destroy) is held until
concurrent destroy-vs-IRQ becomes a real driver pattern", and lists two guards:
the dying flag and the magic clobber. The synchronous drain exists — teardown
sets the dying flag and then **spins until an in-flight dispatch clears its
in-flight marker**, whose final unlock is that dispatch's last touch of the
object. The identifier appears zero times in this document. So the reference
records a deliberate decision not to build a thing that was subsequently built,
which is worse than silence: it tells a reader the window is open when it has
been closed.

Smaller: the single-waiter refusal is described as returning `-1`; it returns a
named sentinel, and the pending count saturates just below that value precisely
so the two can never be confused — the saturation is not mentioned.

What it got right and the vault kept: the collapsed-count contract, the
single-waiter rationale, the relaxed diagnostic counter, and the observation
that the tests reuse the second inter-processor interrupt number. That last one
stops short of the part that matters — the same number is reserved, in a
commented-out line, for a future cross-CPU cache-invalidation interrupt.

The invariant lives at `vault/invariants/inv-i9.md`. The open debt is
`seam-gic-handler-slot-never-cleared` (no task) — the controller's attach
rejects a null handler by design, so the natural unregister cannot be expressed
and the slot permanently references freed memory, which is what the three
defences stand in for. Design scripture is unchanged: `ARCHITECTURE.md section
9.3`.
