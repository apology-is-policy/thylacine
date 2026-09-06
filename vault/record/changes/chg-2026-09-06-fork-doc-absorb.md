---
id: chg-2026-09-06-fork-doc-absorb
type: chg
title: "absorb docs/reference/148-fork (SYS_RFORK + fork, LINEAGE L-3b..L-5): fold the #137 WnR-decode lesson into sub-kernel-fault, 9-surface redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-fault]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The process-creation reference (583 lines) -- the richest remaining doc: rfork
(L-3b), descriptor inheritance (L-3c-1), the vfork suspend (L-3c-2), stock COW
fork (L-5), the whole I-44 arc. Verified atom-by-atom across NINE owners; one
genuine fold, grounded in current fault.c.

HOMES (all carried, most at more depth):
- rfork_internal (one body, three-AS-answers, fork_context) + inheritance (caps
  minus CAP_ELEVATION_ONLY, Territory clone) + the vfork suspend (vfork_await_
  release, the release-IS-the-release predicate, the ABA-safety from the held ref,
  the capture-pid-before-ready UAF the park aligned) -> sub-kernel-proc.
- fork_frame_init (the pure two-edit: regs[0]=0 + sp=child_sp, verbatim else) +
  thread_create_forked (the third shape, frame off the child kstack, FP from live
  regs) -> sub-kernel-thread.
- thread_fork_trampoline (the shared-return branch, #811/#713, no GPR sweep) ->
  sub-kernel-exception.
- SYS_RFORK=102 handler validation -> sub-kernel-syscall-dispatch.
- descriptor inheritance L-3c-1 (handle_table_copy_into, handle_slot_may_alias
  [kind=I-5, object=devsrv Spoor], rights verbatim I-6, the hole-not-refusal) ->
  sub-kernel-handle.
- addrspace_clone + #136 (the writability split: FILE + read-only eager-ANON
  shared, writable ANON refused, MMIO/DMA refused; the vDSO-clock-page reach) +
  I-44 break -> sub-kernel-addrspace.
- the CAP_ELEVATION_ONLY strip (I-2) -> sub-kernel-caps.

FOLD (one genuine gap): #137 -- the WnR write/not-read decode read ISS bit 9 (EA,
always 0 for normal aborts) instead of bit 6, so is_write was ALWAYS FALSE
tree-wide; unreachable until the COW break's write arm needed it, where the wrong
bit re-installs read-only + loops = a HANG with no fault logged. Hidden by a unit
test that MIRRORED THE CONSTANT (set bit 9, asserted bit 9 read -- agreed with the
code not the hardware). Folded into sub-kernel-fault's Caveats (thematically
beside the existing read/write-encoding caveat); the reusable control-trap lesson
(a test that shares the constant it checks is a tautology) had no vault home.
Code-confirmed: ESR_ISS_WNR_BIT==6 + is_write = !is_instruction && ((esr>>6)&1) at
fault.c:72/122.

WHAT THE DOC GOT WRONG: little -- as-built L-3b..L-5, largely current, even
self-corrects its own CLONE_VM/CLONE_FILES misdescription. The change is
distribution across 9 dossiers + the #137 lesson it uniquely held (now folded).
#136 + the vfork ABA + "a new park inherits every unsynchronised access after it"
carried by sub-kernel-addrspace / sub-kernel-proc.

Render clean; lint 0-fail. view-absorption 76 -> 77.
