---
id: fnd-note-wait-r2-f3
type: fnd
round: adt-note-wait-r2
severity: P2
status: fixed
title: "NOTE_WAIT_POLL in a Proc no table governs re-issues for ever on a default-ignored note"
surface: [sub-kernel-notes]
threatens: []
fixed-by: chg-2026-11-04-poll-mask
regression: "notewait.poll_no_table_has_no_tail_note; sabotage K41"
created: 2026-11-04
---
## Prosecution

With no table the tail leaves a default-ignored note queued, the settle rewinds under NOHAND, and the svc finds the same note. No in-tree caller yet, but the syscall is public. The class of [[fnd-note-wait-r1-f7]].

## Disposition

Begin does not go to the tail for a Proc no table governs. The call waits under its mask and answers.
