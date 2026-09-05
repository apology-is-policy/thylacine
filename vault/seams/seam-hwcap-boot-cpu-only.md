---
id: seam-hwcap-boot-cpu-only
type: seam
status: open
title: "CPU features are read from the boot CPU and used system-wide"
surface: [sub-kernel-boot-sequence, sub-kernel-alternatives]
opened-by: chg-2026-08-02-boot-sweep
tracker: ""
created: 2026-08-02
updated: 2026-08-02
---
## What

Feature detection has exactly one caller, on the boot CPU, before any secondary
exists. Verified by census across the tree.

The resulting word has two system-wide consumers:

- **The instruction patcher** decides from it whether to rewrite the kernel's
  atomic primitives — a change to shared code that every CPU then executes.
- **The word published to userspace** at program start is this same detection,
  handed to every process regardless of which CPU it runs on.

On a machine whose cores differ in capability, both are wrong in the same
direction: a feature present on the boot CPU is assumed present everywhere. The
patched instruction would fault on a core that lacks it, and a program told the
feature exists would take a code path its core cannot run.

## Why the tree is nonetheless consistent

The correct answer is documented at both consumers. The published word's audit
recorded that a heterogeneous target must combine the secondaries' registers
before the first program starts, and the patcher's design notes carry the same
assumption explicitly. Every current target is homogeneous, so both are dormant,
and the reasoning is written down rather than assumed.

## What changed, and went unnoticed

**The infrastructure the fix needs now exists, and is already wired.**

Per-CPU *identity* — the processor identifier and cache line size — is recorded by
each CPU into its own slot at bring-up, published with a release store, and read
back through an accessor that returns nothing for a CPU that never came up. Its
header states the reasoning directly: both registers genuinely differ on a
heterogeneous board, so a boot-CPU-only read would be wrong precisely where the
values earn their keep.

That is the same argument, and it is made about neighbouring registers, read at
the same point in the same function, on the same CPU. What it does not do is
extend to the *capability* word, which is still read once on the boot CPU — even
though the per-CPU call site it would need is now three lines away and already
runs before that CPU can execute any user code.

So the seam's cost has quietly dropped from "add a per-CPU mechanism" to "call
the existing one and combine the result", and nothing recorded that.

## Consequence

None today. Every supported machine is homogeneous, and the failure mode on a
machine that is not would be an immediate fault rather than a silent wrong answer
— an unimplemented instruction traps.

The order matters, though: the patcher runs before secondaries exist, so a
combining pass cannot simply be inserted at the same point. The word would have to
be narrowed as each secondary arrives, and the patcher would need to run after
them — which reverses one of the boot sequence's deliberate orderings, since the
patcher is placed before secondaries precisely so that nothing executes a site
mid-rewrite. Making the feature word heterogeneity-correct therefore has a
structural cost in [[sub-kernel-boot-sequence]] that the published-word half does
not: the published word only has to be right before the first program starts,
which is much later.

## Trigger

The first machine with cores of differing capability. The project's portability
work names such a target as a goal, and the scheduler already parses per-core
capacity hints from the device tree — so the tree is being told about
heterogeneity in one dimension while assuming homogeneity in another.

## No task

Not reachable on any current target, correct on every machine the kernel runs on,
and documented at both consumers. Recorded because the fix's shape changed without
anyone revisiting it, and because the two halves have different costs — the
program-visible word is cheap and the patcher is not.
