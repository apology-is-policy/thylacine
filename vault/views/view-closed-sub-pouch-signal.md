---
id: view-closed-sub-pouch-signal
type: view
title: "Do-not-re-report preamble — sub-pouch-signal"
query: closed:sub-pouch-signal
---
# Do-not-re-report preamble — sub-pouch-signal

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-pouch-signal`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH the delivery contract: the kernel keeps `in_handler` true
until `SYS_NOTED`, so **every** arm of the bootstrap must reach one —
that is what makes [[fnd-signals13b-r1-f2]] a P1 rather than a cosmetic
fallback, and what a new arm must satisfy.

Two dispositions here are now superseded by the kernel and read as
history: the multi-thread `NDFLT` refusal (retired by #809's cascade) and
the `raise(SIGKILL)` refusal. The `NCONT` fallback that remains is
defense-in-depth only — relying on it to swallow a terminating signal
would be the bug.

<!-- generated:begin -->
11 closed findings on [[sub-pouch-signal]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-note-wait-r1-f1]] [P3] A one-shot reset can erase a concurrent re-install (fixed) — The kernel half is fixed: SYS_NOTE_DISPOSE and rt_sigaction store under q->lock. The reviewer also proposed writing Pouch's table after the dispose; not taken, because a caught row whose table entry is not yet written sends the note to the dispatcher's default arm, which is the death the ordering exists to prevent. Reading sa_flags in the dispatcher is not taken either: its read set stays {sa_handler}. So the Pouch residue stays, stated in sub-pouch-signal's Concurrency section: a sigaction racing a one-shot delivery.
- [[fnd-note-wait-r1-f2]] [P3] The one-shot leg passes with the kernel reset deleted (fixed) — Fixed before the report returned, by a leg on a default-ignored note: with the row reset the post cannot unwind the read, and with it left caught the read reads EINTR. The reviewer's proposed leg was not needed.
- [[fnd-note-wait-r1-f3]] [P3] Nothing pins that a handler entered out of a suspend builds on the wait mask (fixed) — suspend-mask blocks SIGTERM in the caller and not in the wait, and asserts what the handler read.
- [[fnd-note-wait-r1-f4]] [P3] joey's capture margin (fixed) — The note-wait legs print on failure only, with one summary line, which keeps the output near the measured 2.3 KiB. joey is untouched.
- [[fnd-note-wait-r1-f5]] [P3] An in-handler suspend ends on notes no handler takes (fixed) — Fixed before the report returned, in 057b24967: the scan asks notes_proc_has_live_handler.
- [[fnd-note-wait-r1-f6]] [P3] A SIGPIPE default set inside a handler loses its mask half (fixed) — Operator vote 25: the default is a row that ignores, and the mask bit goes ([[dec-2026-11-03-sigpipe-row]]). That also closes seam-pouch-sigmask-per-thread, at the chunk's landing.
- [[fnd-note-wait-r1-f7]] [P3] NOHAND re-issues forever on an sp the tail refuses (fixed) — The settle refuses the rewind when the tail refuses the sp, so the call answers EINTR as it did before.
- [[fnd-signals13b-r1-f1]] [P1] The seam-check list was not extended for the five note syscall numbers (the threads-round F5, verbatim) (fixed)
- [[fnd-signals13b-r1-f10]] [P3] __restore_rt fell off the end of .text if SYS_NOTED ever returned (fixed)
- [[fnd-signals13b-r1-f11]] [P3] sigaction accepted SIG_ERR as a handler (fixed)
- [[fnd-signals13b-r1-f2]] [P1] A multi-thread Proc's SIG_DFL bootstrap wedged forever after the kernel refused NDFLT (fixed)
<!-- generated:end -->
