---
id: spec-debug-stop
type: spec
title: "debug_stop.tla"
models: [sub-kernel-devproc, sub-kernel-birth-hold]
pins: [inv-i39, inv-i9, inv-i24]
cfgs:
  - "debug_stop.cfg -- clean: Safety (TypeOk + NoLostStop + NoEL0AfterStopped + ExactlyOnceResume + StopImpliesOwned + NoEL0WhileHeld) + EventuallyAllDead + EventuallyResumed + EventuallyLaunchedDies + EventuallyStopSettles + NoEretIntoDeath + LatchedHeldChildEnds (the birth ones vacuous without HELD)"
  - "debug_stop_held.cfg -- clean, the birth hold: the above + EventuallyHoldResolved + BirthWaitReleases + HoldMonotone"
  - "debug_stop_buggy_park_before_die.cfg -- the stop checked before the die-check: DeathWinsOverStop broken"
  - "debug_stop_buggy_lost_stop.cfg -- NoLostStop violated"
  - "debug_stop_buggy_double_wake.cfg -- ExactlyOnceResume violated"
  - "debug_stop_buggy_strand_on_debugger_death.cfg -- NoStrand violated: a dead debugger leaves its quarry parked"
  - "debug_stop_buggy_fault_stop_ungated.cfg -- StopImpliesOwned violated: a hardware fire racing a detach strands the target"
  - "debug_stop_buggy_stop_skips_sleeper.cfg -- a syscall-blocked sleeper never becomes fully-stopped"
  - "debug_stop_buggy_exitkill_ignored.cfg -- EventuallyLaunchedDies violated: a launched target orphans to init"
  - "debug_stop_buggy_held_runs_free.cfg -- NoEL0WhileHeld violated: the birth park ignores the hold"
  - "debug_stop_buggy_convert_clears_first.cfg -- NoEL0WhileHeld violated: the conversion clears the hold before it stops the child"
  - "debug_stop_buggy_orphan_hold_strands.cfg -- EventuallyHoldResolved violated: no orphan rule"
  - "debug_stop_buggy_birth_wait_unwoken.cfg -- BirthWaitReleases violated: a release before the park strands the spawner"
  - "debug_stop_buggy_no_death_recheck.cfg -- NoEL0WhileHeld violated: the birth park erets into its launcher's death"
  - "debug_stop_buggy_no_death_recheck_tail.cfg -- NoEretIntoDeath violated: the tail's park erets into a dying group"
  - "debug_stop_buggy_birth_latch_erets.cfg -- NoEL0WhileHeld violated: an interrupt latched at the birth park runs the held child"
  - "debug_stop_buggy_birth_latch_rerun.cfg -- LatchedHeldChildEnds violated: the re-run checkpoint goes round forever on a declined frame"
gate: "any change to the stop/park/resume protocol, the attach-slot lifetime, the tail ordering of the die-check against the stop-check, the birth park, the hold's writers, or the held spawn's wait"
created: 2026-08-02
updated: 2026-09-29
---
## Abstraction

Written **model-first**, before the impl — the sixth instance of spec-first being
re-enabled for a single surface, and re-enabled for the usual reason: the stop
protocol sits on the tree's most bug-prone lineage (the death path), where the
tests are structurally blind to the interleavings that matter.

The model is a debugger, a target Proc, and its Threads: attach claims a slot,
a stop request parks Threads at their EL0-return checkpoint, a resume releases
them, and a group termination can arrive at any point. What it proves is that
those four can interleave arbitrarily without losing a stop, running a Thread
after it is stopped, resuming twice, parking a Thread nobody owns, or stranding
one forever.

**The sharp line the model exists to hold is the tail ordering.** A Thread
returning to EL0 checks *death first*, then the stop. So a death **unwinds** a
Thread while a stop **parks and re-parks** it — and a target being killed is
never observed as debug-stopped. `park_before_die` is that ordering inverted, and
it is the first buggy cfg for a reason.

**Deliberately beneath the model:**

- the *content* of the register frames, and the SPSR privilege guard on writes —
  a data-flow property, not a protocol one;
- the hardware breakpoint / watchpoint / single-step machinery, which has its
  own model;
- the elected-9P-reader role release, added when a stop was found to freeze the
  shared filesystem client for unrelated Procs — below this abstraction, as the
  reader-frame model is;
- the second stop owner. Job control parks Threads on the *same* rendez with its
  own flag, and that the two compose is [[spec-pty-stop]]'s obligation, not this
  model's;
- the tail park's exit for a latched interrupt. What a stopped Thread owes an
  interrupt latched while it is parked is an open design question (OPEN-BUGS,
  2026-09-29), so the model has nothing yet to hold it to.

## The birth hold (2026-09-29)

With `HELD` the target is a child spawned `SPAWN_DEBUG_HELD`
([[sub-kernel-birth-hold]]): one head Thread, born in `exec_setup`, that marks
itself parked at its birth tail and parks until its hold and every stop are
clear. The debugger's `stop` converts the hold, `start` and an explicit
`detach` release it, and the spawner waits for the park and dies taking a
still-held child with it. Without `HELD` every birth variable is constant and
every birth action disabled, so the non-held cfgs keep their counts.

