# Pouch fstat + lseek — Phase 6 sub-chunk 16b-γ-syscalls

**Status:** LANDED (sub-chunk 16b-γ-syscalls). Audit-bearing surface introduced
without a formal prosecutor round at the checkpoint (deferred to the 16b-γ-mount-close
sub-chunk, where the bdev_thylacine read-path lift surfaces additional surface
worth auditing concurrently).

> **#37/#47 sibling surface (2026-07-04):** positioned I/O (`SYS_PREAD`/`SYS_PWRITE`,
> incl. the pouch `pread64`/`pwrite64` seam wiring) and the `SYS_WSTAT`
> kind-gate posture live in [130-positioned-io.md](130-positioned-io.md) +
> [99-fs-permission.md](99-fs-permission.md).

## Purpose

Two POSIX-shaped kernel syscalls (`SYS_FSTAT`, `SYS_LSEEK`) plus the userspace
glue that closes stratumd's `stm_keyfile_load` path: `open` → `fstat` → `read` →
`lseek` → `read`. Three pieces:

1. **Kernel ABI**: `SYS_FSTAT = 50`, `SYS_LSEEK = 51` (`kernel/include/thylacine/syscall.h`).
2. **Native fstat record**: `struct t_stat` (80 bytes since A-2a; field
   offsets pinned by `_Static_assert`s).
3. **Pouch arm**: patch `0010-pouch-fstat-lseek.patch` retargets musl's
   `src/stat/fstat.c` + `src/fcntl/open.c` and re-defines `__NR_fstat` /
   `__NR_lseek` in `bits/syscall.h.in`.

The sub-chunk also lands two kernel fixes that surfaced as the userspace
fstat / lseek path activated:

- `kernel/devramfs.c::devramfs_walk` now respects sys_walk_open's "return `nc`"
  contract (pre-fix it allocated its own Spoor, which made every userspace
  `SYS_WALK_OPEN(FROM_ROOT, ...)` against devramfs fail at the post-walk
  consistency check).
- `kernel/syscall.c::sys_walk_open_handler` now rejects partial walks
  (`w->nqid != nname`). Pre-fix a walk that missed at step 0 still installed
  an opened fd bound to the source Spoor's pre-walk qid (root directory),
  silently letting a `open("/missing-file")` return root.
- `kernel/joey.c::joey_run` stamps kproc's Territory's `root_spoor` to the
  devramfs root before forking joey. Without this, every Proc descended from
  joey has `root_spoor = NULL` and `SYS_WALK_OPEN(FROM_ROOT, ...)` returns -1.

## SYS_FSTAT

```
SYS_FSTAT(spoor_fd, stat_va) -> 0 / -1
  x0 = spoor_fd   hidx_t; must be KOBJ_SPOOR (any rights -- #46)
  x1 = stat_va    user-VA pointer to an 80-byte struct t_stat
                  (fully mapped + writable; alignment NOT required)
```

The kernel calls `dev->stat_native(spoor, &kernel_scratch)`. devramfs implements
it (filling size + mode + qid from the cpio file table). Other Devs leave the
slot NULL — SYS_FSTAT on those fds returns -1 (graceful "no native stat").

### struct t_stat (80 bytes; ABI-locked; A-2a grew it from 72 with uid+gid)

| Offset | Size | Field          | Purpose |
|--------|------|----------------|---------|
| 0      | 8    | `size`         | File size in bytes |
| 8      | 8    | `qid_path`     | 9P qid.path (Plan 9 identity) |
| 16     | 8    | `atime_sec`    | Access time (epoch seconds; v1.0 = 0) |
| 24     | 8    | `mtime_sec`    | Modify time (epoch seconds; v1.0 = 0) |
| 32     | 8    | `ctime_sec`    | Change time (epoch seconds; v1.0 = 0) |
| 40     | 4    | `mode`         | POSIX mode bits (`S_IFREG | 0644` typical) |
| 44     | 4    | `nlink`        | Link count |
| 48     | 4    | `qid_vers`     | 9P qid.vers (version counter) |
| 52     | 1    | `qid_type`     | 9P qid.type (QTFILE / QTDIR / ...) |
| 53     | 3    | `_pad_qid`     | Explicit padding to align blksize |
| 56     | 4    | `blksize`      | Preferred I/O size hint |
| 60     | 4    | `_pad_blksize` | Explicit padding to align blocks |
| 64     | 8    | `blocks`       | Count of 512-byte blocks |
| 72     | 4    | `uid`          | Owner principal_id (A-2a) |
| 76     | 4    | `gid`          | Owner group id (A-2a) |

`_Static_assert`s pin `sizeof(struct t_stat) == 80` and every `__builtin_offsetof`.
The kernel STORES the full 80 bytes unconditionally, so a consumer's buffer
MUST be at least 80 bytes -- a 72-byte pre-A-2a buffer gets its 8 adjacent
bytes clobbered. A future kernel field add MUST bump both the size and the
assertions; an old consumer reading an 80-byte slot from a larger future
producer would silently see zeros in the new fields. The `_pad_*` slots are
reserved for forward-compat fields (the natural extension is `atime_nsec` /
`mtime_nsec` / `ctime_nsec` when sub-second timestamps land).

