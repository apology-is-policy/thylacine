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
  - usr/lib/pouch/patches/0059-pouch-symlinks.patch
  - usr/lib/pouch/patches/0060-pouch-raw-open.patch
  - usr/lib/pouch/patches/0061-pouch-fstat-ebadf.patch
  - usr/lib/pouch/patches/0062-pouch-link-eperm.patch
  - usr/lib/pouch/patches/0063-pouch-utimensat.patch
  - usr/pouch-hello/pouch-hello-fd.c
  - usr/pouch-hello/pouch-hello-fs.c
  - usr/pouch-hello/pouch-hello-links.c
  - usr/pouch-hello/pouch-hello-tz.c
audit: hard
guarded-by: [inv-i28]
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/POUCH-DESIGN.md", "docs/LLVM-DESIGN.md", "docs/VIVARIUM.md"]
created: 2026-08-01
updated: 2026-10-22
---
## utimensat sets times; chmod's refusal is EPERM (2026-10-22, 0063)

G12 is pulled forward into B-3 ([[dec-2026-10-22-g12-pulled-forward]]), and
`SYS_WSTAT` carries four time bits that mirror 9P's
([[dec-2026-10-22-wstat-times]]). `utimensat` now sets times over them, where
Linux's `SYS_utimensat` reached the 0xFFFF sentinel and answered `ENOSYS`.
musl routes `futimens`, `futimes`, `utimes`, `futimesat`, `lutimes` and
`utime` through `utimensat`, so all of them work. Each time is `UTIME_OMIT`
(left out of the mask), `UTIME_NOW` (its bit alone: the file server's clock)
or a value (its bit and its `_SET` bit, read by the kernel from the
`struct timespec[2]` at x6). A nanosecond outside 0..999999999 that is
neither special is `EINVAL` before anything traps. Null times is both at
`UTIME_NOW`. Both at `UTIME_OMIT` answers 0 and reads nothing, as Linux does.

A null path with a real descriptor is the descriptor form: one `SYS_WSTAT`
on it, and any flag is `EINVAL`. Otherwise the path opens `O_PATH` as
`fchmodat`'s does, with `O_NOFOLLOW` under `AT_SYMLINK_NOFOLLOW`, which sets a
link's own times. `AT_EMPTY_PATH` with `""` names the descriptor itself, or
the working directory for `AT_FDCWD`, as Linux's `LOOKUP_EMPTY` does. A null
path is `EFAULT` and a real dirfd with a name is `ENOTSUP` (P-3).

The kernel answers `EACCES` for every refusal, because -EPERM is the flat -1
the seam reads as EIO. Only the touch (both times to the server's clock) is
admitted by write permission ([[sub-kernel-perm]]); every other time request
is the owner's or `CAP_HOSTOWNER`'s, so `utimensat` answers its refusal as
POSIX's `EPERM`, setting `errno` itself as `link` does, and keeps `EACCES` for
the touch. `fchmod` and `fchmodat` do the same for the mode, whose only
refusal is ownership. `fchmodat`'s open (a component's search denied) and
`lchmod`'s `fstat` keep their `EACCES`.

`tools/test-pouch-times.py` runs the real `utimensat.c`, `fchmod.c` and
`fchmodat.c` over a mocked kernel and the series' own `__syscall_ret`, with
musl's `AT_` and `UTIME_` values (Darwin's `UTIME_NOW` is -1, which would hide
a negative nanosecond). Its 61 checks pin every number to the kernel's header,
read x6 as the kernel does (a null pointer under a `_SET` bit faults), and run
at -O0 and -O2 under ASan and UBSan. It refuses 25 mutants. It also derives
the seam's `__syscall0..7` shapes from `syscall_arch.h` ([[sub-pouch-seam]]).
The guest leg is G12-e's.

## link and linkat answer EPERM (2026-10-21, 0062)

