---
id: chg-2026-11-09-aux-commit-death
type: chg
title: "A fatal note's unwind commits the death: no handler installed a moment later revokes it (vote 35)"
date: 2026-11-09
arc: arc-phase6-pouch
commits: ["15a9379a6", "5e1783bbd", "f264545d9", "ef212b8ef", "b2b75a208", "c8ca8bde5", "729fe7922", "3ffe15e8f", "204b1c82d", "b0284cd29"]
touched:
  - sub-kernel-notes
  - sub-kernel-death
  - sub-kernel-proc
  - sub-kernel-jobctl
  - sub-kernel-fault
  - sub-kernel-exception
  - sub-kernel-rendez
  - sub-kernel-poll
  - spec-death-commit
established: []
closed: []
opened: []
mirrors-checked:
  - "kernel/include/thylacine/proc.h: PROC_FLAG_TERMINATE_COMMITTED (bit 16) and the tty:hup latch (bit 15) live in proc_flags, which no syscall, /proc file or userspace mirror exports (devproc reads single seal bits only); no ABI change"
depth: skeletal
no-dossier-change: "the note changes no code; the touched dossiers were co-staged with the code in 5e1783bbd, ef212b8ef, c8ca8bde5 and 729fe7922"
created: 2026-11-09
---
Operator vote 35 ([[dec-2026-11-05-commit-the-death]]): a wait that unwinds for a terminate latch answers on the footing that its thread dies -- poll's 0, a sleep's SLEEP_INTR -- and a handler installed between the unwind and the EL0 tail used to clear the latch, so the thread lived holding an answer meant for a dying one (OPEN-BUGS 2026-11-04, the poll false-0 class; notes.h's RW-0 F4 caveat). Branch aux-3-commit, off aux-3-unlock 5f6d97e23.

**The mechanism.** tty:quit and tty:hup latch separately (5e1783bbd), so each latch names one note. Reading the death predicate true for a latch commits the death by one lock-free compare-and-swap: `PROC_FLAG_TERMINATE_COMMITTED`, with only the latch acted on left armed (`notes_commit_latch`). Every latch write is a swap refused once that is set (`notes_latch_write_locked`). The tail clears a note's latch by that swap before either delivery path and dies of the committed note when refused (`notes_tail_yield_or_die_locked`); the uncaught, in-handler and SIG_DFL arms commit the note they die of under `q->lock` (`notes_commit_note_locked`). A committed death ends every thread's waits whatever its mask; `el0_return_die_check` and the stop park end the Proc by the named note; a stop, an exec claim and a fork are refused. The page-in reads the predicate once, after its read. [[spec-death-commit]] models it.

**Found on the way.** A census of the 146 test files found one broken test and the stop park's hole; the model found the uncaught arm's race; writing the sabotage list found three unwitnessed arms (b2b75a208, c8ca8bde5).

**Round 1 (Fable 5.1, 0/0/1/2, not dirty; fixed 729fe7922).** F1: the in-handler arm died without committing. F2: the fault arm's pick and stale clear were dead code once every true read commits; removed. F3: two comments. Self-found: the synchronous consumers may pop the committed note while their thread dies before EL0 (Linux's sigtimedwait shape; the claim is scoped to "handed to a handler"), and notes.h's stale latch block.

**Leases.** runs-1 (729fe7922) hung in poll_zero_is_a_death's control: a native Proc gets no EINTR from a caught note (ARCH 8.8.3), and the control had no deadline (3ffe15e8f). runs-2 (3ffe15e8f): green 2442/0 plus guest legs, all 21 sabotages red, regreen == green, death_commit and poll as claimed; it showed a hang under C01, a phenotype leg on the wrong arm and the TLC 2.19 verdict text (204b1c82d). runs-3 (204b1c82d): green again, C01 C07 C09 C22 C23 each red at its leg, regreen == green (bcb4893c19052b69), tail_order and death_wake as claimed. runs-4 (b0284cd29) C24, the in-handler arm's commit dropped for the phenotype alone, red at handler_tail_death_commits_its_note's phenotype leg, regreen == green. The same lease ran the eleven spec check scripts with a temporal leg before and after b0284cd29 (TLC 2.19's unnamed temporal verdict; OPEN-BUGS 11-09 16:21Z).

**Not run:** the full SMP matrix (held since 2026-11-02), the interactive fleet, test-fault.
