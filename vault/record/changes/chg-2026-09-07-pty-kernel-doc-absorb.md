---
id: chg-2026-09-07-pty-kernel-doc-absorb
type: chg
title: "absorb docs/reference/135-pty-kernel (the PTY-1 kernel arc, I-20): fold the STOP-class note consumption (#252 masked reader + the notes_stop_dequeue P1) into sub-kernel-notes"
date: 2026-09-07
arc: arc-vault
commits: ["277b57ad"]
touched: [sub-kernel-notes]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
The PTY-1 kernel arc (I-20; sessions/pgrps, pts registry, tty seam, job-control
stop). Big audit-bearing doc. quaestor owner: kernel/pts.c -> sub-kernel-pts; proc.c
-> sub-kernel-{proc,jobctl,death,caps}; notes.c -> sub-kernel-notes -- ALL audit:hard,
ALL fresh (2026-09-05/06). Verified atom-by-atom; the surface is comprehensively
covered by 6 dedicated dossiers.

ALREADY COVERED (verified fresh + deep): sessions/pgrps (1a) + the wait extension
(1e report-is-not-reap) -> sub-kernel-proc; the pts registry (1c: SrvConn-pointer
identity + magic downcast fail-closed + gen guard F11 + MAY_POST_SERVICE gate) + the
tty seam (1d) -> sub-kernel-pts; the job-control stop (1f: job_stop_req 2nd owner,
the catchability gate + #15 self-stop, #240 freshness susp_stop_armed, the orphan
rule, the #19 NON-COMPLETING torpor wake [confirmed here -- my 136-ptyfs deferral was
correct], the group-global-IPI-once F2, the 8c-3 elected-reader release, SYS_TTY_CONT)
-> sub-kernel-jobctl; the tty:* class registration + POST gate (I-39 F4) ->
sub-kernel-notes + abi-note-names; I-20 + pty_stop.tla -> inv-i20.

THE FOLD (genuine gap -> sub-kernel-notes, depth rich; updated 09-06 -> 09-07):
- The STOP-class note CONSUMPTION was homeless (grep two-scans / notes_stop_dequeue /
  class-filtered -> 0 vault hits; sub-kernel-notes covered the tty:* class + POST gate
  + coalesce but not the dequeue). Folded: (a) the #252 all-masked reader -- when
  every thread masks NOTE_BIT_TTY the fan POSTS tty:susp (pending); pre-fix nothing
  consumed it (fell through the EL0-tail terminate-only arms, silently lost, 1 of 16
  slots with it); notes_stop_note_name_locked (STOP twin of the terminate scanner)
  applies it via proc_job_stop_self so #240 freshness + orphan rule reapply; lands
  exactly at unmask, a cont having disarmed evaporates it; (b) the P1 two-scans lesson
  -- notes_stop_dequeue_locked is class-FILTERED (first STOP note at ANY index) not the
  class-BLIND FIFO pop; a child_exit in front of the susp diverges -> the stop applies,
  the child_exit is destroyed (a wait notification silently gone), the susp re-fires;
  the general lesson (a decision + its consumption as two scans are two predicates,
  only a between-test tells). Noted the c8ab2744 per-note phenotype-sigtab gate ->
  145-vivarium's regression (deferred to that pass).

Redirect stub. TTIN #18 = a v1.x seam (sub-ptyfs), not folded. Zero code change.
