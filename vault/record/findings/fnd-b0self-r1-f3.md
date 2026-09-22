---
id: fnd-b0self-r1-f3
type: fnd
title: "poll.timeout_survives_a_busy_list passed with the deadline test removed: a producer on another thread hits the clear-to-tsleep window only by luck"
round: adt-b0self-r1
severity: P3
status: fixed
surface: [sub-kernel-poll]
threatens: [inv-i9]
fixed-by: chg-2026-09-21-srvconn-two-endpoint-poll
regression: "poll.timeout_survives_a_busy_list, rebuilt; sabotage `deadline` fails it at 'returned at ITS deadline, not when the producer went quiet'"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/test/test_poll.c`
**Invariant**: a regression test must fail on the defect it names
**Prosecution**:
1. `tsleep` prefers a set flag to a passed deadline, so the re-arm loop carries its own `timer_now_ns() >= deadline_ns` test.
2. The test drove a send / recv producer from the test thread. With the loop's test removed the poller still returned 0: after the clear, the flag is set again only if a walk lands before `tsleep`'s cond check, and a cooperative producer almost never does.
3. Measured: the `deadline` sabotage boot PASSED the test.

## Disposition

Rebuilt @237ba793: the producer is the polled object itself, a test Dev whose `.poll` registers, walks its own hook list on every sample, and is never ready. Every re-sample then re-flags the hook inside the window. The walking stops after 1 s so a kernel without the test still returns; the test asserts WHEN the last sample happened, with non-vacuity asserts on the re-sleep counter and the sample count.
