---
id: sub-kernel-irqfwd
type: sub
parent: moc-kernel-devices
title: "Lending an interrupt — where a hardware event meets a process lifetime"
code:
  - kernel/irqfwd.c
  - kernel/include/thylacine/irqfwd.h
audit: hard
guarded-by: [inv-i9, inv-i15]
validated-by: [prose, gate-smp]
locks: [lock-rendez]
abis: []
design:
  - "docs/ARCHITECTURE.md section 9.3"
  - "docs/ARCHITECTURE.md section 9.3.1"
created: 2026-08-02
updated: 2026-09-15
---
## Purpose

Let a driver process block on a hardware interrupt. A capability object is
created for an interrupt number; a thread waits on it; each arrival wakes the
waiter and tells it how many interrupts have accumulated since it last looked.

This is the only place in the area where **a hardware event and a process
lifetime meet**, and essentially every mechanism in the file exists because of
that collision: an interrupt can fire on one CPU while the object it points at
is being torn down on another.

## Contract

Create with an interrupt number; the number is claimed exclusively and the
object owns it until the last reference drops. Wait blocks until at least one
interrupt has arrived and returns how many were seen, resetting the count.

The count is **collapsed, not exact**. The contract is "at least one arrived
since your last wait" — the hardware coalesces repeated occurrences of the same
interrupt while it is already pending, so a driver that treats the number as a
precise arrival count is reading something the machine never promised. Real
drivers do not need it: they discover what actually happened from the device's
own queue state.

A second concurrent waiter is refused with a distinct sentinel rather than
being allowed to proceed.

Each interrupt's **trigger mode is derived from the device tree**, not assumed:
a level-triggered line (virtio-PCI legacy INTx) is masked while a driver
services it and re-armed when it waits again; an edge-triggered line (virtio-mmio)
keeps the older no-mask fast path. The wait also takes an **optional timeout**
(zero meaning wait forever, the default): on expiry it returns a count of zero,
the same as a death-interrupt, so a driver treats both as "no interrupt to
service — re-check the device or unwind". The timeout bounds a completion whose
interrupt was lost, so a driver never blocks forever on one that will not arrive.

## Mechanism

**Exclusive claim.** The interrupt controller has one handler slot per number
and attaching silently overwrites it. Without a claim layer, two creators for
one number would both succeed while the second's registration displaced the
first's — leaving the first object alive, never receiving anything, and the
controller's slot pointing at the second. So a claim bitmap, under its own lock,
gates creation: a number already claimed is refused before anything is
allocated.

The bitmap is also **pre-seeded with the numbers the kernel drives itself** —
the reschedule inter-processor interrupt, the timer, and the console's receive
interrupt. Those three attach directly, bypassing the claim path entirely, so
without pre-seeding a process holding the hardware-creation capability could
create an object for one of them and displace the kernel's own handler. The
syscall layer independently refuses everything below the shared-interrupt range,
which covers two of the three; the reservation is the defence that does not
depend on that second gate remaining in place.

Claims are bounded against the count of interrupt lines the *implementation*
reports at bring-up, not the architectural maximum, because register writes past
an implementation's real line count are undefined. The architectural bound
remains underneath.

**Trigger, derived from the device tree.** Creation reads whether the number is
level- or edge-triggered from the PCIe interrupt-map's flags cell — the same map
row that already resolves an INTx pin to its number, whose trigger cell was
previously loaded and discarded. virtio-PCI legacy INTx is level; a number
absent from that map keeps the edge default, the documented argued fallback (the
QEMU-virt device tree declares virtio-mmio edge-rising, and the pre-change
behaviour was universal edge — so this only *adds* correct level configuration
for PCI). The controller register is written **explicitly for both modes**, so a
number reused after an edge object's teardown never inherits a stale level
configuration or the reverse. The resolved mode is recorded on the object,
written before the handler is attached, so the arrival hook reads a published
value. Numbers below the shared-interrupt range skip this entirely — those are
architecturally fixed (inter-processor interrupts are always edge, the timer is
kernel-reserved).

**Arrival.** The controller calls a hook with the object as its argument. The
hook takes the object's wait lock and, for a **level** line, masks the number at
the controller **before** the vector end-of-interrupt — the still-asserted line
would otherwise re-fire in a storm the moment the vector deactivates it; the
wait re-arms it once the driver has acknowledged the device. (An edge line is
never masked — its pending bit is latched and survives, the fast path.) Then it
increments the pending count, drops the lock, and wakes. Dropping before waking
is required, not stylistic — the wake path takes the same lock, and holding it
through would deadlock by recursion. The mask is a single controller register
write, lock-free per bit, so it is safe under the wait lock; a dispatch that
raced teardown returns before masking, but teardown has already masked, so the
line is quiet either way.

The count **saturates just below the maximum**, so that a pathologically
un-drained counter can never coincidentally equal the sentinel that means "a
second waiter was refused". Two different meanings sharing one return type is
the hazard; the saturation keeps them disjoint.

