# 78 — pouch: the POSIX libc — vendoring & the boundary line

> **Status note.** This document is the as-built reference for **pouch**,
> Thylacine's POSIX libc (execution Phase 6). It is written incrementally as
> Phase 6 sub-chunks land. Through sub-chunk 7b it covers the vendoring of musl,
> the boundary-line architecture + inventory, the kernel-side process-startup
> additions (the auxiliary vector, `SYS_SET_TID_ADDRESS`), the syscall seam —
> the syscall-number retarget, the unimplemented-syscall sentinel, the
> Thylacine error-convention decode, and the stdio backend — the first
> pouch binaries running in Thylacine, the `pouch-ld` link-driver wrapper, the
> compiler runtime (the compiler-rt builtins) and the `printf` hello, and the
> anonymous-memory backend that puts `malloc` over `SYS_BURROW_ATTACH`.
> Sections for pouch's
> lower-half API, data structures, and state machines are stubbed with forward
> pointers and filled in by sub-chunks 8-13. The binding design is
> `docs/POUCH-DESIGN.md`.

---

## Purpose

**pouch** is Thylacine's native C library that also presents a POSIX surface.
It is the layer that makes Thylacine *practical* — that lets the large body of
well-written POSIX C software (daemons, libraries, tools) be recompiled and run
on a Plan 9-heritage kernel that knows nothing about POSIX. The proximate
proving binary is real **stratumd**; the durable deliverable is the
cross-compilation path itself.

pouch is **not** a Linux emulation layer and **not** glibc. It is a
Thylacine-native libc that *also* speaks POSIX: a first-class system component,
not a compat ghetto. It contains no Linux syscall numbers and no Linux kernel
ABI assumptions below its own boundary line.

pouch sits in the stack between application C source and the Thylacine kernel:

```
   application C source  (stratumd, libsodium, the pouch test programs)
            │  POSIX / ANSI C
   ┌────────▼────────────────────────────────────┐
   │  pouch  (the POSIX libc)                     │
   │    upper half — vendored musl, portable C    │
   │    ───────── the boundary line ───────────   │
   │    lower half — Thylacine-native runtime     │
   └────────┬────────────────────────────────────┘
            │  Thylacine syscalls (SYS_* — native)
   ┌────────▼────────────────────────────────────┐
   │  the Thylacine kernel  (unchanged in char.)  │
   └─────────────────────────────────────────────┘
```

---

## The boundary line

pouch is structured as two halves divided by one boundary:

- **Upper half** — vendored musl, near-unmodified. Portable C, pure
  computation, no OS knowledge: `printf`, `malloc`'s allocation logic, `qsort`,
  `strtol`, the math library, character classification, regex. This is musl's
  excellent, lean, portable code; pouch keeps it.
- **Lower half** — Thylacine-native. The POSIX runtime layer: it translates
  POSIX file I/O, sockets, `poll`, signals, and threads into Thylacine
  primitives. pouch *replaces* musl's lower half with this.

The boundary line is musl's own **syscall seam**: `src/internal/syscall.h` is
musl's chokepoint, and musl's OS subsystems (sockets, threads, signals) are
isolable modules. musl is already structured around exactly this seam — which
is what makes the patch-series approach (§"The patch series") tractable.

The boundary line is the load-bearing architectural commitment, pinned by four
invariants (POUCH-DESIGN.md §11, cross-referenced from ARCHITECTURE.md §28):

- **P-1** — no foreign syscall numbers in the kernel.
- **P-2** — pouch is the sole POSIX path; the kernel makes zero POSIX
  accommodations.
- **P-3** — no silently-wrong POSIX: every surface either maps to a defined
  Thylacine behavior or returns a documented `errno`.
- **P-4** — the boundary line holds: the upper half carries no
  Thylacine-specific code; the lower half carries all of it; the patch series
  touches only the lower half + the seam.

---

## Vendoring — musl 1.2.5

pouch's upper half is a **pinned, in-tree copy of musl 1.2.5**, vendored at
`third_party/musl/`. pouch is openly a *musl derivative*, not musl.

| Field | Value |
|---|---|
| Version | musl 1.2.5 (released 2024-03-01) |
| Source | `https://musl.libc.org/releases/musl-1.2.5.tar.gz` |
| sha256 | `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4` |
| License | MIT — `third_party/musl/COPYRIGHT` |
| Vendored path | `third_party/musl/` (2697 files, byte-identical to the release) |
| Patch series | `usr/lib/pouch/patches/` |

**Why in-tree copy, not a submodule** (POUCH-DESIGN.md decision 4.2): an
in-tree copy is simpler for a hermetic build and makes the vendored source
reviewable in the same tree as the patch series. The patch series — not a
whole-tree fork — carries the Thylacine delta, so upstream musl's security
fixes remain a re-vendor-and-rebase away.

**Why `third_party/musl/`, not `usr/lib/pouch/musl/`** (POUCH-DESIGN.md §4 left
the exact path open; resolved here, sub-chunk 2): `third_party/` is the
conventional, instantly-recognizable home for pristine vendored upstream code
under its own license, and it keeps a clean boundary — `usr/lib/pouch/` holds
*pouch's own* code (the patch series now; the lower half later), `third_party/`
holds the unmodified upstream input. See `third_party/README.md`.

The vendored tree is **pristine** — never edited in place. Thylacine's changes
apply as the patch series against a build-time working copy; `third_party/musl/`
stays byte-identical to the published release.

---

## The boundary-line inventory

Where does the boundary line fall inside the musl tree? This inventory
classifies every `src/` subdirectory, the C runtime (`crt/`), and the aarch64
arch layer. It is the map the patch series works from — and the discipline
check for invariant P-4: a patch that needs to touch an **UPPER** entry is, by
definition, off the boundary line.

Classes:

- **UPPER** — portable computation, no OS knowledge. Kept near-unmodified. A
  boundary-line patch never touches these.
- **LOWER** — the OS boundary. pouch replaces what these *do* (which syscalls,
  which ABI). The behavior is wrong on Thylacine until the patch series lands.
- **SEAM** — the syscall chokepoint itself: the `syscall()` dispatch + the
  `__NR_*` number table + the CRT/auxv entry plumbing.
- **MIXED** — computation logic is upper; a minority of entries reach the seam.
  The patch touches only the seam-reaching files.

### `src/` — the libc body

| musl path | Class | Role | pouch sub-chunk |
|---|---|---|---|
| `string/` (74) | UPPER | `mem*` / `str*` (aarch64 asm for the hot ones) | — |
| `stdlib/` (22) | UPPER | `atoi` / `strtol` / `qsort` / `bsearch` / `div` | — |
| `math/` (392) | UPPER | the math library | — |
| `complex/` (68) | UPPER | complex-number math | — |
| `ctype/` (37) | UPPER | character classification | — |
| `multibyte/` (20) | UPPER | multibyte / wide-char conversion | — |
| `regex/` (6) | UPPER | POSIX `regcomp` / `regexec` | — |
| `prng/` (11) | UPPER | `rand` / `random` / `drand48` | — |
| `crypt/` (8) | UPPER | `crypt()` password hashing | — |
| `search/` (8) | UPPER | `hsearch` / `tsearch` / `lfind` | — |
| `fenv/` (18) | UPPER | floating-point environment (aarch64 asm) | — |
| `setjmp/` (2) | UPPER | `setjmp` / `longjmp` (aarch64 asm) | — |
| `stdio/` (116) | MIXED | FILE buffering + `printf`/`scanf` upper; the backend ops (`__stdio_read`/`__stdio_write`, `fopen`→`open`) reach the seam | 4 |
| `malloc/` (18) | MIXED | the `mallocng` allocator — logic portable; rests on the anonymous-memory backend | 7b |
| `time/` (39) | MIXED | `gmtime`/`mktime`/`strftime` upper; `clock_gettime`/`nanosleep` lower | 4 |
| `locale/` (26) | MIXED | C locale is upper; locale-file loading is lower (v1.0 = `C` locale only) | 4 |
| `env/` (11) | MIXED | `getenv`/`setenv` upper; `__libc_start_main` / `__init_libc` is the CRT/auxv seam | 3 |
| `exit/` (9) | MIXED | `exit`/`atexit` upper; `_Exit`/`abort` reach the seam | 4 |
| `conf/` (5) | MIXED | `sysconf`/`pathconf` — mostly constants | 4 |
| `passwd/` (20) | MIXED | `getpwnam`/`getgrnam` — parsing upper; reads `/etc/passwd` | 4 |
| `misc/` (39) | MIXED | grab-bag: `getrlimit`, `syslog`, `realpath`, `uname`, `ioctl` | 4+ |
| `legacy/` (16) | MIXED | obsolete APIs — thin | 4+ |
| `unistd/` (81) | LOWER | `read`/`write`/`close`/`lseek`/`dup`/`pipe`/`access` | 4 |
| `fcntl/` (6) | LOWER | `open`/`openat`/`fcntl`/`creat` | 4 |
| `stat/` (20) | LOWER | `stat`/`fstat`/`mkdir`/`chmod`/`statx` | 4 |
| `dirent/` (12) | LOWER | `opendir`/`readdir`/`scandir` (`getdents`) | 4 |
| `mman/` (13) | LOWER | `mmap`/`munmap` → `SYS_BURROW_ATTACH` / `_DETACH`; `mprotect`/`madvise`/`mlock`/`mremap` → `ENOSYS` sentinel | 7b |
| `select/` (4) | LOWER | `select`/`poll`/`ppoll` → `t_poll` / `SYS_POLL` | 10 |
| `thread/` (133) | LOWER | pthreads + atomics + the futex calls → Thylacine threads + `torpor` | 8-9 |
| `signal/` (40) | LOWER | `sigaction`/`kill`/`raise`/`sigprocmask` → notes | 13 |
| `network/` (77) | LOWER | sockets, resolution → `/srv` (`AF_UNIX`); `AF_INET` deferred | 12 |
| `process/` (34) | LOWER | `posix_spawn` → `rfork`; `fork` declined; `exec*`/`wait*` | 12+ |
| `sched/` (10) | LOWER | `sched_yield` / affinity | 4+ |
| `temp/` (7) | LOWER | `mkstemp`/`tmpfile` (uses `open`) | 4 |
| `errno/` (2) | SEAM | `__errno_location` (TLS-backed) | 9 |
| `internal/` (11) | SEAM | `syscall.h`, `libc.h`, `__libc_start_main` plumbing, `version.c` | 3-4 |
| `aio/` (3) | LOWER | POSIX async I/O — **deferred** (`ENOSYS`) | — |
| `ipc/` (13) | LOWER | System V IPC — **deferred** (`ENOSYS`) | — |
| `mq/` (10) | LOWER | POSIX message queues — **deferred** | — |
| `termios/` (12) | LOWER | terminal I/O — **deferred** to Phase 7 (Utopia; PTYs) | — |
| `ldso/` (10) | LOWER | dynamic-linker support — static-only; `dlopen`→`ENOSYS` stubs | 4 |
| `linux/` (67) | LOWER | Linux-specific calls (`epoll`, `inotify`, `sendfile`, `prctl`) — mostly dropped / `ENOSYS` | 4+ |

