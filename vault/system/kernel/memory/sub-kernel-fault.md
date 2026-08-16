---
id: sub-kernel-fault
type: sub
parent: moc-kernel-memory
title: "The fault dispatcher — classification, demand paging, six backing arms, and the COW break"
code: [arch/arm64/fault.c, arch/arm64/fault.h]
audit: hard
guarded-by: [inv-i12, inv-i32, inv-i7, inv-i36, inv-i44]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md", "docs/EXEC-LOAD-DESIGN.md"]
created: 2026-08-03
updated: 2026-08-16
---
## Purpose

Every translation and permission fault in the system arrives here. The
dispatcher decides whether the fault is a bug (extinct), a dying Proc's problem
(terminate it), or the normal way memory gets populated (resolve it and resume).

At v1.0 the third case is the common one: user memory is not mapped when it is
created, it is mapped when it is first touched. This file is where "first
touch" becomes a page table entry.

## Contract

`fault_info_decode` turns the three exception registers into a struct: the
faulting address, the PC, the class, and five booleans — from-user,
instruction-vs-data, write-vs-read, translation-vs-permission-vs-access-flag.
Pure; it reads no kernel state.

`arch_fault_handle` classifies and dispatches. `userland_demand_page` resolves
a user fault. Four outcomes:

| result | meaning |
|---|---|
| `FAULT_HANDLED` | resolved; the return-from-exception re-runs the faulting instruction |
| `FAULT_UNHANDLED_USER` | bad address or denied permission — the caller terminates the Proc with `snare:segv` |
| `FAULT_USER_BUS` | a **valid** mapping whose backing store failed — `snare:bus` |
| `FAULT_FATAL` | reserved; fatal kernel paths extinct in place |

The `FAULT_USER_BUS` / `FAULT_UNHANDLED_USER` distinction is the substance, not
bookkeeping. A bad address is the program's fault. A file-backed text page that
could not be read is the *filesystem's* fault, and conflating them would report
a wedged FS server as a segfault in the victim.

## Mechanism

**Kernel-side classification runs first, in a fixed order**, and every branch
extincts: stack-guard hit, W^X violation in the kernel image, translation
fault, permission fault, access-flag fault, catch-all. The order encodes
specificity — a guard-page hit is a stack overflow, and saying so is worth more
than "unhandled translation fault at 0xffff…".

Three details in that path are scar tissue and should be read as such:

*The re-entrancy guard.* The handler itself can fault — it dereferences the
current thread to name which stack overflowed, so a corrupted thread pointer
faults *inside* the handler. Unguarded that recurses one exception frame per
fault until the boot stack crosses its own guard, and **the real bug
masquerades as a stack overflow**. The per-CPU flag is never cleared: a kernel
fault is fatal either way, and the flag only has to outlive the recursion.

*The guard-page message names its flavour.* Boot stack, secondary stack,
boot-CPU idle stack, current thread's kstack — four distinct messages, because a
wild stack pointer landing in the wrong guard reads as an overflow of a stack
that provably could not have overflowed, and that ambiguity cost a debugging
session.

*A wild CPU index is clamped to 0 rather than skipped*, so the guard stays live
under exactly the corruption it exists for instead of disabling itself.

**The user path** is `vma_lookup` → permission check → resolve backing PA →
install the PTE, all under `vma_lock`, with one arm that cannot run under a
spinlock.

## Data structures

`struct fault_info` — decoded, pure data, no kernel pointers, which is what
lets tests drive the whole path with a synthetic fault instead of arranging a
real one.

`struct file_fault_req` — the file-backed miss request. It exists because that
one arm must sleep: it carries the pinned Burrow, the backing channel, the file
offset, the target address, the slot index, and whether the mapping is
executable.

## Concurrency

**The whole fast path runs under `Proc.vma_lock`** — lookup, resolve and
install as one atomic sequence. This is the #713 fix. Before it, the walker
raced a sibling thread's detach and could install a leaf PTE aliasing a page
already recycled into kernel memory: a wild kernel pointer appearing inside a
user address space. The same lock serializes two concurrent faults that would
otherwise race sub-table construction and orphan one.

Lock order is `vma_lock → burrow lock → buddy`, matching the attach path, so
there is no inversion.

**The file arm breaks the lock deliberately**, and the protocol is the
interesting part of this file:

1. Under `vma_lock`, observe the slot is not resident. Take a Burrow
   **reference** and record the request. The VMA holds a mapping ref, so the
   Burrow is alive *now*, which is what makes taking the ref safe.
2. Drop the lock. Read from the backing file — a 9P round trip that blocks, and
   that a dying Proc unwinds out of by inheritance from the read itself, with
   no new wait/wake machinery.
