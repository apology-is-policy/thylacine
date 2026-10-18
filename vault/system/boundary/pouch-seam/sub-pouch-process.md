---
id: sub-pouch-process
type: sub
parent: moc-pouch-seam
title: "Process lifecycle — posix_spawn, wait, pipe, the environ populate, the terminators"
code:
  - usr/lib/pouch/patches/0026-pouch-process.patch
  - usr/lib/pouch/patches/0052-pouch-spawn-action-sources.patch
  - usr/lib/pouch/patches/0053-pouch-spawn-source-query.patch
  - usr/lib/pouch/patches/0054-pouch-dup2-self-query.patch
  - usr/lib/pouch/patches/0055-pouch-descriptor-operations.patch
  - usr/lib/pouch/patches/0057-pouch-kill-probe-getppid.patch
  - usr/pouch-hello/pouch-hello-descriptor-ops.c
  - usr/pouch-hello/pouch-hello-dup2-self.c
  - usr/pouch-hello/pouch-hello-handle-query.c
  - usr/lib/pouch/patches/0043-pouch-identity-calls.patch
  - usr/pouch-hello/pouch-hello-identity.c
  - usr/pouch-hello/pouch-hello-spawn-actions.c
  - usr/lib/pouch/patches/0025-pouch-env.patch
  - usr/lib/pouch/patches/0011-pouch-abort.patch
  - usr/lib/pouch/patches/0012-pouch-mallocng-crash.patch
  - usr/lib/pouch/patches/0013-pouch-mallocng-diag.patch
audit: hard
guarded-by: [inv-i24]
validated-by: [prose, gate-smp]
locks: []
design: ["docs/POUCH-DESIGN.md", "docs/LLVM-DESIGN.md"]
created: 2026-08-01
updated: 2026-10-18
---
## Descriptor operations checkpoint (October 11; main B-3b)

Patch0055 (codex/astra 0052) consumes approved native 130/131 for public F_SETFD
and exact-target dup2/dup3; F_GETFD uses 129. Self-dup preserves flags. Live tagged sockets remain
explicit ENOSYS for unsupported flag/target operations; invalid tags are EBADF.
The current spawn interpreter tracks child flags and applies final exclusion
after actions; self-DUP2 actions clear the child flag. OPEN with O_CLOEXEC is
rejected before temporaries; sparse lists still fail. Raw internal SYS_fcntl
callers and general inheritance remain gaps. See `docs/NATIVE-DESCRIPTOR-OPERATIONS-REVIEW.md` for actual-source tests, audit reasoning and source-matched qualification limits.

