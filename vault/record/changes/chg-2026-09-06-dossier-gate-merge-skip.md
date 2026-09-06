---
id: chg-2026-09-06-dossier-gate-merge-skip
type: chg
title: "dossier-gate: durable tracked MERGE_HEAD skip (mergeInProgress) + discrimination test -- a merge that pulls in audit:hard code no longer blocks; the hook's inline skip becomes belt-and-suspenders"
date: 2026-09-06
arc: arc-vault
commits: ["40cb19aa"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Follow-up to [[chg-2026-09-06-dossier-gate-code-reminder]] and
[[chg-2026-09-06-dossier-gate-hook-failopen]]. The third fail-open case, and the
durable form of a fix that shipped only in the local hook last run.

The merge-blindspot: a merge commit that brings in audit:hard-owned code (a code
track merging origin/main carrying a kernel/*.c or an audit:hard userspace
change) re-stages that code without the dossier co-staged or a trailer, so
`dossierGate` blocked it. This is the R6 merge-blindspot class reappearing in the
reminder -- and it is worse here than a nuisance, because aux is classifier-blocked
from `--no-verify`, so a blocking merge leaves that track with no clean way
through. A merge integrates already-committed, already-gated commits; it is not a
new authored change to the code it brings in, so the gate must fail open on it.

Last run this was patched only in the local, untracked commit-msg hook
(`git rev-parse -q --verify MERGE_HEAD && exit 0`). That copy dies on a hook
reinstall and was never tested. This change makes it durable:

- `mergeInProgress(root)` in `dossier_gate.go`, matching the hook's probe
  exactly (`git rev-parse -q --verify MERGE_HEAD`; gitOut maps the -q failure to
  empty), and `dossierGate` returns clean when it fires. Correct now even when
  quaestor is invoked directly, independent of the local hook.
- `TestDossierGateSkipsMergeInProgress` -- the discrimination partner of
  `TestDossierGateBlocksHardCode`: the EXACT same staged state (audit:hard
  kernel/t.c, no dossier, no trailer) with one variable added, MERGE_HEAD
  present, must go from block to clean. `writeMergeHead` simulates it via
  `--git-path` + a real object (HEAD). Sabotage-verified: disabling the skip
  flips this test to FAIL while the block test still passes (the one variable is
  the skip); restored.
- schema.md section 8: the third load-bearing fail-open case, documented beside
  the empty-registry and behind-main-worktree cases.

The local hook keeps the inline skip as belt-and-suspenders (it avoids spawning
`go run` on a merge) with its comment updated from "owed" to landed. Full
quaestor suite green (11 gate tests, incl. the new one); go vet clean.
