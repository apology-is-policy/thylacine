---
id: adt-b1c-r4
type: adt
title: "B-1c (the native heap) round 4: clean -- the capture's edges and the #54 docs' reach"
date: 2026-09-24
scope: [sub-coreutils-lib, sub-coreutils-filters, sub-coreutils-presenters]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 9}
findings: []
round-of: chg-2026-09-24-b1c-round4-close
prior-round: adt-b1c-r3
created: 2026-09-24
---
## Scope

Branch `b1c-native-heap` at 7c54ef71 (WIP 12): round 3's fixes, `git diff
b409f80b 7c54ef71` -- the smoke's `converse` capture and every helper and check
built on it, `stream::firsts` and `stream::Skip` with `runs_within`'s `begin`
hook, `uniq`'s two modes, `tail`'s parse and `+N`, grep's stopped() comment,
and the round-3 close's docs and records, with the two round-2 finding notes as
edited for the squash. The round ran on Opus 5.5 at max effort, the fallback
tier (Fable was out of credits at spawn), told that context independence was
what it brought and to re-derive each claim from the code.

## Convergence

0 P0 / 0 P1 / 0 P2 / 9 P3; model-start equals model-end. Clean by count, and
its fixes are local, so no round 5 follows. The round verified `converse` sound
on every path it was asked about: bounded everywhere, non-blocking on the
parent's write end only, partial writes and EPIPE handled, no output lost to an
exit between the poll and the read, a tool cut off at the bound killed before
its pipes drop and reported as cut off, a tool that finished reported by its
own code, and every child reaped or killed. It verified `firsts` and `Skip`
against the whole-input answer, `uniq`'s modes against GNU's condition,
`tail`'s `+0` and `-n 0` against GNU's, and #54 on the new paths. Four P3s were
harness edges no boot reaches today: a busy poll when a pipe has less room than
a short write needs (F4; the kernel reports POLLOUT at one free byte), a
one-second bound that counted `yes`'s exec setup (F5), a failed kill followed
by a wait with no bound (F6), and a spawn error whose cause was dropped (F9).
One was behaviour, pre-existing: `tail` refused POSIX's `-n -N`, and `tail` and
`head` read `--` as a file (F8). The rest were the docs' reach: the presenters'
#54 paragraph was false for the network tools, `nc` and `con` treating a gone
reader as a failure (F1); the filters' exceptions missed `seq` and `yes` (F2);
round 3's finding note claimed a regression its check could not give (F3); and
four places still described the replaced capture, one sentence the fixed heap
(F7). The main session's self-audit found the same `--` gap (SA4-1) and an
ARCH 6.5 sentence that bounded every line by `LINE_MAX` where only a held line
is (SA4-2). All are fixed before landing. The verbatim report and dispositions
are the repo's untracked `memory/audit_b1c_closed_list.md`.
