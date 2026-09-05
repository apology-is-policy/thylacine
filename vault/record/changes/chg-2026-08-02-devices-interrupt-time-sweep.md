---
id: chg-2026-08-02-devices-interrupt-time-sweep
type: chg
title: "vault sweep: the interrupt and time path -- the devices whose failure has no observer"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-gic
  - sub-kernel-timer
  - sub-kernel-irqfwd
  - inv-i18
  - inv-i15
  - inv-i17
  - inv-i9
established: []
closed: []
opened:
  - seam-gic-handler-slot-never-cleared
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 19, and the first of two or three sweeps of `devices/` -- the LAST empty
area declared at commit 0. Read from code: `arch/arm64/gic.c` (968) + `gic.h`
(237), `arch/arm64/timer.c` (321) + `timer.h` (193), `arch/arm64/rtc.c` (52) +
`rtc.h` (43), `kernel/irqfwd.c` (393) + `irqfwd.h` (124) -- 2331 lines. Not yet
swept: the hardware-capability objects, the virtio/PCI transports, the synthetic
device filesystems. The area MOC says so at the top rather than implying
completeness.

WHY THIS BATCH. L-1 still had not reached main (SEVENTH check -- `addrspace.h`
absent), so address-space stayed deferred again. `devices/` was the only empty
area left; the interrupt-and-time third is the coherent first slice (it is the
hardware the kernel cannot delegate, and it homes I-18).

THE ORGANIZING FACT: **these are the two devices whose failure has no
observer.** Everywhere else a broken device eventually produces a wrong value
someone reads. Not here. A timer that stops firing does not report anything --
the system stops preempting and looks like a slow machine. A dropped
inter-processor interrupt logs nothing -- a CPU idles a fraction of a second
longer. A doubled interrupt is one wake to the driver. So the whole area
substitutes **redundancy for detection**: the message is a flag the sender
already published (the IPI is only promptness), an idle CPU arms a backstop and
re-checks anyway, every CPU arms its own timer so there is no central arming to
fail, and the forwarded count is deliberately collapsed to "at least one".

The visible consequence is **five counters in 2331 lines** -- per-CPU interrupt
totals, per-CPU IPI receipts, forwarded fires, live forwarded objects, ticks.
None is load-bearing. Each exists because the event it counts has no other way
to be seen. In a region where correctness cannot be observed directly, counting
is how the tests get a foothold at all.

THE HEADLINE IS ABOUT THE VAULT, NOT THE CODE. **A code comment was falsified by
contradiction with a dossier written the previous batch, before the code was
re-read.** `ipi_resched_handler`'s comment states that "at v1.0 P2-Cdc no
cross-CPU placer exists yet (P2-Ce work-stealing introduces it); IPI_RESCHED
here is purely a 'wake from WFI' signal proving the SGI delivery path works."
[[inv-i18]], written at the scheduling sweep, already enumerated THREE senders
-- including `sched_notify_cpu`, which exists precisely to wake a specific
target after a cross-CPU placement. Confirmed against the tree afterwards
(`sched.c:768`, called from `:895`). This is the eleventh instance of the
stale-summary class and the first the corpus caught on its own; the framing is
also the dangerous kind, since "purely a proof-of-delivery signal" is what gets
code deleted.

I-18 SHARPENED, AND THE BREAKING CONDITION MOVED. The existing `blind-to:` note
said the model's queue diverges from the hardware's coalescing, and named
*payload* as the condition under which the invariant would need re-deriving.
Reading the driver shows the nearer condition is **multiplicity**. Pending state
lives in set-pending / clear-pending REGISTERS -- one bit per interrupt number,
addressed `1u << (intid % 32)`. A bitmap cannot hold a second occurrence of one
number, and cannot hold the arrival order of two different numbers either; nor
is there a tiebreak, since the driver writes one uniform priority to every
interrupt. So with a single IPI the statement holds VACUOUSLY -- there is no
pair of sends to order -- and a second IPI breaks it whether or not either
carries information. Three more are already reserved in commented-out lines, and
the first of them names the SGI **the test suite is already claiming through the
normal capability path**, with nothing connecting the two. A cross-CPU TLB
invalidation would also be the first IPI whose *completion* matters to the
sender, at which point neither the statement nor `scheduler.tla` describes what
the hardware provides.

