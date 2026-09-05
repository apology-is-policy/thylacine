---
id: seam-848-pivot-walk-race
type: seam
title: "SYS_PIVOT_ROOT vs concurrent multi-thread walk from root_spoor"
status: closed
surface: [sub-kernel-ninep-attach]
opened-by: fnd-16c-r1-f6
closed-by: chg-2026-06-10-rw4-fixes
tracker: "task #848"
created: 2026-07-31
updated: 2026-08-01
---
**Owed (was)**: a Territory-lock (or equivalent) fix for the torn-pointer
/ freed-Spoor read when `territory_pivot_root` (or `territory_chroot`,
the inherited pattern) swaps `root_spoor` while a peer Thread of the same
Proc resolves from it. Deferred at the 16c round to the multi-thread
carve-out; re-confirmed dormant at the #844 handle-lifetime pass (its F3
tracked it as #848).

**What closed it**: [[chg-2026-06-10-rw4-fixes]] — RW-4's SA-F1
([[fnd-rw4-sa-f1]]) took the whole namespace-table surface under a new
per-Territory `ns_lock`: `territory_pivot_root` now reads-old + refs-new
+ swaps under the lock (the displaced root's `spoor_clunk` deferred
OUTSIDE it, since a Dev close hook may sleep), and the new
`territory_root_ref` gives every FROM_ROOT reader an atomic read+ref
under the same lock — so the read-then-ref window this seam named no
longer exists. Regression `territory_mount.root_ref_survives_pivot`.

**Correction note (stalk sweep, 2026-08-01)**: this seam was minted at
the 9P-area sweep from the #844 closed list, which recorded #848 as
dormant-and-open — but RW-4 had already closed it six days after that
list was written. Reading the CURRENT `territory.c` is what caught it;
the surface's own dossier ([[sub-kernel-stalk]] Concurrency) now carries
the standing rule the fix created: no bare `mounts[]`/`root_spoor` read
— go through `mount_lookup` / `territory_root_ref`.

**Risk while open (was)**: a multi-threaded Proc that pivots while a
sibling walks could UAF — no such program existed in-tree; the kernel
must be sound against it regardless (the latent-P1 trap class, which is
exactly how RW-4 dispositioned it: the reviewer's "still dormant" was
OVERRULED to P1).
