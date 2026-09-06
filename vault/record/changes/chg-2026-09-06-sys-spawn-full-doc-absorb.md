---
id: chg-2026-09-06-sys-spawn-full-doc-absorb
type: chg
title: "absorb docs/reference/64-sys-spawn-full: zero-fold, multi-redirect"
date: 2026-09-06
arc: arc-vault
commits: ["b13a35a0"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/64-sys-spawn-full.md -> ABSORBED

Absorbed the 108-line SYS_SPAWN_FULL reference doc into a redirect stub. The
fds+caps union: fd inheritance + the SYS_SPAWN_FULL_ARGV successor ->
sub-pouch-process + sub-kernel-exec; the cap-subset -> sub-kernel-caps; the handler
-> sub-kernel-syscall-dispatch. Zero fold.

106 -> 107 absorbed of 157. lint 0-fail.
