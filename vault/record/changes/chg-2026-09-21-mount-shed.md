---
id: chg-2026-09-21-mount-shed
type: chg
title: "The mount-table shed at pivot / chroot (#80): the cap stays 32 and the orphans go"
date: 2026-09-21
arc: arc-boosty
commits: ["25df504f", "2a737959"]
touched: [sub-kernel-territory, sub-kernel-stalk, sub-kernel-spoor, sub-kernel-dev, sub-kernel-content, sub-kernel-joey, sub-stratum-boot, seam-80-pivot-orphan-mounts]
established: []
closed: [fnd-shed-r1-f1, fnd-shed-r1-f2, fnd-shed-r2-f1, fnd-shed-r2-f2, fnd-b0self-r1-f1, fnd-b0self-r1-f2]
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-21
---
**What.** `territory_pivot_root` / `territory_chroot` drop, in the same
`ns_lock` hold as the root swap, every mount entry whose mount point lies
in a device instance unreachable from the new root (ARCH 9.6.10;
`specs/territory_shed.tla`). A login session went from 23 entries to 17,
and `viv run` -- broken on `main` because 23 + 10 no longer fit 32 -- works
again. Closes [[seam-80-pivot-orphan-mounts]].

**Why.** `unmount` takes a RESOLVED mount point, so an entry keyed in the
old tree can never be named again: it cannot be removed, it is deep-copied
into every child, and its slot is lost for the life of the namespace. The
cap had been raised four times (8 -> 12 -> 16 -> 20 -> 32) for this one
cause. The operator's decision (2026-09-21): design the real fix, keep 32.

**Alternatives rejected.** Raising the cap a fifth time. A per-directory
reachability walk (the kernel cannot see inside a 9P tree; the rule is per
device instance and conservative instead). Prior art, none of which
transfers whole: Plan 9 has no pivot and an unbounded table; Linux sheds
the old generation with `umount2(put_old, MNT_DETACH)`, possible because
its mounts form a tree; Fuchsia / Genode construct a namespace per
component -- recorded as the follow-on for containers.

**Verification.** Round 1 of the audit ([[adt-shed-r1]]) found the rule
unsound for a UNION root and the spec unable to fail at all; both fixed
before landing, the spec rewritten around an operational walker (744,864
and 793,408 distinct states; five buggy cfgs; three closure sabotages each
fail; `specs/check-territory-shed.sh` pins the counts and the NAMED
invariant per cfg). Round 2 ([[adt-shed-r2]]) found the resolver's other
reading of a union handle's point: a DISSOLVED union handed out the
directory it had covered. The rule -- a dissolved union degrades to
member[0], never to the covered directory -- went into ARCH 9.6.10, and its
first implementation was wrong for an opened handle and was caught by its own
kernel test ([[adt-b0self-r1]]), which also turned up a `..` that popped a
crossed base. 12 `territory.shed_*` kernel tests plus
`stalk.union_dissolved_degrades` and `stalk.dotdot_crossed_base_floor`,
deny-path legs for the pivot / chroot directory gate and two union child
stages in `usr/symlink-probe`, the ci fleet with `viv-run` and `r5f9-ash`
green, the SMP gate. Closed list:
`memory/audit_territory_shed_closed_list.md`.
