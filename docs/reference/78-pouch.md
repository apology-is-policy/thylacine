# 78 — pouch: the POSIX libc — vendoring & the boundary line

> **Status note.** This document is the as-built reference for **pouch**,
> Thylacine's POSIX libc (execution Phase 6). It is written incrementally as
> Phase 6 sub-chunks land. At sub-chunk 2 (`pouch-musl-vendor`) it covers the
> vendoring of musl, the boundary-line architecture, and the boundary-line
> inventory. Sections for pouch's lower-half API, data structures, state
> machines, and error paths are stubbed with forward pointers and filled in by
> sub-chunks 3-12. The binding design is `docs/POUCH-DESIGN.md`.

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
| `malloc/` (18) | MIXED | the `mallocng` allocator — logic portable; rests on the anonymous-memory backend | 6 |
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
| `mman/` (13) | LOWER | `mmap`/`mprotect`/`madvise`/`mlock` → Burrow | 6 |
| `select/` (4) | LOWER | `select`/`poll`/`ppoll` → `t_poll` / `SYS_POLL` | 9 |
| `thread/` (133) | LOWER | pthreads + atomics + the futex calls → Thylacine threads + `torpor` | 7-8 |
| `signal/` (40) | LOWER | `sigaction`/`kill`/`raise`/`sigprocmask` → notes | 12 |
| `network/` (77) | LOWER | sockets, resolution → `/srv` (`AF_UNIX`); `AF_INET` deferred | 11 |
| `process/` (34) | LOWER | `posix_spawn` → `rfork`; `fork` declined; `exec*`/`wait*` | 11+ |
| `sched/` (10) | LOWER | `sched_yield` / affinity | 4+ |
| `temp/` (7) | LOWER | `mkstemp`/`tmpfile` (uses `open`) | 4 |
| `errno/` (2) | SEAM | `__errno_location` (TLS-backed) | 8 |
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

`third_party/musl/` stays pristine; the build copies it to a working tree under
`build/pouch/` (gitignored), applies the series there, and builds out-of-tree.
The series is **empty at sub-chunk 2** — the boundary-line replacement lands
across sub-chunks 3-12. The exact `tools/build.sh sysroot` wiring lands with
sub-chunk 4 (`pouch-syscall-seam`), the first sub-chunk that adds a patch.

The series' size is the honest, reviewable measure of pouch's divergence from
musl. An upstream musl security release is handled by re-vendoring
`third_party/musl/` and rebasing the series — never by editing the vendored
tree.

---

## Build — the upper-half cross-compile probe

