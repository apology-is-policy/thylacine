---
id: adt-b1d-v-r1
type: adt
title: "B-1d-v round 1: Emount at the syscall -- a success path that needed lib/, and a flagless mount the vote's wording missed"
date: 2026-09-25
scope: [sub-kernel-territory, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, spec-territory, sub-kernel-joey, sub-kernel-stalk, sub-libthyla-rs, sub-haul, sub-viv]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 2, p3: 5}
findings: [fnd-b1d-v-r1-f1, fnd-b1d-v-r1-f2, fnd-b1d-v-r1-f3]
round-of: chg-2026-09-25-b1d-v-emount
created: 2026-09-25
---
## Scope

WIP 1 (e10c6575, unbuilt at the round): `sys_mount_for_proc`'s Emount check,
the two kernel tests, alloc-smoke's U-2f leg turned into refusal checks,
`libthyla_rs::territory::mount`'s errno pass-through, `territory.tla`'s
`DirPoint` guard and its `file_point` pin, joey's five EXTINCTION bodies and
the prose. Opus 5.5 at max, the fallback tier (Fable died on HTTP 429,
credits): a context-independent read sharing the implementer's family.

## Convergence

0 P0 / 0 P1 / 2 P2 / 5 P3, a clean close by count and by shape; the main
session's parallel self-audit found F2 (its S1) and F7 (its S4) independently,
and two more P3s (a dropped `bind_*` caller set, a stale stalk sentence). F1
([[fnd-b1d-v-r1-f1]]): the new directory success path needed the initrd's
`lib/`, which only an LLVM-fork image ships. F2 ([[fnd-b1d-v-r1-f2]]): a
flagless mount at a file point still stacked members, which went to the
operator. F3 ([[fnd-b1d-v-r1-f3]]): the spec's guard pinned nothing. F4 (stale
comments and docs), F5 (test gaps) and F6 (haul and viv discarded the named
errno; the trailing-slash spelling answers -1) were fixed or documented; F7 (a
directory can no longer be mounted over a symlink point) was decided in the
main session and recorded in ARCH 9.6.1 and manual 14. Verified sound: the
check's placement after the lookup and before `mount()`, the reference release
on the refusal, the unchanged `mount()` layer, and that no tool matches the
renamed EXTINCTION bodies.
