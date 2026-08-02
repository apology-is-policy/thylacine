---
id: seam-pouch-dirfd
type: seam
title: "Every pouch `*at` function is AT_FDCWD-only"
status: open
surface: [sub-pouch-fs]
opened-by: chg-2026-07-23-cl1a-fs-wires
tracker: "CL-1a"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`openat` / `fstatat` / `fchmodat` / `mkdirat` / `unlinkat` / `renameat` /
`faccessat` / `readlinkat` all reject a real dirfd with `-ENOTSUP`. The
kernel has no dirfd-relative resolution surface: `SYS_open` starts either
FROM_ROOT (cwd-joining a relative path) or from a Spoor fd, and the
mutation primitives take a parent fd + a LEAF, not a path.

Fail-closed and loud, so P-3 holds — but a program using the *at family
for its actual purpose (a race-free directory-relative walk) cannot.

## The lift

The parent-fd+leaf primitives already accept a caller-supplied parent,
so the mutation family is a short step: pass the caller's dirfd through
instead of `__pouch_open_parent`'s resolved one. `openat` with a real
dirfd needs the kernel's relative-open arm (`SYS_open` with a start fd),
which exists — the restriction is pouch-side conservatism, not a kernel
gap.
