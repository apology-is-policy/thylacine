---
id: chg-2026-07-12-term2-dentry-name
type: chg
title: "term-2: name-specific L1d dentry invalidation (the wga narrowing)"
date: 2026-07-12
arc: arc-go-build
commits: ["b317fc28"]
touched: [sub-kernel-larder, sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
`larder_dentry_invalidate_name` replaces the whole-parent dentry drop: a
single-name create/rename/unlink stales ONLY the mutated (parent, name)
binding — siblings preserved (creating `foo` cannot change whether `bar`
exists; dentries are per-(parent,name) existence populated from walks,
not listings), O(1) via the serve's hash. Closes the cold-band wga
thrash (every sibling re-walked after each of the build's own
mutations) AND retires the task-#30 O(dentry_cap)-scan seam
([[fnd-29-r1-f2]]) in one move — the faithful realization of
fs_cache.tla's per-token OwnWrite. The mutation enumeration was proven
complete for name-specificity at [[adt-term2-r1]] (no synchronous op
changes a SIBLING's existence without a create/unlink/rename on that
sibling; mknod/symlink/link have no Dev slot). Regression
`larder.dentry_invalidate_name` (drops the named binding AND preserves a
sibling — fails on the whole-parent drop).
