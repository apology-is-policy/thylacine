---
id: chg-2026-08-16-fault-cow-break
type: chg
title: "The fault dispatcher gains the COW break, and a defect found by reading a contract"
date: 2026-08-16
arc: arc-vault
commits: ["56d0e433", "7e89a3b6"]
touched: [sub-kernel-fault]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The LINEAGE arc — AddrSpace extraction, the per-page share count, the COW clone
and break, stock `fork()` — reaching the fault handler. Pairs with
[[chg-2026-08-16-burrow-attribution]]: same invariant, opposite end.

## It is not a sixth backing arm, and saying so is the point

The dossier's table is indexed by **backing type** and answers a
**translation** fault: nothing is mapped here, what should be? The break
answers a **permission** fault on a page already mapped and readable, keyed on
a **VMA flag** rather than a Burrow type. Adding a row would have been the
category error — a second question at a different fault class, filed under the
first question's index.

Recording it separately is not tidiness. The table's rows are mutually
exclusive alternatives resolved by one switch; the break composes *with* them,
on a page some arm already installed.

## The decide is one step, and the lock is global on purpose

Two sharers of one page hold **different Burrow locks**, so no per-Burrow lock
can serialise "is my share the last one?". A global leaf lock can.

The model says the decide happens under the Burrow lock. Its *requirement* is
that drop-decide-act be **one step**, which the global lock satisfies — and
Plan 9 serialises its page refcount under the allocator lock for exactly this
reason. **This is the correct shape for departing from a spec**: the letter
differs, the obligation is met, the departure is argued at the site, and the
precedent is named. A future audit reading the model and then the code finds
the reconciliation already written instead of a discrepancy to prosecute.

The share-drop primitive returns **the free verdict, never the count** — a
caller that read a count and then acted would race precisely the way the buggy
configuration describes. The API shape *is* the safety property, which is the
same move as `burrow_charge_claim`'s read-and-clear one layer over.

## The defect found by reading, not by testing

`mmu_install_user_pte` **refuses** a mismatching install over a valid leaf: it
returns failure rather than overwriting. **Both break outcomes mismatch** — the
copy path changes the physical address, take-in-place changes the permission
bits.

A *read* of a COW page installs a read-only PTE. So the first **write after a
read** would have failed its install and killed the Proc. The uninstall at the
top of the write branch is what makes read-then-write work at all — and it
reads as a redundant step unless you know the install primitive's refusal
contract.

**How it was found is the transferable part.** Not by a test: a test catches
this only if some case happens to do read-then-write on a COW page, and the
obvious tests do write-first. It was found by reading the contract of the
primitive being called. Revert-probed afterwards — the suite fails at exactly
that assertion and nothing else.

Same shape as [[sub-kernel-stalk]]'s through-a-file gate, where the answer was
already a field on the object and nobody had asked it. **The primitive's
contract is a place defects hide precisely because calling it feels like
delegating the question.**

## A sabotage that passed, and it was theirs to find

Six revert probes. Five failed at their own assertion; **the sixth did not fire
at all.**

The refuse test asserted "a failed fork flags nothing" while mapping only the
eager VMA that *caused* the refusal — not a COW candidate, so the flag pass
skipped it whether gated or not. **The assertion named a property it could not
observe.** Fixed by mapping a lazy VMA alongside the eager one, after which the
probe fails.

That is the vault's own pinned lesson arriving from the implementation side on
the same day it arrived three times from mine. A sabotage that quietly passes
is the finding, and it is only visible if every probe is required to fire.

## Four properties that look like details

**The parent is modified by the fork, and must be.** Its already-installed
writable PTEs for every COW range are uninstalled so its next touch re-faults
read-only. Leaving them is the [[inv-i44]] violation stated directly — the
parent writing through a stale writable translation into a page the child now
shares. The pass runs on **success only**, so a refused fork leaves the parent
exactly as found.

**The break's retained share IS the model's pin.** The copy path holds its own
share across the allocate and the copy, releasing only when the copy is done.
The model carries a separate `pin` variable; realising it with a held share is
**strictly stronger**, since a held share also keeps the count off zero. A
refinement, not a deviation — and distinguishing the two is exactly what the
previous section's global-lock argument also does.

**The COW flag is never cleared.** The flag is *routing*; the per-page count is
the *truth*. A VMA whose pages have all been taken in place costs one extra
fault per page; clearing it would need a scan proving no page in the range is
still shared, which is the worse trade.

**A non-resident slot stays NULL in the clone**, which is correct rather than
lazy: a page never written reads as zero, so each side demand-zeroing its own
after the fork is exactly the divergence a fork is supposed to produce.

## The charge is taken at the fork, not the break

Each address space maps the shared page, so each counts it — the Linux RSS
reading. That **over-counts physical memory between fork and break,
deliberately, in the safe direction**: the fork fails up front where the
failure can be reported, rather than the break running out later where there is
nowhere good to put it. The break takes no charge, since one mapped page
becomes one mapped page.

A deliberate over-count, argued by *where the failure can be handled* rather
than by accuracy. The accurate number would be worse.

## A count of mine, again

The heading and the title said **five** backing arms over a **six**-row table.
Second instance in this sweep after the notes dossier's "four families" over
five rows — both a header count contradicted by a table in the same block, both
surviving because no argument rested on them.
