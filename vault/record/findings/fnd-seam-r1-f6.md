---
id: fnd-seam-r1-f6
type: fnd
round: adt-seam-r1
severity: P3
status: fixed
title: "`patch -t` silently skips an already-applied patch"
surface: [sub-pouch-seam]
threatens: []
fixed-by: chg-2026-05-22-p6-syscall-seam
regression: "the post-apply `.rej` scan in `build_sysroot`"
created: 2026-08-01
---
## Prosecution

`build_sysroot` applies the series with `patch -t` (deliberately — it
keeps the apply non-interactive, so a mismatch cannot hang the build).
But `-t` also makes an already-applied or partly-applying patch a quiet
skip, leaving `.rej` files and a sysroot that builds cleanly while
missing a boundary-line replacement.

## Fix

Scan `$musl_src` for `.rej` after the apply loop and abort if any exist.
`-t` is kept; the `rm -rf` of the work tree each run already prevents
double-apply in the normal flow, so the scan is the defense against the
abnormal one.
