---
id: spec-pty-stop
type: spec
title: "pty_stop.tla"
models: [sub-kernel-proc, sub-kernel-devproc]
pins: [inv-i9]
cfgs:
  - "pty_stop.cfg -- clean: StopCompatI39 under the cook-to-stop trigger composed with death"
  - "pty_stop_liveness.cfg -- DeathWinsOverJobStop: a published termination always reaches the group"
  - "pty_stop_buggy_double_stop.cfg -- StopCompatI39 violated: a job resume clears a debugger's stop"
  - "pty_stop_buggy_death_blocked.cfg -- DeathWinsOverJobStop violated: a job-stopped Proc never dies"
gate: "any change to stop ownership — a new stop owner, or a resume path that clears a flag it does not own"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

The sibling of [[spec-debug-stop]], and it exists because job control added a
**second owner of the same park**. A Thread parks on its own debug rendez for
either of two independent reasons — a debugger's stop, or a Ctrl-Z — and the park
predicate is their disjunction.

Two owners sharing one park is exactly the shape that invites a one-flag
implementation, and a one-flag implementation is wrong in a way no single-owner
test can see: resuming from a Ctrl-Z would also release a debugger's stop. So the
model's whole job is the *composition*, with the two flags kept separate and each
resume clearing only its own.

Small by design — a stop kind set, a per-owner flag, and death — because
everything about *how* a Thread parks is already proved next door. What this adds
is that adding an owner did not break it.

## Action-site map

| Action | Site |
|---|---|
| `StopJob` / `ResumeJob` | `proc_job_stop_proc` / `proc_job_cont_proc`, reached from the `/proc` ctl `suspend`/`resume` verbs and from the terminal's Ctrl-Z path |
| `StopDebug` / `ResumeDebug` | `proc_debug_stop_deliver` / `proc_debug_resume` |
| `SetGflag` / `GroupDie` | `proc_group_terminate` and the per-Thread die-check |

| Invariant | Obligation |
|---|---|
| `StopCompatI39` | a debug stop persists until its **own** resume; a job resume never clears it (and the converse) |
| `DeathWinsOverJobStop` | a published group termination reaches every Thread even while the group is job-stopped |

`DeathWinsOverJobStop` is the same clause [[spec-debug-stop]] proves against the
debugger's stop, restated against the second owner — a job-stopped process must
still be killable, or `kill` on a Ctrl-Z'd job would hang forever.

**Beneath the model:** the terminal line discipline that *decides* to raise a
stop, the report latches a waiting parent consumes, the orphan rule, and the
authority gate on the `/proc` verbs — the last of which is deliberately the
**kill** gate, since stopping is strictly weaker than the killing it already
permits.
