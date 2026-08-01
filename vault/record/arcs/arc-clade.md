---
id: arc-clade
type: arc
title: "Clade: the on-device LLVM/clang toolchain"
status: active
design: ["docs/LLVM-DESIGN.md"]
chunks:
  - chg-2026-07-24-getcwd-oversized
follow-ons: []
created: 2026-08-01
---
## Goal

Build and run clang/LLVM ON the device — the compiler as a Thylacine
program, not a cross-compiler on the host. CL-1 wired the pouch FS and
process syscalls clang needs; CL-4 the linker; CL-7 the JIT
(`CAP_JIT` + the dual-mapped code Burrow, [[inv-i28]]'s W^X sibling)
with llvmpipe as the first consumer.

Its value to the rest of the tree is as an ORACLE. A real toolchain
drives POSIX surfaces no hand-written probe thinks to drive, in
combinations no test fixture produces, at a scale that finds the
timing-shaped bugs. `make -j3` on device is the single harshest thing
the kernel is asked to do, and several kernel fixes in other arcs were
found by it rather than by anything aimed at them.

## Planned chunks

The vault has recorded only the one kernel-side fix this arc surfaced on
the territory surface. The arc's own chunks (CL-1 through CL-7b) join at
the clade sweep.

- [[chg-2026-07-24-getcwd-oversized]] — the `getcwd(buf, PATH_MAX)`
  reject, found by the CL-1c `make` oracle.

## Close summary

(pending)
