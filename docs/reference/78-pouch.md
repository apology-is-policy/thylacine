# 78 — pouch: the POSIX libc — vendoring & the boundary line

> **Status note.** This document is the as-built reference for **pouch**,
> Thylacine's POSIX libc (execution Phase 6). It is written incrementally as
> Phase 6 sub-chunks land. Through sub-chunk 5 it covers the vendoring of musl,
> the boundary-line architecture + inventory, the kernel-side process-startup
> additions (the auxiliary vector, `SYS_SET_TID_ADDRESS`), the syscall seam —
> the syscall-number retarget, the unimplemented-syscall sentinel, the
> Thylacine error-convention decode, and the stdio backend — and the first
> pouch binaries running in Thylacine. Sections for pouch's lower-half API,
> data structures, and state machines are stubbed with forward pointers and
> filled in by sub-chunks 7-13. The binding design is `docs/POUCH-DESIGN.md`.

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
| `malloc/` (18) | MIXED | the `mallocng` allocator — logic portable; rests on the anonymous-memory backend | 7 |
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
| `mman/` (13) | LOWER | `mmap`/`mprotect`/`madvise`/`mlock` → Burrow | 7 |
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
-p1`, and builds out-of-tree (see "Build" below). At sub-chunk 4 the series
holds two patches — `0001-pouch-syscall-seam.patch` and
`0002-pouch-stdio-no-iovec.patch` (see "The syscall seam" next); the
boundary-line replacement continues across the lower-half sub-chunks 7-13.

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
2. **Link** — `ld.lld` invoked **directly** (not through the clang driver).

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
it by invoking `ld.lld` directly with an explicit ELF link line. **Sub-chunk 6
(`pouch-compiler-rt`) adds a `pouch-ld` wrapper** that drives `ld.lld` with the
pouch ELF link line, so the CMake/autotools build chunks (libsodium, stratumd —
sub-chunks 14-16) link cleanly: the CMake toolchain points
`CMAKE_C_LINK_EXECUTABLE` at `pouch-ld`.

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
is therefore **deferred** to **sub-chunk 6 (`pouch-compiler-rt`)** — vendor +
build the compiler-rt builtins for `aarch64-thylacine`.
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
the only regression. The Phase 6 proving set — static hello, the multithreaded
test, the `AF_UNIX` echo pair, libsodium's self-test, stratumd's boot — lands
across sub-chunks 5, 9, 12, 14, 16. See `docs/POUCH-DESIGN.md §13` for the
exit-criteria checklist.

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
| 6 `pouch-compiler-rt` | the compiler runtime (compiler-rt builtins) + the `pouch-ld` link wrapper + the real `printf` hello | **next** |
| 7-16 | mem → torpor → threads → poll → devnodes → sockets → signals → libsodium → stratumd | pending |

At sub-chunk 5: the pouch sysroot (sub-chunks 1-4) compiles + links a pouch
program, and the first two — `/pouch-hello` and `/pouch-hello-stdio` — build,
load, and run in Thylacine: they print, exit 0, and joey content-checks them
on every boot. A POSIX C program now runs on a Plan 9-heritage kernel that
knows nothing about POSIX. What remains for the toolchain to be complete is a
compiler runtime — sub-chunk 6 (`pouch-compiler-rt`), next (see "The
compiler-runtime gap"); pouch's lower half (sockets, threads, signals, the
allocator) lands across sub-chunks 7-13.

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
- **The sysroot has no compiler runtime.** A complete C cross-toolchain needs
  compiler-rt builtins; the pouch sysroot has none (Homebrew LLVM ships only
  Darwin compiler-rt). Programs that reference a builtin — notably `printf`,
  whose `vfprintf` needs `binary128` soft-float — will not link until sub-chunk
  6 (`pouch-compiler-rt`) lands. See "The compiler-runtime gap."

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
