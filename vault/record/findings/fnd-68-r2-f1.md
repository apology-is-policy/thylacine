---
id: fnd-68-r2-f1
type: fnd
title: "The exits() close site is ALSO reachable with the death machinery armed"
round: adt-68-r2
severity: P1
status: fixed
surface: [sub-kernel-death]
threatens: []
fixed-by: chg-2026-07-14-68-last-thread-out-close
regression: "deleting the notes.c gate fails the R2-F1 test case (non-vacuous, verified)"
created: 2026-08-01
---
## Prosecution

Round 1 placed the `exit_close_active` flag around `thread_exit_self`'s close
only, on the stated premise that "`exits()`'s site is safe because
`group_exit_msg` is unset there". That premise is false in two ways.

First, the LS-5 interrupt default-terminate path calls `exits()` with the
terminate LATCH deliberately still queued — the note is left in the queue on
purpose — so `thread_die_pending()` reads true at that site through the
latch leg rather than the flag leg.

Second, a racing cross-Proc kill can set `group_exit_msg` MID-close, so even
a site that starts with the flag clear does not stay that way.

Either path re-opens the full [[fnd-68-r1-f1]] failure at the `exits()` site.

## Disposition

FIXED by HOISTING the flag's set/clear INSIDE `proc_close_handles_at_exit`,
so both call sites are covered by construction rather than by a per-site
argument. The closer is always `current_thread()`, so the read needs no
synchronization.

Two consecutive rounds, two confident premises about when the death
machinery is armed, both falsified. The general lesson the sweep records:
on this surface, "the flag is not set here" is a claim about EVERY caller
and every concurrent actor, not about the local code path.
