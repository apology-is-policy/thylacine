---
id: fnd-stube2-r1-f2
type: fnd
title: "Walked-Spoor UAF at Proc exit — the deferred hazard came due"
round: adt-stube2-r1
severity: P1
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-05-21-p5-chroot
regression: "p9_attached.walked_outlives_root_no_uaf (walk, then close the root BEFORE the walked)"
created: 2026-08-01
---
## Prosecution

`dev9p_walk` copied the parent's `client` pointer into the walked
Spoor's priv with NO refcount — a hazard the header had DOCUMENTED
("priv's `client` field is not refcounted at v1.0") and a prior review
series had DEFERRED to "the SYS_WALK chunk", which is this one.

The reproduction needs no race, only ordering: `SYS_ATTACH_9P` ->
`SYS_CHROOT` -> `SYS_WALK_OPEN(FROM_ROOT)` -> `t_exits(0)`. `proc_free`
runs `territory_unref` and then walks the handle table in ASCENDING
index order, so the attach fd (lower) closes first — `dev9p_close(root)`
-> `p9_attached_destroy` -> `kfree(client)`. The walked fd's close then
calls `p9_client_clunk(stale_client, fid)`, reading freed memory: the
magic word at best, and on a recycled SLUB slot the lock, session, or
transport — cross-session confusion at worst.

## Disposition

Fixed by refcounting `p9_attached`: a `ref` plus ownership of the
adapter and transport Spoors, `p9_attached_ref`/`unref`/
`install_transport`, and EVERY priv — root and every walked — holding
one. `priv_alloc` takes an owner and bumps; `dev9p_walk` propagates it;
`dev9p_close` uniformly clunks the fid then unrefs, with the last unref
running the full teardown in an order that keeps `ops.close` before the
adapter free.

The lesson is the deferral. The hazard was correctly identified, written
down, and postponed to the chunk that would make it reachable — and it
was still a P1 when that chunk arrived, because "documented" is not
"contained". What made it come due was ordinary feature work: the first
syscall that let a walked Spoor outlive its root.
