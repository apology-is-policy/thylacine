# 127 — Overcommit memory (lazy-anon demand-zero + decommit)

**Status**: BUILT (#319, 2026-06-23). The kernel mechanism + the EL0 ABI; the
userspace malloc-substrate wiring (libthyla-rs `sysAlloc`, the pouch `mmap`
boundary-line, the Go runtime `sysReserve`/`sysUnused`) lands at #321.

Realizes the demand-zero-anon BUILD seam recorded in ARCH §6.1/§6.5 ("The
overcommit model"); extends invariant **I-32** (the per-Proc resource floor)
with a **fourth axis** — a live-VMA cap — but adds no new invariant number.

## Purpose

Thylacine's v1.0 anonymous-memory primitive (`SYS_BURROW_ATTACH`, §6.5 Tier 1)
commits every page eagerly at attach: `burrow_create_anon` does
`alloc_pages(order, KP_ZERO)` for the whole region. That is correct for a
program that immediately touches what it reserves, but it is the *opposite* of
what the native-toolchain ecosystem (musl/glibc/jemalloc/tcmalloc/LLVM,
Go-stock) assumes: **reserve address space cheaply, commit on first touch, RSS
= what was actually touched, release on `madvise`** — the Linux *overcommit
contract*. The Go runtime's 64-bit page allocator, for example, reserves a
~512-MiB page-summary for a 48-bit heap and touches almost none of it; under
the eager model that reservation overflowed `BURROW_ATTACH_MAX` (256 MiB).

The overcommit model gives a Proc the Linux contract through a **dedicated
lazy-attach syscall** (`SYS_BURROW_ATTACH_LAZY`) that reserves the VA range
but commits no pages, a demand-zero fault arm that allocates + zero-fills each
page on first touch, and a decommit syscall (`SYS_BURROW_DECOMMIT`, the
`madvise(MADV_DONTNEED)` analog) that releases resident pages back to the buddy
while keeping the reservation. The capability-microkernel precedent is
**Fuchsia's VMOs** (lazy commit + `zx_vmo_op_range(DECOMMIT)`); seL4's
explicit-frame no-overcommit model is the deterministic alternative declined
because Thylacine *runs arbitrary Linux binaries* (via pouch) where seL4 does
not.

The eager `SYS_BURROW_ATTACH` (= 37) stays a separate 1-arg syscall,
byte-identical — kernel-internal copy-target callers (exec data segments, DMA
buffers, the Weft rings) keep `burrow_create_anon`, which needs pages present
at create. A dedicated lazy syscall (over a flags arg on `SYS_BURROW_ATTACH`)
was the user vote (2026-06-23): it keeps the hot eager path byte-unchanged and
matches the Plan 9 small-syscall idiom that already splits `ATTACH`/`DETACH`.

## Public ABI

| Syscall | Num | Args (x0..) | Returns |
|---|---|---|---|
| `SYS_BURROW_ATTACH_LAZY` | 83 | `length` | base VA (≥ `EXEC_USER_BURROW_BASE`) / -1 |
| `SYS_BURROW_DECOMMIT` | 84 | `vaddr`, `length` | 0 / -1 |

- **`SYS_BURROW_ATTACH_LAZY(length)`** — reserve a `length`-byte (page-rounded)
  anonymous region in the burrow-attach window. No physical pages are
  committed; the I-32 `page_count` is **not** charged here. Returns the
  kernel-chosen base VA. -1 on `length == 0`, `length > BURROW_ATTACH_MAX`, no
  free gap, the VMA cap (`PROC_VMA_MAX`), or OOM.
- **`SYS_BURROW_DECOMMIT(vaddr, length)`** — release the resident pages of a
  `BURROW_TYPE_ANON_LAZY` region in `[vaddr, vaddr+length)` without removing
  the VMA. Clears each PTE (+ TLBI before the page frees), frees the page,
  NULLs the sparse slot, uncharges `page_count`. A later touch re-faults a
  fresh zero page. Confined to the burrow-attach window; rejects a
  non-`ANON_LAZY` VMA or a range not within one VMA. -1 on a bad range / wrong
  type.

`SYS_BURROW_ATTACH` (37) / `SYS_BURROW_DETACH` (38) are unchanged. A lazy
region is detached with the same `SYS_BURROW_DETACH` (the detach is
type-aware — see below).

## `BURROW_TYPE_ANON_LAZY` (kernel/include/thylacine/burrow.h)

A fifth `enum burrow_type`. The structural twin of `BURROW_TYPE_FILE` (REVENANT,
doc 126) but **simpler**: there is no backing file, so the fault arm allocates
+ zero-fills + installs entirely under `vma_lock` — no blocking read → no slow
path, no pin, no death-interruptible read. `pages` is NULL (no contiguous
chunk); the per-page physical pages live in the **sparse `filepages` array**
(the same field FILE uses), each slot NULL until faulted in (or after a
decommit releases it). No `spoor`, no file/cache fields, no kobj.

- `burrow_create_anon_lazy(size)` — mirrors `burrow_create_file` minus the
  backing Spoor: rounds `size` to whole pages, kmalloc's the `filepages` array
  (the only allocation), `handle_count = 1`. Returns NULL on size 0 / overflow
  / SLUB OOM / array OOM.
- `burrow_free_internal` ANON_LAZY arm — frees every resident page (order 0
  each) + kfrees the array. Runs at `{handle_count==0 && mapping_count==0}`, so
  no concurrent faulter touches `filepages`; the walk needs no lock (mirrors
  the FILE arm, minus the `spoor_clunk`). Double-free guard: `filepages ==
  NULL`.
- `burrow_acquire_mapping` ANON_LAZY arm — liveness is `filepages != NULL`;
  an all-NULL array is the normal "nothing faulted in yet" state, not a UAF.
- `burrow_decommit(p, vaddr, length)` — the SYS_BURROW_DECOMMIT core (see ABI).
- `burrow_lazy_resident_count(v)` — the count of resident slots; the amount
  `SYS_BURROW_DETACH` uncharges (a lazy region charged `page_count` per fault,
  not per reservation, so a full detach uncharges only the resident pages).

## The demand-zero fault arm (arch/arm64/fault.c)

A `BURROW_TYPE_ANON_LAZY` case in `demand_page_locked`'s type switch, run under
the caller's `vma_lock`:

1. Compute the slot. Under `v->lock`, read `filepages[slot]`. A **resident
   hit** (re-fault, or a sibling faulter filled it) installs the PTE with no
   re-charge.
2. **Miss**: `proc_page_charge(p, 1)` **before** the alloc — over `PROC_PAGE_MAX`
   on a non-TCB Proc fails the fault → `FAULT_UNHANDLED_USER` → the caller
   `proc_fault_terminate`s (graceful OOM, never a box extinction; I-32). Then
   `alloc_pages(0, KP_ZERO)`; on OOM, uncharge + fail.
3. **Install-once** under `v->lock`: if the slot is still NULL we win (the
   Burrow owns the page); else a sibling won — we are the loser, free our page
   + uncharge outside `v->lock`. The loser branch is unreachable at v1.0 (one
   mapping under `vma_lock` serializes all faults of the Proc) but is the
   audited FILE-arm shape, robust to a future shared-lazy mapping.
4. Install the L3 PTE at `vma->prot` (= `VMA_PROT_RW` for an attach region) →
   `make_user_pte_l3` gives **RW + PXN + UXN**: writable, never executable
   (**W^X / I-12** holds by construction).

`alloc_pages` runs under `vma_lock` (the established order — eager
`SYS_BURROW_ATTACH` already holds `vma_lock` across `burrow_create_anon →
alloc_pages`), but never under `v->lock`; `free_pages` of the loser/decommit
page runs outside `v->lock` (leaf-lock discipline — `free_pages` takes the
buddy lock, which must not nest under `v->lock`).

## Charge-on-fault: `page_count` tracks true RSS

Eager attach charges `page_count` at attach (the whole region). **Lazy attach
charges nothing at attach** — `page_count` is charged **at fault time, per
page**, so it equals the true committed RSS:

- fault → `+1` (only on the alloc-new path; a resident hit does not re-charge);
- decommit → `-resident` (per freed page);
- re-fault → `+1` (a fresh zero page).

`SYS_BURROW_DETACH` of a lazy region is **type-aware**: it reads
`burrow_lazy_resident_count` (the resident slots) and uncharges exactly that —
not `length/PAGE_SIZE` (which would over-uncharge a partially-touched lazy
region). The eager detach path is unchanged (`uncharge = length/PAGE_SIZE`).
The detach reads the count under one `vma_lock` hold *before* `burrow_unmap`
frees the Burrow, so there is no UAF; `burrow_free_internal`'s ANON_LAZY arm
frees the resident pages.

## The I-32 fourth axis: `PROC_VMA_MAX`

A *free* lazy reservation reopens a DoS the eager path closed by construction:
eager self-limits at ~`PROC_PAGE_MAX` VMAs (each charges ≥1 page), but a
hostile Proc could spam tiny lazy reservations to exhaust the Vma slab. So
I-32 gains a fourth axis — a per-Proc cap on **live VMAs**:

- `PROC_VMA_MAX = 65536` (the Linux `vm.max_map_count` analog; 65536 × 64 B =
  4 MiB of Vma slab is the per-Proc ceiling).
- `proc_vma_charge(p)` / `proc_vma_uncharge(p)` (kernel/proc.c) — charged at
  **`vma_insert`** / uncharged at **`vma_remove`**, both under `p->vma_lock`,
  so the cap is exact. A non-TCB Proc at the cap fails `vma_insert` with -1
  (identically to an overlap; the caller `vma_free`s the rejected Vma). Charges
  nothing on the refused return, so no rollback is needed.
- Counted **uniformly** for every VMA (attach / exec image / guard / DMA / Weft
  share). It is a **no-op for the eager path** (already transitively bounded)
  and the real bound for lazy. `PRINCIPAL_SYSTEM` (the TCB — kproc + the
  boot/service chain) is **exempt** from the cap (the count is still maintained
  for observability); the exemption is unforgeable (CAP_SET_IDENTITY rejects
  SYSTEM). NOT propagated by rfork (KP_ZERO → 0). It is a resource axis, not a
  privilege axis — orthogonal to I-22.

`struct Proc` grows 280 → 288 (the `vma_count` u32 @280 + tail pad); pinned by
`_Static_assert`s.

## Tests

| Test | Covers |
|---|---|
| `sys_burrow.attach_lazy_window_va` | lazy attach: window VA, ANON_LAZY VMA, `page_count == 0`, `vma_count == 1` |
| `demand_page.lazy_zero_fill` | first touch demand-zeroes + installs RW/XN, charges 1; re-fault no double-charge |
| `demand_page.lazy_decommit_refault` | decommit frees resident pages + clears PTEs + uncharges; re-fault = fresh zero; **no page leak** (phys_free_pages baseline) |
| `demand_page.lazy_charge_on_fault_oom` | over-`PROC_PAGE_MAX` fault → `FAULT_UNHANDLED_USER` (graceful OOM), no page committed, `page_count` unchanged |
| `demand_page.lazy_detach_uncharges_resident` | detach uncharges only the resident pages, not the whole span |
| `resource.vma_cap` | `proc_vma_charge` cap boundary, uncharge re-opens, saturation guard, TCB exemption |

Full suite: **992/992 PASS** (986 + these 6), 0 EXTINCTION, boot OK.

## Error paths

- `SYS_BURROW_ATTACH_LAZY` → -1: length 0 / over max / no gap / `PROC_VMA_MAX` /
  OOM. No state changes on any -1 (the failed `burrow_map` frees the empty lazy
  Burrow; the rejected `vma_insert` charges no `vma_count`).
- `SYS_BURROW_DECOMMIT` → -1: bad alignment / overflow / out of window /
  range not within one VMA / VMA not `ANON_LAZY`. No-op on -1.
- Fault arm → `FAULT_UNHANDLED_USER`: over-cap charge, alloc OOM, or an
  impossible shape change. The caller per-Proc-terminates (graceful OOM) —
  never a box extinction.

## Known caveats / footguns

- A lazy region is **W^X-RW only**: the fault installs RW/XN. There is no
  lazy-exec path (text is file-backed, doc 126).
- The install-once loser branch + the `v->lock` in `burrow_lazy_resident_count`
  / the fault arm are **defensive** against a future shared-lazy mapping; at
  v1.0 a lazy Burrow is mapped into exactly one Proc, so `vma_lock` already
  serializes all access to `filepages`. `burrow_share_into` (Weft) handles only
  `BURROW_TYPE_ANON`, not lazy.
- `burrow_decommit` walks the range per page (a `v->lock` grab + a `free_pages`
  per resident slot). A 256-MiB decommit is 65536 TLBIs + frees — a cold path
  (Go's GC calls `sysUnused` occasionally); the TLBI cost dominates. Batched
  TLBI is a future optimization (the load-bearing property — no stale cached
  translation when it returns — holds either way).
- The unconditional migration of *every* `SYS_BURROW_ATTACH` to demand-zero is
  a recorded v1.x follow-up (it would re-audit Weft/Loom/exec-data callers).

## Spec cross-reference

No new TLA+ spec (the 2026-05-23 broadening) — the invariant is validated by
prose (this doc + ARCH §6.5 / §28 I-32) + the focused audit (#320) + the
runtime tests, exactly as the near-identical REVENANT `BURROW_TYPE_FILE` arm
(doc 126). The composed invariants: I-12 (W^X), I-7 (the #847 dual-refcount),
I-32 (the resource floor + the new VMA axis), I-30-class submit-time discipline
(the charge captured before the commit).

## Status / landing

- #318 scripture (ARCH §6.5 + §28 I-32 + §25.4 + CLAUDE.md mirror).
- #319 (this) — the kernel mechanism + the 6 tests + this doc.
- #320 — the focused Opus-4.8-max audit + the SMP gate.
- #321 — the userspace wiring (libthyla-rs / pouch / Go `sysReserve`=LAZY,
  `sysUnused`=DECOMMIT) + the Go **stock `heapAddrBits=48`** proof (a 512-MiB
  page-summary reserve commits nothing until touched — the proof the model
  needs no per-program tuning).
