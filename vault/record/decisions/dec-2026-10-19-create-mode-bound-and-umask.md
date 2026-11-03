---
id: dec-2026-10-19-create-mode-bound-and-umask
type: dec
title: "A create's mode is bounded by its directory and reduced by the creator's umask"
date: 2026-10-19
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-kernel-proc, sub-kernel-perm, sub-kernel-vivarium, sub-pouch-seam, sub-pouch-fs, sub-utopia-eval]
created: 2026-10-19
---
## Fork

No create applies a mask. IDENTITY-DESIGN 3.5 F-5 resolved "mode = a
default, masked by a umask-equivalent" in 2026-05 and it was never
built:
- Pouch's `umask` is the ENOSYS sentinel (`__NR_umask 0xFFFF`, patch
  0001), and so is the phenotype's;
- go-thylacine's `Mkdir` sends `DMDIR | (mode & 0o777)` and `Create`
  sends `perm` as given;
- `spoor_create_install`, the one create chokepoint, passes the mode
  through.

Ported code asks for 0666 and 0777 and relies on the umask to narrow
them, so a system program makes world-writable files and directories on
the shared pool. M7 (2026-10-18) refuses another identity's create
there, but a write to an existing file is judged by `perm_check` on the
file's mode, and the write travels on the pool's session as system.

A concrete chain: joey's Go-4c gate runs `go build` as system with
`GOCACHE=/go-cache` on every boot of an image that bakes GOROOT, and
runs the binary it links. Go creates cache entries 0666, and a cache
hit is trusted on the output file's size alone (go-thylacine
`cmd/go/internal/cache/cache.go` `GetFile`; `work/buildid.go`
`useCache`; `GODEBUG=gocacheverify` is off by default). On a pool that
persists across boots, a user who overwrites an entry in place has code
run as system at the next boot.

## Research

- **Plan 9 (heritage).** open(5): a created file's permissions are
  `perm & (~0666 | (dir.perm & 0666))` and a created directory's
  `perm & (~0777 | (dir.perm & 0777))`; "the group of the file is the
  same as dir". The server applies the bound. There is no umask and no
  per-process state.
- **POSIX / Linux.** A per-process umask (conventionally 022),
  inherited across fork and kept across exec, applied by the kernel;
  `umask()` always succeeds. A parent's default ACL replaces the umask,
  a directory-side bound like Plan 9's. Under 9P2000.L the client
  applies the umask and the server stamps what arrives.
- **Fuchsia (SOTA).** fdio keeps `umask` as per-process libc state
  (`gstate.umask`); its create paths in `unistd.cc` do not read it.
  Authority comes from directory handles, not mode bits.
- **The tree.** Stratum stamps `mode & 07777` with no mask
  (`src/9p/server.c` create and mkdir), so the kernel, the client of
  every session, is the only place a mask can apply.
  `spoor_create_install` already stats the parent wherever the Dev
  enforces rwx. Native code asks 0644 and 0755 itself (libthyla-rs
  `DEFAULT_CREATE_MODE`, `CREATE_DIR_MODE`); ported code asks 0666 and
  0777. No kernel test, gate script or userspace witness asserts a
  created mode (dev9p's fake server records only the gid).
- **POSIX's promise.** `umask()` cannot fail, so the ENOSYS sentinel
  makes musl return -1 and the caller carry on. A program that sets
  `umask(077)` before writing a secret gets no restriction.

## Options

The floor:
1. **The Plan 9 parent bound in the kernel.** No ABI change; it holds
   whatever the program asks or sets.
2. **A umask alone.** A program that clears its umask (the daemon
   idiom) or asks for an explicit wide mode (tar restoring archived
   modes as system) reopens the hole.
3. **No kernel bound.** Fix the writers one at a time.

The umask:
1. **A kernel per-Proc umask** and one new syscall, served to Pouch,
   the phenotype and Go.
2. **Libc-local, as Fuchsia keeps it.** No ABI change, but spawned
   children do not inherit it, and the phenotype and Go bypass it.
3. **Keep ENOSYS.** The bound is the only mask.

## The call

The Plan 9 parent bound (operator vote, 2026-10-19), and a kernel
per-Proc umask (operator vote, 2026-10-19), both the recommended
options. IDENTITY-DESIGN 9.7 M8 states the rule; `SYS_UMASK` = 133 is
the one ABI addition, signed off by the second vote.

## Rationale

The bound is the floor no program can lower: the directory's owner
decides how wide anything created in it may be, and a wider mode takes
an explicit chmod, which only the owner may make. The umask is the
creator's own restriction, which ported code sets and POSIX promises
always takes. Each covers what the other cannot: a daemon's `umask(0)`
cannot widen past the bound, and a 0755 directory cannot make a
`umask(077)` secret private. Both run at the one chokepoint the kernel
already owns, before the request reaches a server that applies neither.
