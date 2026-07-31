---
id: seam-larder-loom-bypass
type: seam
title: "Loom async FS mutations bypass every Larder invalidate"
status: open
surface: [sub-kernel-larder]
opened-by: fnd-l1f-r1-f2
tracker: ""
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: Larder invalidation from the Loom completion path (the
completion carries the op's fid → the affected file/dir qid.path → the
same `larder_*_invalidate` calls), OR a fail-closed reject of a Loom
mutation op submitted against a `cacheable` dev9p fid.

The Larder is populated + invalidated only on the SYNCHRONOUS dev9p
path. The Loom async engine (`LOOM_OP_WRITE` / `MKNOD` / `SYMLINK` /
`LINK` / `UNLINKAT` / `RENAMEAT`) drives `p9_client_*` directly and
touches no sub-cache, so a client mixing Loom mutations with synchronous
resolution on the SAME mount is, from the Larder's view, an out-of-band
writer: a Loom write leaves stale attr/pages, a Loom dirent-mutation a
stale dentry, bounded by the next own-sync mutation or LRU. The term-2
narrowing (name-specific dentry invalidation) additionally removed an
incidental non-guaranteed heal (a sibling mutation's whole-parent drop
used to force-re-walk a Loom-mutated name — fnd-term2-r1-f2).

**What closes it**: the completion-side qid.path plumbing (non-trivial —
the completion must resolve fid → qid for dirent ops), or the fail-closed
reject.

**Risk while open**: SELF-INFLICTED ONLY (the L1f-F2 wording correction:
"self-inflicted-reachable", not "unreachable") — a crafted EL0 program
could mint a stale view of its OWN file under its OWN qid.path; no
cross-file / cross-Proc / privilege leak. No v1.0 consumer mixes the
paths (the go build is pure synchronous pouch/musl; Loom consumers do
network / FSYNC / NOP).
