---
id: seam-poll-srv-registry-retain
type: seam
title: "The KObj_Srv listener poll retain is inert — a mortal registry reintroduces the UAF"
status: open
surface: [sub-kernel-poll]
opened-by: chg-2026-06-10-rw2-poll-retain
tracker: "RW-2 R2-poll F1 (#18)"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

The RW-2 retain fix holds a `handle_get` obj ref for every registered
waiter — but `handle_acquire_obj`/`handle_release_obj` are NO-OPS
for `KObj_Srv`, so the `held[]` entry for a listener poll
(`svc_listener_poll` → `svc->poll_list`) pins nothing. Listener-poll
lifetime is safe ONLY because the sole registry today is the immortal
boot registry (SrvService entries tombstone, never free).

A mortal per-session registry (the A-5b/#827 direction —
`srv_registry_unref`'s `kfree(reg)` already exists) revives the
round-1 UAF on exactly this path: registry freed mid-sleep, the
sweep spin-locks freed memory.

## The lift

Take a real `srv_registry_ref` at register and drop it post-sweep, or
thread a registry ref through `held[]`. Must land IN the chunk that
makes any registry mortal — the inertness is invisible until then.
