---
id: fnd-note-wait-r2-f2
type: fnd
round: adt-note-wait-r2
severity: P2
status: fixed
title: "An interrupted readiness-file open reports POLLNVAL on a live socket"
surface: [sub-kernel-vivarium]
threatens: []
fixed-by: chg-2026-11-04-poll-mask
regression: "viv-pheno-probe L371; sabotage G22"
created: 2026-11-04
---
## Prosecution

A socket's first poll opens its readiness file, one RPC to netd. A caught note ends that open's wait, and `viv_poll_translated` read any failure but a shortage as a broken fd, so a guest got `POLLNVAL` on a connected socket, or `EBADF` from `pselect6`, with no `EINTR` and no handler. The shape is older, and vote 28 makes it the idiom: a held note pending at the first poll of every new socket.

## Disposition

`-T_E_INTR` fails the whole call, as a shortage does, and reaches `notes_wait_mask_end`, so the handler runs under the call's mask.