## SYS_LSEEK

```
SYS_LSEEK(spoor_fd, offset, whence) -> new_offset / -1
  x0 = spoor_fd   hidx_t; must be KOBJ_SPOOR (no rights required)
  x1 = offset     s64 (treated as signed; u64-cast at the ABI)
  x2 = whence     T_SEEK_SET (0) / T_SEEK_CUR (1) / T_SEEK_END (2)
```

Manipulates the per-Spoor `s64 offset` cursor that `SYS_READ` / `SYS_WRITE`
advance per call. SEEK_END queries `dev->stat_native` for size; Devs without
`stat_native` cannot service SEEK_END (returns -1).

The cursor is NOT lock-protected at v1.0 — concurrent `lseek` / `read` / `write`
on the same fd from different threads is unspecified (POSIX user serializes).
The audit-trigger row notes this; serialization is a Phase 7+ lift if it
matters in practice.

Overflow / underflow:
- SEEK_SET: `offset < 0` rejected.
- SEEK_CUR: `cur + offset` checked against `INT64_MIN..INT64_MAX`; resulting
  `new_off < 0` rejected.
- SEEK_END: `size + offset` checked the same way.

## Pouch arm (patch 0010)

Three files at the boundary-line:

### `arch/aarch64/bits/syscall.h.in` — re-define syscall numbers

`#undef __NR_fstat` + `#define __NR_fstat 50` (was 0xFFFF after 0001-pouch-syscall-seam).
Same for `__NR_lseek 51`.

### `src/stat/fstat.c` — translate t_stat → musl struct stat

The upstream musl `fstat()` calls `__fstatat(fd, "", st, AT_EMPTY_PATH)` →
`SYS_statx`, which is 0xFFFF on Thylacine. Replaced with a direct
`__syscall(SYS_fstat, fd, &t_stat)` call + field-by-field translation:

```c
st->st_size    = t.size;
st->st_mode    = t.mode;
st->st_nlink   = t.nlink;
st->st_ino     = t.qid_path;
st->st_blksize = t.blksize;
st->st_blocks  = t.blocks;
st->st_atim.tv_sec  = t.atime_sec;  /* tv_nsec stays zero */
st->st_mtim.tv_sec  = t.mtime_sec;
st->st_ctim.tv_sec  = t.ctime_sec;
```

`st_dev`, `st_uid`, `st_gid`, `st_rdev` stay zero at v1.0 (Thylacine has no
per-Proc uid/gid; devramfs files have no device number distinct from the Dev
itself).

### `src/fcntl/open.c` — redirect open() through openat()

The upstream musl `open()` calls `__sys_open_cp(filename, flags, mode)` which
expands to `__syscall_cp4(SYS_openat, AT_FDCWD, ...)`. That bypasses our patched
`openat.c` (0009) and dispatches `SYS_openat = 0xFFFF` directly → kernel returns
-1 → caller sees a flat -1 with no errno.

The fix: `open()` simply forwards to `openat(AT_FDCWD, filename, flags, mode)` —
the patched C function. Boundary-line clean; no shared SYS_open dispatch.

### `src/unistd/lseek.c` — unchanged

musl's `lseek()` already calls `syscall(SYS_lseek, fd, offset, whence)` which
maps 1:1 to Thylacine's `SYS_LSEEK(fd, offset, whence)`. With `__NR_lseek = 51`
re-defined, `lseek()` "just works" without a rewrite.

## Invariants

