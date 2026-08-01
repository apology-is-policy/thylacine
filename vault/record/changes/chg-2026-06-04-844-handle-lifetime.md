---
id: chg-2026-06-04-844-handle-lifetime
type: chg
title: "#844: the handle-lifetime pass — snapshot handle_get + ref-held lookups"
date: 2026-06-04
arc: arc-identity-detour
commits: ["8acdcce9", "c1c2948f", "252cb91d"]
touched:
  - sub-kernel-stalk
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The surface-wide `handle_get` TOCTOU close (the Plan 9 `fdtochan`
shape): a per-Proc `HandleTable.lock`; `handle_get` returns a BY-VALUE
snapshot with the obj refcount HELD under the lock, paired with
`handle_put` dropping it OUTSIDE (release may sleep); the KOBJ_SPOOR
helpers (`sys_lookup_spoor` et al.) TRANSFER a ref the caller clunks;
~20 call sites converted. The #847 per-Burrow lock is the precursor
(`8acdcce9`). For the resolver surface this is the chunk that closed
stalk-1 F3 ([[fnd-stalk1-r1-f3]]): `SYS_OPEN`'s borrowed `start` is now
pinned by the caller's real ref across up to 40 blocking walks (the
in-commit E2E caught the initial `start` leak). The audit close
(`252cb91d`) locked `handle_alloc` (F1 [P1] — the primary fd-creating
path had been left unlocked) + hoisted the srv_peer cn reads (F2 [P2]).
The handle-surface adt + findings pend that surface's sweep; the F3
pivot-vs-walk residue was tracked as #848 and later closed by
[[chg-2026-06-10-rw4-fixes]].
