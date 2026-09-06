---
id: chg-2026-09-06-joey-boot-borrowed
type: chg
title: "sub-stratum-boot re-verified after the KT-1.5 + VIVARIUM merge: the bringup sequence is borrowed; the ~5659-line churn is all in the undescribed region (#177 gap widened)"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-stratum-boot
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
joey.c moved ~5659 lines since the dossier's 2026-09-02 update (it is 11578 lines
now, was 9771 at batch 35), so `sub-stratum-boot` was flagged stale. The dossier
describes only the BRINGUP SEQUENCE -- spawn stratumd, the readiness handshake,
the attach, the pivot, the re-grafts, the service-post decision -- and on that
surface the churn is BORROWED.

## The bringup sequence did not move

Verified against the tree, not assumed:

- The core bringup tokens have ZERO hits in the full `4bb6048c..HEAD` joey.c diff:
  the readiness token `"bound and ready"`, `SYS_ATTACH_9P_SRV`, `SYS_PIVOT_ROOT`,
  `T_ATTACH_9P_LOOSE`, `--fs-workers`, and the stratumd spawn args.
- `git log -L` on the `SYS_PIVOT_ROOT` line (joey.c 7009) returns `457f22d9`
  (2026-05-26, the original 16c-integration) -- the pivot has not moved in over
  three months, well before this dossier.
- The two direct post-09-02 commits touched only undescribed functions:
  `739f6cb7` (KT-1.5d-1a) edited `login_e2e_run`, `90d95d08` (KT-1.5a) added a
  boot-prove to `main`. The MREPL / MAY_POST_SERVICE / drain hits in the diff are
  the VIVARIUM `/viv/bin` graft and the viv/kaua/git container gates -- not the
  bringup's re-graft or stratumd spawn.

## The growth is all in the region the dossier already disclaims

The dossier's own caveat records that it owns only the bringup few-hundred lines
and that joey.c's other eight-ninths (the daemon registry, the orphan reaper, the
identity-daemon bringup, the smoke suite, the exec/fork/foreign-shell gates, the
login/recovery E2E, the session getty loop, the toolchain/GL gates, the numbered
probes) are undescribed anywhere in the vault (task #177). The +1807 lines since
landed in exactly that region: the KT-1.5d-1a login-spawned per-user
`halcyond --session` bootstrap (the getty loop), a kaua-term transport boot-prove,
and the `/viv/bin` union-graft gates. So the currency action is measurement, not
new bringup prose: the caveat's line/function count refreshed (9771/~50 ->
11578/53) and the #177 gap noted as wider, not narrower. `updated:` -> 2026-09-06;
guarded-by unchanged [inv-i28, inv-i45]. Expanding the dossier to actually cover
init's other jobs remains #177 -- a build, not a de-stale.