(Counts in parentheses are `.c` files in the vendored 1.2.5 tree. `src/include/`
holds headers only.)

### `crt/` — C runtime startup

| musl path | Class | Role | pouch sub-chunk |
|---|---|---|---|
| `crt/crt1.c` | LOWER (seam-adjacent) | static-binary startup; calls `__libc_start_main` | 3 |
| `crt/crti.c` / `crtn.c` + `crt/aarch64/crti.s` / `crtn.s` | keep | `.init` / `.fini` section framing (arch asm, thin) | 3 |
| `crt/Scrt1.c` / `crt/rcrt1.c` | unused | PIE / dynamic startup — pouch v1.0 is static-only | — |

### `arch/aarch64/` — the arch layer & the syscall seam

| musl path | Class | Role | pouch sub-chunk |
|---|---|---|---|
| `bits/syscall.h.in` | **SEAM** | the Linux `__NR_*` table — **the** seam file; retargeted to Thylacine `SYS_*` | 4 |
| `syscall_arch.h` | SEAM (mechanism) | the `__syscall0..6` inline asm (`svc 0`). The `svc 0` mechanism is correct for Thylacine; the *numbers* change, the asm stays | 4 |
| `crt_arch.h` | SEAM-adjacent | the `_start` asm (process entry) | 3 |
| `bits/*.h` (`fcntl.h`, `mman.h`, `signal.h`, `stat.h`, ...) | SEAM-adjacent | POSIX ABI constants; some encode Linux flag values; revisited with the seam | 4 |
| `kstat.h` | SEAM-adjacent | kernel `stat` struct ABI | 4 |
| `reloc.h` | LOWER | dynamic-relocation glue (ldso) — static-only | 4 |
| `atomic_arch.h` | UPPER | LSE / LL-SC atomic primitives | — |
| `pthread_arch.h` | UPPER | `TPIDR_EL0` access for TLS (no kernel TLS work — POUCH-DESIGN.md §7) | — |
| `fp_arch.h` | UPPER | FP arch glue | — |
| `bits/alltypes.h.in` | keep | the generated type header (types, not OS behavior) | — |

### Summary

By directory count the bulk of musl is **LOWER** (≈19 dirs) — but by *file*
count and *value* the bulk is **UPPER**: `math/` alone is 392 files, and
`string/` + `stdlib/` + `complex/` + `ctype/` + the rest of the upper half are
musl's lean, battle-tested portable C. That asymmetry is exactly why the
patch-series approach works: pouch keeps the large, valuable, portable upper
half and replaces the localized OS boundary. The patch series touches the
**LOWER** + **SEAM** entries and the seam-reaching files of the **MIXED**
entries — never an **UPPER** entry (invariant P-4).

---

## The patch series

The Thylacine delta against vendored musl is a **patch series concentrated at
the boundary line**, at `usr/lib/pouch/patches/`:

- `series` — the quilt-style ordered list (one patch per line, `#` comments).
- `NNNN-<slug>.patch` — the patches, numbered in apply order.
- `README.md` — the discipline (the P-4 boundary-line rule; the apply model).

`third_party/musl/` stays pristine; `tools/build.sh sysroot` copies it to
`build/pouch/musl-src/` (gitignored), applies the series there with `patch
-p1`, and builds out-of-tree (see "Build" below). At sub-chunk 7b the series
holds three patches — `0001-pouch-syscall-seam.patch`,
`0002-pouch-stdio-no-iovec.patch` (see "The syscall seam"), and
`0003-pouch-mman.patch` (see "The anonymous-memory backend"); the
boundary-line replacement continues across the remaining lower-half sub-chunks.

The series' size is the honest, reviewable measure of pouch's divergence from
musl. An upstream musl security release is handled by re-vendoring
`third_party/musl/` and rebasing the series — never by editing the vendored
tree.

---

## The syscall seam

Sub-chunk 4 (`pouch-syscall-seam`) retargets musl's aarch64 syscall seam from
the Linux kernel ABI to the Thylacine ABI. Two patches carry it:
`0001-pouch-syscall-seam.patch` (the seam proper) and
`0002-pouch-stdio-no-iovec.patch` (the stdio backend). All six touched files
are SEAM or LOWER-half — invariant **P-4** holds, no UPPER-half file is
touched.

### The number table

`arch/aarch64/bits/syscall.h.in` is musl's table of `#define __NR_<name>
<number>` macros (musl's build appends a `SYS_<name>` twin for each). pouch
rewrites every value:

| `__NR_*` / `SYS_*` name | Thylacine syscall | number |
|---|---|---|
| `read` | `SYS_READ` | 9 |
| `write` | `SYS_WRITE` | 10 |
| `close` | `SYS_CLOSE` | 11 |
| `exit`, `exit_group` | `SYS_EXITS` | 0 |
| `mlockall` | `SYS_MLOCKALL` | 16 |
| `getrandom` | `SYS_GETRANDOM` | 20 |
| `set_tid_address` | `SYS_SET_TID_ADDRESS` | 36 |
| *every other macro (299 of them)* | — | `0xFFFF` |

The eight retargeted calls are the ones that map **1:1** onto a native
Thylacine syscall — identical argument shape and semantics, so musl's existing
wrapper issues an `svc 0` the kernel honours directly. (Thylacine deliberately
matched the aarch64 ABI and chose POSIX-shaped semantics for these calls, so
the seam is a number change, not a shim.) `exit` and `exit_group` both fold
onto `SYS_EXITS`: whole-process exit at v1.0 — the per-thread distinction lands
with `pouch-threads`.

Every other POSIX call has no 1:1 Thylacine syscall: either pouch will
implement it in C in its lower half as a *sequence* of Thylacine syscalls
(`open`, `socket`, `poll`, `pthread_create`, ...), or it is genuinely
unsupported. All such macros are retargeted to **`0xFFFF`**, the
unimplemented-syscall sentinel. The table keeps **every** musl `__NR_*` name —
only the values change — so all of musl still compiles end-to-end; a reference
to an un-retargeted call resolves to a deliberate sentinel, never an undefined
macro. The retarget is a deterministic awk filter over the pristine table; the
recipe is recorded in the `0001` patch header, so a musl re-vendor regenerates
it mechanically.

### The sentinel

musl issues syscalls down **two** paths, and pouch guards both:

- The **non-cancellable** path — `arch/aarch64/syscall_arch.h`'s
  `__syscall0..6` inline-asm (loads `x8`, issues `svc 0`). pouch adds, as the
  **first statement** of each of the seven, the guard `if (n ==
  POUCH_SYSCALL_UNIMPL) return -ENOSYS;`.
- The **cancellable** path — taken by cancellation-point calls (`nanosleep`,
  `poll`, `fcntl`, ...) on a thread with cancellation enabled. It runs the C
  function `__syscall_cp` (`src/thread/__syscall_cp.c`) into the hand-written
  asm `__syscall_cp_asm`, which has no guard of its own. `__syscall_cp` is the
  single C chokepoint for every cancellable call, so pouch adds the same
  `if (n == POUCH_SYSCALL_UNIMPL) return -ENOSYS;` there. (This touches a file
  under `src/thread/` — a seam-natured change, not thread semantics; the
  boundary line, P-4, still holds.)

`POUCH_SYSCALL_UNIMPL` is `0xFFFF`. A call with no Thylacine equivalent
short-circuits to `-ENOSYS` **without issuing the trap**, on either path. Two
invariants are made structurally true:

- **P-1** — no foreign syscall number ever reaches the kernel. The only
  numbers that survive the table are Thylacine numbers (0..36) and the
  sentinel, and the sentinel reaches neither `svc` instruction.
- **P-3** — an un-retargeted POSIX call is a clean `ENOSYS`, never silently
  wrong, and never a stray `svc` the kernel would reject with a flat `-1`.

The sentinel literal lives in two patched files — the `0xFFFF` in the table and
`POUCH_SYSCALL_UNIMPL` in `syscall_arch.h`. A `_Static_assert` in
`syscall_ret.c` (`SYS_io_setup == POUCH_SYSCALL_UNIMPL`) pins the two to the
same value at compile time. `syscall_arch.h` also gains `#include <errno.h>` so
`-ENOSYS` resolves without depending on the includer's context.

### The error convention

Thylacine's syscall error convention is a **flat `-1`** — on failure the kernel
returns `-1` in `x0`, with no errno channel. Linux returns `-errno`. musl's
`__syscall_ret` — the chokepoint every public wrapper funnels through — is
retargeted to decode Thylacine's convention:

| raw return `r` | meaning | result |
|---|---|---|
| `r >= 0` | success | `r` |
| `r == -1` | a Thylacine syscall error | `errno = EIO`; return `-1` |
| `r` in `[-4095, -2]` | an explicit `-errno` | `errno = -r`; return `-1` |

Thylacine does not report *which* errno failed a syscall, so a flat `-1` maps
to the generic **`EIO`**. This is honest, not silently-wrong (P-3): `EIO` is a
defined POSIX errno for "an I/O-layer operation failed," and it replaces the
actively-misleading `EPERM` that unmodified musl would synthesise from a `-1`
(`errno = -(-1) = 1`). The `[-4095, -2]` range carries an *explicit* `-errno`:
the sentinel's `-ENOSYS`, and — in later sub-chunks — pouch lower-half C
wrappers that determine a precise errno before returning. The `r == -1` test
precedes the range test because `-1` also satisfies `r > -4096UL`. A richer
kernel error channel is possible future work (POUCH-DESIGN.md §5.1), out of
scope here.

