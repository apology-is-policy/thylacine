---
id: chg-2026-11-04-mm1c-audit-r1
type: chg
title: "MM-1c audit r1's close"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched: [abi-memory-account, abi-errno, sub-kernel-accounts, sub-kernel-handle]
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/account.h, kernel/include/thylacine/syscall.h, usr/lib/libt/include/thyla/account.h, usr/lib/libt/include/thyla/syscall.h, usr/lib/libthyla-rs/src/account.rs, usr/lib/libthyla-rs/src/lib.rs, usr/lib/libthyla-rs/src/err.rs, usr/lib/pouch/patches/0001-pouch-syscall-seam.patch]
depth: skeletal
created: 2026-11-04
---
MM-1c's first audit round. Refusals that were `-T_E_PERM`, which ERRORS forbids a handler since it reads as the kernel's flat -1, are `-T_E_ACCES`. The spawn tail grew to 16 bytes and carries the account's incarnation, as an operation's record does, so a number closed and reused cannot steer a child. An exec closes the handle whatever its flag says. The warden's tree joins the services account, as the manual already said. One finding is open: what `headroom` tells a sandbox with its own child, which needs the operator ([[chg-2026-11-04-mm1c]]).
