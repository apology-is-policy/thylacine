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
  - usr/lib/pouch/patches/0040-pouch-o-append-omode.patch
  - usr/lib/pouch/patches/0051-pouch-fd-sync-vectored.patch
  - usr/pouch-hello/pouch-hello-fd.c
audit: hard
guarded-by: [inv-i28]
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/POUCH-DESIGN.md", "docs/LLVM-DESIGN.md", "docs/VIVARIUM.md"]
created: 2026-08-01
updated: 2026-10-15
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
  `SYS_WALK_CREATE`; `O_TRUNC` → `+OTRUNC`; `O_APPEND` → the `OAPPEND`
  omode bit (0040) plus a post-open seek-to-END (0030). A real dirfd is `ENOTSUP`; `O_TMPFILE` is `ENOTSUP`.
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
means the root; ~~a trailing slash is `EINVAL` (no leaf)~~ — see below.
`renameat` opens BOTH parents and closes both.

### The trailing slash: strip, report, and let the kernel decide

The blanket `EINVAL` was wrong in **both** directions. POSIX 4.13 makes a
trailing `/` assert *the leaf names a directory*, so `unlink("f/")` wants
`ENOTDIR` — and `rmdir("dir/")` must **succeed**, which is the row that
actually bites: `rm -r dir/` and every script that builds `"$DIR/"` paths.

The splitter now **strips** the separator run and **reports** the assertion
through a `dir_required` out-parameter, with the leaf becoming a
length-bounded slice — every caller passes that length rather than a `strlen`
of the leaf, which would run into the stripped separators. A shared probe
then does **one `SYS_STAT` of the still-slash-terminated path**, which runs
the kernel's own audited trailing-slash gate: zero only for an existing
directory, otherwise the real `ENOTDIR` / `ENOENT` / `EACCES`.

**The POSIX rule is enforced where it is audited, never re-derived here.**
That sentence is the whole design, and it is what makes this layer's fix
different from the other two in the same family — see below.

Each caller then applies its own **Linux** row, chosen deliberately: the rows
were measured on a POSIX host first, and where macOS and Linux diverge the
Linux answer wins, because musl's target ABI is Linux.

| call | answer |
|---|---|
| `mkdir("d/")` | creates `d` — strip alone; `file/` stays `EEXIST` |
| `rmdir("d/")` | removes `d`; `f/` is `ENOTDIR` — strip alone, because the remove-directory primitive already enforces dir-ness |
| `unlink("x/")` | `ENOENT` / `EISDIR` / `ENOTDIR`, and it must **never reach the unlink syscall** |
| `open("x/", O_CREAT)` | `EISDIR`, existing or not (macOS says `ENOENT` when absent) |
| `rename` (either name `x/`) | the Linux rule verbatim — unless the source is a directory, trailing slashes give `ENOTDIR` — answered by one probe of the slash-terminated **source** |
| all-slashes (`"/"`) | `EISDIR` from the splitter |

The `unlink` row carries the sharpest reasoning: **stripped and passed
through, a plain unlink would delete the very file the slash says must not be
touched.** That is byte-for-byte the defect the native runtime had, arrived at
from the opposite direction.

The splitter serves exactly **four** callers — the `O_CREAT` arm, `mkdirat`,
`renameat` (both names), and `unlinkat` (which is also `rmdir` via
`AT_REMOVEDIR`). Smaller than the task's "every leaf-name syscall" framing:
the rest pass full paths and were already kernel-gated.

### Where this layer differs, and the principle that covers all three

Three layers independently normalised paths and all three were repaired in the
same window — the kernel's cwd join, this splitter, and the native runtime's
`Path`. The other two were fixed by **deleting** the normalisation. This one
could not be: splitting `(parent, leaf)` is structurally required, because the
kernel's mutation primitives take a parent fd and a leaf name rather than a
path. The separator genuinely has to come off.

So the rule is not *never transform*. It is **never DECIDE**:

