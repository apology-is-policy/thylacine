---
id: fnd-90-r1-f1
type: fnd
title: "The revert-probe covered tsleep() only; production rides sleep()"
round: adt-90-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
fixed-by: chg-2026-07-19-90-death-block-through
regression: "rendez.reader_frame_blocks_death_sleep (revert-probed)"
created: 2026-07-31
---
## Prosecution

The steady-state reader recv runs with deadline 0, so srvconn's tsleep
DEGRADES to sleep() -- production block-through fires the sleep() guards,
but the regression (a +10ms deadline) revert-probed only the tsleep()
guards: the production path's guards could regress green.

## Disposition

Fixed in-close: added the sleep()-path test (blocks a pending death through
both the register-then-observe and resume-path checks, then returns
SLEEP_OK). Revert-probed: dropping the sleep() resume guard fails it while
the tsleep test still passes -- proving it covers what the other did not.