Thylacine makes no hard links ([[dec-2026-10-21-pouch-link-eperm]]). `link`
and `linkat` answer `EPERM`, Linux's errno for a filesystem that cannot make
one, where the 0xFFFF sentinel answered `ENOSYS`. `linkat` refuses a flag
outside `AT_SYMLINK_FOLLOW | AT_EMPTY_PATH` with `EINVAL` first, as Linux
does. Both set `errno` themselves, as `kill` does (0057): -EPERM is -1, the flat
error the seam reads as EIO (0001), so `__syscall_ret(-EPERM)` would answer
EIO. Neither call reads a path or a descriptor, and nothing traps: a missing
source, an existing target and a bad descriptor all read `EPERM`, where Linux
resolves the paths first and answers `ENOENT`, `EEXIST` or `EBADF`.
fontconfig depends on that order. Its cache lock falls back to a directory
lock on `EPERM`, and it removes a stale lock with `rmdir` only when its link
was refused that way. If `EEXIST` came first, it would `unlink` a stale lock
directory, fail, and never recover it (`fcatomic.c`, `FcAtomicLock`). musl's
one other caller, `sem_open`, never reaches its link: its `MAP_SHARED` map is
refused first (0047).

`pouch-hello-links`' `hardlink` leg reads `EPERM` from `link` and from
`linkat(AT_SYMLINK_FOLLOW)` and checks that no name was made. It reads
`EPERM` again for a missing source and for an existing target, which is left
the link it was, and `EINVAL` for `linkat(AT_SYMLINK_NOFOLLOW)`. The census
line gains `hardlink` after `flags`. `tools/test-pouch-link.py` runs the real
`link.c` and `linkat.c` over a mocked kernel and the series' own
`__syscall_ret`, with musl's `AT_` values: twelve checks, one of them every
single flag bit, and seven mutants, two of which hand -EPERM to the seam and
read EIO.

## fstat answers EBADF for a dead descriptor (2026-10-20, 0061)

The kernel's `SYS_FSTAT` answers -EBADF for a handle that names no Spoor
([[sub-kernel-syscall-abi]]), and `fstat`'s decode passes it through, as it
passes -EFAULT and a device's own errno; 0010's fallback that read any other
non-zero return as `EBADF` could never run and is gone. So the `O_DIRECTORY`
unwind's `EBADF` arm (B-3f' round 1 F2) has a producer: a refused descriptor
a peer thread closed first is not closed again. `fdopendir` keeps fstat's
errno, and `isatty` and `tcgetattr` answer `EBADF` through the tty
dispatcher's `refuse_fd` ([[sub-pouch-tty]]).

`pouch-hello-fd`'s `fstat-ebadf` leg reads `EBADF` from `fstat` of a closed
descriptor, from `fstat` of fd 1000 once `fcntl` shows it free, and from
`fdopendir` of the closed one; the same descriptor open is the control.
`isatty-ebadf` reads `ENOTTY` from `isatty` on an open file and `EBADF` from
`isatty` and `tcgetattr` once it is closed. The census line gains both after
`umask`. `tools/test-pouch-fstat-ebadf.py` runs the real `fstat.c`, `ioctl.c`
and `isatty.c` over a mocked `SYS_fstat`.

## umask (2026-10-19, B-3e M8)

0058 serves `umask(mode)` over native `SYS_UMASK` 133
([[dec-2026-10-19-create-mode-bound-and-umask]]): `__syscall(SYS_thyla_umask,
mode & 0777)`. The kernel refuses a bit outside 0777, so the mask is cut first,
as Linux cuts it, and umask never fails. Every Pouch create gets the mask and
the parent's bound from the kernel's create chokepoint
([[sub-kernel-syscall-dispatch]]); Pouch applies neither itself.

`pouch-hello-fd`'s `umask` leg derives its expected modes from the pool root's
own mode and refuses a root whose mode could not tell the mask from the bound.
It checks the boot mask (022), a 0666 create under 027 (the mask and the
bound), a 0666 create under 0 (the bound alone), the previous mask each call
answers, and that `umask(01022)` is cut to 022. The census line gains `umask`
after `fdopendir`.

## Purpose

Every POSIX path operation a ported program performs: `open` / `openat` /
`fopen`, `stat` / `lstat` / `fstat` / `access`, `lseek` / `pread` /
`pwrite`, `readdir`, the mutation family (`rename` / `unlink` / `rmdir` /
`mkdir` / `chmod` / `ftruncate`), the symlink calls (`lstat`,
`readlink`, `lchmod`, and the open flags `O_NOFOLLOW`, `O_PATH` and
`O_DIRECTORY`), and `link` / `linkat`, which make no hard link. musl's own
raw opens -- the time zone, locale and catalog loaders and the stdio
openers -- take the same translation. It is the
largest surface in the series and the one that grew
the most: `openat.c` alone has three generations.

