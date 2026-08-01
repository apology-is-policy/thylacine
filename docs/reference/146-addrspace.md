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
  distinct states, depth 18).
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

`addrspace_unref`'s last drop destroys the page table and frees the struct.

**Precondition on the last drop**: the VMA list is already drained and no CPU
holds this table in TTBR0. `proc_free` establishes both — `vma_drain` runs
earlier in the teardown, and every thread was reaped and `on_cpu`-spun before
`proc_free` is reached. `addrspace_unref` asserts the first (`extinction` on a
live VMA list) rather than trusting it.

No TLB flush at teardown and no per-AS ASID free — the Linux model. The leaf
mappings were invalidated by `vma_drain`'s all-ASID `tlbi vaae1is`, and any
eventual reuse of the ASID value is gated by the rollover's per-CPU
`flush_pending` local flush (ARCH §6.2.1).

---

## Lifecycle

- **Create**: `proc_alloc` calls `addrspace_alloc` where it used to call
  `proc_pgtable_create`. On OOM the rollback is the same `ZOMBIE + proc_free` as
  every other alloc step; `proc_free`'s `addrspace_unref(NULL)` is a clean no-op.
- **Release**: `proc_free` calls `addrspace_unref` at *exactly* the point the
  inline page-table destroy used to run — after `vma_drain`, before the Proc
  struct is freed. Keeping that position is what makes the teardown ordering
  byte-identical.

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
