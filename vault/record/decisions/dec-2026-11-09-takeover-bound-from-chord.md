---
id: dec-2026-11-09-takeover-bound-from-chord
type: dec
title: "Every wait a takeover makes is bounded from the chord"
date: 2026-11-09
status: standing
decided-by: user-vote
affects: [sub-lictor]
created: 2026-11-09
---
## Fork

The trusted path's round 3 (Opus on Opus, F1): a takeover's paint and activation wait for an idle cursor lane (`Gpu::takeover_clear`), so a wait behind the cursor lane ([[dec-2026-11-09-cursor-lane-wait]], bounded `FENCE_ABANDON_MS` from its own stall) and then a wait behind the control queue ([[dec-2026-11-05-sak-waits-on-backlog]], bounded the same from its own stall) run in series, under one kernel cap of 35 s from the chord (`SEAT_QUIESCE_LIMIT_NS`). A GL client that queues work, a readback of it and about 26 s more, with the pointer moved just before the chord, holds the lane about 10 s and then the paint behind the rest: the kernel fails the chord at 35 s with the engine alive, no trusted screen and nothing on the glass, which vote 71 says cannot happen. Painting while the lane waits narrows the window but does not close it, because activation's own commands follow a late cursor revival.

## Research

Windows' secure desktop rides on TDR: a GPU silent past two seconds is reset and then the secure desktop draws, so the trusted path never waits on client work past a fixed bound from the request. Linux has no compositor trusted path.

## Options

A: bound every takeover wait from the chord: at chord + 33 s a control stall still standing kills the engine and a cursor stall abandons the lane. B: record the residue (the chained case ends at the cap with a live engine, logged). C: raise the cap to 65 s so two waits in series fit.

## The call

Operator vote 80 (2026-11-09): A.

## Rationale

Every chord ends inside the cap in the trusted screen, a dead engine, or a logged failure, which keeps vote 71's sentence and vote 79's without a second cap. The cost lands only on a client that holds the device past 33 s across two waits, the class vote 71 already kills; in that case the engine dies with its own stall under 30 s old.
