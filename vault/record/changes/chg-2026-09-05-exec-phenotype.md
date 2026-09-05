---
id: chg-2026-09-05-exec-phenotype
type: chg
title: "sub-kernel-exec brought current: the phenotype threaded, the interpreter rewrite, the pheno-mount resolver"
date: 2026-09-05
arc: arc-vault
commits: ["f590e607"]
touched:
  - sub-kernel-exec
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The exec portion of aux's VIVARIUM Design D ring (yip 0036), done as a full
de-stale of [[sub-kernel-exec]] rather than a Design-D-only patch -- every
`kernel/exec.c` change since the dossier's 2026-08-18 date is now covered, so
the `updated:` bump is honest.

## What it covers

- **Design D (VIVARIUM 13.10.4) -- the phenotype threaded, not read.**
  `exec_load_into`/`exec_load_body` gained a `u32 pheno` parameter, decided at
  the resolver before the load. The load-bearing consumer is HERE: **Leg C** --
  `exec_load_body` decides the `PT_INTERP` dispatch on the parameter, so a
  native caller `execve`ing a *dynamic* `/viv/bin` binary (decided Linux at the
  resolver) takes the rewrite path instead of the pre-commit `nsp->phenotype`
  (still native) hitting the "dynamic Linux rejected" refusal. **Leg B** is why
  the loader never *writes* `nsp->phenotype` (a failed load returns to the
  surviving old image); the commit is [[sub-kernel-proc]]'s, and its Legs A/B
  are owed there.
- **DISTRO D-4 -- the `PT_INTERP` interpreter rewrite.** A pre-existing stale
  Seam ("Dynamic linking is refused permanently") was overtaken: a `PHENO_LINUX`
  `PT_INTERP` binary runs by name via the interpreter rewrite; what stays
  refused is a dynamic *interpreter* and any dynamic *native* binary. Corrected.
- **The pheno-mount resolver.** `exec_resolve_from_namespace_ex` reports the
  `MPHENO_LINUX` crossing via an optional `*pheno_out` (only
  `SYS_SPAWN_FULL_ARGV` passes non-NULL).
- The extinction-round diagnostics (`exec_report_fail`/`exec_say` ->
  `cons_diag_line`, global cap) were already covered by the dossier's existing
  2026-08-18 section, so nothing there changed.

I-43 was added as prose (a *consumer* note), not to `guarded-by`: exec reads the
decided phenotype to shape the load and confers no authority, so the enforcement
half of I-43 stays at the fork cap-strip.

## What is still owed (the other 5 of aux's 0036)

sub-kernel-{proc,territory,stalk,vivarium,syscall-abi} remain. They are NOT
Design-D-only patches: each is stale for MULTIPLE features
(proc.c alone has churned across ~10 -- socktab-across-images, vfork, the
notes/job-control line, #91 exit(N), phenotype-fork-inherits-caps, ...; territory
+ stalk carry the whole union-mounts arc). Each is its own full de-stale, budgeted
for a fresh context. exec was the tractable one (4 churn commits, all covered).
