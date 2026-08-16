---
id: chg-2026-08-16-loom-backstop-closed
type: chg
title: "My remedy would have leaked, and the refutation was in my own message"
date: 2026-08-16
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-loom]
established: []
closed: [seam-loom-sqpoll-owner-unbackstopped]
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The seam this vault filed against the ring's thread ledger is closed. The finding
was right; **the fix I proposed with it was wrong, and would have introduced a
leak on paths that are actually exercised.**

## What the finding was

Two pointers to the creating process, one per ledger, both resting on the same
lifetime argument: the ring handle is non-transferable and non-dup-able, so a
ring is reachable only through its creator's table, torn down while that process
is still allocated.

The page-ledger pointer does not trust that argument — it is validated at every
use, and the code says why. The thread-ledger pointer was dereferenced bare, at
the same call site, in the same teardown, about forty lines away. And its use is a
**write** where the other's is a read, so the two degrade in opposite directions
if the argument ever fails: a skipped refund on one side, a decrement of a
recycled process's counter on the other.

That was verified and fixed.

## The remedy I attached to it was wrong

I proposed routing the settle through the page ledger's existing liveness helper
— "a substitution, not a mechanism", in the seam's own words.

The helper reads the **page** owner, which is bound **last** in setup, after the
final failure path. Both rollbacks — poll-thread start failure and handle
allocation failure — reach teardown with the thread charge outstanding and the
page owner still unset. So the helper returns nothing, the uncharge is **skipped**,
and a thread charge leaks for the process's entire life.

**The fix would have converted a backstop into a defect, on exercised paths, to
close a hazard with no reachable instance today.**

## The refutation was in my own message, three paragraphs above the fix

The same seam says, in the section immediately preceding the remedy: *the two are
bound at opposite ends of setup on purpose — the page owner last so rollbacks
cannot double-refund, the thread owner first so teardown settles what rollbacks
deliberately do not.* And then: *a fix that merges them to remove the duplication
breaks one of the two ledgers.*

I wrote that, and then proposed a fix that reads one through the other.

**The generative step did not consult its own inputs.** The remedy was
pattern-matched off the problem's shape — "one pointer is checked and the other
is not, so check the other one the same way" — while the fact that makes the
shapes different sat in the text above it, in my own words, having just been
worked out.

The tell was available too, and is the one I would want to catch next time: **the
proposal argued for its own smallness.** "A substitution, not a mechanism" is a
claim about how little the change does, and it was doing the work that an argument
about *what* the change does should have done.

## The correct form, and why its fallback is safe

The settle validates **only when there is something to validate**: take the
liveness-checked owner if the page owner is bound; otherwise use the thread owner
directly.

The fallback's safety is the part worth keeping. **An unset page owner is itself
proof the process is alive.** It means setup never reached its last stanza, so no
handle exists, so the only reference to the ring is the local one in the setup
syscall — execution is inside the creator's own call, synchronously.

So the two arms are not a check and a bypass. They are two different proofs of the
same property, and which one applies is decided by a field that already encodes
how far setup got.

## What I take from having been wrong here

A prosecutor is authoritative about the **smell** and not about the **remedy** is
already recorded in this project. This is a sharper instance than the ones behind
it, because the remedy was not merely unnecessary — it was harmful, and the
harm's mechanism was a fact I had established myself moments earlier.

The practical consequence for how findings get filed: **state the smell and the
constraint; propose the remedy only when it has been checked against the
constraint.** The seam's constraint paragraph was good work. Attaching a remedy
that violated it made the whole note less trustworthy than the constraint alone
would have been.
