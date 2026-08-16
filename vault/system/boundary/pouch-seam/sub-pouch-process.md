---
id: sub-pouch-process
type: sub
parent: moc-pouch-seam
title: "Process lifecycle — posix_spawn, wait, pipe, the environ populate, the terminators"
code:
  - usr/lib/pouch/patches/0026-pouch-process.patch
  - usr/lib/pouch/patches/0025-pouch-env.patch
  - usr/lib/pouch/patches/0011-pouch-abort.patch
  - usr/lib/pouch/patches/0012-pouch-mallocng-crash.patch
  - usr/lib/pouch/patches/0013-pouch-mallocng-diag.patch
  - usr/lib/pouch/patches/0003-pouch-mman.patch
audit: hard
guarded-by: [inv-i24]
validated-by: [prose, gate-smp]
locks: []
design: ["docs/POUCH-DESIGN.md", "docs/LLVM-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-16
---
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
- `abort()` and mallocng's internal `assert` → `_Exit(127)`.
- `__pouch_env_init()` populates `__environ` from `/env` before the ctors.
- `mmap(MAP_ANON)` → `SYS_BURROW_ATTACH_LAZY` (83); `munmap` →
  `SYS_BURROW_DETACH` (38). File-backed and `MAP_FIXED`: `ENOSYS`.

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

**Contiguity is a hard requirement, not a nicety.** The kernel installs
the list at consecutive lowest-free slots, so a HOLE (`{0,2}`) cannot be
expressed and is `EINVAL`.

**Two wait translations are load-bearing.** The kernel's `WAIT_CONTINUED`
is 4 while musl's `WCONTINUED` is 8, and the kernel REJECTS unknown flag
bits — so the option word is mapped bit-by-bit, never passed raw. And a
plain wait returns the RAW exit status (0/1), which musl's `W*` macros
would decode as Linux's packed word — so it is repacked
`(raw & 0xff) << 8`, making `WIFEXITED` true and `WEXITSTATUS == raw`.
A job-control wait already returns a Linux-packed word and is forwarded
verbatim.

**`pipe` needs its own `svc`.** `SYS_PIPE` returns the read fd in `x0`
and the write fd in `x1`; musl's `__syscall` captures only `x0`, so
`__pouch_pipe` is a hand-written two-register shim (the native `t_pipe`
shape). It is therefore OUTSIDE the seam's sentinel guard by
construction — correct only because it names a real number.

**`dup2` onto a target has no primitive and no consumer.** `handle_dup`
allocates the lowest free slot — that is `dup()`, not `dup2()`. The
toolchain never needs it (posix_spawn resolves statically), so
onto-target fails LOUD with `ENOSYS`; `old == new` returns `new`
best-effort, since there is no rights-independent fd-existence probe (a
0-rights dup is rejected, and the source's own rights are unknown).

**`_Exit(127)` replaces `a_crash()` twice, for one reason.** musl's
`abort()` and mallocng's internal `assert` both terminate via
`a_crash()` — a deliberate NULL deref. Under v1.0's
`FAULT_UNHANDLED_USER` policy an EL0 fault EXTINCTS THE KERNEL, so every
`assert()` in every pouch program was a whole-boot kill. 0011 and 0012
route both to `_Exit(127)` — the status musl itself reaches at the bottom
of `abort()`. They are separate patches deliberately: 0012 touches a
macro every mallocng allocation instantiates, so a future audit can
reason about the hot-path change alone. 0013 adds an opt-in
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

**`mmap` is one argument wide.** Only the anonymous subset maps: the
kernel chooses the VA, the region is demand-zero RW (I-12 forbids X at
attach, so PROT bits are upgraded to RW), and an anon region has no
offset — so `start`, `prot`, `fd`, `off` are all ignored and the call
passes only the length. mallocng tolerates this: it asks for `PROT_NONE`
metadata pages then `mprotect`s up, and its `&& errno != ENOSYS` guard
makes the failed mprotect a no-op. `brk` stays a sentinel, so mallocng's
first `brk(0)` fails, it sets `ctx.brk = -1`, and metadata allocation
routes through mmap permanently — a documented mallocng fallback.

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

## Invariants enforced

- **[[inv-i24]] (consumer side).** `_Exit` routes through
  `__NR_exit_group` → `SYS_EXIT_GROUP`, the cascading group terminate —
  which is what makes the 0011/0012 overrides safe in a multi-thread
  Proc. Before #809 they carried an explicit safe-use envelope
  ("reachable only pre-thread-spawn or post-join") because `SYS_EXITS`
  with live peers extincted the kernel; both patch headers still describe
  that envelope and the `SYS_EXIT_GROUP` lift they were waiting for.
- **P-3** — `dup2`-onto-target, `FDOP_CHDIR`, file-backed `mmap`, and
  `MAP_FIXED` all fail loud rather than approximating.

## Error paths

`ENOENT` for every `SYS_SPAWN_FULL_ARGV` failure (name not resolvable /
bad ELF / OOM / a listed fd not a Spoor — collapsed at v1.0); `E2BIG`
for argv overflow; `EMFILE` for fd-list overflow and for pipe failure;
`ECHILD` for every wait failure. `MAP_FAILED` + `ENOSYS` for the refused
mmap forms.

## Performance

A spawn is one syscall plus one parent-side open per `FDOP_OPEN`. The
`/env` populate is one readdir batch plus one open+read per variable, once
per process. Every `malloc` past mallocng's first is userspace-only.

## Prosecution

- The fd model's contiguity check and the `PSPAWN_MAXSLOT` bounds must
  hold for a crafted `file_actions` list (the `opened[]` guard exists
  precisely because open count is not bounded by slot count).
- Both wait translations (flags bit-by-bit, status repack) — passing
  either through raw is silently wrong, not loud.
- `__pouch_pipe`'s hand-rolled `svc` must name a real number; it is
  outside the sentinel guard.
- The two `_Exit(127)` overrides must not regress to `a_crash()`; and any
  new `a_crash()` reachable from a pouch program is a kernel-extinction
  path.
- `__pouch_env_init` must stay fail-soft and stay ordered after
  `__init_libc` and before the ctors.
- The 104-byte spawn-args mirror against the KERNEL's struct — **size AND
  every field offset**. A size-only pin is blind to a field-order swap, and
  the reserved slot is where independent branches collide.
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

- `posix_spawnattr` (SETSID / SETPGROUP / RESETIDS / SETSIG*) is ignored:
  there is no post-spawn child hook, and a fresh child inherits session,
  pgroup, identity, and default note dispositions — which satisfies the
  toolchain's (unset) attr by construction, but would silently ignore a
  program that set one.
- `pipe2(O_NONBLOCK)` is accepted and not honored (no `F_SETFL`);
  `O_CLOEXEC` is a true no-op (a child inherits only listed fds).
- The `/env` populate caps at 512 variables / 256-byte names /
  8 KiB values, and silently skips a variable removed between readdir and
  open.
- `mmap`'s PROT upgrade to RW means no PROT_NONE guard pages anywhere —
  the same root as [[seam-pouch-guard-pages]].

## Provenance

[[chg-2026-05-23-p6-mem-b]] (0003, mallocng over the burrow syscalls;
retargeted to `SYS_BURROW_ATTACH_LAZY` at #321) →
[[chg-2026-05-25-16b-gamma-mount-close]] (0011, the abort override) →
[[chg-2026-05-26-16bg-hardening-3b]] (0012) +
[[chg-2026-05-26-16bg-hardening-3c]] (0013) →
[[chg-2026-07-23-cl1b0-env]] (0025) →
[[chg-2026-07-23-cl1b-process]] (0026).

The 104-byte widening, the twenty offset pins, and the stale-archive
relink are [[chg-2026-08-16-pouch-offset-pins]].
