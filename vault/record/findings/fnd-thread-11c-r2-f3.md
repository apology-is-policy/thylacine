---
id: fnd-thread-11c-r2-f3
type: fnd
round: adt-thread-11c-r2
severity: P3
status: fixed
title: "The sigtab clone copied rows with plain assignment against a peer's set"
surface: [sub-kernel-vivarium, sub-kernel-proc]
threatens: []
fixed-by: chg-2026-11-03-item-11c
regression: "none deterministic: a race on the copy, closed by reading"
created: 2026-11-03
---
## Prosecution

`viv_sigtab_clone_into` read each row with a struct assignment while a peer's `viv_sigtab_set` wrote it. It now copies the handler first with `sigact_ld_handler`, then the three fields atomically, and `notes_row_has_taker` answers for whatever a torn row still says.
