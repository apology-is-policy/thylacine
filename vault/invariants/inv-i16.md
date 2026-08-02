---
id: inv-i16
type: inv
title: "I-16 — the kernel base is randomized at boot, and never zero"
number: I-16
guards: [sub-kernel-kaslr, sub-kernel-boot-entry]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-02
updated: 2026-08-02
---
## Statement

The kernel's runtime virtual base is a link-time constant plus a **boot-chosen
offset that is never zero and never predictable**. Kernel addresses observed in
one boot say nothing about the next.

The never-zero half is the part worth stating separately, because it is the one
that can be lost by accident: an entropy source that yields zero, or a mask that
clears every preserved bit, produces a kernel that runs exactly where it was
linked. That is not a crash and not a warning — it is a correctly functioning
kernel with the mitigation silently absent.

## Why it is stated this way

The value of address randomization is entirely statistical, so its failure mode
is invisible from inside. Nothing in the system behaves differently when the
slide is zero; only an attacker notices. Stating "never zero" as a contract turns
a probabilistic property into a checkable one, and the implementation honours it
directly — when the masked entropy is zero, the offset is forced to the minimum
alignment rather than left as it fell.

## Enforcement

**A choice, then a commitment.** The offset is chosen once, before the MMU is
programmed, and everything downstream derives from it: the high-half mapping the
tables are built for, the target of the long branch out of the boot stub, and
every later translation between an image address and a runtime address. There is
no second decision point and no re-randomization.

**Entropy in priority order, with the source reported.** A firmware-supplied
dedicated seed, then a general random seed, then the architectural counter. The
last is materially weaker — boot-time variance in the low bits — so which one was
used is printed in the banner. The reporting is part of the enforcement: without
it, a machine whose firmware stopped publishing a seed would degrade to the
counter path indistinguishably.

**Mixing before masking.** The raw seed goes through an avalanche function before
any bits are selected, so a source with structured entropy — a counter, whose
high bits barely move — still fills the preserved range.

**The window is deliberately narrower than the address space allows.** Eleven bits
at eight-megabyte alignment, inside a sixteen-gigabyte range. The alignment has
been widened twice, each time because a sanitizer build's image outgrew the fixed
page-grain mapping, and each widening cost a bit. The current figure is the
accumulated result of image growth rather than a security calculation — which is
worth knowing, because the next image growth spends another bit.

**The cookie shares the entropy.** The stack-canary value is seeded from the same
mixed source at the same moment, which is why the function that chooses the slide
is the one function in the kernel exempted from stack protection.

## Validation

Prose plus one registered test, which covers the avalanche property of the mixing
function — the only pure function in the path. The slide choice itself, the
never-zero clamp, and the relocation walk have no direct test.

The practical evidence is the banner: it prints the chosen base, the offset, and
the seed source on every boot, so a zero offset or a degraded source is visible to
anyone reading a boot log, including every automated gate that captures one.

**blind-to:** nothing tests that the offset differs between boots, and nothing
would notice a mixing function that became deterministic while remaining
non-zero — the clamp would be satisfied and the banner would print a plausible
value every time. The relocation walker, which exists so that randomization stays
correct once absolute references appear, is unexercised: the current build emits
none, so its loop body has never run.
