---
id: chg-2026-09-06-dossier-gate-hook-failopen
type: chg
title: "dossier-gate commit-msg hook: fail OPEN on a behind-main worktree (aux 0062) -- an older quaestor with no dossier-gate subcommand exited 2 and exec-bricked the commit; the hook now no-ops when the worktree lacks dossier_gate.go"
date: 2026-09-06
arc: arc-vault
commits: ["6fcbbd40"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Follow-up to [[chg-2026-09-06-dossier-gate-code-reminder]]. aux flagged (call
0062) that the shared commit-msg hook shipped fail-CLOSED for behind-main
worktrees -- the exact failure the TTrace lesson warns of.

Root cause: the hook builds quaestor from the COMMITTING worktree's source. A
worktree behind 292a1f9c (aux-3) has a quaestor with no `dossier-gate`
subcommand, so `go run . dossier-gate` hits quaestor's usage/default arm and
exits 2; the hook `exec`'d that, so the commit aborted. The fail-open guard
covered a missing `go` toolchain, not an old quaestor. The deeper error was in
last session's verification: I confirmed `vault/` EXISTS on main (1147 files)
and concluded every worktree could run the gate -- but presence of the DIRECTORY
is not presence of the SUBCOMMAND in that worktree's HEAD.

Fix (in the local, untracked hook; documented here + schema.md section 8): the
hook no-ops when the committing worktree lacks
`vault/meta/quaestor/dossier_gate.go` -- the feature's own source, checked before
any `go` run so a behind worktree pays zero cost -- plus an `exit 2` tolerance as
defense-in-depth for a half-merged tree. Chosen over aux's usage-grep probe
because the file-check costs nothing for a behind worktree (the probe would
double-compile quaestor every commit), and build-error fail-open is already
covered upstream by pre-commit (it runs quaestor first, so commit-msg only sees a
quaestor that compiles). The gate activates for a worktree once it carries the
feature -- correct: a worktree is gated by its own tooling version.

Verified both directions: the fixed hook run from aux-3's worktree no-ops in
0.02 s (no go run); from up-to-date vault it still BLOCKS without a trailer and
ESCAPES with one. aux unblocked to land N-2c. schema.md section 8 updated to
document the two fail-open cases.
