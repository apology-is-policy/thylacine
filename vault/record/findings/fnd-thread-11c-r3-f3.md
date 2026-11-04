---
id: fnd-thread-11c-r3-f3
type: fnd
round: adt-thread-11c-r3
severity: P3
status: fixed
title: "The clear after the open had no witness"
surface: [sub-kernel-stalk]
threatens: [inv-i19]
fixed-by: chg-2026-11-03-item-11c
regression: "stalk.open_is_the_only_interruptible_step, "nothing after the open may unwind"; W4 red on it"
created: 2026-11-03
---
## Prosecution

Nothing after `dev->open` waits interruptibly today, so removing the clear changed nothing observable, which is the condition under which a later opted-in wait in a close would run interruptible unnoticed. The fixture now records the flag in its close and the test asserts it stayed clear.