### The stdio backend

musl's `__stdio_write` / `__stdio_read` issue `writev` / `readv` — vectored I/O
Thylacine does not provide. `0002-pouch-stdio-no-iovec.patch` replaces them:

- `__stdio_write` loops `SYS_write` over the two spans musl would have passed
  to `writev` (the stream's pending buffer, then the caller's new data),
  handling short writes. The return + `F_ERR` contract is unchanged: `len` on
  full success, the count of *new* bytes written on error. A `SYS_write`
  return `<= 0` is terminal — `0` means no progress, and pouch treats that as
  an error rather than spinning (musl's `writev` loop could spin).
- `__stdio_read` does a single `SYS_read` into the caller's buffer. musl's
  readahead-into-`f->buf` is dropped — a throughput optimisation, not a
  semantic one; the stream refills on the next read. `F_EOF` / `F_ERR` are set
  exactly as musl's original does (`cnt < 0` → `F_ERR`, `cnt == 0` → `F_EOF`).

## Build — `tools/build.sh sysroot`

`tools/build.sh sysroot` builds the pouch libc and populates the sysroot:

1. Copies pristine `third_party/musl/` to `build/pouch/musl-src/` (gitignored).
   `third_party/musl/` is never edited.
2. Applies the patch series (`usr/lib/pouch/patches/series`) to the copy with
   `patch -p1`. A patch that fails to apply aborts the build loudly.
3. Configures musl out-of-tree in `build/pouch/musl-obj/` for the
   `aarch64-thylacine` target (`--disable-shared`; `CC` = clang
   `--target=aarch64-thylacine`; `AR` / `RANLIB` = `llvm-ar` / `llvm-ranlib`).
4. Builds and installs headers + `libc.a` + the CRT objects
   (`crt1.o` / `crti.o` / `crtn.o`) into `build/sysroot/`. `install-tools` is
   skipped — pouch ships `tools/pouch-clang`, not musl's `musl-gcc`.
5. Verifies the install, and that the seam retarget actually landed
   (`SYS_write` → 10 and `SYS_writev` → the `0xFFFF` sentinel in the generated
   `bits/syscall.h`).

The build is from-scratch each run (~1-2 min): the working copy and object tree
are removed and rebuilt so a stale patch never lingers. Output: a 2.4 MB
`libc.a` (1347 objects, valid `ELF 64-bit LSB relocatable, ARM aarch64`), the
CRT objects, and 218 headers — all under `build/sysroot/` (gitignored). The
**patched** musl still compiles **0 errors / 0 warnings**: the seam patches are
as compile-clean as the upper half.

Sub-chunk 2's **R2 probe** (POUCH-DESIGN.md risk R2 — "musl's lower half may
resist a clean seam") built *pristine* musl as the boundary-line
compile-cleanliness check: 1345 TUs, 0 errors, 0 warnings, a structurally valid
but *semantically Linux* `libc.a`. Sub-chunk 4's build is the real thing — the
series applied, the seam retargeted, the libc semantically Thylacine.

---

## The first pouch binaries

Sub-chunk 5 (`pouch-hello-smoke`) builds and runs the first POSIX C programs on
Thylacine. Two binaries, both cross-compiled against the pouch sysroot and
spawned by joey on the boot path:

- **`/pouch-hello`** — the raw-`write(2)` hello. The first runtime exercise of
  everything sub-chunks 3-4 built: the System V startup frame (auxv) the kernel
  writes in `exec_setup`, musl's static CRT (`_start` → `__libc_start_main` →
  `__init_libc` → `main`), `SYS_SET_TID_ADDRESS` (issued by musl's
  thread-pointer init), the `write(2)` seam (`write` maps 1:1 onto `SYS_WRITE`),
  and the `exit` seam (`exit` folds onto `SYS_EXITS`). It is also the durable
  runtime regression for the `0xFFFF` unimplemented-syscall sentinel: it calls
  `chdir()` (non-cancellable — `SYS_chdir` is the sentinel, caught by the
  `__syscall0..6` guard) and `open()` (a cancellation point — `SYS_openat` is
  the sentinel, caught by the `__syscall_cp` guard, the P6-pouch-syscall-seam
  audit's F1 fix) and asserts each returns `-1` with `errno == ENOSYS`.
- **`/pouch-hello-stdio`** — the buffered-stdio hello. `puts()` + `fwrite()`
  copy bytes into `stdout`'s static `FILE` buffer; the exit-time flush drains
  it through the patched `__stdio_write` backend
  (`0002-pouch-stdio-no-iovec.patch` — a `SYS_write` loop). `stdout` is fully
  buffered: the tty probe (`SYS_ioctl`) hits the sentinel, so musl sees "not a
  tty" and does not line-buffer. This is deliberately *not* `printf(3)` — see
  "The compiler-runtime gap" below.

### Building — `tools/build.sh pouch-progs`

`build_pouch_progs` (in `tools/build.sh`) cross-compiles the two programs
against `build/sysroot/` (built on demand if absent). It is a **two-step**
build, by necessity:

1. **Compile** — `tools/pouch-clang -c` (clang as the compiler). Each `.c`
   becomes an `aarch64` ELF relocatable. `-nostdinc -isystem
   <sysroot>/include` so pouch owns the include path; `-fno-pie` for
   fixed-address codegen.
2. **Link** — `tools/pouch-ld`, the link-driver wrapper, which drives `ld.lld`
   **directly** (not through the clang driver) and supplies the musl CRT
   objects + `libc.a` (+ the compiler runtime, once built).

The link line yields a static, non-PIE `ET_EXEC` whose every `PT_LOAD` segment
has a **page-aligned file offset** — the layout `kernel/elf.c` +
`exec_map_segment` accept with no kernel change. `-z separate-loadable-segments`
is what page-aligns each segment's file offset (`exec_map_segment` rejects a
non-page-aligned `file_offset`); `-z max-page-size=4096` keeps the alignment at
4 KiB. `kernel/elf.c` requires `ET_EXEC` (rejects `ET_DYN`/PIE), no
`PT_INTERP`, no `PT_DYNAMIC` — a static non-PIE link emits none.
`build_pouch_progs` re-checks `ET_EXEC` + no-`PT_DYNAMIC` after each link, so a
loader-incompatible binary fails the build, not the boot.

### Why linking cannot go through the clang driver

