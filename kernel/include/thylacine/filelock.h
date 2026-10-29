// SYS_FILE_LOCK -- byte-range locks held by the file's server (B-3h).
//
// ARCHITECTURE.md 9.6.11 (operator votes 34-35, dec-2026-10-29-b3h-file-locks).
// The kernel keeps no lock table: a lock is forwarded through the file's Dev
// (dev9p: 9P2000.L Tlock / Tgetlock). Two owners: the open file (the Spoor's
// own fid) and the Proc (an entry kept with its handle table, handle.h).

#ifndef THYLACINE_FILELOCK_H
#define THYLACINE_FILELOCK_H

#include <thylacine/types.h>

// The SYS_FILE_LOCK handler: (fd, op, start, len, out_va) -> 0 / -errno. The
// contract is at SYS_FILE_LOCK in <thylacine/syscall.h>.
s64 sys_file_lock_handler(u64 fd_raw, u64 op, u64 start, u64 len, u64 out_va);

#endif // THYLACINE_FILELOCK_H
