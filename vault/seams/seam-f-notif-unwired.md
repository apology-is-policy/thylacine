---
id: seam-f-notif-unwired
type: seam
title: "The buffer-lifetime tracker has no production caller"
status: open
surface: [sub-kernel-weft, sub-kernel-loom]
opened-by: chg-2026-08-02-async-sweep
tracker: ""
created: 2026-08-02
updated: 2026-08-02
---
## What

[[inv-i37]]'s third clause — a page still in flight is never reused — names the
holder-set tracker as its defence. The tracker is complete: arm, arm-as-copied,
clear-one-holder, in-flight query, result flags. It is modelled
([[spec-weft]]'s `PinHeldWhileInFlight` and `NoInFlightReuse`, with the premature
release as an executable counterexample) and it has its own unit tests.

**Nothing in production calls it.** The five entry points have callers only in
the test file. No in-flight operation carries a tracker; the notification
completion flag is defined and commented but never set on any path.

## Why the system is nonetheless safe today

The server copies the ring contents into its own socket buffer before replying.
So the client's slice is reusable the instant the reply returns, no page is ever
in flight past its operation, and a single terminal completion is the
reusability signal. This is the "copied" path of a zero-copy send interface, and
it is documented as the deliberate as-built state.

## Why it is worth recording anyway

The safety comes from somewhere other than where the invariant says it comes
from. Today's guarantee lives in a userspace daemon's decision to copy — a
different mechanism, in a different layer, in another file — while the invariant
names a kernel tracker that never runs.

That matters for the next change rather than for today. The moment the server
gains a hold-the-page transmit path, the copy disappears and the property stops
holding by itself. At that point the tracker has to be wired onto the in-flight
operation, and the pin release has to move from reap to the notification
terminal. Both are described; neither is exercised, so neither is known to work
in composition.

The forward-compatible part is already right: a consumer decides whether to wait
by reading the "more follows" flag on the result completion, which is clear on
today's copied path and would be set on a deferred one. So consumers written
against the current behaviour do not need changing — they are already asking the
right question.

## Trigger

A transmit path in which the server holds the client's page instead of copying
it. Also anything that makes a device DMA directly from the shared region, or
that keeps a page pinned until a peer acknowledgement.

## No task

Not a defect and not reachable: there is no path that arms a holder, so there is
nothing to fix and nothing to trip. It is a defence waiting for the situation it
was built for, and the record exists so that situation does not arrive quietly.
