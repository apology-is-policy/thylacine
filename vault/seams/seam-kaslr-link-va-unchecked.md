---
id: seam-kaslr-link-va-unchecked
type: seam
status: open
title: "Two headers each claim the other enforces a cross-check that neither performs"
surface: [sub-kernel-kaslr, sub-kernel-boot-entry]
opened-by: chg-2026-08-02-boot-sweep
tracker: "task #24"
created: 2026-08-02
updated: 2026-08-02
---
## What

The kernel's link-time virtual base exists as two independent constants: one in
the linker script, one as a C macro. They must be equal. Nothing checks that they
are.

Both sides say something checks it.

The C header says the constant "mirrors" the linker script's and that "the linker
ASSERT enforces the C / linker-script values agree." The linker script says the
constant is duplicated in C and that "we keep the constant on both sides and
assert their equality at runtime by linker ASSERT" — and names the wrong header
for the C copy.

The assertion they both point at compares two *linker-side* values: the address
of the image's first byte, against the linker variable that was assigned to it
eleven lines earlier. It is a tautology, and it cannot see the C macro at all.
There is no compile-time assertion on the C side, no value passed in from the
build, and no generated header.

## Why the duplication exists at all

It is not sloppiness. Under position-independent linking the C side cannot read
the linker's value: PC-relative addressing yields the load address, not the link
address, which is exactly the distinction the constant exists to express. The
header says so. Duplication is the only option available.

## What the tree already does about this, correctly, twice

The same problem appears twice more in the boot path, and both instances are
solved with a named idiom. Assembly cannot include a C header, so two constants —
a per-CPU stack slot size and a CPU-count bound — are hardcoded as literals in
assembly. In both cases the C header carries a compile-time assertion **pinning
its own constant to the literal the assembly hardcodes**, with a message naming
the assembly file.

That does not compare the two languages. What it does is guarantee that changing
the C side fails the build with a pointer at the other copy — which is the whole
of what is needed, since silent divergence is the only failure mode.

The gap is therefore not that the problem is unsolved in general. It is that the
one instance which *claims* to be solved is the one that isn't, while the two
that claim nothing are.

## Consequence if they diverge

The runtime address translation would compute a wrong long-branch target, and the
boot stub would jump into unmapped high memory immediately after enabling the MMU.

That is a definite failure rather than a silent one — but it lands in the least
diagnosable window in the system. At that point the MMU is on, the exception
vector table has not been installed (that happens much later), and the console has
not been remapped. There is no output, no fault handler, and no dump. A developer
who changed one constant would see a kernel that stops producing bytes, with
nothing to indicate why.

## Reachability

Nil today; the two values are equal. Nothing keeps them equal except that nobody
has edited either — and both are plausible edits, since moving the kernel's
virtual base is exactly what a port to a different memory layout would do.

## Trigger

Any change to the kernel's link address. Also any refactor that moves the C macro
to a different header, since the linker script's comment already names the wrong
one and would go on naming a wrong one.

## Fix

Add the compile-time assertion the header claims, in the form the same file
family already uses twice: pin the C constant to its literal value with a message
naming the linker script. One line, an established in-tree idiom, and it converts
a silent divergence into a build failure that points at the other copy.

Correct both comments while doing it — the claim that a check exists is what
would stop someone from adding one.
