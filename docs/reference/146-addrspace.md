# 146 — `struct AddrSpace` (LINEAGE L-1)

**Status**: as-built at L-1. Scripture: `docs/LINEAGE.md` §5.1; invariant
`ARCHITECTURE.md` §28 **I-44** (RESERVED — enforced at L-4/L-5); audit-trigger
row `ARCHITECTURE.md` §25.4.

**Files**: `kernel/include/thylacine/addrspace.h`, `kernel/addrspace.c`.

---

## Purpose

The address space, extracted from `struct Proc` into a refcounted object so that
*two Procs can share one address space* — the shape both `rfork(RFPROC|RFMEM)`
(L-3) and copy-on-write `fork` (L-5) require, and which was unrepresentable
while these were inline fields.

That is why the extraction is the LINEAGE arc's **stage 0** and not an
implementation detail of COW: the two features block on the same object.

At L-1 nothing shares. Every `AddrSpace` in the tree has `ref == 1`, allocated
by `proc_alloc` and dropped by `proc_free`. L-1's whole gate is *byte-for-byte
behavioural identity*.

---

## What lives here, and why each field

The membership test is **"does this describe the TRANSLATION, or the PROCESS?"**

| Field | Was | Why it belongs to the address space |
|---|---|---|
| `pgtable_root` | `Proc.pgtable_root` | the L0 table *is* the address space |
| `context_id` | `Proc.context_id` | the rolling ASID (I-31) names a TRANSLATION TABLE, which is what the allocator always semantically meant |
| `vmas` | `Proc.vmas` | the mapping list — shared by definition |
| `lock` | `Proc.vma_lock` | serializes the shared VMA list, so it is the AS's lock, not any one sharer's |
| `page_count` | `Proc.page_count` | I-32 RSS axis: sharing an AS means sharing the pages, so one charge is the honest count |
| `vma_count` | `Proc.vma_count` | I-32 VMA-slab axis, same argument |
| `shared_map_pages` | `Proc.shared_map_pages` | I-32 cross-Proc shared-in axis (G-2) |

**Seven fields, not the six the L-0 scripture listed.** `shared_map_pages` was
added at L-1 by exactly the argument that moves `page_count`: `vma.h` states its
invariant as `shared_map_pages == Σ pages of SHARED_IN VMAs`, and both the charge
and the uncharge run off the VMA list — which lives here. Two Procs sharing an
address space would otherwise keep divergent counts for one VMA set, and the
uncharge would not know whose counter to decrement.

Deliberately **not** here: `bounce_bytes` (CF-3 syscall staging — a per-caller
budget), and everything else in `struct Proc` — identity, handles, territory,
notes, the process tree. Those describe the *process*.

Two consequences were decided rather than emergent (LINEAGE §5.1):

- **`context_id` belongs to the AddrSpace.** Two Procs sharing an AS share one
  ASID: correct, and cheaper than two. The inverse — two tables, one ASID — is
  the I-31 corruption the ASID arc exists to prevent, and holding the field here
  makes it structurally unrepresentable. **I-31 is unaffected in substance**;
  `specs/asid.tla` re-runs unchanged as the gate (verified clean at L-1: 443457
  distinct states; see the note on the depth figure at the end of this file).
- **The I-32 charge follows the AddrSpace.** The per-Proc cap becomes a per-AS
  cap. A fork bomb is still bounded — N children means N address spaces, each
  capped — and a COW break (L-4) will charge *the breaker*, which is where the
  new page actually lands.

---

## `as == NULL` means kernel-only

kproc is built by `proc_init` via a direct `KP_ZERO` `kmem_cache_alloc` and never
calls `proc_alloc`, so its `as` stays NULL for free. This pointer **is** the old
`pgtable_root == 0` test.

The mmu API is unaffected and deliberately so: `mmu_install_user_pte` and friends
keep taking a bare `paddr_t pgtable_root`, because that layer has no business
knowing what a Proc is. Their own `pgtable_root == 0` rejects are *parameter
validation*, not the kernel-Proc test, and did not convert. (Of the 13 measured
`pgtable_root == 0` sites, **nine** converted; those four did not.)

---

## Public API

```c
struct AddrSpace *addrspace_alloc(void);   // ref 1; NULL on OOM, nothing leaked
void addrspace_ref(struct AddrSpace *as);  // extinction on a dead object
void addrspace_unref(struct AddrSpace *as);// NULL-safe; last drop frees
```

`addrspace_unref`'s last drop **drains the VMA list**, destroys the page table
and frees the struct.

**The drain lives here as of L-3, and did not at L-1.** It used to run in the
callers (`proc_free`, and `proc_exec_replace` on the outgoing space), which was
correct only while `ref` could never exceed 1: draining *at a Proc's death* and
draining *at the last reference* are the same event exactly when there is one
reference. Under `RFMEM` they come apart, and the old placement would have had
the first sharer to die free a VMA list the survivor was still translating
through. Both callers had the bug latent; moving the drain fixes them together,
at the layer where the list actually belongs.

**Precondition on the last drop**: no CPU can still translate through this
table. Each caller owes that by its own argument — `proc_free` by having reaped
and `on_cpu`-spun every thread; `proc_exec_replace` by writing the new TTBR0 (a
*different* ASID) and `isb`-ing first.

