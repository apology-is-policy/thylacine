---
id: sub-kernel-gic
type: sub
parent: moc-kernel-devices
title: "The interrupt controller — two hardware generations, one driver"
code:
  - arch/arm64/gic.c
  - arch/arm64/gic.h
audit: hard
guarded-by: [inv-i15, inv-i18]
validated-by: [prose, spec-scheduler, gate-smp]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 12.3"
  - "docs/PORTABILITY.md section 5"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Own the machine's interrupt controller: find it, bring it up on every CPU, route
each interrupt source to a CPU, hand an arriving interrupt to the right handler,
and send interrupts from one CPU to another.

Two hardware generations are supported from one file. The kernel picks between
them at boot from the device tree, and the choice is not cosmetic — the older
generation reaches its per-CPU interface through memory-mapped registers, the
newer through system registers, so almost every operation has two spellings.

## Contract

Brought up once on the boot CPU, then once per secondary as it comes online.
After that the surface is: attach a handler to an interrupt number, enable or
disable that number, acknowledge an arriving interrupt, signal its completion,
and send an inter-processor interrupt to a specific CPU.

Failures during bring-up are fatal — a machine whose interrupt controller cannot
be found or mapped cannot run a scheduler, so the driver ends the world rather
than continuing without preemption. After bring-up, every entry point is
total: an out-of-range interrupt number returns false rather than writing to a
register that does not exist.

Two operations are **promises about the calling CPU**, not global requests.
Enabling a per-CPU interrupt and bringing up a secondary's interface both touch
state that hardware banks per-CPU: the same register address means a different
register depending on who reads it. Calling either on behalf of another CPU
silently configures the caller instead. The contract is stated at both sites,
and it is why secondary bring-up is threaded through the secondary itself
rather than done centrally by the boot CPU.

## Mechanism

**Discovery.** The tree is searched for the newer generation's identifier first,
then two identifiers for the older one — the emulator's and the one real
reference board uses. The first match sets the version and names the node whose
register ranges are then read out of that same node, so version and addresses
cannot come from different devices. No match ends the world.

**Bring-up, newer generation.** Three stages, in an order the architecture
forces. The global part disables the controller, reads how many interrupt lines
this implementation actually has, then — before programming any routing —
enables affinity routing, because the routing registers read as zero and ignore
writes while it is off. Only after every shared interrupt has been disabled,
cleared, assigned a priority and given a destination does the final write turn
the interrupt groups on. Nothing can be delivered while the configuration is
half-applied.

The per-CPU part wakes this CPU's interface out of its reset sleep state and
waits for hardware to acknowledge, bounded by a deadline computed from the
counter frequency: if the acknowledgement never comes, it ends the world loudly
rather than spinning forever. It then clears the per-CPU interrupt bank's
enable, pending **and active** state — because firmware, or a previous kernel in
a kexec-style handover, may have left an interrupt half-delivered, and the first
thing that happens after interrupts are unmasked would otherwise be an interrupt
for which no handler exists.

The per-CPU system-register stage admits all priorities, disables sub-priority
grouping, selects the one-step completion mode, and enables the group last.

**Bring-up, older generation.** No per-CPU redistributor exists; the per-CPU
interrupt bank lives in the *shared* controller's low register region, banked so
each CPU sees its own. So the same split becomes: a global stage on the boot
CPU, and a two-part per-CPU stage that must run on the CPU it configures. The
priority mask is programmed before the interface is enabled, so a CPU begins
admitting interrupts only once it knows which to admit. Interrupt grouping is
skipped entirely: the reference platform has no security extensions, so the
group registers read as zero and ignore writes, and the driver follows the same
choice the mainstream Linux driver makes.

**Priorities are written a byte at a time.** The priority registers pack four
interrupts per word, and the obvious read-modify-write of the word would lose a
concurrent update to a neighbouring interrupt. The hardware permits byte stores;
using them makes each interrupt's priority independently writable.

**Routing.** Shared interrupts all go to CPU 0. The newer generation writes a
per-interrupt destination register; the older writes a per-interrupt bitmask
byte. Per-CPU interrupts are not routed at all — they are delivered to whoever
owns the bank.

**Dispatch.** An arriving interrupt is acknowledged, which yields its number;
that number indexes a flat table of handler-and-argument pairs; the handler is
called; completion is signalled. An out-of-range number or a missing handler
ends the world — there is no quiet drop, because an interrupt nobody handles
will re-assert immediately and the machine would livelock instead of stopping.

**The older generation's completion echo.** Acknowledging returns a word whose
low bits are the interrupt number and whose next three bits, for an
inter-processor interrupt, identify the *sending* CPU. Completion must write
that same field back. So the raw acknowledgement word is saved in a per-CPU slot
between the two calls and echoed at completion when its number still matches
what is being completed. The slot needs no lock: handlers run with interrupts
masked and do not nest, so there is exactly one acknowledgement in flight per
CPU, and the writer is always the CPU that reads it.

**Inter-processor interrupts.** The target index is checked against the number
of CPUs that actually came online, not against the width of the encoding field —
an index inside the encoding but outside the machine would be silently dropped
by hardware while the caller believed it had sent something. The newer
generation writes a system register whose low bits are a bitmap of target CPUs
within a cluster; the older writes a shared register with a target bitmask that
bounds it to eight CPUs, checked separately.

**Edge configuration for lent interrupts.** Shared interrupts are configured
level-sensitive at bring-up, the safer default for an unknown signal. A device
driver that knows its interrupt is edge-triggered flips it, and that write is
followed by a full memory barrier so a strict implementation cannot process the
subsequent enable before the configuration has latched. Getting that order wrong
would deliver the first interrupt as level-sensitive, which — for a source
nothing ever deasserts — is an unrecoverable storm rather than a wrong value.