**Waiting.** Claim the single waiter slot under the lock, refusing if taken.
For a **level** line, re-arm it — unmask the number at the controller — now, on
entry: the arrival hook masked it on the last fire, the driver has since
acknowledged the device (deasserting the line) and returned to wait, so a
still-asserted line (a fresh completion) re-triggers at once and an acknowledged
one stays quiet. The unmask is lock-free and sits outside the wait lock; it can
only *cause* an arrival, and an arrival does its counting under the lock that the
sleep's condition also re-checks, so no wake is lost (see [[inv-i9]]). Promote
the thread to the interactive scheduling band — a thread servicing an interrupt
should preempt ordinary work, and without it the wake could sit behind a
compute-bound peer for a full scheduling slice. Then sleep on the condition that
the count is positive, **bounded by the deadline** if a timeout was given. On
waking, re-take the lock to release the waiter slot and read-and-zero the count
in one critical section, so an interrupt arriving between the sleep returning and
the lock being taken is carried into the *next* wait rather than lost. The
condition has precedence over the deadline, so a timeout return always carries a
count of zero.

The requested timeout is **capped to a sane maximum** (an hour) before the
deadline is formed. A caller that fails to set the timeout register — a
hand-wrapped syscall that omits the argument — would otherwise pass an
indeterminate value the kernel reads as a bogus deadline; the cap degrades any
absurd value to a bounded wait rather than a wrapped, already-past deadline that
returns an immediate spurious zero. A genuine timeout is far below the cap; a
wait that means to block indefinitely passes zero.

A sleep interrupted by the process being terminated returns zero: the thread is
unwinding to its death check and will never reach userspace, so the count is
immaterial.

**Teardown is the interesting part**, because disabling an interrupt does not
retract one that has already been acknowledged and is executing on another CPU —
and that execution holds a raw, non-reference-counted pointer to the object
about to be freed. Three defences, in order:

1. Disable the interrupt, so no *new* arrivals occur.
2. Set a dying flag under the lock, then **spin until any in-flight dispatch has
   cleared its in-flight marker**. The dispatch sets that marker under the lock
   before waking and clears it under the lock afterwards; its final unlock is
   its last touch of the object. A dispatch that arrives after the flag is set
   returns immediately without touching anything further. The spin is bounded —
   one handler, running with interrupts masked, finishing in microseconds — and
   cannot self-deadlock, because teardown runs in process context while the
   dispatch runs in interrupt context on a different CPU.
3. Overwrite the object's magic value before freeing, so a stale pointer
   dereference fails its own sanity check rather than reading arbitrary reused
   memory.

Only then is the claim released, and only after that is the object freed.

**The reason there are three defences and not one** is that the second step of
the obvious teardown does not exist: the controller's attach entry point rejects
a null handler by design, to force callers to be explicit rather than quietly
disarming a slot. So the natural unregister — attach nothing — cannot be
expressed. Teardown calls it anyway, and the code says plainly that it returns
false and the slot keeps pointing at the freed object. The disable plus the
dying flag plus the magic clobber are what stand in for it.

## Data structures

One object per lent interrupt, allocated zeroed: a magic value, the interrupt
number, an atomic reference count, a level-trigger boolean, an embedded wait
structure, the pending count, and three booleans — waiting, dying, in-flight.

The level boolean sits with the number, not with the lock-guarded fields,
because it is written once at creation (before the handler is attached, so
before any arrival can read it) and never mutated — the arrival hook and the
wait read it lock-free, the same publication the number and magic already rely
on.

The in-flight marker is a plain boolean because at most one dispatch per
interrupt number is ever in flight: handlers run masked and do not nest, and a
shared interrupt is routed to a single CPU.

A file-scope claim bitmap, one entry per architectural interrupt number, under
its own lock. Two diagnostic counters, atomic and relaxed: total arrivals
forwarded, and live objects.

## Concurrency

Two locks, never nested. The claim lock covers only the bitmap. The per-object
wait lock covers the pending count and all three booleans.

Every field of the object that two CPUs can reach is under the wait lock. The
reference count is the exception, and is atomic — with acquire-release ordering
on the decrement so that everything done to the object happens-before another
CPU observes the count reaching zero, and only the caller that observes the
one-to-zero edge frees.

The single-waiter refusal exists because the handle lives in the per-process
handle table, which peer threads of a multi-threaded process share. Two of them
reaching a wait on the same descriptor is entirely legal, and the underlying
wait primitive ends the world on a second sleeper. Refusing turns a driver bug
into a clean error return instead of a whole-kernel failure that unprivileged
code could trigger.

## Invariants enforced

**[[inv-i9]]** — no wakeup lost. The register-then-observe shape is: the count
is incremented under the lock before the wake, and the waiter's condition is
evaluated under the same lock, so an arrival cannot slip between the waiter
deciding to sleep and sleeping. The read-and-zero after waking closes the
symmetric window on the other side. The level re-arm preserves this: the unmask
sits outside the lock but can only *cause* an arrival, and that arrival counts
under the lock the condition re-checks — so the added mask/unmask cannot lose a
wake. For a level line the read-and-zero window is additionally quiet, because
the line stays masked from the waking arrival until the next wait re-arms it.

