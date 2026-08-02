---
id: seam-gic-handler-slot-never-cleared
type: seam
status: open
title: "The interrupt controller's handler slot cannot be unregistered"
surface: [sub-kernel-gic, sub-kernel-irqfwd]
opened-by: chg-2026-08-02-devices-interrupt-time-sweep
tracker: ""
created: 2026-08-02
updated: 2026-08-02
---
## What

The controller's attach entry point **rejects a null handler by design**. The
stated reason is good: detaching should go through the explicit disable call,
not through an attach that quietly arms a future "no handler" fatal error. The
API forces callers to say what they mean.

The consequence is that there is no way to say the other thing. When a lent
interrupt's object is torn down, the natural step — unregister the handler — has
no expressible form. The teardown path calls it anyway:

    gic_attach(k->intid, NULL, NULL);

and the comment beside it says plainly that this returns false and the slot
retains its handler and its argument. The argument is the object about to be
freed. **The slot permanently points at freed memory.**

## Why nothing goes wrong

Three defences, and they are the reason the file is shaped the way it is:

1. **The interrupt is disabled first**, so the controller will not route to that
   slot at all.
2. **A dying flag plus an in-flight marker** catch the one arrival that disabling
   cannot prevent — one already acknowledged and executing on another CPU. The
   teardown spins until that dispatch's last touch of the object has completed.
3. **The object's magic value is overwritten** before the memory is released, so
   a stale dereference fails its own sanity check.

And the thing that would actually be dangerous — re-enabling an interrupt whose
slot is stale — does not exist. Every enable call site in the tree is
immediately preceded by an attach that overwrites the slot first. Verified by
census: five attach sites, each followed by its enable; six enable sites, each
preceded by an attach or reserved for a kernel handler that is never torn down.

## The shape of it

This is a defence-in-depth structure standing in for a missing operation, and
the tree is honest about that at every step. What is worth recording is the
*direction*: a safety rule at the interface (reject the ambiguous call) removed
the mechanism the interface's own lifecycle needed, and the cost was paid in the
caller as three overlapping guards rather than in the interface as one more
entry point.

An explicit detach — a separate call that clears the slot, rather than an
overloaded attach — would collapse all three into "the slot no longer points
anywhere". It would not make anything safer today; it would make the safety
local instead of distributed.

## Consequence

None today. The freed pointer is unreachable because the only route to it is
disabled, and the only way to re-enable it overwrites it first.

The exposure is entirely to future change: a code path that enables an interrupt
without attaching, or a claim-layer change that lets a number be re-enabled
between teardown and the next create, would dispatch into freed memory. Both
would have to defeat conventions that are currently uniform but nowhere
enforced — no build-time or run-time check requires attach before enable.

## No task

Nothing is wrong, the reasoning is written at both sites, and the three defences
are individually sound. Recorded because the missing operation is invisible from
the interface — a reader of the controller's header sees a complete-looking API
with no hint that its one lifecycle-critical case cannot be expressed — and
because the invariant that keeps it safe (attach always precedes enable) is a
convention rather than a mechanism.
