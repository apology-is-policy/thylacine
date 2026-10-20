---
id: dec-2026-10-12-note-target
type: dec
title: "A caught note goes to a running thread, then main; every EL0 return delivers notes"
date: 2026-10-12
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-exception, sub-kernel-vivarium, sub-kernel-thread, sub-kernel-sched, sub-kernel-death]
created: 2026-10-12
---
## Fork

Two defects, one design.

**#964.** The IRQ-from-EL0 vector ran the preempt check, the die-check and the
stop check, and returned. It had no notes leg, so a thread that neither makes
a syscall nor faults never took a note: a `while(1);` program ignored Ctrl-C,
and a caught handler (a `SIGALRM` timeout in a compute loop, a `SIGINT`
handler) never ran. Group death and a job stop still reached it, because both
are read by legs the IRQ tail has. The gap was a vault seam
(`seam-el0-irq-tail-no-notes`) and was re-enqueued by the groupstatus audit,
round 2.

**L285.** ARCH 8.8.3 woke every blocked thread for a caught note, and the
first to claim its family unwound. A child's `SIGCHLD` therefore returned
`EINTR` from a worker's `ppoll` while the main thread was running and could
have taken it. The groupstatus SMP gate failed on this once in 40 boots; a
forced construction failed every boot, on main's kernel too. POSIX allows any
thread to take a process-directed signal, but Linux and FreeBSD prefer a
thread that disturbs nothing, and a Linux program can depend on that.

Which thread takes a process-directed caught note, and how does a thread that
never enters the kernel take one?

## Research

- **Plan 9 (9front `sys/src/9/arm64/trap.c`).** Every trap from user mode,
  interrupts included, leaves through one exit that calls `donotify(ureg)`
  when `up->procctl || up->nnote`. Only an FIQ returns early. `notify` copies
  the whole `Ureg` to the user stack and `noted` restores it, so a note
  interrupts any instruction. A Plan 9 process has one thread, so there is no
  choice of thread.
- **Linux (`kernel/signal.c`).** `complete_signal` takes the thread the signal
  was sent to (the group leader for `kill`) when `wants_signal` holds: the
  signal is not blocked, the thread is not exiting or stopped, and it is
  running or has no signal pending. Otherwise it walks the group round-robin
  for one that wants it. `signal_wake_up_state` marks that thread, wakes it if
  it sleeps interruptibly, and otherwise `kick_process` sends an IPI so a
  thread at EL0 enters the kernel and meets the signal on its way out
  (`exit_to_user_mode_loop`, on every return, interrupts included). A thread
  that blocks the signal, or exits, passes the shared signals on
  (`retarget_shared_pending`). A target in an uninterruptible sleep keeps the
  signal until it wakes.
- **FreeBSD (`kern_sig.c`).** `sigtd` takes the current thread when it is in
  the process and does not block the signal, else the first thread that does
  not block it. `tdsigwakeup` aborts an interruptible sleep or forwards the
  signal to a running thread by IPI. `reschedule_signals` passes pending
  process signals on when a thread blocks them.
- **The tree (verified).** `notes_noted_restore` already restores the whole
  context from the kernel-side save: x0-x30, sp, elr, spsr, V0-V31, FPSR and
  FPCR (task #96). ARCH 7.6.4 still described an elr-and-sp restore read back
  from the user stack. The delivery path makes no syscall-only assumption: there
  is no `SA_RESTART` rewind (`restart_syscall` is `ENOSYS`), the frame comes
  from the saved registers, and the IRQ handler acks, dispatches and EOIs with
  IRQs masked and closes its halls frame, so the IRQ tail reaches the notes leg
  in the state the sync tail does. A busy CPU is never tickless (`NO_HZ_IDLE`
  only), so the tick bounds how long a spinner waits even without a kick.
  Only Linux-phenotype waits unwind for a caught note
  (`proc_caught_note_eintr_ready`); a native thread takes it at a tail.

## Options

1. **Prefer a running thread, then main**, native and phenotype alike. The
   poster picks the thread that takes the note. A thread that is not blocked
   reaches a tail soon (an EL0 spinner is kicked into the IRQ tail), so it is
   preferred, with the posting thread first and main next. Only when no such
   thread exists is a blocked thread woken to unwind, main first. The choice is
   recorded, and only the chosen thread may unwind for that family. Any
   thread's tail still delivers the note.
2. **Keep the first claimant (POSIX).** Document the divergence in VIVARIUM
   and leave a Linux program that relies on a quiet peer to retry `EINTR`.

## The call

Option 1, native and phenotype (operator, 2026-10-12, "Prefer running/main
(Recommended)"), with #964 folded into the same chunk ("#964 next, with the
vote"): one design, one audit round, one SMP gate. The hand-off below is
modelled in TLA+ (operator, 2026-10-12, "Model it (Recommended)";
`specs/caught_target.tla`), which re-enables spec-first for this surface.

- **Every EL0 return delivers notes.** The IRQ-from-EL0 vector branches into
  the tail the synchronous vector takes, so an IRQ return runs the preempt
  check, the die-check, the stop leg and the notes leg in that order, with one
  budget. Delivery clears `SPSR.BTYPE` in the handler's context, because an IRQ
  can arrive between an indirect branch and its target; the restore puts the
  saved value back.
- **A kick.** A post that picks a thread running on another CPU sends it
  `IPI_RESCHED`, as `kick_process` does. The interrupt-terminate wake kicks
  the Proc's running threads too, so Ctrl-C reaches a spinner promptly.
- **The designation.** For each open caught family the posting walk picks one
  candidate: a thread that is not exiting, has the family unmasked, is not in
  a handler, and is not in a wait that will not unwind for a caught note. It
  prefers, in order, the posting thread, main (the oldest live thread), any
  other thread that is not blocked, then main blocked in an unwinding wait,
  then any thread blocked in one. In a Linux-phenotype Proc it sets the
  family's claim bit on the pick's behalf (`note_target`), and only the
  designee or a claimant may unwind for a claimed family. A native Proc's
  walk only kicks, since no native wait unwinds for a caught note.
- **The hand-off.** A designation ends at its thread's EL0-return tail,
  whatever the tail delivered. It also ends when the thread enters a wait that
  cannot unwind for the family (one that will not unwind for a caught note, or
  one whose family the thread has masked; the wait's kind is published under
  the thread's `wait_lock`, where the walk decides), before the thread blocks,
  and at its exit. Each end with the note still queued runs the walk again; a
  walk run by a release skips the releasing thread. Every walk also wakes a
  claim holder blocked in an unwinding wait, because a claim can outlive the
  note it was taken for when another thread's tail delivered it.

## Rationale

The phenotype's contract is Linux's, and the native rule should not differ
without a reason; none was found. Waking every sleeper and letting the first
claim interrupts a call that nothing needed to interrupt, and a thread at EL0
that never enters the kernel must still take its notes, as it does on Plan 9
and Linux.

The hand-off is stronger than Linux, which leaves a signal with a target in an
uninterruptible sleep. Here a peer that can unwind is woken instead, so a long
non-interruptible wait in one thread cannot hold a note that another thread
could take now.

Option 2 keeps a defect that the gate had already shown, and leaves #964 to be
designed alone.
