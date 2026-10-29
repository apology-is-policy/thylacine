---
id: seam-poll-srv-registry-retain
type: seam
title: "The KObj_Srv listener poll retain is inert — a mortal registry reintroduces the UAF"
status: closed
closed-by: chg-2026-10-01-srv-listener-retention
surface: [sub-kernel-poll]
opened-by: chg-2026-06-10-rw2-poll-retain
tracker: "RW-2 R2-poll F1 (#18)"
created: 2026-08-01
updated: 2026-10-01
---
## October 1 implementation checkpoint

Listener slots and `handle_get` snapshots now carry covering registry refs;
poll drops its snapshot after sweeping the embedded waiter list. The mortal
registry lifecycle/rollback witnesses pass in the full 1830-test boot and
reject three targeted source mutants. All 50 default/UBSan SMP boots pass,
with zero entries in every failure category, including timing exceptions.
The historical diagnosis below describes the pre-fix implementation. Registry
capacity, per-session activation, poster-exit routing and fairness remain open;
this prerequisite must not be reported as the complete registry repair.

## Historical diagnosis

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

## Required lift (now implemented)

Take a real `srv_registry_ref` at register and drop it post-sweep, or
thread a registry ref through `held[]`. Must land IN the chunk that
makes any registry mortal — the inertness is invisible until then.
