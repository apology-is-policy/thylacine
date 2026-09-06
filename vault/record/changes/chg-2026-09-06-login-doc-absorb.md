---
id: chg-2026-09-06-login-doc-absorb
type: chg
title: "absorb docs/reference/103-login (A-5a login+session): zero-fold, multi-redirect"
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

# docs/reference/103-login.md -> ABSORBED (A-5a)

Absorbed the 354-line login reference doc into a multi-redirect stub. Verified:
/sbin/login + the per-user encrypted-home DEK + home bind + !recover ->
sub-stratum-session (owns usr/login); joey the supervisor -> sub-kernel-joey
(kernel half) + sub-stratum-boot (getty loop); the three syscalls
(SYS_CONSOLE_OPEN=64/RELINQUISH=63 -> sub-kernel-cons/devdev I-27;
SYS_BOOT_COMPLETE=62 -> abi-boot-banner + joey); recovery -> sub-corvus. Zero fold.

97 -> 98 absorbed of 157. lint 0-fail.