Patch0056 (B-3b) completes the family: `dup` is DUP_TO with DUP_LOWEST and
floor 0; fcntl's F_DUPFD / F_DUPFD_CLOEXEC pass their floor (a negative one
widens and the kernel refuses it after a closed fd's EBADF); F_GETFL decodes
FLAGS_GET's status word; F_SETFL changes O_NONBLOCK only (native 123),
accepts a restated O_APPEND and refuses a flipped one; `pipe2` now honours
O_CLOEXEC (FLAGS_SET) and O_NONBLOCK (123) on both ends; a set that fails
fails the call and closes both ends, except an end whose own set answered
EBADF (a peer thread closed it, and its number may already be another
thread's; round 2). Astra's four guest witnesses (descriptor-ops, dup2-self, handle-query,
spawn-actions) were built and run by nothing on main; B-3b compiles them into
`/bin` and `tools/interactive/ls-descriptors.exp` runs each as uid 1000.
dup2-self and handle-query set close-on-exec before they read it. As written,
both read only a clear flag, and a CI image whose FLAGS_GET hid the flag from
uid 1000 passed them; descriptor-ops alone turned red (2026-10-15).
With that image and a posix_spawn whose addclose did nothing for uid 1000, all
four turned red (2026-10-18), but spawn-actions only where the skipped close
left a hole in the child's table, which posix_spawn refuses with EINVAL. The
child gets only stdio and the slots its actions establish (the F7 inheritance
gap), so closing any other descriptor is a no-op, and a skipped close that
leaves the table dense is invisible from the parent. The child now checks that
each slot it was told to close answers EBADF, and established-close-dense
closes a slot established directly above stdio.

The round-1 close (2026-10-15): `pipe2`'s failure unwind closes with the raw
`__syscall(SYS_close)`, as `popen` does, never the cancellable
`close()`. The descriptor-ops witness reads a std stream's FLAGS_GET word as
the flag bit plus status (a console stream is ORDWR, word 0x200), testing
bit0 alone. With `pipe2(O_CLOEXEC)` flagging both ends, a `popen("r")` whose
caller has fd 0 closed gets its read end at fd 0, which spawn's final
exclusion then removes from the child, leaving a hole at child fd 0: spawn
refuses it EINVAL (the approved sparse-spawn contract) where it used to hand
the child the read end as its stdin (a Caveat below; OPEN-BUGS).

## Identity — the calls that could not report failure (A-6)

A program's own identity belongs here with the rest of what it knows about
itself, and until 2026-09-22 Pouch lied about it.

musl's `getuid` / `geteuid` / `getgid` / `getegid` are **cannot-fail by
contract** — each is literally `return __syscall(SYS_x);` with no error path.
The seam parks their numbers at the unimplemented-syscall sentinel, so the
sentinel's `-ENOSYS` was cast straight to `uid_t` and every Pouch program was
told its uid was `0xFFFFFFDA`. Not an error it could check: a **lie it could
not detect**.

**This is a second SHAPE of sentinel defect, and the distinction is why no
sweep had found it.** The known family — the unchecked-wrapper patches — is
*a call whose return is IGNORED and whose out-struct the caller then reads*,
and the sweep that found them was scoped, in its own words, to "a wrapper over
a sentinel whose return value is ignored". Here the return is not ignored:
**the return IS the lie.** A sweep scoped to ignored returns is structurally
blind to it. The shape-2 sweep — parse the patched number table into parked vs
real, find every `return __syscall(` on a parked number, then judge each by
whether the CALLER can tell — found nine sites; four were these, and the rest
are recorded with the residue.

The kernel had the truthful answers throughout, and the **native** substrate
already used them, so the asymmetry this closed was precise: the native
substrate told the truth and the POSIX boundary-line lied.

`effective == real` here is a **finding, not a shortcut**: there is no
`setuid`, no `seteuid` and no effective-uid field anywhere in the kernel, which
is the no-ambient-authority invariant holding by construction. A future change
that made `geteuid()` diverge would be an invariant violation before it was an
ABI change.

`getppid` was **left parked** here -- the only one of the five with no kernel
call behind it, returning the sentinel's -38 as a pid. Vote 6 (2026-10-14)
gave it native `SYS_GETPPID` (132); patch 0057 points it there, as `getpid`
points at 72, and `pouch-hello-signals`' `probe` leg checks a child's
`getppid()` against the pid its parent handed it.

The witness is `pouch-hello-identity`, and it deliberately does **not** assert a
literal principal — that would pass for the wrong reason the moment the boot
principal changed, and would never prove the call reaches the kernel at all.
It reads the same Proc's identity back through devproc's independent
`principal:<N> gid:<M>` channel and demands the two agree. Measured both ways:
with the patch removed the probe reports the sentinel by name and the boot
fails; with it applied the two channels agree.

## Purpose

Making one program start, wait for, and talk to another on a kernel with
no `fork` and no `execve` — plus the two adjacent surfaces that decide
how a program *ends* (the abort/assert terminators) and what it *knows*
at startup (the environment). The anonymous-memory backend lives here
too: `malloc` is a process-lifecycle concern on a system where the heap
is a Burrow.

## Contract

- `posix_spawn(res, path, fa, attr, argv, envp)` →
  `SYS_SPAWN_FULL_ARGV` (49) after resolving `fa` statically.
  `posix_spawnp` PATH-searches first.
- `waitpid` / `wait4` → `SYS_WAIT_PID` (22), with flag and status-word
  translation; `wait4`'s `rusage` is zero-filled.
- `pipe` / `pipe2` → `SYS_PIPE` (8) through a bespoke two-register asm
  shim. `dup2`/`dup3` onto a chosen target: `ENOSYS`.
- `abort()` → one `brk #0xab06`, which the kernel turns into the fault note
  `snare:abort`: the Proc dies signalled, `WTERMSIG == SIGABRT`. mallocng's
  internal `assert` calls `abort()`. `_Exit(127)` follows the `brk` only as a
  fallback that a kernel honouring the immediate never reaches.
- `waitpid` reports the POSIX status word: `WIFEXITED`/`WEXITSTATUS` for an
  exit, `WIFSIGNALED`/`WTERMSIG` for a death by note (gcpark-b, 2026-10-12).
- `__pouch_env_init()` populates `__environ` from `/env` before the ctors.
- `mmap` / `munmap` / `mprotect` / `madvise`: the memory seam, [[sub-pouch-mem]]
  (0003, made exact at B-1b by 0044). `brk` stays a sentinel here.

## Mechanism

**`posix_spawn` resolves file_actions STATICALLY.** Upstream clones with
`CLONE_VM|CLONE_VFORK` into a child that runs the actions and then
`execve`s — impossible here, because a Thylacine Proc cannot
clone-and-replace its image. `SYS_SPAWN_FULL_ARGV` instead installs an
explicit *positional* fd list (`fd_list[i]` → child fd `i`, contiguous,
lowest-free), so pouch interprets the action list against a MODEL of the
child's fd table and emits that list. `FDOP_DUP2` resolves its source
through the model (a source not in the model is a raw parent fd);
`FDOP_OPEN` opens in the parent and records the fd; `FDOP_CLOSE` clears
the slot. The dominant toolchain pattern (open a redirect, dup2 it onto
0/1/2, close the temp) resolves to `{0,1,2}`. `FDOP_CHDIR`/`FCHDIR`
answer `ENOSYS` rather than silently spawning in the wrong directory.
Slots 0/1/2 are seeded by PROBING the parent's std fds (a lowest-free dup
with each right, then close) — a parent lacking stderr must not list a
non-existent fd and fail the whole spawn.

