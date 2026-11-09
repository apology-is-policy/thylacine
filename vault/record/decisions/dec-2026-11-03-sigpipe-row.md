---
id: dec-2026-11-03-sigpipe-row
type: dec
title: "Pouch's SIGPIPE default is a row that ignores, not a mask bit"
date: 2026-11-03
status: standing
decided-by: user-vote
affects: [sub-pouch-signal]
created: 2026-11-03
---
## Fork

POUCH-DESIGN 6.4 [RESOLVED 6.4] had Pouch mask SIGPIPE at startup, so a write to a closed pipe
answers EPIPE with no death. The one restore at a handler's return (vote 24) undoes a mask bit set
in a handler, so `signal(SIGPIPE, SIG_DFL)` there left a default row and a clear bit, and the next
such write terminated ([[fnd-note-wait-r1-f6]]).

Options: document it, as a limit of the emulation; or carry the default in a row.

## Decision

Vote 25, the operator, on the recommendation: "With F6 I will go with your recommendation." The
default is a row that ignores. `SIG_DFL` and `SIG_IGN` write the same row, a real handler a caught
one, and the mask bit goes.

## Why

A row is per Proc and outlasts a handler, so the same change closes
[[seam-pouch-sigmask-per-thread]] and the old lie that `sigprocmask` read SIGPIPE blocked in a
program that never blocked it. The observable default, EPIPE and no death, does not change.

## What it costs

Every Pouch image allocates a table at startup. `raise` and `kill` of SIGPIPE at its default do
nothing, and a `sigwait` on it blocked never returns, because the note is dropped at its post.