`tools/pouch-clang` and `cmake/Toolchain-aarch64-pouch.cmake` were written
(sub-chunk 1) assuming clang would drive the link. On a **macOS host that
assumption is false**: for the unknown OS `thylacine`, clang's driver falls
into the **Darwin toolchain** — it routes the link "via gcc" (Apple's `cc`) and
emits Mach-O linker arguments (`-arch arm64`, `-platform_version`,
`-syslibroot`) that the ELF `ld.lld` rejects. The *compiler* path
(`pouch-clang -c`) is unaffected — clang compiles `aarch64-thylacine` TUs
correctly; only the *link-driver* role is broken. `build_pouch_progs` sidesteps
it by invoking `ld.lld` directly. **`tools/pouch-ld`** (Phase 6 sub-chunk 6a)
is the wrapper that does this: it drives `ld.lld` with the pouch ELF link line
and supplies the CRT + `libc.a` (+ the compiler runtime, once built).
`build_pouch_progs` links through it. The CMake/autotools build chunks
(libsodium, stratumd — sub-chunks 14-16) point their link command
(`CMAKE_C_LINK_EXECUTABLE`, or `LD`) at `pouch-ld` — that wiring lands with the
first CMake-built pouch program, where it can be exercised end-to-end.

### The compiler-runtime gap

The pouch sysroot has **no compiler runtime** yet. A complete C cross-toolchain
is compiler + libc + CRT + *compiler-rt builtins*. POUCH-DESIGN.md §9 now names
all four — it originally enumerated only the first three; the omission was
exactly the `pouch-hello-smoke` finding. Homebrew LLVM ships compiler-rt for the
Darwin host only — there is no `aarch64` ELF `libclang_rt.builtins` archive on
the system, so sub-chunk 6 (`pouch-compiler-rt`) vendors + builds it.

`/pouch-hello` and `/pouch-hello-stdio` link clean because they reference no
compiler-rt builtin (`aarch64` + `-march=...+lse` makes atomics inline; musl is
self-contained for `mem*`/`str*`). **`printf(3)` does not**: musl's `vfprintf`
formats `long double` (aarch64 `binary128`), and `binary128` soft-float —
`__eqtf2`, `__extenddftf2`, `__addtf3`, … — is exactly what compiler-rt
builtins provide. The literal `printf`-shaped hello of POUCH-DESIGN.md §13/§14
is therefore delivered by **sub-chunk 6 (`pouch-compiler-rt`)**: sub-chunk 6b
builds the runtime (see **The compiler runtime** below), and sub-chunk 6c adds
the literal `printf` hello.
`/pouch-hello-stdio` proves the buffered-stdio *path* — identical whether the
bytes arrive via `puts()` or `printf()` — without the format engine.

### joey's smoke orchestration

A pouch binary writes to fd 1 via `SYS_WRITE`; a plain `t_spawn`'d child has no
fds. joey (`usr/joey/joey.c`, `pouch_smoke_one`) makes a pipe and installs its
write-end as the child's fd 0 + fd 1 via `t_spawn_with_fds` — a POSIX fd *is* a
Thylacine handle index (POUCH-DESIGN.md §6.1). joey then **reaps the child
before draining the pipe**: a Thylacine zombie holds its handle table until
`proc_free`, which runs at reap (`t_wait_pid`), so the child's write-end does
not reach EOF until joey reaps it — a drain-until-EOF *before* the reap would
deadlock. The hello output is far under the 4 KiB pipe ring, so the child never
blocks on `write` and exits on its own; joey reaps, then drains the buffered
bytes to the boot-log UART and content-checks them. joey returns non-zero on
any failure — the boot-path regression signal.

---

## The compiler runtime

Sub-chunk 6b (`pouch-compiler-rt`) builds the fourth and final part of the
pouch cross-toolchain: the **compiler runtime**. A complete C toolchain is
compiler + libc + CRT objects + compiler runtime; the pouch sysroot had the
first three (sub-chunks 1-4) and lacked the fourth. `tools/build.sh sysroot`
now installs `build/sysroot/lib/libclang_rt.builtins.a` alongside `libc.a`.

### What a compiler runtime is

The compiler runtime — LLVM names it **compiler-rt**, the analogue of GCC's
`libgcc` — is the set of low-level support routines the compiler emits *calls
to* when the target ISA cannot do an operation inline. clang is free to lower a
C operation into `bl __some_helper` rather than inline instructions; the helper
lives in the runtime. The runtime is not libc — it sits *below* it: it is what
the compiler itself needs to lower C to a given chip. Every C program depends
on it implicitly; a hosted toolchain just auto-links it invisibly.

The component pouch needs is compiler-rt's **builtins** library, and the
routine class that matters here is **soft-float for types the hardware lacks**.
aarch64 has hardware `float` (binary32) and `double` (binary64), but **not**
`long double` — the ARM64 ABI defines `long double` as IEEE **`binary128`**
(quad precision), with no hardware support. Any `long double` arithmetic
therefore compiles to calls: `__addtf3` / `__subtf3` / `__multf3` / `__divtf3`
(the `tf` suffix is the 128-bit float machine mode), `__eqtf2` / `__lttf2` / …
(comparison), `__extenddftf2` (double→binary128), `__trunctfdf2`
(binary128→double), `__fixtfdi` / `__floatditf` (binary128↔integer). musl's
`vfprintf` — the engine behind `printf(3)` — formats floating point in
`long double` for precision, so `vfprintf.o` references these builtins
unconditionally: without the runtime, linking `printf` fails with
`undefined symbol: __addtf3`. That is the gap `pouch-hello-smoke` surfaced (see
"The compiler-runtime gap" above). Homebrew LLVM ships compiler-rt built for
the Darwin **host** only — there is no `aarch64` ELF `libclang_rt.builtins`
archive on the system — so pouch builds its own.

### Vendoring

The compiler-rt builtins source is vendored pristine at
`third_party/compiler-rt/builtins/` (443 files, byte-identical to LLVM 22.1.4 —
the version of the pinned `clang`). Unlike musl, compiler-rt carries **no patch
series**: none of it is OS-boundary code — it is pure computation, the layer
*below* the syscall seam — so there is nothing to retarget. The full vendoring
record (the monorepo-tarball source, sha256, license — LLVM 22 no longer ships
per-component tarballs) is in `third_party/README.md`.

### The build — `build_compiler_rt`

`build_compiler_rt` (in `tools/build.sh`, a step of `build_sysroot`) compiles
the builtins for `aarch64-thylacine` and archives them into
`libclang_rt.builtins.a`.

**Source selection.** The aarch64 source set is *derived* — not transcribed —
from the vendored `builtins/CMakeLists.txt`: the `GENERIC_SOURCES`,
`GENERIC_TF_SOURCES` and `BF16_SOURCES` blocks (every file in them is compiled
for aarch64 by compiler-rt's own build, so each is upstream-guaranteed to
compile cleanly on aarch64), plus the conditional generic additions
(`emutls.c`, `enable_execute_stack.c`, `eprintf.c`, `gcc_personality_v0.c`,
`clear_cache.c`) and the two aarch64-specific files (`cpu_model/aarch64.c`,
`aarch64/fp_mode.c`) — 157 translation units. An `awk` extraction over the
`set(...)` blocks keeps the list re-vendor-safe; a too-short result fails the
build. Four things are deliberately **excluded**:

- **generic `fp_mode.c`** — superseded by `aarch64/fp_mode.c`, the
  FPCR-reading arch override (compiler-rt's own `filter_builtin_sources` drops
  the generic file the same way; compiling both would doubly define
  `__fe_getround` / `__fe_raise_inexact`).
- **`aarch64/lse.S` outline atomics** — `lse.S` is compiled ~90 times by
  compiler-rt's CMake to generate the `__aarch64_{cas,swp,ldadd,…}` outline
  atomic helpers. pouch-clang compiles every TU with `-march=…+lse`, so clang
  emits LSE atomic *instructions inline* and never calls the helpers.
- **`aarch64/emupac.cpp`** — emulated pointer authentication, referenced only
  by code built with `-mbranch-protection`, which pouch's `-march` baseline
  does not enable (also the lone C++ file — omitting it keeps the build C-only).
- **the SME files** (`aarch64/sme-*`) — Scalable Matrix Extension ABI support,
  referenced only by `+sme` code, outside pouch's `-march` baseline;
  compiler-rt itself gates them behind a feature check.

**Compile flags.** `--target=aarch64-thylacine -march=armv8-a+lse+pauth+bti`
(the pouch ISA baseline); `-fno-builtin` (a builtin must never be lowered into
a call to itself); `-O2 -fomit-frame-pointer`; `-fno-stack-protector` (leaf
runtime routines); `-fno-pic` (pouch links static non-PIE);
`-nostdlibinc -isystem build/sysroot/include` — `-nostdlibinc` keeps clang's
resource headers (`stdint.h` / `limits.h` / `stdarg.h` / `unwind.h`, all
compiler-provided) while dropping the host's, and `-isystem` supplies pouch's
own libc headers for the few OS-touching files. The compile runs straight from
`third_party/compiler-rt/builtins/`; objects land in
`build/pouch/compiler-rt-obj/` (gitignored) — `third_party/` stays clean.

**Archive + verification.** `llvm-ar rcs` archives the 157 objects with a
symbol index. `build_compiler_rt` then asserts, with `llvm-nm`, that the
`binary128` soft-float builtins `printf` needs are defined in the archive
(`__addtf3`, `__multf3`, `__eqtf2`, `__extenddftf2`, `__fixtfdi`, …) — the
toolchain-completeness gate: a sysroot with a libc but no runtime links a
static hello but not `printf`. Output: `libclang_rt.builtins.a`, ~227 KiB,
157 ELF `aarch64` relocatable objects.

`tools/pouch-ld` (sub-chunk 6a) picks the archive up automatically: when
`build/sysroot/lib/libclang_rt.builtins.a` is present it links
`--start-group -lc libclang_rt.builtins.a --end-group`, so the mutual
references between musl and the soft-float builtins (musl's `vfprintf` →
`__addtf3`; compiler-rt's `emutls.c` → musl's `malloc`) resolve regardless of
archive order. With the runtime in place, `build_pouch_progs` treats a sysroot
that has `libc.a` but no `libclang_rt.builtins.a` as incomplete and rebuilds it.

### The `printf` hello

Sub-chunk 6c adds `/pouch-hello-printf` (`usr/pouch-hello/pouch-hello-printf.c`)
— the third pouch binary, and the first to exercise the compiler runtime.
Where `/pouch-hello` uses the raw `write(2)` seam and `/pouch-hello-stdio` uses
buffered stdio via `puts()`/`fwrite()`, this one uses `printf(3)` — and
`printf` pulls musl's `vfprintf`, the first thing in the project to reference
a compiler-rt builtin.

It is a self-test. It `snprintf()`s an integer and a floating-point value and
`strcmp()`s the result against the known-correct string:

- the **`%d` check** proves the *link* — any `printf` pulls `vfprintf.o`, which
  references the `binary128` soft-float builtins unconditionally, so the binary
  cannot link without `libclang_rt.builtins.a`.
- the **`%.2f` check** proves the runtime *runs* — `vfprintf`'s `fmt_fp`
  extracts decimal digits in `binary128` `long double`, so a broken
  `__subtf3` / `__multf3` / `__fixtfdi` would format the wrong digits.
  `snprintf("%.2f", 3.140625)` must yield exactly `"3.14"` (3.140625 is exact
  in binary — no rounding ambiguity).

`main` returns non-zero on any mismatch; joey's `pouch_smoke_one`
content-checks the output and the exit status, so a regressed compiler runtime
fails the boot. The binary is ~49 KiB — larger than the `write(2)` / stdio
helloes because it pulls `vfprintf.o` and the `tf`-mode builtins. It links
static non-PIE `ET_EXEC` like the others; the kernel ELF loader needs no
change.

With `/pouch-hello-printf` running — printing via `printf(3)`, its `binary128`
soft-float verified correct at runtime — the pouch cross-toolchain is **proven**
end to end: a C program using the full compiler + libc + CRT + compiler runtime
compiles, links, loads, and runs on Thylacine.

---

## The anonymous-memory backend

Sub-chunk 7b (`pouch-mem`) puts `malloc` over Thylacine's anonymous-memory
syscalls. After 7a landed the kernel-side primitive (`SYS_BURROW_ATTACH` /
`SYS_BURROW_DETACH`; docs/reference/79-sys-burrow.md), 7b is the **pouch-side**
boundary-line patch that routes musl's `mman/` lower half onto it. The patch is
`0003-pouch-mman.patch`; with it applied the static-`libc.a` mallocng
allocator works.

### The patch — `0003-pouch-mman`

Three files, one purpose:

- `arch/aarch64/bits/syscall.h.in` — two macros that `0001-syscall-seam`
  set to the `0xFFFF` sentinel get their Thylacine numbers:
  `__NR_mmap = 37` (= `SYS_BURROW_ATTACH`) and `__NR_munmap = 38`
  (= `SYS_BURROW_DETACH`). Future re-vendor awk-filter runs extend
  `0001`'s `m[]` table with `m["mmap"]="37"; m["munmap"]="38";` (the
  preamble in `0003` records this for the next re-vendor).
- `src/mman/mmap.c` — rewritten. The Linux `mmap(start, len, prot, flags,
  fd, off)` becomes a one-argument call to `SYS_mmap` (= `SYS_BURROW_ATTACH`):
  the length. `start`, `prot`, `fd`, `off` are accepted for source
  compatibility and ignored; the kernel chooses the VA, and the region is
  always demand-zero RW (W^X / I-12 forbids X at attach; PROT bits are
  silently upgraded). File-backed `mmap` (any flags without `MAP_ANON`) is
  refused with `ENOSYS` — a permanent Thylacine refusal, not a v1.0 stub
  (ARCHITECTURE.md §6.5; POUCH-DESIGN.md §8.2). `MAP_FIXED` is rejected too
  (the v1.0 burrow-attach window is kernel-chosen).
