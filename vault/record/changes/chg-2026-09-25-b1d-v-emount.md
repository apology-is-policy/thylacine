---
id: chg-2026-09-25-b1d-v-emount
type: chg
title: "B-1d-v: SYS_MOUNT refuses Plan 9's Emount (only MREPL at a file), joey's extinction bodies name bin/joey, the musl loader is secure-always; a per-post /srv qid.path"
date: 2026-09-25
arc: arc-boosty
commits: ["46d943c5"]
touched:
  - sub-kernel-territory
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-stalk
  - sub-kernel-devsrv
  - sub-kernel-devproc
  - sub-libthyla-rs
  - spec-territory
established: []
closed: [fnd-b1d-v-r1-f1, fnd-b1d-v-r1-f2, fnd-b1d-v-r1-f3, fnd-b1d-v-r2-f1, fnd-b1d-v-r3-f1]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
Three operator votes after B-1d landed, and a fourth finding from the audit.
`SYS_MOUNT` refuses what Plan 9's `cmount` refuses, with `-T_E_NOTDIR`: a
source whose `QTDIR` bit differs from the mount point's under any flag, and any
mount but `MREPL` at a point that is not a directory (a flagless mount appends
here, where Plan 9's flag 0 is `MREPL`, so a second file at a file point would
make a union the resolver searches as a directory). The two votes are
[[dec-2026-09-25-mrepl-only-at-a-file]], which restates and replaces
[[dec-2026-09-25-sys-mount-emount]]; a directory over a symbolic-link point
stays refused and a trailing `/` follows the link, unless a file is mounted on
the link itself. joey's extinction bodies name `bin/joey`
([[dec-2026-09-25-extinction-bodies]]: the prefix and the six tool-matched
bodies are ABI, the rest prose), and the musl loader refuses `LD_*` always
([[dec-2026-09-25-musl-secure-loader]], docs only; static binaries stay as
found). Audit round 1 (Opus on Opus, Fable out of credits) closed 0/0/2/5, round
2 the same shape: round 2's F1 found every `/srv/<name>` node carrying the
registry root's `qid.path`, so a mount at one was keyed at the registry root and
at every other service; devsrv now stamps a per-registry `qid.path` per post.
The kernel change is verified by the suite, the sabotage legs, the SMP gate and
`territory.tla` ([[spec-territory]]). Round 3 (Fable 5.1 -- family diversity
restored) found the round-2 regression witness crossed an uncrossable source
and had never run; it is rewritten to mount-table assertions and verified red
on the reverted kernel, green on the fix. [[adt-b1d-v-r1]], [[adt-b1d-v-r2]],
[[adt-b1d-v-r3]].
