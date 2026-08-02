---
id: inv-i15
type: inv
title: "I-15 — the hardware view derives entirely from the device tree"
number: I-15
guards: [sub-kernel-dtb, sub-kernel-boot-sequence, sub-kernel-gic, sub-kernel-timer, sub-kernel-discovery]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-02
updated: 2026-08-02
---
## Statement

Every hardware fact the kernel acts on — where RAM is, where a device's registers
are, which interrupt it raises, how many CPUs exist and how to start them, what
each CPU is capable of — is **derived at runtime from the device tree the
bootloader supplied**, not compiled in.

The invariant admits argued exceptions, and requires that they be argued rather
than accumulated. Two exist: a fallback console address used only in the window
before the tree has been parsed, so that a parse failure can be reported at all;
and a fallback for the real-time clock on the reference platform. Both are named
at their sites with the reasoning, and both are strictly fallbacks — the
tree-derived value wins whenever there is one.

## Why it is stated this way

It is a portability invariant that behaves like a soundness one. The property it
actually buys is that **one binary boots on machines it was not built for**, which
is what makes a hardcoded address a defect rather than a shortcut: each one
silently narrows the set of machines the kernel runs on, and the narrowing is
invisible until someone tries the machine that no longer works.

Stating it as "entirely, with argued exceptions" rather than "mostly" is
deliberate. A soft version accumulates: each new hardcoded address is individually
defensible, and no single one is where the property is lost.

## Enforcement

**One file answers.** [[sub-kernel-dtb]] is the only place that reads the blob,
and every consumer goes through its accessors. There is no second parser and no
cached copy of a hardware address anywhere else.

**And userspace gets the tree itself, not a summary.** [[sub-kernel-discovery]]
republishes the parsed tree as a namespace whose files hold the properties' raw
bytes, unedited. That is this invariant's most literal enforcement: a driver's
hardware view derives from the device tree because the device tree is what it is
handed. Nothing in the kernel gets to decide which facts a driver is allowed to
learn — only, via the allowance, which it may act on.

**On one device the tree chooses the driver, not just the address.** The
interrupt controller exists in two hardware generations whose per-CPU interfaces
are reached differently — memory-mapped registers on one, system registers on
the other — so [[sub-kernel-gic]] matches the tree's identifier first and reads
the register ranges out of *that same node*, which is what keeps the version and
the addresses from ever coming from different devices. No match ends the world:
this is the one lookup with no fallback and no degraded mode, because a machine
whose interrupt controller cannot be identified cannot preempt.

**Absence is an ordinary answer.** Every accessor returns a boolean or a sentinel
for "the tree does not say", and callers degrade rather than fail. This is what
makes the invariant compatible with a single binary: a machine that lacks a
device answers no, and the kernel runs without it. The banner reports which
lookups fell back, so a degradation is visible rather than silent.

**The CPU is included.** The same principle applies to the processor itself:
capabilities come from reading identifier registers, not from a build-time target
choice, which is what makes the boot-time instruction patching in
[[sub-kernel-alternatives]] possible at all. And per-CPU identity is read *by
each CPU*, because the registers genuinely differ on a heterogeneous machine.

**The dense-index assumption is checked, loudly.** The kernel assumes a CPU's
tree-declared index and its hardware-derived index name the same slot. On a
machine where they diverge, a CPU would initialize one per-CPU slot and use
another — aliasing a live neighbour's scheduler state, which no bounds check can
detect. An equality assertion at each secondary's entry turns that into an
immediate stop on the first machine where the assumption is false. The check
belongs to [[sub-kernel-sched-smp]]; it is listed here because the assumption is
a tree-shape assumption.

## Validation

Prose and the boot itself. Three registered tests cover specific lookups — an
entropy property, interrupt routing, and a memory window — but the invariant's
real evidence is that the kernel boots and reports the machine it found. There is
no test for "nothing else reads hardware addresses"; that is a review property,
and the census belongs with any change that adds a device.

**blind-to:** the reference platform is the only machine the tree is ever read
from, so every lookup's fallback path and every shape the parser handles but has
not seen — multi-bank memory, a heterogeneous capability declaration, a
non-dense CPU numbering — is exercised by reading and not by execution. The
invariant's whole value is realized on the machines it has not yet run on.
