# third_party/gnumake — vendored GNU make 4.4.1 (pruned-pristine)

Clade CL-1c (`docs/LLVM-DESIGN.md`). GNU make is the first real parallel-spawner
port: with `USE_POSIX_SPAWN` it drives CL-1b's `posix_spawn` + `wait4`, and with
`MAKE_JOBSERVER` left undefined a top-level `make -jN` runs the pure `job_slots`
counter + blocking `waitpid` reap — a clean fit for the Thylacine process substrate
(no fork/execve, pipe-blocking-only). The vendored tree is **never edited**; the
Thylacine delta lives in `usr/ports/gnumake/` (the SDL2/tyrquake idiom).

## Source pin

- **GNU make 4.4.1**, `https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz`
- sha256 `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3`

## What is vendored

- `src/*.c` `src/*.h` — the full pristine `src/` (58 files). Includes the
  committed `mkconfig.h` (PACKAGE strings) + `mkcustom.h` (alloca + fallback
  prototypes), which the upstream tarball ships complete (not configure-generated).
- `lib/*.c` `lib/*.h` `lib/*.in.h` — the bundled gnulib modules (15 files).

The alt-OS source files (`amiga.c`, `vms*.c`, `remote-cstms.c`) are vendored for
`#include` integrity but **not compiled** — the object list in
`tools/build.sh::build_gnumake()` is explicit.

## What is excluded from vendoring

- `src/w32/` (the Windows subtree), `po/` `doc/` `tests/` (translations, docs,
  the test suite), and the autotools/build machinery (`configure`, `Makefile*`,
  `build.sh`, `m4/`, `build-aux/`) — Thylacine cross-builds with the pouch
  toolchain directly (the libsodium "no cross make" idiom), so none of it is needed.

## The Thylacine delta (in `usr/ports/gnumake/`, NOT here)

- `config.h` — the hand-derived Thylacine config (an autoconf reference config.h
  with the CL-1c census flips; see its header + LLVM-DESIGN.md section 16).
- `generated/fnmatch.h` `generated/glob.h` — the two gnulib headers that make's
  `build.sh convert()` would normally emit (config-independent; committed like
  SDL2's `SDL_config.h`).
- `patches/` — source patches (empty at CL-1c-1: the port needs zero source
  edits; the whole delta is config).

## The compile list (30 src + 5 lib)

src: ar arscan commands default dir expand file function getopt getopt1 guile
hash implicit job load loadapi main misc output posixos read remake rule shuffle
signame strcache variable version vpath remote-stub

lib: concat-filename findprog-in fnmatch glob getloadavg

(`guile.c` self-stubs without `HAVE_GUILE`; `load.c`/`loadapi.c` self-stub without
`MAKE_LOAD`; `lib/alloca.c` is not built — musl provides `alloca`. `remote-stub.c`
is the default remote backend, not `remote-cstms.c`.)
