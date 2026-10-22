---
id: seam-co-fidless-wstat
type: seam
title: "fchmod/fchown/futimens on a cached-open fd fails loud (no fid to Tsetattr)"
status: open
surface: [sub-kernel-ninep-dev9p]
opened-by: chg-2026-07-11-fid-lifecycle
tracker: ""
created: 2026-07-31
updated: 2026-10-22
---
**Owed** (only if a consumer appears): the retain-the-walk-fid-unopened
cached-open variant, so a fidless open can late-address a Tsetattr.
Today `dev9p_wstat_native` on a `cached_open` priv answers EOPNOTSUPP
LOUD (a flat -1 until G12, 2026-10-22) —
Tsetattr is fid-addressed, a fidless Spoor cannot late-bind (no
fid-from-qid op exists, and a retained-path re-walk is rename-unsound).

**Why open is tolerable**: no v1.0 consumer fchmod/fchowns a read-only
cached-open fd (cmd/go's cache mtime updates are path-based Chtimes);
path-based chmod/chown/utimensat are untouched, since an O_PATH open is
never a cached open. G12's time setter widens the reach to futimens on a
small file opened read-only, so the item is tracked in OPEN-BUGS
(2026-10-22 06:54Z, P3).

**Risk while open**: a future POSIX port doing fchmod-on-O_RDONLY-fd on a
loose mount gets a loud failure, never silent corruption.
