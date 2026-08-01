# 104 — stalk: the per-Proc pathname resolver [ABSORBED INTO THE VAULT]

This document was absorbed at the stalk sweep
(`chg-2026-07-31-stalk-sweep`). Its content now lives, code-verified and
current, in the dossiers:

    vault/system/kernel/namespace/sub-kernel-stalk.md
    vault/system/kernel/namespace/sub-kernel-path.md

(the trail lifetime's three failure shapes, cross-on-descent + the
STALK_MOUNT carve-out, the per-component X-search, the POUNCE run
gather / fail-ordering post-scan / mount-mid-run split / carried attrs /
logical_depth double-cap, the STALK_STAT walk-query, the
FID-LIFECYCLE cached-open arm, the open=connect adoption, the ER-1
errno mapping, the #957 single-hop crossing, `struct Path` and its
three hook sites).

**What this file got WRONG by the time it was absorbed** (the reason
the dossiers are written from the code): stalk-3 was listed "pending"
though it landed June 2026; the borrowed-`start` TOCTOU and the
lock-free mount table were taught as OPEN caveats though #844 and
RW-4's `ns_lock` closed them; `PGRP_MAX_MOUNTS` was quoted 8 (now 20);
and the Performance section still said "no batching at v1.0" in a file
that documents the POUNCE.

The invariants live at `vault/invariants/inv-i28.md` (containment +
per-component X-search) and `inv-i33.md` (name retention is
non-load-bearing). The audit history (stalk-1, stalk-2, #957, RW-4
r1/r2, #66a, #81-June, POUNCE P-5) lives as adt-/fnd- Record notes;
the open debt as `seam-372-latched-double-xcheck`,
`seam-posix-pathname-form-gates`, `seam-fid-monotonic-reclaim`,
`seam-932-devsrv-readdir`, `seam-9p-tag-block-on-full`. Design
scripture is unchanged: `docs/STALK-DESIGN.md`,
`docs/POUNCE-DESIGN.md`, `docs/FID-LIFECYCLE-DESIGN.md`.
