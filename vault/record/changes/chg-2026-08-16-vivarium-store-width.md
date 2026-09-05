---
id: chg-2026-08-16-vivarium-store-width
type: chg
title: "The flags that make it a kernel are the flags that make the idiom unsafe"
date: 2026-08-16
arc: arc-vault
commits: ["5dfa43bb"]
touched: [sub-kernel-vivarium]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The dossier already carried this defect's substance from the previous sweep —
the use-after-free, the four lock-free readers enumerated by enclosing function,
the per-field store width. Two things it did not carry, and both are about how
the work was done rather than what it produced.

## A severity correction that ran downward

The defect arrived from the other track as a **use-after-free read**, with the
reporter saying plainly that they had not audited this tree for further sites.

Verifying it locally turned up seven sites, including two writers — and on their
strength the severity was raised to "use-after-free *writes*, heap corruption".

**That escalation was wrong and was withdrawn.** Both writers run on a thread of
the target process, exactly like the two same-process readers, so the exec-alone
gate already excluded them. The reporter's narrower original read was correct;
the ceiling is a wrong disposition, not corruption.

Two things generalize.

**Finding more call sites is not finding more exposure.** The count went up and
the reachable set did not, because the new sites sat inside a gate that was
already holding. The instinct to treat "more sites than reported" as "worse than
reported" is exactly backwards when the extra sites are on the covered side.

**The same diligence produced the real finding and the false one.** Checking
rather than trusting the reported scope is what enumerated the four genuine
readers *and* what manufactured the withdrawn escalation. That is not an argument
against checking — it is an argument for finishing the check on each new site
before revising the grade, since a half-checked site reads as an uncovered one.

And it is worth writing down at all because **downward corrections almost never
get recorded.** An over-call that gets quietly right-sized leaves no trace, so
the record accumulates a bias toward severity being confirmed.

## The idiom that is safe everywhere else

The first fix replaced a free with a byte-wise zero. The follow-up round found
that this introduced a different defect, in a place the first round had no reason
to look.

The table is read **lock-free by other processes**, so the *width* of the reset's
stores is an ABI with those readers. And the byte loop was measured — not assumed
— to compile to an **unroll-by-two emitting two-byte stores**, precisely because
the kernel's freestanding, no-builtin build is what stops the compiler
recognising the loop as a block fill.

So each eight-byte handler was written as **four independent halfword stores**,
and a concurrent reader could observe a handler value **no code ever wrote** —
half an old address, half zero — and pass the validity gate on it.

**The flags that make this a kernel are the flags that make the idiom unsafe.** A
byte loop is a fine block fill in a hosted build, where the compiler recycles it
into one. Here it is *guaranteed* not to be, which inverts the usual intuition
about which spelling is the conservative one — and the inversion is invisible
unless you look at what was emitted.

A field-wise struct assignment gives paired register stores instead: atomic at
the eight-byte granule, which is the granule every accessor actually reads.

## Naming what the fix does not promise

The replacement carries an explicit non-guarantee, and this is the part I would
most want a future reader to copy.

**A reader still sees an arbitrary mix of pre- and post-reset entries.** That is
the standard exec-versus-signal race and is fine, because every entry it sees is
one that was genuinely installed or the default. The guarantee is **per-field
integrity, not a snapshot.**

Without that sentence the fix reads as having made the reset atomic, which it did
not and could not. **A bounded claim stated as bounded is what stops the next
reader building on a stronger property than exists** — and the stronger reading
is the natural one, since "we fixed the torn writes" sounds like "the reset is
now indivisible".

This is the same discipline as the geometry change elsewhere in today's sweep
refusing to claim a speedup it had not measured: scope of claim equal to scope of
evidence, written down at the point where over-reading is easiest.
