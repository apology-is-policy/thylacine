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
2 closed findings on [[sub-pouch-process]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-noexec-r1-f10]] [P3] The mirror checker did not pin T_E_NOEXEC (fixed) — Added.
- [[fnd-noexec-r1-f2]] [P1] The Pouch witness could not pass where joey runs it (fixed) — The legs moved to pouch-hello-fs, which runs post-pivot on the writable pool, spawn half only. L369 and L370 now take bundle-supplied fixtures, so a refused create cannot read as anything.
<!-- generated:end -->