- `src/mman/munmap.c` — rewritten. The Linux `munmap(start, len)` becomes
  a direct call to `SYS_munmap` (= `SYS_BURROW_DETACH`). The kernel matches
  the page-rounded `[start, start + len)` range exactly against an
  installed VMA; mallocng tracks `needed` losslessly, so the round-trip
  works.

All other `mman/` files (`madvise`, `mprotect`, `mremap`, `mlock`, `mlockall`,
`munlock`, `munlockall`, `msync`, `mincore`, `posix_madvise`, `shm_open`) stay
on the `0xFFFF` sentinel path — invoking them returns `-1` + `errno = ENOSYS`,
which is the contract this layer wants. None of them is on mallocng's
correctness path (see "mallocng correctness" below); pouch-side callers see a
clean ENOSYS.

### mallocng correctness

mallocng (musl's allocator at `src/malloc/mallocng/`) is the consumer this
patch serves. Verifying it works under the Thylacine reshape required walking
every memory-management call:

- **`brk`** — `glue.h` has `brk(p) ((uintptr_t)__syscall(SYS_brk, p))`. `SYS_brk`
  stays `0xFFFF`; on the first allocation mallocng tries `brk(0)` (gets
  `(uintptr_t)(-ENOSYS)`, a huge value), then `brk(new)` fails, and mallocng
  sets `ctx.brk = -1` (line `malloc.c:70`) — permanently routing every
  metadata-area allocation through `mmap`. This is mallocng's *documented*
  brk-fallback path; correctness is unchanged.
- **`madvise`** — gated by `USE_MADV_FREE` (= 0 in `glue.h`). Never invoked.
- **`mremap`** — only called from `realloc.c:34` on a `>= MMAP_THRESHOLD`
  grow. The sentinel guard makes `mremap` return `MAP_FAILED` + `errno = ENOSYS`;
  `realloc` falls through to `malloc + memcpy + free` at line 46.
  Correctness unchanged.
- **`mprotect`** — called from `malloc.c:92` after a successful `brk`
  extension. Since `brk` is permanently unusable, this path is never
  reached; even if reached, `mprotect`'s `errno != ENOSYS` guard tolerates
  the sentinel failure.

`MMAP_THRESHOLD = 131052` (mallocng's `meta.h`). Allocations smaller than this
land in mallocng's size-classed slot allocator (one `mmap` per *group*, many
allocations per group); larger allocations take an *individually-mmapped* path
— one `SYS_BURROW_ATTACH` per allocation, freed by one `SYS_BURROW_DETACH`.

### Footguns silently swallowed

The `prot` argument to `mmap` is ignored — pouch always hands back RW memory.
That's a v1.0 security degradation (no `PROT_NONE` guard pages between
mallocng's meta areas), accepted because mallocng's allocator correctness
**does not depend on guard inaccessibility** — they are defense-in-depth
against overruns, not a load-bearing invariant. v1.x can add `mprotect`
when guard pages are wanted; with `mprotect` working, mallocng's existing
code installs them as-is.

### Verifying live — `/pouch-hello-malloc`

Sub-chunk 7b lands `/pouch-hello-malloc` (`usr/pouch-hello/pouch-hello-malloc.c`)
— the fourth pouch binary, exercising mallocng end-to-end:

| Step | What it proves |
|---|---|
| small `malloc`/`free` | the slot path; the first call triggers mallocng's first metadata-area mmap (the `brk`-fallback). |
| `calloc` (zero-initialized) | the demand-zero contract — `SYS_BURROW_ATTACH` returns pages zeroed by `burrow_create_anon`. |
| `realloc`-grow within a slot | size class transitions inside mallocng (no underlying remap). |
| large `malloc`/`free` (> `MMAP_THRESHOLD`) | mallocng's individually-mmapped path — one `SYS_BURROW_ATTACH` per allocation, one `SYS_BURROW_DETACH` per free. |
| large `realloc`-grow | the `mremap`-ENOSYS path → `malloc + memcpy + free` fallback in `realloc.c:46`. |

Every region is filled with a seeded byte sequence and read back; any byte-
level mismatch returns non-zero from `main`. joey's `pouch_smoke_one` content-
checks `pouch-hello-malloc: exit 0` and asserts the child reaped with status
0, so a regressed allocator or a regressed user-fault path fails the boot.

The binary is ~60 KiB — slightly larger than `/pouch-hello-printf` because it
pulls `realloc.c` + `calloc.c` and the rest of mallocng on top of the
`printf` machinery it shares with the prior hello.

---

## The polling backend

Sub-chunk 10 (`pouch-poll`) is the userspace seam mating onto the audited
kernel `SYS_POLL` (= 29) primitive — the multi-fd wait/wake machine described
in `docs/reference/72-poll.md` (P5-poll-a / P5-poll-b). It is small, NOT
audit-bearing — `SYS_POLL`'s correctness was settled in P5; sub-chunk 10
adds only the boundary-line userspace translation that lets musl-shaped
callers (`poll`, `ppoll`, `select`, `pselect`) reach it.

### The patch — `0005-pouch-poll`

Five files in musl's source touch this seam; four are rewritten, one is
left pristine.

| File | Change | Why |
|---|---|---|
| `arch/aarch64/bits/syscall.h.in` | `#define __NR_poll 29` added | Linux aarch64 dropped legacy `poll(2)` in favor of `ppoll(2)`; musl's table reflects that. Adding the entry reinstates the `SYS_poll` alias the sed pass generates, and lets pristine `src/select/poll.c` route through `SYS_POLL` without the ppoll-conversion fallback. |
| `src/select/poll.c` | **unchanged** | musl's source already does `#ifdef SYS_poll → return syscall_cp(SYS_poll, fds, n, timeout)`. Once `SYS_poll = 29` is defined, the conditional fires and the call goes straight to the kernel. |
| `src/select/ppoll.c` | full rewrite | Folds `timespec → int timeout_ms` (rounded UP per POSIX permitting only early wake); IGNORES `sigset_t` (signals deferred to sub-chunk 13); drops the `SYS_ppoll_time64` cascade and the IS32BIT/CLAMP machinery (no Y2038 fallback to manage). |
| `src/select/select.c` | full rewrite | The `fd_set ↔ pollfd[]` translation. Scans `[0, min(n, 64))`, builds a stack-allocated `pollfd[64]`, calls `SYS_POLL`, clears + re-fills the output sets. fds `≥ 64` get `-EBADF` (unreachable handles); `select(0, NULL, NULL, NULL, {0,0})` returns 0 immediately; `select(0, ..., +tv)` is `-ENOSYS` at v1.0 (no sleep syscall yet). |
| `src/select/pselect.c` | full rewrite | Delegates to `select()` after `timespec → timeval` (ns rounded up to µs); ignores `sigset_t`. |

The kernel's `pollfd` ABI (`kernel/include/thylacine/poll.h`) is byte-identical
to musl's `<poll.h>` (s32 fd / s16 events / s16 revents; POLLIN=0x001,
POLLOUT=0x004, POLLERR=0x008, POLLHUP=0x010, POLLNVAL=0x020) — the wire
crossing is a zero-copy header reinterpret.

### Translation contract — `select()`

The fd_set → pollfd contract follows Linux semantics:

| Input | `events` bit |
|---|---|
| fd ∈ `rfds` | `POLLIN` |
| fd ∈ `wfds` | `POLLOUT` |
| fd ∈ `efds` | `POLLPRI` |

On return, the input sets are CLEARED in place and re-set per the
events-mask gate:

| `revents` from kernel | Goes into | When |
|---|---|---|
| `POLLIN`  / `POLLERR` / `POLLHUP` | `rfds` | the fd contributed `POLLIN` |
| `POLLOUT` / `POLLERR` / `POLLHUP` | `wfds` | the fd contributed `POLLOUT` |
| `POLLPRI`                          | `efds` | the fd contributed `POLLPRI` |

`POLLERR` / `POLLHUP` are output-only — the kernel reports them on a
contributing fd irrespective of the events bitmask, mirroring Linux's
"hangup on a readable fd shows in rfds" behavior.

### Caveats — what v1.0 doesn't do

| Limitation | Why | Future |
|---|---|---|
| `select(0, NULL, NULL, NULL, +tv)` returns `-ENOSYS` | Thylacine has no sleep syscall; the kernel's `SYS_POLL` rejects `nfds=0`. The zero-tv case still returns 0 immediately (POSIX-correct). | A future `SYS_NANOSLEEP` would re-enable; see POUCH-DESIGN §8. |
| `ppoll` / `pselect`'s `sigset_t` is IGNORED | Signals not delivered atomically with poll at v1.0; the race window the variants exist to close has no analog in the v1.0 signal model. | Sub-chunk 13 (`pouch-signals`) — the kernel-side signal model is a prerequisite. |
| `select` rejects fds ≥ 64 with `-EBADF` | The kernel's `SYS_POLL` ceiling is `PROC_HANDLE_MAX = 64`. fds above that index are unreachable through any Thylacine syscall — the handle table can't hold them. | Lift only if `PROC_HANDLE_MAX` grows. |

### Verifying live — `/pouch-hello-poll`

Sub-chunk 10 lands `/pouch-hello-poll` (`usr/pouch-hello/pouch-hello-poll.c`)
— the fifth pouch binary, exercising poll + select end-to-end:

| Step | What it proves |
|---|---|
| `SYS_PIPE` via inline asm | The kernel-side pipe pair is reachable; musl's `pipe(2)` calls `SYS_pipe2` (sentinel-blocked), so the test uses inline asm to capture the two-register `rd/wr` return. |
| `poll(empty, timeout=50ms)` → 0 | The full slow-path: tsleep on the poller's private rendez, register the `poll_waiter` on the pipe's hook list, wait for the timeout, unregister, return 0. |
| `write(byte)` then `poll(timeout=0)` → 1 / POLLIN | The fast path: first-scan readiness sample picks up the pipe-with-byte state without tsleep. |
| `read(byte)`, `write(byte)`, `select(rfds, {0,0})` → 1 / fd in rfds | The fd_set ↔ pollfd translation: build a 1-entry pollfd, call `SYS_POLL`, decode the revents back into `rfds`. |
| `select(0, NULL, NULL, NULL, {0,0})` → 0 | The zero-fds zero-tv short-circuit arm. |

