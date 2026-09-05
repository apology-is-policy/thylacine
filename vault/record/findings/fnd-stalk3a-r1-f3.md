---
id: fnd-stalk3a-r1-f3
type: fnd
title: "A devsrv-root clone's aux is UNOWNED until devsrv.walk takes ownership — a bare clone-then-clunk phantom-unrefs"
round: adt-stalk3a-r1
severity: P3
status: documented
surface: [sub-kernel-devsrv]
threatens: []
created: 2026-07-31
---
## Prosecution

`spoor_clone` of a devsrv root shallow-copies `aux = reg` without taking
a registry ref. A caller that clones and then `spoor_clunk`s WITHOUT an
intervening `devsrv.walk` (which normalizes-then-refs) would make
`devsrv_close` drop a ref the clone never took — an underflow toward a
premature registry free.

## Disposition

Documented as the clone contract at `devsrv_attach_registry`'s
declaration: a devsrv-root clone MUST pass through `devsrv.walk` (every
kernel path does — stalk's `clone_walk_zero` and the main-loop walk) or
have its aux detached before clunk. No bare-clone caller exists; the
contract is the guard against one being written.
