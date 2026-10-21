---
id: dec-2026-10-07-close-eio
type: dec
title: "close(2) returns EIO when the write-behind flush fails; Dev.close returns int"
date: 2026-10-07
status: standing
decided-by: user-vote
affects: [sub-kernel-dev, sub-kernel-ninep-dev9p, sub-kernel-syscall-abi]
created: 2026-10-07
---
## Fork

dev9p stages writes behind (the F1 write-behind run) and flushes them at
fsync and at close. A flush failure latches `wb_err` on the open file, and
every later write and fsync returns it (the voted NFS error model). But
`Dev.close` returned `void` and `close(2)` returned only 0 or `EBADF`, so a
program that wrote and closed without fsync never learned its data was lost.

## Research

- **POSIX.** `close` may fail with `EIO`; the descriptor is closed anyway.
- **Linux.** NFS reports writeback errors from close (`nfs_file_flush`);
  local filesystems do not, and programs that care call fsync.
- **Plan 9.** No write-behind in the mount driver, so the heritage does not
  answer it; Plan 9's `Dev.close` returns `void`.

## Options

1. **close returns EIO.** `Dev.close` returns an error; `close(2)` returns
   `EIO` for a failed write-behind flush after closing the handle.
2. **fsync stays the only report**, as on a local Linux filesystem.

## The call

Option 1 (operator, 2026-10-07, AskUserQuestion). It lands with the tag-pool
chunk ([[dec-2026-10-07-tag-pool]], ARCH 21.11, stage TP-4). ARCH 9.2's `Dev`
gives `close` an `int` result; ERRORS.md adds the `SYS_CLOSE` row.

## Rationale

The write-behind is Thylacine's, so its loss is Thylacine's to report, and
close is the one call every writer makes. `close(2)` reports `EIO` rather
than the latched errno to keep close's POSIX error set; write and fsync still
return the specific errno.
