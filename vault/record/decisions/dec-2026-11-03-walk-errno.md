---
id: dec-2026-11-03-walk-errno
type: dec
title: "A failed walk says why: only a miss reads ENOENT"
date: 2026-11-03
status: standing
decided-by: user-vote
affects: [sub-kernel-stalk, sub-kernel-dev, sub-kernel-ninep-dev9p, sub-kernel-syscall-dispatch, sub-kernel-exec, sub-pouch-process]
created: 2026-11-03
---
## Fork

Every failure of a 9P walk read `ENOENT`, because `Dev.walk` and `Dev.walk_attrs` answer NULL with no errno and the resolver assumed a miss. A Stratum integrity `EIO` on a lookup said the file did not exist, and a program could recreate it.

Options: add the walk to ER-2 in its own chunk, so a server's errno crosses a failed walk as it does rename and unlink; or leave `ENOENT`.

## Decision

Operator vote 23, 2026-11-03, on the recommendation, which is the signoff ERRORS.md needs: the walk joins ER-2. The mechanism was left to the implementer: a per-thread slot read through a wrapper, not an out-param on the vtable. Only dev9p has a reason; an out-param would touch every Dev and about 115 test call sites and churn every branch that adds a Dev; `c` can be shared and the query form has no `nc`, so per-Spoor cannot carry it; a caller that skips the wrapper keeps today's miss, which fails safe.

## Consequences

A spawn and a Linux `execve` and `chdir` report the reason too; the four older spawn calls keep a flat `-1`, which reads as unknown rather than as a miss. A program that read `ENOENT` for a dead session or a server's own refusal now reads that. Pouch's `posix_spawnp` search goes on past `ENOENT`, `ENOTDIR` and `EACCES` only, as upstream's does.
