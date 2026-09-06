---
id: chg-2026-09-06-notes-caught-wait-mechanisms
type: chg
title: "sub-kernel-notes brought current: the caught-note interruptible wait (item 11 + N-3 guard), the siglongjmp in_handler clear (bug-2), and the phenotype handler-time mask"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-notes
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
kernel/notes.c moved ~969 lines since the dossier's 2026-08-16 update (notes.c is
stable now -- last change 2026-09-02, aux having moved to NOCTURNE). A
correct-vocabulary coverage pass (the dossier says `*_INTR`/"interrupted", not
"interruptible" -- the first grep under-counted) found two mechanisms already
covered and borrowed, and three genuinely absent. All three verified in notes.c
before folding; this is I-9 signal-delivery concurrency, so each was reasoned
against the no-lost-wakeup discipline.

## Already covered (borrowed)

`SIG_IGN` discarded at generation (7580c1f7) is the "An ignored signal is
discarded at generation" section; `pipe` as a real TERMINATE note (#237,
34809ab3) is in the note-name/POSIX-mapping prose. The terminate-latch
interruptibility is likewise already described in the phenotype-branch section.

## Added: the caught-note interruptible wait (item 11)

A note with a live handler must run at the return tail, but a blocked peer reaches
no tail until woken. So a caught note arms a per-Proc caught-note mask
(`notes_arm_caught_note_locked`) and wakes blocked peers (`proc_caught_note_wake`);
each re-checks the lock-free `thread_caught_note_deliverable` predicate (`caught &
~note_mask & NOTE_MASK_SUPPORTED`) -- deliverable -> the wait returns `*_INTR`, not
-> re-park. The N-3 guard (0149d1e3) is why the predicate must refuse a note a
running handler would reject: waking for an undeliverable note is the arm-2
livelock (wake, find nothing, re-sleep, forever). Register-then-observe under the
one lock -- [[inv-i9]].

## Added: the siglongjmp in_handler clear (bug-2, VIVARIUM 6.23)

Delivery is gated on `in_handler`, cleared only at `rt_sigreturn`/exec. A
PHENO_LINUX handler that `siglongjmp`s to an ancestor `sigsetjmp` escapes without
`rt_sigreturn`, so `in_handler` sticks true and the N-3 guard deafens the guest
permanently. The detector is exact for a single-stack guest: the pre-handler SP
(`note_saved_sp_el0`) sits above every live handler frame and below every ancestor
`siglongjmp` target, so a same-stack escape is always at/above it (same SP_EL0
bank, no cross-bank compare). Its audit F1 -- a `>=` false-clear across a genuine
cross-stack coroutine swap -- is contained + exotic, tracked for v1.x; recorded
because it is the load-bearing edge (the soundness rests on `sigaltstack` being
unserved, pinned by a static assert to the sigaltstack row).

## Added: the phenotype handler-time mask (01f076f2)

The frame's `uc_sigmask` is the PRE-handler mask (restored by `rt_sigreturn` from
`note_saved_mask`); the live `note_mask` becomes `blocked | sa_mask | sig` (unless
`SA_NODEFER`) via `vivarium_handler_mask` -- the mask a handler observes and passes
on (an inside `rt_sigprocmask`, an inside `execve`/`fork`). Delivery is unchanged:
the `in_handler` guard still holds every note for the handler's duration, so the
widened mask admits no nested delivery. Extended the phenotype-branch region.

`updated:` -> 2026-09-06; guarded-by unchanged. The Design-D self-managing-mark
exec-clear (5f2d4ded) touched notes.c but is the mark's lifecycle -- proc/exec's,
already folded this session in [[sub-kernel-proc]] -- not a notes-delivery change.
