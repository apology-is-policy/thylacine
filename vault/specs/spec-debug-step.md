---
id: spec-debug-step
type: spec
title: "debug_step.tla"
models: [sub-kernel-devproc]
pins: [inv-i39]
cfgs:
  - "debug_step.cfg -- clean: Safety (StepExactlyOne) + EventuallyAllDead + StepEventuallyReparks (146 distinct, 2 Threads)"
  - "debug_step_buggy_runs_free.cfg -- the step exception never re-traps: StepExactlyOne (68)"
  - "debug_step_buggy_death_lost.cfg -- the tail skips the die-check on a step re-entry: EventuallyAllDead (146)"
gate: "any change to the step arm / re-park leg, the hardware single-step machine, or the step exception route"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

A **sibling** of [[spec-debug-stop]], not an extension — deliberately, and the
reasoning is worth keeping. The stop model's counterexamples are the landed
pre-commit gate for the park/resume/death handshake, and a breakpoint-fired stop
is trigger-agnostic: it reuses that machinery unchanged. The only genuine
protocol growth in the hardware-debug tier is the **step**, so it gets its own
small model rather than churning an audited base's configurations.

The whole model is one question: *does a step execute exactly one instruction,
and can a death still win against one in flight?*

A Thread walks four states — stopped, at the tail, stepping, dead — with a
per-Thread armed flag, a per-Thread count of instructions executed in the
current window, and a flag marking a tail entry as a step re-entry. The tail is
the serialization point, shared with the stop model: **the die-check runs first,
then the step-or-park decision**. That ordering is what makes death win, and it
is also what bounds the step to one instruction, because the step exception
routes back through the same tail.

**Deliberately beneath the model:**

- the **hardware**. `MDSCR.SS`, `SPSR.SS`, the exception class — the model has a
  boolean and a counter. Whether the architecture's step machine actually
  re-traps after one instruction is an ARM-ARM question;
- the **step-over-breakpoint dance** — stepping off an address that has a
  breakpoint on it requires disabling that breakpoint for the step and
  re-enabling it after, which is entirely below the abstraction;
- **migration.** A step armed on one CPU that resumes on another is a real
  hazard the implementation handles by arming per-Thread rather than per-CPU;
  the model has no CPUs;
- **the whole-Proc stop.** See below — this is the gap that mattered.

## Action-site map

| Action | Site |
|---|---|
| `RequestStep(t)` | `kernel/devproc.c` — the `step` ctl verb: compute the step-over address from the target's breakpoint slots, set the head Thread's armed flag with a release store, then clear the stop request and wake the parked Threads |
| `Tail(t)` | `kernel/proc.c::el0_return_stop_check` — die-check first; then, if no stop is outstanding and the armed flag is set, set `SPSR.SS` in the trapframe the return will restore. The window from arming to the return is interrupt-masked, so nothing lands between |
| `StepExec(t)` | `arch/arm64/exception.c`, the software-step exception from a lower exception level, routed back to the tail; the step machine's arm and disarm live in `arch/arm64/hwdebug.c` |
| `DeathWake(t)` | the death cascade, unchanged from the stop model — a parked Thread is woken to the tail, where the die-check terminates it |
| `SetGflag` | `proc_group_terminate` |

| Invariant | Obligation |
|---|---|
| `StepExactlyOne` | a step window executes at most one instruction — the safety half of [[inv-i39]]'s execution control |
| `EventuallyAllDead` | death wins over a step in flight; a group termination reaps even a stepping Thread |
| `StepEventuallyReparks` | a step not racing a death completes and returns to stopped — the witness that the step machine advances rather than stranding |

Both counterexamples break `EventuallyAllDead`, by different routes, and that is
the point of having two. A step that **runs free** never re-enters the tail, so
it never reaches a die-check — it breaks the safety invariant *and* the liveness
one. A tail that **skips the die-check** on a step re-entry does re-enter, and
re-parks a Thread that should have died — safety holds, liveness fails. The
first is the stuck-step-machine family; the second is the step-path twin of the
stop model's park-before-die.

## The gap the audit found

A step window can be interrupted two ways in the real system: a **death**, and a
**peer-initiated whole-Proc stop** — another Thread's breakpoint firing, or a
detach and re-attach. This model has the first and **not the second**. Its
actions are request-step, tail, step-execute, death-wake, and publish-the-death
flag; there is no stop action at all, because the stop was the sibling model's
subject and this one was scoped to the step.

That seam is where the tier's one P1 lived. A `step` superseded by a whole-Proc
stop left the armed flag set, since only the step's own exception clears it — so
the *next* resume armed a spurious single-step and the target took an unexpected
one-instruction stop after a continue. The fix is a walk clearing every Thread's
armed flag when a stop is delivered, under the process-table lock, with the
release store paired against the two acquire readers.

The model could not have caught it, and the reason is precise rather than
general: the composition of a step with a stop is in **neither** model. The step
model has no stop; the stop model has no step. Each is sound in its own scope,
and the bug lived in the space between two correctly-scoped siblings.

That is the real cost of the sibling pattern, and it is worth stating next to
the pattern's benefit. Splitting the model kept the audited base's
counterexamples stable — genuinely valuable — at the price of making a
cross-model composition unreachable to both. The multi-threaded target that
surfaced it is the ordinary case for the language runtime this tier exists to
debug, not an exotic one.

## A note on scale

The implementation arms **only the head Thread**, so the clearing walk is
usually a one-element loop, written for a future per-Thread step. The model
quantifies over all Threads, which makes it *stronger* than the code needs
today — a rare direction for a model to be wrong in, and the harmless one.

The action-site map in the tree still describes these sites as reserved, from
when the model landed ahead of the implementation. They landed; the map did not
follow.
