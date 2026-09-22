---
id: fnd-pouchb0-r4-f2
type: fnd
title: "the poll re-arm loop never checks death or stop itself, and tsleep's checks sit behind its cond test: a noise-driven poll(-1) is unkillable and unstoppable"
round: adt-pouchb0-r4
severity: P2
status: fixed
surface: [sub-kernel-poll]
threatens: [inv-i9, inv-i24, inv-i39]
fixed-by: chg-2026-09-21-srvconn-two-endpoint-poll
regression: "poll.death_ends_a_noise_driven_poll, poll.stop_parks_a_noise_driven_poll; specs/poll.tla DeathTerminates + StopHonoured with poll_buggy_no_loop_die_check.cfg and poll_buggy_no_loop_stop_check.cfg"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/poll.c` the re-arm loop; `kernel/sched.c` `tsleep` (cond first, then deadline, then the 8c-2 stop detour, then the #811 die-check)
**Invariant**: I-24 (death), I-39 (stop settles), I-9
**Prosecution**:
1. A producer walks the poller's list inside every re-sample window (deterministic with a self-walking Dev).
2. Every `tsleep` finds a flag set and returns AWOKEN before its stop detour and die-check.
3. The Proc cannot die or be stopped until the deadline -- which EL0 chooses (INT_MAX ms) -- and the loop spins IRQ-masked. pipe_block_locked and chan_role_acquire share the shape with smaller windows.
**Suggested fix**: check thread_die_pending / the stop in the loop.

## Disposition

Fixed as suggested, spec first: each pass checks `thread_die_pending` and parks on `proc_stop_sleeper_park` with every hook off (DEATH WINS: the park returns SLEEP_INTR), and a noise pass `sched_yield_hint`s so queued work runs. RESIDUE, the operator's: with nothing else runnable the loop still spins with interrupts off, because syscalls run IRQ-masked end to end -- a preemption-model question recorded for the F3-F9 kernel-design conversation, not invented here.
