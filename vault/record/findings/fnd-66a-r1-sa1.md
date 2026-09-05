---
id: fnd-66a-r1-sa1
type: fnd
title: "#66a SA-1: path_addelem(namelen==0) built a trailing-slash path"
round: adt-66a-r1
severity: P3
status: fixed
surface: [sub-kernel-path]
threatens: []
fixed-by: chg-2026-06-12-66a-spoor-path
created: 2026-08-01
---
## Prosecution

A non-root parent + an empty component built `"/a/"` (parent + "/" + 0
bytes) — a malformed name. UNREACHABLE from the kernel (stalk
guarantees clen ≥ 1; the walk-open/create handlers reject
`name_len == 0`) and test-unreached — but the helper was not total.

## Disposition

Fixed (self-audit origin, merged with the formal round's fixes):
`namelen == 0` rejected at the top of `path_addelem`, which also
fences the newlen arithmetic. Fable independently verified the case
unreachable — the fix is totality hygiene, now the
[[sub-kernel-path]] Prosecution line "path_addelem must stay total".
