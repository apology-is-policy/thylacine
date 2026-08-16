---
id: chg-2026-08-16-boot-cwg-parenthetical
type: chg
title: "Two comments in one commit disagree about a measured result"
date: 2026-08-16
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-boot-sequence]
established: []
closed: []
opened: [seam-cwg-parenthetical-refuted]
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The feature-detection layer learned to decode a second cache size. Recording it
turned up the cleanest instance of a pattern this whole sweep has been
accumulating.

## Two cache numbers that answer different questions

The kernel had only ever decoded the **minimum data line** — the smallest span
a level will allocate, which is the maintenance number every cache-clean loop
strides by.

The **writeback granule** is the largest span one eviction may write back, and
it is the number that governs **false sharing**. The architecture permits them
to differ, so a per-CPU field padded by the first is not necessarily separated
at all.

Which is why the geometry question next door had no answer available to ask:
the kernel was not reading the field that answers it. **A missing measurement
does not present as a wrong number; it presents as a question nobody asks.**

Two decoding choices worth keeping:

- **A granule field of zero means the part declines to report**, and it is
  recorded as zero, verbatim — not decoded into four bytes, not promoted to the
  architectural maximum. *No information* and *small* are different facts, and
  a consumer that receives them collapsed has been handed a reading nobody took.
- **The padding constant cannot be the measured value**, because struct layout
  is compile-time. So the constant is a margin and the recorded value is what
  keeps it honest: a registered test fails loudly if any target ever reports a
  granule the constant does not cover. **Measure to check a constant you cannot
  derive** is a better pattern than either measuring or asserting alone.

## The falsification and the falsified claim shipped together

The header records, honestly and in detail, that an earlier draft asserted the
development host's silicon reports a 128-byte granule; that **one boot falsified
it**; and that the constant now rests on an asymmetry argument instead.

The decode site in the sibling file still says the opposite, as a parenthetical
supporting the true general claim that the two sizes may differ: *a part may
allocate 64-byte lines while its coherency protocol moves 128 — this silicon
does exactly that.*

**Two comments, one commit, disagreeing in writing about a measured result.**
Not staleness — the correct text explicitly records the measurement that
refutes the other.

## Why it survived, which is the reusable part

Three reasons compound, and each is individually reasonable.

**The header scopes its own correction.** It says "an earlier draft of *this
comment*", which is accurate. The surviving copy is a **different comment
making the same claim**, so anyone verifying the correction landed finds that it
landed, precisely where the sentence says.

**The sentence around the error is true and load-bearing.** The general point —
these two sizes may differ, so padding by the wrong one separates nothing — is
exactly right and is the reason the field is being decoded. **An example inside
a correct claim inherits the claim's credibility**, and reading for the claim is
how you read a comment like that.

**Nothing depends on it.** The constant is a margin, the recorded value is raw,
and a test compares them — so no behaviour rests on the example. Which is the
same immunity a fictional safety argument enjoys: *the comments most likely to
be wrong are the ones nothing checks*, and they are exactly the ones a reader
consults to learn what is going on.

## What the fix is, which is not a delete

The measurement was taken **through a hypervisor**, and whether that path
presents the silicon's own cache identification register or a synthesized one is
itself unverified.

So the parenthetical is not backwards — it is **unverified in both directions**,
and replacing it with the opposite assertion would repeat the original error
with a different sign. The honest replacement keeps the architectural claim and
says the project has no reading of the bare silicon, because every reading it
has taken is through an emulator or a hypervisor.

Filed as [[seam-cwg-parenthetical-refuted]] rather than guessed at here.

## The fourth instance in one sweep

Same shape, four times, in four unrelated places: the invariant mirror whose
prose was right and whose auto-loaded summary was wrong; the gate note whose
table I corrected while leaving its rendered summary stating the pre-correction
version; the console dossier that inherited a claim its own source later
refuted; and this.

**A correction updates the place where you noticed.** You notice while editing
one thing, and the other copy is in another file, off screen, and not what
prompted the edit. Every one of these was made by someone specifically checking
the fact in question.

What distinguishes this one is that the correct text **records the
falsification**, which means the pair is not two versions of a belief — it is a
result and its refutation, coexisting, three lines and one file apart.