No TLB flush at teardown and no per-AS ASID free — the Linux model. Note what
carries this, because an earlier version of this section said otherwise: the
drain issues **no TLB maintenance at all** (measured at L-2 — it frees `Vma`
structs and drops Burrow mapping refs, nothing more). Stale leaf entries are
harmless rather than absent, because every user PTE is non-global (`PTE_NG`) and
so is reachable only under this address space's own ASID, whose reuse is gated
by the rollover's per-CPU `flush_pending` local flush (ARCH §6.2.1).

---

## Lifecycle

- **Create**: `proc_alloc` calls `addrspace_alloc` where it used to call
  `proc_pgtable_create`. On OOM the rollback is the same `ZOMBIE + proc_free` as
  every other alloc step; `proc_free`'s `addrspace_unref(NULL)` is a clean no-op.
- **Share** (L-3): `proc_alloc_in(as)` takes a reference to an existing space
  instead of allocating one — `proc_alloc()` is now `proc_alloc_in(NULL)`. This
  is the whole of what `rfork(RFPROC|RFMEM)` does to the address space, and it
  is the `_in` shape L-2a used for the VMA primitives, for the same reason: the
  callers that want a fresh space stay untouched and the one that does not says
  so explicitly.
- **Release**: `proc_free` calls `addrspace_unref` where it used to call
  `vma_drain` — before `handle_table_free`, preserving the ordering that comment
  reasons about (each `Vma` carries a `burrow_unmap`, and a Burrow with
  `mapping_count > 0` must not free even when `handle_count` hits 0). The page
  table destroy therefore now happens earlier than it did; nothing between the
  two positions touches `p->as`, and the #847 dual refcount makes the Burrow
  free order-independent either way.

**The ASID needs nothing.** `asid_resolve` keys on `as->context_id`, so one
address space is one ASID however many Procs hold it — I-31 composes with
sharing by construction, as a direct consequence of L-1 having moved
`context_id` into the object. `asid.tla` re-ran unperturbed (443457 distinct,
identical to L-1 and L-2a; the depth figure is not a fingerprint — see the end
of this file).

---

## Known caveats / footguns

**A value read that used to yield 0 for a kernel-only Proc now dereferences
NULL.** This is the mirror image of the predicate hazard, and the compiler
catches *neither*. The reachable sites, all guarded at L-1:

- `/ctl/procs` (`devctl.c`) and `/proc/<pid>` (`devproc.c`) — both walk a Proc
  tree whose **root is kproc**, and both report `0` for a Proc with no address
  space, which is exactly what the old inline field read.
- `format_maps` (`devproc.c`) — returns the header alone, which is what an empty
  VMA list produced before.
- `vma_drain` and `proc_quiesce_owned_devices` — reachable from `proc_free`'s
  rollback path with a Proc that failed *before* `addrspace_alloc` ran.
- The six I-32 charge/uncharge helpers — refuse (fail closed) rather than deref.

This is not theoretical. Removing the `devctl.c` guard makes the kernel
**extinct during boot** with `unhandled kernel translation fault 0x20` — and
`0x20` is precisely `page_count`'s offset in `struct AddrSpace` (`ref` 0, `lock`
4, `pgtable_root` 8, `context_id` 16, `vmas` 24, `page_count` **32**). Removing
the `proc_page_charge` guard reproduces the identical fault. Both were
revert-probed at L-1.

**The refcount is atomic from the start** even though it never exceeds 1 today.
An `int` that is "always 1 for now" and becomes contended at L-3 is exactly the
latent-P1 shape CLAUDE.md's multi-thread-shared-state rule warns about.

**Do not rename these fields by grep.** Two of the seven names are overloaded in
this tree — `struct Burrow.page_count` and `psci_cpu_on(..., u64 context_id)` —
so a textual rename corrupts unrelated code silently. The L-1 conversion was
compiler-driven for this reason: delete the fields from `struct Proc` first and
let the compiler enumerate the real sites (measured: 239, all on `struct Proc`,
zero false positives).

---

## Tests

`kernel/test/test_addrspace.c` — `addrspace.alloc_shape` (ref 1, real page
table, empty VMA list, unassigned ASID, zeroed I-32 axes), `addrspace.refcount`
(the object survives a non-final unref; `unref(NULL)` is safe),
`addrspace.kproc_has_none` (the equivalence the refactor rests on), and
`addrspace.charge_helpers_refuse_without_as` (the guards fail closed).

L-1's own gate is that **every pre-existing test is unchanged**: 1272/1272 before,
1276/1276 after (the four new ones), boot OK, 0 EXTINCTION, `asid.tla` clean.

---

## Status

Landed at L-1. `struct Proc` went **408 → 376 bytes**; all 23 surviving offset
`_Static_assert`s were re-baselined and the three for removed fields deleted.

Reserved for later chunks: `execve` (L-2), `rfork(RFPROC|RFMEM)` + VFORK (L-3),
the COW break arm + a real per-page share count (L-4), `SYS_RFORK` +
child-context restoration (L-5).

---

## A note on the `asid.tla` figures

**The depth figure is NOT a fingerprint.** Measured 2026-08-02: `-workers auto` reported depth 17 and 18 on back-to-back runs of the identical command, while `-workers 1` reported 17 on three consecutive runs. TLC's reported search depth varies with worker scheduling; the DISTINCT-STATE COUNT (443457) is the stable figure and the one to compare across chunks. Earlier chunks recorded "depth 18" as though it pinned the model -- it does not.
