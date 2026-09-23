---
id: sub-kernel-mmu
type: sub
parent: moc-kernel-memory
title: "The MMU — page tables, the PTE encoders, and the aliases that keep W^X true"
code: [arch/arm64/mmu.c, arch/arm64/mmu.h]
audit: hard
guarded-by: [inv-i12, inv-i13, inv-i16, inv-i31, inv-i32, inv-i39]
validated-by: [prose, gate-smp]
locks: [lock-vma]
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md", "docs/PORTABILITY.md"]
created: 2026-08-03
updated: 2026-09-23
---
## Purpose

Everything that turns an address into a physical location. The boot-time table
construction, the permission encoding that [[inv-i12]] is made of, the kernel's
own three views of memory, the per-Proc user tables, and the two places the
kernel deliberately holds a second mapping of a page it already has.

The largest file in the memory area and the one with the most invariants
sitting on it, because it is where policy stops being a rule and becomes bits.

## Contract

Grouped by what they are for, since the file is long:

| group | entry points |
|---|---|
| bring-up | `mmu_enable`, `mmu_program_this_cpu`, `mmu_retire_ttbr0_identity` |
| kernel views | `mmu_map_mmio`, `pa_to_kva` / `kva_to_pa` (in `page.h`) |
| guard pages | `mmu_set_no_access[_range]`, `mmu_restore_normal[_range]`, `mmu_pagemap_directmap` |
| per-Proc tables | `proc_pgtable_create` / `_destroy`, `mmu_install_user_pte[_attr](as, exempt, ..)` (1 on an identical leaf), `mmu_replace_user_pte_attr` (the COW break's in-place leaf replace), `mmu_user_pte_admits` (the fault path's resolved-by-a-peer probe), `mmu_uninstall_user_pte(as, ..)`, `mmu_uninstall_user_range(as, ..)` (returns the leaves cleared) and `_keep_tables` (the fork's clear) -- the tables charged and reclaimed since B-1a' (below) |
| self-modification | `mmu_patch_text`, `arch_icache_sync_range` |
| cross-Proc | `mmu_cross_proc_read` / `_write` |
| W^X | `pte_violates_wxe` — **and see Caveats: it has no callers** |

## Mechanism

**Three kernel views of memory, one user view.**

`TTBR1` (high addresses) carries the kernel: the image at a KASLR-slid address,
a **direct map** giving every physical page a fixed kernel address by linear
offset, and a **vmalloc** range where device registers get page-grain mappings
discovered from the device tree. `TTBR0` (low addresses) carries whichever
Proc is running.

The boot identity map is **retired** once nothing needs it — the serial port has
moved to vmalloc, the device tree to a direct-map buffer, the stacks to the
direct map. Retiring it means a stray "use a physical address as a virtual one"
bug faults loudly instead of quietly working. The root tables stay valid with
their second level emptied, so the register never points at physical zero.

**The permission encoding is the invariant.** Composite constants —
`PTE_KERN_TEXT` (read-only, executable at EL1), `PTE_KERN_RO` and `PTE_KERN_RW`
(both execute-never) — make the forbidden combination unconstructible, and
seven `_Static_assert`s pin the bits so a refactor that made kernel text
writable fails the build rather than the boot.

`make_user_pte_l3` derives user permissions from a VMA's prot: writable →
read-write for both levels, otherwise read-only; executable → user-execute
allowed, otherwise not. The kernel-execute bit is set unconditionally, because
the kernel never executes user pages. The cacheability attribute is a MAIR index
the caller passes directly — `NORMAL_WB` for cacheable RAM (the anon/code/DMA
default), `DEVICE` (nGnRnE) for MMIO registers, and, since V-2, `NORMAL_NC`
(write-combining) for host-visible shared memory. Two encoder-level guards sit
beneath the syscall layer as defence in depth: the index must name a defined
MAIR byte, so an out-of-range value **extincts** rather than selecting an
unimplemented attribute or overflowing the field; and — the W^X half — an
executable mapping **extincts** unless its index is `NORMAL_WB`. That last guard
was widened at V-2 from *reject execute-on-device* to *confine execute to
cacheable RAM*, which also rejects execute on the new write-combining index: an
executable page is only ever legitimate on Normal-WB, so this is [[inv-i12]] one
level below the VMA gate that already forbids W^X.

The install entry keeps its original boolean device-flag form as a thin wrapper
over the index-aware `mmu_install_user_pte_attr`, so the boolean→index mapping
lives in exactly one place — a bare `false` and `MAIR_IDX_DEVICE` share the bit
pattern 0, and letting every caller supply a raw index would invite exactly that
fat-finger.

**Two deliberate aliases**, and they are the same idea used twice:

*Self-modification.* The boot-time instruction patcher writes kernel `.text`
through a scratch mapping that is read-write and execute-never, while the
canonical mapping stays read-only and executable. Two virtual addresses, one
physical page, neither PTE ever both writable and executable — not even
momentarily. It resolves the physical address by *asking the hardware to
translate* rather than assuming the image's load offset, so it is correct under
any KASLR slide. Then instruction-cache maintenance: clean the data side at the
scratch address, invalidate the instruction side at the canonical one — the same
physical line, since the caches are physically tagged.

*Cross-Proc debug access.* Walk a target's tables through the direct map to
read or write its memory without a fault-in. The write refuses a read-only leaf,
so a debugger writes data and never text.

**The guard-page path and the race it does not have.** Making a kernel stack's
lower pages inaccessible requires demoting a large block mapping down to page
granularity — a break-before-make sequence, and doing that at runtime while
another CPU walks the same tables is the kind of race that produces impossible
symptoms. It is closed **by construction**: at boot, the entire allocator zone
is pre-demoted to page granularity in one single-CPU pass. Every runtime call
then finds page-grain tables already present and does nothing but flip one leaf
entry. Concurrent flips of distinct entries are single-copy-atomic and each
carries its own broadcast invalidate, so no lock is needed and none exists.

That is worth stating as a pattern: **the race was removed by making the
dangerous transition impossible at runtime, not by locking it.**

**Unmapping and the invalidate.** Clearing a user PTE is not enough; the stale
translation must be flushed. `mmu_uninstall_user_pte` clears the leaf, then
broadcasts an invalidate by address and waits for it. Skipping this was a real
bug: detached memory kept working through cached translations, and when the
allocator returned a *different* physical page for the same address on
re-attach, writes went to the recycled page — content-dependent corruption that
only fired under particular allocation patterns.

## Data structures

Static tables in BSS for the kernel's own maps: two roots, a level-1 direct map
of block descriptors, and the page-grain tables covering the image and the
vmalloc range. Per-Proc trees are allocated from the buddy, zeroed, and freed by
a recursive walk that frees **only translation-table pages** — leaf pages belong
to the VMA layer and are freed through the Burrow's refcounts.

Every page-table page is zeroed before it returns to the allocator: a freed
level-3 table is full of physical addresses of user pages, and a speculative
walker reaching a recycled page should find zeros.

## Concurrency

The kernel tables are built once, single-CPU, before secondaries start; they are
read-mostly for the rest of the boot. The runtime mutation paths are the leaf
flips described above, which need no lock by construction, and the per-Proc
paths, which are serialized by the caller's address-space lock ([[lock-vma]];
[[sub-kernel-fault]] holds it across the whole resolve-and-install).

