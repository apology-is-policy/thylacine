# pouch — the Thylacine POSIX libc

**pouch** is Thylacine's native C library that also presents a POSIX surface.
Foreign POSIX C source "runs in the pouch": pouch's translation layer shelters
it from the fact that the kernel beneath is not the one it was written for.

- Binding design: `docs/POUCH-DESIGN.md`
- As-built reference: `docs/reference/78-pouch.md`
- Phase pickup guide: `docs/phase6-status.md`

## The two halves

pouch is structured as two halves split at one boundary line:

- **Upper half** — vendored musl (`third_party/musl/`), near-unmodified:
  portable C, pure computation, no OS knowledge (`string` / `stdio` / `stdlib`
  / `math` / `ctype` / ...).
- **Lower half** — Thylacine-native: the POSIX runtime layer that translates
  POSIX file I/O, sockets, `poll`, signals, and threads into Thylacine
  primitives. This is pouch's own code.

Invariant **P-4** (POUCH-DESIGN.md §11): the boundary line holds — the upper
half carries no Thylacine-specific code; the lower half carries all of it.

## This directory

| Path | What |
|---|---|
| `patches/` | The boundary-line patch series against vendored musl. |

The lower-half C source, the CRT glue, and the sysroot build wiring land in
later Phase 6 sub-chunks (`pouch-kernel-auxv`, `pouch-syscall-seam`,
`pouch-mem`, `pouch-wait-addr`, `pouch-threads`, `pouch-sockets`,
`pouch-signals`, ...). See `docs/POUCH-DESIGN.md §14` for the sub-chunk
decomposition.
