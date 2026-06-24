# POSIX FS-compat cookbook

When you port a foreign language runtime to Thylacine, its `GOOS=thylacine`
(or libc / platform) boundary translates POSIX file expectations onto
Thylacine's Plan-9/9P-shaped FS (the per-Proc namespace, `stalk` resolution,
`dev9p` over Stratum, the `SYS_*` syscalls). The two models do not line up at
the edges, and each mismatch surfaces as a build/run failure in the ported
program. This document is **(a) a running catalog of every such gap we have
hit, with its root cause + fix, and (b) a recipe for diagnosing + fixing the
next one.** The Go port (`docs/GO-PORT-PLAN.md`, tasks #322-#356) drove the
first pass; Python/Node/etc. will hit the same classes.

Read this before porting a new runtime. Most fixes land in the *runtime's
platform boundary* (the thing playing the role Go's `src/{os,syscall}/*_thylacine.go`
plays), not the kernel — Thylacine's kernel deliberately offers Plan-9-shaped
primitives and lets the POSIX shim assemble POSIX semantics from them.

---

## The meta-patterns (read these first)

These are the *recurring shapes*. A new symptom almost always belongs to one.

### P1 — The bare-`-1` → EPERM decode trap (`#102`)

**Symptom.** A spurious `operation not permitted` (EPERM, errno 1) from a
syscall that should have a specific error (ENOENT, EEXIST, ...) or should
succeed.

**Cause.** Thylacine returns errors as a negative errno in the `[-4095, -1]`
band (Linux-style). Newer syscalls return a real `-T_E_*`; **legacy file
syscalls historically return a bare `-1`**, and `-1 == -T_E_PERM == -EPERM`,
so a negative-errno-decoding runtime (Go's `asm_thylacine_arm64.s` decodes the
`[-4095,-1]` band; the comment there literally documents this) renders *any*
bare `-1` as "operation not permitted". This makes a missing file, an existing
file, a full table, and a real perm denial all look identical.

**Fix.** Make the kernel path return the correct `-T_E_*` (the errno-rollout,
"ER-1"). `err_code()` in `kernel/stalk.c` already maps a dev-layer bare `-1`
to `T_E_IO`, and `T_E_PERM` is deliberately never returned from a handler
(see the warning in `kernel/include/thylacine/errno.h`). When a port reports
EPERM, **do not trust it** — instrument the kernel path to print the real
failure (see the recipe) before concluding it is a permission problem.

**Lesson burned in.** #352 looked like EPERM ("operation not permitted") and
was chased for many boots as a permission/handle-table problem; the real cause
was `Tlcreate` returning `-EEXIST` swallowed into a bare `-1`. Ground-truth the
*actual* errno first.

### P2 — Partial-syscall looping (`SYS_RW_MAX`)

**Symptom.** `io.ErrShortWrite` ("short write"), truncated reads, or a write
that silently moves fewer bytes than asked.

**Cause.** `SYS_READ`/`SYS_WRITE` are capped at `SYS_RW_MAX = 4096` bytes per
call (`kernel/include/thylacine/syscall.h`) and return the (partial) count.
POSIX `write(2)` may legally return a short count; the **runtime's I/O layer
must loop** until the whole buffer is moved or an error occurs. On a normal OS
the cap is huge so the loop never triggers; at 4096 it triggers constantly.

**Fix.** Loop in the runtime's regular-file/socket write path (the role
`internal/poll.FD.Write` plays on other GOOSes). See #351.

### P3 — POSIX open-or-create semantics

**Symptom.** `os.Create` (or `open(O_CREAT|O_TRUNC)`) of an **existing** file
fails — as EEXIST, or as the bare-`-1` EPERM (P1) wearing its disguise.

**Cause.** Thylacine's `SYS_WALK_CREATE` is a **bare create** — it issues a 9P
`Tlcreate`, which returns `EEXIST` if the file already exists (Plan-9 create is
create-only; Linux v9fs walks-first and only `Tlcreate`s a genuinely new file).
POSIX `O_CREATE` *without* `O_EXCL` means **open-or-create**: succeed whether or
not the file exists, truncating if `O_TRUNC`. The runtime's open path must
assemble that from the kernel primitives.

**Fix.** In the runtime's `openFileNolog`-equivalent: for `O_CREATE` without
`O_EXCL`, try **open first** (carry `O_TRUNC` so the kernel truncates an
existing file — `dev9p_open` honors `OTRUNC` -> `Tlopen O_TRUNC`), and fall
back to create only on a not-exist error. Reserve bare-create for `O_EXCL`.
See #352. The kernel already supports truncate-on-open, so this is a pure
runtime-boundary fix.