### How other CPUs see tables this one built — and a safety argument that was fiction

The primary builds and mutates the tables through **cacheable** mappings, and
there is **no cache clean anywhere** on that path. That is sound for every
walker, and the reason is a translation-control setting rather than any
maintenance: the walks themselves are configured cacheable and inner-shareable,
so a secondary's table walker **participates in coherency** and observes the
primary's dirty lines directly.

This dossier previously said nothing about that, which is worth admitting
plainly: it was not wrong here, it was silent, and **an omission that happens to
avoid an error is not a correct treatment of the topic.**

The account that *was* written down, in the source, was **false twice over**. It
claimed the table builder cleans the tables to the point of coherency, and named
the instruction it used to do so. No such clean exists — and the named
instruction operates to the point of *unification*, not coherency, so it would
not have accomplished what was claimed even if it had been there.

Nothing was ever broken, because the coherent walker never needed it.

**That is the whole lesson: a safety argument can be entirely fictional while the
system is correct, which proves the argument was never what made it correct.** A
reader auditing multi-processor coherence would have found that comment, believed
the tables were cleaned, and reasoned from a false premise to a true conclusion —
with nothing available to signal the gap, because the conclusion checks out. The
comment survived precisely *because* it was describing something inessential; a
fictional account of a load-bearing mechanism would have been falsified by the
first failure.