joey's `pouch_smoke_one` content-checks `pouch-hello-poll: exit 0` and
asserts the child reaped with status 0, so a regressed poll wake/sleep
path, a regressed fd_set translation, or a regressed `__NR_poll` route
all fail the boot.

The binary is ~50 KiB — between `/pouch-hello-printf` and
`/pouch-hello-malloc`. It pulls in `select.c` + `pselect.c` + `printf` +
the seam wrappers but no mallocng (no dynamic allocation; all pollfd
state is stack-resident).

---

## The AF_UNIX SOCK_STREAM backend

Sub-chunk 12 (`pouch-sockets`) maps musl's `AF_UNIX` `SOCK_STREAM`
sockets onto Thylacine's `/srv` registry — the highest-leverage seam
in Phase 6 because stratumd (sub-chunk 16) needs it. The chunk has
both a userspace dimension (the boundary-line patch `0006-pouch-
sockets`) and a kernel dimension (a new transport mode on the
SrvConn).

### The byte-mode SrvConn — kernel-side

The kernel `/srv` mechanism shipped at P5-corvus-srv as a **9P
channel**. The client side (KOBJ_SRV) drove `p9_client_read/write`,
which wrapped user data in Tread/Twrite frames; the server endpoint
(KOBJ_SPOOR from `accept`) drained raw 9P T-frame bytes from c2s.
That asymmetry is exactly what a 9P file server like corvus or
stratumd wants — a `read()` on the server endpoint hands the
userspace parser a T-frame to dispatch.

But it is **wrong** for raw AF_UNIX SOCK_STREAM. A POSIX userspace
server reading `read(conn, buf, 5)` to receive `"PING\n"` would
instead see ~13 bytes of Twrite frame header + payload. The pouch
sockets layer cannot map onto this 9P-shaped channel without a
userspace 9P responder.

Sub-chunk 12 adds a second transport mode — **byte mode** — to the
SrvConn:

| | 9P mode (existing) | Byte mode (new) |
|---|---|---|
| Posted via | `SYS_post_service` (= 26) | `SYS_post_service_byte` (= 43) |
| `enum srv_mode` | `SRV_MODE_9P` | `SRV_MODE_BYTE` |
| `SYS_srv_connect` Phase 2 | Drives `p9_client_handshake` (Tversion + Tattach + Tlopen) | **Skipped** — `cn->byte_mode` is true, no handshake |
| Non-empty `path` | Walked via Twalk after Tattach | Refused with `-1` (no 9P fid to walk) |
| Client `read/write` (KObj_SRV) | `srvconn_client_read/write` → 9P Tread/Twrite frames | `srvconn_client_send/recv` → raw `chan_produce/consume` on c2s/s2c |
| Server endpoint `read/write` | Raw bytes — userspace is the 9P responder | Raw bytes — userspace sees the *user data*, no framing |
| Consumer | corvus + future stratumd | pouch `AF_UNIX SOCK_STREAM` |

The mode is captured by value on the SrvConn at mint
(`srv_conn_open_for_proc` reads the service's mode under the registry
lock alongside `poster_stripes`), so a tombstone-then-rebind of the
service does not change the mode of an already-minted connection.

Files (kernel):
- `kernel/include/thylacine/devsrv.h` — `enum srv_mode` (`SRV_MODE_9P` = 0, `SRV_MODE_BYTE` = 1); `SrvService.mode` field; `srv_reserve` takes `mode`.
- `kernel/include/thylacine/srvconn.h` — `SrvConn.byte_mode` bool; `srvconn_set_byte_mode(cn)` setter.
- `kernel/include/thylacine/syscall.h` — `SYS_POST_SERVICE_BYTE` = 43.
- `kernel/devsrv.c::srv_conn_open_for_proc` — captures `service_mode` under registry lock; calls `srvconn_set_byte_mode(cn)` BEFORE the SrvConn is enqueued in the accept backlog.
- `kernel/srvconn.c::srvconn_set_byte_mode` — one-way setter; pre-publication, no lock needed.
- `kernel/syscall.c::sys_post_service_core` — shared body parameterized by mode; `sys_post_service_for_proc` / `sys_post_service_byte_for_proc` are 3-arg public wrappers (the existing 3-arg signature is preserved so kernel tests + corvus's wrapper continue to link).
- `kernel/syscall.c::sys_srv_connect_for_proc` — if `cn->byte_mode`, skips handshake; refuses non-empty path.
- `kernel/syscall.c::sys_read/write_for_proc` (KOBJ_SRV arm) — dispatches on `cn->byte_mode`.

### Why a new syscall, not an `int mode` arg

`SYS_post_service` shipped with a 2-arg signature: the dispatcher reads
`x0`/`x1` only. Adding `x2` for a mode would interpret the calling
convention's leftover value in `x2` as garbage mode — corvus's existing
`t_post_service` wrapper passes nothing in `x2`, so the kernel would
read an arbitrary value. Adding a new syscall number is the safer
extension: legacy callers stay on `SYS_post_service` (= 26, mode 9P);
new callers (pouch's `bind`) call `SYS_post_service_byte` (= 43, mode
BYTE). The two share the post-gate / name-validation / reserve-commit
core (`sys_post_service_core`).

### The pouch userspace shadow

pouch maintains a per-process socket slot table (`POUCH_SOCK_MAX = 8`).
`socket()` allocates a slot and returns a fd tagged with bit 30
(`POUCH_SOCK_TAG = 0x40000000`) — above the kernel's `PROC_HANDLE_MAX`
so no real kernel fd ever overlaps. The slot tracks
(`state`, `kernel_fd`):

| Slot state | kernel_fd | Set by |
|---|---|---|
| `POUCH_SOCK_FRESH` | -1 | `socket()` |
| `POUCH_SOCK_LISTENING` | KObj_Srv listener handle | `bind()` calling `SYS_post_service_byte` |
| `POUCH_SOCK_CONNECTED` | KObj_Srv client SrvConn handle | `connect()` calling `SYS_srv_connect` with `path_len=0` |

POSIX calls dispatch through the slot:
- `bind(tagged_fd, ...)` extracts `<name>` from `sun_path` (accepts `/srv/<name>` or bare `<name>`) and calls `SYS_post_service_byte`. The kernel post-gate (`PROC_FLAG_MAY_POST_SERVICE`) must be stamped; joey grants it at spawn via `t_spawn_with_perms(... T_SPAWN_PERM_MAY_POST_SERVICE)`.
- `listen(tagged_fd, backlog)` is a no-op pouch-side validation. `backlog` is ignored.
- `accept(tagged_fd, ...)` calls `SYS_srv_accept` and returns the **raw kernel Spoor fd** (NOT pouch-tagged). Subsequent `read/write/close` on it bypass the pouch dispatch shim.
- `connect(tagged_fd, ...)` extracts the name and calls `SYS_srv_connect(name, name_len, NULL, 0)`. `path_len=0` is the kernel's "no Twalk" case; in byte mode the kernel ALSO skips the 9P handshake. Per-Proc cap = 1 outstanding client connection.
- `getsockopt(fd, SOL_SOCKET, SO_PEERCRED, ...)` calls `SYS_srv_peer` and marshals `srv_peer_info` (stripes/caps/console/alive) into `struct ucred` (pid = peer stripes truncated to pid_t; uid = gid = 0 at v1.0 — Thylacine has no uid model). Other levels/options return `ENOPROTOOPT`.

The dispatch shims in `src/unistd/{read,write,close}.c` test `(fd & POUCH_SOCK_TAG)`; tagged → route through `pouch_sock_kernel_fd(fd)`; untagged → pass straight to `SYS_read/write/close`. Cost: one bit-test on the common path.

### `pouch_sock_kernel_fd` returns

| Input fd | Return |
|---|---|
| Untagged (any value without bit 30) | `fd` unchanged — pass-through to kernel |
| Tagged + slot vacant | `-1` with `errno = EBADF` |
| Tagged + slot `POUCH_SOCK_FRESH` (post-`socket()`, pre-bind/connect) | `-1` with `errno = ENOTCONN` |
| Tagged + slot `POUCH_SOCK_LISTENING` or `POUCH_SOCK_CONNECTED` | the slot's kernel handle |

### Refusals at the libc layer

Explicit POSIX errnos rather than ENOSYS at the 0xFFFF sentinel:

| Call shape | errno | Why |
|---|---|---|
| `socket(AF_INET, ...)` | `EAFNOSUPPORT` | Network phase, not v1.0 |
| `socket(AF_UNIX, SOCK_DGRAM, 0)` | `EPROTONOSUPPORT` | Datagram sockets deferred |
| `socket(AF_UNIX, SOCK_STREAM, 42)` | `EPROTONOSUPPORT` | Non-zero protocol unsupported |
| `bind(fd, "/usr/foo", ...)` | `EINVAL` | Paths outside `/srv/` aren't AF_UNIX-backed at v1.0 |
| `bind(fd, ...)` without `PROC_FLAG_MAY_POST_SERVICE` | `EACCES` | Kernel post-gate (corvus.tla `PostService` precondition) |
| `connect(fd, "/srv/unposted", ...)` | `ECONNREFUSED` | Service not posted (or unknown name) |
| `socketpair(...)`, `shutdown(...)`, `recv*/send*` other than passthrough, `getsockname/getpeername` | `EIO` (from the 0xFFFF sentinel) at v1.0 — deferred |

### What v1.0 doesn't do

- **`SO_PEERCRED` on a CLIENT-side fd** — the kernel `SYS_SRV_PEER` gate is "caller stripes == poster stripes," which serves a SERVER reading peer identity. A client querying its server's identity would need a kernel extension. v1.0 surfaces `ENOTSOCK` on the client side; the server side works.
- **Poll on a CONNECTED pouch socket fd** — `poll()` doesn't yet know how to translate tagged fds. Blocking I/O works (the proving binary uses it); nonblocking + multiplexing on AF_UNIX sockets is deferred.
- **`AF_UNIX` paths outside `/srv/`** — the registry has one namespace; arbitrary filesystem paths can't back a socket at this phase (POUCH-DESIGN.md OPEN Q 6.2).
- **`SOCK_DGRAM` / `SOCK_SEQPACKET`** — only `SOCK_STREAM`.
- **`socketpair(2)`** — deferred to v1.x.

### Verifying live — `/pouch-hello-sockets`

The eighth pouch binary. One Proc, two pthreads:
- **Server thread**: `socket → bind("/srv/pouch-sock-demo") → listen → accept → read PING → write PONG → getsockopt(SO_PEERCRED) → close`.
- **Main thread** (client): `pthread_barrier_wait → socket → connect → write PING → read PONG → getsockopt(SO_PEERCRED) (expects ENOTSOCK on the client-side fd at v1.0) → close → pthread_join`.

The byte-accurate read counts (5 bytes for `"PING\n"` and `"PONG\n"`)
are the proving claim: byte mode delivers the user data with no 9P
framing visible to userspace. The client conn fd `0x40000001` confirms
the tagged-fd encoding.

---

## The `/dev/full` Dev + the `getrandom` proving path

Sub-chunk 11 (`pouch-devnodes`) lands the third member of the trivial
`/dev` Dev trio — `devfull` (dc='f', name="full") — and proves the
`getrandom` libc surface from a real pouch program. There is no pouch
boundary-line patch in this chunk: `__NR_getrandom` was already mapped to
`SYS_GETRANDOM` (= 20) by `0001-pouch-syscall-seam.patch`, and musl's
`src/linux/getrandom.c` (`syscall_cp(SYS_getrandom, ...)`) is pristine.
The chunk's two deliverables are independent — a kernel Dev that completes
POUCH-DESIGN.md §6.6's "trivial nodes" list, and a proving binary that
asserts the libsodium-grade CSPRNG path the rest of Phase 6 will lean on.

### `devfull` — the "full disk" Dev

`kernel/full.c` mirrors `kernel/null.c` and `kernel/zero.c`: a single-file
leaf Dev (`dc='f'`, `name="full"`, `qid.type=QTFILE`) registered in
`dev_init()` between `devzero` and `devrandom` for alphabetical iteration.
The semantic is Linux's `man 4 full` (the reference POSIX
implementation):

