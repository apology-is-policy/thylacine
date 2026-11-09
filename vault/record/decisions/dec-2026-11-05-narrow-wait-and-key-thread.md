---
id: dec-2026-11-05-narrow-wait-and-key-thread
type: dec
title: "A synchronous command never waits past 500 ms behind a readback, and Lictor reads input on its own thread"
date: 2026-11-05
status: standing
decided-by: user-vote
affects: [sub-lictor, sub-gallery]
created: 2026-11-05
---
## Fork

Lictor reads input on the loop that waits for the GPU. Measured on the Pi's V3D (2026-11-05, round 3's image): at 30 ms a character, 4 of 7 command lines arrived whole behind a client's deep queue and 1 of 2 behind a readback; at 100 ms a character all arrived. Separately, with the seat normal, a synchronous command behind any client's readback waited up to `FENCE_ABANDON_MS` (30 s) instead of stalling at `SUBMIT_DEADLINE_MS`, so one transfer from any unprivileged client stopped the serve loop for as long as it queued: chords unseen, keys lost, the engine dead at 30 s. That widening predated the recoverable stall (vote 29).

## Decision

Operator vote 72: narrow the wait to 500 ms, and add a small Lictor thread that moves raw input reports into a 4096-event queue, the seat logic staying on the serve thread. The alternatives were the same thread also passing keys to the kernel's seat (so a chord reaches the kernel during a stall: Mutter's later step), narrowing alone, and keeping both as they were.

## Consequences

A bystander's present behind a stall answers an error at about 500 ms instead of blocking; aurora and halcyond count it a dropped frame, gallery and tapestry-demo retry within the abandonment bound, and the C-6 gate's DEADLINE arm counts a recovered stall as survival. Plan 9's libdraw, Xorg 1.19 and Mutter all read input on a thread apart from what blocks. The thread's queue is 40 s at machine pace, past the 30-s abandonment bound; ls-graphical-keys-overrun's blind lever must stop the thread too.
