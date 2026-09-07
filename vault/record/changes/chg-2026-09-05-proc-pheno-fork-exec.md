---
id: chg-2026-09-05-proc-pheno-fork-exec
type: chg
title: "sub-kernel-proc brought current: rfork_forked_with_caps (the Linux clone), the PHENO_LINUX note-mask inherit, Design D's phenotype commit"
date: 2026-09-05
arc: arc-vault
commits: ["63f91796"]
touched:
  - sub-kernel-proc
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The fifth kernel giant this session, and the one that CLOSES the cross-refs the
dispatch and vivarium de-stales opened. The dossier was current through the
LINEAGE arc (the three-shape creation, the address-space extraction, the
exec-replace resets); the +772 proc.c churn since 2026-08-16 is the phenotype
fork+exec work, all verified against the code.

## rfork_forked_with_caps -- the Linux clone (fork-inherits-caps)

A new rfork variant (proc.c 1738) the dossier's Contract table did not list.
syscall.c 9855 calls `rfork_forked_with_caps(flags, &fc, CAP_ALL)` for a Linux
clone: a clone has no caps argument, so it passes `caps_mask = CAP_ALL` and the
child inherits the parent's FULL set -- still `& ~CAP_ELEVATION_ONLY`, so I-2's
monotonic reduction holds on the phenotype path (documented in the Contract row +
the I-2 invariant note). This is the "PHENOTYPE-FORK-INHERITS-CAPS" change (6.26);
the caller-side is [[sub-kernel-syscall-dispatch]]'s (done this session).

## The PHENO_LINUX note-mask inheritance (#127, @d3a11c8e)

Added to the rfork Inherited ledger: a PHENO_LINUX fork gives the child thread
the CALLING thread's note mask (POSIX fork's signal-mask inheritance; a native
fork keeps the zero mask, the rfork rule) AND preserves the handler-execution
snapshot, so a fork() from inside a signal handler yields a child whose saved
user context agrees with its stack -- not a KP_ZERO "not in a handler" that would
make the handler-return silent UB under musl's __restore_rt.

## Design D's phenotype commit in proc_exec_replace (@3c26339f)

`proc_exec_replace` now takes `new_pheno` (proc.c 3430) and does "the ONE store
of the new image's phenotype" (3471) in the infallible commit region. The
exec-replace section listed the three RESETS (handler entries, note mask, debug
slots) but not this COMMIT -- the store is the opposite of a reset (a value
written, the new ABI shape). New subsection documenting: the store placement
(before the swap the load can fail back to the OLD image, F1 Leg B); the RELEASE
that orders the swap+cloexec ahead of it but NOT the relaxed signal reset, so all
four (phenotype, reset-state) combinations are observable and each is a legitimate
state of ONE image. This is the commit half of Design D -- decision half in
dispatch, resolver seed in [[sub-kernel-stalk]] (both done this session). I-43
added as prose.

## Not touched

Struct Proc still 392 bytes (verified proc.h 1052). guarded-by unchanged
[inv-i1, inv-i32, inv-i33, inv-i44]; I-2/I-22/I-43 stay prose (the dossier's
established style). The jobctl/caps/death dossiers also cite proc.c's 772 churn
-- their own de-stales remain (each a separate dossier).
