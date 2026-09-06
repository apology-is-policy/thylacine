# 25 — Fault dispatcher [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-fault-absorb`; the
memory area, completing it). This document spanned two layers the vault keeps as
two dossiers, so it redirects to both:

- the **fault dispatch, demand paging, and the COW break** (`arch/arm64/fault.c`
  — the fixed-order kernel classification and its scar tissue, the seven backing
  arms and the HOSTMEM MAIR-index widening, the file arm's four-step
  drop-the-lock protocol and the #190 verify-and-bail, the ordinary arms taking
  no Burrow ref, the no-TLBI-on-invalid→valid install, the bounded offset, the
  copy-on-write break and its uninstall-before-install, the #194 past-EOF
  `FAULT_USER_BUS`, read-ahead. I-12, I-32, I-7, I-36, I-44):

      vault/system/kernel/memory/sub-kernel-fault.md

- the **EL0 sync-vector dispatch layer** (`arch/arm64/exception.c` —
  `exception_sync_lower_el` / `exception_sync_curr_el`, the vector slot dispatch,
  the EL0 SVC/PC_ALIGN/SP_ALIGN/BTI/BRK handling, and where a kernel fault
  extincts vs an EL0 fault terminates the Proc):

      vault/system/kernel/entry/sub-kernel-exception.md

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- Its concurrency posture is stale: it claims the path is **single-threaded and
  needs a future lock**, but the #713 fix made the whole fast path run under
  `vma_lock` (the fault handler is a reader on the same lock), closing the
  half-unlinked-list UAF. A reader following the doc would add a lock that
  already exists.
- It carries only **three `fault_result` values**; the dossier has four
  (`FAULT_USER_BUS` — a valid mapping whose backing store failed, distinct from a
  bad address, so a wedged FS server is not reported as a segfault in the victim).
- It predates the **seven backing arms** (the HOSTMEM arm and the
  `device_memory` bool → MAIR-index widening, the code/JIT arm), the **DISTRO
  D-3** file-mmap generalization (and the verify-and-bail that replaced the R-5
  one-fixed-VMA premise), and the copy-on-write break entirely.
- The exact register-decode bit positions and the vector-slot offsets live in
  `arch/arm64/fault.c` / `arch/arm64/exception.c` — the source of truth — which
  the dossiers point at rather than duplicating.
