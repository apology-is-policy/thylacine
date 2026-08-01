---
id: chg-2026-06-12-81-cwalkonly
type: chg
title: "#81 (June): the O_PATH byte-I/O block (CWALKONLY)"
date: 2026-06-12
arc: arc-holotype-rw
commits: ["4b7c1508", "fe25495d", "94bb0a81"]
touched:
  - sub-kernel-stalk
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The #57b self-audit SA-2 close: `STALK_WALK`/O_PATH skips BOTH the
final R/W perm_check AND `Dev.open`, and the handle is born `R|W` — so
a non-owner who could X-search to a perm-restricted file read it via
`t_open(T_OPATH)` + `SYS_READ` (the live instance: the 0400
`/system.key`). Fix: the `CWALKONLY` Spoor flag, set at the TWO O_PATH
handle-creation sites and rejected by `sys_read`/`sys_write`/
`sys_readdir`; `spoor_clone` CLEARS it (the impl-test catch SA-1 — the
create-from-O_PATH-base child must do its own I/O; a copied flag field
silently propagates). Strengthens I-22 by closing the read-bypass of
A-2d enforcement; fstat/lseek/wstat stay allowed (Linux O_PATH parity).
[[adt-81-r1]] (Fable, 0/0/1/1): F1 — the handler `len==0` fast-path
bypassed the inner gate ("a gate in an inner helper does not cover the
dispatch entry's own short-circuit"); F2 — the Loom content opcodes
gained the check as defense-in-depth. All enforcement sites live
syscall/spoor/loom-side (those surfaces' sweeps own the deep detail);
the resolver-facing contract is the [[sub-kernel-stalk]] O_PATH caveat.
NOTE: task-number collision — the July vivarium "#81" (dot-out-of-file)
is a DIFFERENT item, tracked at [[seam-posix-pathname-form-gates]].
