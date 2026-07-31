---
id: seam-larder-stale-child-attr
type: seam
title: "rename/unlink leave the moved/unlinked file's OWN attr (stale ctime/nlink)"
status: open
surface: [sub-kernel-larder]
opened-by: fnd-l1f-r1-f3
tracker: ""
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: invalidating the moved/unlinked FILE's own cached attr at
`dev9p_rename` / `dev9p_unlink` for the plain-file case. Both sites
operate by (parent, name); the G2 era added a dentry-cache victim
resolution (`larder_dentry_lookup`) that DOES invalidate the resolved
victim's attr — but only when the binding is cached; an uncached binding
still leaves the child attr untouched.

A rename bumps the file's ctime; an unlink drops its nlink. A held-open
fid `fstat`'d after the guest's own unlink/rename (keyed by the file's
unchanged qid.path) serves the stale values until the next own-write or
LRU eviction.

**What closes it**: resolving the child qid unconditionally at both
sites (an extra RPC when the dentry is uncached — the cost that deferred
this), or accepting the dentry-resolved best-effort as the v1.x final
posture and re-classing this documented.

**Risk while open**: METADATA-ONLY — mode/uid/gid are unchanged by
rename/unlink and `perm_check` reads only those, so there is no
privilege / X-search / I-28 consequence; content untouched; nlink is
largely moot at v1.0 (no sync link/symlink/mknod path — hardlinks are
Loom-only, [[seam-larder-loom-bypass]]). The v9fs `cache=loose` residual
class. Found convergently by the L1f prosecutor (F3) and self-audit
(SA-1).