## Contract

- `openat(AT_FDCWD, path, flags[, mode])` — one `SYS_open` (65) through
  the stalk resolver; `O_CREAT` splits into (parent, leaf) +
  `SYS_WALK_CREATE`; `O_TRUNC` → `+OTRUNC`; `O_APPEND` → the `OAPPEND`
  omode bit (0040) plus a post-open seek-to-END (0030); `O_CLOEXEC` → the
  native descriptor flag (FLAGS_SET, 0056), set in the same exit helper once
  the open has its number, so a peer thread's spawn in between sees it
  unflagged (Linux's single call has no window). A set that fails fails the
  open and closes the descriptor, except on EBADF: a peer thread closed it
  first and the number may already be another thread's (round 2). The
  helper is every successful exit's (plain, create, exists-already);
  `tools/test-pouch-open-cloexec.py` pins each. A real dirfd is `ENOTSUP`;
  `O_TMPFILE` is `ENOTSUP`.
- The open flags for links (0059): `O_NOFOLLOW` and `O_PATH` are omode bits
  (`SYS_WALK_OPEN_NOFOLLOW` 0x20, `SYS_WALK_OPEN_OPATH` 0x80). A final link
  under `O_NOFOLLOW` is `ELOOP`, or the link itself with `O_PATH` too; an
  `O_PATH` descriptor answers `fstat` and refuses `read` and `write`,
  `EBADF`. `O_PATH` masks every flag but `O_CLOEXEC`, `O_DIRECTORY` and
  `O_NOFOLLOW` before any other is read, as Linux's `build_open_how` does,
  and then `O_CREAT` with `O_DIRECTORY` is `EINVAL` (Linux 6.4 on).
  `O_DIRECTORY` without `O_NOFOLLOW` appends a `/` to the path, so the
  kernel's trailing-slash gate refuses a non-directory before anything
  opens; with `O_NOFOLLOW`, or with no room for the slash, an fstat of the
  new descriptor closes anything but a directory, `ENOTDIR`, before
  close-on-exec is set. `O_TRUNC` is never sent beside `O_DIRECTORY`, so no
  refusal truncates a file, and a directory answers `EISDIR`, as on Linux.
  `tools/test-pouch-symlinks.py` pins each arm against a mocked kernel.
- musl's own raw opens (0060): the `sys_open`, `__sys_open`, `sys_open_cp`
  and `__sys_open_cp` macros of `src/internal/syscall.h`, at both arities, call
  the hidden `__pouch_sys_open`, which is `openat`'s body: it answers a
  descriptor or `-errno` and leaves `errno` as it found it (the `O_DIRECTORY`
  arm's `__fstat`, its one call that sets `errno`, is saved and restored
  around it; the hidden name, so a program's own `fstat` never runs inside
  libc's open). `openat()` is the wrapper: a real dirfd is `ENOTSUP`, and otherwise it
  sets `errno` once from the body's answer. The macros' users are
  `__map_file` (the time zone, locale and catalog loaders), `fopen`,
  `__fopen_rb_ca` (`/etc/passwd`, `/etc/group`, `/etc/hosts` and the other
  system files), `tmpfile`, and the secure start's `/dev/null`, which never
  runs: the kernel supplies no `AT_SECURE`. None is a cancellation point.
  `__map_file` maps the file `PROT_READ` and `MAP_PRIVATE` without
  `MAP_FIXED` ([[sub-pouch-mem]]); musl unmaps the same unaligned size, and
  the kernel rounds both lengths up to whole pages.
  `tools/test-pouch-raw-open.py` pins the expansions, the raw contract, the map
  and the openers against a mocked kernel, and `/bin/pouch-hello-tz` loads a
  zone file on the device.
- The descriptor family (0055, 0056): `dup`, `dup2`, `dup3`, `fcntl`'s
  F_GETFD / F_SETFD / F_DUPFD / F_DUPFD_CLOEXEC / F_GETFL / F_SETFL over native
  129-131 and 123 (`docs/ERRORS.md`, "Pouch fcntl and dup"). musl's own raw
  `SYS_fcntl` callers -- the `0xFFFF` sentinel -- moved to the native calls:
  `fopen "e"` and `__fopen_rb_ca` rely on the raw open's O_CLOEXEC (0060), `__fdopen "e"`
  sets the flag (ignoring a failed set, as upstream; a tagged socket is
  never flagged -- `pouch_set_cloexec` checks the tag), `freopen` with no
  name sets it and changes the status through `fcntl` (every such call used
  to fail and close the stream). The append bit is fixed at open, so a
  `freopen(NULL, mode)` other than "a" restates the descriptor's own (an
  append descriptor keeps appending, where Linux would stop) and "a" on a
  descriptor without O_APPEND is EINVAL, closing the stream. The
  rest are unreachable and stay: `fchdir` / `fchown` (their own calls are
  sentinels and answer first), `socketpair` and `signalfd` (the same).
- `fstat` → `SYS_FSTAT` (50); `stat`/`fstatat(path)` → `SYS_STAT` (88),
  the POUNCE walk-query; `lstat`/`fstatat(AT_SYMLINK_NOFOLLOW)` → native
  `SYS_LSTAT` (136, [[dec-2026-10-20-native-lstat-readlink]]), which reports
  a final link itself (`S_IFLNK`, its size the target's length);
  `fstatat(fd,"",AT_EMPTY_PATH)` delegates to `fstat`. A flag outside
  `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`, `AT_NO_AUTOMOUNT` and the
  `AT_STATX` sync hints is `EINVAL`, as on Linux, and a path past the
  kernel's 1024-byte bound (`SYS_OPEN_PATH_MAX`, below musl's `PATH_MAX`)
  `ENAMETOOLONG`.
- `readdir(DIR*)` refills via `SYS_READDIR` (56) and translates the 9P
  Treaddir stream into Linux `struct dirent` records. `d_type` is the
  record's type byte, passed through: the kernel Devs write a `d_type`
  there as 9P servers do ([[sub-kernel-spoor]]). The links prover's `dtype`
  leg reads it on the fixture (`DT_LNK`, `DT_DIR`, `DT_REG`, written by
  Stratum: the control) and on `/hw`, a kernel Dev (`DT_DIR` and `DT_REG`
  only, at least one of each).
- The mutation family maps onto the parent-fd+leaf kernel primitives
  through one shared split (`__pouch_open_parent`).
- `readlink(path)` → native `SYS_READLINK` (135): a link's target, at most
  `bufsiz` bytes and never NUL-terminated; `EINVAL` for an existing
  non-link, else the path's own resolution errno. `/proc/{self,<pid>}/
  {exe,cwd}` is answered first, by an open+read of the file whose contents
  ARE the target. A `bufsiz` of 0 rides a one-byte dummy and answers 0 for a
  link, as upstream musl does.
- `fchmodat(AT_SYMLINK_NOFOLLOW)`, so `lchmod`, opens the final component
  `O_PATH|O_NOFOLLOW` and answers `EOPNOTSUPP` for a link, as musl and glibc
  do on Linux; a non-link's mode is set as `chmod` sets it. Any other flag is
  `EINVAL`.
- `link` and `linkat` make no hard link (0062): `EPERM` without a trap, after
  `linkat`'s `EINVAL` for a flag outside `AT_SYMLINK_FOLLOW | AT_EMPTY_PATH`.
- `utimensat` (and every musl time setter above it) sets times over
  `SYS_WSTAT`'s time bits (0063): `UTIME_OMIT` leaves a time alone, `UTIME_NOW`
  takes the server's clock, a value crosses at x6. Its refusal is `EPERM`,
  except the touch's (both times to "now"), which is `EACCES`; the wstat's
  refusal in `fchmod` and `fchmodat` is `EPERM`.

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

**`remove(3)` dispatches on an `lstat`.** musl's `remove` unlinks and
falls through to `rmdir` on `-EISDIR`, a raw `SYS_unlinkat` the seam parks;
0027 asks `lstat` instead: a directory → `rmdir`, anything else → `unlink`.
`SYS_UNLINK` answers `EISDIR` itself (#80), so the classic form would work
again; the dispatch stays because it is correct and costs one `lstat`.
`lstat` does not follow a final link, so a link to a directory is unlinked
and its target left alone, as POSIX asks.

**`O_DIRECTORY` rides the kernel's trailing-slash gate.** The kernel has no
directory-only open, and a check after the open is too late on its own: a
device or clone file would see its open. So without `O_NOFOLLOW` the path
gains a `/`, and the resolver's #82 gate ([[sub-kernel-stalk]]) refuses a
non-directory before any `Dev.open`, in Linux's errno order (`ENOTDIR`
before `EACCES`). Behind the slash nothing more is asked, so an `opendir`
costs no extra RPC. A trailing slash forces the final link's follow (D-1),
which `O_NOFOLLOW` refuses, so with it, and on a path with no room left for
the slash, the open runs unslashed and an fstat of the new descriptor
decides. An fstat that answers `EBADF` means a peer closed the number, so it
is not closed again, the rule the close-on-exec set keeps. `O_TRUNC` is write
intent, so beside `O_DIRECTORY` no open succeeds on Linux: Pouch never sends
the truncate there and answers `EISDIR` for a directory, so no refusal on
the unslashed arms costs a file its bytes.

**`O_APPEND`, in two halves, and the history is why.** musl's `__fdopen`
asks for append via a raw `__syscall(SYS_fcntl, F_SETFL)` — the `0xFFFF`
sentinel, so `ENOSYS` (0056 removed the raw call: the public `fcntl`'s
F_SETFL refuses flipping O_APPEND after open, EINVAL, because the append is
fixed at open, so `__fdopen` now only marks the stream),
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

**`readlink` is the sharpest translation in the series.** musl's
`realpath()` is a pure userspace resolver that calls `readlink` on each
prefix and reads the result as a fork: `EINVAL` means "not a link, keep
walking", a count is a link to expand, **any other errno is fatal**. Both
halves of the answer are load-bearing: a wrong errno fails `realpath()` for
every path on the system, and a link answered `EINVAL` is left unexpanded
in the path it returns. So the general arm is the kernel's own answer,
`SYS_READLINK`, verbatim. The `/proc` arm is a shape translation, not a
contents oracle: Linux presents four paths as symlinks that Thylacine
presents as regular files whose contents are the target, so for exactly
those four shapes readlink is an open+read — a closed whitelist, where a
MISS falls through to the general arm, which answers what the kernel knows.
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

### musl's raw opens (0060)

`bits/syscall.h` gives 65 the name `__NR_open`, and 65 is the native open,
whose arguments are (start_fd, path, length, omode). Upstream
`src/internal/syscall.h` takes its `#ifdef SYS_open` arm whenever that name
exists and sends Linux's (path, flags, mode), so every musl raw open was
refused or misread, and no time zone, locale or catalog file ever loaded:
`tzset` fell back to UTC without a word (OPEN-BUGS 2026-10-15 O-4). A second
refusal stood behind the first. `__map_file` asked for `MAP_SHARED`, which
Pouch refuses (`ENOSYS`; there is no shared file map, ARCHITECTURE.md 6.5),
so mending the open alone would still have read every zone as UTC. 0060
points the four macros at `__pouch_sys_open` and maps `MAP_PRIVATE`; the
readers only read the map.

The body is raw because the macros are. `sys_open` is `__syscall_ret` over
`__sys_open`, so a body that set `errno` and returned -1 would turn every
refusal into `EPERM` (-1 is -EPERM), and the secure start's bare `__sys_open`
must not touch `errno` at all. 0023's openers, which had called the public
`open()` to get the right shape, go back to upstream's `sys_open`, so an ISO
C program that defines its own `open` no longer captures `fopen`.

`/bin/pouch-hello-tz` is the device witness. It writes its own TZif file: zone
THY (+05:30) with one daylight interval, TDT (+06:30), over the first day of
1970, and the rule `THY-5:30` after it. It checks that `tzset` reads the file
(only the file's transition table can say TDT), that the zone is one
read-only private file row of `/proc/<pid>/maps` (matched by
`st_dev:st_ino`), that a switch to UTC unmaps it, and that sixteen switches
leave one row. Its control, a POSIX TZ string for the same zone, maps no file
and has no daylight interval. Without 0060 the four file legs fail and the
control passes.

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
  `O_TMPFILE` `ENOTSUP`, a trailing-slash create `EISDIR` (#86), an over-long
  path `ENAMETOOLONG`.

## Error paths

Real `-errno` from the stalk-resolved calls (`ENOENT` / `EACCES` /
`ENOTDIR`, verbatim — which is what lets `realpath` fail for the right
reason). A failed `SYS_FSTAT` answers its own errno: `EBADF` for a dead
descriptor, `EFAULT`, or the device's (`EIO` when it has none). `ENOENT` for a
non-zero `SYS_STAT`. `SYS_LSTAT`
and `SYS_READLINK` answer `-errno` themselves (`docs/ERRORS.md`, "Native
lstat and readlink"), so `lstat` and `readlink` pass the kernel's errno
through.

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
- `O_PATH`'s mask must run before every other flag check; the
  `O_DIRECTORY` slash is appended only without `O_NOFOLLOW` and only when it
  fits, and only an unslashed open is fstat'd; `O_TRUNC` never reaches the
  kernel beside `O_DIRECTORY`; a refused `O_DIRECTORY` descriptor is closed
  exactly once (not at all after `EBADF`), before close-on-exec is set; `fchmodat` never reaches `WSTAT` on a link and
  closes its descriptor on every path.
- **Every raw open is `openat`'s body, raw.** No expansion of the
  `sys_open` family may reach `SYS_open` (the native shape) or set `errno`
  itself: `sys_open` applies `__syscall_ret` to the answer, so a body that
  returned -1 with `errno` set would turn every refusal into `EPERM`.
  `__pouch_sys_open` answers `-errno` on every path and restores `errno`
  around its fstat; `openat()` sets it once.
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
[[seam-pouch-errno-channel]] (a flat kernel `-1` reads `EIO`).

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
- **`dup` of a socket `accept()` returned answers `EACCES`**, which POSIX
  does not list for dup: the accepted connection is a plain kernel
  descriptor over an untagged devsrv Spoor, and DUP_TO's alias gate refuses
  that kind ([[sub-kernel-handle]]); `dup`, `dup2`, `dup3` and `F_DUPFD`
  pass the EACCES through (the Linux phenotype answers EMFILE or EBADF).
  B-3k replaces the accepted-connection path (OPEN-BUGS 2026-10-15).
- **`fdopen(fd, "a")` cannot add `O_APPEND`** to a descriptor opened without
  it: the append is fixed in the server's open, so the stream writes at the
  descriptor's offset where Linux would append.
- **A socket `accept()` returns is a kernel conn fd, not a pouch slot**, so
  the socket answers above do not reach it: the kernel reports it
  `S_IFREG`, and its seeks and positional I/O read as `EIO`. B-3k makes
  sockets kernel descriptors.
- **`O_DIRECTORY|O_NOFOLLOW` without `O_PATH` answers `ELOOP` for a final
  link**, where current Linux answers `ENOTDIR` (its directory check precedes
  the link refusal; the kernel refuses the link first). Robust callers take
  either (libc++'s `remove_all`, Rust's `remove_dir_all`). On that path,
  and on a path with no room for the slash, a non-directory is opened
  (never truncated) before it is refused.
- **`open`, `openat` and the raw opens are not cancellation points**, where
  POSIX makes `open` and `openat` ones (upstream's `open` is
  `__sys_open_cp`). Pouch's open is several calls, and where a cancel may land
  in the create arm needs a design (OPEN-BUGS 2026-10-20).
- **`O_CREAT` through a dangling final link answers `ENOENT`**, where Linux
  creates the target: the create finds the link's name and answers
  `EEXIST`, and the open after it follows the link to nothing.
- **`lstat("/srv/<name>")` answers `ENOENT` while `stat` answers the
  record**: 0014's stale-socket short-circuit (OPEN-BUGS 2026-10-20).
- **`F_GETFL` on an `O_PATH` descriptor reports `O_RDONLY`**: the status
  word has no `O_PATH` bit, so `fdopendir` of one passes where Linux answers
  `EBADF` (OPEN-BUGS 2026-10-20).

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
[[chg-2026-07-28-v4b4-readlink]] (0031) →
[[chg-2026-10-20-b3fprime-symlinks]] (0059, native lstat and readlink) →
[[chg-2026-10-20-raw-open]] (0060, musl's raw opens).
