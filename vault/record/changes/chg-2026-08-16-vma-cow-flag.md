---
id: chg-2026-08-16-vma-cow-flag
type: chg
title: "Routing is not truth, and a stale docblock got wrong twice over"
date: 2026-08-16
arc: arc-vault
commits: ["ac337061"]
touched: [sub-kernel-vma]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The address-space extraction and the copy-on-write arc both landed here. Most of
the mechanism was already recorded from the neighbouring surfaces this sweep —
what this one adds is the mapping layer's own three details, each of which would
be natural to get backwards.

## The flag routes; the count decides

A mapping marked copy-on-write does not mean its pages are shared. It means *a
write here must go through the break path*. The per-page share count is what
decides each individual break.

Consequences follow immediately and cleanly: the flag is **never cleared**,
because clearing it would need a scan proving no page in the range is still
shared — more work than the extra faults it saves, and a scan that could race the
sharing it is checking for. A mapping whose pages have all been taken in place
costs one fault per page and nothing else.

**Separating routing from truth is what makes "never cleared" correct rather
than lazy.** If the flag claimed the pages *were* shared, leaving it set would be
a lie; because it only claims a path must be taken, leaving it set is merely
conservative.

## The permissions deliberately disagree with the hardware

The mapping keeps its **write** permission and the page table entry is what goes
read-only.

That inversion is load-bearing and reads like a bug: the fault handler's
permission check consults the mapping, so a mapping that dropped write would turn
every copy-on-write write into a fatal fault instead of a break. The entry is the
enforcement; the mapping's permission states what the program is *allowed* to
do. During copy-on-write those are different questions, and collapsing them
breaks the mechanism in the direction that looks safer.

Recorded with an explicit note that this does not touch W^X — a copy-on-write
mapping is writable and not executable, so the rejected combination never arises
— because "the permissions and the hardware disagree" is exactly the sentence
that should make a reader check.

## The docblock is now wrong twice over

The header's mapping-insert comment still says multi-threaded processes "need a
per-Proc lock around the list; documented as a trip-hazard when added", while a
sibling comment twenty-five lines below instructs the caller to hold that very
lock. Already contradictory; already tracked.

The extraction made it worse in a specific way: **the lock exists, and it is no
longer per-Proc.** A reader following the stale docblock would add a lock to the
wrong structure, and the result would look correct while serializing nothing
between two processes sharing one address space.

**Staleness that merely lags is annoying; staleness that now points at the wrong
object is a trap.** The same words got more dangerous without being edited,
because the world moved under them — which is the argument for re-reading a known-
stale note after a structural change rather than leaving it queued at its
original severity.

## Two smaller things

The release primitive now **reports whether it was the drop that freed the
pages**, because "the mapping went away" and "the pages went away" are different
events once a region has a second owner, and which drop is last cannot be
predicted from the mapping's type nor from a count sampled beforehand. Only the
drop knows, so it answers.

And the no-cycle argument for the list lock changed underneath without the
property changing: it used to be *disjointness* — a process never takes another
process's list lock. Two sharers now take **the same** lock, so what carries it
is the ordering. The conclusion survived; the reason did not, and a reader
checking the old reason would find it false and the property fine.
