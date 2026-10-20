---
id: dec-2026-10-20-native-lstat-readlink
type: dec
title: "Native SYS_READLINK and SYS_LSTAT over the phenotype's cores"
date: 2026-10-20
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-vivarium, sub-pouch-fs]
created: 2026-10-20
---
## Fork

The kernel has resolved symlinks since D-1, and the Linux phenotype
reads them: its `newfstatat` passes `AT_SYMLINK_NOFOLLOW` to the stat
core as `STALK_NOFOLLOW`, and its `readlinkat` calls the readlink core
(VIVARIUM 6.26). A native program can call neither. `SYS_STAT` (88)
always follows, and there is no native readlink. Pouch was written
before D-1, on the premise that there are no symlinks (G11):

- `lstat` and `fstatat(AT_SYMLINK_NOFOLLOW)` answer through `SYS_STAT`,
  so `S_ISLNK` is never true and a dangling link reads `ENOENT`.
- `readlink` on a real link answers `EINVAL` for a live link and
  `ENOENT` for a dangling one, because its general arm asks
  `faccessat`, which follows. musl's `realpath` therefore keeps a link
  unexpanded.
- `lchmod` changes the link's target.
- `open` drops `O_NOFOLLOW`, `O_PATH` and `O_DIRECTORY`.

Fixing `lstat` alone would make things worse for a caller that follows
the POSIX contract. SQLite's `unixFullPathname` calls `readlink` when
`lstat` reports a link, and fails `SQLITE_CANTOPEN` when `readlink`
errors.

## Research

- **Plan 9 (heritage).** No symlinks: 9P2000 has no link type. The
  kernel's client speaks 9P2000.L's `Treadlink`, through `Dev.readlink`,
  which only dev9p fills.
- **Fuchsia and Genode.** Both read a link through a handle. fuchsia.io
  opens the node without following and asks it for its target; Genode's
  VFS opens a link handle and reads the target through it.
- **Linux.** `readlinkat(2)`, and `fstatat(2)` with
  `AT_SYMLINK_NOFOLLOW`: path calls.
- **The tree.** `SYS_STAT` is a path call that answers in one round trip
  on a `walk_attrs` Dev (POUNCE 7). The cores are written and audited:
  `sys_stat_for_proc(..., STALK_NOFOLLOW, ...)` and
  `sys_readlink_for_proc`. The native lstat that exists without new ABI
  is three calls (`SYS_OPEN` with `SYS_WALK_OPEN_OPATH |
  SYS_WALK_OPEN_NOFOLLOW`, `SYS_FSTAT`, close), and the descriptor it
  holds lets `lstat` fail `EMFILE` at the table cap, which Linux's never
  does. The only native readlink without new ABI is Loom's `READLINK`
  op, about eight calls and a ring mapping for each `readlink`; musl's
  `realpath` calls `readlink` once per component. DISTRO 4.4 records a
  native pair as a seam, and 134 belongs to corona's
  `SYS_SRV_REGISTRY_NEW` ([[dec-2026-10-19-syscall-numbers-across-tracks]]).

## Options

1. **Two native calls**: `SYS_READLINK` = 135 and `SYS_LSTAT` = 136,
   thin entries over the two cores.
2. **`SYS_READLINK` only**, with `lstat` built from the three existing
   calls.
3. **No new calls**: `readlink` over a Loom ring made for each call,
   and `lstat` from the three calls.

## The call

Option 1 (operator, 2026-10-20). `SYS_READLINK(path_va, path_len,
buf_va, bufsiz)` returns the number of bytes it placed, or -errno, and
does not NUL-terminate. `SYS_LSTAT(path_va, path_len, stat_va)`
returns 0 or -errno, as `SYS_STAT` does, without following a final
link. Both resolve the path as `SYS_STAT` does: absolute from the
caller's root, relative from its cwd, contained at its root (I-28).
Their errnos are the cores', which the phenotype already returns, and
their path checks are `SYS_OPEN_CREATE`'s. Pouch's `lstat`,
`fstatat(AT_SYMLINK_NOFOLLOW)` and `readlink` call them. `symlink`
stays `ENOSYS`, since no port needs it, and the native `SYS_SYMLINK`
seam stays open.

## Rationale

Each is one call, as `stat` is, over code the phenotype already runs
and audit has reviewed. `lstat` costs the same single round trip as
`stat` and holds no descriptor, so it cannot fail `EMFILE`. The new
calls confer no new authority (I-22, I-43). The phenotype reads the
same targets and stats, and a native program can already read a target
through Loom with the same path search.