**Action history and temporary descriptor provenance (0052).** A source
closed by an earlier child action cannot fall back to the parent's live fd.
The nearest earlier CLOSE/OPEN/DUP2 decides whether that source was removed
or reestablished. This check runs before opening parent-side temporaries.
Each newly opened fd is then checked against every original-parent source
mentioned in the action list, including aliases later closed or overwritten.
A collision proves that original source was absent: reject EBADF after
registering the temporary for cleanup. An explicitly child-created source
continues to resolve through the existing model. No additional R/W existence
probes or shared state are introduced; concurrent parent fd close/reuse stays
outside the existing supported envelope. The final kernel list still validates
original sources, with its existing errno collapse; a discarded invalid
original source without a temporary collision remains a tracked gap.
See `docs/POUCH-SPAWN-ACTION-REVIEW.md` for single-agent self-review, host
baseline/mutant evidence and the native14-case ordinary-user witness. The
full official sysroot rebuild includes static/shared libc; runtime7's static
probe verifies child bytes/wait/errors and exits0 with clean harness shutdown.
The witness measures an identical O_CREAT open to locate its temporary slot:
the parent-directory handle means /dev/null predicts a different slot. The
initial wrong-slot run is preserved as a failed fixture, not a code regression.
Existing Ninja and dynamic-loader behavior are not requalified by this probe.

**Contiguity is a hard requirement, not a nicety.** The kernel installs
the list at consecutive lowest-free slots, so a HOLE (`{0,2}`) cannot be
expressed and is `EINVAL`.

