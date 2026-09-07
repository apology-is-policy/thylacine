---
id: chg-2026-09-07-vivarium-doc-absorb
type: chg
title: "absorb docs/reference/145-vivarium (the Linux-compat pole): fold the ^C-mask container behavior into sub-viv"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: [sub-viv]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
VIVARIUM (the Linux-binary-compatibility pole, I-43), the 3792-line master
reference. The DEDICATED PASS from the resume note. quaestor owner:
kernel/vivarium.{c,h} -> sub-kernel-vivarium (audit:hard, I-43, fresh 2026-09-05);
usr/viv -> sub-viv (audit:light, I-43/I-23, dated 2026-08-06); the diorama already
retired (141-diorama). Verified atom-by-atom (verify-by-atom, not a full 3792-line
read).

RESIDUE FROM THE RESUME NOTE, checked:
- per-note phenotype-sigtab gate (c8ab2744) -> COVERED (sub-kernel-vivarium [4
  hits] + sub-kernel-notes). Resolved.
- V-8 findings -> COVERED (sub-kernel-vivarium, 5 hits).
- T1/T2 translation -> COVERED (6 hits).
- DISTRO D-1..D-4 -> COVERED distributed: D-1 symlink -> sub-kernel-stalk; D-2
  ET_DYN -> sub-kernel-elf; D-3 file-backed mmap -> sub-kernel-vma/-fault/-image;
  D-4 PT_INTERP -> sub-kernel-exec. All fresh audit:hard.
- DISTRO D-5 -> a build/test bundle (/vivarium/alpine-stock + the viv-run E2E arc
  gate), staged by sub-substrate-build + gated by the runner E2E; NOT a kernel
  mechanism, no dossier owed. Noted.

THE FOLD (genuine gap -> sub-viv, depth rich; updated 2026-08-06 -> 09-07): the
^C-mask CONTAINER behavior (landed 2026-08-18, postdates the dossier). Code-
verified anti-hollow (proc.c:1614 `if (parent->phenotype == PHENO_LINUX)
{ ct->note_mask = t->note_mask; }`): viv runs in ut's foreground pgrp with its
diorama + every container Proc, so a ^C posts interrupt to the whole group and --
before the fix -- killed the native viv/diorama (uncaught interrupt, LS-5 default),
orphaning the container. The fix: viv masks interrupt at startup; nothing leaks in
because a native child starts ZERO-mask (rfork copies note_mask only when parent
PHENO_LINUX; native exec reset zeroes it); the tty family stays UNMASKED so ^Z
stops viv WITH the container (ut's wait_pid(WUNTRACED)), hangup ends it, ^\
detaches like docker run; the diorama masks BOTH families. Rests on: the terminate
latch armed regardless of mask, both consumers (EL0 tail + #811 sleep predicate)
honour the per-thread mask. Folded as a Mechanism subsection.

Master-reference multi-redirect (dispatch -> sub-kernel-vivarium; world ->
sub-diorama; runner -> sub-viv; DISTRO arc -> stalk/elf/vma/fault/image/exec).
Render + lint verified. Zero code change.
