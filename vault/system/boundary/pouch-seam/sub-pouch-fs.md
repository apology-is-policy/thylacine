---
id: sub-pouch-fs
type: sub
parent: moc-pouch-seam
title: "The FS surface — open, stat, readdir, the mutation family, readlink"
code:
  - usr/lib/pouch/patches/0009-pouch-openat.patch
  - usr/lib/pouch/patches/0010-pouch-fstat-lseek.patch
  - usr/lib/pouch/patches/0019-pouch-stat.patch
  - usr/lib/pouch/patches/0023-pouch-fopen.patch
  - usr/lib/pouch/patches/0024-pouch-fs-process-wires.patch
  - usr/lib/pouch/patches/0027-pouch-remove.patch
  - usr/lib/pouch/patches/0030-pouch-fopen-append.patch
  - usr/lib/pouch/patches/0031-pouch-readlink.patch
audit: hard
guarded-by: [inv-i28]
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/POUCH-DESIGN.md", "docs/LLVM-DESIGN.md", "docs/VIVARIUM.md"]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Every POSIX path operation a ported program performs: `open` / `openat` /
`fopen`, `stat` / `lstat` / `fstat` / `access`, `lseek` / `pread` /
`pwrite`, `readdir`, the mutation family (`rename` / `unlink` / `rmdir` /
`mkdir` / `chmod` / `ftruncate`), and `readlink` on a system with no
symlinks. It is the largest surface in the series and the one that grew
the most: `openat.c` alone has three generations.

## Contract

- `openat(AT_FDCWD, path, flags[, mode])` — one `SYS_open` (65) through
  the stalk resolver; `O_CREAT` splits into (parent, leaf) +
  `SYS_WALK_CREATE`; `O_TRUNC` → `+OTRUNC`; `O_APPEND` → a post-open
  seek-to-END. A real dirfd is `ENOTSUP`; `O_TMPFILE` is `ENOTSUP`.
- `fstat` → `SYS_FSTAT` (50); `stat`/`lstat`/`fstatat(path)` →
  `SYS_STAT` (88), the POUNCE walk-query; `fstatat(fd,"",AT_EMPTY_PATH)`
  delegates to `fstat`. `lstat == stat` (no symlinks, G11).
- `readdir(DIR*)` refills via `SYS_READDIR` (56) and translates the 9P
  Treaddir stream into Linux `struct dirent` records.
- The mutation family maps onto the parent-fd+leaf kernel primitives
  through one shared split (`__pouch_open_parent`).
- `readlink(path)` on an existing non-link is `EINVAL`; on
  `/proc/{self,<pid>}/{exe,cwd}` it is an open+read of the file whose
  contents ARE the target.

## Mechanism

**Resolution moved twice.** 0009 walked the path per-component via
`SYS_WALK_OPEN`, opening every intermediate with the FINAL omode — which
structurally cannot open a write-mode file through a directory or across
a mount (the first intermediate `O_RDWR` dir-open is rejected), and could
answer only a blanket `ENOENT`. PTY-3's `posix_openpt` was the first live
victim (`/dev/pts/ptmx` crosses devramfs → devdev → ptyfs), so 0021
replaced the loop with ONE `SYS_open`: per-component X-search and
mount-crossing happen INSIDE the resolver, only the final component is
opened with the caller's omode, and the real `-errno` comes back. CL-1a
(0024) then lifted the absolute-path-only restriction, because by then
`chdir`/`getcwd` were wired and `SYS_open`'s FROM_ROOT arm cwd-joins.

**The mutation primitives are parent-fd + leaf, not paths.** So every
`*at` function splits its path at the last `/` and opens the parent
`O_PATH` — born R|W (the A-1.7 navigation base the primitives accept),
resolved through the full stalk walk. `__pouch_open_parent` is that one
shared split: no `/` means the cwd (`"."`); a leading-`/`-only prefix
means the root; a trailing slash is `EINVAL` (no leaf). `renameat` opens
BOTH parents and closes both.

