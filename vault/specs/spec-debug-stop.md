---
id: spec-debug-stop
type: spec
title: "debug_stop.tla"
models: [sub-kernel-devproc, sub-kernel-birth-hold]
pins: [inv-i39, inv-i9, inv-i24]
cfgs:
  - "debug_stop.cfg -- clean: Safety (TypeOk + NoLostStop + NoEL0AfterStopped + ExactlyOnceResume + StopImpliesOwned + NoEL0WhileHeld + SpawnReturnsAfterBirth) + EventuallyAllDead + EventuallyResumed + EventuallyLaunchedDies + EventuallyStopSettles + NoEretIntoDeath + ParkEndsOnlyInDeath (the birth ones vacuous without HELD)"
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
  - "debug_stop_buggy_birth_latch_erets.cfg -- NoEL0WhileHeld violated: an interrupt latched at the birth park returns it, and the held child runs"
  - "debug_stop_buggy_tail_latch_erets.cfg -- NoLostStop violated: an interrupt latched at the tail's park returns it, and a confirmed-stopped Thread runs"
  - "debug_stop_buggy_latch_ends_stop.cfg -- ParkEndsOnlyInDeath violated: the park ends a Thread for a latched interrupt (the pre-5g birth rule)"
  - "debug_stop_buggy_spawner_latch_returns.cfg -- SpawnReturnsAfterBirth violated: the spawner's own interrupt returns its birth wait before the child is born"
gate: "any change to the stop/park/resume protocol, the attach-slot lifetime, the tail ordering of the die-check against the stop-check, the birth park, the hold's writers, the held spawn's wait, or the death-only park sleep"
created: 2026-08-02
updated: 2026-10-06
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
- note delivery. The model latches an interrupt and wakes the parks with it,
  but has no notes. Where each tail calls the park against its notes leg --
  after the die-check and, since 2026-10-05, before the notes -- and the leg's
  re-pass for a stop it applies are [[spec-tail-order]]'s. What a Thread does
  with its note once it runs again is shown on the device by debug-probe: the
  `resume` leg meets a note posted during a stop on the very resume, before the
  child runs another instruction, and the `caught-step` leg's step reports at
  the handler's first instruction. In the kernel suite,
  `rendez.tail_parks_for_the_stop_it_applies` shows the notes leg parking for a
  stop it applied itself.

## The birth hold (2026-09-29)

With `HELD` the target is a child spawned `SPAWN_DEBUG_HELD`
([[sub-kernel-birth-hold]]): one head Thread, born in `exec_setup`, that marks
itself parked at its birth tail and parks until its hold and every stop are
clear. The debugger's `stop` converts the hold, `start` and an explicit
`detach` release it, and the spawner waits for the park and dies taking a
still-held child with it. Without `HELD` every birth variable is constant and
every birth action disabled. The latch is not a birth variable: since
2026-09-30 it lands on any target.

The clean held cfg found a real gap on its first run. The EXITKILL release
terminates the group and only then clears the stop, so a Thread that passed its
park's death check just before the terminate read the cleared flag and erets.
The fix is a second death check after the wake condition. The gap was never
specific to the birth park: the tail's park runs the same loop, and
`no_death_recheck_tail` keeps that half through the action property
`NoEretIntoDeath`.

## Stay stopped (2026-09-30)

The birth park's latch leg joined the model on 2026-09-29, after audit round 1
found the first draft's answer to it spinning, and the answer then was that
the park ends a held child for an interrupt. The operator's vote the next day
(DEBUG-FS-DESIGN 5g) replaced it: a stopped Thread keeps its stop, and only a
group death ends it. An interrupt-terminate can now latch on any live target
(`PostInterrupt`), its wake (the `"intr"` source) reaches either park, and the
park absorbs it: with its condition still false and no death published, the
woken Thread stays parked, and a confirmed one stays confirmed. The spawner
has a latch of its own (`slatch`, `PostSpawnerInterrupt`), and its birth wait
sleeps through it.

The kernel goes one step further than the model: the latch's wake walk, the
caught note's and a second stop's sleeper walk pass a thread in a stop park by,
since the park sleeps on the thread's own debug rendez, which nothing else
sleeps on, and could only absorb the wake. The model keeps the wake and its
absorb, which changes no variable, so the kernel's runs are among the model's.
`ParkEndsOnlyInDeath` and `SpawnReturnsAfterBirth` restate the clean model's
own guards, so the clean run cannot fail them; their buggy cfgs are what show
each catches its exit, and the kernel tests hold the code to them. A dying
group is never asked to park in the kernel (`proc_stop_requested` answers false
once `group_exit_msg` is set), so a dying Proc's exit close, which reads no
death in its sleeps, cannot park for a stop; the model has no closer, and
`rendez.exit_close_*` and jc-probe's `killst` rung (a killed job-stopped child
holding a staged 9P write is reaped with no resume) are the witnesses. A dying
Proc also takes no new stop in the kernel: `proc_debug_stop_deliver` refuses
it. `RequestStop` and `FaultStop` carry no `~gflag` guard, so the refusal only
removes runs (`proc.dying_takes_no_stop`).

