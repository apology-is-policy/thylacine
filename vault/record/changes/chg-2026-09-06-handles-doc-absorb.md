---
id: chg-2026-09-06-handles-doc-absorb
type: chg
title: "absorb docs/reference/19-handles (P2-Fc handle table): comprehensively superseded by sub-kernel-handle (which is more current); zero-fold multi-redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["da27b73f"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The P2-Fc handle-table doc. Zero-fold: `sub-kernel-handle` (audit:hard, updated
2026-08-15) not only covers every current atom but is MORE current and MORE
complete than the reference doc -- verified atom-by-atom.

Where the dossier is ahead of 19-handles: `PROC_HANDLE_MAX = 1024` (the doc says
64; the dossier documents the 64->256->1024 drift and that poll.h/syscall.h still
lag); the FOUR-way kind partition (Transferable/HW/SRV/Loom +PCI, seven asserts)
vs the doc's three; the four duplication primitives + the by-value handle_get/put
snapshot vs the doc's lone handle_dup; the per-table spinlock (#844) vs the doc's
"single-CPU, no lock, lock at Phase 5+"; fork-copy; close-on-exec; handle_init now
a no-op (kmalloc-backed, not a SLUB cache).

Redirect: sub-kernel-handle (the table + kinds + dup + lock + I-4/5/6) +
abi-handle-rights (the RIGHT_* values, RIGHT_ALL=0x3f) + sub-kernel-caps (I-2) +
39-hw-handles (already absorbed; the hw-creation surface). The stub names the
doc's stale axes, including its own internal contradiction (KOBJ_KIND_COUNT==10
in the enum but ==9 left in two spec-mapping rows).

No dossier content changed. Render clean; lint 0-fail. view-absorption 69 -> 70.
