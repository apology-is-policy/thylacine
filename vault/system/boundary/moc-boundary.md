---
id: moc-boundary
type: moc
title: "Boundary"
parent: home
created: 2026-08-01
updated: 2026-08-01
---
The surfaces that are deliberately neither kernel nor userspace: the ABIs
both halves mirror, the wire protocols they speak, the registries that
pin their numbers, and the translation layers that make a foreign
contract meet a Thylacine one. A boundary note's distinguishing property
is that **two independent implementations must agree** — so its dossiers
carry the agreement obligations (the mirrors, the static asserts, the
drift gates), not just one side's mechanism.

## Children

- [[moc-pouch-seam]] — pouch, the POSIX libc's boundary line: where
  Linux/POSIX shapes are translated into Thylacine syscalls (the first
  populated boundary area).

## Cross-cutting

- The **mirror discipline** is this plane's recurring hazard: a struct or
  number duplicated in the kernel, `libt`, `libthyla-rs`, a pouch patch,
  and a language fork drifts silently, because a per-mirror
  `_Static_assert(sizeof == N)` verifies only THAT mirror against itself,
  never against the kernel. The `struct t_stat` growth to 88 bytes (#100)
  is the worked failure; see [[sub-pouch-fs]].
- The syscall-abi / ninep-wire / exec-contract / registries areas land
  with their own sweeps.
