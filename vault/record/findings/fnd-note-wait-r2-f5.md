---
id: fnd-note-wait-r2-f5
type: fnd
round: adt-note-wait-r2
severity: P3
status: fixed
title: "The kernel's ready-first test runs no poll pass"
surface: [sub-kernel-notes]
threatens: []
fixed-by: chg-2026-11-04-poll-mask
regression: "none needed: pouch-hello-signals ppoll-ready, viv-pheno-probe L360 run the order"
created: 2026-11-04
---
## Prosecution

`notewait.poll_ready_keeps_the_note` hands the answer in, so it exercises the restore and not poll.c's order, and its comment claimed the order.

## Disposition

The comment says what the test does and names the legs that run the order.
