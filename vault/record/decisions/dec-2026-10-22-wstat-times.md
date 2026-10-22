---
id: dec-2026-10-22-wstat-times
type: dec
title: "Times ride SYS_WSTAT as 9P's four bits, and SYS_WSTAT answers an errno per refusal"
date: 2026-10-22
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-kernel-perm, sub-kernel-ninep-dev9p, sub-kernel-dev, sub-kernel-vivarium]
created: 2026-10-22
---
## Fork

[[dec-2026-10-22-g12-pulled-forward]] needs a kernel call that sets a
file's atime and mtime, by fd and (through `O_PATH`, as chmod does) by
path. SYS_WSTAT (59) answered a flat -1 for every refusal (OPEN-BUGS
2026-10-20 08:24Z).

## Research

- **Plan 9 (heritage).** wstat(5) carries mtime with mode in one
  metadata message; the owner or the group leader may change both.
- **9P2000.L.** Tsetattr's mask has ATIME and MTIME (the server's clock)
  and ATIME_SET and MTIME_SET (the client's values in the message). The
  Linux v9fs client maps `utimensat` onto it; QEMU's virtfs reads it the
  same way.
- **POSIX `utimensat`.** An explicit time needs the owner or privilege
  (EPERM); "now" also admits write permission (EACCES); nsec outside
  [0, 999999999] is EINVAL.
- **The tree.** `struct p9_setattr` already carried the four time fields
  and the kernel's `T_WSTAT_*` bits already equalled `P9_SETATTR_*`.

## Options

1. **Extend SYS_WSTAT**: the four bits, x6 pointing at two
   `struct t_timespec` (atime, mtime), and an errno per refusal.
2. A new `SYS_UTIMENS` = 137 (fd), errno from birth; SYS_WSTAT untouched.
3. A new path call `SYS_UTIMENSAT` = 137, resolver-based.

## The call

Option 1 (operator, 2026-10-22, the recommended option). Bits 4/5
(`T_WSTAT_ATIME` / `MTIME`, the server's clock) and 7/8 (`_SET`, the
caller's values); bit 6, the wire's CTIME, stays reserved. A `_SET` bit
without its time bit is EINVAL. x6 is read only when a `_SET` bit asks.
Permission as POSIX: an explicit time needs the owner or CAP_HOSTOWNER,
"now" also admits write permission. No new number.

Two details were settled from the code after the vote, and recorded with
the implementation:

- **The kernel's denial is EACCES, not EPERM.** `-T_E_PERM` is -1, the
  alias the Pouch seam reads as EIO (`<thylacine/errno.h>`, ERRORS.md),
  so no syscall may return it. The POSIX front ends (Pouch, the
  vivarium, go-thylacine) answer EPERM where ownership is the only
  possible refusal: chmod, chown, an explicit time. "Now" keeps EACCES,
  as POSIX has it.
- **A Dev with no metadata slot answers EOPNOTSUPP, not EROFS**, and so
  does dev9p's fidless cached open. The registry names no EROFS, and
  appending one is an ERRORS.md signoff of its own; a server's EROFS (a
  Stratum snapshot) still crosses by value.

## Rationale

One metadata call is the Plan 9 shape, and the wire already had the four
bits with the same values, so dev9p maps the mask with no translation.
The errno conversion was owed anyway (ER-2), and a new call would have
left SYS_WSTAT's flat -1 standing beside it.