The clean held cfg found a real gap on its first run. The EXITKILL release
terminates the group and only then clears the stop, so a Thread that passed its
park's death check just before the terminate read the cleared flag and erets.
The fix is a second death check after the wake condition. The gap was never
specific to the birth park: the tail's park runs the same loop, and
`no_death_recheck_tail` keeps that half through the action property
`NoEretIntoDeath`.

The birth park's latch leg joined the model on 2026-09-29, after audit round 1
found the first draft's answer to it spinning. An interrupt-terminate posted
to the held child (`PostInterrupt`, the ghost `latch`) wakes the birth park
(the `"intr"` source), and the park ends the child. The two wrong answers are
kept: `birth_latch_erets` erets as the tail's leg does, and
`birth_latch_rerun` re-runs the checkpoint, which goes round forever once note
delivery declines a stack pointer the debugger wrote (`LatchedHeldChildEnds`).

## Action-site map

| Action | Site |
|---|---|
| `Attach` / `Detach` | the ctl `attach`/`detach` verbs — claim/release `debug_owner` under the process-table lock |
| `RequestStop` | `proc_debug_stop_deliver` — the RELEASE store of the stop flag, then the sleeper wake and the EL0 kick; on a held target, then `proc_birth_hold_convert_locked` (`ConvertFinish`) |
| `FaultStop` | `proc_debug_fault_stop` — the hardware-fire path, which takes the table lock and delivers **only while the slot is owned** |
| `Park` | `el0_return_stop_check` at both EL0-return tails, ordered *after* the die-check; the loop is `el0_stop_park`, which re-checks death after its wake condition |
| the sleeper detour | the nested stop check inside `sleep`/`tsleep`, so a syscall-blocked Thread can park without reaching the tail |
| `StartResume` / `StartRelease` | `proc_debug_resume` — clear the flag, then wake every Thread parked on its own debug rendez; on a held target `proc_birth_hold_release_locked` clears the hold first |
| `ReleaseSlot` | the ctl-fd close hook: resume an attached target, or terminate an `exitkill`-marked launched one; an explicit detach releases a hold, the implicit close keeps it |
| `MarkExitkill` | the ctl `exitkill` verb — slot-owner gated |
| `BirthArrive` / `BirthMark` | `userland_enter_held`'s birth tail, then `el0_birth_park` moving the mark from UNBORN to PARKED under the table lock, waking the spawner |
| `BirthLoop` … `BirthResume` | `el0_stop_park` with `birth_park_wake_cond`, which reads the hold first; the latch leg ends the child (`birth_park_terminate`) |
| `PostInterrupt` | the LS-5c latch, armed on the note's commit when nothing catches the note, and `proc_interrupt_terminate_wake`; the park sees a latch only in a family the thread has not masked, and a held child's thread masks nothing, so the ghost `latch` is every armed one |
| `SpawnerScan` / `SpawnerWakeUp` / `SpawnerDie` | `spawn_await_birth` on the parent's `child_waiters`; the orphan rule in `proc_become_zombie_locked`, before the reparent |

| Invariant | Obligation |
|---|---|
| `NoLostStop` | a stop request is never dropped between the flag store and the park |
| `NoEL0AfterStopped` | no Thread executes at EL0 once the target is fully stopped |
| `ExactlyOnceResume` | no double wake of a parked Thread |
| `StopImpliesOwned` | the stop flag is set only while a debugger owns the slot |
| `EventuallyResumed` | NoStrand — detach, close, or debugger death always releases an **attached** target |
| `EventuallyLaunchedDies` | the exitkill refinement — a debugger-**launched** target dies with its launcher instead of orphaning |
| `DeathWinsOverStop` | a published group termination kills every Thread even against a live debugger holding a stop |
| `EventuallyStopSettles` | a stop settles even over a Thread that was asleep in a syscall when it arrived |
| `NoEretIntoDeath` | neither park proceeds to EL0 once a group termination is published |
| `NoEL0WhileHeld` | a held child runs nothing until its hold is released or its converted stop resumed |
| `EventuallyHoldResolved` | a child whose spawner died holding it dies |
| `BirthWaitReleases` | a held spawn returns: the child parks, dies or is released, and each wakes the wait |
| `HoldMonotone` | a released hold is never set again |
| `LatchedHeldChildEnds` | a held child with an interrupt latched ends, unless it is released first |

`StopImpliesOwned` and its counterexample arrived late and from a self-audit: the
hardware-fire path originally delivered a stop by calling the deliver function
directly, with no lock and no owner check, so a breakpoint firing concurrently
with a detach could set the flag *after* the detach's resume cleared it — parking
a target with no debugger left to release it. The fix routes every fire through
the gated path; the cfg pins it.

## Note on the cfg count

The tree carries 17 cfgs, two clean and fifteen buggy, and scripture now counts
the same (DEBUG-FS-DESIGN section 6, ARCHITECTURE's spec table, 2026-09-29).
The earlier drift ran the other way: section 6 listed six buggy cfgs while the
tree carried seven, because `exitkill_ignored` landed without the list being
re-derived. Each count in `specs/SPEC-TO-CODE.md` is a single-worker TLC run.
