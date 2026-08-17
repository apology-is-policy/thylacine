# ERRORS.md — Thylacine error-code system

Binding. Describes how Thylacine encodes errors across its surfaces:
syscall returns, fault notes, exit statuses, extinction messages, and the
boundary-line translation into POSIX-compatible errno values for pouch
binaries.

This is scripture per CLAUDE.md. Updates require user signoff; the header
files referenced here (`kernel/include/thylacine/errno.h`,
`kernel/include/thylacine/notes.h`'s `snare:*` constants) are the
canonical sources for numeric values + string names.

## Why this exists

Before this scripture: the kernel returned a flat `-1` on most syscall
failures (boundary-line patch `0001-pouch-syscall-seam.patch` maps to
generic `EIO`); explicit `-errno` in `[-4095, -2]` passed through to
userspace; extinction messages were free-form strings; note names were
ad-hoc (`kill`, `interrupt`, `pipe`, `child_exit`); exit statuses
collapsed non-zero to `1`. The result was workable but unprincipled:
greppability was poor, cross-surface consistency required reading
multiple files, and the boundary-line patch had to assume the worst
(generic `EIO`) on the common path.

The user-authorized design (2026-05-26): a single registry that pins
*values* (numeric errno; thematic note names) so every surface that
emits or interprets an error references one source, ABI-stable from
v1.0 forward.

## Design principles

1. **Errno values match POSIX errno numbers.** A Thylacine syscall
   returning `-T_E_INVAL` produces POSIX `EINVAL` in pouch without
   translation. The boundary-line patch's existing
   `[-4095, -2] -> -errno` passthrough works unchanged; we only need to
   stop returning generic `-1`. The motivation: cross-project + POSIX-
   binary compatibility is free; we don't reinvent error semantics.

2. **Errno values are ABI-stable.** Once a value is assigned, it never
   changes. New errors append. Removed errors are reserved (the slot
   stays mapped to a forever-unused name; we don't reuse the number).
   Catches accidental ABI drift via `_Static_assert`.

3. **Fault notes are thematic (`snare:*` family).** Following CLAUDE.md
   §"Thematic naming" — `snare` is the trap/snare that catches the
   thylacine (mirrors how the kernel "catches" a userspace fault before
   the Proc terminates). The colon-separated suffix narrows the fault
   kind. Bounded to `NOTE_NAME_MAX = 16` bytes including NUL.

4. **One registry per category.** Errno values live in
   `kernel/include/thylacine/errno.h`. Fault names live in
   `kernel/include/thylacine/notes.h` (alongside the existing
   `NOTE_NAME_KILL` constant). Exit-status semantics live in
   `kernel/include/thylacine/proc.h` (the existing exit_status field;
   v1.0 collapses non-zero to 1; v1.x adds the structured 64-bit
   shape).

5. **Stratum's `STM_E*` enum is NOT unified with Thylacine's errno.**
   Stratum is a separate project with its own ABI commitments
   (CLAUDE.md §"Stratum coordination"). Thylacine consumes Stratum
   errors via 9P (which translates them across the wire) or via
   Stratum's `libstratum-9p` C ABI (where `stm_status_t` stays as-is).
   The two systems coexist at the boundary; neither subsumes the other.

## Errno registry

The canonical source is `kernel/include/thylacine/errno.h`. The values
below are the v1.0 set; additions append (no renumbering).

| Name           | Value | POSIX equiv  | Meaning |
|---|---|---|---|
| `T_E_OK`        | 0     | (success)    | Operation succeeded |
| `T_E_PERM`      | 1     | `EPERM`      | Operation not permitted (capability check failed). **DO NOT RETURN FROM A SYSCALL HANDLER** — `-1` collides with pouch's flat-error sentinel; use `T_E_ACCES` instead. The name remains for translation-from-userspace code. See errno.h for the full rationale. |
| `T_E_NOENT`     | 2     | `ENOENT`     | No such file or namespace entry |
| `T_E_SRCH`      | 3     | `ESRCH`      | No such process (a pid-addressed op names no live Proc; the PTY-1 `setpgid`/`getsid` contour) |
| `T_E_IO`        | 5     | `EIO`        | I/O error (storage, device, network) |
| `T_E_2BIG`      | 7     | `E2BIG`      | Argument list too long — the argv/envp asked for does not fit the new image's frame budget. Distinct from `EINVAL` in the way a caller acts on: `EINVAL` says the request was malformed and retrying it verbatim is pointless, `E2BIG` says it was well-formed and too large, so splitting it will work (which is what `xargs` exists to do). Appended for the LINEAGE L-6c envp projection (#140), the commit wiring its first consumer, under the 2026-08-03 signoff (#142). Emitted by the ENVIRONMENT bounds only (`EXEC_ENV_MAX` / `EXEC_ENV_DATA_MAX`, in both the `/env` projection and the vivarium `envp` walk); the neighbouring **argv** bounds still answer `T_E_INVAL`, because that is a landed native ABI (`SYS_EXECVE`, L-2a) whose error code is its own deliberate decision, reserved to the #142 rollout rather than changed as a side effect of adding envp |
| `T_E_BADF`      | 9     | `EBADF`      | Bad handle / fd |
| `T_E_CHILD`     | 10    | `ECHILD`     | No child processes — a wait names no matching child. The termination condition of every reap loop, which is why the near-miss answers will not do: a bare `-1` reads to a Linux guest as `EPERM`, the #100 class of wrong answer. Appended for the LINEAGE L-6b `wait4` row. `wait_pid_for`'s two `-1` conditions (no matching child; a #811 death-interrupted sleep) both map here — sound because the death path returns through the sync-from-EL0 tail, where `el0_return_die_check` is noreturn on the die branch, so no observer can tell them apart |
| `T_E_AGAIN`     | 11    | `EAGAIN`     | Resource temporarily unavailable (queue full; would block) |
| `T_E_NOMEM`     | 12    | `ENOMEM`     | Out of memory |
| `T_E_ACCES`     | 13    | `EACCES`     | Permission denied (rights check failed; W^X violation) |
| `T_E_FAULT`     | 14    | `EFAULT`     | Bad address (uaccess fault on user VA) |
| `T_E_BUSY`      | 16    | `EBUSY`      | Resource busy (lock contention; mount-busy) |
| `T_E_EXIST`     | 17    | `EEXIST`     | Already exists (mount over existing point; create-excl) |
| `T_E_NODEV`     | 19    | `ENODEV`     | The backing endpoint disappeared (the Loom device-gone terminal CQE; MENAGERIE §10) |
| `T_E_NOTDIR`    | 20    | `ENOTDIR`    | A path prefix component is not a directory, so resolution cannot continue through it (`/bin/ls/foo`). Computed from the `QTDIR` bit BEFORE the X-search, so the answer does not turn on the meaningless x bit of a non-directory. Four producers, all the same rule: `stalk`'s real-component gate (#79); `stalk`'s `.`/`..` gate, since both are path components and the position they resolve in must be a directory too (#81); `stalk`'s trailing-slash gate, where the separator asserts the whole path names a directory (#82, POSIX 4.13 — three sites, since two success exits skip the quarry); and the single-hop `SYS_WALK_OPEN` / `SYS_WALK_CREATE` handlers, on both the source's type and a source Dev with no `.walk` slot (#80, #81). The `.`/`..` gate reads the tip UNCROSSED and the trailing-slash gate the quarry CROSSED — they ask different questions (where resolution stands vs what the path names), see `docs/reference/104-stalk.md` |
| `T_E_INVAL`     | 22    | `EINVAL`     | Invalid argument |
| `T_E_NOSYS`     | 38    | `ENOSYS`     | Function not implemented (placeholder syscall slot or unimpl path) |
| `T_E_PIPE`      | 32    | `EPIPE`      | Broken pipe (write to closed pipe/socket) |
| `T_E_RANGE`     | 34    | `ERANGE`     | Numerical result out of range |
| `T_E_LOOP`      | 40    | `ELOOP`      | Symlink resolution did not terminate, or terminated on a link the caller refused to follow. TWO emitters share the code, as on Linux (DISTRO D-1, user-signed-off 2026-08-05): (1) the resolver exceeded `STALK_MAX_FOLLOWS` (40) -- a cycle, or a chain deeper than the bound; (2) the final component is a symlink and the open passed `SYS_WALK_OPEN_NOFOLLOW` **without** `T_OPATH`, i.e. Linux `O_NOFOLLOW` semantics: the caller asked for not-a-symlink and got one, and a symlink cannot be opened for byte I/O. With `T_OPATH` the same flag instead RETURNS the link (the v1.0 `lstat` spelling) and no error is raised. A trailing `/` overrides the flag entirely (POSIX 4.13), so `link/` follows and can answer `T_E_NOTDIR` instead |
| `T_E_TIMEDOUT`  | 110   | `ETIMEDOUT`  | Operation timed out |
| `T_E_CANCELED`  | 125   | `ECANCELED`  | Operation canceled (Loom LINK-chain cancel: a CQE for a linked SQE dropped when an earlier link failed) |

The values follow Linux's errno numbering (which musl + glibc agree on
for AArch64). Pouch's `bits/errno.h` is unchanged; the boundary-line
patch's `__syscall_ret` already passes `-errno` in `[-4095, -2]`
through to set userspace `errno`.

**Adding a new errno** (per-chunk discipline):
1. Append to `errno.h` with the next-available value (don't gap-fill
   reserved slots; just pick the next POSIX errno number you need).
2. Append a row to the table above.
3. Update CLAUDE.md's audit-trigger row "Errno ABI surface" if the new
   code is referenced by a load-bearing surface.
4. The kernel-side syscall handler returns `-T_E_<NAME>` instead of
   `-1`; the boundary-line patch needs no change.

**Removing a name**: forbidden at v1.0. If a code stops being used,
delete it from the table but reserve the value in the header
(`/* reserved; was T_E_X */`).

## Fault-note naming — the `snare:*` family

The canonical source is `kernel/include/thylacine/notes.h`. Each name
fits within `NOTE_NAME_MAX = 16` bytes including the NUL terminator.

| Name              | Length | POSIX-equiv signal | Cause |
|---|---|---|---|
| `snare:segv`      | 10+1   | `SIGSEGV`          | EL0 data/instruction fault on a VA with no covering VMA, or W^X / permission violation |
| `snare:bus`       | 9+1    | `SIGBUS`           | **RESERVED -- no v1.0 emitter**: a VA-inside-a-VMA-but-Burrow-cannot-satisfy fault would map here, but `arch_fault_handle` returns `FAULT_UNHANDLED_USER` and the EL0 dispatcher posts `snare:segv` (the bus subset is not yet distinguished, like `snare:fpe`). Reserved so callers do not invent a name. |
| `snare:align`     | 11+1   | `SIGBUS` (subset)  | EL0 PC or SP alignment fault |
| `snare:bti`       | 9+1    | `SIGILL` (subset)  | EL0 BTI fault (indirect branch to non-`bti j/c/jc` target on FEAT_BTI hardware) |
| `snare:brk`       | 9+1    | `SIGTRAP`          | EL0 `brk #imm` (assertion / debug trap) |
| `snare:ill`       | 9+1    | `SIGILL`           | EL0 unhandled sync exception (unknown EC) |
| `snare:fpe`       | 9+1    | `SIGFPE`           | EL0 floating-point exception. **RESERVED — no v1.0 path emits this**: the EL0 dispatcher in `arch/arm64/exception.c::exception_sync_lower_el` has no case for `EC_FP_ASIMD` / `EC_FP_TRAP_AARCH64` / `EC_SVE_TRAP`; any FP trap from EL0 currently falls through to the `default:` case and emits `snare:ill`. v1.x wires the FP ECs to `snare:fpe`. The constant is reserved so callers don't have to invent a name. (F6 audit close.) |

The `snare:` prefix is reserved for kernel-synthetic fault notes. User
processes that want to define their own structured event names should
NOT use the `snare:` prefix (the kernel rejects, returning -1 from,
`notes_post` with a `snare:`-prefixed `name` from a non-kernel-synthetic
caller; see `kernel/notes.c::notes_post`).

**Bit-position assignment in `note_mask`**: each `snare:*` shares the
existing `NOTE_BIT_*` block (extends with `NOTE_BIT_SNARE`). Setting
the bit defers delivery of EVERY `snare:*` (the v1.0 supported set
treats them as a single class for masking purposes; per-fault-kind
masking is a v1.x extension if a real need surfaces).

**Default action** for an unhandled `snare:*` note: terminate the
offending Proc via `exits(name)`. The kernel does NOT extinct on a
user-mode fault at v1.0 — the user binary dies cleanly, parent reaps
via `wait_pid`, kernel continues serving other Procs. (Prior to the
P6-pouch-stratumd-boot 16b-γ-mount-bind hardening pass the kernel
extincted on EL0 unhandled faults; the change documented here is
the close of the auditor's #3a recommendation.)

**Multi-thread Proc fault** (the prior v1.0 carve-out is now CLOSED --
RW-1 C-F1, commit `2891bf2`): a multi-thread Proc (`thread_count > 1`)
that faults at EL0 group-terminates the WHOLE Proc -- the kernel does
NOT extinct. `proc_fault_terminate` posts the `snare:*` cause and
cascades peer-Thread termination through the `SYS_EXIT_GROUP`
group-terminate machinery (ARCH 7.9.1 / I-24; the #809 / #811 universal
death-interruptible-sleep cascade), then the offending Proc dies and the
kernel keeps serving others. The earlier limitation -- a multi-thread
fault extincted the kernel with `EL0 fault in multi-thread Proc
(v1.x: cross-thread shootdown)` -- was retired when `SYS_EXIT_GROUP`
(syscall 60) landed; that message no longer exists. The single-thread
and multi-thread fault paths now both terminate only the offending Proc.
`proc_fault_terminate` still emits the uart diagnostic before
terminating, so test failures attribute correctly.

## Exit-status semantics

v1.0: `kernel/proc.c::sys_exits_handler` collapses non-zero exit
statuses to `1` (boolean ok/fail). The structured 64-bit
exit_status (encoding `(signal | code | wait-status-shape)`) is a
v1.x extension — landed when the first POSIX program needs to
distinguish "exited cleanly with code 42" from "killed by snare:segv".

The current v1.0 mapping:
- `exits("ok")` → `exit_status = 0`
- `exits(msg)` for any non-"ok" message → `exit_status = 1`
- `exits("snare:segv")` → `exit_status = 1` (fault-terminated, but the
  current ABI doesn't distinguish fault from clean-non-zero exit)

The fault termination at v1.0 still produces useful diagnostic via the
uart line emitted by `proc_fault_terminate` before `exits` runs:
```
user fault: pid=42 reason="snare:segv" addr=0x000000000123abcd -- terminating Proc
```
A parent doing `t_wait_pid` sees `exit_status = 1`; the uart log
attributes the cause. The v1.x richer-exit-status lift wires the
distinction into the wait API.

## Extinction messages

`extinction()` and `extinction_with_addr()` take free-form strings.
Per CLAUDE.md §"Thematic naming", panic-level messages use the
`extinction` lexicon (extinction = ELE = Extinction Level Event). The
strings are NOT structured codes; they're human-readable diagnostics.

We do NOT plan to enumerate extinction codes at v1.0. Rationale: every
extinction is a kernel bug or a hardware fault that has bypassed
recovery; the diagnostic is already wired to the agentic-loop ABI
(`EXTINCTION:` prefix; see CLAUDE.md §"Boot banner contract"); adding
a code registry would obscure the actual cause without buying
machine-readability that anyone needs. The agentic-loop parser keys on
the prefix + the full message line.

## Boundary-line translation (pouch)

The existing patch `usr/lib/pouch/patches/0001-pouch-syscall-seam.patch`
maps Thylacine syscall returns to POSIX errno as follows:

| Return value `r`         | Pouch `errno`        |
|---|---|
| `r >= 0`                  | (no error; success path) |
| `r == -1`                 | `EIO` (generic — Thylacine signalled an error with no specific code) |
| `r in [-4095, -2]`        | `-r` (specific POSIX errno) |
| `r <= -4096`              | `EIO` (defense-in-depth on malformed return) |

With the new errno registry, Thylacine kernel code returns `-T_E_<NAME>`
instead of `-1` wherever the cause is known. Pouch sees the POSIX
errno number directly. No boundary-line patch change needed.

**Rollout policy** (the retrospective sweep — *user-decided 2026-06-24,
the "errno-rollout arc"*).

The original policy below deferred the sweep ("A retrospective sweep is
NOT scheduled — incremental + per-touch is cheaper"). That deferral is
**reversed**: the GOOS=thylacine Go toolchain is the forcing function.
Go's runtime decodes a negative syscall return Linux-style (`CMN $4095`),
so a bare `-1` becomes `errno == EPERM` ("operation not permitted") — and
`os.IsNotExist`, the `O_CREATE` create-or-open fallback, and the build
cache's existence checks ALL depend on distinguishing `ENOENT` from a real
error. A `-1`-for-everything kernel makes a correct toolchain (and any
Linux-compat consumer) impossible. So the sweep is now scheduled.

The original staged policy still holds for NEW syscalls:
- Sub-chunks introducing NEW syscalls return `-T_E_<NAME>` from day one.
- The boundary-line patch's `EIO` fallback (the `-1` row) still covers any
  not-yet-swept callsite; fidelity improves monotonically as the sweep
  lands.

**Architecture (propagate the known cause; do not re-classify).** The real
errno almost always already exists at the failure source — it is only
*discarded* on the way up:
- The 9P client (`kernel/9p_client.c`) already returns `-(int)ecode` from
  an `Rlerror` (I-14 bounds the hostile ecode), and the errno values are
  POSIX-aligned (`T_E_NOENT == 2 == ENOENT == the 9P ecode`), so a
  Stratum `ENOENT`/`EEXIST` arrives intact — the Dev layer just dropped it
  by collapsing `dev9p_*` to `NULL`/`-1`.
- `stat_native` already returns `int`; `dev9p_stat_native` can return the
  real `-errno` instead of `-1`.
- `stalk()` already *knows* its own failure reason locally (walk-miss vs a
  `perm_check` X/RW denial vs a structural reject); it discards it by
  returning `NULL`. Threading an `int *errp` out-param recovers it.
- The ~700 `-1` sites in `kernel/syscall.c` are mostly *local* handler
  validations (handle-miss → `EBADF`, bad arg → `EINVAL`, missing right →
  `ACCES`, uaccess fault → `FAULT`) — a one-line errno swap at the site,
  no deep threading.

**The binding rules.**
- **NEVER return `-T_E_PERM` (== `-1`)** from a handler — it collides with
  the generic `-1` sentinel (errno.h warning). A capability/permission
  denial returns `T_E_ACCES` (13); reserve `T_E_PERM`'s *name* for the
  symbolic constant only.
- Reserve a bare `-1` for a *truly generic / unclassifiable* failure. When
  the cause is known, return the specific `-T_E_<NAME>`.
- **Backward-compatible**: native `libthyla-rs`/`libt` callers test `r < 0`
  — a `-2`/`-17` is still `< 0`, so no native consumer regresses; pouch
  and Go observe the *correct* POSIX errno instead of `EIO`/`EPERM`.

**Key FS-path mappings** (the toolchain-load-bearing set):

| Operation | Failure | Returns |
|---|---|---|
| `SYS_OPEN` / `SYS_WALK_OPEN` | component not found | `-T_E_NOENT` (2) |
| `SYS_OPEN` / X-search | `perm_check` denial | `-T_E_ACCES` (13) |
| `SYS_WALK_CREATE` | target already exists | `-T_E_EXIST` (17) |
| `SYS_WALK_CREATE` | missing parent component | `-T_E_NOENT` (2) |
| `SYS_WALK_CREATE` | not a directory | `-T_E_NOTDIR` (20) |
| `SYS_UNLINK` (rmdir) | directory not empty | `-T_E_NOTEMPTY` (39) |
| `SYS_FSTAT` / `SYS_*` | bad / wrong-kind handle | `-T_E_BADF` (9) |
| any handler | structurally bad argument | `-T_E_INVAL` (22) |
| any handler | uaccess fault on a user VA | `-T_E_FAULT` (14) |

**Staging** (audit-bearing — the FS-mutation + resolution surfaces are
trigger surfaces; self + Fable audit before the arc closes):
- **ER-0** — this scripture (no code).
- **ER-1** — the resolution keystone: `stalk()` `int *errp` (walk-miss →
  `NOENT`, perm → `ACCES`, structural → `INVAL`, stat-fail → propagate) +
  `dev9p_stat_native` real `-errno` + `SYS_OPEN`/`SYS_WALK_OPEN` return
  `-errno`. Unblocks the Go build's `os.IsNotExist` + create-or-open.
- **ER-2** — FS mutation: `SYS_WALK_CREATE` → `EEXIST`/`NOENT`/`NOTDIR`,
  `SYS_UNLINK`/`SYS_RENAME` → `NOENT`/`NOTEMPTY`/..., `SYS_FSTAT`/`LSEEK`/
  `READDIR`/`FSYNC`/`WSTAT`/`CHDIR` → their specific errnos.
  **The name-op half LANDED at #80**: `Dev.rename`/`Dev.unlink` no longer
  flatten the server's verdict (no side-channel was needed — unlike
  `Dev.create`, those slots already return `int`; the value was simply being
  discarded), and `SYS_UNLINK`/`SYS_RENAME` forward it. **Measured on the live
  Stratum FS**: unlink-a-directory → `EISDIR` (21), rmdir-a-non-empty-directory
  → `ENOTEMPTY` (39). A server errno with no `T_E_*` name crosses BY VALUE, so
  neither needed a registry append — only kernel-ORIGINATED errnos must be
  named here. The `SYS_FSTAT`/`LSEEK`/`READDIR`/`FSYNC`/`WSTAT`/`CHDIR` half
  is still owed.
- **ER-3** — the rest of the surface: the local-validation `-1` sites
  (`EBADF`/`EINVAL`/`ACCES`/`FAULT`/...) across the syscall families.
  **The four FS name-op handlers LANDED at #80** (`SYS_WALK_OPEN`,
  `SYS_WALK_CREATE`, `SYS_RENAME`, `SYS_UNLINK` — every local reject now
  answers a specific code). The `!t`/`!p` preamble guards deliberately stay
  `-1`: they are structurally unreachable from EL0.
  **The byte-I/O family LANDED at #100** (`SYS_READ`, `SYS_WRITE`,
  `SYS_PREAD`, `SYS_PWRITE`, `SYS_CLOSE`, `SYS_LSEEK` — the handle, O_PATH,
  missing-vtable, user-buffer, whence and offset gates), together with the
  pipe Dev, where `T_E_PIPE` had been defined and ABI-pinned since this
  scripture landed **with no emitter anywhere in the tree**: a write to a
  closed pipe returned the flat `-1`, so EPIPE was unobtainable through
  *both* boundaries. That is the shape to watch for elsewhere in the
  rollout — a registered code with zero emitters is a contract stated and
  not kept, and four separate comments (in `pipe.c`, `sys_write_handler`,
  `0007-pouch-signals.patch`, and the Go fork's ABI header) asserted the
  behaviour the code did not implement.
  **RESIDUAL**: three sites mean POSIX `ESPIPE` and still return `-1`
  (`SYS_LSEEK` and the positioned `SYS_PREAD`/`SYS_PWRITE` arms on a
  non-seekable Dev). `T_E_SPIPE` (29) is not registered and appending it is
  signoff-bearing per CLAUDE.md, so they were left honest rather than given
  a plausible-but-wrong `EINVAL`. Tracked as task #106.
  The remaining syscall families are still owed.
- **ER-4** — pouch `__syscall_ret` rework (the `-1 -> EIO` special case is
  now reachable only by the residual truly-generic returns; verify pouch
  programs observe the correct errnos). Note #100 already moved the pouch
  boundary for the byte-I/O family without touching the patch: a specific
  `-T_E_*` lands in the `[-4095,-2]` window `__syscall_ret` already decodes,
  so pouch sees the real errno and the `-1` case is simply no longer taken.
  The rework is what finally REMOVES that case, and cannot land until the
  ER-3 sweep leaves no `-1`-returning handler behind it.
- **ER-5** — focused Fable + self audit over the swept surface, SMP gate,
  in-VM Go-build proof, close.

## Cross-project coordination

Stratum's `STM_E*` enum (`stratum/v2/src/include/stratum/error.h`)
predates this design and is NOT subsumed. Thylacine consumes Stratum
errors via two surfaces:
- 9P wire (Stratum's server translates `stm_status_t` -> 9P `Tlerror`
  with a POSIX errno). Thylacine kernel sees the POSIX errno on the
  client side; no translation needed.
- `libstratum-9p` C ABI (Thylacine-userland tools link against this).
  `stm_status_t` constants pass through; the caller must include
  Stratum's header.

The two error registries (Thylacine's `T_E_*` and Stratum's `STM_E*`)
intersect at the POSIX errno layer. A future v2.x might unify them by
shared codepoints, but at v1.0 they coexist.

## Audit-trigger surface

A change to either header (`thylacine/errno.h` or the `snare:*`
constants in `thylacine/notes.h`) is audit-bearing per CLAUDE.md:
- Errno value renumbering is an ABI break; the audit prosecutor
  re-validates every syscall surface that returns the changed value.
- A new `snare:*` name affects the EL0-fault termination path
  (`proc_fault_terminate` + `exception_sync_lower_el`); the prosecutor
  re-validates the fault-classification logic.

See CLAUDE.md §"Audit-triggering changes" for the row.

## Status

| Component                                  | Status   | Sub-chunk |
|---|---|---|
| `docs/ERRORS.md` (this file)               | LANDED   | scripture |
| `kernel/include/thylacine/errno.h`         | LANDED   | scripture |
| `snare:*` constants in `notes.h`           | LANDED   | scripture |
| CLAUDE.md scripture row + audit-trigger row| LANDED   | scripture |
| `proc_fault_terminate` impl + EL0 wiring   | LANDED   | #3a       |
| Pouch `snare:*` → POSIX signal mapping     | DEFERRED | v1.x      |
| Structured 64-bit exit_status              | DEFERRED | v1.x      |
| Retroactive `-T_E_*` rollout to old syscalls | DEFERRED | per-touch |
