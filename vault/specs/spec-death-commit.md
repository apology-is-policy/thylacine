---
id: spec-death-commit
type: spec
title: "death_commit.tla"
models: [sub-kernel-notes, sub-kernel-death]
pins: [inv-i19, inv-i9]
cfgs:
  - "death_commit.cfg -- clean: the reader commits, every latch write is a swap refused once committed, the tail yields, an uncaught death commits: TypeOK + AnswerIsDeath + OneFate + CommitNamesNote (26 distinct with -workers 1)"
  - "death_commit_buggy_no_commit.cfg -- the reader commits nothing, the code before vote 35: AnswerIsDeath (25)"
  - "death_commit_buggy_plain_clear.cfg -- the disposition's re-read clears the latch with a plain store: CommitNamesNote (13)"
  - "death_commit_buggy_tail_no_yield.cfg -- the notes leg hands the note to a handler without the swap: OneFate (23)"
  - "death_commit_buggy_uncaught_no_commit.cfg -- the notes leg dies of an uncaught note without committing it: OneFate (29)"
  - "death_commit_control.cfg -- the clean model claiming R never lives after an EINTR, which must break: NoEintrLife (23)"
gate: "any change to how the death predicate reads a terminate latch (thread_die_pending's latch leg, notes_commit_latch), to a latch write (notes_latch_write_locked and its callers), to the tail's handling of a terminate-class note (notes_tail_yield_or_die_locked, notes_commit_note_locked) or to el0_return_die_check's committed arm; specs/check-death-commit.sh"
created: 2026-11-09
updated: 2026-11-09
---
## Abstraction

ARCH 8.8.2's committed death (operator vote 35,
[[dec-2026-11-05-commit-the-death]]). A terminate latch stands for a note whose
default ends the Proc and is revocable: a handler installed for the note
re-reads it away under `q->lock`. A wait that unwinds for the latch answers on
the footing that its thread dies, so a revoke between the unwind and the tail
left the thread living with an answer meant for a dying one (the poll false-0
class). The model has one Proc, one note posted with its default and its latch
armed, and three agents:

- R, a thread in a wait that reads the death predicate (the poll loop's check,
  sleep's register-then-observe), then runs its EL0 tail: the die check, then
  the notes leg.
- P, a peer already past its die check, at its notes leg.
- H, a thread installing a handler: the disposition's store, then the re-read.

Every step is one atomic action: a compare-and-swap on the word
`<<latch, committed>>`, or a whole `q->lock` hold. Beneath the model: several
notes and latches (the fixed order that picks among them is
`notes.commit_takes_the_readers_latch`'s), the mask (a masked read commits
nothing: `notes.latch_read_commits_the_death`), group death (monotonic, and
[[spec-poll]]'s `dying`), the stop park (`notes.stop_park_meets_a_commit`), the
wakes, which [[spec-poll]] and the death-wake model own, and the synchronous
consumers (a notes-fd read, a take, a SIG_IGN discard), which may pop the committed
note while its latch stays and the popping thread dies at its tail before EL0.

[[spec-poll]] models the poller's death as monotonic. Before vote 35 that was
false for a latch; this model is why it now holds.

## What it checks

| Property | Obligation |
|---|---|
| `AnswerIsDeath` | a wait that answered on the footing of death is never followed by the thread living |
| `OneFate` | no note is both handed to a handler and died of ([[inv-i19]]) |
| `CommitNamesNote` | a committed death keeps the one latch that names its note armed |
| `NoEintrLife` (control only) | claims R never lives after an EINTR; the clean model breaks it, so the handler-first path is open and the clean verdict is not vacuous |

## Action-site map

| Spec action | Code site (as-built) |
|---|---|
| `RWaitDeath` (the read, and its commit) | `kernel/notes.c::thread_die_pending`'s latch leg -> `notes_commit_latch`: one compare-and-swap sets `PROC_FLAG_TERMINATE_COMMITTED` and leaves the acted-on latch armed. Readers: `sleep_common` / `tsleep_common` (register-then-observe and resume), `kernel/poll.c` the loop's die check, `sys_poll_sleep_for` via `tsleep_noteintr`, `9p_client.c::client_self_dying`, `dev9p.c`, `notes_wait_mask_end`, `arch/arm64/fault.c::userland_demand_page`. `BUGGY_NO_COMMIT` = the predicate before vote 35 |
| `RWaitEintr` | `thread_caught_note_unwinds` at the sleep's caught arm (ARCH 8.8.3) |
| `RDieCheck` | `kernel/proc.c::el0_return_die_check`: `notes_death_committed(p)` -> `exits_note`; the stop park's `el0_stop_park_death` does the same for a thread that parked after it |
| `HStore` / `HReread` | `notes_set_handler` (and `viv_sigaction_settled`, `notes_handler_resets`, exec's drop): the store, then `notes_disposition_reread` under `q->lock`, whose latch write is `notes_latch_write_locked`. `BUGGY_PLAIN_CLEAR` = a plain store there |
| `RNotes` / `PNotes`, a handler | `notes_deliver_at_el0_return` -> `notes_tail_yield_or_die_locked` before both delivery paths (the native frame and `notes_deliver_linux_locked`). `BUGGY_TAIL_NO_YIELD` = no call |
| `RNotes` / `PNotes`, no handler | the native uncaught arm, the in-handler arm (a fatal default while a handler runs) and the phenotype's SIG_DFL terminate arm -> `notes_commit_note_locked` -> `exits_note`. `BUGGY_UNCAUGHT_NO_COMMIT` = no commit. The fault arm `notes_fault_dying_latch` is `RWaitDeath`'s commit (the page-in's read) followed by `RDieCheck`'s death, taken at the fault |

Kernel witnesses: `notes.poll_zero_is_a_death`, `notes.tail_hands_no_committed_note`,
`notes.tail_death_commits_its_note`, `notes.handler_tail_death_commits_its_note`,
`notes.tail_dies_of_the_committed_note`,
`notes.fault_arm_meets_a_commit`, `notes.latch_read_commits_the_death` and its
control `notes.latch_clear_first_lets_it_live`.