- **I-6 (handle rights monotonicity)**: SYS_FSTAT and SYS_LSEEK both require
  KOBJ_SPOOR kind only, no rights bits (#46 dropped fstat's original
  RIGHT_READ tightening). fstat observes metadata, not content -- POSIX
  fstat(2) works on any valid fd (Linux: O_WRONLY, O_PATH, anything; Plan 9
  Tstat / 9P2000.L Tgetattr have no open/read requirement). The tightening
  was falsified by the standard write-then-stat pattern: an O_WRONLY create
  mints a WRITE-only handle (omode-derived rights, A-3 F1), and cmd/go's
  putIndexEntry fstats exactly such an fd for its truncate no-op gate -- the
  -1 made it self-delete every fresh go-cache index entry (#36 layer 4). It
  also guarded nothing: metadata is already reachable by re-walking the path
  O_PATH (#81 keeps fstat allowed on O_PATH by design). The one real
  residual (the #46 audit F1): a spawn-endowed, rights-stripped handle in a
  child whose Territory CANNOT walk the file now reveals its metadata --
  ACCEPTED by the POSIX/Plan 9 fd-passing precedent (a passed fd conveys
  fstat; the endower could read the metadata and chose to pass the handle;
  rights-stripping in this tree bounds read/write/transfer, never metadata
  secrecy -- a future sandbox design must not assume otherwise). The
  in-guest regression is joey's `probe46` (O_WRONLY create -> write(175) ->
  fstat -> fsync -> fstat, size asserted both times, boot-fatal; run on the
  pool root every boot, plus the quiet AND post-build-churned /go-cache in
  bake-config boots).
- **I-13 (kernel-userspace isolation)**: t_stat stored via per-byte
  `uaccess_store_u8`. SYS_FSTAT first validates the entire 80-byte target
  range fits in the user-VA bound. On uaccess fault (unmapped page mid-store)
  the partial bytes already written are observable but the return value is
  -1 — same shape as existing SYS_READ.
- **Partial-walk safety (new closure)**: `sys_walk_open_handler` now rejects
  any walk whose Dev returned `nqid < nname`. dev9p_walk returns NULL on
  partial fail (so it never produced this hazard); devramfs_walk returns a
  Walkqid with `nqid = 0` on miss (now caught + rejected).

## Tests / verification

Runtime regression: joey's per-boot `/system.key` probe.

```
joey: probe /system.key fstat OK size=3656 mode=0o100644
joey: probe /system.key lseek SEEK_END/SEEK_SET OK
```

Tests this probe covers:
- `t_walk_open(FROM_ROOT, "system.key", 10, T_OREAD)` succeeds (devramfs walk
  + sys_walk_open partial-walk check + kproc territory chroot all live).
- `t_fstat(fd, &st)` returns 0 and populates `st.size = 3656`,
  `st.mode = S_IFREG | 0644 = 0100644`.
- `t_lseek(fd, 0, T_SEEK_END)` returns the file size (= 3656). Exercises the
  SEEK_END path through `dev->stat_native`.
- `t_lseek(fd, 0, T_SEEK_SET)` returns 0. Exercises the SEEK_SET path.

stratumd-side: `stm_keyfile_load("/system.key")` now succeeds end-to-end
through open + fstat + read(8 header) + lseek(0) + read(3656 buf), verified
by reaching `stm_bdev_open(THYLACINE)` — the next blocker is in
bdev_thylacine's read I/O (separate 16b-γ-mount-close sub-chunk).

## Naming rationale

`SYS_FSTAT` / `SYS_LSEEK` keep the standard POSIX names because they ARE
POSIX-shaped surfaces — re-naming to a thylacine-themed word would obscure
"this is the fstat() / lseek() kernel ABI" for callers who already know POSIX.
Per CLAUDE.md "Don't force it" — the bar is "a thematic name should add
clarity OR color without obscuring intent" and these don't clear that bar.

The Thylacine-native record is `struct t_stat` (lowercase `t_` prefix matches
`struct t_sys_spawn_args` from 16b-α — the libt-and-kernel-shared ABI
convention).

## Naming caveat — devramfs's `system.key` placement

The 16b-γ-syscalls realization places the system pool keyfile at the ramfs
root (`/system.key`) rather than FHS-shaped `/etc/stratum/system.key`. This is
NOT a design decision but a v1.0 expedient: `tools/mkcpio.py` is flat-only and
`kernel/devramfs.c` maintains a flat file table, so nested cpio entries can't
be walked. The FHS-shaped path lifts when devramfs gains directory walks
(deferred to a later v1.x lift; not blocking 16b-γ-mount-close).

The K1 initramfs-literal-key boot decision (scripture commit `e82e945`) is
preserved; the realization is the only thing shifted.

## Known caveats / footguns

- **No per-Spoor cursor lock at v1.0**: concurrent `lseek` / `read` / `write`
  on the same fd from different threads is unspecified. Per-Spoor seqlock or
  similar is a v1.x lift if it surfaces in practice.
- **fstat on KOBJ_SRV refused**: SYS_FSTAT rejects non-KOBJ_SPOOR handles
  with -1. POSIX programs that fstat a socket fd will get an unexpected
  failure here. If a use case surfaces, the right extension is a per-kind
  stat path (KOBJ_SRV's stat could expose socket-type metadata via t_stat's
  `mode = S_IFSOCK` and zero size).
- **SEEK_END limited by Dev**: Devs without `stat_native` cannot service
  SEEK_END. devnotes, devcons, devnull, devzero all return -1 for SEEK_END.
- **Pouch fstat translates flat ENOENT**: musl-side fstat returns -1 with
  `errno = EBADF` on any kernel rc != 0. Granular errno (EACCES, EFAULT,
  EOVERFLOW, ...) is a v1.x lift on both sides.
- **devramfs subdir walks still deferred**: devramfs is flat. `/etc/stratum/`
  and similar nested paths fail with -ENOENT. The pouch openat patch already
  handles multi-component path-walks — when devramfs gets a directory tree
  the FHS-shaped layout lifts naturally.
