---
id: sub-kernel-mmu
type: sub
parent: moc-kernel-memory
title: "The MMU — page tables, the PTE encoders, and the aliases that keep W^X true"
code: [arch/arm64/mmu.c, arch/arm64/mmu.h]
audit: hard
guarded-by: [inv-i12, inv-i13, inv-i16, inv-i31, inv-i39]
validated-by: [prose, gate-smp]
locks: [lock-vma]
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md", "docs/PORTABILITY.md"]
created: 2026-08-03
updated: 2026-08-16
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
| per-Proc tables | `proc_pgtable_create` / `_destroy`, `mmu_install_user_pte`, `mmu_uninstall_user_pte[_range]` |
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
the kernel never executes user pages. It **extincts** on execute-plus-device,
which is architecturally meaningless — defence in depth beneath the syscall
layer that already rejects it.

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

Per-Proc table *teardown* is safe against a concurrent walk because the Proc is
provably not running anywhere by then — its last thread has exited and the
reaper spun until it was off-CPU — and its leaf mappings were already
invalidated. The freed tables can therefore be recycled immediately.

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

## Error paths

Argument validation returns `-1` without touching anything: null root,
misalignment, an address outside the user half, a malformed table descriptor
found mid-walk. Allocation failure during table growth returns `-1` and the
fault becomes a per-Proc termination.

The extinctions are for conditions that mean an assumption has already failed:
a translation fault while patching text, execute-on-device at the encoder,
vmalloc exhaustion, and the allocation failure during boot page-mapping — that
last one extincts because the invariant it establishes cannot be established
later.

Unmapping is idempotent by design: an already-clear entry, or a missing
sub-table, both return success. There is nothing to undo.

## Performance

The unmap loop invalidates per page and waits each time — microseconds per page,
against ranges bounded at 256 MiB and typically a few tens of kilobytes. The
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

## Tests

`demand_page.*` covers install, its rejections, and idempotence.
`alternatives.*` covers the patcher (every patchable site applied, and the
patched instructions computing correctly). The guard pages and the direct map
are proven by boot: a stack overflow is caught and named, and the boot
page-map's diagnostic accessor is asserted. The multi-boot SMP gate is the
durable witness for the lock-free leaf-flip argument.

## Referenced by

[[moc-kernel-memory]] · [[sub-kernel-vma]] · [[sub-kernel-fault]] ·
[[sub-kernel-asid]] · [[sub-kernel-mm-phys]] · [[inv-i12]] · [[inv-i13]] ·
[[inv-i16]] · [[inv-i31]] · [[inv-i39]]
