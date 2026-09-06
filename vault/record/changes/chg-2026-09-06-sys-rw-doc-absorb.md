---
id: chg-2026-09-06-sys-rw-doc-absorb
type: chg
title: "absorb docs/reference/53-sys-rw (SYS_READ/SYS_WRITE byte I/O): zero-fold, 3-surface redirect stub"
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
The byte-I/O syscall reference (242 lines, P5-fd-rw). Verified atom-by-atom; zero
fold (sub-kernel-syscall-dispatch's literal title is "the staging tiers", warm
from two prior folds).

HOMES: sys_read/write_handler + the RIGHT_READ/WRITE gates + user-VA validation +
the two-tier bounce staging (stack scratch + the CF-3 heap tier to 128 KiB
SYS_RW_MAX) -> sub-kernel-syscall-dispatch; uaccess_store_u8/load_u8 (fault-fixup)
-> sub-kernel-uaccess; the handle rights -> abi-handle-rights.

WHAT THE DOC GOT WRONG: SYS_RW_MAX is 128 KiB now, not 4 KiB (CF-3 A added the heap
staging tier); content distributed to syscall-dispatch (staging/handlers) +
uaccess (byte primitives; uaccess also flags the stale "only uaccess_load_u8"
header comment this doc's era left).

ZERO fold. Render clean; lint 0-fail. view-absorption 82 -> 83.
