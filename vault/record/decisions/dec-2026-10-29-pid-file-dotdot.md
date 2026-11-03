---
id: dec-2026-10-29-pid-file-dotdot
type: dec
title: "`..` from a pid file goes to its pid directory"
date: 2026-10-29
status: standing
decided-by: user-vote
affects: [sub-kernel-devproc]
created: 2026-10-29
---
## Fork

devproc's `..` from a file inside `/proc/<pid>/` returned the root. With a
nested `thread/<tid>/` level, `..` had to be defined one level up from every kind;
the question was whether a pid file's `..` keeps going to the root (old
behaviour) or to its pid directory.

## Decision

Operator vote V3 (2026-10-29 ~08:37Z): one level up from every kind -- a pid
file to its pid directory, a tid file to its tid directory, a tid directory to
`thread/`, `thread/` to the pid directory ([[sub-kernel-devproc]]).
