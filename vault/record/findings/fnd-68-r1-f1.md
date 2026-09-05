---
id: fnd-68-r1-f1
type: fnd
title: "A clean exit_group(0) makes the closer read as dying, silently dropping its staged writes"
round: adt-68-r1
severity: P1
status: fixed
surface: [sub-kernel-death]
threatens: []
fixed-by: chg-2026-07-14-68-last-thread-out-close
regression: "the go6.exp E2E; the failure is silent by construction, so no unit assert reaches it"
created: 2026-08-01
---
## Prosecution

`proc_group_terminate` publishes `group_exit_msg` on EVERY `SYS_EXIT_GROUP`
— including a perfectly clean `exit_group(0)`. `thread_die_pending()` reads
that flag. So the LAST thread out, running the orderly final handle close in
the new window, reads as DYING to every sleep-capable close hook it invokes.

Each hook then short-circuits: `sleep`/`tsleep` return `SLEEP_INTR`, and the
9P client's `client_self_dying` refuses the send outright. The dev9p
write-behind close-flush therefore DROPS its staged bytes, and the
close-time Tclunk is never sent.

The consequence is silent data loss: bytes a program successfully `write()`
returned from are discarded because the program then exited. Accepted writes
must survive exit — that is the page-cache contract, not an optimization.

## Disposition

FIXED: a per-Thread `bool exit_close_active`, checked FIRST in
`thread_die_pending`, suppressing both death legs for the one bounded close
pass.

The author's original framing — "dropping a killed Proc's staged writes is
correct kill semantics" — was a category error the prosecutor named
precisely: `group_exit_msg` set is not the same as killed. That conflation
is worth carrying forward; it produced BOTH of this chunk's P1s.
