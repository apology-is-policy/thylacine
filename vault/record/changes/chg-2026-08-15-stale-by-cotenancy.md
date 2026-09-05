---
id: chg-2026-08-15-stale-by-cotenancy
type: chg
title: "Two dossiers verified current without change: churn measured per file, not per surface"
date: 2026-08-15
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-caps, sub-kernel-jobctl]
established: []
closed: []
opened: []
mirrors-checked: [kernel/proc.c, kernel/include/thylacine/proc.h]
depth: rich
created: 2026-08-15
---
Follows [[chg-2026-08-15-proc-lineage]], which swept the two dossiers on
`kernel/proc.c` whose material actually moved. These are the other two, and the
result is that neither needed touching.

## What was checked

[[sub-kernel-caps]] and [[sub-kernel-jobctl]] both sat in the top six of the
churn list, at ~910 and ~1440 lines moved. A word-bounded diff over every
capability and job-control token, across the whole interval and both files,
found:

- **caps** — one comment line, plus two call sites gaining a trailing parameter.
  `CAP_NONE` unchanged at each.
- **jobctl** — zero semantic changes. `kernel/proc.c` had none at all, and every
  hit in the header was an offset shift: the session and group ids moved from
  336/340 to 304/308 because the address-space extraction shrank `struct Proc`
  out from under them.

Both are now dated current with the verification written into their Provenance,
because "I verified nothing moved" is a real sweep result and leaving them
flagged means the next session redoes this work.

## Why this is a finding and not bookkeeping

Neither dossier's material moved. They were stale because they share a file with
a surface that churned, and `quaestor stale` measures churn on the **file**, not
on the surface a dossier owns. For a multi-dossier file the number is borrowed,
and here two dossiers borrowed almost all of theirs.

That matters because the churn ordering *is* the sweep's priority queue — it is
what makes "churn-first" a strategy rather than a slogan. It remains the right
first approximation, since the file really did change and something had to check.
But a borrowed number does not merely waste a slot: it displaces a dossier whose
material genuinely moved. Two of the visible top six had nothing to sweep.

Filed as task #176 with three fix shapes. The cheap honest one is to report
co-tenancy in the stale line ("shared with 3 other dossiers") so a reader can
discount it, rather than inventing a per-dossier ownership field that would
itself go stale — the failure the vault exists to avoid.

## The check needed a second pass

The first run was word-**unbounded** and reported job-control churn that does not
exist: `sid` matched inside `ASID`, `outside` and `considered`. A three-letter
token grepped without boundaries is a detector reporting its own noise, and it
would have produced exactly the wrong conclusion — "job control changed, go read
it" instead of "it did not". Caught by reading the matched lines rather than the
count.

## One detail kept, because it inverts the usual lesson

Among the offset shifts: a *summary* assertion message that had spelled the
offsets out numerically was rewritten to name the fields without numbers, while
the individual per-field asserts were updated to the new values.

Dropping a number is normally how a proof goes stale. Here it removed the only
copy that had to be maintained by hand, three lines above the copies the compiler
checks.

## Process note

This note exists because rule R3 caught the alternative. The material was first
appended to [[chg-2026-08-15-proc-lineage]] after that note had been committed —
a Record-plane body edit, which the pre-commit lint refused. It was the right
refusal twice over: the append was against the rule, and it was also the wrong
shape, since this was a distinct sweep action on two different dossiers and
deserved its own note rather than a retroactive widening of an existing one.
