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
updated: 2026-11-04
---
## Layout / semantics

**QUERY, CHILD and SET_LIMITS are live; the voucher operations are reserved.** The contract
is `docs/SHARED-MEMORY-ACCOUNTING.md` sections 13 and 14. `SYS_ACCOUNT` is 140. The self
selector is -1, sign-extended to the register. Version is 1. Amounts are bytes.

The checks run in this order: `-EINVAL` for an operation outside 1 to 6; `-EOPNOTSUPP` for
4 to 6; `-EINVAL` for a `rec_size` other than the operation's record; `-EPERM` for SET_LIMITS
on the self selector; `-EBADF` for a handle that is not a `KOBJ_ACCOUNT`; `-EFAULT` for a record
that is not the caller's readable and writable memory, after which any part of it may have
been written; `-EINVAL` for a wrong version, size, flags or pad, or an incarnation that is
not the account's, where 0 is taken only with the self selector.

QUERY returns 0 and replaces the record. CHILD returns the new handle, close-on-exec, with
rights read and write, and replaces the record: the header's incarnation is the parent's and
`created` the child's. It answers `-ENOSPC` at depth 16, `-ENOMEM` with no pool or memory,
`-EMFILE` with a full table, and `-EFAULT` having closed the handle when the record cannot be
written. SET_LIMITS returns 0 having replaced every limit, or `-EPERM` with none replaced when
one is above an ancestor's. A limit is rounded down to a page, and `ACCOUNT_LIMIT_NONE`, all
ones, clears it.

Operations: QUERY 1, CHILD 2, SET_LIMITS 3, VOUCHER_CREATE 4, VOUCHER_CLOSE 5,
ALLOCATE_FUNDED 6. Classes: ALLOC 0, RETAIN 1, META 2, DEVICE 3, four in all. Levels:
NORMAL 0, WARNING 1, CRITICAL 2. Reasons, in section 9's order from 1: AccountLimit,
RetentionLimit, PhysicalPressure, ContiguousUnavailable, DeviceBudget, MetadataLimit,
UnsupportedLayout, PermissionDenied, StaleGrant. Zero names no reason.

All little-endian and aligned to eight. The header is 24 bytes: version, size, flags, pad
(u32 at 0, 4, 8, 12), incarnation (u64 at 16). A class is 24: used, reserved, limit (u64).
The query is 168: header, generation (u64 at 24), level and limiting (u32 at 32, 36),
headroom, reclaim target, retained after exit, awaiting device (u64 at 40, 48, 56, 64),
four classes at 72. The child record is 32: header, created (u64 at 24). The limits record
is 56: header, four limits (u64 at 24). An outcome is 40: request id (u64 at 0), class and
reason (u32 at 8, 12), requested, available, limiting account (u64 at 16, 24, 32).

`tools/test-account-abi.py` compiles the real C headers to AArch64 ELF objects, extracts
a constant section, and compares it and a Rust build of the standalone `account` module
with one byte record packed by hand. The C fixture names its fields, and no field is left
zero, so a swap of two like fields shows. This checks numbers and layout, not behaviour.

## Change protocol

Update all six mirrors and the oracle in one commit, and record `abi-memory-account` in
the change note's mirrors-checked edge. Records for the voucher operations come with those
operations. Version and size are what lets them, so nothing here is renumbered for them.
No admission path consumes the query: section 4 has admission recheck live state.

The number's collision with Linux's setpriority is argued in [[sub-kernel-vivarium]],
and the number space is [[sub-kernel-syscall-abi]]'s.