> A layer that must transform a path may strip, but it may not adjudicate. It
> reports what the original asserted and re-asks the authority.

Under that reading all three fixes are the same fix. The kernel and the native
runtime had nothing to transform, so for them "do not decide" collapses to
"delete the cleaning". Here it means strip, carry the assertion forward in a
side channel, and spend one extra `SYS_STAT` to have the audited gate answer.

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

**`O_APPEND`, in two halves, and the history is why.** musl's `__fdopen`
asks for append via a raw `__syscall(SYS_fcntl, F_SETFL)` — the `0xFFFF`
sentinel, so `ENOSYS` (the public `fcntl` gained F_GETFL / F_SETFL in 0056,
where flipping O_APPEND after open is EINVAL: the append is fixed at open),
leaving the cursor at 0 and making every `fopen("a")` write CLOBBER the
file at offset 0. 0030 (2026-07-27) seeks to END once at open, in a helper
every successful-open exit routes through (`pouch_open_ret`) precisely
because `openat` has THREE such exits — create-ok, EEXIST-fallback-open,
plain-open — and a per-site fix would silently miss one. Its comment said
the kernel had no append mode and that the real fix needed "a v1.x omode
bit". That bit arrived five weeks later with VIVARIUM 6.27 — `OAPPEND`
(0x40), inside `SYS_WALK_OPEN_OMODE_VALID`, forwarded by dev9p to the 9P
open, honoured by Stratum positioning EVERY write at EOF server-side —
and nobody told Pouch: raw Linux binaries got correct appends while ports
stayed on the one-time seek, so `fopen("a+")`, a read or an `fseek`, then
a write landed MID-FILE, over existing bytes (audit B-0 r2 F9). 0040
passes the bit on all three opens and KEEPS the seek, because for an
append fd the kernel cursor is advisory and the seek is what makes
`ftell()` and the first read of an `"a+"` stream start at EOF as they have
since 0030. On a Dev with no append notion the bit is inert — NOT because
"each switches on `omode & 3`", as this paragraph first said (14 of the 16
other `Dev.open` slots are `dev_simple_open`, which stores the whole omode
and switches on nothing), but because `Spoor.mode` is read only in
`kernel/handle.c`, and only for status: `s->mode & 3` for the access mode,
and since B-3b `handle_get_status` reports the bit as F_GETFL's O_APPEND, as
Linux reports it for any fd opened so. No Dev reads it. Conclusion true,
mechanism false (audit r3 F7). `/bin/pouch-hello-fopen` pins it: on the file
its create + append legs left as `alpha\nbeta\n`, reopen `"a+"`, seek 0,
write `gamma\n`, require `alpha\nbeta\ngamma\n` (the 0030 emulation yields
`gamma\nbeta\n`).

**What 0040 does NOT give: atomicity against a CONCURRENT appender.**
Stratum's write is stat-then-write with a documented TOCTOU
(`src/9p/server.c` h_write step b; its `cf-2-design.md` 4.3), so two
appenders to one log can be handed the same end and one overwrites the
other. 0030's source said so ("CONCURRENT appenders may interleave -- that
atomicity is documented-ABSENT"); 0040's first version DELETED that sentence
and claimed "two appenders to one log" as fixed. Restored in `openat.c` and
here. The fix is Stratum's (take the size under the write's own lock);
tracked there.

Costs and divergences, all of them: dev9p keeps an append fd off the Larder's
write-behind path, so each flushed append is one RPC; an `"a+"` stream's
initial read position is END (the BSD choice — musl-on-Linux and glibc give
0); after an append write that followed a seek the cursor — `ftell()`,
`lseek(SEEK_CUR)`, a read after a bare `fflush()` — is seek-position + count,
not the new EOF; after ANOTHER writer grew the file the cursor does not move
at all, though the next write lands at the new end.

