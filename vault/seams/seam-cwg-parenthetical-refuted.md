---
id: seam-cwg-parenthetical-refuted
type: seam
title: "The commit that recorded the falsification left the falsified claim in the sibling file"
status: open
surface: sub-kernel-boot-sequence
opened-by: chg-2026-08-16-boot-cwg-parenthetical
tracker: "unfiled -- yip to main 2026-08-16"
created: 2026-08-16
updated: 2026-08-16
---
## Owed

A correction to one parenthetical, and a decision about what replaces it —
because deleting it asserts something else that is also unverified.

The header carrying the padding constant records, in detail and to its credit,
that an earlier draft asserted the development host's silicon reports a
128-byte writeback granule; that **one boot falsified it** (the granule equals
the minimum line, at 64, under both hardware virtualization on that host and
full emulation); and that the constant now rests on a margin argument instead.

The decode site in the sibling source file still asserts the refuted claim, as
a parenthetical supporting the general point that the two cache sizes may
differ: *a part may allocate 64-byte lines while its coherency protocol moves
128 — this silicon does exactly that.*

**The general point is true. The instance cited is the one that was measured and
refuted**, in the same commit, one file away.

## What closes it

Not a delete, at least not obviously. The measurement was taken **through a
hypervisor**, and whether that path presents the silicon's own cache
identification register or a synthesized one is itself unverified. So the claim
is not simply backwards — it is **unverified in both directions**, and replacing
it with the opposite assertion repeats the original error with a different sign.

The honest replacement states that the two sizes are architecturally permitted
to differ (which is the load-bearing half), and that this project has no
measurement of the bare silicon because every reading it has taken is through
an emulator or a hypervisor.

**Not a vault edit.** The file is on the implementation branch.

## Risk while open

Nothing depends on it. The constant is a margin rather than a measurement, the
recorded per-CPU value is raw, and a registered test compares the two — so no
behaviour rests on the parenthetical being right.

**That is exactly why it survived.** The sentence it lives in is true and
load-bearing; only its example is wrong, and an example inside a correct claim
inherits the claim's credibility. A reader arriving at the decode site to learn
what the field is for leaves believing a specific hardware fact that the header
two files away disproves.

## Why the fix missed it

The header scopes its own correction to *"an earlier draft of **this
comment**"*. That scoping is accurate and it is the trap: the surviving copy is
a **different comment** that made the **same claim**, so a reader — or the
author — checking that the correction landed finds it landed, exactly where the
sentence says it did.

**A correction updates the place where you noticed.** You notice while editing
the constant, so the constant's comment gets the full treatment including the
falsification record; the decode comment is in another file, was not on screen,
and was not what prompted the edit.

This is the fourth instance of that shape recorded in this sweep alone
([[seam-scripture-invariant-mirror-drift]] for the invariant mirror, the gate
note's summary versus its body, the console dossier's inherited claim, and this).
The distinguishing feature here is that **the corrected text explicitly records
the falsification**, so the two comments do not merely differ — they disagree in
writing about a measured result.