**[[inv-i15]]** — the hardware view derives from the device tree. The interrupt
trigger is now part of that view: it is read from the interrupt-map flags cell
rather than assumed, with the edge default for numbers absent from the map as
the argued fallback (the same class as the documented serial-console fallback).

Exclusive ownership of an interrupt number is enforced by the claim bitmap. It
is the hardware analogue of the handle-table rules and is checked by the
scheduling model's resource-exclusivity property rather than being a numbered
invariant of its own.

## Error paths

Creation returns null: number out of range, above the implementation's reported
line count, already claimed, allocation failed, attach failed, enable failed.
The last two release the claim and clobber the magic before freeing, so a
partially-registered object cannot be dispatched into.

Waiting returns zero on a null object or a death-interrupted sleep; the busy
sentinel on a second concurrent waiter.

Reference counting ends the world on a null pointer, a corrupted magic value, a
reference taken on a zero-count object, or a release below zero — all
double-free or use-after-free signatures, treated as structural corruption
rather than recoverable errors.

## Performance

Arrival costs two short critical sections and a wake. The interactive promotion
is what actually determines latency to the driver, and it is sticky.

Teardown's spin is the only unbounded-looking construct, and it is bounded by a
masked handler's completion.

## Prosecution

- The pre-seeded reservations must cover every number the kernel attaches
  directly. A new kernel-internal interrupt that forgets to reserve is
  displaceable by a capability holder.
- The dying flag must be set under the lock, and the in-flight marker cleared
  under the lock as the dispatch's last act. Either moving outside reopens the
  use-after-free.
- The claim is released *before* the object is freed but *after* the
  handler slot has been left alone; a re-creation for the same number
  overwrites the slot before enabling, so nothing can route to the dead object
  in between. Reordering the release later or the attach earlier breaks that.
- The lock must be dropped before waking — holding it recurses.
- The count must keep saturating below the busy sentinel.
- The read-and-zero after waking must stay in one critical section with the
  waiter-slot release.
- The single-waiter refusal is what keeps a shared descriptor from ending the
  world; it is reachable from ordinary multi-threaded code.

## Seams

[[seam-gic-handler-slot-never-cleared]] — the controller's slot cannot be
unregistered, so it permanently references a freed object after teardown. No
task; three defences stand in for it and no path re-enables an interrupt without
first attaching afresh.

## Caveats

- The claim bitmap is one byte per interrupt number rather than one bit — about
  a kilobyte, chosen for legibility over density and documented as such.
- The tests claim the second inter-processor interrupt number for their own
  use, while the kernel's own header reserves that same number in a
  commented-out line for a future cross-CPU cache-invalidation interrupt. The
  absorbed reference noted the tests' reuse, but only as a sequencing concern
  between tests — the collision with the *reserved* future use is connected
  nowhere. Uncommenting it and adding it to the pre-seeded reservations would
  make those tests fail loudly at creation, which is the right failure.
- What actually permits a re-create after teardown is the **claim bitmap being
  released**, not the handler slot being cleared — the slot is never cleared,
  and attaching over a live slot succeeds silently rather than failing. The
  absorbed reference said the opposite in one section while saying the correct
  thing in another; see the stub.
- The comment describing the reschedule interrupt's handler states that no
  cross-CPU placer exists yet and that the interrupt is "purely a wake-from-idle
  signal proving the delivery path works". Two cross-CPU placers now exist and
  the interrupt is load-bearing for both idle wake-up and the death cascade.
  The framing is the kind that invites deletion. See [[inv-i18]], which lists
  the three real senders.

## Provenance

Read from `kernel/irqfwd.c` (393 lines) and `kernel/include/thylacine/irqfwd.h`
(124), 2026-08-02, at `f109477e`. Cross-checked: the attach and enable call
sites across the tree, the scheduler's cross-CPU notification paths, the
interrupt-number definitions, and the five registered tests.

Absorbed `docs/reference/36-irqfwd.md`.

Revised 2026-09-15 at `12d154eb` (irqfwd.c 463 lines, irqfwd.h 137) for the F-A1
cure (docs/ARCHITECTURE.md 9.3.1): the device-tree-derived trigger, the level
mask+ack (arrival masks before end-of-interrupt, wait re-arms), and the bounded
timeout (`kobj_irq_wait_timed` over `tsleep`, capped). The device-tree trigger
read is `lib/dtb.c dtb_pci_intid_is_level`; the controller level configuration is
`gic_set_spi_level_triggered` and its enable-state query `gic_intid_enabled`
(see [[sub-kernel-gic]]); the syscall gained the timeout argument (see
[[sub-kernel-syscall-dispatch]]). Two new discriminating tests
(`irqfwd.level_mask_ack`, `irqfwd.wait_timeout`) plus `dtb.pci_intid_is_level`.
The Opus-fallback audit closed 0 P0 / 1 P1 / 0 P2 / 2 P3; the P1 was a cross-tree
caller (Stratum's virtio-blk `t_irq_wait` omitted the new timeout argument);
the SMP soundness gate passed 0-corruption across default/ubsan x smp4/smp8.
