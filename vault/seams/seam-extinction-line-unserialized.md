---
id: seam-extinction-line-unserialized
type: seam
title: "The fix protected the banner and left the string 14 of 15 consumers actually match"
status: open
surface: abi-boot-banner
opened-by: chg-2026-08-16-cons-writer-set
tracker: "unfiled -- yip to main 2026-08-16"
created: 2026-08-16
updated: 2026-08-16
---
## Owed

A delivery guarantee for the crash-line literal, or a recorded decision that it
does not get one.

The console tearing defect — a kernel emitter holding no writer role, interleaved
byte-wise by a peer writing the same device — was closed for the boot-success
line by enrolling that emitter in the role. The crash path was **deliberately
excluded**: it runs on a dying machine and must stay lock-free and bounded, so a
primitive that can park is exactly wrong there.

That reasoning is correct. The consequence was not written down, and it inverts
the fix's value.

**Classified by which literal each of the fifteen declared mirrors actually
matches**: eight match the now-serialized success line; **fourteen match the
still-unserialized crash prefix**; one matches only the base-address line. The
string that received the guarantee has roughly half the readership of the string
that did not.

The crash emitter uses the same lock-free byte-at-a-time path the banner used,
does **not** halt peer processors before printing, and its pre-emit ring flush is
a bounded try-lock that *skips* when a peer holds the lock — so the one case it
declines to handle is precisely a peer mid-write.

## What closes it

A **try**-acquire of the writer role rather than a park: take it if free, emit
unserialized if not. That preserves every property the exclusion was protecting
— no parking, bounded, non-recursing — while covering the common case where the
peer is not actually inside a write. It is the same shape the pre-emit flush
already uses, in the same file.

Or a deliberate record that a torn crash line is accepted, with the two costs
below stated, so the next reader is not surprised by them.

**Not a vault edit.** Both files are on the implementation branch.

## Risk while open

Two costs, different in kind, and neither degrades gracefully.

**A torn prefix loses a corruption verdict.** Every consumer checks the
start-of-line prefix first on every poll, and the multi-boot classifier keys its
corruption class on it. A tear does not produce a missing result — it produces
the **unclassified** bucket, which is the classifier's most expensive verdict and
the one it was redesigned to stop over-producing. So a real corruption is
demoted to "unexplained", from outside the classifier, by a mechanism nothing in
the classifier can see.

**A torn message body inverts a fault-injection result.** The fault gate matches
seven full crash-message strings — seventeen matches in that one file. A torn one
reports that a protection **did not fire**, on a run where it fired correctly.
That is a false negative on a safety mechanism, which is the worst direction a
gate can fail in.

Both are rare and neither leaves a trace distinguishable from the real failure
it imitates, which is what makes this worth a record rather than a note in
passing.

## Why the existing machinery could not catch it

The mirror check on this surface is derived, fails rather than warns, and carries
a positive control. It reasons entirely about **who reads the literal** — and is
right to, since that is what a value contract needs.

Nothing in it, or in any stricter version of it, addresses the string *arriving
intact*. **A contract on a value is silent about its delivery**, and the surface
note stated the delivery requirement in its first paragraph ("must appear on a
line by itself") while carrying no obligation that would produce it — because
that sentence reads as a property of the emitter and is actually a joint property
of the emitter and every concurrent writer of the device.

This was found only because the mirror rule forced an enumeration that had no
other reason to happen.
