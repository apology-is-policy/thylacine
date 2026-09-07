# 40 — Kernel-mode user-VA access primitives (R12-uaccess) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-uaccess-doc-absorb`).
The `__ex_table`-style kernel-mode user-VA accessor: `uaccess_load_u8` (and the
store/u32/bulk-copy siblings), the `.uaccess_fixup` PC-relative table, and the
sync-fault dispatcher integration that demand-pages the user page and either
retries the load or transfers to a fault-recovery label returning -1. Its
content lives, code-verified and current, in:

- the **whole primitive + table + dispatcher hook** — the fixup-table encoding
  (signed 32-bit relative pairs), the demand-page-then-fixup success path, the
  three-way recovery conjunction (kernel-mode fault AND user-half address AND
  table hit), the CF-3 bulk `copy_out`/`copy_in` (three fault points under one
  label), the alignment-fault-not-recoverable caveat, and the header-comment drift
  it already names — plus the **F210 P1 corollary folded here at this absorption**:

      vault/system/kernel/entry/sub-kernel-uaccess.md   (audit: hard, inv-i13)

- the **exception-entry side** — the recoverable Sync vector slot (the kernel
  sync tail now returns through the shared exit trampoline instead of halting)
  and the dispatcher's placement before the fatal fault handler:

      vault/system/kernel/entry/sub-kernel-exception.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The F210 P1 finding's corollary was uncovered — folded at absorption.** The
  dossier carried "the user-half bound must stay pinned to the memory layer's" and
  "callers must validate range" as *separate* Prosecution bullets, but not the
  *interaction* the F210 audit made concrete: a caller holding a laxer bound than
  the dispatcher's fixup gate (`fi.vaddr < UACCESS_USER_VA_TOP`) creates an
  EL0-triggerable extinction — a VA in the gap passes the caller, reaches the
  non-range-checking primitive, faults above the user half, and fails the gate, so
  the fixup does not apply and the kernel extincts. `SYS_PUTS` once held `2^48`
  while the gate held `2^47`; the fix converges every bound-holder on
  `UACCESS_USER_VA_TOP`. Now folded into the dossier's Invariants section.
- **The doc's own "primitive inventory" caveat is current** — the dossier already
  documents that the file's summarizing comments (still claiming one primitive /
  one table entry) lag the ten fault points and six primitives that exist. The
  rest (the fixup mechanism, the CF-3 bulk pair, the performance numbers) is
  covered. Zero code change.