3. Re-take the lock. **Re-look up the VMA and re-validate** it still maps the
   same pinned Burrow — a sibling may have torn it down while we slept.
4. Install-once under the Burrow lock: if a sibling filled the slot, keep
   theirs and free ours.

The pin is what makes step 3 safe against both use-after-free and address
reuse: the Burrow cannot be freed *or* have its allocator slot recycled under
us. It is dropped exactly once, outside `vma_lock`, because the last reference
may sleep.

## Invariants enforced

[[inv-i12]] — every install passes `vma->prot` through unchanged. The
dispatcher makes no permission decision of its own; that is why the gate can
live in one place ([[sub-kernel-vma]]).

[[inv-i36]] — this dossier holds conditions 5 and 6, the two that were
genuinely new work: the page-in is death-interruptible, and an I/O error
terminates the Proc rather than installing zeros where instructions should be.
The other five live in [[sub-kernel-exec]], [[sub-kernel-image]] and Stratum.

The note did not exist when this dossier was written, deliberately: half its
enforcement was unswept, and an invariant written from half its enforcement is
the error this arc keeps finding. It was minted once exec and the image cache
were read.

[[inv-i32]] — the lazy-anonymous arm charges the page budget **before** the
allocation, so the count equals true resident-set size and a cap hit frees
nothing. Over-budget fails the fault, which terminates one Proc — never the box.

## The backing arms

**Six**, not the five this heading and the title claimed until 2026-08-16 —
against a table in the same block, which is the second miscount of exactly that
shape found in one sweep (the notes dossier said "four families" over five
rows). A count no argument rests on is invisible to every reader, its author
included.

| type | resolution | notes |
|---|---|---|
| anonymous | contiguous chunk; offset arithmetic | the ordinary case |
| **code** | *identical to anonymous* | I-42/JIT: two aliases of one region, each installing at **its own** VMA prot |
| MMIO | device PA + offset, device attributes | |
| DMA | pinned chunk PA + offset, cacheable | coherent on this platform's transports |
| file-backed | sparse per-slot pages, demand-read | the arm that sleeps |
| lazy-anonymous | allocate + zero + install-once, all under the lock | no backing read, so no slow path |

The code arm shares the anonymous arm **because it must**: a JIT region is
mapped twice, writable at one address and executable at another, and both
aliases fault through here. Each installs at its own VMA's prot, so no
code-specific PTE path exists that could drift away from W^X.

**But the comment on that arm overstates where the safety comes from.** It says
the W^X decision "stays entirely in `make_user_pte_l3`, which is what makes
'no PTE is ever W AND X' a property of the encoder." The encoder does no such
thing — handed `WRITE|EXEC` it emits a writable, user-executable PTE faithfully.
The property holds because `vma_alloc` refuses to create such a VMA. On the one
surface that deliberately holds two mappings of one code region, the comment
points at the wrong guard. Task #59.

## The COW break — a different axis, not a seventh row

The table above is indexed by **backing type** and answers a **translation**
fault: nothing is mapped here, what should be? The copy-on-write break answers
a **permission** fault on a page that is already mapped and readable, and it is
keyed on a **VMA flag** rather than a Burrow type. Adding it as a row would be
the category error; it is a second question asked at a different fault class.

**The decision is one step under a global leaf lock.** Two sharers of one page
hold *different* Burrow locks, so no per-Burrow lock can serialise "is my share
the last one?". The model says the decide happens under the Burrow lock; its
actual requirement is that drop-decide-act be **one step**, which a global leaf
lock satisfies — and Plan 9 serialises its page refcount under the allocator
lock for exactly this reason. Worth reading as the correct way to depart from a
spec: the letter differs, the obligation is met, and the departure is argued at
the site rather than discovered later.

The share-drop primitive returns the **free verdict**, never the count. A
caller that read a count and then acted would be racing precisely the way the
buggy configuration describes, so the API shape *is* the safety property.

**Two outcomes, and both change the PTE:** copy into a fresh private page, or —
when this sharer is the last — take the page in place and re-install it
writable.

### The defect found by reading, not by testing

`mmu_install_user_pte` **refuses** a mismatching install over a valid leaf — it
returns failure rather than overwriting. **Both break outcomes mismatch**: the
copy path changes the physical address, and take-in-place changes the
permission bits.

Since a *read* of a COW page installs a read-only PTE, the first **write after
a read** would have failed its install and killed the Proc. The uninstall at
the top of the write branch is what makes read-then-write work at all — a step
that reads as redundant unless you know the install primitive's refusal
contract. Revert-probed: the suite fails at exactly that assertion and nothing
else.

