---
id: fnd-threads9b-r1-f1
type: fnd
round: adt-threads9b-r1
severity: P1
status: fixed
title: "pthread_cond_timedwait with a >1h timeout spins at 100% CPU"
surface: [sub-pouch-thread]
threatens: [inv-i9]
fixed-by: chg-2026-05-23-p6-threads-b
regression: "the clamp at TORPOR_MAX_TIMEOUT_US in `__futex4_cp`"
created: 2026-08-01
---
## Prosecution

1. `__futex4_cp` converted musl's relative timespec to microseconds and
   clamped at `LONG_MAX`.
2. The kernel does not clamp — it REJECTS `timeout_us >
   TORPOR_MAX_TIMEOUT_US` (1 hour) with `-EINVAL`.
3. `__timedwait_cp`'s dispatch is
   `if (r != EINTR && r != ETIMEDOUT && r != ECANCELED) r = 0` — so
   `EINVAL` becomes "no error, and no wait happened".
4. `pthread_cond_timedwait`'s
   `do … while (*fut == seq && (!e || e == EINTR))` therefore loops with
   no wait in it: 100% CPU until the absolute deadline converts to a
   sub-hour relative timespec.

A caller doing the ordinary thing — waiting on a condvar with a long
deadline — burns a core for up to an hour.

## Fix

Clamp `timeout_us` at `TORPOR_MAX_TIMEOUT_US` pouch-side. The kernel then
accepts, returns a clean `ETIMEDOUT` at the cap, and the caller's outer
loop re-evaluates its absolute deadline and re-enters with a fresh
sub-hour relative timespec. Closes the `EINVAL`-collapsed-to-0 finding
(F3) by construction: `EINVAL` no longer occurs on this path.
