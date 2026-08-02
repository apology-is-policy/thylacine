---
id: moc-kernel-devices
type: moc
title: "Devices — the hardware the kernel keeps"
parent: moc-kernel
created: 2026-08-02
updated: 2026-08-02
---
The hardware the kernel drives itself, rather than handing to a userspace
driver: the interrupt controller ([[sub-kernel-gic]]), the timebase and the wall
clock ([[sub-kernel-timer]]), and the one surface that lends an interrupt out to
a driver process ([[sub-kernel-irqfwd]]) — plus the objects that lend the rest of
the hardware out ([[sub-kernel-hwcap]]).

> **Partial area.** Two of three sweeps done. Present: the interrupt and time
> path, and the hardware-capability objects. Not yet swept: the virtio and PCI
> transports (`virtio`, `virtio_pci`, `devpci`, `devhw`) and the synthetic device
> filesystems (`devramfs`, `devenv`, `random`). Statements here are scoped to
> what has been read.

## The organizing fact

**These are the two devices whose failure has no observer.**

Everywhere else, a broken device eventually produces a wrong value that
something reads and someone notices. Not here. If the timer stops firing,
nothing reports it — the system simply stops preempting, and looks like a slow
machine. If an inter-processor interrupt is dropped, nothing logs it — a CPU
idles a fraction of a second longer than it should. If an interrupt is delivered
twice, the driver sees one wake. None of these has a natural output.

So the area's design substitutes **redundancy for detection**. It is built to
survive the loss it cannot see, rather than to notice it:

| The loss | Why nothing would notice | What is done instead |
|---|---|---|
| A reschedule IPI is dropped | the target just idles longer | the message is a *flag* the sender already published; the IPI is only promptness. And an idle CPU arms a backstop so it re-checks anyway |
| Two IPIs collapse into one | the receiver has no count to compare | the receiver's action is idempotent by construction — "look at your flags" |
| A forwarded interrupt fires twice before the driver waits | the driver was not running | the count is deliberately *collapsed*: the contract is "at least one arrived", never "exactly n" |
| A CPU's timer is never armed | it preempts nothing, silently | every CPU arms its own at bring-up and re-arms in its own handler; there is no central arming to fail |

The visible consequence is **five counters in about seventeen hundred lines**
— per-CPU interrupt totals, per-CPU IPI receipts, forwarded-interrupt fires,
live forwarded objects, and ticks. None of them is load-bearing. Each exists
because the event it counts has no other way to be seen: by a test, by
`/proc/stat`, or by a person reading a diagnostic. In an area where correctness
cannot be observed directly, counting is how the tests get a foothold.

**The habit reaches its limit one sweep further in.** The hardware-capability
objects carry six more counters, and *every* consumer of all six is a test —
across the whole kernel and architecture trees there is not one production
reader, and three of the six have no caller at all. Where the first sweep's
counters at least fed a diagnostic someone might read, these exist purely so
that an assertion has something to assert against. The pattern is the same one
pushed to its end: when failure has no observer, you manufacture one, and
sometimes the only observer you manufacture is the test suite.

## What this area owns, and what it deliberately does not

The interrupt controller and the timebase are the two pieces of hardware the
kernel **cannot** delegate, because the scheduler and the death path are built
on them: preemption is the timer's interrupt, and trapping a peer CPU out of
userspace so it can notice it has been killed is the IPI. Everything else in
`devices/` is either lent to a driver process under a capability, or is a pure
kernel fiction with no hardware behind it at all.

The line is drawn in the reservation table rather than here: the controller's
registers, the console's, the clock's and the PCI config space are marked
kernel-owned, so a process holding the hardware-creation capability cannot claim
them. That gate lives in [[sub-kernel-hwcap]] — it is the enforcement site for
[[inv-i5]], the invariant that a driver's authority never reaches the machinery
its own interrupts run on.

Secondary-CPU bring-up (the power-controller trampoline, the online handshake)
belongs to [[sub-kernel-sched-smp]]; this area only supplies the per-CPU
interrupt and timer arming that bring-up calls.

## The version split

The interrupt controller exists in two hardware generations, and the kernel
drives both from one file with a runtime-detected branch. This is not
portability for its own sake: the older generation reaches its CPU interface
through memory-mapped registers rather than system registers, and that is
precisely what makes the hypervisor-accelerated development loop work on the
Apple hardware this is built on. The older form is also what the intended
bare-metal target has. The newer form is the emulator default and the
continuous-integration baseline.

So both paths are live, and the consequence for testing runs through the whole
area: **the two builds exercise different code for the same behaviour**, and a
test that only ever runs on one of them is only ever evidence about one of them.

## Notes

- [[sub-kernel-gic]] — the interrupt controller: discovery, per-CPU bring-up,
  routing, the acknowledge/end-of-interrupt pair, and inter-processor interrupts
- [[sub-kernel-timer]] — the counter, the periodic tick, the one-shot used by
  idle, the wall-clock offset, and the boot-time clock read
- [[sub-kernel-irqfwd]] — lending an interrupt to a driver process: exclusive
  claim, the wait/wake path, and the teardown that races a live interrupt
- [[sub-kernel-hwcap]] — lending the rest: a register range, a DMA buffer and a
  bus function, and the three different ways their exclusivity is enforced
