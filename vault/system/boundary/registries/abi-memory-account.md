---
id: abi-memory-account
type: abi
title: "The memory account call: its number, operations and records"
kind: contract
stability: append-only
pinned-by: ["tools/test-account-abi.py"]
mirrors:
  - kernel/include/thylacine/account.h
  - kernel/include/thylacine/syscall.h
  - usr/lib/libt/include/thyla/account.h
  - usr/lib/libt/include/thyla/syscall.h
  - usr/lib/libthyla-rs/src/account.rs
  - usr/lib/libthyla-rs/src/lib.rs
created: 2026-11-03
updated: 2026-11-03
---
## Layout / semantics

**Reserved; no kernel code is behind it.** The contract is
`docs/SHARED-MEMORY-ACCOUNTING.md` section 13. `SYS_ACCOUNT` is 140, with no dispatch arm
until MM-1, so it answers as an unknown number. The self selector is -1. Version is 1.

Operations: QUERY 1, CHILD 2, SET_LIMITS 3, VOUCHER_CREATE 4, VOUCHER_CLOSE 5,
ALLOCATE_FUNDED 6. Classes: ALLOC 0, RETAIN 1, META 2, DEVICE 3, four in all. Levels:
NORMAL 0, WARNING 1, CRITICAL 2. Reasons, in section 9's order from 1: AccountLimit,
RetentionLimit, PhysicalPressure, ContiguousUnavailable, DeviceBudget, MetadataLimit,
UnsupportedLayout, PermissionDenied, StaleGrant. Zero names no reason.

All little-endian and aligned to eight. The header is 24 bytes: version, size, flags, pad
(u32 at 0, 4, 8, 12), incarnation (u64 at 16). A class is 24: used, reserved, limit (u64).
The query is 168: header, generation (u64 at 24), level and limiting (u32 at 32, 36),
headroom, reclaim target, retained after exit, awaiting device (u64 at 40, 48, 56, 64),
four classes at 72. An outcome is 40: request id (u64 at 0), class and reason (u32 at 8,
12), requested, available, limiting account (u64 at 16, 24, 32).

`tools/test-account-abi.py` compiles the real C headers to AArch64 ELF objects, extracts
a constant section, and compares it and a Rust build of the standalone `account` module
with one byte record packed by hand. The C fixture names its fields, and no field is left
zero, so a swap of two like fields shows. This checks numbers and layout, not behaviour.

## Change protocol

Update all six mirrors and the oracle in one commit, and record `abi-memory-account` in
the change note's mirrors-checked edge. Records for the other operations come with those
operations. Version and size are what lets them, so nothing here is renumbered for them.
No admission path may consume this before MM-1 has the accounts behind it.

The number's collision with Linux's rt_sigreturn is argued in [[sub-kernel-vivarium]],
and the number space is [[sub-kernel-syscall-abi]]'s.
