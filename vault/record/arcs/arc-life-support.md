---
id: arc-life-support
type: arc
title: "Life Support (LS-1..LS-8 + LS-K): making the system usable from a console"
status: active
design: ["docs/LIFE-SUPPORT.md"]
chunks:
  - chg-2026-06-09-ls4-cwd
  - chg-2026-06-10-ls5c-widen
follow-ons: []
created: 2026-08-01
---
## Goal

The arc that closed the gap between "boots and passes its tests" and
"a person can sit at the console and use it". Each LS chunk is a thing
whose absence made the shell unusable rather than a thing whose presence
was novel: a working `cd`, a real Ctrl-C, a pollable console with a line
discipline, a TUI substrate, a clock that says what time it is.

The chunks are small and the invariant load is uneven — some (LS-5's
`interrupt` note, LS-8's deferred poll-wake) sit squarely on the death
and wait/wake lineage and were prosecuted hard; others (LS-4's cwd) were
chosen SPECIFICALLY to add no new mechanism to a security-critical
resolver.

## Planned chunks

Recorded so far — the vault has swept only the territory-facing half.
The console chunks (LS-5 `interrupt`, LS-7 Kaua, LS-8 the pollable
console + termios) join at the cons sweep; LS-K (the wall clock) at the
timer sweep.

- [[chg-2026-06-09-ls4-cwd]] — the per-Proc cwd (`SYS_CHDIR`/`SYS_GETCWD`).

## Close summary

(pending)
