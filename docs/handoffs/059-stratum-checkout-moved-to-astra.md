# 059 -- the shared Stratum checkout moved; your worktree's link was repaired (to Astra)

From main, 2026-11-15, operator votes 90, 128 and 130.

## What happened

Stratum's tree now sits at the root of its repository: v1, the earlier
prototype, is gone and v2 moved up (Stratum main `f3fbf375b`). The shared
checkout was set aside, not pulled, so nothing in it was lost:

- `~/projects/stratum-pre-tidy` is the old checkout, moved there whole at
  2026-11-15T06:35Z: its `main` at `471a339`, its uncommitted work and its
  untracked build dirs exactly as they were. It is still the repository your
  worktree belongs to.
- `~/projects/stratum` is a fresh clone of Stratum main `f3fbf375b`, with
  the tree at the root (no `v2/`).

## What changed in your worktree

Only its one-line `.git` file. `~/projects/stratum-astra/.git` named the old
place of the repository (`~/projects/stratum/.git/worktrees/stratum-astra`),
which the move took away; `git -C ~/projects/stratum-pre-tidy worktree repair
~/projects/stratum-astra` rewrote it to
`~/projects/stratum-pre-tidy/.git/worktrees/stratum-astra`. Your branch
`codex/astra-session-dek` is where you left it (`7dd103d`), and no file of
yours was touched.

## If you carry on there

Your branch has the old layout (the tree under `v2/`). Thylacine's
`tools/build.sh` reads `~/projects/stratum`, or `STRATUM_SRC`, and once the
change waiting on branch `stratum-src-b` lands it refuses a checkout with the
old layout, naming the pull that updates it. To build from your branch, merge
or rebase it onto Stratum main `f3fbf375b` first (git follows the `v2/` ->
root renames). To keep working beside the new clone instead, push your branch
and add a worktree from `~/projects/stratum`; the old one can stay attached to
`~/projects/stratum-pre-tidy` until you no longer need it.
