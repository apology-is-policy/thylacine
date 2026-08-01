---
id: view-closed-sub-pouch-process
type: view
title: "Do-not-re-report preamble — sub-pouch-process"
query: closed:sub-pouch-process
---
# Do-not-re-report preamble — sub-pouch-process

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-pouch-process`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH the termination history: `abort()` and mallocng's `assert`
both reached `a_crash()` — a deliberate NULL deref that, under the v1.0
fault policy, killed the KERNEL rather than the program. Both now
`_Exit(127)`. A finding that some path still reaches `a_crash()` is a
kernel-extinction finding, not a userspace one.

The `posix_spawn` fd model is the other standing shape: it resolves
file_actions STATICALLY because there is no child to run them in, so
every hole, bound, and ordering question is decided in the parent before
the syscall.

<!-- generated:begin -->
0 closed findings on [[sub-pouch-process]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

<!-- generated:end -->