**`fchmod` on a read-only fd is correct, not a bug.** `SYS_WSTAT`'s write
authority is *identity* (owner or CAP), not the fd's rights (#47) — so
the mode axis is set through an `O_PATH` fd with no write right. That is
why `fchmodat` can resolve the path to an `O_PATH` fd and set the mode
through it.

**`faccessat` dropped musl's privilege-drop clone dance** as dead code:
Thylacine has no setuid, so `euid == uid` always. It probes with
`SYS_STAT` and answers from the *owner* rwx bits — advisory by POSIX,
with the kernel's own `perm_check` at open remaining the authority.

**`readdir`'s sizing is the load-bearing detail.** A translated Linux
record (`round_up(offsetof(d_name)+L+1, 8)`) can exceed its 9P source
(`24+L`) by a few bytes, so the refill requests only 3/4 of `dir->buf`
worth of raw stream — guaranteeing the translated form fits and no entry
the kernel returned is dropped mid-batch.

**`remove(3)` cannot use the classic form.** musl's `remove` unlinks and
falls through to `rmdir` on `-EISDIR`; Thylacine's `SYS_UNLINK` collapses
every failure to a flat `-1` with no distinct `EISDIR` (the #102-class
errno-loss), so 0027 dispatches on an `lstat` instead: a directory →
`rmdir`, anything else → `unlink`.

**`O_APPEND` has no kernel mode.** An fd carries a plain cursor and
`SYS_WALK_OPEN` has no append bit, so musl's `__fdopen` asks for it via
`fcntl(F_SETFL)` — which pouch answers `ENOSYS`, leaving the cursor at 0
and making every `fopen("a")` write CLOBBER the file at offset 0. 0030
seeks to END once at open, and does it in a helper every successful-open
exit routes through (`pouch_open_ret`) precisely because `openat` has
THREE such exits — create-ok, EEXIST-fallback-open, plain-open — and a
per-site fix would silently miss one. Single-writer append is thereby
correct; concurrent appenders may still interleave, and that atomicity is
documented-absent rather than silently claimed.

**`readlink` is the sharpest translation in the series.** The seam parked
`__NR_readlinkat` at the sentinel, which is the *wrong* answer rather
than an absent one: on a symlink-free system the result is KNOWN for
every path, and POSIX has the words — an existing non-link is `EINVAL`.
The distinction is load-bearing because musl's `realpath()` is a pure
userspace resolver that calls `readlink` on each prefix and reads the
errno as a fork: `EINVAL` means "not a link, keep walking", **any other
errno is fatal**. Under `ENOSYS`, `realpath()` failed on its first
component — i.e. for every path on the system, for every ported program.
The truthful `EINVAL` repairs it whole with no realpath patch. The
`/proc` arm is a shape translation, not a contents oracle: Linux presents
four paths as symlinks that Thylacine presents as regular files whose
contents are the target, so for exactly those four shapes readlink is an
open+read — a closed whitelist, where a MISS falls through to the general
arm and answers `EINVAL`, which is literally true of every file served.
`self` is rewritten to the caller's own pid rather than passed through,
because a shared `/proc` mount's `self` resolves to the MOUNTER.

## Data structures

`struct t_stat` — the kernel's stat ABI, **mirrored by hand in three
pouch files** (0010 `fstat.c`, 0019 `fstatat.c`, 0021 `ioctl.c`) plus
`libt`, `libthyla-rs`, and the Go fork.

## Concurrency

None of pouch's own. The parent-fd+leaf split is not atomic with the
mutation (a rename between the parent open and the `SYS_RENAME` targets
the new parent) — inherent to a path-based API over fd-based primitives,
and the same window Linux's `*at` functions have when a caller passes
`AT_FDCWD`.

## Invariants enforced

- **[[inv-i28]] (composed, not enforced).** Every path pouch resolves
  goes through one `SYS_open` / `SYS_STAT`, so containment at
  `root_spoor` and per-component X-search are the kernel's — pouch
  cannot widen them and does not try. This is the substantive reason the
  0009 → 0021 change was a correctness fix and not just a simplification:
  the per-component loop was doing resolution work that belongs to the
  resolver.
