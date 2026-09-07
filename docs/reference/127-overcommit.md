# 127 — Overcommit memory (lazy-anon demand-zero + decommit) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-overcommit-doc-absorb`).
The demand-zero anonymous overcommit model (#319): a lazy anonymous region reserves
address space but mints no pages until first touch, and `decommit` releases them
again without unmapping. It extends I-32 with a fourth axis (a live-VMA cap) but
adds no new invariant number. Every atom is carried by the fresh owning dossiers:

- the **substrate and the decommit primitive** — `burrow_create_anon_lazy` (an
  anonymous, demand-zeroed, sparse Burrow) and `burrow_decommit` (releases resident
  pages of a lazy region without unmapping it):

      vault/system/kernel/memory/sub-kernel-burrow.md

- the **demand-zero fault arm** — allocate + zero + install-once under the lock,
  no backing read and so no slow path, and the charge-on-fault that makes
  `page_count` track true RSS (the I-32 page budget charged *before* the allocation,
  fail-closed):

      vault/system/kernel/memory/sub-kernel-fault.md

- the **I-32 accounting and its fourth axis** — the `vma_count` live-VMA cap
  alongside the page budget and shared-map axes, the six charge/uncharge counter
  operations, and the atomic charge discipline (a lost charge announces itself in
  neither direction):

      vault/system/kernel/memory/sub-kernel-addrspace.md

- the **EL0 ABI** — the lazy-attach syscall and `decommit`, in the syscall number
  space and its dispatch:

      vault/system/kernel/entry/sub-kernel-syscall-abi.md
      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The userspace malloc-substrate wiring landed.** This file says it "lands at
  #321" — the libthyla-rs `sysAlloc`, the pouch `mmap` boundary-line, and the Go
  runtime `sysReserve`/`sysUnused` are built; the mechanism this doc describes is
  the substrate they sit on.
- **The content is now distributed** across the four dossiers above, each carrying
  its half at more depth — notably the charge concurrency (`sub-kernel-addrspace`)
  and the fail-closed charge-before-allocate ordering (`sub-kernel-fault`), which
  are the I-32 soundness arguments the axis turns on.
