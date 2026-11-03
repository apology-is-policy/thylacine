---
id: dec-2026-10-22-g12-pulled-forward
type: dec
title: "The time setter (G12) is built now, so fontconfig's test runs unmodified"
date: 2026-10-22
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-dispatch, sub-kernel-perm, sub-kernel-ninep-dev9p]
created: 2026-10-22
---
## Fork

fontconfig's upstream test `test_bz106632` copies a font with
`system("cp -a ...")`, and its result depends on the copy keeping the
file's times. Thylacine had no way to set a time: SYS_WSTAT set mode,
owner, group and size; Pouch's `utimensat` was unimplemented; the native
`touch` could not move an mtime and the native `cp` had no `-a` or `-p`.
LIFE-SUPPORT LS-9 listed the setter (G12) for v1.x.

## Options

1. **Pull G12 forward**: the kernel setter, Pouch's `utimensat` family,
   go-thylacine's `os.Chtimes`, the native `touch` and `cp -p`/`-a`, so the
   test runs as upstream wrote it.
2. **A port patch** turning the test's `cp -a` into `cp` (recommended at
   the time, as the smaller change).

## The call

Option 1 (operator, 2026-10-22). The syscall's shape was a second vote:
[[dec-2026-10-22-wstat-times]].

## Rationale

The test exercises a real behaviour, a font cache noticing a copied face
whose times were kept, and a patched test would no longer prove it. Every
POSIX port that keeps times (`cp -p`, `tar`, `rsync`, `make` inputs,
cmd/go's cache) needs the same setter, and building it once now is the
depth-first dependency. On the way it found two Stratum defects: a
directory's mtime never moved when an entry was created or removed, and
Tsetattr ignored the `_SET` bits (OPEN-BUGS 2026-10-22).
