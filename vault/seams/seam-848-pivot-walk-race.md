---
id: seam-848-pivot-walk-race
type: seam
title: "SYS_PIVOT_ROOT vs concurrent multi-thread walk from root_spoor"
status: open
surface: [sub-kernel-ninep-attach]
opened-by: fnd-16c-r1-f6
tracker: "task #848"
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: a Territory-lock (or equivalent) fix for the torn-pointer /
freed-Spoor read when `territory_pivot_root` (or `territory_chroot`, the
inherited pattern) swaps `root_spoor` while a peer Thread of the same
Proc resolves from it. Deferred at the 16c round to the multi-thread
carve-out; re-confirmed dormant at the #844 handle-lifetime pass (its F3
tracked it as #848).

**Why open is tolerable**: dormant at v1.0 — joey (the only pivoting
Proc) performs the pivot single-threaded during bringup.

**What closes it**: the territory-surface fix at its sweep (this seam
re-homes to the territory dossier then — it is filed here because the
pivot syscall landed in the 16c attach chunk and this surface's auditor
must see it).

**Risk while open**: a multi-threaded Proc that pivots while a sibling
walks could UAF — no such program exists in-tree; the kernel must
eventually be sound against it (the latent-P1 trap class).
