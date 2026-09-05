---
id: fnd-294-self-1
type: fnd
title: "Poll-state refs born zero -- the first op teardown frees it under p->poll"
round: adt-294-self
severity: P1
status: fixed
surface: [sub-kernel-ninep-dev9p-poll]
threatens: []
fixed-by: chg-2026-06-21-294-cancel-at-close
regression: dev9p.poll_cancel_at_close
created: 2026-07-31
---
## Prosecution

The lazily-allocated poll-state came from kmalloc(KP_ZERO) with refs = 0;
the priv's ref was never taken. The first op's teardown takes refs 1 -> 0
and FREES the state while p->poll still points at it -- every subsequent
poll UAFs the freed poll_list. The default build's test passed; UBSan
would have caught it.

## Disposition

Fixed (self-found, pre-formal): `cand->refs = 1` BEFORE the RELEASE
publish, with the in-code comment carrying the MUST.
