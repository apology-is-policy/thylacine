---
id: chg-2026-08-16-weft-third-kind
type: chg
title: "A third shareable kind, and a leak that breached nothing only by coincidence"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [sub-kernel-weft]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
Two arcs reached the shared-page substrate since it was written: the graphics
work added a kind, and the resource-attribution work found that the *sharer's*
side of the budget had never been settled at all.

Both were reached correctly this time — by asking which commits **arrived** on
this branch after the dossier's date rather than which were authored after it.
One of the four (the address-space extraction) predates the dossier by a day and
landed four days later, and an author-date query would have dropped it. That
lesson was learned two surfaces earlier in this same sweep and applied here.

## The third kind, and the axis that separates it

Shareable regions were two: an anonymous flow ring, and a device-passive
framebuffer. There are now three — a graphics buffer object joins.

The distinction between the two device kinds is **which way the device moves the
bytes**: the framebuffer is read by the device, the buffer object is written by
it. Neither direction weakens the admission argument, and stating why is worth
more than stating the fact. What admission actually turns on is that the device
treats the region as **data** rather than as instructions — a command queue or
descriptor table stays structurally unshareable, because those are regions the
device *interprets*.

Both new-style kinds must declare an empty descriptor geometry, since neither has
a ring, and a server declaring one over them contradicts its own registration and
fails closed.

## The coupling, now visible from both ends

The two device kinds are stored as separate bits and read here by **test order**
— framebuffer first, buffer object second — in two different functions. That is
unambiguous only because a region cannot be both.

The guarantee is enforced in [[sub-kernel-hwcap]], by an enumerated constructor
argument that makes the illegal combination unwritable at the call. Nothing at
either reading site here says so.

I recorded this from the constructor's side earlier in the same sweep, as the
*cost* of making a state unconstructible: the check disappears from the reader
and the argument moves to another file. Arriving at the same coupling from the
consumer's side is the confirmation that mattered — **from here it is genuinely
invisible.** A reader of this file sees two independent booleans tested in an
order that looks arbitrary, and nothing prompts the question. Both dossiers now
carry it, which is the only place it can live, since neither file can express it.

## The leak, and why it was safe

The daemon detaches its ring when a flow closes — on **every** closed zero-copy
flow — while the guest's mapping and the binding's pin live on. So the drop that
finally frees the pages is the guest's address-space teardown: generic code, in
another process, holding that process's lock, with no way to name who paid.

Sixty-four pages per closed flow, monotonically, forever.

**It breached no bound only because the daemon happens to be exempt from the
resource floor**, and that exemption follows from an identity chain granted for
entirely unrelated reasons. Two independent gates coinciding — not a property
anyone enforced, and not one anyone had checked. The first non-exempt driver
turns it into a live monotonic leak.

That is the pattern worth extracting: **a bug bounded by a coincidence looks
exactly like a bug that is bounded.** Nothing in the observable behaviour
distinguishes "this cannot exceed the cap" from "the only thing doing it is
uncapped". The second becomes the first the moment the population changes.

## The release rule, and the test it deliberately is not

The sharer now settles its charge when the region **is shared out and it has
unmapped its own view** — whether or not the pages freed. Having handed the
region across and let go, it cannot reach those pages, and charging a process for
memory it cannot touch caps it for nothing; the consumer's own shared-mapping
axis accounts them from there.

The sharp part is what the rule is *not*. The obvious formulation — settle when
nothing else holds the region — is wrong, and wrong in the dangerous direction. A
process's **own** other claim, such as a registered buffer pinned by its own
async ring, also keeps the region alive, and there the charge must stay until
that claim drops. The broad test would release early, inflating the budget.

So the discriminator is specifically "shared **out**", not "still held". Two
predicates that agree on most inputs and disagree exactly where it matters — and
the one that reads more natural is the broken one.

## What a sweep at the right granularity buys

This surface and the hardware-handle surface were swept an hour apart, and the
second one's finding is what made the first one's coupling legible. Neither file
could have produced it alone: one holds the constructor, the other holds the
readers, and the invariant lives in the gap.

Recorded as an argument for sweeping *adjacent* surfaces close together rather
than in strict churn order when a finding suggests the neighbour — the churn
ranking is a good default precisely because it is indifferent, and indifference
is what you want until you have a reason.
