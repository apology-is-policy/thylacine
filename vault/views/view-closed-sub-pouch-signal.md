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
4 closed findings on [[sub-pouch-signal]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-signals13b-r1-f1]] [P1] The seam-check list was not extended for the five note syscall numbers (the threads-round F5, verbatim) (fixed)
- [[fnd-signals13b-r1-f10]] [P3] __restore_rt fell off the end of .text if SYS_NOTED ever returned (fixed)
- [[fnd-signals13b-r1-f11]] [P3] sigaction accepted SIG_ERR as a handler (fixed)
- [[fnd-signals13b-r1-f2]] [P1] A multi-thread Proc's SIG_DFL bootstrap wedged forever after the kernel refused NDFLT (fixed)
<!-- generated:end -->
