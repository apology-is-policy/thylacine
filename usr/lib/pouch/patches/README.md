# pouch — the boundary-line patch series

The Thylacine delta against vendored musl (`third_party/musl/`, pinned 1.2.5)
as a **patch series concentrated at musl's boundary line**. pouch is openly a
*musl derivative*, not musl; this series is that derivation, made explicit and
reviewable.

## Why a patch series, not a fork

- The Thylacine changes are deep but *localized*. The OS-boundary lower half
  (sockets, threads, signals, the syscall seam) is replaced; the portable
  upper half (string / stdio / stdlib / math) is untouched. A localized patch
  series is tractable; a whole-tree fork forfeits upstream musl's security
  fixes.
- The series is scripture-tracked. An upstream musl security release →
  re-vendor `third_party/musl/` (see `third_party/README.md`), rebase this
  series.
- The series' *size* is the honest measure of pouch's divergence from musl.

## Invariant P-4 — the boundary line holds

Every patch in this series touches **only** musl's lower half + the syscall
seam. No patch touches the portable upper half. If a patch needs to modify
`src/string/`, `src/stdlib/`, `src/stdio/`'s computational core, `src/math/`,
`src/ctype/`, `src/multibyte/`, `src/regex/`, ... — stop: those are upper-half
files, and a boundary-line patch series does not touch them. The per-directory
UPPER / LOWER / SEAM inventory is in `docs/reference/78-pouch.md`.

## Layout

- `series` — the quilt-style ordered list; one patch per line, applied
  top-to-bottom. `#` lines are comments.
- `NNNN-<slug>.patch` — the patches, numbered in apply order (e.g.
  `0001-syscall-seam-retarget.patch`).

## How the series is applied

`third_party/musl/` stays **pristine**. `tools/build.sh sysroot` copies the
vendored tree to `build/pouch/musl-src/` (gitignored), applies this series
there with `patch -p1`, then configures + builds the pouch libc out-of-tree in
`build/pouch/musl-obj/` and installs it into `build/sysroot/` — the vendored
source is never edited.

A patch that fails to apply aborts the `sysroot` build loudly. After
re-vendoring `third_party/musl/` to a newer release, rebase the series against
it; each patch carries a preamble describing its intent, and `0001` records the
exact awk filter that regenerates the syscall-table retarget.

## Status

Two patches, landed at sub-chunk 4 (`pouch-syscall-seam`):

- `0001-pouch-syscall-seam.patch` — retargets musl's aarch64 syscall seam
  (`bits/syscall.h.in`, `syscall_arch.h`, `syscall_ret.c`) to the Thylacine
  ABI: Thylacine numbers for the eight 1:1 calls, a `0xFFFF` sentinel →
  `-ENOSYS` for the rest, and the Thylacine `-1` → `errno` decode.
- `0002-pouch-stdio-no-iovec.patch` — replaces the `writev`/`readv`-based
  stdio backend ops (`__stdio_write.c`, `__stdio_read.c`) with `SYS_write` /
  `SYS_read`.

The boundary-line replacement continues across Phase 6 sub-chunks 5-12
(`pouch-mem` through `pouch-signals`). See `docs/POUCH-DESIGN.md §14`.
