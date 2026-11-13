---
id: dec-2026-11-05-sak-waits-on-backlog
type: dec
title: "A chord behind a GPU backlog keeps waiting, keys held, while the engine drains"
date: 2026-11-05
status: standing
decided-by: user-vote
affects: [sub-lictor, sub-kernel-proc]
created: 2026-11-05
---
## Fork

Measured on the Pi's V3D (2026-11-05, round 3's image): behind a client's deep GPU queue (`chain-hold 120 deep`), six secure-attention chords of six met a takeover stall, started over, and failed at the kernel's five-second deadline; the trusted screen never came up and input went back to the session's screen, which is the spoof the SAK exists to stop. On QEMU virgl the trusted paint cannot get ahead of a client's queued work (one host FIFO).

## Decision

Operator vote 71: keep waiting with input withheld. Lictor renews the kernel's deadline while the engine is alive and draining, up to a bound, so a chord always ends in the trusted screen or a dead engine; the screen looks hung for as long as the client queued. A kernel seat change. The alternatives were failing at five seconds and then showing a trusted notice when the device can, with input flowing in the gap, and failing and documenting it.

## Consequences

Venus rings keep running during an episode, so a Venus client's backlog is not proven finite; the bound covers that case. Real display hardware may have a scanout engine apart from the render queue; none is in the gates.
