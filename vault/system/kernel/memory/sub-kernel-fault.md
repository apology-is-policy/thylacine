---
id: sub-kernel-fault
type: sub
parent: moc-kernel-memory
title: "The fault dispatcher — classification, demand paging, seven backing arms, and the COW break"
code: [arch/arm64/fault.c, arch/arm64/fault.h]
audit: hard
guarded-by: [inv-i12, inv-i32, inv-i7, inv-i36, inv-i44]
validated-by: [spec-cow, spec-capacity, prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md", "docs/EXEC-LOAD-DESIGN.md"]
created: 2026-08-03
updated: 2026-09-23
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
| `FAULT_USER_BUS` | a **valid** mapping whose backing store failed, or an abort no page install can resolve — an alignment fault or a synchronous external abort on a mapped page (B-1a' round 4, F17) — `snare:bus` |
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

Two properties of that install are load-bearing and invisible. The offset is
**bounded**: `burrow_byte_off = vma->burrow_offset + (page_va - vma->vaddr_start)`
is rejected if `>= burrow->size`, so a mapping can never resolve past its
backing. And the install into a not-present leaf needs **no TLB invalidation** —
invalid→valid requires no flush (ARM ARM B2.7.1). The demand-page path never
overwrites a *valid* leaf: `mmu_install_user_pte` refuses a mismatching install
over one (the COW break does its own uninstall-first dance instead), so the fast
path issues no TLBI at all.

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

**The ordinary arms take no Burrow reference.** A demand-paged anonymous or lazy
page is just a PA installed into the VMA's *already-mapped* Burrow, whose
`mapping_count` is the liveness guarantee — so `NoUseAfterFree` holds by
construction without a fresh ref. Only the file arm, which drops the lock to
sleep, needs its own pin, exactly because it lets go of the lock that keeps the
mapping (and therefore the Burrow) alive.

**The file arm breaks the lock deliberately**, and the protocol is the
interesting part of this file:

1. Under `vma_lock`, observe the slot is not resident. Take a Burrow
   **reference** and record the request. The VMA holds a mapping ref, so the
   Burrow is alive *now*, which is what makes taking the ref safe.
2. Drop the lock. Read from the backing file — a 9P round trip that blocks, and
   that a dying Proc unwinds out of by inheritance from the read itself, with
   no new wait/wake machinery.
3. Re-take the lock. **Re-look up the VMA and re-validate the GEOMETRY** — not
   just that it still maps the same pinned Burrow (a sibling may have torn it
   down), but that `freq->page_va` is still inside `[vaddr_start, vaddr_end)` AND
   `slot_now == freq->slot`. On a mismatch, BAIL: free the read page, return
   `FAULT_UNHANDLED_USER`, and let a re-fault re-resolve against the new geometry.
4. Install-once through `pagemap_install` (B-1a'), which takes the Burrow
   lock itself and allocates the map's missing nodes OUTSIDE it (uncharged
   for FILE): if a sibling filled the slot, keep theirs and free ours; if the
   nodes could not be had, free ours and refuse the fault. The cluster path
   installs each slot the same way, and a node OOM midway leaves the earlier
   slots adopted (the bytes are the Burrow's) and the rest in `clpages` for
   the caller to free; the faulting slot decides the verdict as a lost race
   would.

**Step 3 verifies geometry because DISTRO D-3 retired the premise that once let
it check only identity (#190).** The R-5 audit's F2 justified trusting the cached
`freq->slot` on "a FILE Burrow is created only by exec, mapped once at offset 0,
never re-mapped" — so it had ONE fixed VMA and `page_va -> slot` was stable. D-3
is exactly the future that premise excluded, on every count: it creates FILE
Burrows from EL0, a MAP_FIXED replace SPLITS a FILE VMA (so a tail carries a
non-zero `burrow_offset`), and one Image-cached Burrow is reachable from several
address spaces at once. "Same Burrow?" answers yes to a split over that same
Burrow, so identity no longer suffices. And F2's *prescribed* remedy — "recompute
the slot from `vma->burrow_offset`" — is WRONG: `freq->file_offset` was ALSO
derived from the pre-sleep geometry and the read page already holds the bytes read
at it, so recomputing only the index files stale bytes under a fresh slot number
(a correctly-indexed slot holding the wrong page — the same corruption, tidier
address). The bytes and the index must agree, and only the mapping still being in
place guarantees that. Verify and bail; never recompute. Both the single and the
cluster path carry the identical check.

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
DISTRO D-3 GENERALIZED I-36 from exec text to phenotype mmap-time library maps —
the same seven conditions now gate a userspace read-only/exec file map — and this
arm's two conditions are unchanged by that generalization; the one fail-mode it
added is the #194 past-EOF `FAULT_USER_BUS` (Error paths).

The note did not exist when this dossier was written, deliberately: half its
enforcement was unswept, and an invariant written from half its enforcement is
the error this arc keeps finding. It was minted once exec and the image cache
were read.

[[inv-i32]] — the lazy-anonymous arm charges the page budget **before** the
allocation, so the count equals true resident-set size and a cap hit frees
nothing. Over-budget fails the fault, which terminates one Proc — never the box.
B-1a': the miss charges the DATA page first (`proc_page_charge(p, 1)`), then
`pagemap_install` charges the map's missing nodes to `p->as` (under
`proc_resource_exempt(p)`) before allocating them; a refused node charge, or a
node OOM, installs nothing, the data page is freed and uncharged, and the fault
answers `FAULT_UNHANDLED_USER` exactly as a refused page does -- so
`page_count` reads as data plus the nodes that index it ([[sub-kernel-pagemap]];
`capacity.pagemap_nodes_charged_and_reclaimed`, and `test_exec.c`'s charge
assertions read data + nodes). Since the round-1 close every page an arm
mints comes from `alloc_user_pages(0, .., proc_resource_exempt(p))` -- the
demand-zero page, the COW private copy, the FILE page-in's pages -- so the
machine-wide bound is decided at the allocation ([[sub-kernel-mm-phys]]: a
non-exempt Proc refused when the pool is full, the TCB never), and the PTE
install (`mmu_install_user_pte[_attr](p->as, proc_resource_exempt(p), ..)`)
charges the hardware tables it grows ([[sub-kernel-mmu]]); a refused table --
the cap or the pool -- returns -1 and every arm answers
`FAULT_UNHANDLED_USER` with the data page already resident and charged in
its slot, which the dying Proc's drain releases. The FILE arm installs with
`as = NULL`: an Image-cached page is shared and charged to no address space,
and so are its nodes -- but both are pool pages, counted.

## The backing arms

**Seven** since Warp-6 V-2 added the HOSTMEM arm. It read five until 2026-08-16
(a miscount against a table in the same block, the second of that shape in one
sweep — the notes dossier said "four families" over five rows), was corrected to
six, and is now seven. A count no argument rests on is invisible to every reader,
its author included — which is exactly why it keeps drifting here, three times
now.

| type | resolution | notes |
|---|---|---|
| anonymous | contiguous chunk; offset arithmetic | the ordinary case |
| **code** | *identical to anonymous* | I-42/JIT: two aliases of one region, each installing at **its own** VMA prot |
| MMIO | device PA + offset, device attributes | |
| DMA | every page resolved through `kobj_dma_pa_at` (a weave is a SKEIN of blocks since 2026-09-09; `Burrow.pa` is 0 for a DMA Burrow, deliberately), cacheable | coherent on this platform's transports |
| **HOSTMEM** | PCI BAR PA + offset, **host-dictated** MAIR attr | Warp-6 V-2: a hostmem subrange; `kobj_pci` non-NULL is the liveness guard |
| file-backed | sparse per-slot pages in the pagemap, demand-read | the arm that sleeps; its nodes are uncharged (`as = NULL`); each LEAF it installs is charged to the holder (`addrspace_charge_file`; the round-2 close) |
| lazy-anonymous | allocate + zero + install-once under the lock; the map's nodes charged and allocated by the install outside `v->lock` | no backing read, so no slow path |

**The install attribute widened from a bool to a MAIR index for HOSTMEM (V-2).**
Every arm used to hand `mmu_install_user_pte` a `device_memory` bool — a two-way
Device-vs-Normal-WB choice. HOSTMEM broke that: a hostmem BAR subrange carries a
**create-time host-dictated** attribute (NORMAL_WB for a CACHED mapping,
NORMAL_NC for WC), honoured exactly rather than guessed. So the arms now carry a
`mair_idx` and install through `mmu_install_user_pte_attr`; the other six pass a
fixed index (NORMAL_WB or DEVICE) and are byte-identical to the bool they
replaced.

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

**Closed, at the site, in the code as it stands -- and re-closed differently
by the round-2 audit.** `mmu_install_user_pte` **refuses** a mismatching
install over a valid leaf -- it returns failure rather than overwriting.
**Both break outcomes mismatch**: the copy path changes the physical address,
and take-in-place changes the permission bits. Since a *read* of a COW page
installs a read-only PTE, the first **write after a read** would fail its
install and kill the Proc if nothing dealt with the stale leaf -- which is
exactly what makes the step read as redundant to anyone who does not know the
install primitive's refusal contract.

The first close *uninstalled* the stale leaf before the break, unconditionally.
The round-2 audit (F9) prosecuted that line under the round-1 close's table
reclaim: the clear freed the leaf's (now empty) tables to the pool, and the
install that followed re-allocated them through `pool_charge`, which respects
no lock the faulter holds -- so at the pool's edge a peer's allocation could
take the released pages and the Proc was terminated for writing a page it
already held; and every sparse break paid a table free and an allocation. The
write arm now sets `cow_replace`, and step 5 calls
`mmu_replace_user_pte_attr` instead of the install: a break-before-make on the
LEAF alone (invalid, TLBI, the new entry, TLBI; [[sub-kernel-mmu]]), the
tables untouched, 1 if the leaf is already what is asked (a sibling thread
broke it first), the plain install when nothing is mapped (the first touch
after a fork, whose phase-1 clear took the leaf and kept its table).
`cow.break_sole_holder_takes_in_place` pins the pool and `pgtable_pages`
unchanged across the break.

It is worth noting how the first defect was found. Not by a test -- a test
would have caught it only if some case did read-then-write on a COW page --
but by **reading the contract of the primitive being called**. The same shape
as the through-a-file gate in [[sub-kernel-stalk]], where the answer was
already a field on the object and nobody had asked it. And the second was
found by reading the first fix against a mechanism that landed later: a line
that was load-bearing under one allocator became a hazard under the next.

### Three properties that look like details and are not

**The break's retained share is the model's pin.** The copy path holds its own
share across the allocate and the copy, releasing only once the copy is done.
The model carries a separate `pin` variable; realising it with a held share is
**strictly stronger** — a held share also keeps the count off zero — so this
refines the model rather than deviating from it.

**The parent is modified by the fork, and must be.** Its already-installed
writable PTEs for every COW range are uninstalled, so its next touch re-faults
read-only -- the leaves only: the tables stay linked for the re-faults to find
(`mmu_uninstall_user_range_keep_tables`, the round-2 close; [[sub-kernel-mmu]]). Leaving them is the [[inv-i44]] violation directly: the parent
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

### The copy keeps its share until the leaf is replaced, and a queued fault is answered by the leaf that is there (2026-09-23; B-1a' audit F12 / F13)

Round 3 prosecuted the replace and found the argument it had voided. Under
the first close the write arm cleared the stale leaf BEFORE the decide, so
during a break no thread of the space could reach the page without faulting
and waiting on `as->lock`; the replace moved the only invalidation to the END
of the fault, and the copy branch still put its share of the original right
after the slot swap. In that window the space's own read-only leaf (the read
that preceded the write installed it) still translated to the original, a
sibling thread read through it without faulting, and the other holder -- now
sole -- was free to take the page in place and write into what this space
could still see (I-44), or to exit and free it under a live leaf (I-13). F12
is a regression of the F9 fix, and the fix is the order Linux's
`wp_page_copy` keeps: `cow_release` carries the original past step 5 and the
put (with the free on the last share) runs after the replace, on the
failed-replace path too. A tests-only probe (`g_cow_copy_probe_for_test`,
`KERNEL_TESTS`) fires between the swap and the replace, and
`cow.break_copy_pins_the_original_until_replaced` asserts the control (the
stale leaf still names the original at the probe -- the window is real) and
the pin (the share is still two there).

F13 is older than the replace and lives in the same lines: two threads touch
one page after a fork, the writer wins `as->lock` and its break leaves a
WRITABLE leaf; the reader's fault, decoded while the leaf was invalid, then
runs the read arm, which asks for a READ-ONLY install over it -- the one arm
that narrows -- and `mmu_install_user_pte_attr` refuses the mismatch, so the
Proc was terminated for reading a page it holds. The same shape reaches a
read that faulted inside the replace's break-before-make gap. The answer is
the one Linux gives (the fault re-checks the PTE under the lock): step 2b,
after the prot admission and before any arm, asks `mmu_user_pte_admits(as,
va, write, exec)` ([[sub-kernel-mmu]]) and returns `FAULT_HANDLED` when a
valid leaf already admits the access -- nothing charged, nothing installed,
the instruction retried -- for every arm, so no arm can narrow itself into a
refusal. `cow.read_queued_behind_break_is_handled` drives both break shapes;
`cow.break_read_then_write_copies` and `cow.break_sole_holder_takes_in_place`
each end with the queued read.

### The pager refuses the abort classes it cannot resolve, and the probe fires at the leaf write (2026-09-23; B-1a' audit F17 / F20)

Round 4 prosecuted the pre-check and found a class it answered that no arm
should: an EL0 abort that is neither a translation, an access-flag nor a
permission fault. An ALIGNMENT fault (FSC 0x21 -- an exclusive, an ordered
load or store, or a Device access at an address the instruction cannot take;
ARM ARM D5.10.3, regardless of `SCTLR.A`) or a synchronous EXTERNAL abort
(0x10, 0x14-0x17) is raised on a MAPPED page by the instruction itself.
`fault_info_decode` classified only the three resolvable classes and nothing
consumed a fourth, `arch_fault_handle` dispatched on `from_user` alone, and
the pager answered the fault `FAULT_HANDLED` -- since round 3 through step 2b
(the leaf admits the access), before it through the idempotent install's 1 --
so the ERET re-executed the instruction into the same abort, forever: a
livelock, preemptible and killable only because the return tail delivers
notes, where the Proc owes a `snare:bus` death (Linux's `do_alignment_fault`
/ `do_sea` deliver SIGBUS). Pre-existing since P3-Dc; the pre-check made it
the one site every arm passes through, so it is where the class is refused.
As built: `fault_info` decodes `is_alignment` / `is_external`, and the top of
`userland_demand_page`, before `as->lock` and any lookup, returns
`FAULT_USER_BUS` when none of `is_translation` / `is_access_flag` /
`is_permission` holds -- the class decides, not the page. The kernel-mode
uaccess entry (`arch/arm64/exception.c`) already admitted only the three
classes, so a kernel-side alignment fault through a user pointer still
extincts as the kernel bug it is. Witnesses:
`demand_page.alignment_abort_is_bus_not_handled` (a synthetic 0x21 and a 0x10
on a mapped, admitting page -> `FAULT_USER_BUS`; CONTROL: a translation fault
on the same VA -> `FAULT_HANDLED`) and `/bus-probe-child` from EL0 (joey: a
4-byte load-exclusive from offset 14 of a touched page must die via
`snare:bus`, not hang the boot). The first draft of that child did `ldar` from
`va + 1` and SURVIVED on the Apple core under HVF: FEAT_LSE2 with
`SCTLR_EL1.nAA = 0` (the kernel never sets it) permits a misaligned ORDERED
access inside one 16-byte quantity and faults only across a boundary, while
exclusives keep their natural alignment under every rule -- so the witness is
an `ldxr` at an offset that is both misaligned and crossing.

F20 moved the F12 probe: it fired right after `cow_release` was set, so
"share == 2 at the probe" bounded the put only to "after the probe", and a
put reintroduced between the probe and the replace would have passed. The
probe now fires at step 5, immediately before the replace (nothing runs
between the probe and the leaf write), and two witnesses cover the paths the
first one could not: `cow.break_copy_releases_the_share_on_a_failed_replace`
(a child with no tables under the page and the pool parked to one free page:
the copy's page is served, the install's first table refused,
`FAULT_UNHANDLED_USER`, the share released -- no leak -- the slot holding the
copy, no leaf) and `cow.break_copy_last_share_frees_after_the_replace` (the
probe drains the other holder's mapping, so the copy's own share is the last
and is held through the drain; after the replace the original returns to the
buddy exactly once).

### A file page is the cache's; its mapping is the holder's (2026-09-23; B-1a' audit F8)

A FILE page-in takes a pool page (`alloc_user_pages`) and installs it into the
Burrow's map with `as = NULL` -- the page and the map's nodes are the Image
cache's, shared by every space that maps the file. Until the round-2 close
that was the whole story, and the round-2 audit named what it left: a
toucher's text pages counted against no space (a confined Proc never saw its
own text on its cap), and outlived the toucher as an idle cache entry that
held the physical pool with nothing to reclaim it. The mapping is charged
now, per leaf, like a COW-shared page: every FILE leaf install is preceded by
`addrspace_charge_file(p->as, exempt)` -- the resident-hit fast path through
step 5's `file_charge` flag, and both slow-path install tails -- and the charge
goes back when the install returns 1 (the leaf was already there: a sibling
paid) or refuses; a range clear refunds per leaf it removes
(`vma_uninstall_range_in`, [[sub-kernel-vma]]; the drain refunds nothing, the
space dies). `page_count` is the holder count -- data, file pages, nodes,
tables -- with `tables:` and `file:` beside it ([[sub-kernel-devproc]]). The
pages themselves stay the cache's, which is what the pool's reclaim strips
under pressure ([[sub-kernel-image]]). `demand_page.file_pages_charge_the_holder`
is the witness.

## The permission a fault is checked against can now change (B-1a, 2026-09-23)

Step 2 -- `vma->prot` against the access, before any Burrow is resolved -- did
not change, but the value it reads became mutable: `SYS_BURROW_PROTECT`
([[sub-kernel-burrow]], [[sub-kernel-vma]]) moves a mapping's `prot` among
{none, R, RW} under its mint-time ceiling. Three consequences land here, and
the dispatcher's own code changed for none of them.

**A range at none is a guard, and this dispatcher is what makes it one.** A
touch of a page whose mapping is `none` fails the permission check at step 2
and returns `FAULT_UNHANDLED_USER` -> `snare:segv` with no Burrow consulted --
the path a Burrow-less guard VMA already took. That is the whole of Linux's
`PROT_NONE` guard semantics, and `/protect-guard-child` witnesses it on every
boot: a page sealed at none, written through, dies; joey's expect-fault census
requires the death ([[sub-kernel-protect-witness]]).

**A protect uninstalls the range's PTEs, and the re-fault installs at the NEW
prot.** `burrow_protect_in` clears the leaf PTEs before it changes any `prot`
(the D-3b rule), on a lowering AND on a raise -- `mmu_install_user_pte` refuses
a mismatching install over a valid leaf, so a raise that left the old
read-only leaf in place would have this dispatcher's install fail on the next
write. So every arm above re-runs for a resident page after a protect and
installs at `vma->prot` as it now reads: refused at none, read-only at R,
writable at RW (`protect.pte_uninstalled_then_reinstalled_at_prot`). This is
`cow.tla`'s `Reinstall` action; `BUGGY_PROTECT_KEEPS_PTE` is the shape with the
uninstall skipped, and the `nouninstall` sabotage fails exactly the two
assertions that look through the page table.

**The COW break still keys on the flag and installs from `vma->prot`.** A cut
piece of a forked mapping keeps its COW bit and its ceiling, so a write into
one piece breaks that page alone and the parent's page is untouched
(`protect.cow_split_then_break`); the read arm's `vma->prot & ~WRITE` install
is unaffected. `Fault` in the model is guarded on `prot = "rw"`:
`BUGGY_FAULT_IGNORES_PROT` is a break arm that would fire on a mapping
protected below RW, and `BreakOnlyWhenWritable` is its witness.

The property that made all three possible is the one this dossier already
states: every install passes `vma->prot` through unchanged, and nothing
downstream re-derives a permission.

**The FILE slow path is the one arm whose admission and install span an
unlock, and it re-runs the admission after the sleep (the holotype audit's
F1, P1, fixed in the close).** Step 2 admits a FILE read at the prot of that
moment; the page-in then drops `as->lock` and sleeps on the 9P read; a sibling
thread's `SYS_BURROW_PROTECT` to none (sealed or not) lands in that window --
the precheck admits FILE, and a whole-mapping protect leaves the geometry the
re-lookup verifies exactly as it was. Installing at the CURRENT `vma->prot`
then encoded none as a user-READABLE RO leaf (`make_user_pte_l3` has no
"no access" encoding): a guard that did not guard, with no fault ever running
step 2 again for that page. `file_fault_still_admitted` now re-checks the
recorded fault type against the prot as it reads after the sleep, in BOTH
install paths (each carries its own copy, as each carries the geometry check);
a refusal installs nothing and answers `FAULT_UNHANDLED_USER`, which is what
the retry would answer, and the page-in is KEPT in the slot -- the bytes are
the Burrow's, prot-independent, and the geometry check proved they belong
there. `protect.file_pagein_racing_protect_bails_{single,cluster}` interpose
the protect from inside the stub `dev->read` (the same stand-in the #190
geometry-shift pair uses) and assert no PTE, the slot resident, and a raise
back to R resolving with no second read.

## Error paths

Kernel faults extinct with the faulting address and a message naming the class.
User faults return a result the caller turns into a per-Proc termination with
the matching `snare:*` note. A read failure in the file arm is **fail-closed**:
`FAULT_USER_BUS`, never a silent zero-fill of executable text — filling text
with zeros on an I/O error would execute them.

**A node OOM, a refused node charge, a refused pool allocation or a refused
table charge refuses the fault** (B-1a'): `FAULT_UNHANDLED_USER` from the
ANON_LAZY miss (the data page freed and uncharged first), the two FILE
install paths and the PTE install of every arm -- the same per-Proc OOM
policy as a refused page, never an extinction; a refused table install
leaves the slot resident and charged for the drain, a refused node install
nothing.

**An abort no page install can resolve is `FAULT_USER_BUS` too** (B-1a' round
4, F17): an alignment fault or a synchronous external abort on a mapped page
is refused on its class at the top of `userland_demand_page`, before any
lookup -- answered `FAULT_HANDLED`, the ERET would raise it again forever.

**A fault WHOLLY past the file's last page is `FAULT_USER_BUS`, not a zero-fill
(#194).** Demand-zeroing it would mint real memory the I-32 page axis never sees
— anonymous in effect, accounted as FILE, i.e. not at all (the R-5 uncharged
posture is justified by SHARED FILE BYTES, which a past-EOF page is not). The
refusal comes BEFORE any allocation, so nothing is minted and nothing needs
charging; the file's final PARTIAL page still zero-fills past EOF (the read-short
path), matching Linux. `file_limit` is creation-time (close-to-open); an UNKNOWN
limit — only the immutable baked ramfs — keeps the pre-#194 behaviour. Read-ahead
carries the same bound: the cluster never PRE-READS a neighbour past the limit,
because a past-EOF resident zero page would then be installed by the fast path
with no check — the uncharged mint sneaking back in through read-ahead.

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
failure, the node charge included (a refused pagemap install must leave the
data page uncharged too, and the install itself must reach the buddy only
with `v->lock` dropped); that every page an arm mints comes from
`alloc_user_pages` with the faulting Proc's exemption and is freed through
`free_pages` on every losing path, so the pool never drifts; that read-ahead stays byte-identical to sequential reads and stays
degradable; that no arm invents a permission (they must all pass `vma->prot`);
and that the fail-closed posture on read errors is never relaxed into a
zero-fill.

## Seams

- The header's stale claim that this path is single-threaded and needs a future
  lock — the body documents the fix in detail directly below it (task #60).
- DISTRO D-3 REALIZED what was once a seam here: it maps file-backed Burrows at
  chosen offsets (userspace mmap, MAP_FIXED split, cross-AddrSpace Image share),
  which invalidates the cached slot the slow path carries across its sleep. The
  #190 verify-and-bail (Concurrency, above) closes it in both paths, so the seam
  is discharged, not open.

## Caveats

Instruction aborts report as reads, which is correct — a fetch *is* a read — and
falls out of the encoding rather than being special-cased.

**The WnR write/not-read bit was decoded from the wrong `ISS` position for the
whole life of the fault path, and nothing failed until the COW break needed it
(#137).** `is_write` is `ESR.ISS` bit 6; the decoder read bit 9, which is `EA` —
zero for every normal abort — so `is_write` was **always false**, tree-wide. It
survived because no arm branched on it for *correctness*: demand-zero and the
`FILE` arm install at `vma->prot` whichever way it reads, so first touch works
either way, and a store to genuinely read-only memory is a program bug nobody
ran. The COW break's write arm was the first real consumer, and with the wrong
bit that arm is simply unreachable — the store re-installs read-only, re-faults,
and loops, so the symptom is a **hang with no fault logged**, not anything shaped
like a memory bug. The seam that hid it is the reusable part: the decode's own
unit test **mirrored the constant** — it set bit 9 and asserted the decoder read
bit 9, so it agreed with the *code* instead of the *hardware* and could not have
failed however wrong both were, while every test on the consuming side assigns
`is_write` directly and never runs the decode. The fix pins the test's bit as an
independent literal with an ARM ARM citation, deliberately **not** `#include`d
from the kernel header — sharing the constant would restore the tautology.

The access-flag arm is defensive: this kernel sets the access flag eagerly, so
those faults should not occur. Classifying them anyway means the day one appears
it is named rather than landing in the catch-all.

## Provenance

P3-C built the classifier; P3-Dc added demand paging; P6 #713 added the lock
coverage; REVENANT added the file-backed arm and later its read-ahead; the
overcommit model added the lazy-anonymous arm; I-42 added the code arm.

DISTRO D-3a generalized the file arm to userspace mmap — retiring the R-5 F2
one-fixed-VMA premise, which the #190 verify-and-bail replaces — D-3c added the
#194 past-EOF `FAULT_USER_BUS`, and Warp-6 V-2 added the HOSTMEM arm (the
`device_memory` bool widened to a MAIR index). [[chg-2026-09-06-fault-distro-hostmem]].

[[chg-2026-09-06-fork-doc-absorb]] folds the #137 WnR-decode caveat absorbed from
docs/reference/148: `is_write` was decoded from the wrong `ISS` bit for the life
of the path, unreachable-until-COW, hidden by a constant-mirroring unit test.

## Tests

`demand_page.*` drives the path with synthetic faults and manufactured Procs —
including the read-ahead cluster's per-slot byte map, its boundedness, its
one-batched-read property, an interior short read, and the fail-closed arm. The
production path is exercised by every EL0 first touch on every boot.

B-1a: `protect.pte_uninstalled_then_reinstalled_at_prot`,
`protect.raise_and_write_keeps_contents` and `protect.cow_split_then_break`
(`kernel/test/test_protect.c`) drive `arch_fault_handle` on synthetic faults
after a protect; `/protect-guard-child` is the EL0 witness of the step-2
refusal.

B-1a' (2026-09-23): `capacity.pagemap_nodes_charged_and_reclaimed` drives this
arm's misses on a 1 GiB map and watches the nodes appear in `page_count` as
touched (root + leaf, then a second leaf) and go as decommitted; `test_exec.c`'s
`exec_writable_segment_is_sparse` / `exec_stack_is_sparse` read the charge as
data + exactly the nodes the two maps hold ([[sub-kernel-pagemap]]).

B-1a' round 3 (2026-09-23): `cow.break_copy_pins_the_original_until_replaced`
(the probe between the swap and the replace: the stale leaf as the control,
the share as the claim), `cow.read_queued_behind_break_is_handled` (a read
decoded before a peer's break, run after it, for the copy and the
take-in-place), and the read legs at the end of the two older break tests.

B-1a' round 4 (2026-09-23): `demand_page.alignment_abort_is_bus_not_handled`
(a synthetic alignment fault and a synthetic external abort on a mapped,
admitting page are `FAULT_USER_BUS`; the control is a translation fault on the
same VA, `FAULT_HANDLED`), `cow.break_copy_releases_the_share_on_a_failed_replace`
and `cow.break_copy_last_share_frees_after_the_replace` (the probe at step
5, the two exits the F12 witness could not see); `/bus-probe-child` is the
EL0 witness of the class gate.

## Referenced by

[[moc-kernel-memory]] · [[sub-kernel-vma]] · [[sub-kernel-mmu]] ·
[[sub-kernel-burrow]] · [[inv-i12]] · [[inv-i32]]