**0024's patch file had no trailing newline**, so its last hunk
(`unlinkat.c`) applied only because BSD `patch` silently spends fuzz on a
final context line; `git apply` calls the file corrupt. Found 2026-09-21 by
applying the series under `--fuzz=0`; fixed, and
`tools/check-patch-hunks.py` now fails the class.

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

### fsync, and the vectored calls as one transfer (0051)

`fsync` and `fdatasync` trap `SYS_FSYNC(fd, datasync)` -- 0 for the full
barrier, 1 for data only. Until 0051 both sat at the 0001 sentinel and
answered `ENOSYS`, though the kernel had served the call since the
FS-mutation work; SQLite's commit path fails on exactly that. A file that
does not support synchronization answers `EINVAL`, as POSIX and Linux say: a
pouch socket number (answered before the trap; a vacant slot `EBADF`), and a
kernel handle whose Dev has no fsync slot (a pipe, the console), which the
kernel answers `EOPNOTSUPP` (`kernel/syscall.c`, `sys_fsync_handler`) and
pouch maps. A handle without `RIGHT_WRITE` is the kernel's `EBADF`, and a
server error arrives as its own errno.

`readv`, `writev`, `preadv` and `pwritev` have no kernel call behind them.
One `read`/`write` per iovec would split a record the caller hands over
whole -- a UDP datagram, or a pipe write within `PIPE_BUF` that POSIX makes
atomic -- so `_pouch_iov.c` makes ONE transfer per call. A vector with one
non-empty element moves in place; otherwise up to 128 KiB of it (the
kernel's `SYS_RW_MAX`, the most one read or write moves) is gathered into
one buffer and moved by one call of the tag-aware wrapper, and a read is
scattered back. The answer is that transfer's count: the kernel and the 9P
client cut a transfer at their own bounds (`SYS_RW_MAX`; the session's
largest write payload), so a large vector answers a short count, as a large
`write` does, and the caller resumes. Up to 4096 bytes are gathered on the
stack, in an out-of-line helper so the in-place and refusal paths do not
carry the frame; more on the heap, freed by a cleanup handler if the thread
is cancelled inside the transfer; a heap buffer that cannot be had answers
`ENOMEM`. Validation is POSIX's: a count outside `[0, IOV_MAX]` or lengths
summing past `SSIZE_MAX` is `EINVAL` (Linux caps the sum instead), and so is
a positional offset that is negative or would pass `LLONG_MAX`. `preadv2`
and `pwritev2` without flags run on `preadv` and `pwritev`.

The witness for "one transfer" is the pipe's atomicity, not the bytes: on a
nonblocking pipe with two bytes free, a four-byte `writev` of two two-byte
pieces must answer `EAGAIN` and write nothing (`pouch-hello-fd`,
`writev-atomic`). A write per piece lands the first piece and answers 2;
the bytes read back would be the same either way.

`lseek`, `pread` and `pwrite` on a pouch socket answer `ESPIPE` (a socket
has no offset) instead of handing the kernel a number it never issued. A
negative `pread`/`pwrite` offset is `EINVAL` before the descriptor is looked
at, the order Linux checks them in (the kernel answers `EINVAL` too, for its
own handles). `fstat` of a socket answers a zeroed stat with
`S_IFSOCK | 0777`, one link, the slot (plus one) as `st_ino`, and all bits
set as `st_dev` -- a device number the kernel's counter (from 1, upward)
never issues, so no file or pipe shares the `(st_dev, st_ino)` pair (static
Devs report 0).

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

- Every successful-open exit must route through `pouch_open_ret`, and the
  `OAPPEND` bit must be OR-ed into `omode` BEFORE the first of the three
  opens, or `O_APPEND` silently degrades on that path (the reason both
  halves sit at one place each and not at three call sites).
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
- **A stripped separator must never be silently forgotten.** Every caller of
  the splitter takes the reported `dir_required` and the leaf *length*, not a
  `strlen` of the leaf. The `unlink` row is the one that shows why: stripped
  and passed through, a plain unlink deletes the very file the slash asserts
  must be a directory.
- **The trailing-slash verdict comes from the kernel, never from here.** The
  shared probe exists so the audited gate answers; re-deriving "is this a
  directory" in the boundary-line would put a second, unaudited copy of a
  POSIX rule one layer below the one that was reviewed.
- **A patch's CONTEXT lines are a dependency on another patch's output.**
  0030's `openat` hunks carried the old `strlen(leaf)` call as context, so
  editing 0024 alone would have broken 0030's apply — silently, at bake time,
  in the direction this project has already been bitten by. Any edit to a
  shared helper's call sites must be re-diffed across the whole series from a
  scratch tree and dry-run-verified before the build.
- **`size_t` in scope for every CALLER is not every INCLUDER.** A header that
  gains a prototype with a `size_t` parameter must include `<stddef.h>`
  itself; a file including it only for the syscall constants otherwise breaks
  the build under `-nostdinc`.

## Seams

[[seam-pouch-dirfd]] (every `*at` is AT_FDCWD-only) ·
[[seam-pouch-errno-channel]] (the `EBADF`-for-everything fstat collapse).

## Caveats

- **`87-pouch-fstat-lseek.md` (absorbed) documented `struct t_stat` as
  80 bytes** with a 16-row table ending at `gid@76`. The struct is 88
  bytes: #100 added `devno@80`, which is 64 bits over what was the pad
  after it; all three pouch mirrors carry `_Static_assert(sizeof == 88)`. The doc even states the rule it broke —
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
- **`fsync` on a read-only handle fails where POSIX succeeds.** The kernel's
  `SYS_FSYNC` requires `RIGHT_WRITE` ("the write-side durability barrier")
  and answers `EBADF`. SQLite opens a directory read-only to fsync it. Its
  sync path ignores the result, but `unixDelete` with `dirSync` set does not:
  a multi-database commit deleting its super-journal, and a rollback
  journal's delete under `SYNCHRONOUS=EXTRA`, fail `SQLITE_IOERR_DIR_FSYNC`
  (read from `os_unix.c`; B-3i runs it). An operator vote precedes B-3i: the
  kernel drops `RIGHT_WRITE` for `fsync`, or SQLite builds with
  `SQLITE_DISABLE_DIRSYNC` (OPEN-BUGS, 2026-10-15). A program that fsyncs a
  file through a read-only descriptor fails either way until the first.
- **The vectored calls fault where musl answers `EFAULT`, and lose
  async-signal safety past 4096 gathered bytes.** A bad iov array faults on
  every path; a bad `iov_base` faults whenever the vector is gathered (more
  than one non-empty element), stack tier included. A gathered vector over
  4096 bytes calls `malloc` and pushes a cleanup handler: a signal handler
  that `siglongjmp`s out of that transfer strands the handler's record, and
  a later `pthread_exit` calls through the abandoned frame. At or under 4096
  bytes nothing is pushed, so a small `readv`/`writev` stays as safe as
  POSIX lists it.
- **A regular-file `write` or `writev` past the transfer bounds is short.**
  One call moves at most `SYS_RW_MAX` (128 KiB), and over 9P at most the
  session's largest write payload, where Linux writes a regular file whole.
  POSIX allows it and stdio resumes; a program that treats a short
  regular-file write as an error does not (OPEN-BUGS, 2026-10-14).
- **The read half of "one transfer" has no device witness.** A byte source
  (a pipe, a file) answers one gathered read and a read per iovec alike; only
  a record source, a datagram, tells them apart.
- **`fcntl`, `dup`, `dup2`/`dup3` onto a number are still `ENOSYS`** after
  0051: they need the native descriptor calls B-3b brings onto main
  (`dec-2026-10-14-fd-calls-on-astra`).
- **A socket `accept()` returns is a kernel conn fd, not a pouch slot**, so
  the socket answers above do not reach it: the kernel reports it
  `S_IFREG`, and its seeks and positional I/O read as `EIO`. B-3k makes
  sockets kernel descriptors.

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
