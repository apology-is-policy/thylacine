---
id: adt-stalk2-r1
type: adt
title: "stalk-2 (mount re-key + domount crossing) round 1"
date: 2026-06-02
scope: [sub-kernel-stalk]
reviewer: opus
model-start: "claude-opus-4"
model-end: "claude-opus-4"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 2}
findings: [fnd-stalk2-r1-f1, fnd-stalk2-r1-f2, fnd-stalk2-r1-f3]
round-of: chg-2026-06-02-stalk2
created: 2026-08-01
---
## Scope

The `(dc, devno, qid.path)` mount-table re-key + `cross_mounts` +
`STALK_MOUNT` + path-keyed SYS_MOUNT/UNMOUNT (commits `e291b74d` +
`c185186c`). Background Opus prosecutor (session agent `a1d95b43`) +
an in-session self-audit, merged.

## Convergence

CONVERGED on SOUND for the crux — lifetime across the cross (the
base-cross push, the in-place descent replacement clunking exactly the
old owned entry, the quarry-cross swap, `cross_mounts` never clunking
the borrowed table source, `clone_walk_zero`'s detach-on-failure) — and
for the mount-key correctness (devno minted per session, inherited by
clone; `devno_disambiguates` proving two same-`(dc,qid)` instances
distinct), the STALK_MOUNT/MREPL re-key, the crossed-root-governs
X-ordering, and MountRefcountConsistency across the 32-byte entry. The
one P2 (F1) falsified the design's "cycle-free by construction" claim —
a self-mount reachable in ONE t_mount call — fixed by ENFORCING I-3 at
mount() (`would_create_mount_cycle`); NOT a dirty close (a localized
additive guard mirroring bind's). The lock-free mount-table read was
carried as the same inherited class as stalk-1 F3 (both later closed:
#844 the handle half, RW-4 SA-F1 the table half). Matrix: 706/706 all
configs + both joey E2Es + the migrated mount-cycle probes.
