# 110 — Per-Proc resource floor (the DoS bound) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-resource-doc-absorb`).
The I-32 resource floor: a non-TCB Proc's fork / thread / memory bomb hits a clean
per-object cap instead of stressing the allocator toward the box-killing cliff — a
resource axis, not a privilege axis (orthogonal to I-22). Every atom is carried,
more currently, by the fresh owning dossiers:

- the **thread and child caps, the exemption, and the bounded TOCTOU** —
  `proc_thread_cap_ok` / `proc_child_cap_ok` (the I-32 creation gates that run
  before the expensive allocations and take the table lock themselves),
  `proc_resource_exempt` (the TCB exemption), and the deliberate `ncpus-1`
  overshoot on the child/thread caps (read under a different lock than the counter
  they gate):

      vault/system/kernel/execution/sub-kernel-proc.md

- the **page / VMA / shared-map axes** — `addrspace_alloc(page_budget)` (which
  refuses a **0** budget rather than reading it as unlimited), the six
  `addrspace_charge_*`/`uncharge_*` counter operations, and `page_budget` /
  `vma_count` / `shared_map_pages` / `page_peak` each sitting beside the count it
  bounds under `as->lock`:

      vault/system/kernel/memory/sub-kernel-addrspace.md

- the **thread count** the thread cap reads (linearizable with the list head):

      vault/system/kernel/execution/sub-kernel-thread.md

- the **TCB exemption as a capability property** (`PRINCIPAL_SYSTEM` is exempt +
  unforgeable; the floor confers and bypasses no capability):

      vault/system/kernel/security/sub-kernel-caps.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The page axis is per-ADDRESS-SPACE, not per-Proc.** This file (title and the
  CL-5 section both) frames the page budget as per-Proc. Since LINEAGE L-1 / RW-12
  the enforced bound lives on the **AddrSpace** (`AddrSpace.page_budget`, seeded at
  `addrspace_alloc` from the creating Proc's authorization); `Proc.page_budget` is
  the *authorization* — what a Proc may seed or raise an AddrSpace to — not the
  enforced cap. `rfork(RFMEM)` siblings and peer threads share the one address
  space and therefore the one cap, which is why the counter must hang off the
  AddrSpace it charges (`sub-kernel-addrspace` carries the full reasoning, and the
  rejected inverse). The thread and child axes stay per-Proc.
- **The content is now distributed** across the four dossiers above, each at more
  depth — notably the charge concurrency and the page_budget-beside-its-count
  discipline, which are the I-32 soundness arguments.
