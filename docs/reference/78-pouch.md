# 78 — pouch: the POSIX libc [ABSORBED INTO THE VAULT]

Absorbed at the pouch sweep (`chg-2026-08-01-pouch-sweep`). Its content
now lives, code-verified against all 31 patches and current, in:

    vault/system/boundary/pouch-seam/moc-pouch-seam.md   (the plane + P-1..P-4)
    vault/system/boundary/pouch-seam/sub-pouch-seam.md   (number table, sentinel, errno, stdio)
    vault/system/boundary/pouch-seam/sub-pouch-fs.md     (open/stat/readdir/mutation/readlink)
    vault/system/boundary/pouch-seam/sub-pouch-thread.md (pthreads + sleeping over torpor)
    vault/system/boundary/pouch-seam/sub-pouch-process.md (spawn/wait/pipe, /env, the terminators, mallocng)
    vault/system/boundary/pouch-seam/sub-pouch-signal.md (signals over notes)
    vault/system/boundary/pouch-seam/sub-pouch-net.md    (poll/select, AF_UNIX, AF_INET)
    vault/system/boundary/pouch-seam/sub-pouch-tty.md    (the tty ioctl dispatcher)

**What this file got WRONG by the time it was absorbed.** The headline is
a numbering collision it never absorbed. The gfx-4 merge renumbered two
aux-branch patches (`0025` → `0028`, `0026` → `0029`) and DROPPED a
third, while main independently landed new `0024` / `0025` / `0026`. This
document names all three of those numbers under their OLD meanings —
including a full section for `0024-pouch-fopen-create.patch`, a file that
no longer exists in the tree — so three patch numbers each denote a
different patch here than in `usr/lib/pouch/patches/`.

Two caveats asserted the opposite of what the document itself documents:

- "**`exit` and `exit_group` both terminate the whole process. Both map
  to `SYS_EXITS`**" — false since #809. `0001`'s own header says
  `__NR_exit → 0` (`SYS_EXITS`) and `__NR_exit_group → 60`
  (`SYS_EXIT_GROUP`, the cascading group terminate).
- "**Terminal detection always reports 'not a tty'**" — retired by
  `0021` (PTY-3) and `0029` (#55c), and the section documenting the
  working `isatty` sits 400 lines BELOW the caveat denying it.

Three more: the patch series is described as "the boundary-line
inventory" for seven patches when it is 31; `0011` / `0012` / `0013` (the
`abort()` and mallocng-assert overrides that stopped every pouch
assertion from extincting the kernel) are never mentioned here at all —
they were documented only in `86-pouch-stratumd-boot.md`; and the
`select()` fd bound is described as `PROC_HANDLE_MAX = 64`, a name that
became wrong at #355 when the fd table grew to 256 (see
`vault/system/seams/seam-pouch-select-fd-bound.md`).

Binding design (unchanged, still authoritative): `docs/POUCH-DESIGN.md`.
