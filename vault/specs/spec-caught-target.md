---
id: spec-caught-target
type: spec
title: "caught_target.tla"
models: [sub-kernel-notes, sub-kernel-rendez, sub-kernel-exception]
pins: [inv-i19, inv-i9]
cfgs:
  - "caught_target.cfg -- clean, 2 threads, 2 posts, 2 waits a call: TypeOK + OneClaimant + NoStrand (613245 distinct, -workers 1 -lncheck final)"
  - "caught_target_3t.cfg -- 3 threads, the same properties; not in the gate and not finished: stopped at the disk floor after 12789190 distinct states (4 workers), the invariants holding throughout and NoStrand not judged (specs/SPEC-TO-CODE.md)"
  - "caught_target_buggy_double_designate.cfg -- the walk designates without checking the family is still open: OneClaimant (371)"
  - "caught_target_buggy_no_handoff.cfg -- a designee keeps the family into a wait that holds: NoStrand (611617)"
  - "caught_target_buggy_unlocked_designate.cfg -- the designation is made without the re-check under the pick's wait_lock: NoStrand (632863)"
  - "caught_target_buggy_no_rekick.cfg -- no walk re-wakes a holder blocked in an unwinding wait: NoStrand (613245)"
  - "caught_target_buggy_exit_keeps.cfg -- an exiting thread keeps its designation: NoStrand (639673)"
  - "caught_target_buggy_release_no_wake.cfg -- a release owes no walk: NoStrand (165597)"
  - "caught_target_buggy_irq_no_notes.cfg -- the IRQ tail has no notes leg: NoStrand (613245)"
  - "caught_target_buggy_stale_keeps.cfg -- a designee entering an unwinding wait it cannot take (masked, or in a handler) keeps the designation: NoStrand (616829)"
gate: "any change to proc_caught_note_walk, thread_caught_note_claim, notes_release_claims, sleep_caught_wait_enter, notes_handoff_target, notes_exit_drop_locked, or where a claim or a designation is set or cleared; specs/check-caught-target.sh"
created: 2026-10-12
updated: 2026-10-12
---
## Abstraction

The threads of one Proc and one caught note family. Posts queue notes; the
posting walk picks a candidate and, in two steps (`Walk1` picks, `Walk2`
re-checks under the pick's lock and designates), takes the family's claim on
its behalf. Each thread runs at EL0, enters waits that unwind for a caught note
or hold, takes the IRQ tail, delivers at its tail, releases there, and exits.
Only the holder of a claimed family may unwind for it. The walk may pick ANY
candidate, so the preference order (poster, main, awake, then sleepers) is the
kernel tests' to pin, not this model's.

The mask and the handler flag are the thread's own and written without a lock;
`Walk2` reads them exactly only for a blocked thread (`Fresh`). A designee that
was picked on a stale value gives the designation up at its tail, or at its next
wait if that wait cannot unwind for it (`uentry`). A release's walk skips the
releasing thread (`wskip`, `wcur`).

## What it pins

- **`OneClaimant`** -- no two threads hold one family, as a claim or a
  designation. The double-designation cfg is the one that breaks it.
- **`NoStrand`** (liveness) -- a caught note stays queued forever only if no
  thread stays able to take it. Every other buggy cfg breaks this one, each by
  its own mechanism (read off the counterexample, recorded in
  `specs/SPEC-TO-CODE.md`).

## What it cannot see

One family; a delivered note's handler is one bit that the return clears;
discarded and uncaught notes (the terminate latch's wake is
[[spec-death-wake]]'s); the preference order; the kick, since the tick is the
fairness that matters and a kick only makes it prompt; a call enters at most
`MAX_WAITS` waits.

## Action-site map

The full table is in `specs/SPEC-TO-CODE.md`. In short: `Walk1` and `Walk2` are
`kernel/proc.c::proc_caught_note_walk` (`caught_pick`, `caught_designate`);
`EnterUWait` / `EnterHWait` / `Handoff` are `kernel/sched.c::sleep_caught_wait_enter`
and `kernel/notes.c::notes_handoff_target`; `Resume` is
`thread_caught_note_claim`'s designation-to-claim conversion; `Irq` is the
0x480 vector's branch into `.Lel0_return`; `Deliver` / `Release` are
`notes_deliver_at_el0_return` and `notes_release_claims`; `Exit` is
`thread_exit_self` -> `notes_exit_drop_locked`.

## Holding the code to it

Kernel tests in `test_rendez.c`: `rendez.caught_note_prefers_awake`,
`_poster_first`, `_handoff` (through sleep and tsleep), `_exit_drop`,
`_holder_rewake`, `_native_kicks_only`, `_repick_fallback` (forced stale picks
reach the wake-every-unwinder fallback, and one unwinds), `_one_unwind`,
`_release_wakes_peer`. On the device:
viv-pheno-probe L331-L333 (a spinning main takes a child's `SIGCHLD` while a
polling peer is left alone) and jc-probe's spinint and spinnote rungs (a native
spinner ended by `^C`, and one whose handler runs). The pre-commit gate is
`specs/check-caught-target.sh`.
