---
id: chg-2026-09-06-resource-doc-absorb
type: chg
title: "absorb docs/reference/110-resource (per-Proc resource floor, I-32 DoS bound): zero-fold, 4-surface redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["2626175f"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The I-32 resource-floor reference (477 lines) -- the DoS bound. Verified
atom-by-atom across four fresh owners (I-32 is the invariant this sweep has
touched most: 24-pgtable, 127-overcommit, 99-fs-perm, 148-fork); zero fold.

HOMES: proc_thread_cap_ok/proc_child_cap_ok (the I-32 creation gates) +
proc_resource_exempt (TCB exemption) + the ncpus-1 bounded TOCTOU overshoot ->
sub-kernel-proc; addrspace_alloc(page_budget) [0-budget refused not unlimited] +
the six charge/uncharge counters + page_budget/vma_count/shared_map_pages beside
their counts under as->lock -> sub-kernel-addrspace; the thread_count ->
sub-kernel-thread; the TCB exemption as a capability property -> sub-kernel-caps.

WHAT THE DOC GOT WRONG: the page axis is per-ADDRESS-SPACE, not per-Proc. The doc
(title + CL-5 section) frames it per-Proc; since L-1/RW-12 the enforced bound is
AddrSpace.page_budget (seeded from the Proc's authorization Proc.page_budget), so
RFMEM siblings + peer threads share the one cap -- the counter hangs off the
AddrSpace it charges. Thread+child axes stay per-Proc. Content now distributed
across 4 dossiers at more depth.

ZERO fold. Render clean; lint 0-fail. view-absorption 80 -> 81.
