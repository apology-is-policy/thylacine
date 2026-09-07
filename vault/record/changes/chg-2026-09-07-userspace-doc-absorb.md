---
id: chg-2026-09-07-userspace-doc-absorb
type: chg
title: "absorb docs/reference/38-userspace (the P4-Ia1 userspace tree + libt): clean multi-redirect (build / libt / libthyla-rs)"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
The P4-Ia1 userspace-tree + libt-runtime doc (a foundational doc documenting the
usr/ CMake+Cargo project's two-toolchain split and the C->Rust runtime transition
P4-Ia1 libt -> P4-Ic4 libthyla-rs). quaestor owner: usr/lib/libt/{src/start.S,
include/thyla/syscall.h} -> sub-kernel-syscall-abi (audit:hard, fresh 2026-09-05);
usr/Cargo.toml -> UNOWNED; libthyla-rs -> sub-libthyla-rs (audit:light, fresh
2026-09-06). Verified atom-by-atom.

ALREADY COVERED (verified): the usr/ CMake+Cargo build + the two-toolchain split +
the aarch64-thylacine target + the artifact ledger -> sub-substrate-build (which
"produces the native and Rust userspace"); libt (start.S + header SVC wrappers,
the userspace side of the syscall ABI) -> sub-kernel-syscall-abi; libthyla-rs (the
no_std _start via global_asm!, the single #[panic_handler], the syscall wrappers)
-> sub-libthyla-rs. The doc itself already documents the P4-Ic4 C->Rust handover,
so the supersession is not a lost claim.

CLEAN MULTI-REDIRECT, zero fold. A historical doc; libt(C) survives only as a
small legacy runtime for the remaining C probes, authored native userspace is now
libthyla-rs(Rust). NOTED not folded: usr/Cargo.toml (the native workspace
manifest) is UNOWNED -- a build-structure orphan belonging with the build-config
authoring backlog (the uncovered tools/ build-config surface 150-build-config also
leaves live), not with a tools/build.sh dossier. Redirect stub. Zero code change.
