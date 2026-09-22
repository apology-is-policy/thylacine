---
id: dec-2026-09-21-timewait-yield-ratified
type: dec
title: "The operator ratifies the TIME-WAIT admission yield"
date: 2026-09-21
status: standing
decided-by: user-vote
affects: [sub-netd-server, sub-netperf]
created: 2026-09-21
---
## Fork

[[dec-2026-09-21-timewait-yields-to-admission]] was made AUTONOMOUSLY, as a
refinement of a design the operator had voted on
([[dec-2026-09-17-tcp-transport-retirement]]): at netd's 64-transport bound the
oldest retiree that has reached TIME-WAIT yields to a new admission, and no
retiree that carries anything ever does. That note said the operator might
overturn it. The question left open was theirs: keep it, or revert.

## Research

Nothing new. What was put in front of the operator is what the first note
records: one boot in roughly eighty-five extincted on the boot probe's connect;
EVERY boot carried one 9.81-9.85 s dial stall against a 2.8 ms mean; after the
change the longest dial is milliseconds. The integrity rule of the 09-17
design is untouched and is pinned from both sides (netd's bound self-test, and
a sabotage that lets a FIN-WAIT retiree yield and fails it).

## Options

1. **Ratify.** The yield stands as built.
2. **Revert.** `tcp_admit` returns `false` at the bound; the stall and the
   intermittent boot failure return, and the probe's retry alone covers the
   extinction.

## The call

**Ratified.** The operator, on reading the takeover report, 2026-09-21:
"ratifying."

## Rationale

The record plane is append-only, so the ratification is its own note rather
than an edit to the first. `decided-by` on that note stays `autonomous` -- it
records how the call was MADE. This one records that it no longer awaits a
vote, so a later session can tell a ratified decision from an assumed one.
