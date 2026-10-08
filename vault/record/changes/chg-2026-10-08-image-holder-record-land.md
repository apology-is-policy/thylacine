---
id: chg-2026-10-08-image-holder-record-land
type: chg
title: "capmark's land: the holder record's verification, which its change note left as an unfilled marker"
date: 2026-10-08
arc: arc-boosty
commits: ["82381492f"]
touched:
  - sub-kernel-proc
  - sub-kernel-devproc
established: []
closed: []
opened: []
mirrors-checked: []
supersedes: chg-2026-10-08-image-holder-record
depth: skeletal
created: 2026-10-08
---
The land of [[chg-2026-10-08-image-holder-record]] (the image's record of
departed holders, [[dec-2026-10-08-image-holder-record]]). That note was
committed before its gates ran, so its body froze with its Verification ending
in a literal `@@FILL-AT-LAND@@` marker; the record plane is append-only, so the
verification lives here.

**Verification.** RED in its own worktree (06:54-07:10Z): five sabotage runs,
each red at the tests predicted and nowhere else; R1 and R2 fired the "not
empty" guard one line above the equality predicted, which reads the same record
at the same moment, so it names the same site; base and green 1959/1959. On
82381492f (code 04d32797b): suite 1959/1959 (main's 1955 plus four new
witnesses in [[sub-kernel-devproc]]); test-fault 8/8; ci-smp-gate N=10, 50/50
over default-smp1/4/8 and ubsan-smp4/8, no corruption (07:11-08:33Z). No spec
models the join or the record. The ZOMBIE transition's modelled steps
([[sub-kernel-proc]]) are unchanged, since the record is one OR ahead of them,
so no buggy cfg is owed. `ls-ci` was not run: nothing in userspace, the gate
image or an interactive surface changed, and the B-2 land ran it on 4b48cb0f6.
