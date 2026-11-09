---
id: chg-2026-11-09-account-estale
type: chg
title: "A memory account named by another account's incarnation answers ESTALE (vote 76)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched: [abi-memory-account, abi-errno, sub-kernel-accounts]
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/account.h, kernel/include/thylacine/syscall.h, usr/lib/libt/include/thyla/account.h, usr/lib/libt/include/thyla/syscall.h, usr/lib/libthyla-rs/src/account.rs, usr/lib/libthyla-rs/src/lib.rs, usr/lib/libthyla-rs/src/err.rs, usr/lib/pouch/patches/0001-pouch-syscall-seam.patch]
depth: skeletal
created: 2026-11-09
---
Operator vote 76, before MM-1c lands. `sys_account_op` and the spawn account tail answered `-T_E_INVAL` for an expected incarnation that was not the account's; they now answer `-T_E_STALE`, so a caller can tell a handle number closed and reused under it from a malformed record. An incarnation of 0 on a handle stays `-T_E_INVAL`: 0 is the self selector's word and names no account, so it is malformed rather than stale. Acceptance is unchanged (no account has incarnation 0).

The kernel's own `SYS_ACCOUNT` and `SPAWN_EXT_ACCOUNT` comments list the new answer. Of the mirrors, none lists the incarnation answer: libt's and libthyla-rs's syscall wrappers return the negated errno raw, `err.rs` already maps 116 to `StaleFile`, and the pouch seam is a range contract. capacity-probe's legs `account-handle-stale`, `account-self-named-wrong` and `account-spawn-stale` now expect `ESTALE`; `account-handle-unnamed` and `account-spawn-unnamed` keep `EINVAL`. SHARED-MEMORY-ACCOUNTING section 14 and ERRORS's 116 row say so. Written only: not yet compiled or booted; the next MM-1c lease (its mutants) boots it.