- **P-3** — every unsupported form fails loud: a real dirfd `ENOTSUP`,
  `O_TMPFILE` `ENOTSUP`, a trailing-slash create `EINVAL`, an over-long
  path `ENAMETOOLONG`.

## Error paths

Real `-errno` from the stalk-resolved calls (`ENOENT` / `EACCES` /
`ENOTDIR`, verbatim — which is what lets `realpath` fail for the right
reason). `EBADF` for any non-zero `SYS_FSTAT` return (granular errno is a
v1.x lift on both sides). `ENOENT` for a non-zero `SYS_STAT`.

## Performance

One syscall per open (was one per path component). `stat(path)` is one
`SYS_STAT` = one fused `Twalkgetattr` on the disk FS with no fid created
(POUNCE). `readdir` costs one `SYS_READDIR` per 3/4-buffer batch plus an
in-place translation pass.

## Prosecution

- Every successful-open exit must route through `pouch_open_ret`, or
  `O_APPEND` silently clobbers on that path (the reason the fix is a
  helper and not three call-site edits).
- `__pouch_open_parent`'s parent fd must be closed on EVERY arm,
  including the EEXIST fallback and both `renameat` endpoints.
- The `readlink` `/proc` whitelist must stay closed and strict (no `//`
  or `/./` normalization) — a loose match turns readlink into a
  file-contents oracle; a miss is fail-safe.
- The pid-digit scan in `proc_link_path` bounds the run INSIDE the scan,
  before any copy: `pidtext` is 20 bytes and the input is caller-supplied,
  so `readlink("/proc/00000000000000000000/exe")` — reachable from any
  program — would otherwise overflow it.
- Any `struct t_stat` mirror added or edited must be checked against the
  KERNEL's size, not against itself.

## Seams

[[seam-pouch-dirfd]] (every `*at` is AT_FDCWD-only) ·
[[seam-pouch-errno-channel]] (the `EBADF`-for-everything fstat collapse).

## Caveats

- **`87-pouch-fstat-lseek.md` (absorbed) documented `struct t_stat` as
  80 bytes** with a 16-row table ending at `gid@76`. The struct has been
  88 bytes since #100 added `devno@80`; all three pouch mirrors carry
  `_Static_assert(sizeof == 88)`. The doc even states the rule it broke —
  "a future kernel field add MUST bump both the size and the assertions" —
  which is the #100 lesson recorded in project memory, met by a doc that
  is itself a stale mirror.
- The same doc's "devramfs subdir walks still deferred / `/etc/stratum/`
  fails with `-ENOENT`" is stale (devramfs has synthetic dirs and the
  resolver crosses mounts). And it had **no row in `docs/REFERENCE.md`
  at all** — a reference doc the index never listed, so a reader
  following the index never learned it existed and the doc-per-PR
  discipline never reached it. That is the mechanical reason its
  `t_stat` table was still at 80 bytes.
- The 0009 patch header still describes the per-component walk loop and
  "absolute paths only", both retired — read it as history, and
  `openat.c`'s current text as truth.

## Provenance

[[chg-2026-05-25-16b-beta-hw-openat]] (0009, the per-component walk —
stratumd's keyfile load) → [[chg-2026-05-25-16b-gamma-syscalls]] (0010,
fstat + lseek + `open()`→`openat()`) → [[chg-2026-07-07-pounce]] (0019,
the path-stat family onto `SYS_STAT`) → [[chg-2026-07-18-pty3]] (0021,
resolution onto the stalk resolver) → [[chg-2026-07-20-g7b-quake]] (0023,
the stdio openers through the public `open()`) →
[[chg-2026-07-23-cl1a-fs-wires]] (0024, the mutation family + readdir +
the create arm) → [[chg-2026-07-24-cl2-cxx-runtime]] (0027, `remove(3)`)
→ [[chg-2026-07-27-30-fopen-append]] (0030) →
[[chg-2026-07-28-v4b4-readlink]] (0031).
