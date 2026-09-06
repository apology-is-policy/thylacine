---
id: chg-2026-09-06-chroot-doc-absorb
type: chg
title: "absorb docs/reference/77-sys-chroot (SYS_CHROOT territory-root pivot): fold the one-way lifetime caveat into sub-kernel-territory, redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["26b3b9c3"]
touched: [sub-kernel-territory]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The v1.0 territory-root pivot syscall (SYS_CHROOT=35). sub-kernel-territory
(audit:hard, owns territory.c) LAPS the doc; verified atom-by-atom.

ALREADY COVERED:
- territory_chroot mechanism: bump-before-swap (spoor_ref extincts on corrupted
  source), spoor_clunk-not-unref on the displaced root, idempotent same-pointer,
  the chroot-vs-pivot precondition split -> sub-kernel-territory (which ALSO
  covers SYS_PIVOT_ROOT, the v1.x successor the doc only sketches).
- The clone ref-copy + final-release drop + MountRefcountConsistency (root_spoor
  is a term) + the five-matched-refcount-sites discipline -> sub-kernel-territory.
- The FROM_ROOT walk companion (the -1 sentinel resolves root_spoor without a
  fresh ref) + the KOBJ_SPOOR/RIGHT_READ handler gate -> the dossier + dispatch.
- The spec Chroot(p,s) + territory_buggy_chroot_no_refbump -> spec-territory.

THE FOLD (the atom that lived only in the doc):
The one-way-chroot LIFETIME caveat. A chroot is one-way at v1.0 (no unchroot /
chroot(NULL)), so the ref it takes on the root Spoor is held for the Proc's WHOLE
LIFE. The caller-discipline consequence: a persistent Proc that chroots to a
mounted Spoor pins it (and the 9P session behind it) forever, which is why the
long-running init exercises chroot only through short-lived child probes that
release it on exit -- never in its own persistent context, where the pin would
wedge a teardown waiting for the session's EOF. Folded one sentence into the
dossier's chroot mechanism (the ref-discipline paragraph).

NOT REFUTED: the doc is current; the gap was the one-way lifetime consequence.
Zero code change. Redirect stub.
