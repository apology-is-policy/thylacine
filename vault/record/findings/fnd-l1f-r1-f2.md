---
id: fnd-l1f-r1-f2
type: fnd
title: "Loom async mutations bypass every Larder invalidate — 'unreachable' overclaimed"
round: adt-l1f-r1
severity: P3
status: deferred
surface: [sub-kernel-larder]
threatens: [inv-i38]
seam: seam-larder-loom-bypass
created: 2026-07-31
---
## Prosecution

The invalidation hooks live only on the SYNCHRONOUS dev9p vtable; the
Loom engine's WRITE/MKNOD/SYMLINK/LINK/UNLINKAT/RENAMEAT drive
p9_client_* directly and touch no larder_* call. The design's
"unreachable at v1.0" was TOO STRONG: no v1.0 consumer drives a Loom
mutation on a cacheable Stratum fid (confirmed — Weft is /net
non-cacheable; loom-stress/bench are FSYNC/NOP), but a crafted EL0
program COULD (SYS_LOOM_* + a Stratum fid), self-inflicting a stale view
of ITS OWN file. Page/attr keys are the file's own qid.path — no
cross-file, cross-Proc, or privilege leak.

## Disposition

Wording corrected in the design scripture ("not driven by any v1.0
consumer; self-inflicted-reachable" — never "unreachable"); the
mechanism fix (invalidate-from-Loom-completion, or fail-closed reject of
a Loom mutation on a cacheable fid) deferred to the seam. The term-2
round later noted the name-specific narrowing also removed an
incidental non-guaranteed heal of this class (fnd-term2-r1-f2).
