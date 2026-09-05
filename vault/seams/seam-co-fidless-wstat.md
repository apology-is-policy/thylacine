---
id: seam-co-fidless-wstat
type: seam
title: "fchmod/fchown on a cached-open fd fails loud (no fid to Tsetattr)"
status: open
surface: [sub-kernel-ninep-dev9p]
opened-by: chg-2026-07-11-fid-lifecycle
tracker: ""
created: 2026-07-31
updated: 2026-07-31
---
**Owed** (only if a consumer appears): the retain-the-walk-fid-unopened
cached-open variant, so a fidless open can late-address a Tsetattr.
Today `dev9p_wstat_native` on a `cached_open` priv returns -1 LOUD —
Tsetattr is fid-addressed, a fidless Spoor cannot late-bind (no
fid-from-qid op exists, and a retained-path re-walk is rename-unsound).

**Why open is tolerable**: no v1.0 consumer fchmod/fchowns a read-only
cached-open fd (cmd/go's cache mtime updates are path-based Chtimes);
path-based chmod/chown are untouched.

**Risk while open**: a future POSIX port doing fchmod-on-O_RDONLY-fd on a
loose mount gets a loud failure, never silent corruption.