The genuinely non-coherent accesses on this path are elsewhere and are handled
explicitly: the secondary's own writes before its translation is enabled, which
bypass the caches and are confined to a documented mailbox protocol
([[sub-kernel-boot-entry]]).

The patcher runs single-CPU with interrupts fully masked, before any secondary
exists. Full masking rather than interrupt-only is deliberate: it also closes
the window between asking the hardware for a translation and reading the result.

Per-Proc table *teardown at death* is safe against a concurrent walk because
the Proc is provably not running anywhere by then — its last thread has exited
and the reaper spun until it was off-CPU — and its leaf mappings were already
invalidated. The freed tables can therefore be recycled immediately. A table
reclaimed while the space is LIVE (B-1a' audit F1, below) has no such argument
available -- a sibling thread may be running on another CPU -- so it is a
break-before-make on the table descriptor: the parent entry written invalid,
`dsb ishst`, `tlbi vaae1is` on a VA under it, `dsb ish`, and only then the
page freed; the software walkers of a user tree (the debugger's
`mmu_cross_proc_*`, the clone, the drain) all run under the space's lock,
which the reclaim holds.

## Invariants enforced

[[inv-i12]] — the composites, the seven asserts, the user encoder, and both
transient aliases. The **gate** is not here: it is `vma_alloc`'s rejection
([[sub-kernel-vma]]). This file makes the invariant *representable* and
unbreakable-by-refactor; one `if` a layer up makes it *true*.

[[inv-i13]] — the address-space split itself. Kernel and user live in different
translation roots.

[[inv-i16]] — the image is mapped at a slid address; the slide is what the
direct map's translation helpers and the patcher's hardware-translate both have
to be correct under.

[[inv-i31]] — the rolling ASID model's counterpart here: the invalidate on
unmap is all-ASID at the tightest address scope, which is why the ASID argument
threaded through these functions is vestigial and documented as such.

[[inv-i39]] — cross-Proc read and write, confined to the target's own tables,
never faulting anything in, and refusing to write a read-only page.

**I-42** — the JIT's outward generalization of the self-modification alias
(the [[sub-kernel-burrow]] `BURROW_TYPE_CODE` dual mapping), and its two I-cache
obligations both ride this file's primitive, `arch_icache_sync_range`. *At
create*: a code Burrow's backing is a recycled anon page, and zeroing it — which
makes an un-emitted page decode as `UDF #0` rather than run residue — does **not**
touch the instruction cache, and nothing on the free path does either (unmap
broadcasts a TLBI, a *TLB* operation; `free_pages` performs no cache maintenance).
So a fresh code region could carry a previous region's I-cache lines and execute
bytes the Proc never emitted; `sys_jit_create_region` therefore invalidates the
I-cache over the fresh pages before any RX PTE can name them (the CL-7k-3 F1
finding — the code Burrow had been the sole executable backing in the tree that
skipped this, where `exec.c`'s eager paths and `fault.c`'s FILE demand-page arm
already did it). *At publish* (`SYS_ICACHE_SYNC`): the maintenance runs on the
**direct map**, never the user VA — `dc cvau`/`ic ivau` can take translation
faults and a user VA is exactly what a caller can arrange to be unmapped, and
`IC IVAU` is architecturally PIPT-exact across every alias of the PA. The cache
half is genuinely cross-PE (`IC IVAU` is Inner-Shareable *broadcast*); the
trailing `ISB` is **not** — it retires prefetch on the *calling* PE only, so a
peer PE that already executed at those exec-alias addresses must take a
context-synchronization event (any syscall or exception return is one) before
branching in. Nothing at v1.0 drives the exposed spin-wait case, but an
emit-on-one-thread / execute-on-a-worker mapper (ORC's `DualMapMemoryMapper`)
must honor the contract or make the syscall broadcast an ISB.

## Error paths

Argument validation returns `-1` without touching anything: null root,
misalignment, an address outside the user half, a malformed table descriptor
found mid-walk. Allocation failure during table growth returns `-1` and the
fault becomes a per-Proc termination.

The extinctions are for conditions that mean an assumption has already failed:
a translation fault while patching text, an executable mapping on a non-cacheable
index or an out-of-range MAIR index at the encoder, vmalloc exhaustion, and the
allocation failure during boot page-mapping — that last one extincts because the
invariant it establishes cannot be established later.

Unmapping is idempotent by design: an already-clear entry, or a missing
sub-table, both return success. There is nothing to undo.

## Performance

The unmap loop invalidates per page and waits each time — microseconds per page,
against ranges that may be a whole reservation since B-1a' (up to the burrow
window; the subtree walk below keeps the cost to the pages present) and are
typically a few tens of kilobytes. The
batching optimization is noted in the code and deliberately not taken: the
load-bearing property is that no stale translation is observable when the call
returns, and that holds either way.

## Prosecution

On any change here: that no PTE constructor can produce writable-and-executable
(add an assert with each new composite — three of the seven currently have
none); that the patcher's scratch stays execute-never and the canonical stays
read-only, with instruction-cache maintenance between the write and any fetch;
that the boot pre-demote still covers the whole allocator zone, since the
lock-free guard path depends on it entirely; that every user-PTE clear keeps its
broadcast invalidate; that table teardown continues to free only table pages and
to zero them; and that cross-Proc write keeps refusing read-only leaves.

Two more, both about arguments rather than code:

- **Table-walk coherence rests on the translation-control settings, not on cache
  maintenance.** Nothing cleans the tables and nothing needs to. A change that
  made the walks non-cacheable or non-shareable would silently require the
  maintenance that has never existed, and the failure would appear on a secondary
  as a walk of stale table memory.
- **A safety comment describing maintenance that is not performed is worse than
  no comment**, and this file carried one for a long time. When a mechanism is
  cited as the reason something is safe, check that it exists and that it does
  what its name says — the previous claim failed both tests and survived because
  the property it purported to guarantee was guaranteed by something else.

## Seams

- Three of the seven W^X-bearing composites carry no `_Static_assert` — the
  block-form kernel mappings and the page-grain device mapping. All are correct
  today; none is pinned. The device one is used for every driver's registers.
- The direct map reaches 8 GiB. More physical memory than that needs the map
  extended first; the allocator is capped to match, and the cap is enforced
  where the coupling lives rather than assumed.
- Batched invalidation for large unmaps.

## Caveats

**`pte_violates_wxe` has no callers.** It is a correct W^X predicate that
nothing invokes — not the install path, not a test, not a tool. Five documents
name it as enforcement, including the invariant table in the architecture
scripture and an error-path table describing what "callers" do. An audit round
once fixed a real bug *inside* it (it checked only the kernel-execute bit, so a
writable user-executable page read as clean) and recorded "dormant; 0 callers"
as a parenthetical without asking why. Full chain in [[inv-i12]]; task #59.

**The ASID argument is vestigial** on the install and uninstall paths and is
documented as such at each — the invalidates are all-ASID. It is retained for a
future address-targeted optimization.

**Three headers in this subsystem still describe `Proc.vma_lock` as future
work.** It has existed since #713 and is taken at 116 sites. Task #60.

## The range uninstall walks by subtree (2026-09-23; B-1a audit F2)

`mmu_uninstall_user_range` used to call `mmu_uninstall_user_pte` once per
page of the range, so a range with nothing under it still paid a four-level
table miss per page -- and B-1a's `SYS_BURROW_PROTECT` became the first caller
with no length cap, deliberately: a whole-region protect over a 4 GiB engine
reservation is the producer the chunk exists for, and the burrow window is
64 TiB. The range form now descends the tree once per subtree: an absent (or
malformed) L0 / L1 / L2 entry skips 512 GiB / 1 GiB / 2 MiB at once
(`uninstall_next_block`, clamped to the range end; `v < 2^47` and the shift is
at most 39, so the arithmetic cannot wrap), and only inside a PRESENT L3 table
does each page go through `mmu_uninstall_user_pte`. So every valid leaf is
cleared with exactly the TLBI + DSB ISH it always got -- the load-bearing
invariant (no stale cached translation observable when the call returns) is
untouched, only the no-op iterations are gone -- and the per-leaf cost is
bounded by the pages the address space has faulted in, which I-32 bounds. A
malformed intermediate entry is skipped like an absent one; the per-page walk
answered -1 for it and the range loop ignored that, so nothing changed there
either. `mmu_uninstall_pte_calls()` counts the per-page calls and
`protect.uninstall_range_skips_absent_subtrees` pins the walk: a GiB with one
resident page costs exactly 512 calls (its present 2 MiB table), an empty GiB
zero, where the per-page loop cost 262144 each.

## User page tables are charged and reclaimed (2026-09-23; B-1a' audit F1)

Until the B-1a' close every L1 / L2 / L3 table under a user L0 was allocated
uncharged by `mmu_install_user_pte`'s walk-and-grow and freed only by
`proc_pgtable_destroy` at death, on the argument that deciding emptiness meant
scanning 512 slots and the pages went free at death anyway. The round-1 audit
named the hole: with the reservation cap lifted to the window, an unprivileged
Proc that reserves 64 TiB, touches one page per 2 MiB and decommits it leaves
one uncharged, unreclaimable table page per iteration -- the buddy empty at a
charged count of three, the reserve a fiction (and without any decommit the
tables allocate 1:1 with charged pages, emptying the buddy at charged
~RAM/2). The tables predate the chunk; the chunk removed the last incidental
cost of the attack, and its scripture claim depended on them.

As built: `user_table_alloc(as, exempt)` charges the space
(`addrspace_charge_table`: `page_count` + the `pgtable_pages` telemetry,
under the I-32 cap) and takes the page from the user pool
(`alloc_user_pages(0, KP_ZERO, exempt)`, [[sub-kernel-mm-phys]]) BEFORE the
table is linked, exactly as a pagemap node is; a refused charge or an empty
buddy installs nothing, and a table this call linked but could not fill goes
back (`mmu_install_user_pte_attr` calls `user_tables_reclaim` on its -1 path,
which frees every table on the path whose occupancy is 0 -- only the ones
this call linked can be). The install's signature is `(as, exempt, vaddr,
pa, prot, ..)` and the uninstalls' `(as, ..)`; every caller passes
`proc_resource_exempt(p)` or its creator's `exempt` ([[sub-kernel-fault]],
[[sub-kernel-addrspace]], [[sub-kernel-vma]], [[sub-kernel-burrow]]).

**Occupancy lives in the table page's own `page->refcount`**, ESTABLISHED at
0 when the table is allocated (the buddy hands the head page over holding 1)
and never inherited -- the pagemap's rule and `page.h`'s `cow_share`
contract: an L3 counts its valid leaves, an L2 its L3 tables, an L1 its L2
tables; the L0 is uncounted and never reclaimed here (one page per address
space, bounded by the Proc axes like a kernel stack, freed by
`proc_pgtable_destroy`). The install increments the parent when it links a
new table and the L3 when it writes a leaf over an invalid entry; an
idempotent re-install (`existing == want`) changes nothing and returns 1
(since the round-2 close: a caller that charged for the leaf refunds it); a
mismatching valid leaf is -1 with nothing to unwind -- the one legitimate
mismatch, the copy-on-write break, has `mmu_replace_user_pte_attr` (below),
a third leaf writer that changes no count. Since round 3 the demand-page
path never asks for a mismatch: `mmu_user_pte_admits(as, va, write, exec)`
reads the leaf without growing (a VALID leaf with EL0 access, AP[2] clear for
a write, UXN clear for an instruction fetch) and a leaf that admits the
access answers the fault before any arm runs (B-1a' audit F13 -- the read
arm's read-only install over the writable leaf a sibling's break had left was
exactly such a mismatch, and it terminated the Proc; [[sub-kernel-fault]]). Each uninstall
(`mmu_uninstall_user_pte`; the range form per page inside a present L3) drops
the L3's count for each valid leaf it clears (`table_occupancy_drop`: 0 -> -1
is a miscount and extincts) and, when the L3 reaches 0, calls
`user_tables_reclaim(as, va)` -- leaf upward, stopping at the first occupied
table, since its ancestors are then occupied too.

**`user_table_free` is a break-before-make on a TABLE descriptor**: the
parent entry is written invalid, `dsb ishst` orders that write before the
invalidate, `tlbi vaae1is` on one VA under the table drops any
translation-walk-cache entry holding the old descriptor (a by-VA invalidate
reaches every cached entry that could translate that VA, the intermediate
levels included, ARM ARM D5.10.1 -- and every leaf beneath was invalidated the
same way as it was cleared, so nothing under the table is cached), `dsb ish`
waits for every CPU, then the 512 entries are RE-READ and a live one extincts
(a table freed with a valid entry would be a translation walk through
recycled memory: the count is load-bearing and the scan is its witness, at
one page read per table freed), then `free_pages` (the pool takes the page
back) and `addrspace_uncharge_table`. Serialisation is the caller's: every
install, uninstall and reclaim of a tree runs under that address space's lock
(the fault path, the detach, the decommit, the protect, the clone's phase 1),
so the counts are plain increments and a parent-entry write never races a
peer's walk-and-grow.

What it costs and what it buys: a first touch in a fresh region charges its
data page and three tables (the tests' `pages_of(as) = page_count -
pgtable_pages` is the data view); a protect to none or a decommit reclaims
them, down to the L0 entry (a clone's phase-1 clear keeps them, below); a
sole-holder COW break used to churn the table of a page alone in its L3
(uninstall-then-install), and a fork used to reclaim every table under every
COW range -- both gone since the round-2 close (below). `/proc/<pid>/status`
reports `tables:` beside `pages:` ([[sub-kernel-devproc]]). Witnesses:
`capacity.page_tables_charged_and_reclaimed` (the path grows and shrinks
table by table, the freed page back in the buddy, the range form reclaiming
as the single form does), `capacity.memory_bomb_leaves_the_reserve` (the
attack above refused within one touch's cost of the pool's room, the
physical footprint bounded by the room, a decommit returning everything, the
second round refused at the same point), and the REDs `notablecharge` /
`notablereclaim`, each reddening both.

## The COW break replaces its leaf in place, and a fork keeps the parent's tables (2026-09-23; B-1a' audit F9)

The copy-on-write write arm used to clear the leaf before deciding the break
-- `mmu_uninstall_user_pte` unconditionally, because the install refuses a
mismatching valid leaf and both break outcomes mismatch (the copy changes the
PA, the take-in-place the permission bits). With tables reclaimed on the
clear, that line freed the leaf's L3 (and any emptied ancestor) to the pool,
and the install that followed re-allocated the path through `pool_charge`,
which respects no lock the faulter holds: at the pool's edge a peer's
allocation takes the released pages and the Proc is terminated for writing a
page it already holds -- SMP-only, non-deterministic, the round-2 audit's F9.
And every sparse break paid a table free (TLBI + the verify scan) and an
allocation; `addrspace_clone`'s phase 1 reclaimed every table under every COW
range, so a forked parent rebuilt all of them one fault at a time.

**`mmu_replace_user_pte_attr(as, exempt, va, pa, prot, mair)`** is the break's
install now (the write arm sets `cow_replace`; [[sub-kernel-fault]]). It walks
without growing; anything short of a valid leaf -- an absent level, an invalid
entry -- is the plain install (which grows and may refuse as before); an
identical leaf is 1; otherwise it is a break-before-make on the LEAF alone:
the entry written invalid, `dsb ishst`, `tlbi vaae1is` on the VA, `dsb ish`,
`isb`, the new entry written, the same drain and invalidate again (ARM ARM
D5.10.1: changing a valid descriptor's output address or permissions needs an
invalid write and a TLBI between the two valid states, or two translations
for one VA can coexist in a TLB). One valid leaf became one valid leaf, so the
table's occupancy is untouched and nothing is freed or allocated; a peer
thread's write between the two valid states faults and waits on `as->lock`,
which the faulter holds; a peer thread's READ between the two valid states,
or one queued behind the break, is answered by `mmu_user_pte_admits` (round
3's F13, above). `cow.break_sole_holder_takes_in_place` pins it: the pool
AND `pgtable_pages` unchanged across the break -- and, since round 3, both
break tests pin the L3 table's PA and `mmu_uninstall_pte_calls()` unchanged
across it, because a free-then-realloc of the table leaves every count equal
(round 3's F16: the count-only witness was satisfied by the defect it named).

**`mmu_uninstall_user_range_keep_tables`** is the same range clear without the
reclaim (a static `uninstall_range(as, lo, hi, reclaim)` sits behind both public
forms, which now return the number of valid leaves cleared -- what a FILE
mapping's holder is refunded, [[sub-kernel-vma]]). The clone's phase 1 uses it:
every leaf it takes the parent re-faults, so the emptied tables stay linked --
charged, at occupancy 0 -- for the re-installs to find, as Linux keeps the
parent's tables across fork. An empty linked table is a state the install
already tolerated (it fills it: 0 -> 1) and the reclaim already handled (a
range clear over it finds nothing valid and frees it once its block is done;
death frees it with the rest); its parents' counts are untouched because
nothing was unlinked, so a kept L3 keeps its L2 occupied. What it costs: the
parent's `pgtable_pages` (and `tables:`) over-report by the kept tables until
its next range clear or its death -- telemetry, no policy reads it -- and a
range clear visits the 512 slots of a kept table exactly as it visits any
present table (the B-1a F2 bound is per PRESENT table and unchanged).
Witnesses: `capacity.fork_clone_charges_pages_and_nodes` and
`capacity.fork_costs_the_pool_only_its_nodes` (the parent's six and three
tables stay across the fork; the pool pays the child only its node mirror),
`protect.uninstall_range_skips_absent_subtrees` (the range form answers 1 for
its one leaf).

## Provenance

P1-C built the tables and the W^X encoding; P1-H added the branch-target
guarding on kernel text; P3-Bb added the direct map and vmalloc; P3-Bca the
guard pages; P3-Bda the identity retirement; P3-Bcb/Db the per-Proc trees;
P3-Dc the user install; P6 hardening #2 the unmap-plus-invalidate that closed
the corruption bug; #808 the boot pre-demote; W1.5 the patcher; the debug
surface added cross-Proc access; I-42 the JIT's outward generalization of the
alias trick.

Re-read 2026-08-16: the real-silicon bring-up corrected a table-walk coherence
claim that named a maintenance operation the tree does not perform.
[[chg-2026-08-16-mmu-fictional-clean]].

Re-read 2026-09-06 for Warp-6 V-2 (`7973f8dc`): `make_user_pte_l3` took a MAIR
index in place of the device bool (adding `NORMAL_NC` write-combining for
host-visible shared memory), its W^X extinction widened from execute-on-device
to execute-unless-`NORMAL_WB`, a MAIR-range extinction was added, and the bool
install API became a wrapper over the index-aware entry.
[[chg-2026-09-06-mmu-warp-v2-attr-index]].

## Tests

`demand_page.*` covers install, its rejections, and idempotence (its charge
figures read the data view, `page_count - pgtable_pages`, since B-1a');
`capacity.page_tables_charged_and_reclaimed` and
`capacity.memory_bomb_leaves_the_reserve` the table charge and reclaim.
`alternatives.*` covers the patcher (every patchable site applied, and the
patched instructions computing correctly). The guard pages and the direct map
are proven by boot: a stack overflow is caught and named, and the boot
page-map's diagnostic accessor is asserted. The multi-boot SMP gate is the
durable witness for the lock-free leaf-flip argument.

## Referenced by

[[moc-kernel-memory]] · [[sub-kernel-vma]] · [[sub-kernel-fault]] ·
[[sub-kernel-asid]] · [[sub-kernel-mm-phys]] · [[inv-i12]] · [[inv-i13]] ·
[[inv-i16]] · [[inv-i31]] · [[inv-i39]]
