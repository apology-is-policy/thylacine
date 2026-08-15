---
id: chg-2026-08-15-syscall-dispatch-lineage
type: chg
title: "The dispatcher re-swept after LINEAGE and VIVARIUM: the prologue, the frame-taking arms, and the core split"
date: 2026-08-15
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-syscall-dispatch]
established: []
closed: []
opened: []
mirrors-checked: [kernel/syscall.c, kernel/include/thylacine/syscall.h, kernel/vivarium.c, kernel/dma_handle.c]
depth: rich
created: 2026-08-15
---
The churn-first re-sweep of the surface at the top of `quaestor stale`:
~3500 lines moved on `kernel/syscall.c` across seventeen commits since the
dossier was written, from the LINEAGE fork/exec arc, the VIVARIUM phenotype
arc, the AddrSpace extraction, Warp-2, and the #130/#131/#132 charge-attribution
rework. The file went 8178 -> 11138 lines and 100 -> 103 syscalls.

## What changed in the subject, not just in the file

Three of the dossier's structural claims were **falsified rather than merely
outgrown**, which is the distinction that made this worth a full re-read rather
than a count refresh:

- **"The dispatcher performs no validation, no capability check and no
  bookkeeping."** A phenotype prologue now runs *before* the syscall number is
  read. For a Linux-phenotype process it intercepts one frame-rewriting call,
  runs one side-effecting entry hook (a socket-table drop that must precede the
  native close), and may rewrite the number and all six argument registers in
  place — so the native switch can run on registers userspace did not set. It
  still holds no *authority* check, which is the [[inv-i43]] half, and the
  narrowed claim is now stated that way.
- **"Each arm reads its arguments from x0..x5 ... writes the result to x0."**
  Three arms now take the exception frame itself, and they share a property
  rather than an accident: the frame is the *subject* of the call. execve
  rewrites it, rfork copies it, noted restores it. That forced the dispatcher to
  abandon the uniform result-write for exactly one arm.
- **"Two layers"** is now three. execve and rfork are split at the *argument
  shape* — one core, a native front end and a Linux front end — because a Linux
  `clone` arrives in a different register order carrying garbage in three of
  them. The gate-placement rule generalises to the new shape and the source says
  so: the multi-thread gate sits in the core "so it cannot be forgotten by one"
  front end.

## What was verified rather than assumed

- 103 enum entries, 103 distinct dispatch arms, neither set with a member the
  other lacks. The 106 raw case labels include three belonging to a second,
  inner pts switch.
- The no-fallthrough claim is compiler-enforced, not observed: all three arms
  without a `return` call handlers marked `noreturn`, each with an
  `extinction()` backstop.
- Every native symbol the phenotype table can emit has a dispatch arm (three
  apparent misses are flag constants, not numbers), and a compile-time assertion
  pins the native/Linux boundary constant to the highest assigned native number.
- The two open caveats carried forward unchanged: the descriptor-name read still
  keeps the oversized-buffer rejection its sibling removed as a field-proven
  defect (#89), and the console-open gate still sits in the handler above a
  separately-callable inner (#90).

## The finding

The three DMA-family mint handlers -- plain, weave, and GPU buffer object --
are twenty-six lines each and differ in **exactly one token**: the mint function
they call. Each carries a full copy of the [[inv-i34]] CreateBegin/CreateCommit
sequence. The object layer one level down already factored the identical family
into a shared body parameterised by envelope and subtype, so the same three
members are parameterised on one side of the boundary and copied on the other --
and the copied side is the one holding the invariant.

The comments make it worse rather than better: both derived handlers say they
differ from their model "only in the size envelope and the minted subtype bit",
but the envelope is not in the handler at all. That sentence accurately
describes the factored layer, printed on top of the copy.

## Also recorded

- **Who paid is recorded, not inferred.** The detach path's refund was decided
  by VMA *shape* and is now decided by *attribution*: a claim taken before the
  drop that would free its record, discriminated on shared-out rather than on
  "does anything else still hold this" -- because the process's own other claim
  also keeps a region alive, and there the charge must stay. The failure it
  closed leaked 64 pages per closed zero-copy network flow.
- **execve's ordering**, which is the file's most consequential: detached build,
  then a commit that is the point of no return, then every stamp and every close
  *after* the last thing that can fail, then the frame rewrite last so no
  instruction of the new image observes a descriptor that should be gone.
- **Two new seams**: execve cannot report "not executable" because the errno
  registry has no such code and additions are ABI-bearing, so a shell cannot
  learn to re-run a file as a script; and the environment's bounds deliberately
  answer a different error code than the argv bounds beside them.
- **The error-convention seam propagates along the copy, not the calendar.** Of
  the three syscalls added since the last sweep, execve and rfork use errno
  throughout while the GPU-buffer mint uses the bare sentinel -- because it was
  written as a copy of the weave mint, which was a copy of the plain mint.
