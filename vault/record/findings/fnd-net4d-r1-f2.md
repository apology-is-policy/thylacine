---
id: fnd-net4d-r1-f2
type: fnd
title: "Content[128] left a ~6-byte margin under the widest status render (and the commit message claimed 256)"
round: adt-net4d-r1
severity: P3
status: fixed
surface: [sub-netd-server]
threatens: []
fixed-by: chg-2026-06-18-net4-cs-dns-ipifc
created: 2026-07-31
---
## Prosecution

`Content` was `[u8; 128]` while the net-4c commit message asserted a
128→256 bump that never landed — a doc/code mismatch over a thin
margin (the worst-case ipifc status render ≈ 120–126 B). SAFE
regardless: `push` min-clamps every copy (truncate, never OOB).

## Disposition

Fixed: the buffer bumped to the documented 256 (inert — realistic
renders already fit). The mismatch itself is the lesson: a commit
message's claim is not a change.