THE SEAM: A SAFETY RULE REMOVED THE MECHANISM ITS OWN LIFECYCLE NEEDED. The
controller's attach entry point rejects a null handler by design, so that
detaching goes through the explicit disable rather than through an attach that
quietly arms a future fatal error. Good rule -- and it leaves no way to say the
other thing. Teardown of a lent interrupt calls `gic_attach(intid, NULL, NULL)`
anyway; the comment beside it says plainly that this returns false and the slot
keeps its handler and its argument, which is the object about to be freed. **The
slot permanently points at freed memory.** Three overlapping defences stand in
for the missing operation: disable first, a dying flag plus an in-flight marker
with a bounded spin (for the one arrival disabling cannot prevent -- already
acknowledged, executing on another CPU), and a magic clobber before the free.
Nothing is wrong: every enable site in the tree is preceded by an attach that
overwrites the slot (census: 5 attach sites, 6 enable sites). What is worth
recording is the direction -- the cost was paid in the caller as three
distributed guards rather than in the interface as one more entry point, and the
convention that keeps it safe (attach always precedes enable) is nowhere
enforced. [[seam-gic-handler-slot-never-cleared]], no task.

REGISTRY TAIL: **MINTED NOTHING.** The first batch of the sweep to establish no
invariant at all, and it is the right outcome rather than a gap: both homes here
were already minted by neighbours -- I-18 by the scheduling sweep, I-15 by the
boot sweep the batch before -- and I-5 (hardware handles non-transferable, whose
enforcement includes the reservation of the controller's own registers against a
capability holder) belongs to the hardware-capability objects in the NEXT
devices batch. Extended four instead: I-18 (+gic, + the multiplicity note), I-15
(+gic, +timer, + the one device where the tree chooses the *driver* and there is
no fallback), I-17 (+timer), I-9 (+irqfwd). Same discipline as the last three
batches, arrived at from the other side.

MEASURED. **Sixteen registered tests** across the area (1 controller smoke, 3
timer, 5 forwarding, 7 clock). The forwarding tests are the substantive ones --
they drive a real interrupt through the real dispatch hook. The controller's is
a smoke test whose comment claims the unused generation's region is "left zero"
while asserting only that the used one is non-zero: true, unchecked, and the
batch-18 shape again in miniature. And **both hardware generations are different
code for the same behaviour, with a run exercising exactly one**, so any claim
about the older path is evidence only if produced on the older path.

SMALLER FINDINGS. Two per-CPU counters in the same dispatch path do the same job
with different tools -- one atomic with an explicit relaxed ordering and a
written rationale, the other a plain `volatile` increment; both single-writer,
both correct, only one current. Two accessors publish the counter frequency, one
truncating to half a machine word, distinguished only by a comment at the single
call site that must have the wide form (the page userspace reads, where
disagreement would make a program's clock differ from the kernel's); the narrow
one survives in a banner, a benchmark and two tests, with nothing at its
definition saying it is diagnostic-only. The forwarding layer cites a line number
in the controller for a clamp that has since moved. The timer's file header
describes its reload arithmetic in terms of the physical timer's registers, a
leftover from before the switch to the virtual one.

THREE ABSORBED DOCUMENTS, THREE DISTINCT FAILURE MODES -- and the first batch
where the absorbed files were worth reading as a set. The interrupt controller's
was updated for the older generation's **presence but not its substance**: the
headline, the identifiers and the shape are all correct and eleven mentions
deep, while every mechanism that makes that generation hard to drive is absent
(the completion echo of the sending CPU's identity, the active-state clear, the
byte-wise priority writes, the runtime line-count bound, the edge barrier, the
per-CPU dispatch counter -- zero mentions each). The timer's **knows it is wrong
and delegates the correction to the reader**: a note records the switch from the
physical timer to the virtual one and then says the interior pseudocode still
names the old registers and should be read as its counterpart -- twenty-four
lines of it, in the section that exists to be copied from. That is worse than
staleness in a specific way: staleness is discovered, and this is *agreed to*.
The forwarding layer's was **the best-preserved and still contradicts itself
twenty lines apart** -- one section correctly says the handler slot is retained
because a null attach is rejected, the next says destroy "clears the handler
slot", and its failure prediction is then wrong twice over (attaching over a
live slot does not fail, it silently overwrites; the next create would fail at
the *claim*, before reaching attach). It also **defers a guarantee the code
now has**, recording that a synchronous drain is "held until concurrent
destroy-vs-IRQ becomes a real driver pattern" when the in-flight-marker spin
exists and the identifier appears zero times.

AND THE INDEX IS STALER THAN THE DOCUMENTS IT INDEXES. `REFERENCE.md`'s row for
the controller reads "GIC v3 driver ... v2 path + ITS + SMP redist walk
**deferred**" while the document it points at is titled "GIC v2 + v3 driver" and
describes the v2 path as landed. The timer's row still describes the physical
timer, its old interrupt number and its old registers, and lists "oneshot +
per-CPU SMP" as deferred -- both landed. Nothing reads an index row against its
target, so a summary of a summary drifts twice as fast.

SCOPE. Secondary-CPU bring-up (the power-controller trampoline, the online
handshake) stays owned by [[sub-kernel-sched-smp]] and was cross-linked, not
restated -- this area supplies only the per-CPU interrupt and timer arming that
bring-up calls. The reservation table that keeps a driver process from claiming
the controller's own registers is named here and described when the
hardware-capability objects are swept.
