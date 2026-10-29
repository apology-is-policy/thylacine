---
id: chg-2026-10-13-loom-ledger-one-owner
type: chg
title: "Loom's I-32 ledger: one owner, bound with the charge"
date: 2026-10-13
arc: arc-vault
commits: ["df458fae4", "8809cdd5c"]
touched: [sub-kernel-loom, sub-kernel-loom-bridge, sub-kernel-handle, sub-kernel-syscall-dispatch]
established: []
closed: []
opened: []
supersedes: chg-2026-08-16-loom-charge-ledger
mirrors-checked: []
depth: skeletal
created: 2026-10-13
---
Supersedes the August record's "The duplication that must not be removed". The
ring's page owner used to bind last, beside a separate thread owner bound
first, because SYS_LOOM_SETUP's rollbacks refunded the ring's pages explicitly.
That discipline was itself the defect: the refund ran after the map's lock had
dropped, beside an exact-geometry unmap whose failure was ignored, so a ring a
sibling thread had trimmed or split stayed mapped and resident with its pages
refunded and no charge record for its last drop (OPEN-BUGS 2026-10-13 15:48Z,
P2 latent, [[inv-i32]]). Setup now binds the owner and stamps the ring's charge
record in the lock hold that charges and maps the ring, every later failure path
settles by claim, the owner precedes the thread charge, and the second pointer
is gone. The same change made the params-copyout rollback act by identity (the
descriptor via `handle_close_obj`, the mapping via `sys_loom_ring_detach`).
[[sub-kernel-loom]] "One owner, bound with the charge" carries the reasoning.
