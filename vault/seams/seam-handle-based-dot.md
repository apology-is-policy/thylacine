---
id: seam-handle-based-dot
type: seam
title: "The cwd is a cleaned string, not a Spoor — symlinks force the upgrade"
status: open
surface: [sub-kernel-territory]
opened-by: chg-2026-06-09-ls4-cwd
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Plan 9 and Linux both hold the cwd as a live handle (`Chan`/`dentry`).
Thylacine holds a cleaned absolute STRING and re-resolves it from
`root_spoor` on every relative open. The v1.0 choice was deliberate and
correct — see What closes it — but it is a choice with an expiry.

Two consequences today. A `cd` into a directory that is then renamed or
unmounted leaves the Proc's cwd naming something that no longer resolves,
where a handle-based dot would have kept working (or kept failing, but
consistently). And relative resolution pays a full walk from root each
time rather than starting at a held dot.

## What closes it

Symlinks (G11). The moment a path component can be a symlink, lexical
`..` resolution stops agreeing with structural resolution — `a/b/..` is
not `a` when `b` is a symlink — and the string form becomes actually
wrong rather than merely re-resolving. That is the forcing function: the
upgrade lands WITH symlinks, not before.

The upgrade is a `dot` Spoor alongside (or replacing) `dot_path`, which
drags in a `..`-walk mechanism on the Dev vtable, because
[[sub-kernel-stalk]] contains `..` at its trail floor and a handle-dot
start has no trail above it.

## Risk while open

Low, and deliberately so — the string form adds ZERO new [[inv-i28]]
mechanism, which is exactly why it was the right v1.0 shape for a
security-critical resolver before symlinks exist. `cwd_lexical_resolve`
always hands `stalk` an absolute-from-root path, and `stalk` re-clamps
`..` at `root_spoor` regardless, so the two containment arguments
compose rather than substitute. A stale cwd names something wrong; it
cannot name something OUTSIDE.
