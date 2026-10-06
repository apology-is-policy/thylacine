---
id: chg-2026-10-06-none-wall
type: chg
title: "A Proc running as none reaches no other Proc's state; a /proc refusal answers EACCES"
date: 2026-10-06
arc: arc-identity-detour
commits: *(pending)*
touched:
  - sub-kernel-devproc
  - sub-kernel-devctl
  - sub-kernel-perm
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
Every `/proc` owner axis compared principals, so two unrelated Procs running as
`none` were one owner: each could kill the other, debug it whenever its caps
covered the other's, and read its environment, CPU time, scheduler view and
elevation record. The 9p-sessions audit found the read half (round 1 F1); the
kill and debug halves turned up when the whole owner family was read. The
operator chose Plan 9's `nonone` ([[dec-2026-10-06-none-owns-nothing]];
IDENTITY-DESIGN's reserved ids).

No owner axis admits a none caller for any Proc but itself, and the kill gate
gained the self arm its siblings had. A none caller reads none of another
Proc's files; `/ctl/procs` lists it only its own row, and `/ctl/9p-sessions`
no row at all ([[sub-kernel-devproc]], [[sub-kernel-devctl]]). The capability axes are
unchanged. File permissions keep none's files shared, as Plan 9 does
([[sub-kernel-perm]]).

Every `/proc` authority refusal, and `/ctl`'s `kernel-base` and `kstack`, now
answers `EACCES`; they answered a bare -1, which ERRORS.md forbids for a denial
and which pouch and Go read as `EPERM`. The other failures stay -1. Kill,
suspend and debug attach ask authority before liveness, so a refused caller
learns nothing of whether its target is alive.