| Op | Behavior |
|---|---|
| `attach(spec)` | Returns a fresh `Spoor` of `qid.type=QTFILE`. Stateless. |
| `open(omode)` | Standard `dev_simple_open` (sets `COPEN`, records `omode`). |
| `read(buf, n, off)` | NUL-fills `buf[0..n)` and returns `n` — same shape as `/dev/zero`. The "full" abstraction is asymmetric: full on write, transparent on read. |
| `write(buf, n, off)` | Always returns `-1`. Every write fails — the "full disk" contract. |
| `walk` / `stat` / `wstat` / `bread` / `bwrite` / `remove` / `power` | Leaf-Dev no-ops (return `NULL` / `-1`). |

**The v1.0 errno caveat**: `devfull_write` returns flat `-1`, so the
SYS_WRITE handler (`sys_write_for_proc`) collapses it to a flat `-1`
syscall return, and pouch's `syscall_ret` decodes that to `errno=EIO` (the
generic-EIO mapping). Linux returns `errno=ENOSPC` specifically — to
match that, the SYS_WRITE handler would need to widen its error channel
to pass dev-supplied `-errno` through (a one-line `return n;` if `n < 0`,
followed by per-Dev audit of negative-return discipline). That's a
documented future improvement; the proving binary's contract here is "the
write returns < 0," which is POSIX-correct under both errno values.

### Kernel tests

`kernel/test/test_trivial_devs.c` gains three devfull tests + the
bestiary-smoke assertion is extended:

| Test | What it proves |
|---|---|
| `full.attach_open_close` | Lifecycle plumbing — Spoor alloc, `dc='f'`, `qid.type=QTFILE`, `COPEN` toggle on open/close. Mirrors the existing `null.attach_open_close`. |
| `full.read_fills_zeroes` | Reads NUL-fill the buffer; `n=0` is a no-op; `NULL buf` and negative `n` are rejected (return `-1`). |
| `full.write_returns_minus1` | Every write fails: non-empty, single-byte, `n=0`, and NULL-buf all return `-1`. |
| `trivial_devs.bestiary_smoke` (extended) | `dev_count() >= 6`; `dev_lookup_by_dc('f') == &devfull`; `dev_lookup_by_name("full") == &devfull`. |

Test count: 561 → **564** (+3 devfull tests). No new regression cases at
this chunk — devfull is a leaf Dev with no audit-bearing concurrency
surface; the contract is exercised entirely through the read/write
return shape.

### Path-based access — deferred

Sub-chunk 11 does NOT make `open("/dev/null")` work from a pouch program.
That requires either (a) a multi-component `SYS_OPEN_PATH` syscall that
walks `/dev` then `null`, or (b) a libc-side path decomposer over
`SYS_WALK_OPEN`'s single-component contract — both larger than this
sub-chunk's scope. The trivial Devs are reachable today only through
`dev_lookup_by_dc/name` (kernel-internal) and `dev->attach("")`
(kernel-internal). Pouch programs that want CSPRNG bytes go through
`getrandom(2)` (the syscall surface — proven below), not `/dev/random`
(the path surface — deferred).

### `/pouch-hello-getrandom` — verifying the libsodium CSPRNG path

`usr/pouch-hello/pouch-hello-getrandom.c` is the seventh pouch binary
(~50 KiB, between the printf and the threads bodies). It calls musl's
`getrandom(2)` at the libc surface and verifies the round-trip:

| Probe | Expectation |
|---|---|
| `getrandom(buf, 32, 0)` | Returns 32 (no flags, blocking-OK). |
| Buffer contains a non-zero byte | RNDR delivered entropy (`P(all 32 zero) = 2^-256`). |
| Two consecutive 16-byte reads differ | Basic CSPRNG sanity (`P(equal) = 2^-128`). |

Without `CAP_CSPRNG_READ` the kernel SYS_GETRANDOM handler returns
-1/EIO. Joey grants the cap explicitly by spawning the binary through a
new `pouch_smoke_one_caps` helper that uses `t_spawn_full` with
`cap_mask = T_CAP_CSPRNG_READ` (joey holds `CAP_ALL` post-elevation, so
the intersection is `CAP_CSPRNG_READ`). The wiring proves both that
musl's `getrandom(2)` reaches the kernel through the seam AND that the
cap-grant path through `t_spawn_full` works — both consumed by sub-chunk
14's libsodium build.

---

## Public API

Not yet — pouch exposes no API surface at sub-chunk 2. pouch's lower-half API
(the POSIX runtime layer) lands across sub-chunks 3-13; this section is filled
in as each lands. The headers pouch will install into the sysroot are musl's
(`include/`), retargeted at the seam. See `docs/POUCH-DESIGN.md §6, §8` for the
POSIX surface pouch commits to and the documented-error surface it does not.

## Data structures

Not yet — see `docs/POUCH-DESIGN.md §7` for the thread model (the first
pouch-native data structures land with `pouch-threads`, sub-chunk 9).

## State machines

Not yet. The `torpor` wait-on-address state machine lands with sub-chunk 8
(`pouch-wait-addr`), spec-first against `specs/futex.tla`.

## Spec cross-reference

pouch's invariant-bearing kernel additions are spec-pinned:

