---
id: chg-2026-09-05-jobctl-self-stop
type: chg
title: "sub-kernel-jobctl brought current: the #15/#240 self-stop (proc_job_stop_self + susp_stop_armed) -- a third stop source that predated the dossier; post-08-16 churn borrowed"
date: 2026-09-05
arc: arc-vault
commits: ["e0ae7d07"]
touched:
  - sub-kernel-jobctl
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The last proc.c-cluster sibling. The stale flag is the same shared proc.c churn
that flagged [[sub-kernel-proc]], [[sub-kernel-caps]], and [[sub-kernel-death]]
this session -- and on the job-control-owned surface it is BORROWED, exactly as
the dossier's two prior cotenancy re-verifies recorded.

## The post-08-16 churn is borrowed (verified function by function)

The six owned entry points (`proc_job_stop_pgrp` / `_cont_pgrp` / `_stop_proc` /
`_cont_proc` / `proc_orphan_rule_locked` / `proc_pgrp_in_session`) were checked
against the current tree with git's function-aware `-L`: every one is unchanged
since 2026-08-14, predating the dossier's 2026-08-16 update. The session and
group ids are still at offset 304/308 (unchanged since
[[chg-2026-08-15-stale-by-cotenancy]]).

The one post-08-16 change on an adjacent line is `proc_mark_self_managing_notes`
gaining a Design-D exec-clear at `5f2d4ded` (the mark is the image's, dropped at
every execve so a Linux image cannot inherit a native image's delivery-off bit).
That is the mark's LIFECYCLE, owned by [[sub-kernel-proc]] / [[sub-kernel-exec]];
this dossier describes only the READ POLARITY of the self-managing query (the
"one predicate, two polarities" section), and that is unchanged. Borrowed.

## But a pre-existing gap was closed (a de-stale, not a pure re-verify)

The surface review found a gap that PREDATED the dossier. The `#15`/`#240`
self-stop landed 2026-08-13 (`434c3fd9`) and 2026-08-14 (`17e8b2b8`), days before
the dossier's first write, and was never captured:

- `proc_job_stop_self(m)` -- a THIRD stop source, distinct from the terminal fan
  and the `/proc` verbs, called from `kernel/notes.c` (the `SYS_NOTED(NDFLT)`
  path and the delivery of a `tty:susp` a masking process opted to receive). The
  Contract listed six entry points where there were seven.
- `susp_stop_armed` (offset 348, born with `#240`) -- the freshness guard the
  self-stop reads. Data-structures read "three fields" for four.

Its defining property is that it does NOT re-run the catchability gate: the gate
was already asked and answered "caught", so re-asking would read the live handler
and refuse the very stop the caller requested via `NDFLT` (the `#15` ignore bug).
It replaces the gate with two premises read under the process-table lock --
freshness (`susp_stop_armed`, cleared by a continue that overtook the suspend --
the `#240` cont-cancels-a-pending-stop fix) and not-orphaned (`pgrp_orphaned_-
locked`, the same POSIX suppression the fan applies, re-checked because carrier
loss or shell death may have orphaned the group since post time). If it stops it
reuses `proc_job_stop_one_locked` (so the report latches fire) and issues the one
reschedule broadcast. The dossier's own third-stop-source caveat had anticipated
this abstractly while the source already existed.

Edits: added the Contract row + the caller note, a "The self-stop" Mechanism
subsection, the fourth Data-structures field, and rewrote the third-stop-source
caveat to record what the source decided. A coverage gap rather than drift -- but
the currency bar is the same. `updated:` -> 2026-09-05; guarded-by unchanged
[inv-i20, inv-i39, inv-i9, inv-i19].

## Surfaced, not fixed (a vivarium-arc code fix)

`kernel/include/thylacine/proc.h` still carries "struct Proc stays 352" in the
comment beside `debug_exitkill`, while the live
`_Static_assert(sizeof(struct Proc) == 392)` is correct -- VIVARIUM's phenotype
fields grew the struct after that comment was written. The compile-time assert is
sound (the build is fine); only the prose comment drifted, so it belongs to the
[[sub-kernel-vivarium]] code arc, not a vault edit. Surfaced to that owner rather
than fixed here.
