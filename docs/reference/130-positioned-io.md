# 130. Positioned byte I/O — SYS_PREAD / SYS_PWRITE (#37) + the SYS_WSTAT kind-gate (#47)

The positioned-I/O syscall pair and the wstat rights-posture correction, landed
as one syscall-surface chunk (2026-07-04; the go-build clean+perf mission P2.1).

---

## Purpose

POSIX `pread(2)`/`pwrite(2)`: byte I/O at a caller-supplied absolute offset
that **never reads or advances the fd cursor**. The absence of cursor traffic
is the contract — concurrent positioned ops on one fd share no mutable state,
which is what `io.ReaderAt`'s documented parallel-use guarantee (and every
archive/loader reader built on it) rides on. No Seek+Read emulation can
provide it; the Go port's pre-#37 emulation carried an explicit "not atomic
against a concurrent cursor move" caveat and its cursor-restore bug was #36
layer 2.

The kernel half is deliberately thin: the Dev vtable `read`/`write` slots have
**always** taken an explicit `s64 off` (the Plan 9 shape — `Dev.read(c, buf,
n, off)`); the per-Spoor cursor is syscall-layer sugar maintained by
`SYS_READ`/`SYS_WRITE`. Positioned I/O is therefore the same lookup + gates
with the caller's offset passed through and the cursor untouched.

---

## ABI

```
SYS_PREAD  = 85
  Args:   x0 = fd  (hidx_t; KOBJ_SPOOR + RIGHT_READ; CWALKONLY/O_PATH rejected)
          x1 = buf (user VA; validated + per-byte uaccess staged, as SYS_READ)
          x2 = len (clamped to SYS_RW_MAX = 4096 per call; short reads normal)
          x3 = off (s64 absolute byte offset)
  Return: bytes read (0 = EOF); -1; or the Dev's -errno passthrough (#3).

SYS_PWRITE = 86
  Args:   as SYS_PREAD, with RIGHT_WRITE.
  Return: bytes written (>= 0); -1; or the Dev's -errno passthrough.
```

Positioned-specific gates, both directions:

- `off < 0` → -1 (checked before the handle lookup).
- `dev->seekable == false` → -1 — the POSIX **ESPIPE shape**. The RW-4 R2-F2
  flag (devramfs + dev9p only) already marks which Devs have byte offsets;
  positioned I/O on a pipe/cons/srv stream fails up front rather than
  silently acting as a cursor-free stream op. The gate sits BEFORE the
  `len == 0` short-circuit, so even a zero-length positioned probe reports it.
- `len > INT64_MAX - off` → -1 (u64-arithmetic overflow guard; no UB).
- Everything else is byte-identical to `SYS_READ`/`SYS_WRITE`: the #844
  ref-held lookup, the #81 `CWALKONLY` reject (len 0 included), the #3 errno
  clamp. **No weft fast-path** — a weft flow is a stream; no consumer preads
  a `/net` data fid, and wiring it would buy nothing.

`SYS_WSTAT` (59) changes posture, not shape: the fd gate drops `RIGHT_WRITE`
→ kind-gate only (`KOBJ_SPOOR`, any rights). See
[99-fs-permission.md](99-fs-permission.md) — the handler section carries the
#47 rationale (POSIX fchmod/fchown work on an `O_RDONLY` fd; the write
authority is `perm_wstat_check`, the identity axis, which is unchanged and
now the ONLY write-authority gate on that path).

---

## Implementation — `kernel/syscall.c`

`sys_read_for_proc`/`sys_write_for_proc` refactored into shared inners
`spoor_read_common`/`spoor_write_common(p, h, kbuf, len, positioned, off)`:

