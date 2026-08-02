---
id: lock-vma
type: lock
title: "Proc.vma_lock — the address-space table guard"
kind: spin
orders-before: [lock-buddy-zone]
guards: "one Proc's vmas list, its vma_count, and the page/VMA resource charges checked against it"
created: 2026-08-02
updated: 2026-08-02
---
## Discipline

A per-Proc spinlock, added when the first heavily-threaded server made a
multi-thread Proc's address space genuinely concurrent. The rule is total:
**every mutator of `p->vmas` and the demand-page reader hold it.** A walk that
skips it is reading a list another Thread may be splicing.

Two exemptions, both by construction rather than by argument: the exec setup path
and the teardown drain run single-threaded, before any peer exists or after all
have gone.

Order: `vma_lock -> v->lock -> buddy zone lock`. The nesting onto the Burrow's own
lock is what makes a cross-Proc share safe — Proc A holds A's `vma_lock` then the
shared Burrow's lock, Proc B holds B's then the same, and since a Proc never takes
*another* Proc's `vma_lock` there is no cycle.

Inbound, it is a **leaf below [[lock-proc-table]]**, and three callers rely on
that: the cross-Proc memory copy for a debugged target, the address-space render,
and the hardware-driver map paths all take the table lock first. Nothing held
under `vma_lock` reaches back for it.

**Bounded hold is a correctness property, not a courtesy.** It is taken with
interrupts off, and the address space may legitimately hold tens of thousands of
regions, so every consumer bounds its own walk: the debug memory copy clamps to a
page per call, and the `/proc/<pid>/maps` render commits whole rows and stops when
its output buffer fills — which bounds the walk by the *buffer*, not by the region
count. Removing that truncation without re-deriving the bound turns a diagnostic
read into a multi-millisecond interrupts-off hold.

Nothing that sleeps may run under it. The demand-page path that *does* block —
faulting a file-backed page in over 9P — is structured to do its blocking read
outside, then re-take the lock to install.
