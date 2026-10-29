---
id: dec-2026-10-28-ut-unset-env
type: dec
title: "ut's unset also removes the name from the shell's own /env"
date: 2026-10-28
status: standing
decided-by: user-vote
affects: [sub-utopia-eval, sub-kernel-content]
created: 2026-10-29
---
## Fork

Under [[dec-2026-10-22-env-bind-at-open-sequenced]] the `/env` directory
is always the caller's: an unlink there removes the name from the
remover's own environment. So `rm /env/NAME`, typed at ut, removes NAME
from the copy rm received at spawn and leaves ut's untouched. The
bind-at-open draft of the manual promised the opposite, and the ls-3b leg
that asserted it failed on every run. ut's `unset` builtin removed the
name from the script's scope frames only, never from ut's `/env`.

## Research

- **Plan 9 (heritage).** rc empties a variable with `x=()`. rc runs in
  the Proc whose environment group it changes, so no child is involved.
- **POSIX sh.** `unset` removes a variable from the shell and from the
  environment later children inherit.
- **The tree.** utopia keeps script variables in scope frames
  (`eval/env.rs`) beside the process `/env`, and `bi_unset`
  (`eval/builtin.rs`) already existed for the frames. The vote's ask said
  ut keeps no variable table; that premise was wrong, and the
  implementation extends the existing builtin rather than adding one.

## Options

1. **A builtin `unset`** that also removes NAME from ut's own `/env`
   (recommended).
2. Document that ut has no unset: `echo -n > /env/NAME` empties a value
   but leaves the name.

## The call

Option 1 (operator vote 31, 2026-10-28). It ships with a manual section
(21-environment: `unset NAME...` and why `rm` does not do it) and an
ls-3b leg that asserts both halves: a spawned `rm` leaves the shell's
variable, and `unset` removes it.

## Rationale

Only code running in the shell's own Proc can remove a name from the
shell's environment, and a builtin is that code. The builtin refuses an
empty name, a name with a `/`, and `.` or `..` ("invalid variable name"),
before it touches anything. A name that cannot be an `/env` component
(an interior NUL, or longer than `T_WALK_OPEN_NAME_MAX`) leaves the
frames only. A name absent from `/env` is not a failure. The first
failure sets `$errstr` and status 1, and the rest of the names are still
removed.
