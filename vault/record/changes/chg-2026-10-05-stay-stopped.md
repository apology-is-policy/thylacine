---
id: chg-2026-10-05-stay-stopped
type: chg
title: "Stay stopped: a stopped thread keeps its stop, and death wins in the exit close"
date: 2026-10-05
arc: arc-go-ide
commits: ["*(pending)*"]
touched:
  - sub-kernel-rendez
  - sub-kernel-notes
  - sub-kernel-death
  - sub-kernel-jobctl
  - sub-kernel-birth-hold
  - sub-kernel-devproc
  - sub-kernel-devctl
  - sub-kernel-proc
  - sub-kernel-ninep-client
  - sub-kernel-hwdebug
  - sub-kernel-exception
  - sub-kernel-poll
  - sub-substrate-gates
  - spec-debug-stop
  - spec-pty-stop
  - spec-death-wake
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-05
---
The operator voted on 2026-09-30 that a stopped thread keeps its stop when an
interrupt arrives, and that only group death ends it, as Plan 9, POSIX and
Linux all do ([[dec-2026-09-30-stay-stopped]]). One sleep that only group death
unwinds, `sleep_death_only`, now serves the tail's stop park and the birth
park, the nested sleeper park, the vfork suspend and the held spawn's birth
wait ([[sub-kernel-rendez]], [[sub-kernel-exception]],
[[sub-kernel-birth-hold]]). The latch's, a caught note's and a second stop's
wake walks pass a stop park by, so a stop park is woken by a resume or by death
alone ([[sub-kernel-notes]], [[sub-kernel-jobctl]], [[sub-kernel-poll]]). Death
now wins in the exit close too: `proc_stop_requested` answers false once the
group is dying, so a killed, stopped Proc's last thread never parks in its own
handle close, while the EL0-return tail reads the raw owners
([[sub-kernel-death]], [[sub-kernel-ninep-client]]). No reader calls a dying
Proc stopped: both stop delivers refuse it, a parent's wait reports neither of
its latches, `stop`, `waitstop` and a step's wait read it as gone, the orphan
rule does not count it, and `/ctl/procs` does not show it STOPPED
([[sub-kernel-proc]], [[sub-kernel-devproc]], [[sub-kernel-devctl]]). A pending
step belongs to its debugger slot: a whole-Proc stop, a detach and the ctl-fd
close cancel it ([[sub-kernel-hwdebug]]). The test runner releases a fixture
Proc that a failing test left linked, keyed on a mark the link helpers set,
after a red test hung the boot at the next drain of kproc's children
([[sub-substrate-gates]]). `debug_stop.tla` models the latch on any target and
on the spawner, with three new buggy configurations ([[spec-debug-stop]]);
`pty_stop.tla`'s refusal of a stop to a dying group is now the code's too
([[spec-pty-stop]]), and the death-wake model leaves the exit close to the
kernel tests ([[spec-death-wake]]). Four audit rounds, three of Opus 5.5 on
Opus 5.5 while Fable was out and the fourth Fable 5.1, found nothing above P3.
Verified: `tools/test.sh` 1798/1798 with debug-probe and jc-probe (killst)
on 78b34682; TLC (debug_stop 19 configurations, pty_stop 4) as claimed and
`tools/ci-smp-gate.sh` 5 rows x 10/10 with no corruption on 47cddabb, whose
code the squash carries unchanged.