The pre-5g exits are kept as knobs, each caught by a named property:
`tail_latch_erets` (the tail's park erets with the stop set: `NoLostStop`),
`birth_latch_erets` (the birth park erets a held child: `NoEL0WhileHeld`),
`latch_ends_stop` (the park ends the Thread: the action property
`ParkEndsOnlyInDeath`), and `spawner_latch_returns` (the birth wait returns on
the spawner's latch: `SpawnReturnsAfterBirth`). The re-run knob and
`LatchedHeldChildEnds` retired with the old rule: the park consults no note
delivery, so nothing re-runs, and a latched held child stays held until it is
released or killed. `specs/check-debug-stop.sh` runs every cfg and checks each
verdict. The vfork suspend shares the birth wait's park but is not modelled
here ([[spec-cow]] has no interrupts); the kernel test of the shared park is
its witness.

## Action-site map

| Action | Site |
|---|---|
| `Attach` / `Detach` | the ctl `attach`/`detach` verbs — claim/release `debug_owner` under the process-table lock |
| `RequestStop` | `proc_debug_stop_deliver` — the RELEASE store of the stop flag, then the sleeper wake and the EL0 kick; on a held target, then `proc_birth_hold_convert_locked` (`ConvertFinish`) |
| `FaultStop` | `proc_debug_fault_stop` — the hardware-fire path, which takes the table lock and delivers **only while the slot is owned** |
| `Park` | `el0_return_stop_check` at both EL0-return tails, ordered *after* the die-check and before the synchronous tail's notes leg, which calls it once more to park for a stop it applied ([[spec-tail-order]]); the loop is `el0_stop_park`, which re-checks death after its wake condition and sleeps in `sleep_death_only`, which a latch's wake does not return |
| the sleeper detour | the nested stop check inside `sleep`/`tsleep`, so a syscall-blocked Thread can park without reaching the tail; its park (`proc_stop_sleeper_park`) is death-only too |
| `StartResume` / `StartRelease` | `proc_debug_resume` — clear the flag, then wake every Thread parked on its own debug rendez; on a held target `proc_birth_hold_release_locked` clears the hold first |
| `ReleaseSlot` | the ctl-fd close hook: resume an attached target, or terminate an `exitkill`-marked launched one; an explicit detach releases a hold, the implicit close keeps it |
| `MarkExitkill` | the ctl `exitkill` verb — slot-owner gated |
| `BirthArrive` / `BirthMark` | `userland_enter_held`'s birth tail, then `el0_birth_park` moving the mark from UNBORN to PARKED under the table lock, waking the spawner |
| `BirthLoop` … `BirthResume` | `el0_stop_park` with `birth_park_wake_cond`, which reads the hold first; a latch's wake passes the park by (the model absorbs it), and the child stays held |
| `PostInterrupt` | the LS-5c latch, armed on the note's commit when nothing catches the note, and `proc_interrupt_terminate_wake`, which wakes every peer's blocked rendez but a stop park's, where the model's wake is absorbed and changes nothing |
| `PostSpawnerInterrupt` / `SpawnerLatchReturn` | the spawner's own latch: `await_child_release` sleeps in `sleep_death_only`, so it returns only for the child's birth, release or death, or its own group death |
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
| `ParkEndsOnlyInDeath` | a parked Thread ends only in a group death, never for a latched interrupt |
| `SpawnReturnsAfterBirth` | the held spawn returns only once the child is born: parked, released, dead or gone |

`StopImpliesOwned` and its counterexample arrived late and from a self-audit: the
hardware-fire path originally delivered a stop by calling the deliver function
directly, with no lock and no owner check, so a breakpoint firing concurrently
with a detach could set the flag *after* the detach's resume cleared it — parking
a target with no debugger left to release it. The fix routes every fire through
the gated path; the cfg pins it.

## Note on the cfg count

The tree carries 19 cfgs, two clean and seventeen buggy, and scripture counts
the same (DEBUG-FS-DESIGN section 6, ARCHITECTURE's spec table, 2026-09-30).
The earlier drift ran the other way: section 6 listed six buggy cfgs while the
tree carried seven, because `exitkill_ignored` landed without the list being
re-derived. Each count in `specs/SPEC-TO-CODE.md` is a single-worker TLC run,
and `specs/check-debug-stop.sh` re-runs every cfg against its claimed verdict.
