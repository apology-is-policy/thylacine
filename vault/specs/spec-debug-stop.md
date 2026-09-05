---
id: spec-debug-stop
type: spec
title: "debug_stop.tla"
models: [sub-kernel-devproc]
pins: [inv-i39, inv-i9]
cfgs:
  - "debug_stop.cfg -- clean: Safety (TypeOk + NoLostStop + NoEL0AfterStopped + ExactlyOnceResume + StopImpliesOwned) + the EventuallyResumed NoStrand witness + EventuallyLaunchedDies"
  - "debug_stop_buggy_park_before_die.cfg -- the stop checked before the die-check: DeathWinsOverStop broken"
  - "debug_stop_buggy_lost_stop.cfg -- NoLostStop violated"
  - "debug_stop_buggy_double_wake.cfg -- ExactlyOnceResume violated"
  - "debug_stop_buggy_strand_on_debugger_death.cfg -- NoStrand violated: a dead debugger leaves its quarry parked"
  - "debug_stop_buggy_fault_stop_ungated.cfg -- StopImpliesOwned violated: a hardware fire racing a detach strands the target"
  - "debug_stop_buggy_stop_skips_sleeper.cfg -- a syscall-blocked sleeper never becomes fully-stopped"
  - "debug_stop_buggy_exitkill_ignored.cfg -- EventuallyLaunchedDies violated: a launched target orphans to init"
gate: "any change to the stop/park/resume protocol, the attach-slot lifetime, or the tail ordering of the die-check against the stop-check"
created: 2026-08-02
updated: 2026-08-02
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
  model's.

## Action-site map

| Action | Site |
|---|---|
| `Attach` / `Detach` | the ctl `attach`/`detach` verbs — claim/release `debug_owner` under the process-table lock |
| `RequestStop` | `proc_debug_stop_deliver` — the RELEASE store of the stop flag, then the sleeper wake and the EL0 kick |
| `FaultStop` | `proc_debug_fault_stop` — the hardware-fire path, which takes the table lock and delivers **only while the slot is owned** |
| `Park` | `el0_return_stop_check` at both EL0-return tails, ordered *after* the die-check |
| the sleeper detour | the nested stop check inside `sleep`/`tsleep`, so a syscall-blocked Thread can park without reaching the tail |
| `StartResume` | `proc_debug_resume` — clear the flag, then wake every Thread parked on its own debug rendez |
| `ReleaseSlot` | the ctl-fd close hook: resume an attached target, or terminate an `exitkill`-marked launched one |
| `MarkExitkill` | the ctl `exitkill` verb — slot-owner gated |

| Invariant | Obligation |
|---|---|
| `NoLostStop` | a stop request is never dropped between the flag store and the park |
| `NoEL0AfterStopped` | no Thread executes at EL0 once the target is fully stopped |
| `ExactlyOnceResume` | no double wake of a parked Thread |
| `StopImpliesOwned` | the stop flag is set only while a debugger owns the slot |
| `EventuallyResumed` | NoStrand — detach, close, or debugger death always releases an **attached** target |
| `EventuallyLaunchedDies` | the exitkill refinement — a debugger-**launched** target dies with its launcher instead of orphaning |
| `DeathWinsOverStop` | a published group termination kills every Thread even against a live debugger holding a stop |

`StopImpliesOwned` and its counterexample arrived late and from a self-audit: the
hardware-fire path originally delivered a stop by calling the deliver function
directly, with no lock and no owner check, so a breakpoint firing concurrently
with a detach could set the flag *after* the detach's resume cleared it — parking
a target with no debugger left to release it. The fix routes every fire through
the gated path; the cfg pins it.

## Note on the cfg count

Scripture describes this module as "clean + 6 buggy cfgs". The tree carries
**seven** — the count was correct before `exitkill_ignored` landed and was not
re-derived when it did. The eight cfg lines above are the as-built set.
