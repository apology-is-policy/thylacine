---
id: fnd-l1f-r1-f3
type: fnd
title: "rename/unlink leave the moved/unlinked file's OWN attr — stale ctime/nlink on a held-open fstat"
round: adt-l1f-r1
severity: P3
status: deferred
surface: [sub-kernel-larder]
threatens: [inv-i38]
seam: seam-larder-stale-child-attr
created: 2026-07-31
---
## Prosecution

dev9p_unlink invalidates the parent (+ dentries) but deliberately leaves
the child's own attr; dev9p_rename likewise never touches the moved
file's. A rename bumps the file's ctime and an unlink drops its nlink,
so a HELD-OPEN fid to a file the guest itself unlinks/renames, fstat'd,
serves stale ctime/nlink until the next own-write or LRU eviction. The
fix needs the child's qid.path, which the by-name sites do not hold
without an extra RPC.

## Disposition

Deferred to the seam (the v9fs cache=loose residual class). Bounded:
METADATA-ONLY — mode/uid/gid are unchanged by rename/unlink and
perm_check reads only those (no privilege/I-28 consequence); content
untouched; nlink largely moot at v1.0 (hardlinks are Loom-only). Found
CONVERGENTLY by the prosecutor (F3) and the self-audit (SA-1) —
independent derivation, same bound, same disposition. The G2 era later
narrowed it: when the (parent,name) binding is cached, the victim
resolution now invalidates the resolved child's attr — the uncached-
binding case remains.
