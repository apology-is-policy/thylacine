---
id: chg-2026-09-06-uaccess-doc-absorb
type: chg
title: "absorb docs/reference/40-uaccess (R12 kernel-mode user-VA accessor): fold the F210 P1 caller-bound corollary into sub-kernel-uaccess, redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-uaccess]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The __ex_table-style kernel-mode user-VA accessor. An audit-trigger surface
(Exception entry + Page fault + Capability checks). A dedicated dossier
(sub-kernel-uaccess, audit:hard) owns uaccess.c/.S/.h; verified atom-by-atom.

WHAT WAS ALREADY COVERED (verified, not assumed):
- The fixup table (signed-32-bit-relative pairs) + the demand-page-then-fixup
  success path + the three-way recovery conjunction (kernel-mode AND user-half
  AND table-hit) -> sub-kernel-uaccess.
- The CF-3 bulk copy_out/copy_in (three fault points under one label, byte
  head/8-byte body/byte tail) -> sub-kernel-uaccess ("three fault points wearing
  one coat"; CF-3 is in the dossier's design: frontmatter, so NOT a fold).
- The alignment-fault-not-recoverable caveat + the header-comment drift (the
  dossier already flags the stale "one primitive/one entry" comments vs the ten
  fault points / six primitives) -> sub-kernel-uaccess.
- The uaccess.c <-> mmu.h compile-time bound assertion -> sub-kernel-uaccess.
- The recoverable Sync vector slot + dispatcher placement -> sub-kernel-exception.

THE FOLD (the atom that lived only in the doc):
The F210 P1 finding's COROLLARY. The dossier had "the user-half bound must stay
pinned to the memory layer's" AND "callers must validate range" as SEPARATE
bullets, but not the interaction they don't connect: a CALLER holding a laxer
bound than the dispatcher's fixup gate (fi.vaddr < UACCESS_USER_VA_TOP) is an
EL0-triggerable extinction -- a VA in the gap passes the caller, reaches the
non-range-checking primitive, faults ABOVE the user half, and fails gate #2, so
the fixup does NOT apply and the kernel extincts. SYS_PUTS once held 2^48 while
the gate held 2^47, so any user pointer in [2^47, 2^48) was an unprivileged
kernel extinction; the fix converges every bound-holder (caller + dispatcher gate
+ memory layer) on the one UACCESS_USER_VA_TOP constant. Folded into the dossier's
Invariants section (where the three-way conjunction is explained).

NOT REFUTED: the doc is current. The gap was the caller-drift corollary connecting
two existing bullets. Zero code change. Redirect stub.
