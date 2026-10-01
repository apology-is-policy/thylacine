---
id: fnd-b1d-v-r3-f1
type: fnd
title: "The round-2 regression witness crossed a devnone source, so it was red on the fixed kernel and had never run"
round: adt-b1d-v-r3
severity: P2
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-09-25-b1d-v-emount
regression: "devsrv.service_keys_distinct (rewritten to mount-table assertions; verified red on the reverted kernel, green on the fix)"
created: 2026-09-28
---
## Prosecution

`devsrv.service_keys_distinct` (the round-2 F1 witness) resolved `/srv/a` after
MREPLing a `devnone` file over it, and `/srv/b` under an MBEFORE union at the
registry root. Both go through `stalk`'s cross, which calls `clone_walk_zero`,
which needs a zero-element `Walkqid`; `devnone_walk` returns NULL for every
call, so the cross returned -1 and the walks came back NULL. The `!= NULL`
assertions then failed before any identity was tested. The commit was unbuilt
("NOT BUILT; suite skipped"), so the red never surfaced. A raw registry root as
a resolution base also crosses member 0 only, so the union leg could not be an
honest resolution from the root regardless of the source.

## Disposition

Fixed test-only: the two crossing legs become `mount_is_point_id` assertions on
the mount table (keyed on `(dc, devno, qid.path)`, exactly what crossing
consults) plus `mount_member_at(b, 0, NULL) == NULL`. Pre-fix `a`, `b` and the
root share key 0, so the two negative legs fail; post-fix they pass. Verified
by reverting the devsrv `qid_path` hunks (red) and on the fix (green). The
kernel fix under audit was sound; only its witness was broken.
