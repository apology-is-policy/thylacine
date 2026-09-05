---
id: seam-srv-registry-lifecycle
type: seam
title: "The /srv registry entry lifecycle: tombstones never free; one shared boot registry"
status: open
surface: [sub-kernel-devsrv]
opened-by: chg-2026-05-19-srv-birth
created: 2026-07-31
updated: 2026-07-31
---
## What is owed

The v1.x registry-entry lifecycle: **entry-free-at-last-handle-ref**
(or the per-session registry the A-5b design note intended but which was
never built — as-built, every login session `territory_clone`s joey's
`/srv` mount and shares the ONE boot registry). Today a dead poster's
entry pins its name + slot FOREVER (the stale-handle defense), which is
what forced `SRV_MAX_SERVICES` 8 → 16 when per-user `home-<user>`
tombstones accumulated across sessions and made the registry exactly
full at the login prompt (#30 — michael's rebind needed no free slot
while any OTHER user's fresh post found none: an at-capacity asymmetry
now pinned by `devsrv.registry_full_tombstone_rebinds`).

## What closes it

One chunk that retires tombstone accumulation AND the cross-session name
visibility together: either the ref-counted entry lifecycle (free at the
last KObj_Srv listener ref once no rebind-authority claim is live) or
per-session registries mounted at login. Whichever lands must discharge
the **mortal-registry ordering obligation** recorded at
`srv_registry_unref` ([[fnd-stalk3a-r1-f2]]): a raw `SrvService *` /
listener-handle obj points INTO `reg->entries[]` with no registry ref,
so a mortal registry's last unref must be ordered after every
listener/connection handle into it closes (the poster group-terminates
first, #811), or those holders must carry a covering registry ref. The
per-registry fairness caps deferred at [[fnd-stalk3b-r1-f3]] land in the
same pass.

## Risk while open

Bounded and known: name-slot exhaustion is a boot-fatal regression
(the second-user login E2E guards it); cross-session NAME visibility
(any session sees `/srv/home-<other>`) is real but data-inert — the
per-user proxy is `--single-session` and dataset scope + the per-user
DEK gate the bytes. A further `SRV_MAX_SERVICES` raise costs
`srv_registry_drain` kernel stack (~2 KiB at 16) — raise only with the
real fix.
