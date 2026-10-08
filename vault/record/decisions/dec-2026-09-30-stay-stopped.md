---
id: dec-2026-09-30-stay-stopped
type: dec
title: "A stopped thread keeps its stop when an interrupt arrives; only group death ends it"
date: 2026-09-30
status: standing
decided-by: user-vote
affects: [sub-kernel-rendez, sub-kernel-notes, sub-kernel-death, sub-kernel-jobctl, sub-kernel-birth-hold, sub-kernel-devproc, sub-kernel-proc, sub-kernel-ninep-client, sub-kernel-devctl, sub-kernel-hwdebug]
created: 2026-09-30
---
## Fork

A note whose default action is to terminate (`interrupt`, `tty:quit`,
`tty:hup`, `pipe`) arms a terminate latch when nothing catches it, and every
sleep unwinds for an armed latch (LS-5c). The tail's stop park left the park on
it so the thread could take the note at its next checkpoint, but only the
synchronous tail delivers notes: a debug- or job-stopped compute-bound thread
that was sent an interrupt ran at EL0 with its stop set. The nested sleeper
park unwound on it, the birth park ended a held child on it, and the vfork
suspend and the held spawn's birth wait returned early on a latch that a peer
thread can still revoke. What should a stopped thread do with an interrupt?
Found while the birth hold was built (OPEN-BUGS 2026-09-29 21:36Z and 22:52Z);
asked in one batch with the Go-fork question on 2026-09-30.

## Research

- Plan 9: `postnote` readies only a process waiting on a Rendez, which a
  `Stopped` process is not, so the note waits for `start`; `notify` runs
  `procctl` before it handles notes; `kill` on `ctl` readies a Stopped process
  with `Proc_exitme` (port/proc.c, port/devproc.c).
- POSIX XSH 2.4.3: signals sent to a stopped process are not delivered until it
  is continued, except SIGKILL.
- Linux: `wants_signal` is false for a stopped or traced task for every signal
  but SIGKILL, so `complete_signal` leaves the signal queued; its group-exit
  short cut also requires `sig == SIGKILL || !p->ptrace`. The vfork wait is
  killable only. (The question as asked said an untraced job-stopped Linux
  process dies of a fatal signal; that overlooked `wants_signal`. Corrected in
  DEBUG-FS-DESIGN 5g: all three heritages keep a stopped process stopped.)
- Tree facts: only the synchronous tail delivers notes
  (seam-el0-irq-tail-no-notes); the latch is cleared by `notes_set_handler`
  and `notes_mark_self_managing`, so it is a wake hint, not a commitment.

## Options

1. **Stay stopped** (recommended): the note waits until the stop clears; only
   group death ends a stopped thread. Cost: a park sleep that only group death
   interrupts, and the spec models the tail's latch leg.
2. **The interrupt ends it**: the park exits with the note's name, as the birth
   park did.
3. **Deliver on the IRQ tail**: closes the seam; a stopped thread then dies of
   the interrupt at its tail, the second option's outcome through a larger
   change.

## The call

Option 1 (operator, answered 2026-09-30 05:11Z). As specified in
DEBUG-FS-DESIGN 5g:

- One sleep, `sleep_death_only`, unwinds for group death alone and absorbs any
  other wake that reaches it. The tail's stop park, the birth park, the nested
  sleeper park, the vfork suspend and the held spawn's birth wait use it. The
  latch's, a caught note's and a second stop's wake walks pass a stop park by,
  since it could only absorb them and a woken park reads as an unsettled stop:
  a stop park is woken by a resume or by death alone (audit round 1, F1).
- The stop park loses its latch exit, and `birth_park_terminate` goes. The
  birth tail's note delivery before the park is unchanged: a note latched while
  a held child is still loading ends it before its first instruction.
- A park neither consumes nor clears the latch. When the stop clears or the
  suspend returns, the thread takes the note at its next note checkpoint.
- `debug_stop.tla` models the latch on any target and on the held spawn's
  spawner: `tail_latch_erets`, `latch_ends_stop` and `spawner_latch_returns`
  are new buggy cfgs, and `birth_latch_rerun` retires with
  LatchedHeldChildEnds.
- Death wins in the exit close too. A dying Proc's last thread reads no death
  while it closes its handles, and group death clears no stop, so a stop could
  park that close until the stop cleared (found in round 1's self-audit,
  pre-existing since #68 F1). `proc_stop_requested` answers false once
  `group_exit_msg` is set; the tail reads the raw owners (`proc_stop_owned`) so
  its park's death check still ends a thread killed after its die check; `stop`
  and `waitstop` read a dying target as gone. The predicate's meaning, rather
  than a second predicate, was settled with main (yip 0152): a split would
  spin the 9P tag drain for the closer.
- A dying Proc is not stopped to any reader (audit rounds 2 and 3): both stop
  delivers refuse it, so no flag is set; a parent's wait reports neither its
  stop nor its continue (POSIX reports a child that is stopped; Linux's group
  exit drops both); `stop`, `waitstop` and a step's wait read it as gone; the
  orphan rule does not count it as a stopped member; `/ctl/procs` does not
  show it STOPPED. The flags a stop set before the kill stay set, because the
  tail reads them.

## Rationale

It is what every heritage system does, and it is what a debugger needs: a
stopped target that an interrupt can set running cannot be inspected, and a
stop that holds until its owner clears it is the premise of every stopped-only
read and write (I-39). A parent suspend cannot trust the latch, because a peer
thread can revoke it after the wake; waiting for the child's release costs the
parent only the rest of an exec or a load, and `kill` still ends it at once.
