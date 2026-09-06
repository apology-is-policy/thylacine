---
id: chg-2026-09-06-sys-burrow-doc-absorb
type: chg
title: "absorb docs/reference/79-sys-burrow (SYS_BURROW_ATTACH/DETACH): fold the F1 window-confinement finding into sub-kernel-vma, multi-redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-vma]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The v1.0 native anonymous-memory interface (SYS_BURROW_ATTACH=37 / DETACH=38, the
malloc substrate). An mm audit-trigger surface (I-7). Verified atom-by-atom.

ALREADY COVERED (verified, not assumed):
- The Tier-1 anon Burrow + I-7 dual-refcount discipline (create_anon handle=1 ->
  map mapping->1 -> unref handle->0 keeps alive; free-when-both-zero at
  sub-kernel-burrow:86-87; the eager power-of-two backing) -> sub-kernel-burrow.
- vma_find_gap (overflow-free first-fit, never forms cand+length) + the detach
  path -> sub-kernel-vma.
- The per-AddrSpace vma_lock + a sibling thread's SYS_BURROW_ATTACH race ->
  sub-kernel-addrspace.
- The handlers (thin current_thread wrappers over _for_proc inners) + the window
  constants -> sub-kernel-syscall-dispatch.
- pouch mmap(MAP_ANONYMOUS)/munmap onto the two syscalls -> sub-pouch-process.

THE FOLD (the atom that lived only in the doc):
The P6-pouch-mem-a F1 window-confinement SECURITY finding. burrow_unmap matches a
VMA by GEOMETRY ALONE, so SYS_BURROW_DETACH must reject any vaddr outside the
burrow-attach window [EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP) BEFORE the
match -- else EL0 could pass the coordinates of its own ELF-segment / stack /
stack-guard VMA and have it dismantled (the stack-guard case silently retiring a
security-relevant page; a geometry match cannot tell those from a burrow region).
Every attach lives in the window and every ELF/stack/guard sits below it
(_Static_assert'd in exec.h), so the bound structurally excludes them. Folded into
sub-kernel-vma's Prosecution (where vma_remove/burrow_unmap's geometry-match
lives), tied to I-1.

NOT REFUTED: the two-tier model + the deliberate brk/file-mmap refusals are
current; the libt wrappers remain deferred. Zero code change. Multi-redirect stub.
