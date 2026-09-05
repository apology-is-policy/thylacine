---
id: fnd-rw4-sa-f1
type: fnd
title: "RW-4 SA-F1: the per-Territory namespace tables had no lock"
round: adt-rw4-r1
severity: P1
status: fixed
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-06-10-rw4-fixes
regression: "territory_mount.lookup_ref_survives_unmount + territory_mount.root_ref_survives_pivot"
created: 2026-08-01
---
## Prosecution

`Territory.mounts[]`/`nmounts`/`binds[]`/`root_spoor` had NO lock (only
`dot_lock`, LS-4, for the cwd string). Multi-thread Procs share the
Territory (RFNAMEG too), and SYS_MOUNT/CHROOT/PIVOT_ROOT carry no cap
gate — so an unprivileged pthread program racing `open(FROM_ROOT)`
against chroot/pivot/unmount is a `root_spoor`/mount-source UAF plus a
torn `mounts[]` RMW. The #844 `spoor_ref(root)` sat INSIDE the
read-then-ref window. This is the #848 race PROMOTED from P3-dormant to
P1 by the P6 multi-thread lift; the Fable R1 reviewer rated it "still
dormant" (no in-tree Proc both walks and mutates its namespace) and was
OVERRULED — the kernel must be sound against any EL0 program.

## Disposition

Fixed at `6cf5933c`: a per-Territory `ns_lock` (mount/unmount/bind/
unbind/chroot/pivot/clone take it); `mount_lookup`'s contract goes
borrow→OWNED (lookup + ref atomic under the lock; NEVER held across
`clone_walk_zero` or `spoor_clunk` — displaced sources clunk outside);
the new `territory_root_ref` gives the six FROM_ROOT readers an atomic
read+ref. Closes [[seam-848-pivot-walk-race]]. The self-audit found it;
the formal reviewer confirmed the FACTS and disputed the disposition —
the overrule is the recorded lesson (the latent-P1 trap). The standing
rule in [[sub-kernel-stalk]] Concurrency: any new `mounts[]`/
`root_spoor` reader goes through `mount_lookup`/`territory_root_ref`,
never a bare read. The lock's own dossier home pends the territory
sweep.