Sub-chunk 2 verifies the boundary-line architecture against reality: does
vendored musl take the `aarch64-thylacine` cross toolchain (sub-chunk 1)
cleanly? This is the POUCH-DESIGN.md **risk R2** probe ("musl's lower half may
resist a clean seam").

The probe builds the static archive out-of-tree:

```sh
mkdir -p build/pouch/musl-obj && cd build/pouch/musl-obj
sh ../../../third_party/musl/configure \
    --target=aarch64-thylacine --disable-shared \
    CC="/opt/homebrew/opt/llvm/bin/clang --target=aarch64-thylacine" \
    AR=/opt/homebrew/opt/llvm/bin/llvm-ar \
    RANLIB=/opt/homebrew/opt/llvm/bin/llvm-ranlib
make -j8 lib/libc.a
```

**Result** (musl 1.2.5, Homebrew Clang 22.1.4):

- musl's `configure` **accepts the `aarch64-thylacine` triple** — it derives
  `ARCH=aarch64` and writes `config.mak` without complaint. (The linker-flag
  probes report "no" — clang has no linker defaults for the unknown
  "thylacine" OS — but that affects only the *link* of test executables and
  the shared library, not the compile of `libc.a`. pouch's link lines name the
  CRT objects + compiler-rt explicitly; that wiring is sub-chunks 4-5.)
- `make lib/libc.a` **succeeds**: **1345 translation units compiled, 0 errors,
  0 warnings**, producing a 2.4 MB `lib/libc.a` of valid `ELF 64-bit LSB
  relocatable, ARM aarch64` objects.
- musl compiles its own source hermetically — `-std=c99 -nostdinc
  -ffreestanding` with only musl's own `-I` paths; no host headers leak in.

**R2 verdict** — the boundary line is **compile-clean**. Vendored musl, as
portable C, cross-compiles end-to-end for `aarch64-thylacine` with the pouch
toolchain. The seam is as clean as the design assumed: the patch series'
burden is **semantic** — replacing what the lower half *does* (which syscalls,
which ABI, which `errno` convention) — not a fight with what it *compiles to*.

Caveat: this `libc.a` is built from *unpatched* musl, so it is structurally
valid but **semantically Linux** — `obj/include/bits/syscall.h` still holds
Linux `__NR_*` numbers, and the lower half assumes the Linux kernel ABI. It is
not a runnable pouch libc; it is the R2 evidence that the upper half is sound.
The semantically-correct pouch `libc.a` — patch series applied, seam
retargeted, installed into the sysroot — is sub-chunks 4-5.

Build artifacts live under `build/pouch/` (gitignored); the vendored source is
untouched.

---

## Public API

Not yet — pouch exposes no API surface at sub-chunk 2. pouch's lower-half API
(the POSIX runtime layer) lands across sub-chunks 3-12; this section is filled
in as each lands. The headers pouch will install into the sysroot are musl's
(`include/`), retargeted at the seam. See `docs/POUCH-DESIGN.md §6, §8` for the
POSIX surface pouch commits to and the documented-error surface it does not.

## Data structures

Not yet — see `docs/POUCH-DESIGN.md §7` for the thread model (the first
pouch-native data structures land with `pouch-threads`, sub-chunk 8).

## State machines

Not yet. The `torpor` wait-on-address state machine lands with sub-chunk 7
(`pouch-wait-addr`), spec-first against `specs/futex.tla`.

## Spec cross-reference

pouch's invariant-bearing kernel additions are spec-pinned:

| Sub-chunk | Spec | Invariant |
|---|---|---|
| 7 `pouch-wait-addr` (`torpor`) | `specs/futex.tla` (#7) | wait-on-address atomicity (I-9) |
| 12 `pouch-signals` | `specs/notes.tla` (#8) | note delivery ordering (I-19) |
| 9 `pouch-poll` | `specs/poll.tla` (#6) | missed-wakeup-freedom (I-9) |

Sub-chunk 2 introduces no spec obligation (vendoring + scaffolding).

## Tests

Sub-chunk 2 adds no kernel-test-suite cases (vendoring + scaffolding; no kernel
code). Its verification is the build probe above. The Phase 6 proving set —
static hello, `printf` hello, the multithreaded test, the `AF_UNIX` echo pair,
libsodium's self-test, stratumd's boot — lands across sub-chunks 5, 8, 11, 13,
15. See `docs/POUCH-DESIGN.md §13` for the exit-criteria checklist.

## Error paths

Not yet — pouch's Thylacine-rc → POSIX-`errno` mapping lands with the syscall
seam (sub-chunk 4). Per POUCH-DESIGN.md §5.1, every pouch lower-half wrapper
carries a per-call mapping from Thylacine's `-1`-on-error convention to a POSIX
`errno`. Invariant P-3: no POSIX surface is silently wrong — each either maps
to defined behavior or returns a documented `errno`.

## Performance characteristics

Not yet measured. pouch v1.0 is static-only; the `malloc` backend, the
uncontended-lock fast path (`torpor`), and the `/srv` socket path are the
performance-relevant surfaces, sized as sub-chunks 6, 7-8, 11 land.

## Status

| Sub-chunk | What | Status |
|---|---|---|
| 1 `pouch-toolchain` | the `aarch64-thylacine` cross toolchain | landed (`90b5333`/`e03be8d`) |
| 2 `pouch-musl-vendor` | vendor musl 1.2.5; patch-series scaffold; upper-half probe | **this chunk** |
| 3-15 | auxv → seam → hello → mem → torpor → threads → poll → devnodes → sockets → signals → libsodium → stratumd | pending |

At sub-chunk 2: musl 1.2.5 is vendored at `third_party/musl/`; the boundary-line
patch series is scaffolded (empty) at `usr/lib/pouch/patches/`; the R2
upper-half cross-compile probe is green. The sysroot
(`build/sysroot/{include,lib}`) is still the empty skeleton from sub-chunk 1 —
populated by sub-chunks 3-5.

## Known caveats / footguns

- **`third_party/musl/` is pristine — do not edit it.** All Thylacine changes
  go through `usr/lib/pouch/patches/`. A direct edit breaks the re-vendor /
  rebase model and silently violates invariant P-4.
- **The probe `libc.a` is not a runnable libc.** It is built from unpatched
  musl and makes Linux syscalls. Do not install it into the sysroot or link a
  Thylacine program against it. The real pouch `libc.a` is sub-chunks 4-5.
- **Compiles ≠ correct.** That musl compiles for `aarch64-thylacine` proves the
  *upper half* is portable; it says nothing about the *lower half's* runtime
  behavior, which is Linux until the patch series replaces it.
- **`build/pouch/` is gitignored.** Re-run the probe (above) to regenerate it;
  it is not committed.

## Naming rationale

**pouch** — the libc — is the marsupium, the pouch in which a marsupial joey
develops, sheltered, until it can survive in the open. Foreign POSIX C code
"runs in the pouch": pouch's translation layer shelters it from the fact that
the kernel beneath is not the one it was written for. Unlike Plan 9's APE (the
second-class ANSI/POSIX Environment), pouch is a first-class, central,
nurturing system component. **torpor** — the wait-on-address primitive
(sub-chunk 7) — is the marsupial deep-sleep state: a thread enters torpor on an
address until another thread rouses it. See POUCH-DESIGN.md §16.

---

*Binding design: `docs/POUCH-DESIGN.md`. Phase pickup: `docs/phase6-status.md`.
Vendoring record: `third_party/README.md`. Patch series:
`usr/lib/pouch/patches/`.*