**The wait option word is translated; the status word is not.** The
kernel's `WAIT_CONTINUED` is 4 while musl's `WCONTINUED` is 8, and the
kernel REJECTS unknown flag bits — so the option word is mapped
bit-by-bit, never passed raw. `__pouch_wait` then always adds
`POUCH_WAIT_PACKED` (8, the kernel's `WAIT_PACKED`), so the kernel returns
the Linux `wait(2)` word for every wait: `(code << 8)` for an exit,
`signal & 0x7f` for a death by note, the stopped and continued shapes when
asked. That word goes to the caller verbatim. Before gcpark-b a plain wait
returned the RAW status (0/1) and this patch repacked it `(raw & 0xff) << 8`
— so a child killed by a fault or a note read as `WIFEXITED` with status 1,
and an `abort()` read as exit 127. The kernel now records the signal
([[sub-kernel-death]]), and only the packed word can carry it.

**`pipe` needs its own `svc`.** `SYS_PIPE` returns the read fd in `x0`
and the write fd in `x1`; musl's `__syscall` captures only `x0`, so
`__pouch_pipe` is a hand-written two-register shim (the native `t_pipe`
shape). It is therefore OUTSIDE the seam's sentinel guard by
construction — correct only because it names a real number.

**`dup2` onto a target** was ENOSYS until patch0055 (DUP_TO); `dup` until
patch0056 (DUP_TO's DUP_LOWEST). Patch0054 validates `old == new`
using native129 for a kernel descriptor or the existing locked Pouch socket
resolver for a tagged socket, including fresh sockets without kernel handles.
Absent slots return EBADF; historical native unknown-call -1 maps to ENOSYS.
Self-dup allocates nothing, acquires no object reference and preserves flags.
It observes validity at one instant; it does not pin concurrent close/reuse.
The12-case actual-source host suite and native uid1000 probes cover both
owners, table exhaustion and old-kernel refusal. See
`docs/POUCH-DUP2-SELF-REVIEW.md` for exact evidence and limits.

**`abort()` dies of `snare:abort`; mallocng's `assert` calls it.** musl's
`abort()` and mallocng's internal `assert` both ended in `a_crash()` — a
deliberate NULL store. When an EL0 fault extincted the kernel, that made
every `assert()` a whole-boot kill, so 0011 and 0012 routed both to
`_Exit(127)`: an orderly exit, but one a parent could not tell from a
program that returned 127. Since gcpark-b (2026-10-12) 0011's `abort()`
executes one `brk #0xab06`. The kernel's EL0 `brk` handler reads the
immediate ([[sub-kernel-exception]]), and `0xab06` (the kernel's
`SNARE_ABORT_BRK_IMM`) names `snare:abort`, so the Proc dies like any fault: the console prints
the `user fault: ... reason="snare:abort"` line with the program counter,
the group terminates, and the parent's `waitpid` reads `WIFSIGNALED` with
`SIGABRT`. A dedicated immediate rather than a NULL store, because a store
would read as `snare:segv` and a stray `brk #0` (a compiler trap) as
`snare:brk`. `_Exit(127)` stays after the `brk` as an unreachable fallback.
SIGABRT handlers do not run: there is no signal delivery to run them
(`raise(SIGABRT)` still answers `EINVAL`; [[sub-pouch-signal]]). 0012 now
makes mallocng's `assert` call `abort()` instead of `a_crash()`, so a heap
corruption reports as an abort. They are separate patches deliberately:
0012 touches a macro every mallocng allocation instantiates, so a future
audit can reason about the hot-path change alone. 0013 adds an opt-in
(`POUCH_MALLOCNG_DIAG`) dump of the corruption pattern *before* the
assertion fires, written with a direct `SYS_write` so it survives a
corrupted heap.

**The environment lives in `/env`, not `envp`.** The kernel writes
`envp[0] = NULL` for every program, so `__pouch_env_init` — called from
`__libc_start_main` after `__init_libc` (malloc + TLS up) and before the
ctors (so a constructor's `getenv` sees it) — readdirs `/env`, reads each
value, and builds a `"NAME=value"` vector for `__environ`. Fail-soft:
a missing `/env` leaves the kernel's empty envp. `/env` stays the source
of truth; a later `setenv` mutates only this in-process copy.

**The memory seam lives in [[sub-pouch-mem]] since B-1b.** Here `mmap` was
one argument wide (0003: the anonymous subset only, demand-zero RW whatever
the prot, `MAP_FIXED` refused) and mallocng's `PROT_NONE`-then-`mprotect`
metadata idiom survived on its `&& errno != ENOSYS` guard; 0044 mints at the
prot asked for and routes `mprotect` / `madvise` / `MAP_FIXED` to the kernel.
What stays here is `brk`: a sentinel, so mallocng's first `brk(0)` fails, it
sets `ctx.brk = -1`, and metadata allocation routes through mmap permanently
— a documented mallocng fallback.

## Data structures

`struct pouch_spawn_args` — a hand-mirrored **104-byte** copy of the
kernel's `sys_spawn_args`, pinned by **twenty-one** `_Static_assert`s: one
on the size and **one per field offset**. `struct __dirstream` (unchanged)
for the `/env` scan.

**A size assert cannot see a field-order mismatch, and that is why the
offsets exist.** Swap two same-width fields and the struct is still the
right size, still compiles, and a caller then fills the wrong one — here,
a page budget written into the phenotype word, which the kernel reads as
an unknown-bit phenotype request and refuses. The failure surfaces at
runtime, in the kernel, as a validation error naming neither field.

**The struct grew 96 → 104 because two branches each authored against the
same reserved slot.** A page budget and a phenotype word were independently
placed in the one `_pad_allow`, so the merge could not let either quietly
take the other's bytes and had to grow the struct instead. **A reserved pad
slot is a cross-branch collision point**, invisible on either branch alone
— each is a correct, size-preserving use of documented slack. The offset
asserts are what make the *next* claimant disagree with the kernel loudly,
at build time, rather than in a refused syscall.

**The last reserved slot is spent, and the build now checks this mirror
(2026-09-29).** The birth hold renamed the pad at offset 100 from `_pad_spawn2`
to `debug_flags` ([[sub-kernel-birth-hold]]). The size and every offset are
unchanged, so no pouch caller moves, and the patch's offset assert names the
new field. Pouch zero-fills the record and never sets the bit: a ported program
has no way to ask for a held spawn. `tools/check-spawn-args-mirrors.py` now
compares this struct with the kernel's on every build, field by field, reading
it straight out of the patch ([[sub-kernel-syscall-abi]]). That closes the half
of the next section's lesson a build step can close, the header against the
kernel. The other half, the header against the archive that gets linked, still
has nothing checking it.

### The asserts all held and the binaries were wrong anyway

Worth recording in full, because it is the sharpest available statement of
what a compile-time assertion buys.

Landing the offset pins exposed an unrelated defect: a workaround elsewhere
was a **blind directory overlay** that reverted the sysroot's C archive to a
day-old snapshot. Twenty-five ported binaries relinked against the **old
96-byte** struct, and the kernel's 104-byte validator refused their spawns.

**Both sets of assertions held throughout.** The new offset asserts were
compiled into the header the fresh build used and were correct about it. The
stale archive carried its own then-correct 96-byte assert and was correct
about *itself*. Two artifacts, two true self-descriptions, one broken link.

**A per-artifact assert certifies that artifact, never that the artifact in
the link is the one you just built.** Compile-time checking cannot reach
across a build-system substitution, because the substituted object was
compiled — correctly — at a different time against a different truth. The
gap is not in the assertions; it is that nothing compares the *thing you
built* to the *thing that got linked*.

The patch's own hunk header had to move 81 → 108 for the added lines,
counted **from the hunk body** rather than derived from the delta — `patch`
trusts the stated count and silently drops added lines past it, so an
arithmetic slip there removes content while reporting success.

## Concurrency

`posix_spawn` runs entirely in the parent (no vfork child), so the
internal-allocator remapping in `fdop.h` is undone and the plain
allocators are used. The `opened[]` array is bounded independently of the
slot count, because many opens can target the same bounded slot.

## Native source validation (October 11)

Patch0050 queries each original-parent DUP2 source before temporary opens,
including sources whose child aliases are later closed or overwritten. Earlier
child assignments still use the ordered child model. Standard streams are
seeded with the same nonallocating, rights-independent native128 query: EBADF
means absent; other errors stop spawn. A full parent table no longer makes a
live stream appear absent. Historical kernels return raw -1 for an unknown native call; this query alone
normalizes that value to ENOSYS and the new libc fails closed before publishing
a child. Explicit -ENOSYS is also passed through. Kernel and libc must therefore ship together.
Close-history and temporary-origin guards remain. Parent close/reuse and action
edits must still be serialized by the caller; this query does not pin a handle.

The maintained real-source host suite now has25 cases and close/query/legacy-error mutants;
the native spawn witness has16 cases. The dedicated metadata witness covers
narrowed aliases, rights-independent access and saturated tables, with a
separate old-kernel mode. Qualification is recorded in
`docs/NATIVE-HANDLE-QUERY-PROPOSAL.md`. General inheritance, F_GETFD/F_SETFD,
spawn attributes and signal policy remain separate unfinished contracts.

## Invariants enforced

- **[[inv-i24]] (consumer side).** An `abort()` from any thread kills the
  whole group: the `brk` lands in `proc_fault_terminate`, which terminates
  the group exactly once and records `SIGABRT` with the first claim, so a
  peer dying in the same group cannot replace it. The `_Exit(127)` fallback
  routes through `__NR_exit_group` → `SYS_EXIT_GROUP`, the same cascading
  terminate. Before #809 both overrides carried a safe-use envelope
  ("reachable only pre-thread-spawn or post-join") because `SYS_EXITS` with
  live peers extincted the kernel; the patch prose no longer describes it.
- **P-3** — `dup2`-onto-target, `FDOP_CHDIR`, file-backed `mmap`, and
  `MAP_FIXED` all fail loud rather than approximating.

## Error paths

`ENOENT` for every `SYS_SPAWN_FULL_ARGV` failure (name not resolvable /
bad ELF / OOM / a listed fd not a Spoor — collapsed at v1.0); `E2BIG`
for argv overflow; `EMFILE` for fd-list overflow and for pipe failure;
`ECHILD` for every wait failure. A failed assertion or `abort()` is not an
error return: the program dies of `snare:abort` and its parent reads
`SIGABRT`. `MAP_FAILED` + `ENOSYS` for the refused
mmap forms.

## Performance

A spawn is one syscall plus one parent-side open per `FDOP_OPEN`. The
`/env` populate is one readdir batch plus one open+read per variable, once
per process. Every `malloc` past mallocng's first is userspace-only.

## Prosecution

- The fd model's contiguity check and the `PSPAWN_MAXSLOT` bounds must
  hold for a crafted `file_actions` list (the `opened[]` guard exists
  precisely because open count is not bounded by slot count).
- The wait flag translation (bit-by-bit, plus `POUCH_WAIT_PACKED` on every
  call) — a raw musl flag word is rejected loud, but dropping
  `POUCH_WAIT_PACKED` is silent: every death would read as an exit.
- `__pouch_pipe`'s hand-rolled `svc` must name a real number; it is
  outside the sentinel guard.
- `abort()`'s `brk` immediate must equal the kernel's
  `SNARE_ABORT_BRK_IMM`; any other immediate dies as `snare:brk` (SIGTRAP).
  The patch writes the immediate as an asm literal, which no compile-time
  check can compare with a kernel header, so the pin is at run time:
  pouch-hello-signals' `abort` leg requires `SIGABRT` and fails on `SIGTRAP`. mallocng's
  `assert` must keep calling `abort()`, not `a_crash()` (which reads as
  `snare:segv`).
- `__pouch_env_init` must stay fail-soft and stay ordered after
  `__init_libc` and before the ctors.
- The 104-byte spawn-args mirror against the KERNEL's struct — **size AND
  every field offset**. A size-only pin is blind to a field-order swap, and
  the reserved slot is where independent branches collide. The build's mirror
  check reads the struct out of this patch, so a hunk that renames or moves a
  field must keep the struct parseable.
- **An assert certifies its own artifact, not the link.** A stale archive
  substituted into the sysroot carries its own correct assert for its own
  older layout, so a mismatched binary can ship with every assertion in the
  tree holding. Whatever compares built-to-linked has to live outside the
  compiler.
- **A patch hunk header's line count is authoritative and unchecked**: it is
  trusted, and added lines past it are dropped silently. Count the body.

## Seams

[[seam-pouch-dup2-target]] (freopen / login_tty / daemon / wordexp are
non-functional) · [[seam-pouch-spawn-envp]] (the passed `envp` is ignored;
the child inherits `/env` via the kernel clone).

## Caveats

- **A close-on-exec descriptor at fd 0 or 1 makes a stdio-inheriting spawn
  fail `EINVAL`** (since 0056) when a higher standard stream is open. A
  process with fd 0 (or 1) closed that opens anything O_CLOEXEC -- open,
  fopen "e", opendir, shm_open, mkostemp, the loader's opens, Rust std's
  File::open, the read end of `popen("r")`'s pipe -- gets it there flagged;
  spawn's final exclusion drops it from the child, and the hole is the
  sparse list spawn refuses. Linux would give the child a closed fd 0. Before
  0056 open ignored O_CLOEXEC and the child inherited the descriptor.
  OPEN-BUGS 2026-10-15 owns the inheritance model this belongs to.

- `posix_spawnattr` (SETSID / SETPGROUP / RESETIDS / SETSIG*) is ignored:
  there is no post-spawn child hook, and a fresh child inherits session,
  pgroup, identity, and default note dispositions — which satisfies the
  toolchain's (unset) attr by construction, but would silently ignore a
  program that set one.
- `pipe2` sets its flags after SYS_PIPE creates the ends, so a peer
  thread's spawn between the two steps can see an unflagged end (Linux's one
  call cannot).
- The `/env` populate caps at 512 variables / 256-byte names /
  8 KiB values, and silently skips a variable removed between readdir and
  open.

## Provenance

[[chg-2026-05-23-p6-mem-b]] (0003, mallocng over the burrow syscalls;
retargeted to `SYS_BURROW_ATTACH_LAZY` at #321) →
[[chg-2026-05-25-16b-gamma-mount-close]] (0011, the abort override) →
[[chg-2026-05-26-16bg-hardening-3b]] (0012) +
[[chg-2026-05-26-16bg-hardening-3c]] (0013) →
[[chg-2026-07-23-cl1b0-env]] (0025) →
[[chg-2026-07-23-cl1b-process]] (0026) →
gcpark-b (2026-10-12: 0011 `brk #0xab06`, 0012 assert → `abort()`, 0026
`POUCH_WAIT_PACKED` and the verbatim status word).

The 104-byte widening, the twenty offset pins, and the stale-archive
relink are [[chg-2026-08-16-pouch-offset-pins]].
