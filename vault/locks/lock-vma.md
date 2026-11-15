---
id: lock-vma
type: lock
title: "AddrSpace.lock — the address-space guard (was Proc.vma_lock)"
kind: spin
orders-before: [lock-burrow, lock-cow, lock-buddy-zone]
guards: "one address space's vmas list, and -- for the cap DECISION only -- the three I-32 counters charged against it"
created: 2026-08-02
updated: 2026-11-15
---
## Discipline

A spinlock added when the first heavily-threaded server made a
multi-thread Proc's address space genuinely concurrent. The rule is total:
**every mutator of the `vmas` list and the demand-page reader hold it.** A
walk that skips it is reading a list another Thread may be splicing.

Two exemptions, both by construction rather than by argument: the exec setup path
and the teardown drain run single-threaded, before any peer exists or after all
have gone.

**It is per-ADDRESS-SPACE, not per-Proc**, since the L-1 extraction moved it off
`struct Proc` — and that is not a rename. Under `rfork(RFPROC|RFMEM)` and after a
copy-on-write `fork`, **two Procs can hold one AddrSpace and therefore contend on
one lock**, which is exactly the sharing the extraction existed to make
representable. See [[sub-kernel-addrspace]].

Order: `as->lock -> v->lock -> buddy zone lock`, with [[lock-cow]] as a further
leaf beneath the Burrow lock on the copy-on-write break path. The nesting onto the
Burrow's own lock is what makes a cross-Proc share safe. The no-cycle argument
survived the extraction but its *reason* changed: it used to be "a Proc never
takes *another* Proc's `vma_lock`", and now two sharers of one address space take
**the same** lock rather than each its own — so the ordering, not the
disjointness, is what carries it.

Inbound, it sits **below [[lock-proc-table]]**: nothing held under it reaches
back for the table lock. No table-lock hold *waits* for it, though, because that
hold is global and masks interrupts while a space's holder can run long. The
cross-Proc memory copy for a debugged target only *tries* it under the hold; on a
busy lock it pins the space, drops the hold, waits with interrupts on, and runs
again from the start (operator vote 132). The `/proc/<pid>/maps` render and the
weft reaper pin the space under the hold and take its lock after the walk.

**Bounded hold is a correctness property, not a courtesy.** Every hold is
non-preemptible (`spin_lock` leaves interrupts as they were), and the address space
may legitimately hold tens of thousands of regions, so every consumer bounds its own walk: the debug memory copy clamps to a
page per call, and the `/proc/<pid>/maps` render commits whole rows and stops when
its output buffer fills — which bounds the walk by the *buffer*, not by the region
count. Removing that truncation without re-deriving the bound turns a diagnostic
read into a multi-millisecond non-preemptible hold. `addrspace_clone` is the one
consumer that does **not** bound its walk (three passes, linear in region count)
and is recorded as a seam on [[sub-kernel-addrspace]].

Nothing that sleeps may run under it. The demand-page path that *does* block —
faulting a file-backed page in over 9P — is structured to do its blocking read
outside, then re-take the lock to install.

**It does NOT serialise the I-32 counter arithmetic**, and the header that
declares those counters still says it does. The six charge/uncharge operations are
CAS loops and are correct with no lock held — they had to become so once the
uncharge moved to where pages actually free, which is reached from handle close
and holds no address-space lock. What the lock still buys is the **cap decision**:
holding it across check-then-charge is what makes a bound exact against a sibling,
and two charges from outside it can both pass and both land, overshooting by at
most the smaller. That is the documented I-32 floor, not exactness. The stale
precondition is tracked as task #165.
