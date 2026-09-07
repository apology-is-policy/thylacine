# 24 — per-Proc page-table allocator (P3-Bcb/Bd/Db/Dc) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-pgtable-doc-absorb`).
The per-Proc user-half (TTBR0) translation-table allocator: create the L0 root,
grow the L0→L3 tree on demand-fault to install leaf PTEs, and free the whole tree
on teardown. A Phase-3-era doc, comprehensively superseded; every atom is carried,
more currently, by:

- the **allocator + the PTE encoding** — `proc_pgtable_create` (one KP_ZERO L0
  page), `proc_pgtable_destroy` (the recursive L0→L3 walk that frees only
  translation-table pages, leaves belong to the VMA/Burrow layer — #116),
  `mmu_install_user_pte` (walk-and-grow, allocate missing sub-tables, install the
  L3 leaf; OOM returns -1, identical re-install is idempotent), and the PTE bit
  composition that makes W^X true by construction (PXN always, UXN unless exec,
  AP from prot, nG/AF; the encoder extincts an executable non-`NORMAL_WB`
  mapping):

      vault/system/kernel/memory/sub-kernel-mmu.md

- the **`pgtable_root` field and its lifecycle** — it now hangs off the
  **AddrSpace**, not the Proc (LINEAGE L-4; see below), allocated with the
  AddrSpace and freed with it:

      vault/system/kernel/memory/sub-kernel-addrspace.md

- the **TTBR0 install at context switch** — `cpu_switch_context` saves+loads
  `TTBR0_EL1 = (asid << 48) | pgtable_root` atomically with the rest of the
  register state, and `sched_activate_addrspace` is the one out-of-switch install:

      vault/system/kernel/scheduling/sub-kernel-sched-smp.md

- **why the walk needs no per-Proc pgtable lock** — the demand-fault path holds
  the address-space lock across the whole resolve-and-install, which serializes
  the walk-allocate-install sequence (this is what closes the race the doc left
  open, below):

      vault/system/kernel/memory/sub-kernel-fault.md

- the **teardown TLB lifecycle** — the ASID tag plus `asid_free`'s broadcast
  `tlbi` before the ASID is recycled, so the recursive free issues no per-page TLB
  ops:

      vault/system/kernel/memory/sub-kernel-asid.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **`pgtable_root` lives on the AddrSpace, not the Proc.** This file says
  "`struct Proc.pgtable_root`"; since LINEAGE L-4 the field hangs off
  `struct AddrSpace` (`p->as->pgtable_root`), which is what lets `rfork(RFMEM)`
  siblings and peer threads share one user address space. The allocator functions
  are unchanged; their *owner* moved.
- **The multi-thread threading hazard is CLOSED, not open.** This file documents
  a trip-hazard — "Phase 5+ multi-thread Procs need a per-Proc pgtable lock … a
  concurrent fault on the same Proc would race on sub-table allocation." That race
  is closed: the demand-fault path holds the **address-space lock** (`lock-vma`)
  across the whole resolve-and-install, serializing the walk. It is not an
  outstanding per-Proc-lock TODO.
- **The Phase-3 forward-references landed.** "P3-Bd will wire this [TTBR0] load",
  "in advance of P3-Dc's demand-paging fault handler", "any future caller" — all
  built; the demand-paging fault handler is `sub-kernel-fault`.