### P4 — Toy resource limits

**Symptom.** A failure deep into a build/run that the same program survives on
a normal OS; often a bare-`-1` EPERM (P1) at a table-full boundary.

**Cause.** Several v1.0 limits are deliberately small and were never sized for
a demanding FS consumer: `PROC_HANDLE_MAX` (open-fd count per Proc),
`POLL_MAX_NFDS` (fds polled at once), `SYS_RW_MAX` (per-call I/O). The Go
toolchain (`cmd/go` + `compile`/`asm`/`link`, fd-hungry) is the canonical
stress case.

**Fix.** Raise the limit to a realistic value, *and* mind the ripples — a
fixed-size limit is often the size of stack arrays / slab objects elsewhere
(raising `PROC_HANDLE_MAX` 64->256 tripped the SLUB 2048-byte object cap *and*
a poll-syscall stack overflow; see #355 + the handle-table change). The real
v1.x fix is a growable structure (the growable fd-table, #355).

### P5 — Unsupported ops, fail-closed

**Symptom.** `unsupported operation` / `function not implemented` (ENOSYS).

**Cause.** Some POSIX surfaces are simply not built at v1.0: advisory file
locks (`flock`/OFD locks), `os.Executable`, scatter-gather (`sendmsg`/`recvmsg`),
etc. Thylacine returns `ENOSYS`/`ErrUnsupported`.

**Fix.** Two options: (a) make the runtime degrade gracefully if the op is
best-effort (Go's cache-trim lock failure is non-fatal — leave it, or make the
boundary a no-op success where the semantics permit it on a single-user
device); (b) build the kernel surface if the op is load-bearing. Decide per op;
most are (a) at v1.0. See #353.

---

## The catalog (running list)

Newest first. Status: FIXED / DEFERRED / OPEN. "Where" = the layer the fix
lands in. Pattern = the meta-pattern above.

| # | Symptom | Pattern | Root cause | Where | Status |
|---|---|---|---|---|---|
| #356 | `compile: writing output: write $WORK/bNNN/_pkg_.a: operation not permitted` | P1+? | EPERM (bare `-1`?) writing the compiled package archive after the create succeeds. Not yet root-caused — instrument the write path (`sys_write_for_proc` -> `dev9p_write` -> `Twrite`) for the real errno. | TBD | OPEN |
| #352 | `open $WORK/bNNN/go_asm.h: operation not permitted` | P3 (disguised as P1) | `os.Create` (O_CREATE\|O_TRUNC) re-creates an existing go_asm.h; `dev9p` `Tlcreate` returns `-EEXIST`, swallowed to a bare `-1` -> Go decodes EPERM. | Fork `os/file_thylacine.go::openFileNolog` (open-or-create-truncate). Kernel already honors OTRUNC-on-open. | FIXED |
| #351 | `compile: writing output: short write` | P2 | `os.File.write`/`pwrite` did one 4096-capped `syscall.Write` and returned the partial; `os.File.Write` then raised `io.ErrShortWrite`. | Fork `os/file_thylacine.go` (loop `write`/`pwrite` on partials, mirroring `poll.FD.Write`). | FIXED |
| #353 | `go: failed to trim cache: Lock /go-cache/trim.txt: unsupported operation` | P5 | Advisory file lock unimplemented (`cmd/go/internal/lockedfile/.../filelock_other.go` returns `errors.ErrUnsupported`; thylacine is `!unix !windows`). NON-FATAL (cache trim is best-effort). | Fork (no-op the lock on thylacine) OR kernel (advisory locks). | DEFERRED |
| #345 | `os.Stat`/`os.Mkdir`/write EPERM on `/tmp` (0777) + `/go-cache` | P1 | Bare-`-1` from the resolution/stat path on the pool dirs; the ER-1 resolution keystone (`stalk_err -> -T_E_NOENT`) closed it. | Kernel (errno-rollout) + boot env setup. | FIXED |
| #344 | `wait: operation not permitted` on a parallel build | P1+P4 | `wait_pid_for` refused a 2nd concurrent same-Proc waiter (single-waiter Rendez) -> bare `-1`; Go's parallel `go build` has N workers each `wait4`-ing. | Kernel (multi-waiter `child_waiters` poll-list). | FIXED |
| #102 | open-of-missing-file -> EPERM not ENOENT | P1 | The open-leaf path returns a bare `-1` for some non-resolution failures instead of `-T_E_NOENT`; Go renders EPERM, not `os.IsNotExist`. | Kernel (errno-rollout). | OPEN (partial; ER-1 covers resolution) |

(Pre-Go gaps already closed in the kernel: `SYS_FSTAT`/`SYS_LSEEK` (16b-gamma),
the create/fsync/readdir/rename/unlink FS-mutation surface (FS-alpha..gamma),
`/env` (#338). Those are in `docs/reference/` per-subsystem, not repeated here.)

---

## The diagnosis recipe (ground-truth-first)

When a port hits an FS failure, follow this — it is the method that eventually
cracked #352 after a wrong-hypothesis detour, and it is much faster if you do
it *first*:

1. **Get the exact error verbatim** from the port's output (the `PathError`
   `Op` + `Path` + `Err`). `Op="open"` vs `"write"` vs `"create"` tells you the
   syscall family; the `Err` text tells you the errno *as the runtime decoded
   it* — which, per P1, may be a lie (a bare `-1` masquerading as EPERM).

2. **Find which kernel syscall it maps to.** Read the runtime's platform
   boundary (`src/syscall/*_thylacine.go`, `src/os/*_thylacine.go`). Beware:
   `os.Create` of an existing file is a *create* (`SYS_WALK_CREATE`), not an
   open; `os.OpenFile` failures wrap as `Op:"open"` even when the failing
   syscall was a create (see #352). Trace the actual SVC number.

3. **Instrument the kernel path, do not theorize.** Add a temporary
   `uart_puts(...)` at *every* `-1`/error exit of the handler (`sys_open_handler`,
   `sys_walk_create_handler`, `sys_write_for_proc`, `dev9p_create`/`dev9p_write`),
   printing the path/name + the *real* internal `rc`. Gate it on the filename
   (e.g. ends-with "go_asm.h") to keep output bounded. **One boot with full
   bracketing diagnostics beats five boots of hypotheses.** The #352 hunt cost
   ~9 boots because the first diagnostic only covered one handler + one path;
   instrument *all* exits of *both* the open and the create handler at once.

4. **Confirm the real errno** (e.g. `-17 = -EEXIST`, `-2 = -ENOENT`). Decode
   against `kernel/include/thylacine/errno.h`. Now you know the meta-pattern.

5. **Fix at the right layer.** Default to the *runtime boundary* (assemble POSIX
   semantics from the kernel primitives) — it is cheaper, isolated to the port,
   and matches how the kernel is designed (Plan-9 primitives + a POSIX shim).
   Reach for a kernel change only when the primitive itself is missing or
   wrong (a real errno that should be `-T_E_*`, a genuinely-absent operation, a
   toy limit).

6. **Strip the diagnostics, re-verify, record the gap here.** Add a row to the
   catalog with symptom/pattern/cause/where/status + the task #.

### Iteration mechanics (so this doesn't cost a day)

- A **kernel-only** change can skip the slow GOROOT re-bake: `cmake --build
  build/kernel` rebuilds just the ELF and `tools/test.sh` boots it without
  regenerating the pool (the GOROOT pool survives — verified). Only a **fork
  (runtime) change** needs `THYLACINE_BAKE_GOROOT=1 tools/build.sh all` (it
  re-cross-builds the toolchain). Back up the GOROOT pool first
  (`cp build/fixtures/pool.img build/fixtures/pool.img.goroot`) so a stray
  standard build doesn't force a re-bake.
- `build.sh kernel` and the SMP gate **regenerate a standard (non-GOROOT)
  pool** — they clobber the GOROOT pool. Restore from the backup or re-bake.
- Cap boot logs and clean up throwaways (M2 disk hygiene).

---

## Status of the Go port FS surface (snapshot)

- **Working:** file create/open/read/write/seek/stat/lseek/mkdir/readdir/
  rename/unlink; partial-write looping (#351); open-or-create-truncate (#352);
  `/env`; parallel `wait` (#344); spawn/exec; net over `/net`.
- **In the cascade (next):** #356 (`_pkg_.a` write EPERM) is the current
  blocker; expect more as the build proceeds to link.
- **Deferred:** #353 (advisory lock, non-fatal); `os.Executable`
  (`function not implemented`, telemetry sidecar only, non-fatal); sendmsg/
  recvmsg/socketpair (#214).
- **The arc:** getting the full on-device `go build` (#342) green is a cascade
  of these gaps, closed one at a time. Each is small; the value is the
  *catalog* — the next port (Python, Node) re-uses the patterns + the recipe
  instead of re-discovering them.
