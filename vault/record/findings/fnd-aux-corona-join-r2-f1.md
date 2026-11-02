---
id: fnd-aux-corona-join-r2-f1
type: fnd
round: adt-aux-corona-join-r2
severity: P3
status: fixed
title: "The F1 test stranded its fixture on an early assert, so one red cascaded"
surface: [sub-kernel-pts]
threatens: []
fixed-by: chg-2026-10-29-aux-corona-join
regression: "One cleanup path; SJ3a and SJ3b each one FAIL line. An early assert through that path has NOT run: owed"
created: 2026-11-02
---
## Prosecution

Eleven asserts returned before the release block, stranding a live binding, 33 hooks, a watch, a pts and four Procs; the stranded slot then refuses every later empty-pool ID fixture with an unrelated message. Fixed in 009d20767 with the sibling boundary test's single cleanup. The SJ3 runs fail at the test's last asserts, so they do not execute the new path on an early exit; a planted early failure is owed under the next lease.