| Sub-chunk | Spec | Invariant |
|---|---|---|
| 8 `pouch-wait-addr` (`torpor`) | `specs/futex.tla` (#7) | wait-on-address atomicity (I-9) |
| 13 `pouch-signals` | `specs/notes.tla` (#8) | note delivery ordering (I-19) |
| 10 `pouch-poll` | `specs/poll.tla` (#6) | missed-wakeup-freedom (I-9) |

Sub-chunk 2 introduces no spec obligation (vendoring + scaffolding).

## Tests

Sub-chunks 2 and 4 add no kernel-test-suite cases (vendoring, scaffolding, and
the syscall-seam retarget change no kernel code; the kernel suite stays
**538/538**). Sub-chunk 5 (`pouch-hello-smoke`) adds none either — it touches
no kernel code — but it lands the seam's first *runtime* regression: joey
spawns `/pouch-hello` + `/pouch-hello-stdio` on every boot, relays +
content-checks their output, and asserts each exits 0 (joey returns non-zero,
a boot-path regression signal, on any mismatch). `/pouch-hello`'s two sentinel
assertions (`chdir` non-cancellable, `open` cancellable) are the durable
runtime check that an unimplemented syscall yields `ENOSYS` on both musl
syscall paths — until sub-chunk 5 the `build_sysroot` structural greps were
the only regression. Sub-chunk 7a added 11 kernel tests for the
`SYS_BURROW_ATTACH` / `_DETACH` surface and `vma_find_gap` (the kernel suite
moved from 538/538 to **549/549**). Sub-chunk 7b adds no further kernel
tests — it touches no kernel code — but it adds a fourth *runtime* regression:
joey runs `/pouch-hello-malloc`, which exercises mallocng's small-slot path,
`calloc` zero-init, in-slot realloc-grow, the `>= MMAP_THRESHOLD`
individually-mmapped path, and the `mremap`-ENOSYS realloc-grow fallback,
verifying byte-level round-trip on every region. The Phase 6 proving set —
static hello, the multithreaded test, the `AF_UNIX` echo pair, libsodium's
self-test, stratumd's boot — lands across sub-chunks 5, 9, 12, 14, 16. See
`docs/POUCH-DESIGN.md §13` for the exit-criteria checklist.

## Error paths

pouch translates Thylacine's flat-`-1` syscall error convention into the POSIX
`errno` convention. At sub-chunk 4 the translation has three tiers:

- **`__syscall_ret`** — the chokepoint every musl public wrapper funnels
  through — maps a raw `-1` to `errno = EIO` and a raw `-errno` in `[-4095,-2]`
  to that errno. See "The syscall seam — the error convention".
- **The sentinel** turns any un-retargeted POSIX call into `errno = ENOSYS`
  (the `0xFFFF` macro → `-ENOSYS` from `syscall_arch.h` → `__syscall_ret`).
- **Per-call precision** — where `EIO` is too coarse, a pouch lower-half C
  wrapper (later sub-chunks: sockets, signals, ...) determines a precise errno
  itself before returning. Sub-chunk 4 establishes the convention; the eight
  1:1 seam calls use the `EIO` generic, honest for them — a failed
  `read` / `write` / `close` is a defensible `EIO`.

Invariant **P-3**: no POSIX surface is silently wrong — every call either maps
to defined Thylacine behaviour or returns a documented `errno` (`EIO` for an
opaque failure, `ENOSYS` for an unimplemented call).

## Performance characteristics

Not yet measured. pouch v1.0 is static-only; the `malloc` backend, the
uncontended-lock fast path (`torpor`), and the `/srv` socket path are the
performance-relevant surfaces, sized as sub-chunks 7, 8-9, 12 land.

## Status

| Sub-chunk | What | Status |
|---|---|---|
| 1 `pouch-toolchain` | the `aarch64-thylacine` cross toolchain | landed (`90b5333`/`e03be8d`) |
| 2 `pouch-musl-vendor` | vendor musl 1.2.5; patch-series scaffold; upper-half probe | landed (`6f60b7e`/`45e287e`) |
| 3 `pouch-kernel-auxv` | `exec_setup` builds the System V startup frame (auxv); `SYS_SET_TID_ADDRESS` | landed (`d505e73`/`f2a1130`) |
| 4 `pouch-syscall-seam` | retarget the syscall table; the sentinel; the errno decode; the stdio backend; `build.sh sysroot` builds the real libc | landed (`dbc6bd3`/`aad33d6`) |
| 5 `pouch-hello-smoke` | the first pouch binaries — `/pouch-hello` + `/pouch-hello-stdio` build + run in Thylacine | landed (`5c0623d`) |
| 6a `pouch-ld` | the `pouch-ld` link-driver wrapper; `build_pouch_progs` links through it | landed (`eeaa5ab`) |
| 6b `pouch-compiler-rt` | vendor + build the compiler-rt builtins — `libclang_rt.builtins.a` | landed (`72f45f9`) |
| 6c `pouch-compiler-rt` | the real `printf` hello — `/pouch-hello-printf` | landed (`aea1ab2`) |
| 7a `pouch-mem` | kernel anonymous-memory syscalls `SYS_BURROW_ATTACH` / `_DETACH` | landed (`198fda1`) |
| 7b `pouch-mem` | the pouch side — `0003-pouch-mman` + `/pouch-hello-malloc` | landed (this chunk) |
| 8-16 | torpor → threads → poll → devnodes → sockets → signals → libsodium → stratumd | pending |

At sub-chunk 7b: the pouch cross-toolchain is **complete and proven** and
**`malloc` works**. Four POSIX C programs — `/pouch-hello`,
`/pouch-hello-stdio`, `/pouch-hello-printf`, `/pouch-hello-malloc` — build,
load, and run in Thylacine: they print, exit 0, and joey content-checks them
on every boot. `/pouch-hello-printf` exercises the compiler runtime
(`binary128` soft-float verified correct at runtime). `/pouch-hello-malloc`
exercises mallocng end-to-end over `SYS_BURROW_ATTACH` / `SYS_BURROW_DETACH`,
including the `> MMAP_THRESHOLD` individually-mmapped path and the
`realloc`-grow `mremap`-ENOSYS fallback. A POSIX C program with a working
heap runs on a Plan 9-heritage kernel that knows nothing about POSIX. Next
is sub-chunk 8 (`pouch-wait-addr` — the `torpor` primitive), then threads,
sockets, signals across the remaining sub-chunks.

## Known caveats / footguns

- **`third_party/musl/` is pristine — do not edit it.** All Thylacine changes
  go through `usr/lib/pouch/patches/`. A direct edit breaks the re-vendor /
  rebase model and silently violates invariant P-4.
- **A failed syscall reports the generic `EIO`** — a known, design-sanctioned
  limitation. Thylacine's flat-`-1` error convention carries no errno, so the
  eight 1:1 seam calls report `EIO` on any failure — not the precise `EBADF` /
  `EFAULT` / `EPIPE` a Linux caller might expect. `EIO` is a *documented* POSIX
  errno (not silently wrong — P-3 holds), but code that branches on a specific
  `errno` after `read` / `write` / `close` will not get it until either a
  richer kernel error channel exists or a pouch lower-half wrapper supplies
  precision (POUCH-DESIGN.md §5.1).
- **Terminal detection always reports "not a tty."** `ioctl` is a sentinel
  (Thylacine has no PTY at v1.0), so `isatty()` returns 0 with `errno = ENOTTY`
  and stdio's tty probe leaves `stdout` fully buffered. Correct for a v1.0
  system with no terminals; revisited when the PTY work (Phase 7, Utopia)
  lands.
- **`exit` and `exit_group` both terminate the whole process.** Both map to
  `SYS_EXITS`. Correct at v1.0 (single-threaded); the per-thread-exit
  distinction lands with `pouch-threads`.
- **The retarget keeps every musl `__NR_*` name.** An un-retargeted POSIX call
  resolves to the `0xFFFF` sentinel and returns `ENOSYS` at runtime — it does
  not fail to compile. "Compiles" therefore does not mean "implemented"; the
  sentinel is the marker of an un-done lower-half call.
- **`build/pouch/` and `build/sysroot/` are gitignored** and rebuilt from
  scratch by every `tools/build.sh sysroot` (~1-2 min); they are not committed.
- **Linking must invoke `ld.lld` directly.** clang as the ELF *link driver*
  is broken on a macOS host for the `aarch64-thylacine` triple — it falls into
  the Darwin toolchain and emits Mach-O linker arguments. The compiler path
  (`pouch-clang -c`) is fine. `build_pouch_progs` links with `ld.lld`
  directly; the CMake/autotools build chunks will need a real toolchain fix.
  See "Why linking cannot go through the clang driver."
- **The compiler runtime carries no unwinder.** `libclang_rt.builtins.a`
  includes `gcc_personality_v0.o`, which references `_Unwind_*` — a libunwind's
  symbols, and pouch has no libunwind. The references are dormant for plain C:
  nothing pulls `gcc_personality_v0.o` from the archive unless
  `__gcc_personality_v0` is referenced, which only `-fexceptions` / C++ cleanup
  code does. C++ or `-fexceptions` code therefore cannot link until pouch gains
  an unwinder — out of scope for v1.0 (static C: `printf`, libsodium,
  stratumd). See "The compiler runtime."
- **AF_UNIX `bind`/`connect` errno is coarse-grained (P6-pouch-sockets F3
  deferred).** Both calls collapse every kernel-side `SYS_post_service_byte`
  / `SYS_srv_connect` failure to one POSIX errno — `EACCES` for `bind`,
  `ECONNREFUSED` for `connect`. The kernel returns flat `-1` with no errno
  channel; pouch picks the most-likely cause for each call. A program
  hitting (a) the post-gate, (b) name-already-in-use, (c) registry-full,
  (d) handle-table-full, or (e) bad-name all sees the same errno for the
  call. Acceptable at v1.0; a future richer kernel error channel
  (POUCH-DESIGN.md §5.1) would let pouch surface precision.
- **Multi-thread bind/connect on the same socket fd is racy (P6-pouch-sockets
  F4 deferred).** `pouch_sock_resolve` returns a slot pointer under lock,
  but post-resolve state inspection (FRESH/LISTENING/CONNECTED) and the
  kernel-handle write happen without re-acquiring `g_lock`. Two pthreads
  concurrently calling `bind()` on the same fresh fd can each see
  `state==FRESH`, each post a service, and the second's write overwrites
  the first — orphaning the first listener handle in the kernel handle
  table. The proving binary uses one slot per thread; no exposure at
  v1.0. A real multi-threaded server doing concurrent socket ops on the
  same fd should serialize at the application layer or wait for the
  CAS-style transition helper.
- **`srv_conn_count` is not atomic for multi-thread Procs (P6-pouch-sockets
  F12 deferred).** Sub-chunks 9a/9b landed multi-threaded Procs, but the
  per-Proc cap counter `p->srv_conn_count` (kernel/devsrv.c:355-358) is
  read-then-incremented without atomicity. Two threads in the same Proc
  concurrently calling `SYS_SRV_CONNECT` can both pass the cap check and
  both proceed. The cap of 1 is briefly violated. At v1.0 the pouch
  proving binary issues exactly one client connect; the multi-thread
  cap violation is dormant. A future atomic-CAS would close it.
- **Service `mode` persists across TOMBSTONED (P6-pouch-sockets F7
  documented).** The header text "mode immutable through LIVE" omits
  the TOMBSTONED case. Behavior: `srv_proc_exit_notify` does NOT clear
  `e->mode` on tombstone, and `srv_reserve` of a tombstoned entry
  refuses a mode-changing rebind (F2 close). Net result: mode is in
  fact immutable across LIVE → TOMBSTONED → LIVE cycles for the same
  service name — a service identity property. A different mode means
  a different name.
- **AF_UNIX SOCK_STREAM server reads are blocking (P6-pouch-sockets F1
  close).** Unlike corvus's 9P-mode server (non-blocking; poll-then-
  read pattern), pouch's byte-mode server endpoint Spoor uses
  `srvconn_server_recv_blocking` (kernel/srvconn.c) — a tsleep on the
  c2s ring's Rendez until data arrives or the peer closes. This is the
  fix for the F1 race where a non-blocking `read()` returned 0 (POSIX
  EOF) when the server thread's accept-wake raced the client's first
  `write()` across SMP CPUs. A future per-connection idle deadline
  would extend by setting a `server_deadline_ns` analog to
  `client_deadline_ns`.

## Naming rationale

**pouch** — the libc — is the marsupium, the pouch in which a marsupial joey
develops, sheltered, until it can survive in the open. Foreign POSIX C code
"runs in the pouch": pouch's translation layer shelters it from the fact that
the kernel beneath is not the one it was written for. Unlike Plan 9's APE (the
second-class ANSI/POSIX Environment), pouch is a first-class, central,
nurturing system component. **torpor** — the wait-on-address primitive
(sub-chunk 8) — is the marsupial deep-sleep state: a thread enters torpor on an
address until another thread rouses it. See POUCH-DESIGN.md §16.

---

*Binding design: `docs/POUCH-DESIGN.md`. Phase pickup: `docs/phase6-status.md`.
Vendoring record: `third_party/README.md`. Patch series:
`usr/lib/pouch/patches/`.*
