---
id: fnd-b0poll-r5-f1
type: fnd
title: "A noise-driven poll(-1) holds its CPU IRQ-masked for as long as the noise lasts, and the noise is unprivileged"
round: adt-b0poll-r5
severity: P1
status: fixed
surface: [sub-kernel-poll]
threatens: [inv-i9]
fixed-by: chg-2026-09-22-poll-preemption-point
regression: "poll.point_services_noise"
created: 2026-09-22
---

## Prosecution

Syscall bodies run with interrupts masked from EL0 exception entry to return
([[sub-kernel-syscall-abi]]; ARCH 8.11). The poll re-arm loop re-registers and
re-samples on every wake, so a wake it can never satisfy keeps it looping
inside that masked body: a pipe, one thread writing, one reading, and a third
polling the pipe with `events = 0`. Every ingredient is unprivileged. While the
loop runs, the CPU serves no interrupt at all -- the timer tick and the SAK
included. A deadline bounds nothing, because `poll(-1)` has none.

## Disposition

FIXED at the time with a per-thread spin budget (`nsleeps`) plus a 1 ms
backoff: after a pass with no real sleep in the last millisecond, the poller
sleeps for real, and a real sleep unmasks. SUPERSEDED by
[[fnd-b0poll-r6-s1]], which shows the bound is per-THREAD while the obligation
is the CPU's; replaced by the preemption point in
[[chg-2026-09-22-poll-preemption-point]].
