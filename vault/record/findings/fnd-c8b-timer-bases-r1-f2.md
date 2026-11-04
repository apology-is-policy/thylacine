---
id: fnd-c8b-timer-bases-r1-f2
type: fnd
round: adt-c8b-timer-bases-r1
severity: P3
status: fixed
title: "A cross-CPU link's deadline was bounded by the backstop, where the prose said a tick or the kick"
surface: [sub-kernel-rendez, sub-kernel-gic]
threatens: []
fixed-by: chg-2026-11-03-c8b-timer-bases
regression: "none: memory ordering TCG does not show"
created: 2026-11-03
---
## Prosecution

`earliest` is stored relaxed and gic_send_ipi had no barrier, so a cpu0 woken by the kick could read the old value and wait out its 4 ms backstop. I-9 held; the stated bound was wrong. Fixed: gic_send_ipi opens with DSB ISHST, as Linux's does, which makes the claim true and closes the same shape for every lockless reader an IPI wakes. The prose states the test phase's bound, where the kick is gated off.
