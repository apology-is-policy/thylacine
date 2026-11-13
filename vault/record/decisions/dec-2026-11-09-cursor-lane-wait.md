---
id: dec-2026-11-09-cursor-lane-wait
type: dec
title: "A chord behind a stalled cursor lane waits too, and fails at the lane's abandonment"
date: 2026-11-09
status: standing
decided-by: user-vote
affects: [sub-lictor]
created: 2026-11-09
---
## Fork

The trusted path's round 2 (Opus on Opus, R2-F1): [[dec-2026-11-05-sak-waits-on-backlog]] renews the kernel's deadline while the control queue holds a takeover up, but a takeover also waits for the cursor lane to go idle (`Gpu::takeover_clear`), since a late cursor chain could put the pointer plane over the trusted scene. A cursor chain waits on the same QEMU host loop a client's readback holds. With the pointer moved just before the chord, the lane is stalled, the takeover submits nothing, the control queue never stalls, nothing renews, and the kernel fails the chord at five seconds: no trusted screen and no dead engine, the silent failure vote 71 exists to remove. The cursor lane abandons itself `FENCE_ABANDON_MS` after its stall and comes back on a late retirement (vote 50's legs C and X).

## Research

Windows' secure desktop rides on TDR: a GPU silent past two seconds is reset, then the secure desktop draws, and the pointer there is the display driver's, not a client's. Linux has no compositor trusted path. In this tree, `Gpu::stall_pending` already counts a non-idle cursor lane as a stall, so a cursor stall inside `Screen::activate` starts the takeover over rather than failing it; only the renewal ignored the lane.

## Options

A: renew while the lane's stall stands; its abandonment ends the wait and the chord fails, with the reason on the log; the lane comes back as vote 50 has it. B: the same, but an abandonment during a takeover kills the engine, so vote 71 holds literally. C: keep the five-second failure behind a cursor stall and record it as vote 71's exception.

## The call

Operator vote 79 (2026-11-09): A.

## Rationale

A client's backlog delays the SAK rather than failing it, on either lane. The residue (a host loop held 30 s) is the same hold that kills the engine on the control queue, and abandonment stays recoverable, as vote 50 decided for the lane.