## Data structures

A handful of file-scope values, all written once during bring-up: the detected
version; the mapped kernel addresses of the shared region, the per-CPU regions
and the older generation's CPU interface; the physical addresses each was found
at, kept for diagnostics; and the highest interrupt number this implementation
reports.

A flat array of handler-and-argument pairs, one per architectural interrupt
number.

Two per-CPU arrays: the saved acknowledgement word for the older generation's
completion echo, and a count of interrupts dispatched.

The interrupt count is a per-CPU slot written only by its own CPU — handlers run
masked, a per-CPU interrupt is banked, and a shared interrupt is routed to a
single CPU — so no read-modify-write race exists, and a cross-CPU reader loads
it relaxed as the monotonic counter it is. It is counted at dispatch, deliberately
at the point *every* interrupt passes, rather than reusing the narrower count of
interrupts forwarded to driver processes: publishing the narrow number under the
wide name would have been a plausible-looking fabrication.

**The pending state is a bitmap, not a queue.** This is the fact the rest of the
system's interrupt reasoning rests on. Set-pending and clear-pending are
registers of one bit per interrupt number. There is nowhere for a second
occurrence of the same number to be recorded, and nowhere for the arrival order
of two different numbers to be recorded either. Repeated sends of one
inter-processor interrupt collapse into one delivery, which the design relies
on; ordering between two *different* interrupt numbers is not represented at
all. See [[inv-i18]].

## Concurrency

No locks. The driver's mutable state is either written once before secondaries
exist, or is per-CPU and single-writer.

The handler table is the one shared mutable structure, and it is written without
synchronization. That is safe because of a call-order discipline every caller
follows: attach, *then* enable. An interrupt cannot arrive between the two field
writes of an attach, because the source is still masked. There are five attach
sites and each is immediately followed by its enable.

Bring-up ordering does the rest: the global stage runs on the boot CPU before
any secondary exists, and each per-CPU stage runs on the CPU it configures.

## Invariants enforced

**[[inv-i15]]** — the version, the register addresses and the interrupt numbers
all come from the tree, with no compiled-in address. This is the one device
where the tree also selects *which driver runs*.

**[[inv-i18]]** — inter-processor interrupt ordering. Enforced here by the
hardware's coalescing plus a deliberately trivial receiver. Read the invariant
for what that does and does not guarantee: the model behind it assumes a queue
this hardware does not have.

The kernel-ownership of the controller's own registers — so that a process
holding the hardware-creation capability cannot claim them and interfere with
live acknowledgement state — is enforced in the reservation table, not here.
Both generations' regions are covered, including the older generation's CPU
interface. That site is swept with the hardware-capability objects.

## Error paths

Bring-up failures all end the world, each with a distinct message naming what
was missing: no controller in the tree, a missing register range for either
region, a failed mapping, or a per-CPU interface that never acknowledged its
wake.

After bring-up, nothing is fatal except a dispatched interrupt that is
out-of-range or unhandled. Attaching rejects an out-of-range number and rejects
a null handler; enabling, disabling and setting pending all reject out-of-range
numbers by returning false. Completion silently ignores the architecturally
reserved numbers at the top of the range, defending an entry point that the
exception path already filters.

## Performance

Every operation is a small number of memory-mapped or system-register accesses.
Dispatch is one array index and an indirect call, plus one counter store.

The older generation's accessors each carry a memory barrier, an
empirically-validated mitigation for the hypervisor path rather than an
architectural requirement — the cost is paid only on that generation.

## Prosecution

- The two generations are **different code for the same behaviour**, and a run
  exercises exactly one. Any claim about the older path is evidence only if it
  was produced on the older path.
- Attach-then-enable is a discipline, not a mechanism. A future caller that
  enables before attaching, or re-attaches a live interrupt, reintroduces the
  race the ordering currently avoids.
- The per-CPU contract on enabling and on secondary bring-up: called for another
  CPU, they configure the caller silently.
- Completion's echo for the older generation depends on the acknowledge and
  complete pair happening on one CPU with no nesting.
- Bring-up clears pending *and* active state; dropping the active clear would
  leave a stale half-delivered interrupt from firmware.
- Affinity routing must be enabled before the routing registers are written, and
  the groups enabled only after.
- The edge-configuration barrier must precede the enable.

## Seams

[[seam-gic-handler-slot-never-cleared]] — the handler table has no unregister
path, so a slot keeps pointing at a freed object after its owner is torn down.
No task: the interrupt is disabled first, and every enable is preceded by a
fresh attach, so nothing can route there.

## Caveats

- **The interrupt-count array and the inter-processor-interrupt count array do
  the same job in the same code path with different tools** — one uses atomic
  accessors with an explicit relaxed ordering and a documented rationale, the
  other is a plain `volatile` increment. Both are single-writer per slot and
  both are correct; only the second is the older style. Worth converging if the
  file is touched.
- The claim-tracking layer bounds interrupt numbers against the value read from
  hardware at bring-up rather than the architectural maximum, because writes
  beyond an implementation's actual line count are undefined. The architectural
  bound is kept underneath as a second check.
- A comment in the forwarding layer cites a line number in this file for the
  clamp that establishes that bound; the clamp has since moved.
- The bring-up smoke test's comment says the *unused* generation's region is
  "left zero", but the test only asserts the used one is non-zero. The claim is
  true and unchecked.

## Provenance

Read from `arch/arm64/gic.c` (968 lines) and `arch/arm64/gic.h` (237), 2026-08-02,
at `f109477e`. Cross-checked: the five attach sites and their enables, the
enable and disable call sites, the inter-processor interrupt definitions, the
reservation entries for both generations, and the registered tests.

Absorbed `docs/reference/10-gic.md`.