It is worth noting how this was found. Not by a test — a test would have caught
it only if some case did read-then-write on a COW page — but by **reading the
contract of the primitive being called**. The same shape as the through-a-file
gate in [[sub-kernel-stalk]], where the answer was already a field on the object
and nobody had asked it.

### Three properties that look like details and are not

**The break's retained share is the model's pin.** The copy path holds its own
share across the allocate and the copy, releasing only once the copy is done.
The model carries a separate `pin` variable; realising it with a held share is
**strictly stronger** — a held share also keeps the count off zero — so this
refines the model rather than deviating from it.

**The parent is modified by the fork, and must be.** Its already-installed
writable PTEs for every COW range are uninstalled, so its next touch re-faults
read-only. Leaving them is the [[inv-i44]] violation directly: the parent
writing through a stale writable translation into a page the child now shares.
That pass runs on **success only**, so a refused fork leaves the parent exactly
as it was found.

**The COW flag is never cleared.** The flag is *routing*; the per-page count is
the *truth*. A VMA whose pages have all been taken in place costs one extra
fault per page — clearing it would require a scan proving no page in the range
is still shared, which is a worse trade than the fault.

### The charge is taken at the fork, not the break

Each address space maps the shared page, so each counts it — the Linux RSS
reading. That **over-counts physical memory between fork and break,
deliberately, in the safe direction**: the fork fails up front where the
failure can be reported, rather than the break running out later where there is
nowhere good to put it. The break itself takes no charge, since one mapped page
becomes one mapped page. See [[sub-kernel-burrow]] for the attribution half.

## Error paths

Kernel faults extinct with the faulting address and a message naming the class.
User faults return a result the caller turns into a per-Proc termination with
the matching `snare:*` note. A read failure in the file arm is **fail-closed**:
`FAULT_USER_BUS`, never a silent zero-fill of executable text — filling text
with zeros on an I/O error would execute them.

## Performance

The read-ahead cluster is the one performance mechanism here, and it exists
because of an amplification measured downstream: a 4 KiB demand read lands in a
multi-megabyte encrypted extent that the filesystem decrypts and verifies
*whole*. Paging a toolchain that way is thousands of round trips against
thousands of whole-extent decrypts.

So the **fill** batches — one read for a 64-page cluster — while the **install**
stays per-fault: a cluster-mate's later touch hits the resident fast path and
installs its own PTE with no read. It is byte-identical to N sequential
single-page reads, and **best-effort by construction**: any allocation shortfall
or a degenerate one-page cluster degrades to the single-page path. Read-ahead
can never fail a fault.

Each cluster page is instruction-cache-synced *before* any PTE can back it,
because the page was filled through the data path and instruction fetch is not
coherent with it — a stale line from the recycled page's previous occupant
would be executed.

## Prosecution

On any change here: that the file arm's four-step protocol keeps its pin across
the sleep and its re-validation after it; that install-once stays install-once
on both the single and cluster paths, with the loser's page freed outside the
Burrow lock; that the lazy arm charges before allocating and uncharges on every
failure; that read-ahead stays byte-identical to sequential reads and stays
degradable; that no arm invents a permission (they must all pass `vma->prot`);
and that the fail-closed posture on read errors is never relaxed into a
zero-fill.

## Seams

- The header's stale claim that this path is single-threaded and needs a future
  lock — the body documents the fix in detail directly below it (task #60).
- A future path that maps a file-backed Burrow at a chosen offset would
  invalidate the cached slot index the slow path carries across its sleep; both
  the single and cluster paths say so at the point it would matter.

## Caveats

Instruction aborts report as reads, which is correct — a fetch *is* a read — and
falls out of the encoding rather than being special-cased.

The access-flag arm is defensive: this kernel sets the access flag eagerly, so
those faults should not occur. Classifying them anyway means the day one appears
it is named rather than landing in the catch-all.

## Provenance

P3-C built the classifier; P3-Dc added demand paging; P6 #713 added the lock
coverage; REVENANT added the file-backed arm and later its read-ahead; the
overcommit model added the lazy-anonymous arm; I-42 added the code arm.

## Tests

`demand_page.*` drives the path with synthetic faults and manufactured Procs —
including the read-ahead cluster's per-slot byte map, its boundedness, its
one-batched-read property, an interior short read, and the fail-closed arm. The
production path is exercised by every EL0 first touch on every boot.

## Referenced by

[[moc-kernel-memory]] · [[sub-kernel-vma]] · [[sub-kernel-mmu]] ·
[[sub-kernel-burrow]] · [[inv-i12]] · [[inv-i32]]
