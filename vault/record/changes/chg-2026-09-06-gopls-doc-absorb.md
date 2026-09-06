---
id: chg-2026-09-06-gopls-doc-absorb
type: chg
title: "absorb docs/reference/137-gopls (Go LSP engine port, Stage 8d): clean redirect; the two RESOLVED kernel findings (#99/#100) verified home"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The GOOS=thylacine gopls port (Stage 8d, NOVEL #13). No kernel surface; the engine
fork is external (~/projects/gopls). Verified atom-by-atom.

WHERE EACH ATOM LIVES (verified, not assumed):
- #99 filecache open-or-create (RESOLVED): the non-atomic SYS_OPEN-then-
  SYS_WALK_CREATE race under concurrent content-addressed Sets; dev9p_create
  records the real errno (EL0 sees EEXIST not a bare -1->EPERM) AND drops the
  (parent,name) dentry + bumps the gen on create-EEXIST so a loser's retry-Open
  sees the file -> sub-kernel-ninep-dev9p (audit:hard, FS-mutation).
- #100 robustio FileID devno (RESOLVED): t_stat 80->88 with a devno field ->
  abi-t-stat (88 bytes); the shim returns FileID{device: stat.Dev, inode:
  QidPath}, closing the fail-dangerous device:0 cross-dataset collision.
- The env chain (CAP_CSPRNG_READ for crypto/rand, PATH for go, module cwd; login
  stamps SHELL_CAPS + seeds PATH) -> sub-stratum-session + sub-kernel-caps.
- The boot probe (joey: go8d OK) -> sub-kernel-joey; the LSP client (Nora, Stage
  8e) -> sub-parley.

Zero-fold. The gopls fork (2 //go:build-unix-gap shims, telemetry disabled, the
teardown-segv #98 disposition -- did not reproduce under full-env) is external,
not a Thylacine-tree surface. The build-time lesson (per-mirror _Static_assert
checks only its own size, not vs the kernel -- a stale 80-byte mirror overflows,
caught by a boot segv not the build) is carried with the t_stat ABI. Redirect stub.