- `positioned == false` — byte-identical to the pre-#37 bodies: `dev->read/
  write(c, kbuf, len, c->offset)`, then `c->offset += n`. `SYS_READ`/
  `SYS_WRITE` and every existing caller are unchanged in behavior.
- `positioned == true` — the three gates above; `dev->read/write(c, kbuf,
  len, off)`; the cursor is neither read nor written on any path.

Exported for tests (the `_for_proc` convention): `sys_pread_for_proc` /
`sys_pwrite_for_proc`. The handlers (`sys_pread_handler`/`sys_pwrite_handler`,
4 args) mirror `sys_read_handler`/`sys_write_handler`'s user-buffer validation
+ `SYS_RW_MAX` clamp + per-byte uaccess staging; `len == 0` rides the inner's
validate-then-0 path. One asymmetry worth noting: a copy-out fault mid-pread
loses nothing (the cursor never moved — the caller can repeat), unlike
`SYS_READ`'s documented consumed-bytes-lost caveat.

`sys_wstat_handler` thins to `current_thread()` + the exported all-scalar
inner `sys_wstat_for_proc` (same refactor pattern), with the lookup rights
mask 0.

---

## Consumers

| Layer | Surface |
|---|---|
| libt | `t_pread` / `t_pwrite` (`usr/lib/libt/include/thyla/syscall.h`) |
| libthyla-rs | `t_pread` / `t_pwrite` (`usr/lib/libthyla-rs/src/lib.rs`; native `File` sugar deferred until a consumer needs it) |
| Go port | `syscall.Pread`/`Pwrite` are single `Syscall6` calls (`src/syscall/syscall_thylacine.go`; `SYS_PREAD`/`SYS_PWRITE` in `zsysnum_thylacine_arm64.go`). The Seek+Read/Write emulation is deleted. `os.File.ReadAt`/`WriteAt` loop over short reads as usual. cmd/go's `buildid.ReadFile` — the #36-layer-2 victim — is the heaviest real user. |
| pouch | musl's `pread64`/`pwrite64` seam numbers flipped 0xFFFF → 85/86 in `usr/lib/pouch/patches/0001-pouch-syscall-seam.patch` (musl aarch64 passes exactly `(fd, buf, count, ofs)` — `__SYSCALL_LL_PRW(x) == (x)`). Pre-#37 both short-circuited to ENOSYS. |

---

## Tests

Kernel (`tools/test.sh`, registered in `kernel/test/test.c`):

- `sys_prw.pread_devramfs_offset_and_cursor` — known `/welcome` content at
  offsets; past-EOF → 0; negative off → -1; off+len overflow → -1; a
  read–pread–read sandwich proves the cursor is untouched.
- `sys_prw.pipe_not_seekable` — pread/pwrite on a pipe → -1 (len 0 included);
  the cursor path still round-trips after the rejects.
- `sys_prw.rights_and_walkonly` — WRITE-only handle can't pread; READ-only
  can't pwrite; `CWALKONLY` rejects both (len 0 included).
- `dev9p.prw_wire_offset_and_cursor` — the loopback responder captures the
  Tread/Twrite offset field: pwrite(0x1234)/pread(0x77) put the CALLER's
  offset on the wire with the cursor at 0; interleaved cursor writes go at
  0 then 5 (the cursor path advances; positioned ops in between don't move
  it).
- `dev9p.wstat_readonly_fd` — the #47 regression: the owner principal chmods
  through a `RIGHT_READ`-only handle (pre-#47: -1 at the rights gate; the
  mode reaches the wire), while a stranger holding FULL rights is denied by
  `perm_wstat_check` — the rights-drop opened no authority hole.

In-guest, every boot:

- `pouch-hello` gained the pread/pwrite seam section: a read–pread–read
  sandwich on `/welcome` through musl (a pre-#37 binary gets ENOSYS — the
  wiring proof is the success), plus a pwrite that must reach the kernel
  (errno != ENOSYS).
- The go4c gate builds run cmd/go's buildid/archive Pread paths on the real
  syscall; a wrong-offset or cursor-moving pread would resurface the #36
  cache-miss cascade immediately (build2-warm is the canary).

---

## Error paths

Identical to `SYS_READ`/`SYS_WRITE` (the -1 sentinel + the Dev's -errno
passthrough window), plus: negative offset, non-seekable Dev, off+len
overflow — all -1. The errno-rollout seam (a distinct EINVAL/ESPIPE at the
boundary) is ER-1's, shared with the rest of the fd surface.

## Known caveats / footguns

- The per-call `SYS_RW_MAX` (4096) clamp applies; positioned I/O has no weft
  large-op bypass. Big positioned transfers loop (all current consumers do).
- On a seekable Dev whose backing ignores offsets for a particular file
  (e.g. a 9P server's synthetic stream file), a pread degrades to what that
  server does with the offset — the Dev-interpreted Plan 9 semantics.
  `dev->seekable` is per-Dev, not per-file.
- `pread(fd, buf, 0, -5)` → -1 (the off gate precedes the len==0 return);
  Linux agrees.
