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

`third_party/musl/` stays **pristine**. The build copies the vendored tree to a
working tree under `build/pouch/` (gitignored), applies this series there, and
builds out-of-tree from the patched copy — the vendored source is never edited.

The exact `tools/build.sh sysroot` wiring lands with sub-chunk 4
(`pouch-syscall-seam`) — the first sub-chunk that adds a patch. Until then the
series is empty and the sub-chunk-2 build probe builds pristine musl directly
(see `docs/reference/78-pouch.md` "Build").

## Status

Empty at sub-chunk 2 (`pouch-musl-vendor`). The boundary-line replacement lands
across Phase 6 sub-chunks 3-12 (`pouch-kernel-auxv` through `pouch-signals`).
See `docs/POUCH-DESIGN.md §14`.
