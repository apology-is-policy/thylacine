---
id: adt-b1d-r1
type: adt
title: "B-1d (dlopen) round 1: burrow_map_file, the native PT_INTERP lift, libc.so, the covered union member and the devramfs tree -- clean"
date: 2026-09-25
scope: [sub-kernel-exec, sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-vivarium, sub-pouch-mem, sub-substrate-build, sub-kernel-territory, sub-kernel-stalk, sub-kernel-content]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 0}
findings: []
round-of: chg-2026-09-25-b1d-dlopen
created: 2026-09-25
---
## Scope

Branch `b1d-loader` at e4502da9, the code of WIP 6: `git diff 51edf72b
e4502da9`. `SYS_BURROW_MAP_FILE` (126) over DISTRO D-3's three cores, with the
vouching rule on every exec-capable arm and the native door's EACCES; the
PT_INTERP lift to every phenotype with a namespace; patches 0047 (file maps)
and 0048 (RELRO); the two builtins archives and `build_libc_shared`; the fork
driver's `-shared`, `-pie` and `-static-pie` refusal; B-1d-u's covered member
(`MCOVERED`, `starts_union`, `stalk_cross_src`) and its spec; the devramfs
tree (`ramfs_load_cb`, parent before child, `walk_one`'s `..`, readdir,
lookup). The round ran on Opus 5.5 at max effort, the fallback tier (the Fable
spawn answered 429), told that context independence was what it brought.

## Convergence

0 P0 / 0 P1 / 0 P2 / 0 P3: clean. One withdrawn: the Linux phenotype's two
file-map arms return the cores' `-T_E_PERM` unmapped, but 0047 routes every
musl file map through 126, so no Pouch program reaches them, and a Linux caller
reads -1 as EPERM, which is that ABI's answer. Verified sound: 126's FIXED
window confinement, the detach refusal set, argument validation, vouching on
every exec-capable arm, the eager-copy writable FIXED map and the Image cache's
`qid_vers` keying; the PT_INTERP lift (one level, the interpreter resolved in
the Proc's own namespace, the nameless spawns refusing); 0047, 0048 and the
archives; `MCOVERED` unreachable from EL0, `starts_union`'s order and
capacity, the covered entry's source reference on every path, the cycle
check's self-edge; each witness's control; the tree's refusals. The bin/
layout arrived after this round ([[adt-b1d-r2]]). The verbatim report and
dispositions are the repo's untracked `memory/audit_b1d_closed_list.md`.
