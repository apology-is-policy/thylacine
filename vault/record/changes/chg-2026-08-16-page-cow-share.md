---
id: chg-2026-08-16-page-cow-share
type: chg
title: "A blind instrument reports the number that means broken"
date: 2026-08-16
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-mm-phys]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
One commit — the copy-on-write share count's substrate half. The field itself is
small and careful; the two failed test instruments in front of it are the part
worth keeping.

## The dossier's prediction was tested and held

This dossier already carried the refcount trap, and it ended with a prediction:
*a lineage or copy-on-write design that reads `page.refcount` as a share count is
wrong on arrival.*

The arc that needed exactly such a count did not take it — and not on the general
warning. It took it on two **measured** grounds: the buddy writes that field per
**block head**, so every tail page of a higher-order block carries a stale value;
and the slab layer double-books it as an inuse count observed reaching 85.

**A warning that gets acted on for its own reasons rather than its authority is
the good outcome**, and the difference is visible: the new field's justification
cites what was measured, not what a note said.

## The contract is the correctness, and the allocator's part is deceptive

The new field is meaningful only while a page sits in an anonymous mapping slot,
and it is **established, never inherited**. A page recycled through the buddy
carries whatever its last owner left, so every site that installs one into such a
slot sets it — a closed, enumerated set of three. The operations extinct on a
zero count rather than guess, because zero means a site skipped the establish.

The subtle part is that **the allocator zeroes it at initialization and does not
maintain it.** Those look the same from the outside. A field the allocator zeroes
reads as a field the allocator owns — which is precisely the history of the
neighbouring trap, described in the source as *"a field whose name states a
contract nothing keeps."* The distinction is written down because nothing in the
code expresses it.

Also worth recording as a live cost: **the padding slack is gone.** The field took
the struct's spare four bytes, so the size assertion held and the per-RAM
reservation did not move. The next field costs eight bytes of struct and a
proportional slice of reserved memory at every RAM size, and there is no slot left
to absorb one quietly.

## Two instruments were wrong before one was right

The integration test asks a simple question — did the teardown return the page to
the allocator — and both obvious ways to ask it are traps, in **opposite**
directions.

**The first passed vacuously.** It asserted on the free flag. Unusable here: the
buddy's coalesce anchors on the lower-numbered buddy, so a freed page that merges
rightward never gets the flag set on its own descriptor. The assertion succeeded
without measuring anything.

**The second failed, and failed in the shape of a real bug.** It sampled the
allocator's free-page count across the teardown and reported **zero delta for both
the shared and the private case** — which reads exactly like "the conditional free
is broken."

It was not broken. **An order-0 free does not reach the buddy at all**: it is
pushed onto a per-CPU magazine, so the free count never moves. The instrument was
blind to both arms *equally*.

That is the trap worth naming. **A blind instrument does not report "cannot
measure" — it reports the number that means broken.** And the correct-looking
response to a red is to go fix the mechanism. The only reason a working mechanism
did not get "fixed" around this one is that the diagnosis came from a temporary
diagnostic rather than from reasoning about what the number ought to be.

Draining the magazines before each sample restores the figure to a true total.
Measured with it: the private teardown returns one page, the shared teardown
returns zero.

**A vacuous pass and a blind zero are the same defect at different signs** — in
both cases the instrument is disconnected from the property, and only one of the
two announces itself. The one that announces itself is the safer failure, which
is an argument for preferring an instrument that can be *wrong* over one that can
only be *quiet*.

The surviving assertion is on the **difference between two otherwise identical
runs**, so the incidental allocations along the path cancel instead of each having
to be reasoned about.

## Two smaller things I would have got wrong

**The spec says one thing and the implementation correctly does another.** The
model specifies the decide "under the mapping lock". The implementation uses a
**global** lock, because two sharers of one page hold *different* mapping locks —
so no per-mapping lock could serialize the decision at all. The model's
requirement is that drop-decide-act be one step; the lock it names is one way to
get that, not the requirement itself. The heritage system serializes its own page
reference under a single global lock for the same reason.

**Reading the spec's spelling instead of its requirement would have produced an
unserializable design that matched the document.**

**And two counts that look like they should agree correctly do not.** The
decommit path keeps counting *slots* for its resource accounting, not surviving
pages: this address space stops mapping the page either way, so its usage drops
whether or not a co-sharer keeps the page alive. Same event, two different
numbers, both right.

## A chunk no existing test could check

The change is behaviourally inert — nothing shares yet, so every count is one and
every release reports last. The suite came back byte-identical to the previous
baseline.

**A chunk that changes no observable behaviour is a chunk no existing test can
check**, so four tests manufacture the state directly, including the one asserting
that *deciding* does not drop the share — because the retained share is the
model's pin. The revert probe fails one assertion and no other.
